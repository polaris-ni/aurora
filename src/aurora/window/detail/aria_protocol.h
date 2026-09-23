#pragma once

// Wasm/ARIA 平台桥的**平台中立折算层**：语义树快照 → WAI-ARIA 镜像元素（role /
// aria-* 属性 / 文本内容 / 反向动作位）与 DOM 应用载荷（全量 JSON / 增量 ops JSON /
// 播报 JSON）。纯 C++、零 Emscripten、零平台头 —— 与 `atspi_protocol` /
// `ime_composition` / `title_bar_painter` 同列「先纯后桥」纪律：全部折算逻辑无头可
// 单测（Windows CI 亦覆盖），`wasm_aria.cpp` 只剩 JSON 落 DOM 的机械粘合。
//
// 规范事实来源（W3C WAI-ARIA 1.1/1.2，2026-09-21 逐名核对）：
//  * role 名：ARIA 1.2「Roles Definition」列表名（小写连字符）——本文档用到的每个
//    role 均为其成员：button / checkbox / switch / slider / textbox / img / list /
//    listitem / heading / progressbar / dialog / generic / text。
//    注意 `text`（静态文本，Chromium 映射为 StaticText）与 `textbox`（可编辑，
//    配 aria-multiline）是 ARIA 1.1 起才进入规范主文的较新名字。
//  * 属性名：ARIA 1.2「State and Property Definitions」，一律小写、`aria-` 前缀。
//    布尔属性按规范写字符串 "true"/"false"；数值属性写十进制字符串。
//  * aria-activedescendant 指向元素 **DOM id**（IDREF）⇒ 每个镜像元素必须有无歧义
//    的确定性 id（本层规则：`aurora-a11y-<runtime_id>`）。
//  * JSON 转义：RFC 8259 最小集（`"` `\` 与控制字符 < 0x20 转 `\u00xx`；
//    `\b \f \n \r \t` 用短形式），UTF-8 多字节序列原样透传（JSON 允许）。
//  * 播报走 `aria-live` 区域（ARIA 1.2 Live Region 章）：文本替换即触发朗读，
//    同文本重复需先清空再回写（JS 侧职责，本层载荷保持纯数据）。
//
// 镜像形态（与原生桥的**已知差异**，如实申报）：镜像元素挂在页面隐藏容器内，
// 只有语义、没有画布几何 —— 不产出位置/尺寸（ARIA 无坐标概念，读屏按 DOM 顺序
// 导航，不受影响）。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/widget/a11y_diff.h"

namespace aurora::detail {

/// @brief 一条折算后的 ARIA 属性（键为规范全名，含 `data-aurora-*` 自有扩展位）。
struct AriaAttr {
    std::string key;  ///< 属性名（如 "aria-label"、"tabindex"）
    std::string value;  ///< 属性值（一律字符串形态，与 DOM setAttribute 对齐）
};

/// @brief 语义节点的 ARIA 镜像元素（DOM 应用载荷的最小完备单元）。
///
/// `role` 与 `attrs` 分开是为了让 JSON 面稳定：role 恒存在（可空串 = 不设），
/// 其余属性按固定次序进 `attrs`（表驱动单测逐位断言的前提）。
struct AriaElement {
    std::uint64_t id = 0;  ///< 稳定身份（`Widget::runtime_id()`）
    std::uint64_t parent_id = 0;  ///< 父节点 id（0 = 根，直接挂容器）
    std::string dom_id;  ///< DOM id（`"aurora-a11y-" + id`，IDREF 用）
    std::string role;  ///< WAI-ARIA role（空串 = 不写 role 属性）
    std::vector<AriaAttr> attrs;  ///< aria-*/tabindex/data-aurora-*（确定序）
    std::string content;  ///< 文本内容（Text/TextInput 的 value；其余为空）
    std::vector<std::uint64_t> child_ids;  ///< 子 id（先序，同快照 `children_of`）
    std::uint16_t click_action = 0;  ///< 镜像元素被点击时回灌的动作位（0 = 不监听）
};

/// @brief 语义角色 → WAI-ARIA role 名（空串 = 无对应，不写 role 属性）。
[[nodiscard]] auto aria_role_of(AccessibilityRole role) -> std::string_view;

/// @brief 动作位掩码 → `data-aurora-actions` 值（确定序：focus click invoke toggle
///        select value scroll-*；空格分隔，None 为空串）。观测与调试镜像用。
[[nodiscard]] auto aria_actions_text(AccessibilityAction actions) -> std::string;

/// @brief 镜像元素被点击时应回灌的**主**动作位（单按钮语义的确定性取法）：
///        Invoke > Toggle > Click > Select > Value > Focus > 0。
[[nodiscard]] auto aria_primary_click_action(const AccessibilityNode &n) -> std::uint16_t;

/// @brief 单个快照节点 → 镜像元素（role + 属性表 + 内容 + 主点击动作）。
[[nodiscard]] auto aria_element_of(const a11y::NodeSnapshot &n) -> AriaElement;

/// @brief 快照 → 先序镜像元素表（与 `TreeSnapshot::flat` 同序）。
[[nodiscard]] auto aria_tree_of(const a11y::TreeSnapshot &snap) -> std::vector<AriaElement>;

/// @brief RFC 8259 最小集 JSON 字符串转义（不含首尾引号；UTF-8 透传）。
[[nodiscard]] auto aria_json_escape(std::string_view s) -> std::string;

/// @brief 镜像元素 → JSON 对象（`{"id":…,"parent":…,"dom":"…","role":"…","attrs":
///        [["k","v"],…],"content":"…","children":[…],"click":N}`）。
[[nodiscard]] auto aria_element_json(const AriaElement &el) -> std::string;

/// @brief 全量载荷（首次应用 / 容器重建）：`{"els":[E…],"focus":N,"rtl":b}`。
/// @param focus 当前聚焦节点 id（0 = 无）
[[nodiscard]] auto aria_full_json(const std::vector<AriaElement> &tree, std::uint64_t focus, bool rtl) -> std::string;

/// @brief 增量载荷：`{"ops":[{"op":"remove","id":N}|{"op":"add","el":E}|
///        {"op":"update","el":E}|{"op":"move","id":N,"parent":M,"index":K},…],
///        "focus":N}`。
///
/// ops 次序固定：remove → add（按新快照先序）→ move → update（按新快照先序去重），
/// 让 JS 侧「先腾空、再挂新、后复位、终改面」的单趟应用无位置歧义。
/// `focus` 为**绝对值**（新快照中 `state.focused` 为真者 id，0 = 无）——焦点是
/// 镜像面最需要保持正确的属性，宁每帧重设也不依赖增量的相对语义。
// `new_` 与 `old_` 成对：`new` 是 C++ 关键字，无法照 lower_case 正名，故命名检查就地豁免
// （定义侧同法，见 aria_protocol.cpp）。
// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] auto aria_ops_json(const a11y::TreeSnapshot &old_, const a11y::TreeSnapshot &new_,
                                 const a11y::TreeDiff &diff) -> std::string;

/// @brief 快照中的聚焦节点 id（`state.focused` 为真者；无则 0）。
///
/// 焦点在载荷里按**绝对值**重发而非增量事件 —— 镜像面的焦点属性最需要「每帧自洽」，
/// 不依赖增量的相对语义（丢一帧 ops 也不会错位）。
[[nodiscard]] auto focus_id_of(const a11y::TreeSnapshot &snap) -> std::uint64_t;

/// @brief 播报载荷（G4 → aria-live）：`{"text":"…","target":N}`（target 0 = 无关联控件）。
[[nodiscard]] auto aria_announce_json(const std::string &text, std::uint64_t target) -> std::string;

}  // namespace aurora::detail
