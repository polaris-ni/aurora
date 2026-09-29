#pragma once

#include <memory>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/types.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 动态列表：把 `State<std::vector<T>>` 的每一项
/// 经 `itemBuilder` 展开为一个子 widget。容器尺寸随数据项变化，依赖变化时触发定点刷新。
///
/// @code
/// auto items = std::make_shared<State<std::vector<std::string>>>(std::vector<std::string>{"A","B","C"});
/// Repeater<std::string>{items, [](const std::string& s, int i){
/// return Node{ au::Text(s) };
/// }};
/// @endcode
///
/// @tparam T 列表项类型（须可拷贝）。
/// @param T 列表项类型（模板形参，与 @tparam 同值）。
template <typename T>
class Repeater : public Container {
  public:
    /// @brief 条目构建回调类型：给定条目值与下标返回一个子节点。
    /// @param T 回调入参类型：条目值（const T &）。
    using ItemBuilder = std::function<Node(const T &, int)>;

    /// @brief 构造动态列表：接管数据源信号与条目构建回调，并立即首轮展开。
    /// @param items 列表数据源信号（State<std::vector<T>>），共享持有。
    /// @param builder 条目构建回调，移动存入 builder_。
    Repeater(std::shared_ptr<State<std::vector<T>>> items, ItemBuilder builder)
        : items_(std::move(items)), builder_(std::move(builder)) {
        rebuild_if_needed();  // 首轮展开子树（built_ 此时必为 false）。
    }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Repeater"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Repeater"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Repeater 的控件描述符（尺寸属性、multiple 子策略与示例）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Repeater",
            .properties =
                {
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false},
                },
            .events = {},
            .children_policy = "multiple",
            .examples =
                {"au::Repeater<std::string>{items, [](const std::string& s, int i){ return Node{ au::Text(s) }; }}"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Repeater 的控件描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 当前允许的子树最大深度（默认 `AURORA_DEFAULT_MAX_WIDGET_DEPTH`）。
    /// @return max_depth_ 当前值。
    [[nodiscard]] auto max_depth() const -> std::size_t { return max_depth_; }
    /// @brief 设置子树最大深度；超限展开经 `Diagnostics` 截断（specification/01-core.md §4.4）。
    /// @param d 新的最大深度。
    auto set_max_depth(std::size_t d) -> void { max_depth_ = d; }

    /// @brief 收集本控件的可订阅信号视图：items_ 存在时收录数据源信号。
    /// @param out 输出参数，收录本控件持有的 SignalViewBase 指针。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (items_) {
            out.push_back(static_cast<SignalViewBase *>(items_.get()));
        }
    }
    /// @brief 序列化属性：数据源为运行时态不可序列化，仅附加说明 note。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("note", "Repeater items are runtime-state driven, not serialized");
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        rebuild_if_needed();
        for (Node &child : children_) {
            child.widget().set_layout_parent(this);
        }
        float max_w = 0.0F;
        float total_h = 0.0F;
        for (Node &child : children_) {
            const Size s = child.widget().layout(c, ctx);
            max_w = std::max(max_w, s.width);
            total_h += s.height;
        }
        max_w = std::max(c.min.width, std::min(max_w, c.max.width));
        total_h = std::max(c.min.height, std::min(total_h, c.max.height));
        return c.constrain(Size{.width = max_w, .height = total_h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        float y = bounds.origin.y;
        for (Node &child : children_) {
            const Size s = child.widget().size();
            Rect child_bounds{.origin = Point{.x = bounds.origin.x, .y = y}, .size = s};
            child.widget().paint(p, child_bounds, ctx);
            y += s.height;
        }
    }

  private:
    // 仅在数据源 size 变化（或尚未构建）时重建子节点，避免每次布局都重新构造全部 widget。
    auto rebuild_if_needed() -> void {
        if (!items_ || !builder_) {
            return;
        }
        // 本检查把 `items_->get()` 读成「智能指针上多余的 get()」：此处的 `get()` 属于 `State`（值
        // 读取口，返回 `const std::vector<T> &`），不是 `shared_ptr::get()`——按建议删掉就成
        // `items_.size()`，编译不过。仅浏览器口径命中——native 遍同一份代码不报
        // （CODING_STANDARDS.md §5.2 的口径差异）。
        // NOLINTNEXTLINE(readability-redundant-smartptr-get)
        const std::size_t n = items_->get().size();
        if (built_ && n == child_count_) {
            return;
        }
        const std::vector<T> &data = items_->get();
        std::vector<Node> kids;
        kids.reserve(data.size());
        for (std::size_t i = 0; i < data.size(); ++i) {
            kids.push_back(builder_(data[i], static_cast<int>(i)));
        }
        children_ = std::move(kids);
        child_count_ = data.size();
        built_ = true;

        const std::size_t d = widget_tree_depth(*this);
        if (d > max_depth_) {
            Diagnostics::degraded("repeater",
                                  "Repeater subtree depth " + std::to_string(d) + " exceeds max_depth " +
                                      std::to_string(max_depth_) + "; truncating expansion",
                                  "repeater.h:rebuild_if_needed");
            children_.clear();
        }
    }

    std::shared_ptr<State<std::vector<T>>> items_;
    ItemBuilder builder_;
    std::size_t child_count_ = 0;
    bool built_ = false;
    std::size_t max_depth_ = AURORA_DEFAULT_MAX_WIDGET_DEPTH;

    /// @brief 递归统计 widget 子树深度（含自身），用于有界层深度守卫（specification/01-core.md §4.4）。
    [[nodiscard]] static auto widget_tree_depth(const Widget &w) -> std::size_t {
        std::size_t best = 0;
        for (const Node &c : w.child_nodes()) {
            best = std::max(best, widget_tree_depth(c.widget()));
        }
        return 1 + best;
    }
};

}  // namespace aurora
