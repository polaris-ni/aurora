#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 分步向导的步骤描述。
struct StepperStep {
    std::string label;  ///< 步骤标题
    std::function<bool()> validate;  ///< 验证当前步骤（可选）
};

/// @brief 分步向导。
///
/// `Stepper{steps, current, on_complete, on_cancel}` — 多步骤表单。
/// 支持线性步骤、步骤验证、完成/取消回调。
/// 对标 Flutter `Stepper`、Qt `QWizard`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Stepper : public LeafWidget {
  public:
    /// @brief 默认构造：无步骤、当前索引 0。
    Stepper() = default;
    /// @brief 以步骤列表构造向导。
    /// @param steps 步骤描述列表（标题 + 可选验证函数）。
    /// @param current 初始当前步骤索引（默认 0）。
    explicit Stepper(std::vector<StepperStep> steps, int current = 0) : steps_(std::move(steps)), current_(current) {}

    /// @brief 类型名字符串 "Stepper"。
    /// @return 静态字符串常量，指向类型名。
    [[nodiscard]] auto type_name() const -> const char * override {
        return "Stepper";
    }  // NOLINT(readability-convert-member-functions-to-static)
    /// @brief 只读访问步骤列表。
    /// @return 步骤描述列表的常量引用。
    [[nodiscard]] auto steps() const -> const std::vector<StepperStep> & { return steps_; }
    /// @brief 只读访问当前步骤索引。
    /// @return 当前步骤序号（未做边界归一化）。
    [[nodiscard]] auto current() const -> int { return current_; }
    /// @brief 设置当前步骤索引（标绘制脏）。
    /// @param i 目标步骤索引。
    /// @return 自身引用，便于链式调用。
    auto set_current(int i) -> Stepper & {
        current_ = i;
        mark_needs_paint();  // 步骤变化只影响绘制
        return *this;
    }

    /// @brief 设置完成回调（末步再前进时触发）。
    /// @param cb 无参回调；覆盖既有值。
    /// @return 自身引用，便于链式调用。
    auto set_on_complete(std::function<void()> cb) -> Stepper & {
        on_complete_ = std::move(cb);
        return *this;
    }
    /// @brief 设置取消回调（点击 Cancel 区域时触发）。
    /// @param cb 无参回调；覆盖既有值。
    /// @return 自身引用，便于链式调用。
    auto set_on_cancel(std::function<void()> cb) -> Stepper & {
        on_cancel_ = std::move(cb);
        return *this;
    }

    /// @brief 前进一步（验证当前步骤后推进）。
    /// @return true = 已推进；索引越界、验证失败或已在末步（改触发 on_complete）时为 false。
    auto next() -> bool {
        if (current_ < 0 || std::cmp_greater_equal(current_, steps_.size())) {
            return false;
        }
        if (steps_[current_].validate && !steps_[current_].validate()) {
            return false;
        }
        if (current_ + 1 >= static_cast<int>(steps_.size())) {
            if (on_complete_) {
                on_complete_();
            }
            return false;
        }
        ++current_;
        mark_needs_paint();
        return true;
    }

    /// @brief 后退一步。
    /// @return true = 已后退；已在首步（`current <= 0`）时为 false。
    auto prev() -> bool {
        if (current_ <= 0) {
            return false;
        }
        --current_;
        mark_needs_paint();  // 步骤变化只影响绘制
        return true;
    }

    /// @brief 是否已处于最后一步。
    /// @return `current + 1 >= steps.size()`；无步骤时为 true。
    [[nodiscard]] auto is_last_step() const -> bool { return current_ + 1 >= static_cast<int>(steps_.size()); }

    /// @brief 静态属性描述符：current/step_count 等可序列化属性与事件声明。
    /// @return 名为 "Stepper"、子策略为 none 的完整描述符。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Stepper",
            .properties =
                {
                    {.name = "current",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "当前步骤序号",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "step_count",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "步骤总数（只读描述）",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {"on_complete", "on_cancel"},
            .children_policy = "none",
            .invariants = {"current >= 0", "current < step_count"},
            .examples = {R"(au::Stepper({ {"Step 1"}, {"Step 2"} }, 0))"},
        };
    }
    /// @brief 运行时自描述：转发到静态描述符。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return describe_static();
    }  // NOLINT(readability-convert-member-functions-to-static)

    /// @brief 指针交互：左键按下命中底部按钮区——左侧触发取消回调、右侧前进一步。
    /// @param e 鼠标事件；命中时置 `is_handled`。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action != MouseAction::Press || e.button != MouseButton::Left) {
            return;
        }
        const float btn_y = size().height - 44.0F;
        // Cancel 区域
        if (e.local_position.x < 100.0F && e.local_position.y > btn_y) {
            if (on_cancel_) {
                on_cancel_();
            }
            e.is_handled = true;
            return;
        }
        // Next/Done 区域
        if (e.local_position.x > size().width - 110.0F && e.local_position.y > btn_y) {
            next();
            e.is_handled = true;
        }
    }

    /// @brief 序列化：写入通用属性 + current/step_count（后者为只读描述）。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性（width/height/show 等）
        props["current"] = current_;
        props["step_count"] = static_cast<int>(steps_.size());
    }

    /// @brief 反序列化：恢复通用属性与 current（缺失键保持当前值）。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);  // 先由基类恢复通用属性
        if (props.contains("current")) {
            current_ = props["current"].get<int>();
        }
    }

    /// @brief 无信号依赖：步骤与当前索引为运行时内部状态。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

  protected:
    /// @brief 固定宽 400dp；高度 = 步骤指示区（40dp/步）+ 内容占位 120dp + 按钮区 44dp，按约束夹取。
    /// @param c 父级传入的布局约束。
    /// @return 夹取后的期望尺寸。
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {  // NOLINT
        constexpr float step_h = 40.0F;  // 单个步骤指示行高度（dp）
        const auto header_h = static_cast<float>(steps_.size()) * step_h;  // 步骤指示区总高
        constexpr float content_h = 120.0F;  // 步骤内容区占位高度（dp）
        constexpr float button_h = 44.0F;  // 底部按钮区高度（dp）
        return c.constrain(Size{.width = 400.0F, .height = header_h + content_h + button_h});
    }

    /// @brief 绘制步骤指示器（当前高亮、已完成打勾）、分隔线、内容占位文本与 Cancel/Next-Done 按钮。
    /// @param p 目标画笔。
    /// @param bounds 本控件的绘制矩形（相对坐标系）。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        const Font f{.size_pt = 14.0F};
        const Font f_bold{.size_pt = 14.0F, .weight = 700};
        constexpr float step_h = 40.0F;
        float y = bounds.origin.y;

        // 步骤指示器
        for (size_t i = 0; i < steps_.size(); ++i) {
            const bool is_current = std::cmp_equal(i, current_);
            const bool is_done = std::cmp_less(i, current_);
            const Color pending_color = is_done ? Color{100, 100, 100} : Color{180, 180, 180};
            const Color text_color = is_current ? Color{66, 133, 244} : pending_color;
            const std::string prefix = is_done ? "✓ " : (std::to_string(i + 1) + std::string{". "});
            p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + 12.0F, .y = y + 10.0F},
                             .size = Size{.width = 300.0F, .height = step_h}},
                        prefix + steps_[i].label, is_current ? f_bold : f, text_color);
            y += step_h;
        }

        // 分隔线
        y += 4.0F;
        p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = y},
                         .size = Size{.width = bounds.size.width, .height = 1.0F}},
                    Color{220, 220, 220});
        y += 8.0F;

        // 当前步骤内容区（占位）
        p.draw_text(
            Rect{.origin = Point{.x = bounds.origin.x + 12.0F, .y = y}, .size = Size{.width = 350.0F, .height = 80.0F}},
            std::string{"Step "} + std::to_string(current_ + 1) + std::string{" content area"}, f,
            Color{128, 128, 128});

        // 底部按钮区
        const float btn_y = bounds.origin.y + bounds.size.height - 44.0F;
        // "Cancel" 按钮
        p.draw_rect(Rect{.origin = Point{.x = bounds.origin.x + 12.0F, .y = btn_y},
                         .size = Size{.width = 80.0F, .height = 32.0F}},
                    Color{200, 200, 200});
        p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + 20.0F, .y = btn_y + 6.0F},
                         .size = Size{.width = 60.0F, .height = 20.0F}},
                    "Cancel", f, Color::black());
        // "Next/Done" 按钮
        const std::string btn_text = is_last_step() ? "Done" : "Next →";
        const float btn_x = bounds.origin.x + bounds.size.width - 100.0F;
        p.fill_rect(Rect{.origin = Point{.x = btn_x, .y = btn_y}, .size = Size{.width = 88.0F, .height = 32.0F}},
                    Color{66, 133, 244});
        p.draw_text(
            Rect{.origin = Point{.x = btn_x + 8.0F, .y = btn_y + 6.0F}, .size = Size{.width = 72.0F, .height = 20.0F}},
            btn_text, f, Color::white());
    }

  private:
    std::vector<StepperStep> steps_;
    int current_ = 0;
    std::function<void()> on_complete_;
    std::function<void()> on_cancel_;
};

}  // namespace aurora
