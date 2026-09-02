// Form / FormField 表单验证 demo：姓名必填 + 邮箱格式校验，提交时统一验证。
#include "demo_common.h"

auto main() -> int {
    // 共享 TextInput 实例：value_provider 直接读控件当前文本（TextInput::value()）。
    auto name_input = std::make_shared<au::TextInput>();
    auto email_input = std::make_shared<au::TextInput>();

    au::FormField name_field{ au::Node{ std::shared_ptr<au::Widget>(name_input) },
                              [name_input]() -> std::string { return name_input->value(); },
                              au::validators::required("Name cannot be empty") };
    au::FormField email_field{ au::Node{ std::shared_ptr<au::Widget>(email_input) },
                               [email_input]() -> std::string { return email_input->value(); },
                               au::validators::combine({ au::validators::required("Email cannot be empty"),
                                                         au::validators::email("Email format invalid") }) };

    std::vector<au::Node> fields;
    fields.emplace_back(std::move(name_field));
    fields.emplace_back(std::move(email_field));
    auto form = std::make_shared<au::Form>(std::move(fields), []() -> void { AURORA_LOG_INFO("demo", "Submit success!"); });

    au::Button submit{ au::ButtonProps{ .label = "Submit" } };
    submit.on_click = [form]() -> void { AURORA_LOG_INFO("demo", form->submit() ? "Pass" : "Validation failed"); };

    au::Node root = au::Column{
        GradientTitle{ "Form validation" },
        gap(12),
        au::Node{ std::shared_ptr<au::Widget>(form) },
        gap(12),
        std::move(submit),
    };
    return run_demo(Card{ std::move(root) }, "Form · Aurora Demo", 480.0f, 400.0f);
}
