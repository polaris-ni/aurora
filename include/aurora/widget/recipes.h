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

namespace aurora {

/**
 * @brief 组合配方（需求 #3）：由基础原语组合而成的高阶构件。
 *
 * 按既有决策以**自由辅助函数**形式提供（不新增核心 Widget 类），返回 `au::Node`，
 * 用户可像使用原语一样把它们嵌进任意布局。每个函数都只是把 `Row`/`Column`/`Scroll`/
 * `Button` 等原语按约定排版，保持“从原语推导”的设计哲学。
 */

/// @brief 表单一行：标签 + 字段。
struct FormRow {
    std::string label;
    Node field;
};

/// @brief 纵向表单：每行 = `Row(标签, 字段)`，整体包在带内边距的 Column 中。
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
inline auto toolbar(std::vector<Node> actions) -> Node {
    Row row{RowProps{.children = std::move(actions)}};
    row.modifier = Modifier{}.fill_max_width().background(Color{245, 245, 247, 255}).padding(4.0F);
    return row;
}

/// @brief 侧边栏：纵向导航条（固定宽度 + 浅色背景）。
inline auto sidebar(std::vector<Node> items, float width = 200.0F) -> Node {
    Column col{ColumnProps{.children = std::move(items)}};
    col.modifier = Modifier{}.width(width).background(Color{245, 245, 247, 255}).padding(4.0F);
    return col;
}

/// @brief 菜单栏：顶部横向菜单条（外观类似工具栏，语义为应用主菜单）。
inline auto menu_bar(std::vector<Node> items) -> Node {
    Row row{RowProps{.children = std::move(items)}};
    row.modifier = Modifier{}.fill_max_width().background(Color{235, 235, 240, 255}).padding(2.0F);
    return row;
}

/// @brief 列表视图：可滚动的纵向列表（Scroll 包裹 Column）。
inline auto list_view(std::vector<Node> items) -> Node {
    Column col{ColumnProps{.children = std::move(items)}};
    Scroll scroll{ScrollProps{.child = std::move(col), .step = 16.0F}};
    return scroll;
}

namespace detail {

/// @brief 标签内容体：按选中的索引显示对应页面的内容（随 selected 状态刷新）。
class TabBody : public Widget {
  public:
    TabBody(std::shared_ptr<State<int>> selected, std::vector<Node> pages)
        : selected_(std::move(selected)), pages_(std::move(pages)) {}

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (selected_) {
            out.push_back(selected_.get());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "TabBody"; }

    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "TabBody", .children_policy = "multiple"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const int i = current();
        if (i >= 0 && std::cmp_less(i, pages_.size())) {
            pages_[i].widget().set_layout_parent(this);
            return pages_[i].widget().layout(c, ctx);
        }
        return c.constrain(Size{.width = 0.0F, .height = 0.0F});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const int i = current();
        if (i >= 0 && std::cmp_less(i, pages_.size())) {
            pages_[i].widget().paint(p, bounds, ctx);
        }
    }

  private:
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
    std::string title;
    Node content;
};

/// @brief 选项卡视图：顶部标签按钮行 + 随选中态切换的内容体。
/// 选中态由内部 `State<int>` 持有，点击标签切换并触发内容刷新。
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
