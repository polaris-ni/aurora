#include "aurora/app/document_output.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/platform.h"
#include "aurora/render/detail/pdf_writer.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include "aurora/app/detail/win32_shell_integration.h"
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
#include "aurora/app/detail/macos_shell_integration.h"
#endif

namespace aurora {

namespace {

/// @brief 每毫米对应的 PostScript 点（1 pt = 1/72 英寸）。
constexpr double AURORA_POINTS_PER_MM = 72.0 / 25.4;

/// @brief 每毫米对应的逻辑 dp（96 dp = 1 英寸）。
constexpr double AURORA_DP_PER_MM = 96.0 / 25.4;

/// @brief 取当前可用的打印平台桥（判据同 URL 方案：通路登记即运行期事实）。
[[nodiscard]] auto active_print_backend() -> const desktop_shell::PrintBackend * {
    const auto *backend = desktop_shell::print_backend();
    if (backend != nullptr) {
        return backend;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    (void)detail::win32_shell_integration_ensure();
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    (void)detail::macos_shell_integration_ensure();
#endif
    return desktop_shell::print_backend();
}

/// @brief 纸张描述校验：尺寸 / 边距 / DPI 合法，且内容区为正。
[[nodiscard]] auto validate_spec(const PageSpec &spec) -> Result<void> {
    if (!(spec.width_mm > 0.0) || !(spec.height_mm > 0.0)) {
        return make_error(ErrorCode::GeneralInvalidArgument, "document-output: page size must be positive");
    }
    if (spec.dpi <= 0) {
        return make_error(ErrorCode::GeneralInvalidArgument, "document-output: dpi must be positive");
    }
    const auto &m = spec.margins;
    if (m.top_mm < 0.0 || m.right_mm < 0.0 || m.bottom_mm < 0.0 || m.left_mm < 0.0) {
        return make_error(ErrorCode::GeneralInvalidArgument, "document-output: page margins must not be negative");
    }
    if (!(spec.width_mm - m.left_mm - m.right_mm > 0.0) || !(spec.height_mm - m.top_mm - m.bottom_mm > 0.0)) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "document-output: page margins leave no printable content area");
    }
    return Result<void>{};
}

/// @brief RGBA 帧缓冲合成到不透明底色 → RGB8（PDF `DeviceRGB` 不带 alpha）。
///
/// 直接丢 alpha 会把未覆盖区域印成黑块；故逐像素按 `src*a + background*(1-a)` 合成，
/// 输出恒不透明（与验收判据「每页必须不透明」同源）。
[[nodiscard]] auto composite_to_rgb(std::span<const std::uint8_t> rgba, int width_px, int height_px,
                                    const Color &background) -> std::vector<std::uint8_t> {
    const auto pixel_count = static_cast<std::size_t>(width_px) * static_cast<std::size_t>(height_px);
    std::vector<std::uint8_t> rgb(pixel_count * 3U, 0);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const std::size_t src = index * 4U;
        const std::size_t dst = index * 3U;
        const unsigned alpha = rgba[src + 3U];
        const unsigned inverse = 255U - alpha;
        // 四舍五入的整数混合：+127 后整除 255。
        const unsigned mixed_r =
            (static_cast<unsigned>(rgba[src]) * alpha) + (static_cast<unsigned>(background.r) * inverse) + 127U;
        const unsigned mixed_g =
            (static_cast<unsigned>(rgba[src + 1U]) * alpha) + (static_cast<unsigned>(background.g) * inverse) + 127U;
        const unsigned mixed_b =
            (static_cast<unsigned>(rgba[src + 2U]) * alpha) + (static_cast<unsigned>(background.b) * inverse) + 127U;
        rgb[dst] = static_cast<std::uint8_t>(mixed_r / 255U);
        rgb[dst + 1U] = static_cast<std::uint8_t>(mixed_g / 255U);
        rgb[dst + 2U] = static_cast<std::uint8_t>(mixed_b / 255U);
    }
    return rgb;
}

/// @brief RGBA 位图合成到不透明底色 → 不透明 RGBA 位图（送打印；alpha 恒 255）。
[[nodiscard]] auto composite_to_opaque(const Image &page, const Color &background) -> Image {
    Image out;
    out.width = page.width;
    out.height = page.height;
    const auto pixel_count = static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height);
    out.pixels.resize(pixel_count * 4U);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const std::size_t offset = index * 4U;
        const unsigned alpha = page.pixels[offset + 3U];
        const unsigned inverse = 255U - alpha;
        out.pixels[offset] = static_cast<std::uint8_t>(((static_cast<unsigned>(page.pixels[offset]) * alpha) +
                                                        (static_cast<unsigned>(background.r) * inverse) + 127U) /
                                                       255U);
        out.pixels[offset + 1U] = static_cast<std::uint8_t>(((static_cast<unsigned>(page.pixels[offset + 1U]) * alpha) +
                                                             (static_cast<unsigned>(background.g) * inverse) + 127U) /
                                                            255U);
        out.pixels[offset + 2U] = static_cast<std::uint8_t>(((static_cast<unsigned>(page.pixels[offset + 2U]) * alpha) +
                                                             (static_cast<unsigned>(background.b) * inverse) + 127U) /
                                                            255U);
        out.pixels[offset + 3U] = 255U;
    }
    return out;
}

}  // namespace

auto page_content_raster(const PageSpec &spec) -> Result<PageRaster> {
    auto valid = validate_spec(spec);
    if (!valid) {
        return valid.error();
    }
    const auto content_width_mm = spec.width_mm - spec.margins.left_mm - spec.margins.right_mm;
    const auto content_height_mm = spec.height_mm - spec.margins.top_mm - spec.margins.bottom_mm;
    // 逻辑尺寸取整到 dp（Painter::begin 只收 int）；页几何本身仍以毫米为准，物理像素数
    // 一律从 Painter 读回，不在这里另算一套取整。
    const auto width_dp = static_cast<int>(std::lround(content_width_mm * AURORA_DP_PER_MM));
    const auto height_dp = static_cast<int>(std::lround(content_height_mm * AURORA_DP_PER_MM));
    if (width_dp <= 0 || height_dp <= 0) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "document-output: content area is smaller than one logical pixel");
    }
    Painter painter;
    const auto scale = static_cast<float>(spec.dpi) / 96.0F;
    painter.set_scale(scale);
    painter.begin(width_dp, height_dp);
    PageRaster raster;
    raster.width_px = painter.width();
    raster.height_px = painter.height();
    raster.width_dp = width_dp;
    raster.height_dp = height_dp;
    raster.scale = painter.scale();
    return raster;
}

auto export_pdf(const std::string &path, const PageSpec &spec, std::size_t page_count, const PageDrawer &draw)
    -> Result<void> {
    if (page_count == 0) {
        return make_error(ErrorCode::GeneralInvalidArgument, "document-output: page_count must be at least 1");
    }
    if (!draw) {
        return make_error(ErrorCode::GeneralInvalidArgument, "document-output: page drawer is empty");
    }
    auto raster = page_content_raster(spec);
    if (!raster) {
        return raster.error();
    }
    const auto &page = raster.value();

    std::vector<render::detail::PdfImagePage> pages;
    pages.reserve(page_count);
    for (std::size_t index = 0; index < page_count; ++index) {
        Painter painter;
        painter.set_scale(page.scale);
        painter.begin(page.width_dp, page.height_dp);
        // 先铺底色再回调：未绘制区域也落在纸白（或调用方底色）上。
        painter.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                               .size = Size{.width = static_cast<float>(page.width_dp),
                                            .height = static_cast<float>(page.height_dp)}},
                          spec.background);
        draw(index, painter);
        const auto frame = painter.to_image();
        if (frame.width != page.width_px || frame.height != page.height_px) {
            return make_error(ErrorCode::GeneralInvalidArgument,
                              "document-output: painter frame size drifted from the page raster spec");
        }
        render::detail::PdfImagePage pdf_page;
        pdf_page.pixel_width = frame.width;
        pdf_page.pixel_height = frame.height;
        pdf_page.rgb =
            composite_to_rgb(std::span<const std::uint8_t>(frame.pixels), frame.width, frame.height, spec.background);
        pdf_page.place_x_pt = spec.margins.left_mm * AURORA_POINTS_PER_MM;
        pdf_page.place_y_pt = spec.margins.bottom_mm * AURORA_POINTS_PER_MM;
        pdf_page.place_width_pt = (spec.width_mm - spec.margins.left_mm - spec.margins.right_mm) * AURORA_POINTS_PER_MM;
        pdf_page.place_height_pt =
            (spec.height_mm - spec.margins.top_mm - spec.margins.bottom_mm) * AURORA_POINTS_PER_MM;
        pages.push_back(std::move(pdf_page));
    }
    return render::detail::write_pdf(path, spec.width_mm * AURORA_POINTS_PER_MM, spec.height_mm * AURORA_POINTS_PER_MM,
                                     pages);
}

auto print_targets() -> Result<std::vector<PrintTarget>> {
    const auto *backend = active_print_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::PrintSessionUnsupported);
    }
    return backend->targets();
}

// ---------- PrintSession ----------

struct PrintSession::Impl {
    std::unique_ptr<desktop_shell::PrintSessionBackend> backend;  ///< 平台会话
    bool open = false;  ///< 是否仍可提交页
    PageRaster raster;  ///< 打开时的内容区栅格尺寸
};

PrintSession::PrintSession() noexcept = default;

PrintSession::PrintSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PrintSession::PrintSession(PrintSession &&other) noexcept : impl_(std::move(other.impl_)), spec_(other.spec_) {
    other.impl_.reset();
}

auto PrintSession::operator=(PrintSession &&other) noexcept -> PrintSession & {
    if (this != &other) {
        if (impl_ != nullptr && impl_->open) {
            impl_->backend->abort();
        }
        impl_ = std::move(other.impl_);
        spec_ = other.spec_;
        other.impl_.reset();
    }
    return *this;
}

PrintSession::~PrintSession() { abort(); }

auto PrintSession::open(std::string_view target_id, const PageSpec &spec) -> Result<PrintSession> {
    if (target_id.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, "document-output: print target id is empty");
    }
    auto raster = page_content_raster(spec);
    if (!raster) {
        return raster.error();
    }
    const auto *backend = active_print_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::PrintSessionUnsupported);
    }
    auto session = backend->open(target_id, spec);
    if (!session) {
        return session.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->backend = std::move(session.value());
    impl->open = true;
    impl->raster = raster.value();
    PrintSession out(std::move(impl));
    out.spec_ = spec;
    return out;
}

auto PrintSession::submit_page(const Image &page) -> Result<void> {
    if (impl_ == nullptr || !impl_->open) {
        return make_error(ErrorCode::PrintSessionFailed, "document-output: print session is not open");
    }
    if (page.width != impl_->raster.width_px || page.height != impl_->raster.height_px) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "document-output: page image does not match the page raster size");
    }
    if (page.pixels.size() != static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height) * 4U) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "document-output: page image is not RGBA8 (pixels size mismatch)");
    }
    return impl_->backend->submit_page(composite_to_opaque(page, spec_.background));
}

auto PrintSession::finish() -> Result<void> {
    if (impl_ == nullptr || !impl_->open) {
        return Result<void>{};  // 幂等：已结束 / 空会话为空操作
    }
    auto result = impl_->backend->finish();
    if (result) {
        impl_->open = false;
    }
    return result;
}

auto PrintSession::abort() -> void {
    if (impl_ != nullptr && impl_->open) {
        impl_->backend->abort();
        impl_->open = false;
    }
}

auto PrintSession::is_open() const -> bool { return impl_ != nullptr && impl_->open; }

}  // namespace aurora
