// csd_geometry.h —— Wayland CSD「阴影边距」架构的纯值逻辑（仅库实现可见，不属公共 API）。
//
// 为何单列：阴影边距的尺寸换算、塌缩矩阵与缩放热区判定都是无副作用纯逻辑，但消费点散在
// WaylandSurface 的 configure / present / ptr_* 三条路径里。收敛为头内自由函数后，三条路径
// 共享同一真源（不会各写一套边界条件而漂移），单元测试也可在不连接 Wayland 合成器的前提下
// 逐值断言（一源 ↔ 一测，见 CODING_STANDARDS.md §3.1）。
//
// 坐标系契约：
// - 表面坐标（surface-local）：wl_pointer 事件与 wl_surface/wl_buffer 的原生坐标域，buffer
//   比可视窗口每边多出 shadow margin，原点在整幅 buffer 左上角。
// - 内容坐标（content）：window geometry 坐标域，原点在可视窗口左上角（即表面坐标平移 margin）。
//   应用内容、标题栏命中、Mouse 事件派发全部使用内容坐标。
#pragma once

#include <cstdint>

#include "aurora/core/color.h"  // Color（阴影基色）
#include "aurora/core/types.h"  // EdgeInsets / Size
#include "aurora/window/window_state.h"  // WindowMode

namespace aurora::csd {

/// @brief 阴影边距厚度（逻辑 dp）：可视窗口四周的 margin 带宽，阴影与缩放热区共用此带。
/// 取值约束：≥ 8（Wayland 边缘缩放热区的可用性下限，任务书 §2）；阴影模糊半径同值，
/// 使 alpha 在 buffer 外缘衰减到 0（无可见硬边）。
inline constexpr int AURORA_SHADOW_MARGIN_DP = 10;

/// @brief 当前装饰/几何态下是否存在阴影边距（Normal 态的客户端装饰窗口）。
///
/// 塌缩条件（任一即塌缩，margin=0）：非客户端装饰（SSD / Frameless）、最大化、全屏、
/// 合成器平铺（xdg-shell v2 TILED_*）。挂起（SUSPENDED）映射为非 Normal 模式，同样塌缩。
/// @param client_decorated 本窗口策略解析后需要自绘装饰（csd_title || csd_border）。
/// @param mode 当前窗口几何态。
/// @param tiled 合成器是否报告任一平铺状态（TILED_LEFT/RIGHT/TOP/BOTTOM）。
/// @return 存在阴影边距时 true。
[[nodiscard]] inline auto has_shadow_margin(bool client_decorated, WindowMode mode, bool tiled) -> bool {
    return client_decorated && mode == WindowMode::Normal && !tiled;
}

/// @brief 当前态的阴影边距厚度（塌缩态恒 0）。
/// @param client_decorated 本窗口是否自绘装饰。
/// @param mode 当前窗口几何态。
/// @param tiled 合成器平铺态。
/// @return 边距厚度（逻辑 dp）；塌缩态为 0。
[[nodiscard]] inline auto shadow_margin_dp(bool client_decorated, WindowMode mode, bool tiled) -> int {
    return has_shadow_margin(client_decorated, mode, tiled) ? AURORA_SHADOW_MARGIN_DP : 0;
}

/// @brief 整幅表面逻辑尺寸 = 内容尺寸 + 四周 margin。
/// @param content xdg configure（window geometry）口径的内容尺寸。
/// @param margin_dp 当前态边距厚度（逻辑 dp）。
/// @return buffer 对应的整幅表面逻辑尺寸。
[[nodiscard]] inline auto surface_size(const Size &content, int margin_dp) -> Size {
    const auto m = static_cast<float>(margin_dp);
    return Size{.width = content.width + (2.0F * m), .height = content.height + (2.0F * m)};
}

/// @brief `Surface::content_inset()` 的 Wayland CSD 口径：表面原点 → 应用内容原点的总偏移。
///
/// - Normal + 边距：`{m, m+tb, m, m}`（顶边 = margin + 标题栏；标题栏在可视窗口内顶部）；
/// - Borderless（无标题栏）+ 边距：`{m, m, m, m}`；
/// - 最大化 / 平铺：边距塌缩，保留标题栏顶偏移 `{0, tb, 0, 0}`；
/// - 全屏：标题栏退化为悬停揭示覆盖层、不回流布局，全零；
/// - SSD / Frameless（margin=0 且无标题栏）：全零。
/// @param csd_title 是否自绘标题栏（决定顶部标题栏分量）。
/// @param title_bar_height 标题栏高度（逻辑 dp）。
/// @param margin_dp 当前态边距厚度（由 `shadow_margin_dp` 求得）。
/// @param mode 当前窗口几何态。
/// @return 四向内边距（left/top/right/bottom 顺序）。
[[nodiscard]] inline auto content_inset_for(bool csd_title, float title_bar_height, int margin_dp, WindowMode mode)
    -> EdgeInsets {
    const auto m = static_cast<float>(margin_dp);
    const float tb = (csd_title && mode != WindowMode::FullScreen) ? title_bar_height : 0.0F;
    return EdgeInsets{.left = m, .top = m + tb, .right = m, .bottom = m};
}

/// @brief 表面坐标 → 内容坐标（减 margin 偏移；ptr_enter/ptr_motion/ptr_button 三入口统一映射）。
/// @param surface_coord wl_pointer 上报的表面本地坐标（逻辑 dp）。
/// @param margin_dp 当前态边距厚度。
/// @return 可视窗口（window geometry）坐标域内的对应坐标。
[[nodiscard]] inline auto surface_to_content(double surface_coord, int margin_dp) -> double {
    return surface_coord - static_cast<double>(margin_dp);
}

/// @brief 缩放热区边/角分类（平台中立，宿主再映射到各后端的 resize edge 枚举）。
enum class CsdResizeZone : std::uint8_t {
    None = 0,  ///< 不在缩放带内
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

/// @brief 判定内容坐标点是否落在 margin 缩放带及其边/角分类。
///
/// 判定在**内容坐标域**进行（表面坐标先经 `surface_to_content` 平移）：四带为内容矩形的
/// 外延环 `[-m, 0) / (W, W+m)` 等，表达式形状与旧「内容内嵌带」实现一致、仅坐标域外移。
/// 顶边带位于标题栏之外，故有标题栏时同样可从顶部边缘缩放（对齐 libadwaita）。
/// @param cx 内容坐标 x（逻辑 dp）。
/// @param cy 内容坐标 y。
/// @param content_w 内容宽（逻辑 dp）。
/// @param content_h 内容高。
/// @param margin_dp 边距厚度（0 = 无缩放带，恒返回 None）。
/// @return 边/角分类；带外为 None。
[[nodiscard]] inline auto classify_resize_zone(double cx, double cy, double content_w, double content_h,
                                               int margin_dp) -> CsdResizeZone {
    if (margin_dp <= 0) {
        return CsdResizeZone::None;
    }
    const auto m = static_cast<double>(margin_dp);
    const bool left = cx >= -m && cx < 0.0;
    const bool right = cx > content_w && cx < content_w + m;
    const bool top = cy >= -m && cy < 0.0;
    const bool bottom = cy > content_h && cy < content_h + m;
    if (left && top) {
        return CsdResizeZone::TopLeft;
    }
    if (right && top) {
        return CsdResizeZone::TopRight;
    }
    if (left && bottom) {
        return CsdResizeZone::BottomLeft;
    }
    if (right && bottom) {
        return CsdResizeZone::BottomRight;
    }
    if (left) {
        return CsdResizeZone::Left;
    }
    if (right) {
        return CsdResizeZone::Right;
    }
    if (top) {
        return CsdResizeZone::Top;
    }
    if (bottom) {
        return CsdResizeZone::Bottom;
    }
    return CsdResizeZone::None;
}

/// @brief 内容坐标点是否落在标题栏带内（标题栏在可视窗口内顶部，内容坐标域 y ∈ [0, tb)）。
/// @param cy 内容坐标 y。
/// @param title_bar_height 标题栏高度（逻辑 dp）。
/// @return 落在标题栏垂直带内时 true。
[[nodiscard]] inline auto point_in_title_bar(double cy, float title_bar_height) -> bool {
    return cy >= 0.0F && cy < static_cast<double>(title_bar_height);
}

// ── 阴影像素参数（软件上屏合成器与 GPU/DL underlay 的共同真源）─────────────────
//
// 关键架构事实：软件 Painter 的混合恒写 alpha=255（目标画布不透明是其全局不变量，
// 见 painter.cpp set_pixel/fill_rect_fast_path），无法直接在半透明 margin 上产出真 alpha。
// 故软件路径的 margin 像素在 present() 阶段由 `compose_shadow_margins_bgra` 直接写入
// wl_shm 缓冲；GPU 路径经 paint_window_shadow 录成 Shadow 命令由 RHI 真 alpha 合成。
// 两条路共享本处的基色/模糊几何与衰减因子，不允许各写一套视觉参数。

/// @brief 阴影基色（近内容边以此 alpha 起步，向外线性衰减到 0；libadwaita 风格柔黑）。
inline constexpr Color AURORA_SHADOW_BASE_COLOR{0, 0, 0, 70};

/// @brief 模糊半径相对 margin 的内缩（物理/逻辑同比例）：像素中心取整约定下，衰减严格
/// 在缓冲边界（距内容边 = margin 的最外列）前归零，四向外缘 alpha 恒 0、无硬切线。
inline constexpr int AURORA_SHADOW_BLUR_INSET_PX = 1;

/// @brief 阴影衰减因子：内容矩形外一点的距离 → [0,1] 不透明度因子。
///
/// 与 Painter::draw_shadow 边缘环及 WgpuRhi `fs_shadow` 同式：内部为 1，外部
/// `max(0, 1 − dist/blur)`；软件合成器在角部按欧氏距离组合两向距离（另两者同构）。
/// @param dist_px 像素到内容矩形最近覆盖列的距离（首条外延列 = 1）。
/// @param blur_px 模糊半径（像素；正值）。
/// @return [0,1] 衰减因子。
[[nodiscard]] inline auto shadow_attenuation(float dist_px, float blur_px) -> float {
    if (dist_px <= 0.0F) {
        return 1.0F;
    }
    if (blur_px <= 0.0F) {
        return 0.0F;
    }
    const float f = 1.0F - (dist_px / blur_px);
    return f > 0.0F ? f : 0.0F;
}

}  // namespace aurora::csd
