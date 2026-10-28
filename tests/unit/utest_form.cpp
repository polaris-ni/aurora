/// 测试类型: unit
/// 目标单元: include/aurora/widget/form.h
/// 测试说明: 覆盖 validators 命名空间全部工厂（required/min_length/max_length/email/matches/range/combine）
/// 与 FormField/Form 的验证、错误态、序列化、布局、绘制与提交链路。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/form.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_form {
using aurora::testing::require_field;
using aurora::validators::combine;
using aurora::validators::email;
using aurora::validators::matches;
using aurora::validators::max_length;
using aurora::validators::min_length;
using aurora::validators::range;
using aurora::validators::required;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto field_with(Validator v, std::function<std::string()> provider) -> std::shared_ptr<FormField> {
    return std::make_shared<FormField>(Node{std::make_shared<Text>("input")}, std::move(provider), std::move(v));
}

}  // namespace

// ---------- validators 命名空间 ----------

AURORA_TEST_CASE(required_passes_non_empty_and_fails_empty) {
    auto v = required();
    AURORA_TEST_CHECK_EQ(v(""), std::string{"This field is required"});
    AURORA_TEST_CHECK_EQ(v("x"), std::string{});
}

AURORA_TEST_CASE(required_uses_custom_message) {
    auto v = required("must not be blank");
    AURORA_TEST_CHECK_EQ(v(""), std::string{"must not be blank"});
    AURORA_TEST_CHECK_EQ(v("ok"), std::string{});
}

AURORA_TEST_CASE(min_length_bounds) {
    auto v = min_length(3);
    AURORA_TEST_CHECK_EQ(v("ab"), std::string{"Must be at least 3 characters"});
    AURORA_TEST_CHECK_EQ(v("abc"), std::string{});
    AURORA_TEST_CHECK_EQ(v("abcd"), std::string{});
    auto custom = min_length(2, "too short");
    AURORA_TEST_CHECK_EQ(custom("z"), std::string{"too short"});
}

AURORA_TEST_CASE(max_length_bounds) {
    auto v = max_length(5);
    AURORA_TEST_CHECK_EQ(v("abcdef"), std::string{"Must be at most 5 characters"});
    AURORA_TEST_CHECK_EQ(v("abcde"), std::string{});
    auto custom = max_length(3, "too long");
    AURORA_TEST_CHECK_EQ(custom("abcd"), std::string{"too long"});
}

AURORA_TEST_CASE(email_accepts_and_rejects) {
    auto v = email();
    AURORA_TEST_CHECK_EQ(v(""), std::string{});  // 空串交给 required
    AURORA_TEST_CHECK_EQ(v("a@b.com"), std::string{});
    AURORA_TEST_CHECK_EQ(v("no-at-sign"), std::string{"Invalid email address"});
    auto custom = email("bad email");
    AURORA_TEST_CHECK_EQ(custom("x"), std::string{"bad email"});
}

AURORA_TEST_CASE(matches_regex_pattern) {
    auto v = matches(R"(\d{3}-\d{4})", "bad phone");
    AURORA_TEST_CHECK_EQ(v(""), std::string{});  // 空串交给 required
    AURORA_TEST_CHECK_EQ(v("123-4567"), std::string{});
    AURORA_TEST_CHECK_EQ(v("12-345"), std::string{"bad phone"});
}

AURORA_TEST_CASE(range_numeric_bounds) {
    auto v = range(0.0, 10.0);
    AURORA_TEST_CHECK_EQ(v("5"), std::string{});
    AURORA_TEST_CHECK_EQ(v("15"), std::string{"Must be between 0.000000 and 10.000000"});
    AURORA_TEST_CHECK_EQ(v("not-a-number"), std::string{"Must be between 0.000000 and 10.000000"});
    auto custom = range(1.0, 5.0, "out of range");
    AURORA_TEST_CHECK_EQ(custom("8"), std::string{"out of range"});
}

AURORA_TEST_CASE(combine_runs_in_order_and_skips_null) {
    auto v = combine({required(), min_length(3)});
    AURORA_TEST_CHECK_EQ(v(""), std::string{"This field is required"});  // 首个失败即返回
    AURORA_TEST_CHECK_EQ(v("ab"), std::string{"Must be at least 3 characters"});
    AURORA_TEST_CHECK_EQ(v("abc"), std::string{});

    // 含空条目：被跳过，不报错也不崩溃。
    Validator none;
    auto with_null = combine({none, required()});
    AURORA_TEST_CHECK_EQ(with_null("x"), std::string{});
    AURORA_TEST_CHECK_EQ(with_null(""), std::string{"This field is required"});
}

// ---------- FormField ----------

AURORA_TEST_CASE(formfield_validate_pass_and_fail) {
    auto pass = field_with(required(), []() -> std::string { return "hi"; });
    AURORA_TEST_CHECK_TRUE(pass->validate());
    AURORA_TEST_CHECK_FALSE(pass->has_error());
    AURORA_TEST_CHECK_EQ(pass->error_text(), std::string{});

    auto fail = field_with(required(), []() -> std::string { return ""; });
    AURORA_TEST_CHECK_FALSE(fail->validate());
    AURORA_TEST_CHECK_TRUE(fail->has_error());
    AURORA_TEST_CHECK_EQ(fail->error_text(), std::string{"This field is required"});
}

AURORA_TEST_CASE(formfield_clear_error_resets_state) {
    auto f = field_with(required(), []() -> std::string { return ""; });
    AURORA_TEST_CHECK_FALSE(f->validate());  // 进入错误态
    AURORA_TEST_CHECK_TRUE(f->has_error());
    f->clear_error();
    AURORA_TEST_CHECK_FALSE(f->has_error());
    AURORA_TEST_CHECK_EQ(f->error_text(), std::string{});
}

AURORA_TEST_CASE(formfield_set_validator_and_provider_take_effect) {
    auto f =
        std::make_shared<FormField>(Node{std::make_shared<Text>("x")}, []() -> std::string { return ""; }, Validator{});
    AURORA_TEST_CHECK_TRUE(f->validate());  // 无验证器：恒通过
    f->set_validator(required());
    f->set_value_provider([]() -> std::string { return ""; });
    AURORA_TEST_CHECK_FALSE(f->validate());
}

AURORA_TEST_CASE(formfield_serialize_writes_error_text) {
    auto f = field_with(required(), []() -> std::string { return ""; });
    f->validate();
    Json props = Json::object();
    f->serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "error_text"), std::string{"This field is required"});

    StateSink sigs;
    f->collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.sources().size(), 1U);  // 仅内部 error_ 信号
}

AURORA_TEST_CASE(formfield_layout_grows_with_error_and_paints) {
    auto f = field_with(required(), []() -> std::string { return "ok"; });
    LayoutEngine::layout(*f, bounded(320.0F, 400.0F));
    const float ok_h = f->size().height;

    f->set_value_provider([]() -> std::string { return ""; });
    f->validate();  // 进入错误态
    LayoutEngine::layout(*f, bounded(320.0F, 400.0F));
    AURORA_TEST_CHECK_GT(f->size().height, ok_h);  // 错误文本额外高度

    Painter painter;
    Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = f->size()};
    f->paint(painter, bounds, BuildContext{});  // 子控件 + 红框 + 错误文本

    // 清除错误后再绘制：仅子控件分支。
    f->clear_error();
    LayoutEngine::layout(*f, bounded(320.0F, 400.0F));
    f->paint(painter, bounds, BuildContext{});
}

AURORA_TEST_CASE(formfield_describe_metadata) {
    const auto d = FormField::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "FormField");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
    bool has_err = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "error_text") {
            has_err = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_err);
}

// ---------- Form ----------

AURORA_TEST_CASE(form_validate_all_and_submit_pass) {
    int submitted = 0;
    auto a = field_with(required(), []() -> std::string { return "a"; });
    auto b = field_with(min_length(2), []() -> std::string { return "bc"; });
    Form form({Node{a}, Node{b}}, [&submitted]() -> void { ++submitted; });

    AURORA_TEST_CHECK_TRUE(form.validate_all());
    AURORA_TEST_CHECK_TRUE(form.submit());
    AURORA_TEST_CHECK_EQ(submitted, 1);
}

AURORA_TEST_CASE(form_submit_blocked_when_field_fails) {
    int submitted = 0;
    auto a = field_with(required(), []() -> std::string { return ""; });  // 失败
    auto b = field_with(min_length(2), []() -> std::string { return "bc"; });
    Form form({Node{a}, Node{b}}, [&submitted]() -> void { ++submitted; });

    AURORA_TEST_CHECK_FALSE(form.validate_all());
    AURORA_TEST_CHECK_FALSE(form.submit());
    AURORA_TEST_CHECK_EQ(submitted, 0);
    AURORA_TEST_CHECK_TRUE(a->has_error());  // 失败消息已写入字段
}

AURORA_TEST_CASE(form_clear_errors_resets_fields) {
    auto a = field_with(required(), []() -> std::string { return ""; });
    Form form({Node{a}}, {});
    AURORA_TEST_CHECK_FALSE(form.validate_all());
    AURORA_TEST_CHECK_TRUE(a->has_error());
    form.clear_errors();
    AURORA_TEST_CHECK_FALSE(a->has_error());
}

AURORA_TEST_CASE(form_set_gap_and_layout_stacks) {
    auto a = field_with(required(), []() -> std::string { return "a"; });
    auto b = field_with(required(), []() -> std::string { return "b"; });
    Form form({Node{a}, Node{b}}, {});
    form.set_gap(4.0F);

    LayoutEngine::layout(form, bounded(320.0F, 600.0F));
    AURORA_TEST_CHECK_GT(form.size().width, 0.0F);
    AURORA_TEST_CHECK_GT(form.size().height, 0.0F);
    AURORA_TEST_CHECK_LE(form.size().width, 320.0F);

    StateSink sigs;
    form.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.sources().size(), 0U);  // Form 自身无信号
}

AURORA_TEST_CASE(form_describe_metadata) {
    const auto d = Form::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Form");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
    bool has_gap = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "gap") {
            has_gap = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_gap);
}

}  // namespace aurora::test_cases::utest_form
