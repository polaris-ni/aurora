#pragma once

#include <functional>
#include <string>
#include <utility>

#include "aurora/event/focus.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"  // inherit_theme：遮罩取主题 scrim
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 对话框控件。
///
/// 模态覆盖层：显示/隐藏由 `open_` 控制，关闭时不渲染。
/// 内容居中显示在半透明遮罩之上。
///
/// @section dialog_geom 几何与命中契约
///
/// 几何权威唯一落在 `Node::bounds_`：`on_layout` 度量内容后把**居中后的内容盒**写入
/// `children_[0]`，`on_paint` 直接读该盒落笔（遮罩仍按自身 bounds 铺满）。命中链与绘制
/// 因此共用同一次折算——两者若各算一遍居中，窗口尺寸或内容尺寸一变就会分叉。
///
/// 命中语义分两区：
/// - **内容盒内**：下降到 `children_[0]` 子树，按钮等可交互控件正常命中。
/// - **内容盒外（遮罩区）**：本控件自身入链、吸收该次点击，**不下落穿透**到对话框下方的
///   视口。模态对话框必须挡住下层交互，否则「想点确认却在下层起了个选区」。
///   遮罩点击**不触发** `on_close_`（见 `set_on_close`）。
///
/// 关闭态不参与命中：`open_` 为假时命中链恒为空，即便上帧残留的 `bounds` 仍有效。
///
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Dialog : public Container {
  public:
    Dialog() = default;
    /// @brief 以内容节点构造（对话框初始为关闭态，需 `show()` 才渲染）。
    /// @param content 置于遮罩中央的内容根节点，收为本类唯一子节点。
    explicit Dialog(Node content) { children_.push_back(std::move(content)); }

    /// @brief 控件类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Dialog"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Dialog"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "Dialog" 的描述表：属性 `open`、事件 `on_close`、子节点策略 "single"。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Dialog",
            .properties =
                {
                    {.name = "open",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Visible",
                     .json_type = "boolean"},
                },
            .events = {"on_close"},
            .children_policy = "single",
            .examples = {R"(au::Dialog(au::alert("Title", "Message")))"},
        };
    }
    /// @brief 实例级自描述：对话框无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：`open` 是普通 bool 字段，无响应式属性可登记。
    auto collect_signals(StateSink & /*out*/) -> void override {}

    /// @brief 显示对话框：压入焦点作用域——Tab 焦点关在本层内、焦点自动移入首个可聚焦控件。
    /// 派发回调（点击处理器等）内调用时可取到当前 FocusManager；无焦点管理器时降级为仅置位。
    /// @note Side-effects: pushes focus scope, marks layout dirty. 必须标布局脏：
    ///       `on_layout` 以 `open_` 为分支（关闭态返回零尺寸），而布局缓存的命中条件只看
    ///       约束相等——不标脏则开/关切换会复用上帧缓存，`on_layout` 不重跑、内容盒不更新。
    auto show() -> void {
        if (!open_ && current_focus_manager() != nullptr) {
            current_focus_manager()->push_scope(this);
        }
        open_ = true;
        mark_needs_layout();
    }

    /// @brief 关闭对话框：弹出焦点作用域并恢复打开前焦点。
    /// @note Side-effects: pops focus scope, marks layout dirty, may invoke on_close.
    ///       标布局脏的理由同 `show()`：关闭态 `on_layout` 返回零尺寸，缓存命中会让零尺寸
    ///       布局与「已打开」的绘制状态并存。
    auto close() -> void {
        if (open_ && current_focus_manager() != nullptr) {
            current_focus_manager()->pop_scope();
        }
        open_ = false;
        mark_needs_layout();
        if (on_close_) {
            on_close_();
        }
    }

    /// @brief 是否打开。
    /// @return `show()` 之后、`close()` 之前为 true（关闭态不渲染、布局返回零尺寸、不参与命中）。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 设置关闭回调（`close()` 末尾同步调用）。
    /// @param cb 关闭时执行的闭包；传空 `std::function` 即清除既有回调。
    /// @note 遮罩区点击**不**触发本回调（模态对话框的遮罩只吸收点击，不等于取消）。
    auto set_on_close(std::function<void()> cb) -> void { on_close_ = std::move(cb); }

    /// @brief 设置内容（替换而非追加：先清空既有子节点）。
    /// @param content 置于遮罩中央的内容根节点。
    /// @note Side-effects: marks layout dirty. 必须标布局脏：新子节点的 `bounds_` 是默认
    ///       零盒（几何权威只在 `Node::bounds_`），不重排则内容既不落笔也不可命中。
    ///       宿主典型的「打开状态下复用同一实例换文案」序列依赖这一条。
    auto set_content(Node content) -> void {
        children_.clear();
        children_.push_back(std::move(content));
        mark_needs_layout();
    }

    // Widget 接口
  protected:
    [[nodiscard]] auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (!open_) {
            return Size{.width = 0.0F, .height = 0.0F};
        }
        // 对话框占满父约束
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 400.0F, .height = 300.0F};
        }
        // 布局子节点（内容限于容器盒的 0.8 倍）并把居中后的盒写入 children_[0]：
        // 几何权威唯一落在 Node::bounds_，on_paint 与命中链共用它（见类注释「几何与命中契约」）。
        // 居中偏移按 (self − content) / 2 计算，与 on_paint 原先的落笔公式同式，但现在只此一处。
        for (auto &child : children_) {
            Constraints inner;
            inner.min = Size{.width = 0.0F, .height = 0.0F};
            inner.max = Size{.width = self.width * 0.8F, .height = self.height * 0.8F};
            const Size content = child.widget().layout(inner, ctx);
            child.set_bounds(centered_box(self, content));
        }
        return self;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (!open_) {
            return;
        }
        // 半透明遮罩：按本控件的盒铺满（模态语义要求盖住下层视口，不限于内容盒）。
        p.fill_rect(bounds, inherit_theme(ctx).roles.scrim.with_alpha(128));
        // 内容按 on_layout 落定的居中盒落笔——不再在此另算一遍居中（两处各算一遍必然分叉）。
        // children_[0].bounds() 是相对本控件内容区的局部盒，转全局需加 bounds.origin。
        if (!children_.empty()) {
            const Rect cb = children_[0].bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            children_[0].widget().paint(p, global, ctx);
        }
    }

    /// @brief 命中链：内容盒内下降到内容子树，遮罩区本控件入链吸收（不下落穿透）。
    ///
    /// 遮罩区必须返回**非空**链：本控件 `wants_click()` 为假、又无后代入链，若此处返回空链，
    /// 上层 `OverlayHost::on_hit_test_chain` 会继续询问下层基础内容，点击将穿透到对话框下方的
    /// 视口（症状：想点确认却在下层起了个选区）。返回仅含自身的链即可让该次点击止于模态层。
    ///
    /// @param local 相对本控件内容区原点的命中点。
    /// @param bounds 本控件的绘制盒（`bounds.origin` 为其全局原点）。
    /// @param ctx 构建上下文，原样透传给内容子树。
    /// @return 内容盒内为内容子树的链；遮罩区为仅含本控件的单元素链；关闭态恒为空链。
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!open_ || children_.empty()) {
            return {};  // 关闭态/无内容：不可见即不参与命中（否则会命中上帧残留的 bounds）
        }
        const Rect cb = children_[0].bounds();
        // 闸并入内容的追加命中盒：内容里的覆盖绘制区（如展开的下拉面板）画在内容盒外，
        // 只按内容盒判定会被当成遮罩区、由本控件吸收（同 `Container` 口径）。
        if (cb.contains(local) ||
            children_[0].widget().covers_extra_hit_box(local - cb.origin, ctx, bounds.origin + cb.origin)) {
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            return children_[0].widget().hit_test_chain(local - cb.origin, global, ctx);
        }
        // 遮罩区：仅自身入链（不调 on_close_，见 set_on_close）。
        return std::vector{HitNode{this, weak_from_this(), bounds.origin}};
    }

  private:
    /// @brief 容器盒内居中一个内容盒（本控件几何折算的唯一实现）。
    /// @param self 本控件的盒尺寸。
    /// @param content 内容自然尺寸。
    /// @return 相对本控件内容区原点的居中盒（`bounds_` 存的即此矩形）。
    [[nodiscard]] static auto centered_box(const Size &self, const Size &content) -> Rect {
        return Rect{
            .origin = Point{.x = (self.width - content.width) * 0.5F, .y = (self.height - content.height) * 0.5F},
            .size = content};
    }

    bool open_ = false;
    std::function<void()> on_close_;
};

// ---------- 便捷工厂 ----------

namespace detail {

/// @brief 按主题惯例落定对话框标题的对齐（`conventions.dialog_title_alignment` 的唯一消费点）。
///
/// 两档：`Leading` 与正文同侧起排（接线批之前的行为，也是当前五套标定档的共同取值——没有任何
/// 一档把标题改成居中，见 `src/aurora/theming/theme_presets.cpp` 的惯例覆写行）；`Centered` 相对
/// 卡片居中，其自由空间由外层 Column 的 `CrossAxisAlignment::Stretch` 提供（见
/// `dialog_stretches_children`）。
///
/// @param title 标题文本。
/// @param theme 生效主题：只读 `conventions.dialog_title_alignment`。
/// @return 标题 Text 节点（18 号加粗，与接线前同规格）。
[[nodiscard]] inline auto dialog_title_text(const std::string &title, const Theme &theme) -> Node {
    Text text(title);  // 控件不可拷贝，只能就地构造后移动进 Node（见 widget.h 的所有权口径）
    text.font_size(18).bold();
    text.set_align(theme.conventions.dialog_title_alignment == DialogTitleAlignment::Centered ? TextAlign::Center
                                                                                              : TextAlign::Left);
    return {std::move(text)};
}

/// @brief 按主题惯例组装对话框动作按钮：本文件的两个工厂与 `LayoutConventions` 之间唯一的接线点。
///
/// 三档排布（枚举语义见 theming/layout_conventions.h）：
/// - `AcceptFirst`：水平行、接受钮在左。Material / Windows / KDE 档取此，且工厂产出的树与接线批
///   之前**逐位同构**（同一层 Row、同一间距、不多包一层），零回归判据靠这一条兜住。
/// - `AcceptLast`：水平行、拒绝钮在左、整行靠卡片右缘（GNOME/Adwaita 的 [取消] 左、[确认] 右）。
/// - `VerticalRightAligned`：竖排、接受钮在上、两钮右缘对齐并整体靠卡片右缘（macOS 口径）。
///
/// 宽度口径（非默认档的既有取舍，写明白胜过留坑）：End 对齐要有自由空间，就得有「卡片宽」。工厂
/// 不拥有卡片宽——真实对话框由宿主定宽，本库 `Dialog` 给内容的是 0.8 倍视口盒。故非默认档的动作
/// 容器取 `MainAxisSize::Max` 撑满该内容盒、再用 `main_axis = End` 把钮推到右缘，配套外层 Column
/// 用 `CrossAxisAlignment::Stretch` 让标题/正文同占卡片宽（见 `dialog_stretches_children`）。
/// 代价是非默认档的卡片不再「贴内容宽」；默认档不包层也不撑宽，形态与接线前逐位一致。
/// 注：`Stretch` 只放大子项盒、不重跑子项内部布局（`src/aurora/layout/flex_layouter.cpp` 的定位
/// pass），所以 Text 靠自身 `TextAlign` 就能借到这份宽度，而按钮组必须自己 `Max` 才推得动。
///
/// 间距一律读令牌：钮间距 = `theme.spacing.medium`、钮组与正文之间由外层 Column 的
/// `theme.spacing.large` 承担。Material 基线档实测 8 / 12，与接线前的两个字面量同值。
///
/// @param theme 生效主题：只读 `conventions` 与 `spacing`，不读色。
/// @param accept 接受钮（结果为正的那个，如 OK / Yes）。
/// @param reject 拒绝钮（取消侧）；空节点表示单钮对话框。
/// @return 动作按钮的承载节点。单钮 + `AcceptFirst` 原样返回 accept（不多包一层）；其余组合包一层
///         撑满 + End 容器——包的层只在非默认档出现，故默认档的盒与命中链逐位不变。
[[nodiscard]] inline auto dialog_actions(const Theme &theme, Node accept, Node reject = {}) -> Node {
    const auto order = theme.conventions.dialog_button_order;
    const float gap = theme.spacing.medium;
    const auto fill_right = []() {  // 动作行统一口径：主轴 End、撑满可用宽度
        return RowProps().set_main_axis_alignment(MainAxisAlignment::End).set_main_axis_size(MainAxisSize::Max);
    };

    if (!reject) {
        if (order == DialogButtonOrder::AcceptFirst) {
            return accept;  // 与接线前同构：按钮直接作 Column 子项
        }
        auto row = Row({std::move(accept)}, fill_right());
        return {std::move(row)};
    }

    if (order == DialogButtonOrder::VerticalRightAligned) {
        auto stack = Column({std::move(accept), std::move(reject)},
                            ColumnProps().set_cross_axis_alignment(CrossAxisAlignment::End).set_gap(gap));
        auto row = Row({Node{std::move(stack)}}, fill_right());
        return {std::move(row)};
    }

    if (order == DialogButtonOrder::AcceptLast) {
        std::swap(accept, reject);  // 拒绝钮换到行首、接受钮落到行尾（Adwaita 的 [取消] 左、[确认] 右）
        auto row = Row({std::move(accept), std::move(reject)}, fill_right().set_gap(gap));
        return {std::move(row)};
    }

    auto row = Row({std::move(accept), std::move(reject)}, RowProps().set_gap(gap));
    return {std::move(row)};
}

/// @brief 本轮惯例是否需要把子项盒拉到卡片同宽（决定外层 Column 的交叉轴对齐取 Stretch 还是 Start）。
///
/// 只有 `AcceptFirst` + `Leading`（Material 基线档，也是接线前的实际行为）不需要基准宽度；其余档
/// 里的标题居中与动作右靠都要求子项盒等于卡片宽，否则没有自由空间可用。取 Stretch 时卡片宽 =
/// `Dialog` 供给的内容盒宽（0.8 倍视口），与 `dialog_actions` 里动作容器的 `MainAxisSize::Max`
/// 同向；默认档两者都不启用，故盒子逐位不变。
///
/// @param theme 生效主题：只读 `conventions.dialog_button_order` 与 `dialog_title_alignment`。
/// @return 需要 Stretch 时 true；默认档为 false（保持接线前的盒逐位不变）。
[[nodiscard]] inline auto dialog_stretches_children(const Theme &theme) -> bool {
    return theme.conventions.dialog_button_order != DialogButtonOrder::AcceptFirst ||
           theme.conventions.dialog_title_alignment != DialogTitleAlignment::Leading;
}

}  // namespace detail

/// @brief 构建警告对话框（标题 + 消息 + 确定按钮）。
/// @param title 标题文本（18 号加粗）。
/// @param message 消息正文（14 号）。
/// @param on_ok 点击 OK 按钮时同步调用的闭包；传空则按钮不挂回调。
/// @param theme 生效主题：决定动作按钮排布与标题对齐（`conventions`）及两处间距（`spacing`）。
///        默认 `Theme::defaults()`，即本机风格档 × 未表态明暗；宿主已知生效主题时应把它传进来。
/// @return 内容节点（垂直 Column，间距取 `theme.spacing.large`：标题 + 消息 + 标签为 "OK" 的按钮），
///         可直接交给 `Dialog` 承载。
[[nodiscard]] inline auto alert(const std::string &title, const std::string &message, std::function<void()> on_ok = {},
                                const Theme &theme = Theme::defaults()) -> Node {
    auto ok_btn = Button(ButtonProps().set_label("OK"));
    if (on_ok) {
        ok_btn.set_on_click(std::move(on_ok));
    }

    auto content =
        Column({detail::dialog_title_text(title, theme), std::move(Text(message).font_size(14)),
                detail::dialog_actions(theme, std::move(ok_btn))},
               ColumnProps()
                   .set_cross_axis_alignment(detail::dialog_stretches_children(theme) ? CrossAxisAlignment::Stretch
                                                                                      : CrossAxisAlignment::Start)
                   .set_gap(theme.spacing.large));
    return {std::move(content)};
}

/// @brief 构建确认对话框（标题 + 消息 + 确定/取消按钮）。
/// @param title 标题文本（18 号加粗）。
/// @param message 消息正文（14 号）。
/// @param on_result 结果回调：点 "Yes" 传 true、点 "No" 传 false；传空则两个按钮都不挂回调。
/// @param theme 生效主题：决定动作按钮排布（`conventions.dialog_button_order` 三档）、标题对齐与
///        两处间距。默认 `Theme::defaults()`；宿主已知生效主题时应把它传进来。
/// @return 内容节点（垂直 Column，间距取 `theme.spacing.large`：标题 + 消息 + 动作按钮组）。
///         按钮组在 `AcceptFirst` / `AcceptLast` 档是水平 Row（间距 `theme.spacing.medium`），
///         在 `VerticalRightAligned` 档是右靠的竖排 Column。
[[nodiscard]] inline auto confirm(const std::string &title, const std::string &message,
                                  const std::function<void(bool)> &on_result = {},
                                  const Theme &theme = Theme::defaults()) -> Node {
    auto yes_btn = Button(ButtonProps().set_label("Yes"));
    auto no_btn = Button(ButtonProps().set_label("No"));
    if (on_result) {
        yes_btn.set_on_click([on_result]() -> void { on_result(true); });
        no_btn.set_on_click([on_result]() -> void { on_result(false); });
    }

    auto content =
        Column({detail::dialog_title_text(title, theme), std::move(Text(message).font_size(14)),
                detail::dialog_actions(theme, std::move(yes_btn), std::move(no_btn))},
               ColumnProps()
                   .set_cross_axis_alignment(detail::dialog_stretches_children(theme) ? CrossAxisAlignment::Stretch
                                                                                      : CrossAxisAlignment::Start)
                   .set_gap(theme.spacing.large));
    return {std::move(content)};
}

}  // namespace aurora
