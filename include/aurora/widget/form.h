#pragma once

#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 字段验证器：输入字符串 -> 错误消息（空 = 通过）。
 *
 * 可组合：`combine({required(), min_length(3)})` 依次执行，返回首个失败消息。
 * 对标 Flutter `FormFieldValidator`、Qt `QValidator`、WPF `ValidationRule`。
 */
using Validator = std::function<std::string(const std::string &)>;

namespace validators {

/// @brief 必填：空字符串报错。
[[nodiscard]] inline auto required(std::string message = "This field is required") -> Validator {
    return [msg = std::move(message)](const std::string &v) -> std::string { return v.empty() ? msg : std::string{}; };
}

/// @brief 最小长度。
[[nodiscard]] inline auto min_length(std::size_t n, std::string message = {}) -> Validator {
    if (message.empty()) {
        message = "Must be at least " + std::to_string(n) + " characters";
    }
    return [n, msg = std::move(message)](const std::string &v) -> std::string {
        return v.size() < n ? msg : std::string{};
    };
}

/// @brief 最大长度。
[[nodiscard]] inline auto max_length(std::size_t n, std::string message = {}) -> Validator {
    if (message.empty()) {
        message = "Must be at most " + std::to_string(n) + " characters";
    }
    return [n, msg = std::move(message)](const std::string &v) -> std::string {
        return v.size() > n ? msg : std::string{};
    };
}

/// @brief 邮箱格式（宽松正则）。
[[nodiscard]] inline auto email(std::string message = "Invalid email address") -> Validator {
    return [msg = std::move(message)](const std::string &v) -> std::string {
        if (v.empty()) {
            return {};  // 空值交给 required 检查
        }
        static const std::regex PATTERN{R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)"};
        return std::regex_match(v, PATTERN) ? std::string{} : msg;
    };
}

/// @brief 自定义正则。
[[nodiscard]] inline auto matches(const std::string &pattern_str, std::string message = "Invalid format") -> Validator {
    return [pattern_str, msg = std::move(message)](const std::string &v) -> std::string {
        if (v.empty()) {
            return {};
        }
        const std::regex pattern{pattern_str};
        return std::regex_match(v, pattern) ? std::string{} : msg;
    };
}

/// @brief 数值范围（不可解析为数字也报错）。
[[nodiscard]] inline auto range(double min_v, double max_v, std::string message = {}) -> Validator {
    if (message.empty()) {
        message = "Must be between " + std::to_string(min_v) + " and " + std::to_string(max_v);
    }
    return [min_v, max_v, msg = std::move(message)](const std::string &v) -> std::string {
        if (v.empty()) {
            return {};
        }
        try {
            const double d = std::stod(v);
            return (d < min_v || d > max_v) ? msg : std::string{};
        } catch (...) {
            return msg;
        }
    };
}

/// @brief 组合验证器：依次执行，返回首个失败消息。
[[nodiscard]] inline auto combine(std::vector<Validator> list) -> Validator {
    return [list = std::move(list)](const std::string &v) -> std::string {
        for (const auto &fn : list) {
            if (!fn) {
                continue;
            }
            std::string err = fn(v);
            if (!err.empty()) {
                return err;
            }
        }
        return {};
    };
}

}  // namespace validators

/**
 * @brief 表单字段：包裹任意输入控件 + 验证器 + 错误文本展示。
 *
 * 子节点为实际输入控件（TextInput 等）；`value_provider` 提供当前值供验证；
 * `validate()` 执行验证并更新错误状态，验证失败时在子控件下方绘制红色错误文本。
 *
 * 对标 Flutter `FormField`、WPF 验证装饰。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class FormField : public SingleChild {
  public:
    FormField() = default;
    FormField(Node child, std::function<std::string()> value_provider, Validator validator)
        : SingleChild(std::move(child)), value_provider_(std::move(value_provider)), validator_(std::move(validator)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FormField"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "FormField",
            .properties =
                {
                    {.name = "error_text",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "当前错误消息（空=通过）",
                     .json_type = "string"},
                },
            .events = {"on_validate"},
            .children_policy = "single",
            .examples = {"au::FormField(input, []{ return value; }, au::validators::required())"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&error_); }

    /// @brief 执行验证：返回是否通过；错误消息写入响应式状态（驱动 UI 刷新）。
    auto validate() -> bool {
        if (!validator_ || !value_provider_) {
            error_.set(std::string{});
            return true;
        }
        const std::string err = validator_(value_provider_());
        error_.set(err);
        mark_needs_layout();
        mark_needs_paint();
        return err.empty();
    }

    /// @brief 清除错误状态。
    auto clear_error() -> void {
        error_.set(std::string{});
        mark_needs_paint();
    }

    /// @brief 当前错误消息（空 = 通过或未验证）。
    [[nodiscard]] auto error_text() const -> std::string { return error_.get(); }

    /// @brief 是否处于错误态。
    [[nodiscard]] auto has_error() const -> bool { return !error_.get().empty(); }

    /// @brief 设置验证器。
    auto set_validator(Validator v) -> void { validator_ = std::move(v); }
    /// @brief 设置值提供者。
    auto set_value_provider(std::function<std::string()> fn) -> void { value_provider_ = std::move(fn); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["error_text"] = error_.get();
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size child_size{.width = 0.0F, .height = 0.0F};
        if (child_) {
            child_size = child_.widget().layout(c, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = child_size});
        }
        // 错误态额外占用错误文本高度
        const float extra = has_error() ? AURORA_ERROR_TEXT_HEIGHT : 0.0F;
        return c.constrain(Size{.width = child_size.width, .height = child_size.height + extra});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            const Rect child_box{.origin = bounds.origin, .size = child_.bounds().size};
            child_.widget().paint(p, child_box, ctx);
            if (has_error()) {
                // 红色边框标识错误态
                p.draw_rect(child_box, Color(220, 53, 69, 255));
                // 错误文本绘制在子控件下方
                const Rect text_box{
                    .origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + child_box.size.height + 2.0F},
                    .size = Size{.width = bounds.size.width, .height = AURORA_ERROR_TEXT_HEIGHT}};
                Font err_font;
                err_font.size_pt = 11.0F;
                p.draw_text(text_box, error_.get(), err_font, Color(220, 53, 69, 255));
            }
        }
    }

  private:
    static constexpr float AURORA_ERROR_TEXT_HEIGHT = 18.0F;  ///< 错误文本预留高度（dp）

    std::function<std::string()> value_provider_;  ///< 当前值提供者（供验证）
    Validator validator_;  ///< 验证器
    State<std::string> error_{std::string{}};  ///< 当前错误消息（响应式）
};

/**
 * @brief 表单容器：聚合多个 FormField，统一验证与提交。
 *
 * `submit()` 依次验证所有字段：全部通过则调用 `on_submit`；任一失败则不提交，
 * 各字段各自展示错误。对标 Flutter `Form`、HTML `<form>`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Form : public Container {
  public:
    Form() = default;
    explicit Form(std::vector<Node> children, std::function<void()> on_submit = {}) : on_submit_(std::move(on_submit)) {
        children_ = std::move(children);
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Form"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Form",
            .properties =
                {
                    {.name = "gap",
                     .type = "float",
                     .default_value = "8.0",
                     .required = false,
                     .note = "字段垂直间距",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {"on_submit"},
            .children_policy = "multiple",
            .allowed_child_types = {"FormField"},
            .examples = {"au::Form({ field1, field2 }, []{ save(); })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 验证全部字段（递归查找子树中的 FormField）：返回是否全部通过。
    auto validate_all() -> bool {
        bool all_ok = true;
        for (Node &child : children_) {
            all_ok = validate_recursive(child.widget()) && all_ok;
        }
        return all_ok;
    }

    /// @brief 提交：验证全部字段，通过则触发 on_submit 并返回 true。
    auto submit() -> bool {
        if (!validate_all()) {
            return false;
        }
        if (on_submit_) {
            on_submit_();
        }
        return true;
    }

    /// @brief 清除全部字段错误。
    auto clear_errors() -> void {
        for (Node &child : children_) {
            clear_recursive(child.widget());
        }
    }

    /// @brief 设置提交回调。
    auto set_on_submit(std::function<void()> cb) -> void { on_submit_ = std::move(cb); }

    /// @brief 设置字段间距（链式）。
    auto set_gap(float gap) -> Form & {
        gap_ = gap < 0.0F ? 0.0F : gap;
        return *this;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 垂直排布（同 Column 简化版：字段逐行向下）
        float y = 0.0F;
        float max_w = 0.0F;
        const Constraints inner{.min = Size{.width = 0.0F, .height = 0.0F},
                                .max = Size{.width = c.max.width, .height = c.max.height}};
        for (Node &child : children_) {
            const Size s = child.widget().layout(inner, ctx);
            child.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = y}, .size = s});
            y += s.height + gap_;
            max_w = std::max(max_w, s.width);
        }
        if (!children_.empty()) {
            y -= gap_;  // 末尾不加间距
        }
        return c.constrain(Size{.width = max_w, .height = y});
    }

  private:
    static auto validate_recursive(Widget &w) -> bool {
        bool ok = true;
        if (auto *field = dynamic_cast<FormField *>(&w)) {
            ok = field->validate();
        }
        w.for_each_child([&ok](const Widget &child) -> void {
            // for_each_child 是 const 遍历；FormField 验证需要非 const，安全去 const
            ok = validate_recursive(const_cast<Widget &>(child)) && ok;  // NOLINT
        });
        return ok;
    }

    static auto clear_recursive(Widget &w) -> void {
        if (auto *field = dynamic_cast<FormField *>(&w)) {
            field->clear_error();
        }
        w.for_each_child([](const Widget &child) -> void {
            clear_recursive(const_cast<Widget &>(child));  // NOLINT
        });
    }

    std::function<void()> on_submit_;
    float gap_ = 8.0F;
};

}  // namespace aurora
