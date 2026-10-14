#include "aurora/app/menu.h"
#include "aurora/widget/menu_bar.h"

namespace aurora {

auto to_menu_model(const MenuItem &item) -> MenuModel {
    MenuModel model;
    model.id = item.id;
    model.label = item.label;
    model.enabled = item.enabled;
    model.checkable = item.checkable;
    model.checked = item.checked;
    model.shortcut_text = item.shortcut_text;
    model.separator = item.separator;
    model.children.reserve(item.children.size());
    for (const auto &child : item.children) {
        model.children.push_back(to_menu_model(child));
    }
    return model;
}

auto to_menu_model(const Menu &menu) -> MenuModel {
    MenuModel model;
    model.id = menu.id;
    model.label = menu.title;
    model.children.reserve(menu.items.size());
    for (const auto &item : menu.items) {
        model.children.push_back(to_menu_model(item));
    }
    return model;
}

auto to_menu_models(const std::vector<Menu> &menus) -> std::vector<MenuModel> {
    std::vector<MenuModel> models;
    models.reserve(menus.size());
    for (const auto &menu : menus) {
        models.push_back(to_menu_model(menu));
    }
    return models;
}

auto from_menu_model(const MenuModel &model) -> MenuItem {
    MenuItem item;
    item.id = model.id;
    item.label = model.label;
    item.enabled = model.enabled;
    item.checkable = model.checkable;
    item.checked = model.checked;
    item.shortcut_text = model.shortcut_text;
    item.separator = model.separator;
    item.children.reserve(model.children.size());
    for (const auto &child : model.children) {
        item.children.push_back(from_menu_model(child));
    }
    return item;
}

}  // namespace aurora
