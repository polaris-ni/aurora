#pragma once

#include <memory>
#include <utility>

#include "aurora/core/types.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 条件显示：当条件为真时显示子节点，
 * 否则自身尺寸为 0 且不绘制子节点。条件可为 `bool` 或 `State<bool>`（响应式）。
 *
 * @code
 *   auto visible = std::make_shared<State<bool>>(true);
 *   au::Show(visible, au::Text("hello"));
 * @endcode
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Show : public SingleChild {
  public:
    Show() = default;
    explicit Show(bool condition, Node child) : SingleChild(std::move(child)), condition_(condition) {}

    explicit Show(std::shared_ptr<State<bool>> condition, Node child)
        : SingleChild(std::move(child)), state_(std::move(condition)), condition_(state_ ? state_->get() : false) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Show"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Show",
            .properties =
                {
                    {.name = "visible", .type = "bool", .default_value = "true", .required = false, .note = "是否可见"},
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::Show(true, au::Text(\"visible\"))"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 当前是否可见（条件为真时显示子节点）。
    [[nodiscard]] auto is_visible() const -> bool { return state_ ? state_->get() : condition_; }

    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    auto adopt_children(std::vector<Node> &&kids) -> void override {
        auto local_kids = std::move(kids);  // 整 vector 移入本地，后续按需取首项
        if (!local_kids.empty()) {
            child_ = std::move(local_kids.front());
        }
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (state_) {
            out.push_back(state_.get());
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["visible"] = is_visible();
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (const bool vis = is_visible(); !vis) {
            return c.constrain(Size{.width = 0.0F, .height = 0.0F});
        }
        const Size s = child_.widget().layout(c, ctx);
        return c.constrain(s);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (is_visible()) {
            child_.widget().paint(p, bounds, ctx);
        }
    }

  private:
    std::shared_ptr<State<bool>> state_;
    bool condition_ = true;
};

}  // namespace aurora
