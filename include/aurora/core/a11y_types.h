#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "aurora/core/types.h"

namespace aurora {

/// @brief 无障碍状态位集（平台中立；三桥共用的语义来源）。
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
    bool focused = false;  ///< 持有键盘焦点
    bool checkable = false;  ///< 可勾选（Checkbox / Switch）
    bool checked = false;  ///< 当前勾选态
    bool selected = false;  ///< 列表选中项（预留位：选择模型未接入）
    bool read_only = false;  ///< 文本只读
    bool disabled = false;  ///< 禁用态（预留位：Widget 尚无禁用语义）
    bool visible = false;  ///< 可见（自身 `show` 且祖先均可见；派生位）
    bool focusable = false;  ///< 可参与焦点序（派生位）
    bool offscreen = false;  ///< 视觉不可达：零尺寸 / 被祖先裁剪 / 滚出视口（派生位）
    bool expandable = false;  ///< 可展开（ExpandCollapse 语义，预留位）
    bool expanded = false;  ///< 已展开（预留位）
    bool multiline = false;  ///< 多行文本（UIA 侧决定 Edit / Document）
    bool password = false;  ///< 密码框：读屏不得逐字朗出
};

/// @brief 无障碍取值域：可量化控件的 min/max/step/value。
/// 供 Slider / ProgressIndicator 等覆写 `Widget::accessibility_range()`；
/// UIA 侧有值即暴露 `IRangeValueProvider`，AT-SPI2 侧暴露 `org.a11y.atspi.Value`。
/// @note Side-effects: reads state
struct AccessibilityRange {
    double min = 0.0;  ///< 下界
    double max = 1.0;  ///< 上界
    double step = 0.0;  ///< 步长（0 = 连续）
    double value = 0.0;  ///< 当前值
};

/// @brief 无障碍文本选区：**UTF-8 字节偏移**的半开区间 `[start, end)`（§4.5）。
/// 偏移单位一律 UTF-8 字节（与控件内部字符串同构、零转换成本）；UTF-16 换算
/// （UIA `ITextRangeProvider` 要求）只在 Win32 桥边界做一次。
/// @note Side-effects: pure
struct AccessibilityTextSelection {
    std::size_t start = 0;  ///< 选区起点（字节偏移，含）
    std::size_t end = 0;  ///< 选区终点（字节偏移，不含）
};

/// @brief 无障碍滚动量：滚动容器的 {min, max, position} + 视口/内容两量。
/// 由 `Scroll` 等滚动控件覆写 `Widget::accessibility_scroll()` 提供；UIA 侧据此暴露
/// `IScrollProvider`（`get_VerticalViewSize` 取 `viewport/content × 100` 表达「可见内容占全部内容」，
/// `get_VerticalScrollPercent` 取 `(position−min)/(max−min) × 100` 表达「已滚到哪儿」），
/// AT-SPI2 / macOS 侧映射 `Component.ScrollTo` / `accessibilityPerformScrollToVisible`。
/// @note 不变量：`max - min` 应等于 `content - viewport`（同义——可滚跨度）；两量独立提供是为了让
///       `get_VerticalViewSize` 直接从几何算百分比，不依赖「差值恰好等于 content−viewport」这条隐含约式。
/// @note Side-effects: reads state
struct AccessibilityScrollRange {
    double min = 0.0;  ///< 最小偏移（通常为 0）
    double max = 0.0;  ///< 最大偏移（内容量 − 视口量；不可滚时为 0）
    double position = 0.0;  ///< 当前偏移
    double viewport = 0.0;  ///< 视口尺寸（纵向为视口高）：`get_VerticalViewSize` 的分子
    double content = 0.0;  ///< 内容总尺寸（纵向为内容高）：`get_VerticalViewSize` 的分母
};

/// @brief 由滚动量算 UIA `VerticalViewSize` 百分比（可见内容占全部内容的百分比）。
/// 纯函数、平台中立：UIA provider 与三桥共用的唯一真源，便于脱离 COM 环境做三腿单测。
/// @param range 滚动量：视口尺寸取 `.viewport`，内容总尺寸取 `.content`。
/// @return `viewport / content × 100`，夹到 `[0, 100]`；无跨度 / 不支持滚动
///         （`content` 或 `viewport` 非正）报 100（全部可见）。
/// @note 与 `max - min` 无关：即便 `content - viewport != max - min` 也能算得有几何意义的百分比。
/// @note Side-effects: pure
/// @note 措辞勿改成「反引号内以 `.` 开头 + 等号」的形态（如 `.viewport` = …）：Doxygen 会把
///       点号开头的反引号内容当 HTML 属性解析，生成定宽标签时失配，以 WARN_AS_ERROR 判红。
///       改成「取 `x`」的措辞即可绕过，语义不变。
[[nodiscard]] inline auto compute_vertical_view_size(const AccessibilityScrollRange &range) -> double {
    if (range.content <= 0.0 || range.viewport <= 0.0) {
        return 100.0;  // 无内容 / 无视口 ⇒ 全部可见
    }
    return std::clamp((range.viewport / range.content) * 100.0, 0.0, 100.0);
}

}  // namespace aurora
