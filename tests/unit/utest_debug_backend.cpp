/// 测试类型: unit
/// 目标单元: include/aurora/debug/debug_backend.h
/// 测试说明: 覆盖真实后端 DEBUG 门面——输出目录三件套（resolve_output_path 的空串/纯文件名/
/// 显式路径规则、set_output_directory 往返与默认恢复）、capture（关闭态结构化错误、开启态
/// 帧缓冲 PNG 落盘、OnScreenWindow 在无窗口后端 unsupported）、surface_state（开启态逐字段
/// 快照 / 关闭态 unavailable）。测试 TU 不写宏门控：以 feature_flags().debug 运行时探测。
/// Surface 用本 TU 内的最小桩实现（不依赖 AURORA_BACKEND_HEADLESS 等后端宏），临时文件落
/// 系统临时目录并在用例内清理。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "aurora/debug/debug_backend.h"
#include "aurora/debug/feature_flags.h"  // 运行时探测 AURORA_ENABLE_DEBUG 的归一化镜像
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_debug_backend {

using aurora::debug::capture;
using aurora::debug::CaptureSource;
using aurora::debug::feature_flags;
using aurora::debug::output_directory;
using aurora::debug::resolve_output_path;
using aurora::debug::set_output_directory;
using aurora::debug::surface_state;

namespace {

/// @brief 最小 Surface 桩：仅满足门面读取的虚接口，帧缓冲为固定 4×4 RGBA。
class StubSurface final : public Surface {
  public:
    StubSurface() { pixels_.assign(static_cast<std::size_t>(4U * 4U * 4U), 128U); }

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override {
        width_ = width;
        height_ = height;
        return Result<bool>{true};
    }
    [[nodiscard]] auto painter() -> Painter& override { return painter_; }
    [[nodiscard]] auto present() -> Result<bool> override { return Result<bool>{true}; }
    [[nodiscard]] auto size() const -> Size override {
        return Size{.width = static_cast<float>(width_), .height = static_cast<float>(height_)};
    }
    [[nodiscard]] auto scale_factor() const -> float override { return 2.0F; }
    [[nodiscard]] auto should_close() const -> bool override { return true; }
    [[nodiscard]] auto clear_color() const -> Color override { return Color{64, 128, 192, 255}; }
    [[nodiscard]] auto frame_count() const -> int override { return 7; }
    [[nodiscard]] auto data() const -> const std::uint8_t* override { return pixels_.data(); }

  private:
    Painter painter_;
    std::vector<std::uint8_t> pixels_;
    int width_ = 4;
    int height_ = 4;
};

/// @brief 用例专属临时 PNG 路径（框架隔离临时目录 test_temp/<case> 下固定名；各套件独立进程不会互撞）。
[[nodiscard]] auto temp_png_path(const std::string& name) -> std::string {
    return (std::filesystem::path{aurora::testing::isolation::temp_dir()} / name).string();
}

/// @brief 清理临时文件（忽略不存在/权限错误）。
auto remove_quiet(const std::string& path) -> void {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

/// @brief 运行时探测 AURORA_ENABLE_DEBUG 是否生效（feature_flags 为始终可用的编译期快照）。
[[nodiscard]] auto probe_debug_enabled() -> bool { return feature_flags().debug; }

}  // namespace

AURORA_TEST_CASE(resolve_empty_path_returns_output_directory) {
    // 空串契约：返回 output_directory() 本身（仅目录，无文件名）。
    AURORA_TEST_CHECK_EQ(resolve_output_path(""), output_directory());
}

AURORA_TEST_CASE(resolve_pure_filename_joins_output_directory) {
    const std::string original = output_directory();
    const std::string custom = (std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_outdir").string();
    set_output_directory(custom);

    // 纯文件名（无目录分隔）落入缺省输出目录。
    const std::string resolved = resolve_output_path("shot.png");
    AURORA_TEST_CHECK_EQ(resolved, (std::filesystem::path(custom) / "shot.png").string());

    set_output_directory(original);  // 恢复全局输出目录
}

AURORA_TEST_CASE(resolve_explicit_paths_pass_through) {
    const std::string original = output_directory();
    set_output_directory((std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_outdir").string());

    // 绝对路径原样返回。
    const std::string absolute = temp_png_path("abs_shot.png");
    AURORA_TEST_CHECK_EQ(resolve_output_path(absolute), absolute);
    // 含目录分隔的相对路径同样视为显式路径，不改动。
    AURORA_TEST_CHECK_EQ(resolve_output_path(std::string("sub/dir/shot.png")), std::string("sub/dir/shot.png"));

    set_output_directory(original);  // 恢复全局输出目录
}

AURORA_TEST_CASE(output_directory_roundtrip_and_default_reset) {
    const std::string original = output_directory();

    const std::string custom = (std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_outdir2").string();
    set_output_directory(custom);
    AURORA_TEST_CHECK_EQ(output_directory(), custom);

    // 空串恢复缺省：当前程序运行目录下的 ./aurora_debug/。
    set_output_directory("");
    // TEST_TEMP_EXEMPT: 断言调试后端默认输出目录为 cwd/aurora_debug（产品默认行为，非写入测试临时文件）。
    const std::filesystem::path expected_default = std::filesystem::current_path() / "aurora_debug";
    AURORA_TEST_CHECK_EQ(output_directory(), expected_default.string());

    set_output_directory(original);  // 恢复用例前的取值（可能为显式目录）
}

AURORA_TEST_CASE(capture_disabled_returns_structured_error) {
    if (probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 已启用：关闭态 disabled 错误语义不适用");
    }
    StubSurface surface;
    const std::string path = temp_png_path("aurora_utest_capture_disabled.png");
    remove_quiet(path);

    const Result<bool> r = capture(surface, path);
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::GeneralNotSupported);
    AURORA_TEST_CHECK_TRUE(r.error().message.find("AURORA_ENABLE_DEBUG") != std::string::npos);
    AURORA_TEST_CHECK_FALSE(std::filesystem::exists(path));  // 关闭态不触碰文件系统
}

AURORA_TEST_CASE(capture_framebuffer_writes_png_file) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：帧缓冲截图按宏裁切返回 disabled 错误");
    }
    StubSurface surface;
    AURORA_TEST_REQUIRE_TRUE(surface.begin_frame(4, 4).ok());

    const std::string path = temp_png_path("aurora_utest_capture_fb.png");
    remove_quiet(path);

    const Result<bool> r = capture(surface, path, CaptureSource::Framebuffer);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_TRUE(r.value());

    // 落盘文件存在、非空、以 PNG 魔数开头。
    std::ifstream f(path, std::ios::binary);
    AURORA_TEST_REQUIRE_TRUE(f.is_open());
    std::vector<char> bytes{(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>{}};
    AURORA_TEST_CHECK_GT(bytes.size(), 8U);
    AURORA_TEST_CHECK_EQ(bytes[0], '\x89');
    AURORA_TEST_CHECK_EQ(bytes[1], 'P');
    AURORA_TEST_CHECK_EQ(bytes[2], 'N');
    AURORA_TEST_CHECK_EQ(bytes[3], 'G');
    f.close();
    remove_quiet(path);
}

AURORA_TEST_CASE(capture_onscreen_window_unsupported_or_disabled) {
    // 双构建统一断言 GeneralNotSupported：开启态为桩后端无窗口截图能力；关闭态为宏未开。
    StubSurface surface;
    const std::string path = temp_png_path("aurora_utest_capture_window.png");
    remove_quiet(path);

    const Result<bool> r = capture(surface, path, CaptureSource::OnScreenWindow);
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::GeneralNotSupported);
    remove_quiet(path);
}

AURORA_TEST_CASE(surface_state_reflects_surface_when_enabled) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：surface_state 按宏裁切返回 unavailable");
    }
    StubSurface surface;
    const Json j = surface_state(surface);
    AURORA_TEST_CHECK_EQ(j["available"], true);
    AURORA_TEST_CHECK_EQ(j["width"], 4);
    AURORA_TEST_CHECK_EQ(j["height"], 4);
    AURORA_TEST_CHECK_NEAR(j["scale_factor"].get<double>(), 2.0, 1e-6);
    AURORA_TEST_CHECK_EQ(j["frame_count"], 7);
    AURORA_TEST_CHECK_TRUE(j["clear_color"].is_array());
    AURORA_TEST_CHECK_EQ(j["clear_color"].size(), 4U);
    AURORA_TEST_CHECK_EQ(j["clear_color"][0], 64);
    AURORA_TEST_CHECK_EQ(j["clear_color"][1], 128);
    AURORA_TEST_CHECK_EQ(j["clear_color"][2], 192);
    AURORA_TEST_CHECK_EQ(j["clear_color"][3], 255);
    AURORA_TEST_CHECK_EQ(j["should_close"], true);
    AURORA_TEST_CHECK_EQ(j["has_native_window"], false);  // 桩无原生窗口句柄
}

AURORA_TEST_CASE(surface_state_unavailable_when_disabled) {
    if (probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 已启用：关闭态 unavailable 语义不适用");
    }
    StubSurface surface;
    const Json j = surface_state(surface);
    AURORA_TEST_CHECK_EQ(j["available"], false);
    AURORA_TEST_CHECK_TRUE(j["reason"].get<std::string>().find("AURORA_ENABLE_DEBUG") != std::string::npos);
}

}  // namespace aurora::test_cases::utest_debug_backend
