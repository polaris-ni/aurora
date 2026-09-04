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

/**
 * @brief 分步向导。
 *
 * `Stepper{steps, current, on_complete, on_cancel}` — 多步骤表单。
 * 支持线性步骤、步骤验证、完成/取消回调。
 * 对标 Flutter `Stepper`、Qt `QWizard`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Stepper : public LeafWidget {
  public:
    Stepper() = default;
    explicit Stepper(std::vector<StepperStep> steps, int current = 0) : steps_(std::move(steps)), current_(current) {}

    [[nodiscard]] auto type_name() const -> const char * override {
        return "Stepper";
    }  // NOLINT(readability-convert-member-functions-to-static)
    [[nodiscard]] auto steps() const -> const std::vector<StepperStep> & { return steps_; }
    [[nodiscard]] auto current() const -> int { return current_; }
    auto set_current(int i) -> Stepper & {
        current_ = i;
        mark_needs_paint();
        return *this;
    }

    auto set_on_complete(std::function<void()> cb) -> Stepper & {
        on_complete_ = std::move(cb);
        return *this;
    }
    auto set_on_cancel(std::function<void()> cb) -> Stepper & {
        on_cancel_ = std::move(cb);
        return *this;
    }

    /// @brief 前进一步（验证当前步骤后推进）。
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
    auto prev() -> bool {
        if (current_ <= 0) {
            return false;
        }
        --current_;
        mark_needs_paint();
        return true;
    }

    [[nodiscard]] auto is_last_step() const -> bool { return current_ + 1 >= static_cast<int>(steps_.size()); }

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
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return describe_static();
    }  // NOLINT(readability-convert-member-functions-to-static)

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

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["current"] = current_;
        props["step_count"] = static_cast<int>(steps_.size());
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("current")) {
            current_ = props["current"].get<int>();
        }
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {  // NOLINT
        constexpr float step_h = 40.0F;
        const auto header_h = static_cast<float>(steps_.size()) * step_h;
        constexpr float content_h = 120.0F;
        constexpr float button_h = 44.0F;
        return c.constrain(Size{.width = 400.0F, .height = header_h + content_h + button_h});
    }

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
