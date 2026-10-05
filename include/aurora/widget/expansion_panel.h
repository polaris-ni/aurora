#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 折叠面板：标题头 + 可折叠内容区。
///
/// 点击标题头切换展开/收起；`expanded()` 为响应式状态可订阅。
/// 收起时内容不参与布局（高度仅头部）；展开时头部下方显示内容。
///
/// 对标 Flutter `ExpansionTile`、WPF `Expander`、SwiftUI `DisclosureGroup`。
///
/// 本行隐式生成的拷贝/移动构造逐成员复制 std::function 回调 on_toggle_，而其拷贝与 operator()
/// 皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
/// 本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
/// 刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
///
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// NOLINTNEXTLINE(bugprone-exception-escape)
class ExpansionPanel : public SingleChild {
  public:
    ExpansionPanel() = default;

    /// @brief 构造：设定标题文本与内容子树，可选初始展开态。
    /// @param header 标题头显示的文本。
    /// @param content 折叠区内承载的内容节点，移动接管。
    /// @param initially_expanded 构造完成时的展开状态，默认收起。
    ExpansionPanel(std::string header, Node content, bool initially_expanded = false)
        : SingleChild(std::move(content)), header_(std::move(header)) {
        expanded_.set(initially_expanded);  // 初始展开态写入响应式状态，订阅方随后即可读到。
    }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "ExpansionPanel"。
    [[nodiscard]] auto type_name() const -> const char * override { return "ExpansionPanel"; }

    /// @brief 类级自描述：声明 header/expanded/header_height 属性与 on_toggle 事件。
    /// @return 本控件的静态描述符。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ExpansionPanel",
            .properties =
                {
                    {.name = "header",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = true,
                     .note = "Title text",
                     .json_type = "string"},
                    {.name = "expanded",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Expanded",
                     .json_type = "boolean"},
                    {.name = "header_height",
                     .type = "float",
                     .default_value = "36.0",
                     .required = false,
                     .note = "Header height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {"on_toggle"},
            .children_policy = "single",
            .examples = {R"(au::ExpansionPanel("Details", au::Text("content"), false))"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return 本控件的静态描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的响应式信号供宿主订阅。
    /// @param out 输出参数，追加 expanded 状态信号视图。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&expanded_); }

    /// @brief 取得展开状态的响应式引用，可订阅其变化。
    /// @return 内部 State<bool> 的非 const 引用。
    [[nodiscard]] auto expanded() -> State<bool> & { return expanded_; }
    /// @brief 当前是否展开（同步读快照）。
    /// @return expanded 状态的当前值。
    [[nodiscard]] auto is_expanded() const -> bool { return expanded_.get(); }
    /// @brief 标题头文本。
    /// @return header_ 的 const 引用。
    [[nodiscard]] auto header() const -> const std::string & { return header_; }

    /// @brief 展开/收起（触发 on_toggle）。
    /// @param v 目标展开状态；与当前值相同时不做任何事。
    auto set_expanded(bool v) -> void {
        if (v != expanded_.get()) {
            expanded_.set(v);
            mark_needs_layout();
            mark_needs_paint();
            if (on_toggle_) {
                on_toggle_(v);
            }
        }
    }

    /// @brief 切换状态。
    auto toggle() -> void { set_expanded(!expanded_.get()); }

    /// @brief 设置切换回调（链式）。
    /// @param cb 展开态变化时调用，入参为变化后的展开值。
    /// @return 自身引用，便于链式调用。
    auto set_on_toggle(std::function<void(bool)> cb) -> ExpansionPanel & {
        on_toggle_ = std::move(cb);
        return *this;
    }

    /// @brief 点击标题头切换展开。
    /// @param e 鼠标事件；命中标题头区域的 Press 被吞掉并触发 toggle。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && e.local_position.y < header_height_) {
            toggle();
            e.is_handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 本控件需要点击事件（标题头切换）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 将标题文本、展开状态与头部高度写入 JSON 对象。
    /// @param props 输出参数，序列化后的属性集合。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("header", header_);
        props.set("expanded", Json{expanded_.get()});
        props.set("header_height", header_height_);
    }

    /// @brief 从 JSON 恢复标题、展开状态与头部高度（缺键项保留当前值）。
    /// @param props 反序列化来源的属性对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("header")) {
            header_ = props.at("header")->as_or<std::string>("");
        }
        if (props.contains("expanded")) {
            expanded_.set(props.at("expanded")->as_or<bool>(false));
        }
        if (props.contains("header_height")) {
            header_height_ = props.at("header_height")->as_or<float>(0.0F);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float w = c.max.is_finite() ? c.max.width : 320.0F;
        float h = header_height_;
        if (expanded_.get() && child_) {
            Constraints inner;
            inner.min = Size{.width = 0.0F, .height = 0.0F};
            inner.max =
                Size{.width = w, .height = c.max.is_finite() ? std::max(0.0F, c.max.height - header_height_) : 1e9F};
            const Size cs = child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = header_height_}, .size = cs});
            h += cs.height;
        }
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = 13.0F;
        // 标题头
        const Rect head{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = header_height_}};
        p.fill_rect(head, Color(246, 246, 248, 255));
        p.draw_rect(head, Color(228, 228, 232, 255));
        // 展开箭头 + 标题
        const Rect arrow_box{.origin = Point{.x = head.origin.x + 10.0F, .y = head.origin.y + 10.0F},
                             .size = Size{.width = 16.0F, .height = header_height_ - 20.0F}};
        p.draw_text(arrow_box, expanded_.get() ? "v" : ">", f, Color(100, 100, 105, 255));
        const Rect title_box{.origin = Point{.x = head.origin.x + 30.0F, .y = head.origin.y + 10.0F},
                             .size = Size{.width = head.size.width - 40.0F, .height = header_height_ - 20.0F}};
        p.draw_text(title_box, header_, f, Color(30, 30, 30, 255));
        // 内容
        if (expanded_.get() && child_) {
            const Rect cb = child_.bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            child_.widget().paint(p, global, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (local.y < header_height_) {
            return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
        }
        if (expanded_.get() && child_) {
            const Rect cb = child_.bounds();
            // 闸并入内容的追加命中盒（同 `Container::on_hit_test` 口径）。
            if (cb.contains(local) || child_.widget().covers_extra_hit_box(local - cb.origin, ctx)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                return child_.widget().hit_test(local - cb.origin, global, ctx);
            }
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (local.y >= header_height_ && expanded_.get() && child_) {
            const Rect cb = child_.bounds();
            // 闸并入内容的追加命中盒（同 `Container::on_hit_test_chain` 口径）。
            if (cb.contains(local) || child_.widget().covers_extra_hit_box(local - cb.origin, ctx)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                return child_.widget().hit_test_chain(local - cb.origin, global, ctx);
            }
        }
        return {};
    }

  private:
    std::string header_;  ///< 标题头文本，序列化与绘制共用。
    State<bool> expanded_{false};  ///< 展开状态响应式信号，订阅方经 expanded() 取得。
    float header_height_ = 36.0F;  ///< 标题头高度（dp），同时作为点击命中判定边界。
    std::function<void(bool)> on_toggle_;  ///< 展开态变化回调，入参为变化后的展开值。
};

}  // namespace aurora
