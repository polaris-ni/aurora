#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "aurora/core/types.h"

namespace aurora {

/// @brief 无障碍状态位集（平台中立；三桥共用的语义来源）。
///
/// 对标 Qt `QAccessible::State` / Chromium `AXNodeData` 的状态位裁剪到 Aurora 现有语义：
/// 六位基础位（focused/checkable/checked/selected/read_only/disabled）+ 第二轮评审补齐的
/// visible/focusable/offscreen/expandable/expanded/multiline/password。
///
/// 取值分工：控件经 `Widget::accessibility_state()` 虚钩子填**自身可知**的位；
/// `visible` / `focusable` / `offscreen` 三个**派生位**由共享语义层（`core/accessibility.h`
/// 的 `build_accessibility_node`）按可见性与几何统一填，控件无需关心。
///
/// @note `disabled` 与 `selected` 是预留位：Widget 尚无禁用语义、List 选择模型尚未接入，
///       二者当前无生产者（恒 false），待对应能力落地后由控件覆写填充。
/// @note Thread: main-thread only
/// @note Side-effects: pure
struct AccessibilityState {
    bool focused = false;     ///< 持有键盘焦点
    bool checkable = false;   ///< 可勾选（Checkbox / Switch）
    bool checked = false;     ///< 当前勾选态
    bool selected = false;    ///< 列表选中项（预留位：选择模型未接入）
    bool read_only = false;   ///< 文本只读
    bool disabled = false;    ///< 禁用态（预留位：Widget 尚无禁用语义）
    bool visible = false;     ///< 可见（自身 `show` 且祖先均可见；派生位）
    bool focusable = false;   ///< 可参与焦点序（派生位）
    bool offscreen = false;   ///< 视觉不可达：零尺寸 / 被祖先裁剪 / 滚出视口（派生位）
    bool expandable = false;  ///< 可展开（ExpandCollapse 语义，预留位）
    bool expanded = false;    ///< 已展开（预留位）
    bool multiline = false;   ///< 多行文本（UIA 侧决定 Edit / Document）
    bool password = false;    ///< 密码框：读屏不得逐字朗出
};

/// @brief 无障碍取值域：可量化控件的 min/max/step/value（D7）。
///
/// 供 Slider / ProgressIndicator 等覆写 `Widget::accessibility_range()`；
/// UIA 侧有值即暴露 `IRangeValueProvider`，AT-SPI2 侧暴露 `org.a11y.atspi.Value`。
/// @note Side-effects: reads state
struct AccessibilityRange {
    double min = 0.0;    ///< 下界
    double max = 1.0;    ///< 上界
    double step = 0.0;   ///< 步长（0 = 连续）
    double value = 0.0;  ///< 当前值
};

/// @brief 无障碍文本选区：**UTF-8 字节偏移**的半开区间 `[start, end)`（§4.5）。
///
/// 偏移单位一律 UTF-8 字节（与控件内部字符串同构、零转换成本）；UTF-16 换算
/// （UIA `ITextRangeProvider` 要求）只在 Win32 桥边界做一次。
/// @note Side-effects: pure
struct AccessibilityTextSelection {
    std::size_t start = 0;  ///< 选区起点（字节偏移，含）
    std::size_t end = 0;    ///< 选区终点（字节偏移，不含）
};

/// @brief 无障碍滚动量：滚动容器的 {min, max, position} 三分量（G32）。
///
/// 由 `Scroll` 等滚动控件覆写 `Widget::accessibility_scroll()` 提供；UIA 侧据此暴露
/// `IScrollProvider`（可滚动量换算为百分比），AT-SPI2 / macOS 侧映射
/// `Component.ScrollTo` / `accessibilityPerformScrollToVisible`。
/// @note Side-effects: reads state
struct AccessibilityScrollRange {
    double min = 0.0;       ///< 最小偏移（通常为 0）
    double max = 0.0;       ///< 最大偏移（内容量 − 视口量；不可滚时为 0）
    double position = 0.0;  ///< 当前偏移
};

}  // namespace aurora
