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
    std::string label{};            ///< 步骤标题
    std::function<bool()> validate; ///< 验证当前步骤（可选）
};

/**
 * @brief 分步向导（规格 §3.14）。
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
    explicit Stepper(std::vector<StepperStep> steps, int current = 0) : m_steps(std::move(steps)), m_current(current) {}

    [[nodiscard]] auto type_name() const -> const char * override {
        return "Stepper";
    } // NOLINT(readability-convert-member-functions-to-static)
    [[nodiscard]] auto steps() const -> const std::vector<StepperStep> & { return m_steps; }
    [[nodiscard]] auto current() const -> int { return m_current; }
    auto set_current(int i) -> Stepper & {
        m_current = i;
        mark_needs_paint();
        return *this;
    }

    auto set_on_complete(std::function<void()> cb) -> Stepper & {
        m_on_complete = std::move(cb);
        return *this;
    }
    auto set_on_cancel(std::function<void()> cb) -> Stepper & {
        m_on_cancel = std::move(cb);
        return *this;
    }

    /// @brief 前进一步（验证当前步骤后推进）。
    auto next() -> bool {
        if (m_current < 0 || std::cmp_greater_equal(m_current, m_steps.size())) {
            return false;
        }
        if (m_steps[m_current].validate && !m_steps[m_current].validate()) {
            return false;
        }
        if (m_current + 1 >= static_cast<int>(m_steps.size())) {
            if (m_on_complete) {
                m_on_complete();
            }
            return false;
        }
        ++m_current;
        mark_needs_paint();
        return true;
    }

    /// @brief 后退一步。
    auto prev() -> bool {
        if (m_current <= 0) {
            return false;
        }
        --m_current;
        mark_needs_paint();
        return true;
    }

    [[nodiscard]] auto is_last_step() const -> bool { return m_current + 1 >= static_cast<int>(m_steps.size()); }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Stepper",
            .properties = {
                { .name = "current", .type = "int", .default_value = "0", .required = false, .note = "当前步骤序号", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "step_count", .type = "int", .default_value = "0", .required = false, .note = "步骤总数（只读描述）", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = { "on_complete", "on_cancel" },
            .children_policy = "none",
            .invariants = { "current >= 0", "current < step_count" },
            .examples = { R"(au::Stepper({ {"Step 1"}, {"Step 2"} }, 0))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return describe_static();
    } // NOLINT(readability-convert-member-functions-to-static)

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action != MouseAction::Press || e.button != MouseButton::Left) {
            return;
        }
        const float btn_y = size().height - 44.0f;
        // Cancel 区域
        if (e.local_position.x < 100.0f && e.local_position.y > btn_y) {
            if (m_on_cancel) {
                m_on_cancel();
            }
            e.handled = true;
            return;
        }
        // Next/Done 区域
        if (e.local_position.x > size().width - 110.0f && e.local_position.y > btn_y) {
            next();
            e.handled = true;
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["current"] = m_current;
        props["step_count"] = static_cast<int>(m_steps.size());
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("current")) {
            m_current = props["current"].get<int>();
        }
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { // NOLINT
        constexpr float step_h = 40.0f;
        const auto header_h = static_cast<float>(m_steps.size()) * step_h;
        constexpr float content_h = 120.0f;
        constexpr float button_h = 44.0f;
        return c.constrain(Size{ .width = 400.0f, .height = header_h + content_h + button_h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        const Font f{ .size_pt = 14.0f };
        const Font f_bold{ .size_pt = 14.0f, .weight = 700 };
        constexpr float step_h = 40.0f;
        float y = bounds.origin.y;

        // 步骤指示器
        for (size_t i = 0; i < m_steps.size(); ++i) {
            const bool is_current = std::cmp_equal(i, m_current);
            const bool is_done = std::cmp_less(i, m_current);
            const Color pending_color = is_done ? Color{ 100, 100, 100 } : Color{ 180, 180, 180 };
            const Color text_color = is_current ? Color{ 66, 133, 244 } : pending_color;
            const std::string prefix = is_done ? "✓ " : (std::to_string(i + 1) + std::string{ ". " });
            p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + 12.0f, .y = y + 10.0f },
                              .size = Size{ .width = 300.0f, .height = step_h } },
                        prefix + m_steps[i].label, is_current ? f_bold : f, text_color);
            y += step_h;
        }

        // 分隔线
        y += 4.0f;
        p.fill_rect(Rect{ .origin = Point{ .x = bounds.origin.x, .y = y },
                          .size = Size{ .width = bounds.size.width, .height = 1.0f } },
                    Color{ 220, 220, 220 });
        y += 8.0f;

        // 当前步骤内容区（占位）
        p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + 12.0f, .y = y },
                          .size = Size{ .width = 350.0f, .height = 80.0f } },
                    std::string{ "Step " } + std::to_string(m_current + 1) + std::string{ " content area" }, f,
                    Color{ 128, 128, 128 });

        // 底部按钮区
        const float btn_y = bounds.origin.y + bounds.size.height - 44.0f;
        // "Cancel" 按钮
        p.draw_rect(Rect{ .origin = Point{ .x = bounds.origin.x + 12.0f, .y = btn_y },
                          .size = Size{ .width = 80.0f, .height = 32.0f } },
                    Color{ 200, 200, 200 });
        p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + 20.0f, .y = btn_y + 6.0f },
                          .size = Size{ .width = 60.0f, .height = 20.0f } },
                    "Cancel", f, Color::black());
        // "Next/Done" 按钮
        const std::string btn_text = is_last_step() ? "Done" : "Next →";
        const float btn_x = bounds.origin.x + bounds.size.width - 100.0f;
        p.fill_rect(Rect{ .origin = Point{ .x = btn_x, .y = btn_y }, .size = Size{ .width = 88.0f, .height = 32.0f } },
                    Color{ 66, 133, 244 });
        p.draw_text(Rect{ .origin = Point{ .x = btn_x + 8.0f, .y = btn_y + 6.0f },
                          .size = Size{ .width = 72.0f, .height = 20.0f } },
                    btn_text, f, Color::white());
    }

  private:
    std::vector<StepperStep> m_steps{};
    int m_current = 0;
    std::function<void()> m_on_complete;
    std::function<void()> m_on_cancel;
};

} // namespace aurora
