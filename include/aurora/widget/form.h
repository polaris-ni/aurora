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

using Validator = std::function<std::string(const std::string &)>;  ///< 字段验证器：输入字符串 -> 错误消息（空 = 通过）

// 内置验证器工厂所在命名空间，对标 Flutter `FormFieldValidator`、Qt `QValidator`、WPF `ValidationRule`。
namespace validators {

/// @brief 必填：空字符串报错。
/// @param message 空值时的错误文案
/// @return 新验证器：输入为空返回该文案，非空返回空串
[[nodiscard]] inline auto required(std::string message = "This field is required") -> Validator {
    return [msg = std::move(message)](const std::string &v) -> std::string { return v.empty() ? msg : std::string{}; };
}

/// @brief 最小长度。
/// @param n       允许的最小字符数
/// @param message 错误文案；留空时自动生成 "Must be at least n characters"
/// @return 新验证器：长度小于 `n` 返回该文案，否则空串
[[nodiscard]] inline auto min_length(std::size_t n, std::string message = {}) -> Validator {
    if (message.empty()) {
        message = "Must be at least " + std::to_string(n) + " characters";
    }
    return [n, msg = std::move(message)](const std::string &v) -> std::string {
        return v.size() < n ? msg : std::string{};
    };
}

/// @brief 最大长度。
/// @param n       允许的最大字符数
/// @param message 错误文案；留空时自动生成 "Must be at most n characters"
/// @return 新验证器：长度大于 `n` 返回该文案，否则空串
[[nodiscard]] inline auto max_length(std::size_t n, std::string message = {}) -> Validator {
    if (message.empty()) {
        message = "Must be at most " + std::to_string(n) + " characters";
    }
    return [n, msg = std::move(message)](const std::string &v) -> std::string {
        return v.size() > n ? msg : std::string{};
    };
}

/// @brief 邮箱格式（宽松正则）。
/// @param message 格式不符时的错误文案
/// @return 新验证器：空串直接通过（留给 `required` 检查），非空按 `x@y.z` 宽松正则判定，不符返回该文案
[[nodiscard]] inline auto email(std::string message = "Invalid email address") -> Validator {
    return [msg = std::move(message)](const std::string &v) -> std::string {
        if (v.empty()) {
            return {};  // 空值交给 required 检查
        }
        // 样式正则刻意做成函数内 static **缓存**：提到调用点即每次校验重编一次正则（真性能缺陷），
        // 改成 constexpr 又不可能。惰性构造与跨 TU 初始化顺序无关（本检查的担心面），且仅浏览器
        // 口径命中——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
        // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
        static const std::regex PATTERN{R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)"};
        return std::regex_match(v, PATTERN) ? std::string{} : msg;
    };
}

/// @brief 自定义正则。
/// @param pattern_str ECMAScript 正则源串；非法样式在验证器 lambda 内构造 `std::regex` 时抛 `std::regex_error`
/// @param message     不匹配时的错误文案
/// @return 新验证器：空串直接通过，非空 `regex_match` 不命中返回该文案
[[nodiscard]] inline auto matches(const std::string &pattern_str, std::string message = "Invalid format") -> Validator {
    // 验证器 lambda 体内构造 std::regex（非法样式即抛 std::regex_error），转入 Validator（std::function）
    // 后被本检查一律判「不应抛出」——其 operator() 无 noexcept 规格，即 .clang-tidy 记录在案的系统性
    // 假告警面。样式由宿主在装配期传入，抛出沿栈交给调用验证器的宿主代码，本库不做异常捕获。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    return [pattern_str, msg = std::move(message)](const std::string &v) -> std::string {
        if (v.empty()) {
            return {};
        }
        const std::regex pattern{pattern_str};
        return std::regex_match(v, pattern) ? std::string{} : msg;
    };
}

/// @brief 数值范围（不可解析为数字也报错）。
/// @param min_v    闭区间下界
/// @param max_v    闭区间上界
/// @param message  错误文案；留空时自动生成 "Must be between min and max"
/// @return 新验证器：空串直接通过；`std::stod` 解析失败或落在区间外返回该文案
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
///
/// 可组合：`combine({required(), min_length(3)})`；空指针条目跳过。
/// @param list 验证器列表
/// @return 新验证器：按序执行各成员，返回首个非空错误消息；全部通过返回空串
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

/// @brief 表单字段：包裹任意输入控件 + 验证器 + 错误文本展示。
///
/// 子节点为实际输入控件（TextInput 等）；`value_provider` 提供当前值供验证；
/// `validate()` 执行验证并更新错误状态，验证失败时在子控件下方绘制红色错误文本。
///
/// 对标 Flutter `FormField`、WPF 验证装饰。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// @note 隐式生成的拷贝/移动构造逐成员复制 std::function 回调 value_provider_ / validator_，而其拷贝与
///       operator() 皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按
///       [except.spec] 本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，
///       本库回调路径刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// NOLINTNEXTLINE(bugprone-exception-escape)
class FormField : public SingleChild {
  public:
    /// @brief 默认构造：无子节点，回调与验证器均空（验证恒通过）。
    FormField() = default;
    /// @brief 组装字段：绑定输入控件、值提供者与验证器。
    /// @param child          实际输入控件子节点
    /// @param value_provider 当前值提供者，验证时读取被验证文本
    /// @param validator      验证器，作用于 `value_provider()` 的结果
    FormField(Node child, std::function<std::string()> value_provider, Validator validator)
        : SingleChild(std::move(child)), value_provider_(std::move(value_provider)), validator_(std::move(validator)) {}

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"FormField"`
    [[nodiscard]] auto type_name() const -> const char * override { return "FormField"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "FormField" 的描述表：单属性 `error_text`，事件 `on_validate`，单子节点
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "FormField",
            .properties =
                {
                    {.name = "error_text",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "Current error message (empty = passing)",
                     .json_type = "string"},
                },
            .events = {"on_validate"},
            .children_policy = "single",
            .examples = {"au::FormField(input, []{ return value; }, au::validators::required())"},
        };
    }
    /// @brief 实例级自描述：无实例差异描述，直接转发本类 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号采集：把字段错误态 `error_` 挂进视图层，供订阅刷新红框/错误文本。
    /// @param out 收集目标：追加本字段可订阅的 SignalView 节点指针
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&error_); }

    /// @brief 执行验证：返回是否通过；错误消息写入响应式状态（驱动 UI 刷新）。
    /// @return true=验证通过（错误态已清除）；false=失败，消息见 `error_text()`
    auto validate() -> bool {
        if (!validator_ || !value_provider_) {
            error_.set(std::string{});
            return true;
        }
        const std::string err = validator_(value_provider_());
        error_.set(err);  // 写入响应式错误状态（空串=通过）
        mark_needs_layout();  // 错误文本会增减高度，需重排
        mark_needs_paint();  // 红框/错误文本需重绘
        return err.empty();
    }

    /// @brief 清除错误状态。
    auto clear_error() -> void {
        error_.set(std::string{});
        mark_needs_paint();
    }

    /// @brief 当前错误消息（空 = 通过或未验证）。
    /// @return `error_` 响应式状态的现值
    [[nodiscard]] auto error_text() const -> std::string { return error_.get(); }

    /// @brief 是否处于错误态。
    /// @return 错误消息非空时为 true
    [[nodiscard]] auto has_error() const -> bool { return !error_.get().empty(); }

    /// @brief 设置验证器。
    /// @param v 新验证器，移动存入 `validator_`，下次 `validate()` 起生效
    auto set_validator(Validator v) -> void { validator_ = std::move(v); }
    /// @brief 设置值提供者。
    /// @param fn 新提供者，移动存入 `value_provider_`，`validate()` 经它读取被验证文本
    auto set_value_provider(std::function<std::string()> fn) -> void { value_provider_ = std::move(fn); }

    /// @brief 序列化 FormField 自有属性到 `props`。
    /// @param props 输出目标 JSON 对象：先由基类写入通用属性，再补本类的 `error_text` 键
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性
        props.set("error_text", error_.get());
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

/// @brief 表单容器：聚合多个 FormField，统一验证与提交。
///
/// `submit()` 依次验证所有字段：全部通过则调用 `on_submit`；任一失败则不提交，
/// 各字段各自展示错误。对标 Flutter `Form`、HTML `<form>`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Form : public Container {
  public:
    /// @brief 默认构造：无字段、无提交回调，间距取默认 8dp。
    Form() = default;
    /// @brief 组装表单：接管字段子节点并可选设置提交回调。
    /// @param children 字段子节点列表（应为 FormField）
    /// @param on_submit 提交回调，`submit()` 全通过时同步调用
    explicit Form(std::vector<Node> children, std::function<void()> on_submit = {}) : on_submit_(std::move(on_submit)) {
        children_ = std::move(children);
    }

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"Form"`
    [[nodiscard]] auto type_name() const -> const char * override { return "Form"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "Form" 的描述表：属性 `gap`，事件 `on_submit`，多子节点且仅允许 `FormField`
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Form",
            .properties =
                {
                    {.name = "gap",
                     .type = "float",
                     .default_value = "8.0",
                     .required = false,
                     .note = "Vertical spacing between fields",
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
    /// @brief 实例级自描述：无实例差异描述，直接转发本类 `describe_static()`（本容器无响应式信号）。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号采集：Form 自身无响应式状态，字段信号经各 `FormField` 上报，此处不收集。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 验证全部字段（递归查找子树中的 FormField）：返回是否全部通过。
    /// @return true=所有字段验证通过；各失败字段的错误消息已写入自身状态
    auto validate_all() -> bool {
        bool all_ok = true;  // 累积结果：任一字段失败即置 false
        for (Node &child : children_) {
            all_ok = validate_recursive(child.widget()) && all_ok;
        }
        return all_ok;
    }

    /// @brief 提交：验证全部字段，通过则触发 on_submit 并返回 true。
    /// @return true=全字段通过且已派发 `on_submit`；false=存在失败字段，未提交
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
    /// @param cb 新回调；`submit()` 全通过时同步调用
    auto set_on_submit(std::function<void()> cb) -> void { on_submit_ = std::move(cb); }

    /// @brief 设置字段间距（链式）。
    /// @param gap 目标间距（dp）；负值按 0 截断
    /// @return 引用 `*this`，便于链式续写
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
