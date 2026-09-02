// test_validate.cpp — app::validate 渲染前校验 1:1 测试：合法树 / 未知类型 / 空子节点 / 深度超限。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Column;
using aurora::Constraints;
using aurora::ErrorCode;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Spacer;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {
// 确定性、但未注册到 WidgetRegistry 的控件，用于触发「未知类型」分支。
class UnregisteredBox : public Widget {
  public:
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "UnregisteredBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "UnregisteredBox", .children_policy = "none" };
    }

  protected:
    void on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}

    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{ .width = 10.0f, .height = 10.0f };
    }
};
} // namespace

static void test_validate_valid() {
    // Column / Spacer 已在核心注册表，合法树应通过。
    auto ok = validate(Node{ Column{ Spacer{}, Spacer{} } });
    AURORA_TEST_CHECK_MSG(ok.ok() && ok.value() == true, "validate: valid tree -> ok(true)");
}

static void test_validate_unknown_widget() {
    const auto res = validate(Node{ UnregisteredBox{} });
    AURORA_TEST_CHECK_MSG(!res.ok(), "validate: unregistered type -> not ok");
    // 成功态上调用 Result::error() 会抛 std::bad_variant_access，故先短路。
    if (res.ok()) {
        return;
    }
    AURORA_TEST_CHECK_MSG(res.error().code_enum == ErrorCode::ValidationUnknownWidget,
                          "validate: error code UnknownWidget");
}

static void test_validate_null_child() {
    // Column 两个子项均为空 Node（nullptr），应报 NullChild。
    const auto res = validate(Node{ Column{ Node{}, Node{} } });
    AURORA_TEST_CHECK_MSG(!res.ok(), "validate: null child -> not ok");
    if (res.ok()) {
        return;
    }
    AURORA_TEST_CHECK_MSG(res.error().code.find("validation-null-child") != std::string::npos,
                          "validate: error code NullChild");
}

static void test_validate_depth_limit() {
    // 3 层 Column 嵌套 + Spacer，max_depth=2 应触发 TreeTooDeep。
    const auto res = validate(Node{ Column{ Column{ Column{ Spacer{} } } } }, 2);
    AURORA_TEST_CHECK_MSG(!res.ok(), "validate: too deep -> not ok");
    if (res.ok()) {
        return;
    }
    AURORA_TEST_CHECK_MSG(res.error().code.find("validation-tree-too-deep") != std::string::npos,
                          "validate: error code TreeTooDeep");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_validate ===\n");
    test_validate_valid();
    test_validate_unknown_widget();
    test_validate_null_child();
    test_validate_depth_limit();
}
