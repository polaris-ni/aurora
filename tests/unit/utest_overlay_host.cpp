/// 测试类型: unit
/// 目标单元: include/aurora/widget/popup.h（OverlayHost）
/// 测试说明: 覆盖浮层登记的序号契约——`add_overlay` 与 `remove_overlay` 对「0 = 基础内容、
/// 不可移除」必须口径一致。修复前 `add_overlay` 在宿主尚无基础内容时返回 0，而 `remove_overlay`
/// 拒收 0 ⇒ 调用方拿到一个永远删不掉的序号（浮层泄漏）。修复后该情形返回 `std::nullopt`，
/// 由类型本身表达「本次追加未产生可移除浮层」。
///
/// 判据：有基础内容时序号自 1 起且可移除；空宿主时返回 nullopt、节点虽已入树但不可移除。

#include <cstddef>
#include <memory>
#include <optional>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/popup.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_overlay_host {

using aurora::testing::require_value;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 一棵基础内容树（Column 内一个按钮）。
/// @return 基础内容节点。
auto base_content() -> Node { return Node{Button(ButtonProps{.label = "Base"})}; }

}  // namespace

AURORA_TEST_CASE(overlay_index_starts_at_one_and_is_removable) {
    // 有基础内容：序号自 1 起，`remove_overlay` 收得下（与返回的序号同一口径）。
    auto host = std::make_shared<OverlayHost>(base_content());
    // 「检查 + 取值」走框架的 `require_value`：宏展开对 clang-tidy 的路径分析不透明，
    // 直接 `*first` 会被误报 unchecked-optional-access。
    const std::size_t first = require_value(host->add_overlay(Node{Button(ButtonProps{.label = "Overlay"})}));
    AURORA_TEST_CHECK_EQ(first, 1U);
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 1U);

    host->remove_overlay(first);
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 0U);
}

AURORA_TEST_CASE(add_overlay_on_empty_host_is_not_removable) {
    // 空宿主：新节点落在序号 0（基础内容槽位）⇒ 返回 nullopt；且该节点确实不可移除，
    // 与 `remove_overlay` 的口径一致（修复前返回 0，调用方据此去删会被静默忽略）。
    auto host = std::make_shared<OverlayHost>();
    LayoutEngine::layout(*host, bounded(240.0F, 200.0F));
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 0U);

    const std::optional<std::size_t> index = host->add_overlay(Node{Button(ButtonProps{.label = "Orphan"})});
    AURORA_TEST_CHECK_FALSE(index.has_value());

    // 节点已入树（追加本身生效），但不计入浮层、也删不掉——这正是返回值要如实表达的情形。
    host->remove_overlay(0);
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 0U);
    AURORA_TEST_CHECK_EQ(host->child_count(), 1U);
}

}  // namespace aurora::test_cases::utest_overlay_host
