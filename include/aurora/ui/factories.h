#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/state/reactive.h"
#include "aurora/widget/bottom_nav_bar.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/grid.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/lazy_row.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"

namespace aurora::ui {

namespace detail {
/// @brief 构造 `T` 控件、包成 `Node` 并追加到父容器，返回强类型裸指针。
/// @note 指针生命周期由父树 `shared_ptr` 持有，父树销毁后即失效，调用方勿长期持有。
template<typename T, typename... Args> [[nodiscard]] inline auto make_add(Container &parent, Args &&...args) -> T * {
    Node node(std::make_shared<T>(std::forward<Args>(args)...));
    T *raw = static_cast<T *>(&node.widget());
    parent.add(node);
    return raw;
}
} // namespace detail

/// @brief 文本标签。`text` 为主文案（覆盖 `props.content`）。
inline auto label(Container &parent, std::string_view text, TextProps props = {}) -> Text * {
    props.content = std::string(text);
    return detail::make_add<Text>(parent, std::move(props));
}

/// @brief 按钮。`text` 为主文案（覆盖 `props.label`）；`on_click` 可选，连接后于点击触发。
inline auto button(Container &parent, std::string_view text, ButtonProps props = {},
                   std::function<void()> on_click = {}) -> Button * {
    props.label = std::string(text);
    auto *b = detail::make_add<Button>(parent, std::move(props));
    if (on_click) {
        b->set_on_click(std::move(on_click));
    }
    return b;
}

/// @brief 单行文本输入框。`value` 为初始文本（覆盖 `props.value`）。
inline auto input(Container &parent, std::string_view value = "", TextInputProps props = {}) -> TextInput * {
    props.value = std::string(value);
    return detail::make_add<TextInput>(parent, std::move(props));
}

/// @brief 复选框（双模：`Reactive<bool>` 或简单 `bool` 初值）。
inline auto checkbox(Container &parent, Reactive<bool> checked, std::function<void(bool)> on_changed = {})
    -> Checkbox * {
    return detail::make_add<Checkbox>(parent, std::move(checked), std::move(on_changed));
}
inline auto checkbox(Container &parent, bool initial, std::function<void(bool)> on_changed = {}) -> Checkbox * {
    return checkbox(parent, Reactive<bool>{ initial }, std::move(on_changed));
}

/// @brief 滑块（双模：`Reactive<double>` 或简单 `double` 初值）。
inline auto slider(Container &parent, Reactive<double> value, std::function<void(double)> on_changed = {}) -> Slider * {
    return detail::make_add<Slider>(parent, std::move(value), std::move(on_changed));
}
inline auto slider(Container &parent, double initial, std::function<void(double)> on_changed = {}) -> Slider * {
    return slider(parent, Reactive<double>{ initial }, std::move(on_changed));
}

/// @brief 纵向盒子容器（`Column` 的语法糖）。
inline auto vbox(Container &parent, ColumnProps props = {}) -> Column * {
    return detail::make_add<Column>(parent, std::move(props));
}
/// @brief 横向盒子容器（`Row` 的语法糖）。
inline auto hbox(Container &parent, RowProps props = {}) -> Row * {
    return detail::make_add<Row>(parent, std::move(props));
}
/// @brief 叠加容器（`Stack`，子节点自由对齐）。
inline auto stack(Container &parent, Alignment align = Alignment::TopLeft) -> Stack * {
    return detail::make_add<Stack>(parent, std::vector<Node>{}, align);
}
/// @brief 网格容器（`Grid`）。
inline auto grid(Container &parent, GridProps props = {}) -> Grid * {
    return detail::make_add<Grid>(parent, std::move(props));
}
/// @brief 滚动容器（`Scroll`）。
inline auto scroll(Container &parent, ScrollProps props = {}) -> Scroll * {
    return detail::make_add<Scroll>(parent, std::move(props));
}
/// @brief 横向虚拟列表（`LazyRow`）。`builder` 在可见窗口内惰性构造子项。
inline auto lazy_row(Container &parent, int count, LazyRow::ItemBuilder builder, float item_extent = 96.0f)
    -> LazyRow * {
    auto *w = detail::make_add<LazyRow>(parent, count, std::move(builder), item_extent);
    return w;
}
/// @brief 横向虚拟列表（`LazyRow`，以属性块构造）。
inline auto lazy_row(Container &parent, LazyRowProps props) -> LazyRow * {
    return detail::make_add<LazyRow>(parent, std::move(props));
}
/// @brief 纵向虚拟列表（`LazyList`）。`builder` 在可见窗口内惰性构造子项。
inline auto lazy_list(Container &parent, int count, LazyList::ItemBuilder builder, float item_extent = 48.0f)
    -> LazyList * {
    return detail::make_add<LazyList>(parent, count, std::move(builder), item_extent);
}
/// @brief 底部导航栏（`BottomNavBar`）。
inline auto bottom_nav_bar(Container &parent, BottomNavBarProps props) -> BottomNavBar * {
    return detail::make_add<BottomNavBar>(parent, std::move(props));
}

} // namespace aurora::ui
