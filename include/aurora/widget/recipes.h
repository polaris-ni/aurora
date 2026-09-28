#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/dimension.h"
#include "aurora/core/types.h"
#include "aurora/state/signal_view.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

/// @brief 组合配方（需求 #3）：由基础原语组合而成的高阶构件。
///
/// 按既有决策以**自由辅助函数**形式提供（不新增核心 Widget 类），返回 `au::Node`，
/// 用户可像使用原语一样把它们嵌进任意布局。每个函数都只是把 `Row`/`Column`/`Scroll`/
/// `Button` 等原语按约定排版，保持“从原语推导”的设计哲学。
namespace aurora {

/// @brief 表单一行：标签 + 字段。
struct FormRow {
    std::string label;  ///< 行左侧标签文本。
    Node field;  ///< 行右侧字段控件节点。
};

/// @brief 纵向表单：每行 = `Row(标签, 字段)`，整体包在带内边距的 Column 中。
/// @param rows 表单行列表（标签 + 字段节点）。
/// @param label_width 标签列统一宽度（dp，默认 120）。
/// @return 组合完成的表单节点。
inline auto form_layout(std::vector<FormRow> rows, float label_width = 120.0F) -> Node {
    std::vector<Node> kids;
    kids.reserve(rows.size());
    for (auto &r : rows) {
        std::vector<Node> row_children;
        Text label{r.label};
        label.width(px(label_width));
        row_children.emplace_back(std::move(label));
        row_children.push_back(std::move(r.field));
        Row row{RowProps{.children = std::move(row_children)}};
        row.modifier = Modifier{}.padding(4.0F);
        kids.emplace_back(std::move(row));
    }
    Column col{ColumnProps{.children = std::move(kids)}};
    return col;
}

/// @brief 工具栏：横向动作条（带浅色背景与内边距，填满宽度）。
/// @param actions 动作控件节点列表（横向排列）。
/// @return 组合完成的工具栏节点。
inline auto toolbar(std::vector<Node> actions) -> Node {
    Row row{RowProps{.children = std::move(actions)}};
    row.modifier = Modifier{}.fill_max_width().background(Color{245, 245, 247, 255}).padding(4.0F);
    return row;
}

/// @brief 侧边栏：纵向导航条（固定宽度 + 浅色背景）。
/// @param items 导航项节点列表（纵向排列）。
/// @param width 侧边栏宽度（dp，默认 200）。
/// @return 组合完成的侧边栏节点。
inline auto sidebar(std::vector<Node> items, float width = 200.0F) -> Node {
    Column col{ColumnProps{.children = std::move(items)}};
    col.modifier = Modifier{}.width(width).background(Color{245, 245, 247, 255}).padding(4.0F);
    return col;
}

/// @brief 菜单栏：顶部横向菜单条（外观类似工具栏，语义为应用主菜单）。
/// @param items 菜单项节点列表（横向排列）。
/// @return 组合完成的菜单栏节点。
inline auto menu_bar(std::vector<Node> items) -> Node {
    Row row{RowProps{.children = std::move(items)}};
    row.modifier = Modifier{}.fill_max_width().background(Color{235, 235, 240, 255}).padding(2.0F);
    return row;
}

/// @brief 列表视图：可滚动的纵向列表（Scroll 包裹 Column）。
/// @param items 列表项节点列表（纵向排列）。
/// @return 包裹好滚动容器的列表视图节点。
inline auto list_view(std::vector<Node> items) -> Node {
    Column col{ColumnProps{.children = std::move(items)}};
    Scroll scroll{ScrollProps{.child = std::move(col), .step = 16.0F}};
    return scroll;
}

/// @brief 配方内部实现细节：选项卡内容体等不对外暴露的辅助类型。
namespace detail {

/// @brief 标签内容体：按选中的索引显示对应页面的内容（随 selected 状态刷新）。
class TabBody : public Widget {
  public:
    /// @brief 绑定选中态与页面内容列表。
    /// @param selected 共享的选中索引状态（变化时经信号触发重绘）。
    /// @param pages 各页面内容节点，按索引与标签一一对应。
    TabBody(std::shared_ptr<State<int>> selected, std::vector<Node> pages)
        : selected_(std::move(selected)), pages_(std::move(pages)) {}

    /// @brief 收集对选中态信号的依赖，使内容体随 tab_view 的选中索引刷新。
    /// @param out 输出向量；selected_ 非空时追加其裸指针。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (selected_) {
            out.push_back(selected_.get());
        }
    }

    /// @brief 类型名字符串 "TabBody"。
    /// @return 静态字符串常量，指向类型名。
    [[nodiscard]] auto type_name() const -> const char * override { return "TabBody"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "TabBody"、子策略为 multiple 的默认描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "TabBody", .children_policy = "multiple"};
    }

  protected:
    /// @brief 仅布局当前选中的页面；未选中页不参与布局。
    /// @param c 父级传入的布局约束。
    /// @param ctx 构建上下文（透传给页面子树）。
    /// @return 选中页的尺寸；索引越界时按约束夹取零尺寸。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const int i = current();
        if (i >= 0 && std::cmp_less(i, pages_.size())) {
            pages_[i].widget().set_layout_parent(this);
            return pages_[i].widget().layout(c, ctx);
        }
        return c.constrain(Size{.width = 0.0F, .height = 0.0F});
    }

    /// @brief 仅绘制当前选中的页面；未选中页不绘制。
    /// @param p 目标画笔（平移/裁剪由框架处理）。
    /// @param bounds 本控件的绘制矩形（相对坐标系）。
    /// @param ctx 构建上下文（透传给页面子树）。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const int i = current();
        if (i >= 0 && std::cmp_less(i, pages_.size())) {
            pages_[i].widget().paint(p, bounds, ctx);
        }
    }

  private:
    /// @brief 读取并夹取当前选中索引。
    /// @return 合法页索引；selected_ 缺失、无页或状态越界时为 0。
    [[nodiscard]] auto current() const -> int {
        if (!selected_ || pages_.empty()) {
            return 0;
        }
        const int s = selected_->get();
        if (s < 0 || std::cmp_greater_equal(s, pages_.size())) {
            return 0;
        }
        return s;
    }

    std::shared_ptr<State<int>> selected_;
    std::vector<Node> pages_;
};

}  // namespace detail

/// @brief 单个标签页。
struct TabPage {
    std::string title;  ///< 标签按钮上的页标题文本。
    Node content;  ///< 该页对应的内容控件节点。
};

/// @brief 选项卡视图：顶部标签按钮行 + 随选中态切换的内容体。
/// 选中态由内部 `State<int>` 持有，点击标签切换并触发内容刷新。
/// @param pages 标签页列表（标题 + 内容），按序各生成一个标签按钮。
/// @return 组合完成的选项卡节点（Row 标签行在上、内容体在下）。
inline auto tab_view(std::vector<TabPage> pages) -> Node {
    auto selected = std::make_shared<State<int>>(0);
    std::vector<Node> tab_buttons;
    tab_buttons.reserve(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const int idx = static_cast<int>(i);
        Button btn{pages[i].title};
        btn.on_click = [selected, idx]() -> void { selected->set(idx); };
        tab_buttons.emplace_back(std::move(btn));
    }
    std::vector<Node> bodies;
    bodies.reserve(pages.size());
    for (auto &pg : pages) {
        bodies.push_back(std::move(pg.content));
    }
    Node body = detail::TabBody{selected, std::move(bodies)};

    std::vector<Node> col_children;
    col_children.emplace_back(Row{RowProps{.children = std::move(tab_buttons)}});
    col_children.push_back(std::move(body));
    return Column{ColumnProps{.children = std::move(col_children)}};
}

}  // namespace aurora
