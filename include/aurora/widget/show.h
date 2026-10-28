#pragma once

#include <utility>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/state/read_only_state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 条件显示：当条件为真时显示子节点，
/// 否则自身尺寸为 0 且不绘制子节点。条件可为 `bool` 或响应式读句柄（`State<bool>` /
/// `ReadOnlyState<bool>`，经句柄单向转换接收）。
///
/// @code
/// au::State<bool> visible{true};
/// au::Show(visible, au::Text("hello"));
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Show : public SingleChild {
  public:
    /// @brief 默认构造：无子节点，条件取成员默认值 true。
    Show() = default;
    /// @brief 以静态条件构造：child 移动接管，条件写入自有来源。
    /// @param condition 为真时显示子节点，否则自身尺寸为 0。
    /// @param child 被包裹的子节点。
    explicit Show(bool condition, Node child) : SingleChild(std::move(child)) {
        install_condition(ReadOnlyState<bool>{condition});
    }

    /// @brief 以只读条件句柄构造：可见性随句柄来源取值，child 移动接管。
    /// @param condition 条件读句柄（可由 `State<bool>` 隐式降级）；本控件不改写它。
    /// @param child 被包裹的子节点。
    explicit Show(ReadOnlyState<bool> condition, Node child) : SingleChild(std::move(child)) {
        install_condition(std::move(condition));
    }

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
                    {.name = "visible", .type = "bool", .default_value = "true", .required = false, .note = "Visible"},
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
    /// @return 安装来源的当前值。
    [[nodiscard]] auto is_visible() const -> bool { return condition_.get(); }

    /// @brief 条件来源（只读句柄）：供宿主接走或另行订阅。
    /// @return 当前安装的条件读句柄。
    [[nodiscard]] auto condition_source() const -> ReadOnlyState<bool> { return condition_; }

    /// @brief 替换条件来源（展示属性换源）：标记重排重绘并重建挂载期订阅。
    /// @param next 新的条件读句柄（可由 `State<bool>` 或 `bool` 隐式转换）。
    /// @return 自身引用，便于链式调用。
    auto set_condition_source(ReadOnlyState<bool> next) -> Show & {
        install_condition(std::move(next));
        return *this;
    }

    /// @brief 以字面量替换条件来源（等价于换到一份自有常量来源）。
    /// @param condition 新的条件值。
    /// @return 自身引用，便于链式调用。
    auto set_condition(bool condition) -> Show & {
        install_condition(ReadOnlyState<bool>{condition});
        return *this;
    }

    /// @brief 收养解析出的子节点：仅取首项作唯一子（single 子策略），其余丢弃。
    /// @param kids 子节点列表，整体移入本地后按需取首项。
    /// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved) 整 vector 已移入本地，剩余项随局部析构释放。
    auto adopt_children(std::vector<Node> &&kids) -> void override {
        auto local_kids = std::move(kids);  // 整 vector 移入本地，后续按需取首项
        if (!local_kids.empty()) {
            child_ = std::move(local_kids.front());
        }
    }

    /// @brief 收集本控件的可订阅信号视图：收录安装的条件来源。
    /// @param out 输出参数，收录本控件持有的条件读句柄。
    auto collect_signals(StateSink &out) -> void override { out.push(condition_); }

    /// @brief 序列化可见性属性：基类通用属性之外写入当前 is_visible() 值。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("visible", Json{is_visible()});
    }

    /// @brief 禁用布局缓存：可见性由 State\<bool\> 驱动，on_layout 输出随状态翻转而变
    ///        （约束不变但结果可能变），必须退出布局缓存（见 Widget::can_cache_layout 文档的 Path B 说明）。
    /// @return 恒 false（本控件布局结果不可按约束缓存复用）。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

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
    /// @brief 安装条件来源：换源后标记重排重绘，并由基类重建挂载期订阅（旧订阅固定旧源）。
    /// @param next 新的条件读句柄。
    auto install_condition(ReadOnlyState<bool> next) -> void {
        condition_ = std::move(next);
        mark_needs_layout();
        mark_needs_paint();
        resubscribe_signals();  // 未挂载时为空操作；挂载后旧订阅仍固定旧源，必须重建
    }

    ReadOnlyState<bool> condition_{true};  ///< 安装的条件来源（只读句柄）
};

}  // namespace aurora
