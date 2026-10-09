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
///
/// 坐标口径：`width`/`height` 是**内容尺寸**（window geometry 口径）；内容区在整幅表面上的
/// 原点为 (`origin_x`, `origin_y`) = 阴影边距（塌缩态 0）。标题栏 overlay 画在内容区内部
/// 顶部，阴影 underlay 画在内容区外的 margin 带，二者经同一状态装配，软件/GPU 两路同源。
struct TitleBarPaintState {
    float width = 0.0F;  ///< 内容宽（逻辑 dp，window geometry 口径）
    float height = 0.0F;  ///< 内容高（逻辑 dp，window geometry 口径）
    float origin_x = 0.0F;  ///< 内容区原点相对整幅表面的 x 偏移（= 阴影边距，塌缩态 0）
    float origin_y = 0.0F;  ///< 内容区原点相对整幅表面的 y 偏移（= 阴影边距，塌缩态 0）
    WindowMode mode = WindowMode::Normal;  ///< 决定标题栏显隐与「最大化/还原」字形
    bool fullscreen_bar_revealed = false;  ///< 全屏下顶边悬停揭示态（仅此一例全屏仍绘制）
    bool title_bar = false;  ///< 是否自绘标题栏（`DecorationPolicy` 解析结果 csd_title）
    bool active = true;  ///< 焦点态：决定取激活色还是失焦色
    bool resizable = true;  ///< false 时最大化钮退化为空盒（与命中测试同口径）
    int hovered_button = -1;  ///< 悬停按钮序号：0=min / 1=max / 2=close / -1=无
    std::string title;  ///< 标题文字（`style.show_title` 为 false 或本串为空则不画）
    std::shared_ptr<Image> icon;  ///< 图标像素（nullptr = 留白，预留几何位不变）
    TitleBarStyle style{};  ///< 样式（高度 / 配色 / 按钮布局 / 各元素显隐开关）
    /// @brief 阴影边距厚度（逻辑 px；0 = 本帧无阴影：塌缩态或非 CSD）。
    /// 由 35851da 过渡实现的 `border`（可见边框带厚度）字段转型：边带绘制已删除，
    /// 本字段只承载 margin 几何（阴影范围 + underlay 录制判据），与 `content_inset()` 同口径。
    float shadow_margin = 0.0F;

    /// @brief 本帧标题栏 overlay 是否有内容要画（GPU overlay 路径据此跳过空回放）。
    /// 阴影 underlay 的判据独立为 `shadow_margin > 0`（Borderless 无标题栏仍有阴影）。
    /// @return 标题栏（或全屏揭示条）需要绘制时 true。
    [[nodiscard]] auto paints_title_bar() const -> bool {
        if (!title_bar) {
            return false;  // Borderless：只有阴影 underlay，无标题栏 overlay
        }
        return mode != WindowMode::FullScreen || fullscreen_bar_revealed;
    }
};

/// @brief 绘制 CSD 标题栏 overlay：背景 → 图标 → 标题文字 → 三枚按钮（按 `style.button_layout`
///        分派 Adwaita / Windows / Mac 三种视觉语言）。
///
/// 入参 `Painter` 处于直绘还是录制模式皆可——录制模式下各原语只落命令、不触帧缓冲，这正是
/// 两条上屏路径能共用本函数的原因。坐标一律**表面逻辑 dp**（内容几何按 `origin_x/origin_y`
/// 平移到内容区），缩放在回放/光栅侧生效。
auto paint_title_bar(Painter &p, const TitleBarPaintState &s) -> void;

/// @brief 录制窗口阴影 underlay（**GPU/DisplayList 路径专用**）：内容矩形外 margin 带的柔和
///        投影（libadwaita 风格，无可见边框带）。GPU 端在 app 帧之前回放，Shadow 管线以真
///        alpha 合成到透明清除的 swapchain（`shadow_margin <= 0` 时无操作）。
///
///        软件 wl_shm 路径**不得**调用本函数直绘——Painter 目标画布恒不透明（混合恒写
///        alpha=255），半透明 margin 由 `csd::compose_shadow_margins_bgra` 在 present()
///        阶段直接写入 BGRA 缓冲；两路视觉参数同源于 detail/csd_geometry.h。
/// @param p 录制目标（软件 Painter 仅用于录 DisplayList，回放侧为 RHI）。
/// @param s 本帧装饰状态（`shadow_margin <= 0` 时无操作）。
auto paint_window_shadow(Painter &p, const TitleBarPaintState &s) -> void;

}  // namespace aurora::csd
