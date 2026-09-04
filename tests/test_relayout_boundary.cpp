// RelayoutBoundary 验证：滚动一个 relayout boundary（LazyList）时，其非 boundary
// 祖先控件不应被重排（布局脏在 boundary 处截断，由 present_root 局部重排 boundary 子树）。
//
// 核心断言：祖先 CountingWidget 的 on_layout 调用次数在「首帧整树重排」后恒为 1——
// 后续滚动 LazyList 只触发 boundary 局部重排，祖先不重排。若 RelayoutBoundary 未生效
// （mark_needs_layout 仍冒泡到根），滚动会置根脏 → 祖先 on_layout 再次调用 → 计数变 2 → 失败。
//
// 同时开启 StrictMode：合法 boundary 滚动后尺寸不应变化（否则 assert 失败），
// 间接验证 boundary 声明正确。
#include <functional>
#include <memory>

#include "aurora/aurora.h"
#include "aurora/core/strict_mode.h"
#include "aurora/widget/lazy_list.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::HeadlessSurface;
using aurora::LazyList;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::Size;
using aurora::strict_mode;
using aurora::StrictMode;
using aurora::Text;
using aurora::Widget;
using aurora::Window;

namespace {

// 计数 on_layout 调用次数的祖先控件：撑满父约束（自身即 boundary），持有单个子节点。
class CountingWidget : public Widget {
  public:
    int layout_calls_ = 0;
    Node child_;

    explicit CountingWidget(Node c) : child_(std::move(c)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "CountingWidget"; }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        ++layout_calls_;
        Constraints cc = c;
        cc.min = c.max;  // 撑满父约束
        cc.max = c.max;
        if (child_) {
            child_.widget().set_layout_parent(this);
            child_.widget().layout(cc, ctx);
        }
        return c.max;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (child_) {
            fn(child_.widget());
        }
    }
};

auto make_window(int w, const int h) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return Window{std::move(surface)};
}

auto scenario_boundary_truncates_layout_bubble() -> void {
    // 严格模式：合法 boundary 滚动后尺寸不变，不应触发断言。
    const StrictMode prev = strict_mode();
    set_strict_mode(StrictMode::On);

    LazyList::ItemBuilder builder = [](int i) -> Node {
        return Node{std::make_shared<Text>(std::string("item ") + std::to_string(i))};
    };
    const auto lazy = std::make_shared<LazyList>(1000, builder, 48.0F);
    const auto root_w = std::make_shared<CountingWidget>(Node{lazy});
    Node root{root_w};

    Window win = make_window(400, 600);
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 首帧：整树重排（含 Root + LazyList）
    const int calls_after_first = root_w->layout_calls_;
    AURORA_TEST_CHECK(calls_after_first == 1);  // 首帧 Root 恰好重排一次

    // 滚动 LazyList（boundary 内部标脏，应截断冒泡，不置根脏）
    lazy->set_scroll_offset(200.0F);
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 第二次：boundary 局部重排，Root 不重排
    const int calls_after_scroll = root_w->layout_calls_;
    AURORA_TEST_CHECK(calls_after_scroll == 1);  // 滚动后 Root 未重排 → boundary 截断生效

    // 滚动确实生效（boundary 自身重排了，内容偏移变化）
    AURORA_TEST_CHECK(std::abs(lazy->scroll_offset() - 200.0F) < 1e-3F);
    // 虚拟化仍生效：存活实例远小于总数
    AURORA_TEST_CHECK(lazy->live_item_count() < 1000U);

    set_strict_mode(prev);
}

}  // namespace

AURORA_TEST() { scenario_boundary_truncates_layout_bubble(); }
