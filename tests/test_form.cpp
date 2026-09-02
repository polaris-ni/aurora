// 验证表单验证框架：validators 组合、FormField 验证/错误态、Form 聚合提交。

#include <memory>

#include "aurora/widget/form.h"
#include "aurora/widget/text_input.h"

#include "test_harness.h"

using aurora::Form;
using aurora::FormField;
using aurora::Node;
using aurora::TextInput;
using aurora::Validator;
namespace validators = aurora::validators;

AURORA_TEST() {
    // ---- 1. required 验证器 ----
    {
        auto v = validators::required();
        AURORA_TEST_CHECK(!v("").empty());     // 空值报错
        AURORA_TEST_CHECK(v("hello").empty()); // 非空通过
    }

    // ---- 2. min_length / max_length ----
    {
        auto lo = validators::min_length(3);
        AURORA_TEST_CHECK(!lo("ab").empty());
        AURORA_TEST_CHECK(lo("abc").empty());

        auto hi = validators::max_length(5);
        AURORA_TEST_CHECK(hi("abcde").empty());
        AURORA_TEST_CHECK(!hi("abcdef").empty());
    }

    // ---- 3. email ----
    {
        auto v = validators::email();
        AURORA_TEST_CHECK(v("user@example.com").empty());
        AURORA_TEST_CHECK(!v("not-an-email").empty());
        AURORA_TEST_CHECK(!v("missing@domain").empty());
        AURORA_TEST_CHECK(v("").empty()); // 空值不报错（交给 required）
    }

    // ---- 4. matches 正则 ----
    {
        auto v = validators::matches(R"(^\d{4}$)", "need 4 digits");
        AURORA_TEST_CHECK(v("1234").empty());
        AURORA_TEST_CHECK(v("12a4") == "need 4 digits");
    }

    // ---- 5. range ----
    {
        auto v = validators::range(1.0, 100.0);
        AURORA_TEST_CHECK(v("50").empty());
        AURORA_TEST_CHECK(!v("0").empty());
        AURORA_TEST_CHECK(!v("101").empty());
        AURORA_TEST_CHECK(!v("abc").empty()); // 不可解析报错
    }

    // ---- 6. combine 组合（首个失败短路）----
    {
        auto v = validators::combine({ validators::required("REQ"), validators::min_length(3, "SHORT") });
        AURORA_TEST_CHECK(v("") == "REQ");
        AURORA_TEST_CHECK(v("ab") == "SHORT");
        AURORA_TEST_CHECK(v("abc").empty());
    }

    // ---- 7. FormField 验证与错误态 ----
    {
        std::string current_value;
        auto field = std::make_shared<FormField>(
            Node(TextInput()), [&current_value]() -> std::string { return current_value; },
            validators::required("cannot be empty"));

        // 空值验证失败
        AURORA_TEST_CHECK(!field->validate());
        AURORA_TEST_CHECK(field->has_error());
        AURORA_TEST_CHECK(field->error_text() == "cannot be empty");

        // 填值后通过
        current_value = "filled";
        AURORA_TEST_CHECK(field->validate());
        AURORA_TEST_CHECK(!field->has_error());

        // clear_error
        current_value = "";
        field->validate();
        AURORA_TEST_CHECK(field->has_error());
        field->clear_error();
        AURORA_TEST_CHECK(!field->has_error());
    }

    // ---- 8. FormField 无验证器时恒通过 ----
    {
        auto field = std::make_shared<FormField>();
        AURORA_TEST_CHECK(field->validate());
        AURORA_TEST_CHECK(!field->has_error());
    }

    // ---- 9. Form 聚合验证与提交 ----
    {
        std::string name_value;
        std::string email_value;

        auto name_field = FormField(
            Node(TextInput()), [&name_value]() -> std::string { return name_value; },
            validators::required("name required"));

        auto email_field = FormField(
            Node(TextInput()), [&email_value]() -> std::string { return email_value; },
            validators::combine({ validators::required("email required"), validators::email() }));

        int submitted = 0;
        std::vector<Node> fields;
        fields.emplace_back(std::move(name_field));
        fields.emplace_back(std::move(email_field));
        auto form = std::make_shared<Form>(std::move(fields), [&submitted]() -> void { ++submitted; });

        // 全空：验证失败，不提交
        AURORA_TEST_CHECK(!form->submit());
        AURORA_TEST_CHECK(submitted == 0);

        // 只填 name：仍失败
        name_value = "Alice";
        AURORA_TEST_CHECK(!form->submit());
        AURORA_TEST_CHECK(submitted == 0);

        // email 格式错误：仍失败
        email_value = "bad-email";
        AURORA_TEST_CHECK(!form->submit());
        AURORA_TEST_CHECK(submitted == 0);

        // 全部合法：提交成功
        email_value = "alice@example.com";
        AURORA_TEST_CHECK(form->submit());
        AURORA_TEST_CHECK(submitted == 1);

        // clear_errors 后再全空验证
        name_value = "";
        email_value = "";
        form->validate_all();
        form->clear_errors();
        // 清除后字段无错误态（validate_all 之后又清除了）
        AURORA_TEST_CHECK(form->submit() == false); // 提交时重新验证仍失败
    }

    // ---- 10. Form validate_all 返回值 ----
    {
        std::string v1 = "ok";
        auto f1 = FormField(Node(TextInput()), [&v1]() -> std::string { return v1; }, validators::required());

        std::vector<Node> fields;
        fields.emplace_back(std::move(f1));
        auto form = std::make_shared<Form>(std::move(fields));

        AURORA_TEST_CHECK(form->validate_all());
        v1 = "";
        AURORA_TEST_CHECK(!form->validate_all());
    }
}
