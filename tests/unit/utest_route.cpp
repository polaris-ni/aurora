/// 测试类型: unit
/// 目标单元: include/aurora/navigation/route.h
/// 测试说明: 覆盖 Route 的空路由判定、根节点/名称/转场配置存取、拷贝共享根树、
/// 移动转移所有权与根节点可变访问

#include <string>
#include <utility>

#include "aurora/navigation/route.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_route {

namespace {

/// 纯色填充探针叶控件：布局取满约束（测试仅需一个真实 widget 实例作根）。
class SolidBox final : public LeafWidget {
  public:
    SolidBox() = default;

    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(c.max);
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

}  // namespace

AURORA_TEST_CASE(route_default_is_empty) {
    const Route r;
    AURORA_TEST_CHECK_TRUE(r.empty());
    AURORA_TEST_CHECK_TRUE(r.name().empty());
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(r.root()));
}

AURORA_TEST_CASE(route_holds_root_and_name) {
    auto page = std::make_shared<SolidBox>();
    const Route r{Node{page}, "home"};

    AURORA_TEST_CHECK_FALSE(r.empty());
    AURORA_TEST_CHECK_EQ(r.name(), std::string{"home"});
    AURORA_TEST_CHECK_EQ(&r.root().widget(), page.get());
    AURORA_TEST_CHECK_STREQ(r.root().widget().type_name(), "SolidBox");
}

AURORA_TEST_CASE(route_transition_defaults) {
    const RouteTransition tr;
    AURORA_TEST_CHECK_FALSE(tr.animated);
    AURORA_TEST_CHECK_TRUE(tr.kind == TransitionKind::Fade);
    AURORA_TEST_CHECK_NEAR(tr.duration_seconds, 0.3, 1e-4F);
}

AURORA_TEST_CASE(route_custom_transition_stored) {
    const RouteTransition tr{.animated = true,
                             .kind = TransitionKind::Slide,
                             .curve = Curves::linear(),
                             .duration_seconds = 0.5};
    const Route r{Node{SolidBox{}}, "detail", tr};

    AURORA_TEST_CHECK_TRUE(r.transition().animated);
    AURORA_TEST_CHECK_TRUE(r.transition().kind == TransitionKind::Slide);
    AURORA_TEST_CHECK_NEAR(r.transition().duration_seconds, 0.5, 1e-4F);
}

AURORA_TEST_CASE(route_copy_shares_root_widget) {
    const Route src{Node{SolidBox{}}, "src"};
    const Route copy{src};  // Node 内部为 shared_ptr：拷贝共享同一棵 widget 树。

    AURORA_TEST_CHECK_FALSE(copy.empty());
    AURORA_TEST_CHECK_EQ(&copy.root().widget(), &src.root().widget());
    AURORA_TEST_CHECK_EQ(copy.name(), src.name());
}

AURORA_TEST_CASE(route_move_transfers_root) {
    Route src{Node{SolidBox{}}, "src"};
    const Route dst{std::move(src)};

    // 移动后源路由的 shared_ptr 已被转移：源变空、目标持有根。
    AURORA_TEST_CHECK_TRUE(src.empty());
    AURORA_TEST_CHECK_FALSE(dst.empty());
    AURORA_TEST_CHECK_EQ(dst.name(), std::string{"src"});
}

AURORA_TEST_CASE(route_root_mutable_access) {
    Route r{Node{SolidBox{}}, "home"};
    r.root().set_id("page-root");

    const Route &view = r;
    AURORA_TEST_CHECK_EQ(view.root().id(), std::string_view{"page-root"});
}

}  // namespace aurora::test_cases::utest_route
