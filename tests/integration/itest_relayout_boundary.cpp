/// 测试类型: integration
/// 目标单元: include/aurora/widget/lazy_list.h + include/aurora/core/strict_mode.h
/// 测试说明: RelayoutBoundary 集成——滚动一个 relayout boundary（LazyList）时，
/// mark_needs_layout 在 boundary 处截断、不冒泡置根脏，祖先 CountingWidget 的
/// on_layout 在首帧整树重排后不再被调用；同时开启 StrictMode 间接校验 boundary
/// 声明正确（滚动后 boundary 尺寸不变，否则窗口侧影子断言失败）

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "aurora/core/strict_mode.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_relayout_boundary {

#ifdef AURORA_BACKEND_HEADLESS

namespace {

/// 计数 on_layout 调用次数的祖先控件：撑满父约束（自身非 boundary），持有单个子节点。
class CountingWidget final : public Widget {
  public:
    int layout_calls = 0;
    Node child;

    explicit CountingWidget(Node c) : child(std::move(c)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "CountingWidget"; }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (child) {
            fn(child.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        ++layout_calls;
        Constraints cc = c;
        cc.min = c.max;  // 撑满父约束
        if (child) {
            child.widget().set_layout_parent(this);
            child.widget().layout(cc, ctx);
        }
        return c.max;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child) {
            child.widget().paint(p, bounds, ctx);
        }
    }
};

}  // namespace

AURORA_TEST_CASE(scrolling_boundary_does_not_relayout_ancestors) {
    // 严格模式：合法 boundary 滚动后尺寸不变，不应触发窗口侧影子断言。
    const StrictMode prev = strict_mode();
    set_strict_mode(StrictMode::On);

    const LazyList::ItemBuilder builder = [](int i) -> Node {
        return Node{std::make_shared<Text>("item " + std::to_string(i))};
    };
    const auto lazy = std::make_shared<LazyList>(1000, builder, 48.0F);
    const auto root_w = std::make_shared<CountingWidget>(Node{lazy});
    Node root{root_w};

    // 传入初始尺寸，避免 present_root 首帧读到 0 尺寸把整树布局到 0x0。
    auto surface = std::make_unique<HeadlessSurface>("", Size{.width = 400.0F, .height = 600.0F});
    Window win{std::move(surface)};

    const auto first = win.present_root(root);
    AURORA_TEST_CHECK_TRUE(first.ok());  // 首帧：整树重排（含 Root + LazyList）
    const int calls_after_first = root_w->layout_calls;
    AURORA_TEST_CHECK_EQ(calls_after_first, 1);

    // 滚动 LazyList（boundary 内部标脏，应截断冒泡，不置根脏）。
    lazy->set_scroll_offset(200.0F);
    const auto second = win.present_root(root);
    AURORA_TEST_CHECK_TRUE(second.ok());  // 第二帧：boundary 局部重排，Root 不重排
    AURORA_TEST_CHECK_EQ(root_w->layout_calls, 1);

    // 滚动确实生效（boundary 自身重排了，内容偏移变化）。
    AURORA_TEST_CHECK_NEAR(lazy->scroll_offset(), 200.0F, 1e-3F);
    // 虚拟化仍生效：存活实例远小于总数。
    AURORA_TEST_CHECK_TRUE(lazy->live_item_count() < 1000U);

    set_strict_mode(prev);
}

#else

AURORA_TEST_CASE(scrolling_boundary_does_not_relayout_ancestors) {
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：HeadlessSurface/Window 帧驱动未编译");
}

#endif

}  // namespace aurora::test_cases::itest_relayout_boundary
