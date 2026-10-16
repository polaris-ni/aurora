#include "aurora/widget/icon_widget.h"

#include <string>

#include "aurora/core/utf8.h"
#include "aurora/render/font_engine.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/props_io.h"

namespace aurora {
namespace {

/// @brief 解析图标数据：优先 `IconData` 字面，否则经注册表查语义名。
/// @param name 语义名（可空）。
/// @param literal 代码侧字面（可空）。
/// @return 命中返回 `IconData`；注册表未命中返回 `icon-name-not-registered` 结构化错误。
[[nodiscard]] auto resolve_data(const std::string &name, const std::optional<icon::IconData> &literal)
    -> Result<icon::IconData> {
    if (literal.has_value()) {
        return *literal;
    }
    if (name.empty()) {
        return make_error(ErrorCode::IconNameNotRegistered,
                          ErrorParams{{"name", ""}, {"detail", "no icon name or literal provided"}});
    }
    return icon::IconRegistry::lookup(name);
}

}  // namespace

auto Icon::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const auto data = resolve_data(icon_name_, icon_data_);
    if (!data.ok()) {
        AURORA_LOG_WARN("icon", "%s", data.error().message.c_str());
        return;
    }
    const Color color = color_.value_or(inherit_theme(ctx).roles.on_surface_variant);
    icon::paint_icon_data(p, bounds, data.value(), color, optical_bias_);
}

auto IconButton::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const auto data = resolve_data(icon_name_, icon_data_);
    if (!data.ok()) {
        AURORA_LOG_WARN("icon.button", "%s", data.error().message.c_str());
        return;
    }
    const auto roles = inherit_theme(ctx).roles;
    // 四态取色：禁用取降透明度的变体色；按下取 primary；悬停取 on_surface；常态取 on_surface_variant。
    Color color = color_.value_or(roles.on_surface_variant);
    if (!enabled_) {
        color = roles.on_surface_variant;
        color.a = static_cast<std::uint8_t>(static_cast<float>(color.a) * 0.38F);
    } else if (pressed_) {
        color = color_.value_or(roles.primary);
    } else if (hovered()) {
        color = roles.on_surface;
    }
    // 焦点环：复用 focus_ring，沿点击区描边。
    if (is_focused_ && enabled_) {
        p.draw_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y},
                         .size = Size{.width = bounds.size.width, .height = bounds.size.height}},
                    roles.focus_ring);
    }
    const Rect icon_box{.origin = Point{.x = bounds.origin.x + padding_, .y = bounds.origin.y + padding_},
                        .size = Size{.width = size_pt_, .height = size_pt_}};
    icon::paint_icon_data(p, icon_box, data.value(), color, 0.0F);
}

}  // namespace aurora
