/// 测试类型: unit
/// 目标单元: include/aurora/app/validate.h
/// 测试说明: 覆盖渲染前树校验的纯逻辑分支——合法树放行、未知控件类型、null 子节点、
/// 深度超限（含建议文案）、多问题树返回首个错误、校验仅查结构不查属性值

#include <string>

#include "aurora/app/validate.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_validate {

namespace {

/// 未注册进 WidgetRegistry 的自定义控件（类型名拼写错误/未注册场景的替身）。
struct UnregisteredWidget final : Widget {
    [[nodiscard]] auto type_name() const -> const char* override { return "AuroraTestUnregisteredWidget"; }
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override { return c.max; }
    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}
};

}  // namespace

AURORA_TEST_CASE(valid_tree_passes) {
    auto col = std::make_shared<Column>();
    col->add(Node{std::make_shared<Text>("hi")});
    col->add(Node{std::make_shared<Button>()});
    col->add(Node{std::make_shared<Column>()});

    const auto r = validate(Node{col});
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_TRUE(r.value());
}

AURORA_TEST_CASE(unknown_widget_type_reported) {
    const auto r = validate(Node{std::make_shared<UnregisteredWidget>()});
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::ValidationUnknownWidget);
    AURORA_TEST_CHECK_TRUE(r.error().message.find("AuroraTestUnregisteredWidget") != std::string::npos);
    // 建议文案指向注册入口。
    AURORA_TEST_CHECK_TRUE(r.error().suggestion.find("register_factory") != std::string::npos);
}

AURORA_TEST_CASE(null_child_reported) {
    auto col = std::make_shared<Column>();
    col->add(Node{});  // 结构不完整的 nullptr 子节点

    const auto r = validate(Node{col});
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::ValidationNullChild);
    AURORA_TEST_CHECK_TRUE(r.error().message.find("null child") != std::string::npos);
}

AURORA_TEST_CASE(depth_limit_reported) {
    auto leaf = std::make_shared<Text>("x");
    auto inner = std::make_shared<Column>();
    inner->add(Node{leaf});
    auto outer = std::make_shared<Column>();
    outer->add(Node{inner});

    // max_depth=0：根(0)放行、子(1)超限。
    const auto too_deep = validate(Node{outer}, 0);
    AURORA_TEST_REQUIRE_FALSE(too_deep.ok());
    AURORA_TEST_CHECK_EQ(too_deep.error().code_enum, aurora::ErrorCode::ValidationTreeTooDeep);
    AURORA_TEST_CHECK_TRUE(too_deep.error().message.find("depth 1") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(too_deep.error().suggestion.find("max_depth") != std::string::npos);

    // 实现语义（validate.h）：depth > max_depth 即报错，根为 depth 0——
    // max_depth 是「允许的最大深度」，depth == max_depth 边界放行。本树最深叶子位于 depth 2。
    // max_depth=1：inner(1) 放行、leaf(2) 超限——仍不放行。
    const auto still_deep = validate(Node{outer}, 1);
    AURORA_TEST_REQUIRE_FALSE(still_deep.ok());
    AURORA_TEST_CHECK_EQ(still_deep.error().code_enum, aurora::ErrorCode::ValidationTreeTooDeep);
    AURORA_TEST_CHECK_TRUE(still_deep.error().message.find("depth 2") != std::string::npos);

    // 放宽到 2 层：最深节点 depth==max_depth，同一棵树边界放行。
    const auto ok = validate(Node{outer}, 2);
    AURORA_TEST_REQUIRE_TRUE(ok.ok());
    AURORA_TEST_CHECK_TRUE(ok.value());
}

AURORA_TEST_CASE(first_error_wins) {
    auto col = std::make_shared<Column>();
    col->add(Node{});  // 先命中：null child
    col->add(Node{std::make_shared<UnregisteredWidget>()});  // 后命中：unknown type

    const auto r = validate(Node{col});
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    // 返回首个问题（DFS 前序），而非错误聚合。
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::ValidationNullChild);
}

AURORA_TEST_CASE(validate_is_structural_not_prop_level) {
    // 属性值不参与校验（Text 的 text 为空也不会命中错误）：validate 只保证树结构可渲染。
    const auto r = validate(Node{std::make_shared<Text>("")});
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_TRUE(r.value());
}

}  // namespace aurora::test_cases::utest_validate
