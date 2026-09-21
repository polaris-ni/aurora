#pragma once

// golden 基线比对的共享设施。仅测试框架内部使用，不进 `include/`、不进 `aurora_api.json`。
//
// 背景：`golden_dir()` / `env_value|flag|int` / `compare_or_update_golden()` 曾在 7 个测试文件里
// 各抄一份，导致语义化差异报告（`SnapshotDiffReport`）没有任何单一挂载点 —— 想让 golden 失败
// 说出「差异在哪儿、是谁画的」，就得改 7 处。本头把三者收敛为单一实现，并顺带把失败信息从
// 「N px, max delta M」升级为带空间位置与控件归因的摘要。
//
// 用法：
//   golden::compare_or_update("chart_line", render_to_png(...));           // 已写好临时 PNG
//   golden::compare_or_update_painter("painter_polyline", painter);        // 直接吃 Painter
//   golden::compare_or_update("chart_bar", current_path, &root);           // 带控件归因
//   golden::compare_gpu_tolerance("chart_bar", gpu_img, tol, budget);      // GPU 容差层（场景级申报）
//
// 四个运行时环境变量见 `codespec/BUILD_OPTIONS.md`：
// AURORA_GOLDEN_DIR / AURORA_UPDATE_GOLDEN / AURORA_GOLDEN_MAX_DIFF / AURORA_GOLDEN_MAX_PIXELS。

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"
#include "framework/isolation.h"

namespace aurora::testing::golden {

/// @brief 读取环境变量；未设置返回空视图（避免对空指针做下标访问）。
[[nodiscard]] inline auto env_value(const char *name) -> std::string_view {
    const char *raw = std::getenv(name);
    if (raw == nullptr) {
        return {};
    }
    return {raw};
}

/// @brief 环境变量是否「已设置且非空」。
[[nodiscard]] inline auto env_flag(const char *name) -> bool { return !env_value(name).empty(); }

/// @brief 环境变量取整；缺失或解析失败回退 fallback（`from_chars` 不抛异常）。
[[nodiscard]] inline auto env_int(const char *name, int fallback) -> int {
    const std::string_view raw = env_value(name);
    if (raw.empty()) {
        return fallback;
    }
    int parsed = fallback;
    // from_chars 需要 [begin, end) 区间，末指针只能由 data() + size() 求得，属必要指针算术。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *last = raw.data() + raw.size();
    // raw 是 string_view，data() 不保证 null 结尾；但读取区间已由 size() 定界，不存在越界。
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto [end, ec] = std::from_chars(raw.data(), last, parsed);
    return (ec == std::errc{} && end == last) ? parsed : fallback;
}

/// @brief golden 真值目录：`AURORA_GOLDEN_DIR` 覆盖优先，其次仓库根下的 `tests/golden`。
[[nodiscard]] inline auto dir() -> std::filesystem::path {
    const char *override_dir = std::getenv("AURORA_GOLDEN_DIR");
    if (override_dir != nullptr && *override_dir != '\0') {
        return {override_dir};
    }
    if (!isolation::repo_root().empty()) {
        return std::filesystem::path(isolation::repo_root()) / "tests" / "golden";
    }
    return {"tests/golden"};
}

/// @brief 把 Painter 的当前内容写成临时 PNG 并返回路径（供比对使用）。
[[nodiscard]] inline auto write_temp_png(const Painter &p, std::string_view tag) -> std::filesystem::path {
    const std::filesystem::path path =
        std::filesystem::path(isolation::temp_dir()) / ("current_" + std::string(tag) + ".png");
    AURORA_TEST_REQUIRE_TRUE(write_png(path.string().c_str(), p.width(), p.height(), p.data()).ok());
    return path;
}

/// @brief 与 golden 基线比对；`AURORA_UPDATE_GOLDEN` 非空时改为重生成基线。
///
/// 判据沿用历史语义：逐像素差异数 <= `AURORA_GOLDEN_MAX_PIXELS`（默认 0，即零漂移），
/// 单通道色差超过 `AURORA_GOLDEN_MAX_DIFF`（默认 0）才算差异像素。
///
/// 与过去唯一的行为差别在**失败信息**：除像素计数外，还会附上 `SnapshotDiffReport::to_text()`
/// 的摘要 —— 差异区域的位置，以及（调用方给了 `root` 时）归因到的控件路径与类型。
/// 归因只在失败时才做，通过路径上零额外开销。
///
/// @param base_name     基线文件名（不含 `.png`）
/// @param current_path  当前渲染产物的 PNG 路径（通常位于用例临时目录）
/// @param root          可选：已完成布局的控件树根，用于把差异区域归因到具体控件
inline auto compare_or_update(std::string_view base_name, const std::filesystem::path &current_path,
                              const Node *root = nullptr) -> void {
    const std::string name(base_name);
    const std::filesystem::path dir_path = dir();
    const std::filesystem::path golden_path = dir_path / (name + ".png");

    const auto current = Image::load(current_path.string());
    AURORA_TEST_REQUIRE_TRUE(current.ok());

    if (env_flag("AURORA_UPDATE_GOLDEN")) {
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
        std::filesystem::copy_file(current_path, golden_path, std::filesystem::copy_options::overwrite_existing, ec);
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(ec));
        return;
    }

    const auto golden = Image::load(golden_path.string());
    AURORA_TEST_REQUIRE_MSG(golden.ok(),
                            name + ".png missing or undecodable (run with AURORA_UPDATE_GOLDEN=1 to regenerate)");

    const int tolerance = env_int("AURORA_GOLDEN_MAX_DIFF", 0);
    const int max_pixels = env_int("AURORA_GOLDEN_MAX_PIXELS", 0);
    const SnapshotDiff diff = compare_snapshots(golden.value(), current.value(), tolerance);
    // 两侧已显式同型比较；tidy 对含显式 cast 的操作数仍误报混比。
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison)
    const bool within_budget = diff.pixel_diff_count <= static_cast<std::size_t>(std::max(0, max_pixels));

    std::string message = "pixel drift vs golden " + name + ": " + std::to_string(diff.pixel_diff_count) +
                          " px, max delta " + std::to_string(diff.max_color_delta);
    if (!within_budget) {
        const std::vector<WidgetBox> boxes = (root != nullptr) ? collect_widget_boxes(*root) : std::vector<WidgetBox>{};
        message += "\n" + build_snapshot_diff_report(golden.value(), current.value(), boxes, tolerance).to_text();
    }
    AURORA_TEST_CHECK_MSG(within_budget, message);
}

/// @brief 便捷重载：直接吃 Painter（内部先写临时 PNG，再走与文件版完全相同的比对路径）。
inline auto compare_or_update_painter(std::string_view base_name, const Painter &p) -> void {
    compare_or_update(base_name, write_temp_png(p, base_name));
}

/// @brief GPU 容差 golden：离屏 GPU 读回帧 vs 软件 SSOT 基线，按**场景级显式申报**的容差比对。
///
/// 判据词汇与 `compare_or_update` 同源（单通道色差 > tolerance 才算差异像素、差异像素数 ≤ 预算
/// 即通过），差别有二：
///   1. tolerance / 预算由调用方逐场景传常量，**不读** `AURORA_GOLDEN_MAX_DIFF/PIXELS` 全局旋钮
///      ——那两个是软件逐位红线的显式放松开关，若 GPU 场景复用它们，一视同仁设值会连带静默放松
///      软件侧判据；跨驱动 AA 差异预算属场景固有属性，就该写在场景旁。
///   2. 无 update 分支：基线恒为软件路径产物（唯一 SSOT），GPU 输出永不回写基线；`AURORA_UPDATE_GOLDEN`
///      对本函数无效（各 utest 仍负责重生成基线本身）。
///
/// 无论通过与否都 TRACE 实测值（差异像素数 / 最大单通道差）——容差校准需要常态可见，不止失败时。
///
/// @param base_name      基线文件名（不含 `.png`，与软件 utest 共用同一张基线）
/// @param current        GPU 离屏 read_pixels 产出的 RGBA 帧（直内存比较，无 PNG 往返）
/// @param tolerance      单通道容差（逐场景校准值）
/// @param max_diff_pixels 差异像素预算（逐场景校准值）
/// @param root           可选：已完成布局的控件树根，失败时把差异区域归因到控件
inline auto compare_gpu_tolerance(std::string_view base_name, const Image &current, int tolerance,
                                  std::size_t max_diff_pixels, const Node *root = nullptr) -> void {
    const std::string name(base_name);
    const auto golden = Image::load((dir() / (name + ".png")).string());
    AURORA_TEST_REQUIRE_MSG(golden.ok(), name + ".png missing or undecodable");

    const SnapshotDiff diff = compare_snapshots(golden.value(), current, tolerance);
    const bool within_budget = diff.pixel_diff_count <= max_diff_pixels;
    const std::string measured = "gpu drift vs software golden " + name + ": " + std::to_string(diff.pixel_diff_count) +
                                 " px / budget " + std::to_string(max_diff_pixels) + ", tol " +
                                 std::to_string(tolerance) + ", max delta " + std::to_string(diff.max_color_delta);
    AURORA_TEST_TRACE(measured);
    if (within_budget) {
        return;
    }
    const std::vector<WidgetBox> boxes = (root != nullptr) ? collect_widget_boxes(*root) : std::vector<WidgetBox>{};
    AURORA_TEST_CHECK_MSG(
        false, measured + "\n" + build_snapshot_diff_report(golden.value(), current, boxes, tolerance).to_text());
}

}  // namespace aurora::testing::golden
