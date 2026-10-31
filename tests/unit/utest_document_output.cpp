/// 测试类型: unit
/// 目标单元: include/aurora/app/document_output.h
/// 测试说明: 文档输出与打印会话的契约回归——页几何（毫米真源 → `Painter` 读回物理像素）、非法纸张描述、
/// PDF 1.4 结构（Catalog/Pages/Page/图像 XObject/FlateDecode/xref/trailer）、栅格入 PDF 的**无文本
/// 操作符**判据、zlib stored 流逐字节回读（与帧缓冲逐像素相等）、RGBA → 页面底色合成（透明不得印成
/// 黑块、半透明混合取整），以及打印接缝的三条口径：空目标列表配成功、未接线报 `print-session-unsupported`、
/// 会话提交前先强制不透明且 finish / abort 幂等。
///
/// 为何单测能跑产品代码路径：打印与 PDF 写出都不是平台专属——PDF 写出器与合成逻辑在库内，打印会话
/// 经 `desktop_shell::PrintBackend` 进程级单槽注入伪后端；真机上平台桥登记同名后端，两侧走同一条
/// `export_pdf` / `PrintSession` 代码，被测的不是它的复刻。
///
/// 变异自证（改坏实现应转红的六条）：
///  * 把 `AURORA_DP_PER_MM` 从 96/25.4 改成 72/25.4，或把缩放从 `dpi/96` 改成 `dpi/72`
///    → `content_raster_tracks_the_painter_readback` 转红（页几何先损失，再传到每页像素）；
///  * 把 zlib 包装改成裸 DEFLATE（去掉 0x78 0x01 头）→ `pdf_image_streams_round_trip_through_zlib`
///    转红（解压失败，或与帧缓冲不再逐字节相等）；
///  * 往内容流里塞 `BT` / `Tj` → `pdf_content_streams_carry_no_text_operators` 转红；
///  * 去掉 `composite_to_rgb`（直接丢 alpha）→ `pdf_pages_are_composited_onto_the_page_background`
///    转红（未绘制区从纸白变黑块）；
///  * 去掉 `composite_to_opaque` → `print_session_composites_pages_before_submission` 转红；
///  * 把空目标列表当成错误 → `print_targets_reports_an_empty_system_as_success` 转红。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/document_output.h"
#include "aurora/core/compression.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/image.h"
#include "aurora/render/painter.h"
#include "aurora/window/platform.h"
#include "framework/aurora_test.h"
#include "framework/isolation.h"

namespace aurora::test_cases::utest_document_output {

namespace {

constexpr double AURORA_TEST_MM_PER_INCH = 25.4;
constexpr double AURORA_TEST_DP_PER_INCH = 96.0;
constexpr double AURORA_TEST_PT_PER_INCH = 72.0;

/// @brief 25.4 mm 见方页面在 96 dpi 下的像素数（判据基准，独立于被测实现的常量）。
constexpr std::size_t AURORA_TEST_SQUARE_PIXELS = std::size_t{96U} * 96U;

/// @brief 内容区逻辑尺寸（dp）——独立重算，不引用被测实现的常量。
[[nodiscard]] auto expected_dp(double mm) -> int {
    return static_cast<int>(std::lround(mm * AURORA_TEST_DP_PER_INCH / AURORA_TEST_MM_PER_INCH));
}

/// @brief 框架隔离临时目录下的文件路径（每用例 TMP 被接管）。
[[nodiscard]] auto temp_path(std::string_view name) -> std::string {
    return (std::filesystem::path(aurora::testing::isolation::temp_dir()) / std::string(name)).string();
}

/// @brief 读整个文件为字节。
[[nodiscard]] auto read_bytes(const std::string &path) -> std::vector<std::uint8_t> {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// @brief 字节视图 → 字符串（测试内只用于结构判据）。
[[nodiscard]] auto as_text(const std::vector<std::uint8_t> &bytes) -> std::string {
    return {bytes.begin(), bytes.end()};
}

/// @brief 解析 zlib 包装的 stored DEFLATE 块（RFC 1950 + RFC 1951 BTYPE=00）。
///
/// 不依赖 `AURORA_ENABLE_COMPRESSION`：写出器只产出 stored 块，测试因此能自证字节级往返；
/// 能力开着的构建另在用例体内用 `aurora::decompress` 交叉复核。
/// @param stream zlib 流。
/// @return 解压后的原始字节；结构不符返回空。
[[nodiscard]] auto inflate_stored_zlib(std::span<const std::uint8_t> stream) -> std::vector<std::uint8_t> {
    if (stream.size() < 2U + 5U + 4U) {
        return {};
    }
    if (stream[0] != 0x78U || stream[1] != 0x01U) {
        return {};
    }
    std::vector<std::uint8_t> out;
    std::size_t offset = 2U;
    bool final_block = false;
    while (!final_block && offset + 5U <= stream.size()) {
        const auto header = stream[offset++];
        final_block = (header & 0x01U) != 0U;
        if (((header >> 1U) & 0x03U) != 0x00U) {
            return {};  // 只认 stored 块
        }
        const auto length =
            static_cast<std::size_t>(stream[offset]) | (static_cast<std::size_t>(stream[offset + 1U]) << 8U);
        const auto complement =
            static_cast<std::size_t>(stream[offset + 2U]) | (static_cast<std::size_t>(stream[offset + 3U]) << 8U);
        offset += 4U;
        if ((length ^ complement) != 0xFFFFU || offset + length > stream.size()) {
            return {};
        }
        out.insert(out.end(), stream.begin() + static_cast<std::ptrdiff_t>(offset),
                   stream.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    if (!final_block || offset + 4U > stream.size()) {
        return {};
    }
    return out;
}

/// @brief 从 PDF 文本里取所有图像流的解压结果（按出现序）。
[[nodiscard]] auto pdf_image_streams(const std::string &pdf) -> std::vector<std::vector<std::uint8_t>> {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 只读字节视图，不转移所有权
    const std::span<const std::uint8_t> pdf_bytes(reinterpret_cast<const std::uint8_t *>(pdf.data()), pdf.size());
    std::vector<std::vector<std::uint8_t>> out;
    std::size_t pos = 0;
    while ((pos = pdf.find("/Subtype /Image", pos)) != std::string::npos) {
        const auto length_pos = pdf.find("/Length ", pos);
        if (length_pos == std::string::npos) {
            break;
        }
        const auto length_end = pdf.find(' ', length_pos + 8U);
        if (length_end == std::string::npos) {
            break;
        }
        const auto length =
            static_cast<std::size_t>(std::stoull(pdf.substr(length_pos + 8U, length_end - length_pos - 8U)));
        const auto data_start = pdf.find("stream\n", length_end);
        if (data_start == std::string::npos) {
            break;
        }
        const auto begin = data_start + 7U;
        out.push_back(inflate_stored_zlib(pdf_bytes.subspan(begin, length)));
        pos = begin + length;
    }
    return out;
}

/// @brief 从 PDF 文本里取所有**内容流**正文（图像流跳过，避免把二进制位图当文本扫）。
[[nodiscard]] auto pdf_content_streams(const std::string &pdf) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t pos = 0;
    // 以 `\nstream\n` 定位：`endstream` 里也含 `stream`，只搜 `stream\n` 会把流尾误认成新流的开头。
    while ((pos = pdf.find("\nstream\n", pos)) != std::string::npos) {
        const auto data_start = pos + std::strlen("\nstream\n");
        const auto object_start = pdf.rfind(" 0 obj\n", pos);
        const bool image = object_start != std::string::npos && pdf.find("/Subtype /Image", object_start) < pos;
        const auto length_pos =
            object_start == std::string::npos ? std::string::npos : pdf.find("/Length ", object_start);
        if (length_pos == std::string::npos || length_pos > pos) {
            break;
        }
        const auto length_end = pdf.find(' ', length_pos + 8U);
        if (length_end == std::string::npos) {
            break;
        }
        const auto length =
            static_cast<std::size_t>(std::stoull(pdf.substr(length_pos + 8U, length_end - length_pos - 8U)));
        if (!image) {
            out.push_back(pdf.substr(data_start, length));
        }
        pos = data_start + length;
    }
    return out;
}

/// @brief 全不透明像素判定：RGB 三元组逐字节相等。
[[nodiscard]] auto all_pixels_are(const std::vector<std::uint8_t> &rgb, std::uint8_t r, std::uint8_t g, std::uint8_t b)
    -> bool {
    if (rgb.size() % 3U != 0U) {
        return false;
    }
    for (std::size_t index = 0; index < rgb.size(); index += 3U) {
        if (rgb[index] != r || rgb[index + 1U] != g || rgb[index + 2U] != b) {
            return false;
        }
    }
    return true;
}

/// @brief 页面描边几何（测试局部助手，避免每处重复聚合初始化）。
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 一枚 25.4 mm 见方、无边距的纸张（内容区恰为 96 dp / 96 px @96 dpi）。
[[nodiscard]] auto square_spec(int dpi = 96) -> PageSpec {
    PageSpec spec;
    spec.width_mm = 25.4;
    spec.height_mm = 25.4;
    spec.margins = PageMargins{.top_mm = 0.0, .right_mm = 0.0, .bottom_mm = 0.0, .left_mm = 0.0};
    spec.dpi = dpi;
    return spec;
}

/// @brief 伪造打印会话后端收到的页快照。
struct FakePrintedPage {
    int width = 0;  ///< 宽
    int height = 0;  ///< 高
    std::vector<std::uint8_t> rgba;  ///< RGBA8 像素
};

/// @brief 伪造打印后端的观测状态。
struct FakePrintState {
    std::vector<PrintTarget> targets{{{.id = "printer-a", .name = "Printer A", .is_default = true},
                                      {.id = "printer-b", .name = "Printer B", .is_default = false}}};
    bool targets_called = false;  ///< targets 是否被调
    bool open_called = false;  ///< open 是否被调
    bool finish_called = false;  ///< finish 是否被调
    int abort_calls = 0;  ///< abort 次数
    std::string opened_target;  ///< 打开的目标 id
    PageSpec opened_spec;  ///< 打开时收到的纸张描述
    std::vector<FakePrintedPage> pages;  ///< 已提交页
};

/// @brief 伪造一次打印会话。
class FakeSession final : public desktop_shell::PrintSessionBackend {
  public:
    explicit FakeSession(FakePrintState &state) : state_(state) {}

    [[nodiscard]] auto submit_page(const Image &page) -> Result<void> override {
        state_.pages.push_back(FakePrintedPage{.width = page.width, .height = page.height, .rgba = page.pixels});
        return Result<void>{};
    }
    [[nodiscard]] auto finish() -> Result<void> override {
        state_.finish_called = true;
        return Result<void>{};
    }
    auto abort() -> void override { ++state_.abort_calls; }

  private:
    FakePrintState &state_;  ///< 观测状态
};

/// @brief 每个用例进出清空打印通路的伪造后端（进程级单槽）。
class PrintBackendGuard {
  public:
    PrintBackendGuard() { desktop_shell::unregister_print_backend(); }
    ~PrintBackendGuard() { desktop_shell::unregister_print_backend(); }
    PrintBackendGuard(const PrintBackendGuard &) = delete;
    auto operator=(const PrintBackendGuard &) -> PrintBackendGuard & = delete;
    PrintBackendGuard(PrintBackendGuard &&) = delete;
    auto operator=(PrintBackendGuard &&) -> PrintBackendGuard & = delete;
};

/// @brief 安装伪造打印桥。
void install_fake_print_backend(FakePrintState &state) {
    desktop_shell::PrintBackend backend;
    backend.targets = [&state]() -> Result<std::vector<PrintTarget>> {
        state.targets_called = true;
        return state.targets;
    };
    backend.open = [&state](std::string_view target_id,
                            const PageSpec &spec) -> Result<std::unique_ptr<desktop_shell::PrintSessionBackend>> {
        state.open_called = true;
        state.opened_target = std::string(target_id);
        state.opened_spec = spec;
        std::unique_ptr<desktop_shell::PrintSessionBackend> session = std::make_unique<FakeSession>(state);
        return session;
    };
    desktop_shell::register_print_backend(std::move(backend));
}

}  // namespace

AURORA_TEST_CASE(content_raster_tracks_the_painter_readback) {
    PageSpec spec;
    spec.width_mm = 210.0;
    spec.height_mm = 297.0;
    spec.margins = PageMargins{.top_mm = 12.7, .right_mm = 12.7, .bottom_mm = 12.7, .left_mm = 12.7};
    spec.dpi = 96;

    auto raster = page_content_raster(spec);
    AURORA_TEST_REQUIRE(raster.ok());
    // 内容区 184.6 × 271.6 mm；A4 的 210 mm 在 96 dpi 下不是整数 dp，页几何仍以毫米为准。
    AURORA_TEST_CHECK_EQ(raster.value().width_dp, expected_dp(184.6));
    AURORA_TEST_CHECK_EQ(raster.value().height_dp, expected_dp(271.6));
    AURORA_TEST_CHECK_NEAR(raster.value().scale, 1.0, 1e-6);
    AURORA_TEST_CHECK_EQ(raster.value().width_px, expected_dp(184.6));
    AURORA_TEST_CHECK_EQ(raster.value().height_px, expected_dp(271.6));

    // 300 dpi：物理像素数 = dp × (dpi/96)，且以 Painter 读回为唯一真源。
    spec.dpi = 300;
    auto dense = page_content_raster(spec);
    AURORA_TEST_REQUIRE(dense.ok());
    AURORA_TEST_CHECK_NEAR(dense.value().scale, 300.0 / AURORA_TEST_DP_PER_INCH, 1e-6);
    AURORA_TEST_CHECK_EQ(dense.value().width_dp, expected_dp(184.6));
    AURORA_TEST_CHECK_EQ(dense.value().width_px,
                         static_cast<int>(std::lround(expected_dp(184.6) * (300.0 / AURORA_TEST_DP_PER_INCH))));
    AURORA_TEST_CHECK_EQ(dense.value().height_px,
                         static_cast<int>(std::lround(expected_dp(271.6) * (300.0 / AURORA_TEST_DP_PER_INCH))));
}

AURORA_TEST_CASE(content_raster_rejects_invalid_descriptions) {
    PageSpec zero_width;
    zero_width.width_mm = 0.0;
    auto bad = page_content_raster(zero_width);
    AURORA_TEST_REQUIRE(!bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::GeneralInvalidArgument);

    PageSpec zero_dpi;
    zero_dpi.dpi = 0;
    bad = page_content_raster(zero_dpi);
    AURORA_TEST_REQUIRE(!bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::GeneralInvalidArgument);

    PageSpec negative_margin;
    negative_margin.margins.left_mm = -1.0;
    bad = page_content_raster(negative_margin);
    AURORA_TEST_REQUIRE(!bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::GeneralInvalidArgument);

    PageSpec squeezed;
    squeezed.width_mm = 10.0;
    squeezed.margins.left_mm = 6.0;
    squeezed.margins.right_mm = 6.0;
    bad = page_content_raster(squeezed);
    AURORA_TEST_REQUIRE(!bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::GeneralInvalidArgument);

    PageSpec sub_pixel;
    sub_pixel.width_mm = 0.05;  // 小于一个逻辑 dp
    sub_pixel.margins = PageMargins{.top_mm = 0.0, .right_mm = 0.0, .bottom_mm = 0.0, .left_mm = 0.0};
    bad = page_content_raster(sub_pixel);
    AURORA_TEST_REQUIRE(!bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::GeneralInvalidArgument);
}

AURORA_TEST_CASE(export_pdf_writes_a_well_formed_document) {
    const auto path = temp_path("aurora_doc_structure.pdf");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    const auto spec = square_spec();
    auto result = export_pdf(path, spec, 2U, [](std::size_t page_index, Painter &painter) {
        painter.fill_rect(rect_at(0.0F, 0.0F, 96.0F, 96.0F), page_index == 0U ? Color::red() : Color::blue());
    });
    AURORA_TEST_REQUIRE(result.ok());

    const auto bytes = read_bytes(path);
    AURORA_TEST_REQUIRE(!bytes.empty());
    const auto text = as_text(bytes);
    AURORA_TEST_CHECK(text.rfind("%PDF-1.4\n", 0) == 0U);
    AURORA_TEST_CHECK(text.find("/Type /Catalog") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/Type /Pages") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/Count 2") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/Type /Page /Parent") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/Subtype /Image") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/ColorSpace /DeviceRGB") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/BitsPerComponent 8") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/Filter /FlateDecode") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/MediaBox [0 0 72.0 72.0]") != std::string::npos);
    AURORA_TEST_CHECK(text.find("/Im0 Do") != std::string::npos);
    AURORA_TEST_CHECK(text.find("xref") != std::string::npos);
    AURORA_TEST_CHECK(text.find("trailer") != std::string::npos);
    AURORA_TEST_CHECK(text.find("startxref") != std::string::npos);
    AURORA_TEST_CHECK(text.find("%%EOF") != std::string::npos);
}

AURORA_TEST_CASE(pdf_image_streams_round_trip_through_zlib) {
    const auto path = temp_path("aurora_doc_streams.pdf");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    const auto spec = square_spec();
    auto result = export_pdf(path, spec, 2U, [](std::size_t page_index, Painter &painter) {
        painter.fill_rect(rect_at(0.0F, 0.0F, 96.0F, 96.0F), page_index == 0U ? Color::red() : Color::blue());
    });
    AURORA_TEST_REQUIRE(result.ok());

    const auto pdf = as_text(read_bytes(path));
    const auto streams = pdf_image_streams(pdf);
    AURORA_TEST_REQUIRE_EQ(streams.size(), std::size_t{2});
    for (const auto &stream : streams) {
        AURORA_TEST_REQUIRE_EQ(stream.size(), AURORA_TEST_SQUARE_PIXELS * 3U);
    }
    AURORA_TEST_CHECK(all_pixels_are(streams[0], 255U, 0U, 0U));
    AURORA_TEST_CHECK(all_pixels_are(streams[1], 0U, 0U, 255U));

    // 能力开着的构建再用框架解压器复核同一份字节（stored 块亦须是合法 zlib 流）。
#ifdef AURORA_ENABLE_COMPRESSION
    const auto data_start = pdf.find("/Filter /FlateDecode /Length ");
    AURORA_TEST_REQUIRE(data_start != std::string::npos);
    const auto length_end = pdf.find(' ', data_start + std::strlen("/Filter /FlateDecode /Length "));
    AURORA_TEST_REQUIRE(length_end != std::string::npos);
    const auto length = static_cast<std::size_t>(
        std::stoull(pdf.substr(data_start + std::strlen("/Filter /FlateDecode /Length "),
                               length_end - data_start - std::strlen("/Filter /FlateDecode /Length "))));
    const auto marker = pdf.find("stream\n", length_end);
    AURORA_TEST_REQUIRE(marker != std::string::npos);
    const auto stream_start = marker + std::strlen("stream\n");
    const std::span<const char> compressed = std::span<const char>(pdf).subspan(stream_start, length);
    auto decompressed = decompress(AURORA_COMPRESS_FORMAT_ZLIB, compressed);
    AURORA_TEST_REQUIRE(decompressed.ok());
    AURORA_TEST_REQUIRE_EQ(decompressed.value().size(), AURORA_TEST_SQUARE_PIXELS * 3U);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): `char`/`uint8_t` 字节视图互换属预期类型双关
    const std::span<const std::uint8_t> raw(reinterpret_cast<const std::uint8_t *>(decompressed.value().data()),
                                            decompressed.value().size());
    AURORA_TEST_CHECK(std::vector<std::uint8_t>(raw.begin(), raw.end()) == streams[0]);
#endif
}

AURORA_TEST_CASE(pdf_content_streams_carry_no_text_operators) {
    const auto path = temp_path("aurora_doc_no_text.pdf");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    const auto spec = square_spec();
    auto result = export_pdf(path, spec, 1U, [](std::size_t, Painter &painter) {
        painter.fill_rect(rect_at(0.0F, 0.0F, 96.0F, 96.0F), Color::red());
    });
    AURORA_TEST_REQUIRE(result.ok());

    const auto content_streams = pdf_content_streams(as_text(read_bytes(path)));
    AURORA_TEST_REQUIRE_EQ(content_streams.size(), std::size_t{1});
    for (const auto &content : content_streams) {
        for (const char *operator_name : {"BT", "ET", "Tj", "TJ", "Tf"}) {
            std::size_t pos = content.find(operator_name);
            while (pos != std::string::npos) {
                // 只认「独立操作符」：前导须是空白或行首，尾随须是空白或行尾。
                const bool left_ok = pos == 0U || content[pos - 1U] == ' ' || content[pos - 1U] == '\n';
                const auto after = pos + std::strlen(operator_name);
                const bool right_ok = after >= content.size() || content[after] == ' ' || content[after] == '\n';
                AURORA_TEST_CHECK_FALSE(left_ok && right_ok);
                pos = content.find(operator_name, pos + 1U);
            }
        }
    }
}

AURORA_TEST_CASE(pdf_pages_are_composited_onto_the_page_background) {
    const auto path = temp_path("aurora_doc_composite.pdf");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    const auto spec = square_spec();
    // 第 0 页什么都不画（帧缓冲透明黑 → 必须落成纸白，不是黑块）；
    // 第 1 页整页半透明黑（与纸白合成得中灰）；第 2 页左上角 10×10 红块，其余仍是纸白。
    auto result = export_pdf(path, spec, 3U, [](std::size_t page_index, Painter &painter) {
        if (page_index == 1U) {
            painter.fill_rect(rect_at(0.0F, 0.0F, 96.0F, 96.0F), Color{0U, 0U, 0U, 128U});
        } else if (page_index == 2U) {
            painter.fill_rect(rect_at(0.0F, 0.0F, 10.0F, 10.0F), Color::red());
        }
    });
    AURORA_TEST_REQUIRE(result.ok());

    const auto streams = pdf_image_streams(as_text(read_bytes(path)));
    AURORA_TEST_REQUIRE_EQ(streams.size(), std::size_t{3});

    AURORA_TEST_CHECK(all_pixels_are(streams[0], 255U, 255U, 255U));

    // 半透明黑 (0,0,0,128) 与纸白合成：四舍五入的整数混合给出 127 或 128。
    for (std::size_t index = 0; index < streams[1].size(); index += 3U) {
        AURORA_TEST_REQUIRE(streams[1][index] >= 126U && streams[1][index] <= 128U);
        AURORA_TEST_CHECK_EQ(streams[1][index], streams[1][index + 1U]);
        AURORA_TEST_CHECK_EQ(streams[1][index], streams[1][index + 2U]);
    }

    const auto pixel_at = [](const std::vector<std::uint8_t> &rgb, int x, int y) -> std::vector<std::uint8_t> {
        const auto offset = (static_cast<std::size_t>(y) * 96U) + static_cast<std::size_t>(x);
        return {rgb[offset * 3U], rgb[(offset * 3U) + 1U], rgb[(offset * 3U) + 2U]};
    };
    AURORA_TEST_CHECK(pixel_at(streams[2], 2, 2) == std::vector<std::uint8_t>({255U, 0U, 0U}));
    AURORA_TEST_CHECK(pixel_at(streams[2], 50, 50) == std::vector<std::uint8_t>({255U, 255U, 255U}));
}

AURORA_TEST_CASE(export_pdf_rejects_invalid_requests) {
    const auto path = temp_path("aurora_doc_invalid.pdf");
    auto result = export_pdf(path, square_spec(), 0U, [](std::size_t, Painter &) {});
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);

    result = export_pdf(path, square_spec(), 1U, {});
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);

    PageSpec bad_spec = square_spec();
    bad_spec.dpi = -1;
    result = export_pdf(path, bad_spec, 1U, [](std::size_t, Painter &) {});
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
}

AURORA_TEST_CASE(export_pdf_draws_each_page_in_ascending_order) {
    const auto path = temp_path("aurora_doc_order.pdf");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    std::vector<std::size_t> indices;
    std::vector<int> widths;
    auto result = export_pdf(path, square_spec(), 3U, [&indices, &widths](std::size_t page_index, Painter &painter) {
        indices.push_back(page_index);
        widths.push_back(painter.width());
    });
    AURORA_TEST_REQUIRE(result.ok());
    AURORA_TEST_CHECK(indices == std::vector<std::size_t>({0U, 1U, 2U}));
    AURORA_TEST_CHECK(widths == std::vector<int>({96, 96, 96}));
}

AURORA_TEST_CASE(print_targets_reports_an_empty_system_as_success) {
    PrintBackendGuard guard;
    FakePrintState state;
    state.targets.clear();
    install_fake_print_backend(state);

    auto targets = print_targets();
    AURORA_TEST_REQUIRE(targets.ok());
    AURORA_TEST_CHECK(targets.value().empty());
    AURORA_TEST_CHECK(state.targets_called);
}

AURORA_TEST_CASE(print_targets_lists_every_target_from_the_backend) {
    PrintBackendGuard guard;
    FakePrintState state;
    install_fake_print_backend(state);

    auto targets = print_targets();
    AURORA_TEST_REQUIRE(targets.ok());
    AURORA_TEST_REQUIRE_EQ(targets.value().size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(targets.value()[0].id, std::string{"printer-a"});
    AURORA_TEST_CHECK_EQ(targets.value()[0].name, std::string{"Printer A"});
    AURORA_TEST_CHECK(targets.value()[0].is_default);
    AURORA_TEST_CHECK_EQ(targets.value()[1].id, std::string{"printer-b"});
    AURORA_TEST_CHECK_FALSE(targets.value()[1].is_default);
}

AURORA_TEST_CASE(print_session_composites_pages_before_submission) {
    PrintBackendGuard guard;
    FakePrintState state;
    install_fake_print_backend(state);

    auto session = PrintSession::open("printer-a", square_spec());
    AURORA_TEST_REQUIRE(session.ok());
    AURORA_TEST_CHECK(session.value().is_open());
    AURORA_TEST_CHECK(state.open_called);
    AURORA_TEST_CHECK_EQ(state.opened_target, std::string{"printer-a"});
    AURORA_TEST_CHECK_NEAR(state.opened_spec.width_mm, 25.4, 1e-9);

    // 全透明页：不得把 alpha=0 的黑色直接送给后端。
    Image transparent;
    transparent.width = 96;
    transparent.height = 96;
    transparent.pixels.assign(AURORA_TEST_SQUARE_PIXELS * 4U, 0U);
    AURORA_TEST_REQUIRE(session.value().submit_page(transparent).ok());
    AURORA_TEST_REQUIRE_EQ(state.pages.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(state.pages[0].width, 96);
    AURORA_TEST_CHECK_EQ(state.pages[0].height, 96);
    AURORA_TEST_CHECK_EQ(state.pages[0].rgba.size(), AURORA_TEST_SQUARE_PIXELS * 4U);
    AURORA_TEST_CHECK_EQ(state.pages[0].rgba[0], 255U);
    AURORA_TEST_CHECK_EQ(state.pages[0].rgba[1], 255U);
    AURORA_TEST_CHECK_EQ(state.pages[0].rgba[2], 255U);
    AURORA_TEST_CHECK_EQ(state.pages[0].rgba[3], 255U);

    // 半透明黑：合成到纸白后 alpha 恒 255，RGB 取中灰。
    Image translucent;
    translucent.width = 96;
    translucent.height = 96;
    translucent.pixels.assign(AURORA_TEST_SQUARE_PIXELS * 4U, 0U);
    for (std::size_t index = 0; index < translucent.pixels.size(); index += 4U) {
        translucent.pixels[index + 3U] = 128U;
    }
    AURORA_TEST_REQUIRE(session.value().submit_page(translucent).ok());
    AURORA_TEST_REQUIRE_EQ(state.pages.size(), std::size_t{2});
    const auto &page = state.pages[1].rgba;
    for (std::size_t index = 0; index < page.size(); index += 4U) {
        AURORA_TEST_REQUIRE(page[index] >= 126U && page[index] <= 128U);
        AURORA_TEST_CHECK_EQ(page[index], page[index + 1U]);
        AURORA_TEST_CHECK_EQ(page[index], page[index + 2U]);
        AURORA_TEST_CHECK_EQ(page[index + 3U], 255U);
    }

    AURORA_TEST_REQUIRE(session.value().finish().ok());
    AURORA_TEST_CHECK(state.finish_called);
    AURORA_TEST_CHECK_FALSE(session.value().is_open());
    // 结束后的提交被拒，而不是悄悄丢弃。
    const auto rejected = session.value().submit_page(transparent);
    AURORA_TEST_REQUIRE(!rejected.ok());
    AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::PrintSessionFailed);
    // finish 幂等：再次调用不重复触达后端。
    AURORA_TEST_REQUIRE(session.value().finish().ok());
}

AURORA_TEST_CASE(print_session_rejects_mismatched_pages_and_bad_arguments) {
    PrintBackendGuard guard;
    FakePrintState state;
    install_fake_print_backend(state);

    auto empty_target = PrintSession::open("", square_spec());
    AURORA_TEST_REQUIRE(!empty_target.ok());
    AURORA_TEST_CHECK_EQ(empty_target.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_FALSE(state.open_called);

    auto session = PrintSession::open("printer-a", square_spec());
    AURORA_TEST_REQUIRE(session.ok());

    Image wrong_size;
    wrong_size.width = 32;
    wrong_size.height = 32;
    wrong_size.pixels.assign(std::size_t{32U} * 32U * 4U, 255U);
    auto result = session.value().submit_page(wrong_size);
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);

    Image not_rgba;
    not_rgba.width = 96;
    not_rgba.height = 96;
    not_rgba.pixels.assign(AURORA_TEST_SQUARE_PIXELS * 3U, 255U);
    result = session.value().submit_page(not_rgba);
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK(state.pages.empty());

    // 析构未 finish 的会话自动 abort（丢弃已提交页），不静默留下后台任务。
    session.value().abort();
    AURORA_TEST_CHECK_EQ(state.abort_calls, 1);
    AURORA_TEST_CHECK_FALSE(session.value().is_open());
}

AURORA_TEST_CASE(desktop_shell_capability_bits_follow_the_wired_bridges) {
    // 四项入口是进程级自由函数（不挂在 Surface 上），故能力位按**平台**申报、不随 SurfaceKind 变化：
    // 同一构建下逐 SurfaceKind 取值必须一致——这是与 `recent_documents_os_projection` 等
    // 「逐后端翻位」位的刻意差异，见 `window/platform.h` 的字段注释。
    constexpr SurfaceKind all_kinds[] = {SurfaceKind::Headless, SurfaceKind::Win32, SurfaceKind::Glfw,
                                         SurfaceKind::D3D11,    SurfaceKind::X11,   SurfaceKind::Wayland,
                                         SurfaceKind::MacOS,    SurfaceKind::Wasm};
    for (const auto kind : all_kinds) {
        Platform probe;
        probe.surface = kind;
        probe.device = DeviceKind::Desktop;
        const auto caps = probe.capabilities();
#ifdef AURORA_PLATFORM_WINDOWS
        AURORA_TEST_CHECK(caps.native_print_session);
        AURORA_TEST_CHECK(caps.url_scheme_registration);
        AURORA_TEST_CHECK(caps.file_association);
        AURORA_TEST_CHECK(caps.run_at_login);
#elif defined(AURORA_PLATFORM_LINUX)
        // Linux 打印桥缺位（不引 libcups），其余三项已接 XDG 桥。
        AURORA_TEST_CHECK_FALSE(caps.native_print_session);
        AURORA_TEST_CHECK(caps.url_scheme_registration);
        AURORA_TEST_CHECK(caps.file_association);
        AURORA_TEST_CHECK(caps.run_at_login);
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
        AURORA_TEST_CHECK(caps.native_print_session);
        AURORA_TEST_CHECK(caps.url_scheme_registration);
        AURORA_TEST_CHECK(caps.file_association);
        AURORA_TEST_CHECK(caps.run_at_login);
#else
        // 无 AppKit 腿 / wasm / 未知平台：机制与桥都不在，位如实为 false。
        AURORA_TEST_CHECK_FALSE(caps.native_print_session);
        AURORA_TEST_CHECK_FALSE(caps.url_scheme_registration);
        AURORA_TEST_CHECK_FALSE(caps.file_association);
        AURORA_TEST_CHECK_FALSE(caps.run_at_login);
#endif
    }
}

AURORA_TEST_CASE(print_session_requires_a_wired_backend) {
    PrintBackendGuard guard;
#if defined(AURORA_PLATFORM_WINDOWS) || (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
    // Win32 / macOS 有可按需自举的真桥：清掉伪造槽位后公共入口会去枚举真打印机，不适合无头断言。
    AURORA_TEST_SKIP("native print bridge boots on demand on this platform; unsupported degradation is not reachable");
#else
    auto targets = print_targets();
    AURORA_TEST_REQUIRE(!targets.ok());
    AURORA_TEST_CHECK_EQ(targets.error().code_enum, ErrorCode::PrintSessionUnsupported);

    auto session = PrintSession::open("any", square_spec());
    AURORA_TEST_REQUIRE(!session.ok());
    AURORA_TEST_CHECK_EQ(session.error().code_enum, ErrorCode::PrintSessionUnsupported);
#endif
}

}  // namespace aurora::test_cases::utest_document_output
