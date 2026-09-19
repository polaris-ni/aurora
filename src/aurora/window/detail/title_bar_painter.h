#pragma once

// CSD 自绘装饰的**绘制层**：内部头（与 `ime_composition.h` / `win32_cursor.h` 同列于 src/）。
//
// 为何单列：同一套装饰有两条上屏路径，且必须画出同一份内容——
//   * 软件路径（`WaylandSurface::present`）把它直接光栅进 Painter 帧缓冲，随 wl_shm 提交；
//   * GPU 路径（`WgpuWaylandSurface`）swapchain 独占同一 `wl_surface`，帧缓冲不上屏，故须把
//     装饰**录制**成 DisplayList 追加回放进当帧。
// 两处各写一份绘制代码必然漂移（一条路改了配色/几何，另一条静默落后），故绘制逻辑收敛为
// 「吃一份纯值状态 + 一个 Painter」的自由函数，两条路共用；状态由宿主各自装配。
//
// 与 `title_bar_geometry()` 的分工：几何仍是绘制与命中测试的单一来源，本函数只消费它，
// 不在函数内另推任何矩形（否则热区与像素分家，正是该函数要防的那类缺陷）。

#include <memory>
#include <string>

#include "aurora/core/image.h"  // Image（标题栏图标像素）
#include "aurora/core/types.h"  // Rect / Size / Point
#include "aurora/window/title_bar_style.h"
#include "aurora/window/window_state.h"  // WindowMode

namespace aurora {
class Painter;  // 直绘或录制的目标（完整定义见 render/painter.h）；本头只按引用取用
}  // namespace aurora

namespace aurora::csd {

/// @brief 一帧 CSD 装饰的绘制输入（纯值，无副作用；由宿主在绘制/录制那一刻装配）。
struct TitleBarPaintState {
    float width = 0.0F;  ///< 窗口宽（逻辑 dp，含装饰整幅）
    WindowMode mode = WindowMode::Normal;  ///< 决定标题栏显隐与「最大化/还原」字形
    bool fullscreen_bar_revealed = false;  ///< 全屏下顶边悬停揭示态（仅此一例全屏仍绘制）
    bool title_bar = false;  ///< 是否自绘标题栏（`DecorationPolicy` 解析结果 csd_title）
    bool active = true;  ///< 焦点态：决定取激活色还是失焦色
    bool resizable = true;  ///< false 时最大化钮退化为空盒（与命中测试同口径）
    int hovered_button = -1;  ///< 悬停按钮序号：0=min / 1=max / 2=close / -1=无
    std::string title;  ///< 标题文字（`style.show_title` 为 false 或本串为空则不画）
    std::shared_ptr<Image> icon;  ///< 图标像素（nullptr = 留白，预留几何位不变）
    TitleBarStyle style{};  ///< 样式（高度 / 配色 / 按钮布局 / 各元素显隐开关）

    /// @brief 本帧是否有任何装饰要画（GPU 路径据此整段跳过录制与回放，不产生空重放开销）。
    [[nodiscard]] auto paints_anything() const -> bool {
        if (!title_bar) {
            return false;
        }
        return mode != WindowMode::FullScreen || fullscreen_bar_revealed;
    }
};

/// @brief 绘制 CSD 标题栏：背景 → 图标 → 标题文字 → 三枚按钮（按 `style.button_layout` 分派
///        Adwaita / Windows / Mac 三种视觉语言）。
///
/// 入参 `Painter` 处于直绘还是录制模式皆可——录制模式下各原语只落命令、不触帧缓冲，这正是
/// 两条上屏路径能共用本函数的原因。坐标一律**逻辑 dp**（缩放在回放/光栅侧生效）。
auto paint_title_bar(Painter &p, const TitleBarPaintState &s) -> void;

}  // namespace aurora::csd
