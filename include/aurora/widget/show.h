#pragma once

#include <memory>
#include <utility>

#include "aurora/core/types.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 条件显示：当条件为真时显示子节点，
/// 否则自身尺寸为 0 且不绘制子节点。条件可为 `bool` 或 `State<bool>`（响应式）。
///
/// @code
/// auto visible = std::make_shared<State<bool>>(true);
/// au::Show(visible, au::Text("hello"));
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Show : public SingleChild {
  public:
    /// @brief 默认构造：无子节点，条件取成员默认值 true。
    Show() = default;
    /// @brief 以静态条件构造：child 移动接管，可见性固定为 condition。
    /// @param condition 为真时显示子节点，否则自身尺寸为 0。
    /// @param child 被包裹的子节点。
    explicit Show(bool condition, Node child) : SingleChild(std::move(child)), condition_(condition) {}

    /// @brief 以响应式条件构造：可见性随 state_ 取值，child 移动接管。
    /// @param condition 条件信号（State<bool> 共享指针）；为空时按 false 处理。
    /// @param child 被包裹的子节点。
    explicit Show(std::shared_ptr<State<bool>> condition, Node child)
        : SingleChild(std::move(child)), state_(std::move(condition)), condition_(state_ ? state_->get() : false) {}

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Show"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Show"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Show 的控件描述符（visible/尺寸属性、single 子策略与示例）。
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
    /// @brief 运行时自描述：转发静态描述符（Show 的 visible/尺寸属性与示例）。
    /// @return 本控件的静态描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 当前是否可见（条件为真时显示子节点）。
    /// @return 有 state_ 时取其当前值，否则取静态 condition_。
    [[nodiscard]] auto is_visible() const -> bool { return state_ ? state_->get() : condition_; }

    /// @brief 收养解析出的子节点：仅取首项作唯一子（single 子策略），其余丢弃。
    /// @param kids 子节点列表，整体移入本地后按需取首项。
    /// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved) 整 vector 已移入本地，剩余项随局部析构释放。
    auto adopt_children(std::vector<Node> &&kids) -> void override {
        auto local_kids = std::move(kids);  // 整 vector 移入本地，后续按需取首项
        if (!local_kids.empty()) {
            child_ = std::move(local_kids.front());
        }
    }

    /// @brief 收集本控件的可订阅信号视图：state_ 存在时收录条件信号。
    /// @param out 输出参数，收录本控件持有的 SignalViewBase 指针（无响应式条件时不写入）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (state_) {
            out.push_back(state_.get());
        }
    }

    /// @brief 序列化可见性属性：基类通用属性之外写入当前 is_visible() 值。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("visible", Json{is_visible()});
    }

  protected:
    /// @brief 禁用布局缓存：可见性由 State\<bool\> 驱动，on_layout 输出随状态翻转而变
    ///        （约束不变但结果可能变），必须退出布局缓存（见 Widget::can_cache_layout 文档的 Path B 说明）。
    /// @return 恒 false（本控件布局结果不可按约束缓存复用）。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

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
