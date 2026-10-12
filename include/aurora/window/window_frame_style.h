#pragma once

#include "aurora/core/color.h"

namespace aurora {

/// @brief CSD 自绘窗框样式值类型（纯数据聚合，可直接 `WindowFrameStyle{...}` 指定成员初始化）。
///
/// 承载「内容矩形之外那一圈」的全部视觉参数：投影的开关 / 厚度 / 基色 / 模糊半径，以及窗口
/// 外轮廓圆角。默认成员值逐值复现本库既有阴影（`AURORA_SHADOW_MARGIN_DP = 10`、
/// `AURORA_SHADOW_BASE_COLOR{0,0,0,70}`、模糊半径 = margin − 1），故 `WindowFrameStyle{}`
/// 与引入本类型之前的观感完全一致。
///
/// 与 `TitleBarStyle` 的分工：后者管标题栏「带内」，本类型管窗框「带外」。两者各自持有
/// `corner_radius`，因为它们是**两个不同的形状**——标题栏背景的下两角与窗口外轮廓的四角；
/// 预设会把二者配成一致的值，但语义上不共享一个字段。
///
/// 「0 = 自动」的判据与 `TitleBarMetrics` 同源：`shadow_blur_radius = 0` 表示沿用
/// 「margin − 1」的既有推导式，非 0 才是显式覆写（超出 margin 的部分会被夹到 margin）。
///
/// 能力不足时的降级由各后端执行：探测不到合成能力（X11 无 composite manager、Win32 DWM
/// 未启用）时把厚度就地降为 0 并打 `AURORA_LOG_INFO`——**只撤投影**，不依赖合成能力的
/// 圆角等分量照常生效。
struct WindowFrameStyle {
    bool shadow_enabled = true;  ///< 是否绘制窗框投影（false = 无投影，内容矩形即整幅表面）
    int shadow_margin_dp = 10;  ///< 投影带厚度（逻辑 dp）；同时决定可拖拽缩放的热区宽度
    Color shadow_color{0, 0, 0, 70};  ///< 投影基色（含 alpha；衰减由 `csd::shadow_attenuation` 施加）
    float shadow_blur_radius = 0.0F;  ///< 投影模糊半径（逻辑 dp）；**0 = 由 margin − 1 推导**
    float corner_radius = 0.0F;  ///< 窗口外轮廓四角圆角半径（逻辑 dp）；0 = 直角
};

}  // namespace aurora
