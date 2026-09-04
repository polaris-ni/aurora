#pragma once

// 可视化调试叠层 + 控件拾取（specification/06-app-platform.md §11.1）。
//
// 设计要点：
// - 本头**始终声明** `aurora::debug` 的叠层 / 拾取 API，使调用可编译、ODR 安全。
// - 函数**体**按 `AURORA_ENABLE_DEBUG` 裁切：Release（未开开关）下，`set_flags` 为 no-op、
//   `flags()` 返回全 false、`any_flag_enabled()` 返回 false、`paint_debug_overlays` / `widget_picker`
//   为空操作；零叠层 / 零拾取代码被编译进 Release，保持零开销。
// - 叠层**不在** `Widget::paint` 内绘制（DL 缓存 replay 路径会 `return` 漏画且顺序难控），
//   而是在 `Window::present_root` **全树绘制后**做一次独立树遍历统一绘制（含 overdraw 热力图），
//   完全不侵入 `Widget::paint`。`repaint_highlight` 依赖 `Widget::render_into` 入口打的「本帧重绘」标记。
// - `widget_picker` 直接接收根控件（Surface 不持有 widget 树根，故不取 Surface）；复用既有
//   `Widget::hit_test_chain`，返回根→最深的命中链与最深层控件信息。

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/types.h"

namespace aurora {

class Widget;  // 前向声明：仅在声明中按引用使用
class Painter;  // 前向声明
class BuildContext;  // 前向声明

namespace debug {

/// @brief 可视化调试叠层开关（全部默认 off）。任一开启即进入叠层绘制分支。
struct DebugPaintFlags {
    bool layout_guides = false;  ///< render box 边框 / 对齐参考（对齐 Flutter debugPaintSizeEnabled）。
    bool relayout_boundaries = false;  ///< 重排边界框（复用 `Widget::is_relayout_boundary()`）。
    bool layer_borders = false;  ///< 离屏缓存层（含 `cache_layer()` 修饰）边框。
    bool repaint_highlight = false;  ///< 本帧实际重绘的控件循环色高亮（rainbow）。
    bool overdraw = false;  ///< 控件粒度过度绘制热力图（painter_bounds 半透明叠加）。
};

/// @brief 设置全局叠层开关（DEBUG 下生效；Release 为 no-op）。
auto set_flags(const DebugPaintFlags &f) -> void;

/// @brief 读取全局叠层开关（Release 恒为全 false）。
[[nodiscard]] auto flags() -> DebugPaintFlags;

/// @brief 是否有任一叠层开启（Release 恒 false，供 `present_root` 短路）。
[[nodiscard]] auto any_flag_enabled() -> bool;

// ---- 调试帧计数（仅 repaint_highlight 需要；DEBUG 下递增）----
[[nodiscard]] auto current_debug_frame() -> std::uint64_t;
auto bump_debug_frame() -> void;

/// @brief 全树绘制后统一绘制 5 个叠层（含 overdraw 热力图）。由 `present_root` 在
///        `AURORA_ENABLE_DEBUG` 且 `any_flag_enabled()` 时调用；Release 为空操作。
/// @param p 已绘制完 app 树的 painter（叠层画在最上层）。
/// @param root 根控件。
/// @param root_bounds 根控件全局盒（通常 `Rect{Point{0,0}, surface.size()}`）。
/// @param ctx 构造 / 绘制用的 BuildContext（叠层遍历不依赖其环境）。
auto paint_debug_overlays(Painter &p, const Widget &root, const Rect &root_bounds, const BuildContext &ctx) -> void;

/// @brief 叠层绘制统计（供测试断言，无需检查像素）。每次 `paint_debug_overlays` 前清零。
struct DebugOverlayStats {
    std::uint64_t layout_guides_drawn = 0;  ///< 绘制了 layout_guides 边框的控件数。
    std::uint64_t relayout_boundaries_drawn = 0;  ///< 绘制了 relayout boundary 边框的控件数。
    std::uint64_t layer_borders_drawn = 0;  ///< 绘制了缓存层边框的控件数。
    std::uint64_t repaint_highlight_drawn = 0;  ///< 被 repaint_highlight 高亮的控件数。
    std::uint64_t overdraw_regions_drawn = 0;  ///< 参与 overdraw 热力图叠加的控件（非根）数。
};

[[nodiscard]] auto overlay_stats() -> DebugOverlayStats;
auto reset_overlay_stats() -> void;

/// @brief 控件拾取结果中的单层节点。
struct DebugPickNode {
    std::string type_name;  ///< 命中控件类型名（`Widget::type_name()`）。
    Rect bounds;  ///< 该控件全局盒（`paint_bounds()`）。
};

/// @brief 控件拾取结果：根→最深的完整命中链 + 是否命中。
struct DebugPickResult {
    std::vector<DebugPickNode> chain;  ///< 命中链（chain[0] = 根，末元素 = 最深命中控件）。
    bool hit = false;  ///< 是否在 `screen` 处命中任意控件。
};

/// @brief 控件拾取：输入根控件与屏幕（窗口逻辑 dp）坐标，复用 `Widget::hit_test_chain`
///        解析命中链，返回根→最深路径与最深层控件信息。
/// @param root 根控件（Surface 不持有树根，故由调用方提供，如 `Window` / 测试树）。
///             非 const：命中测试经 `hit_test_chain` 进入各控件的非 const 虚函数链。
/// @param root_bounds 根全局盒（用于把 `screen` 作为根局部坐标传入 hit_test_chain）。
/// @param ctx BuildContext（命中测试多忽略其环境，可默认构造）。
/// @param screen 窗口逻辑 dp 坐标（相对根原点；与 `root_bounds.origin` 同坐标系）。
/// @note Release 下返回 `{ {}, false }`（空操作）。
[[nodiscard]] auto widget_picker(Widget &root, const Rect &root_bounds, const BuildContext &ctx, Point screen)
    -> DebugPickResult;

}  // namespace debug
}  // namespace aurora
