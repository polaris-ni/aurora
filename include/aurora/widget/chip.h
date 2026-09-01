#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 标签/标记控件。
 *
 * `Chip{label, avatar?, on_delete?}` — 带可选头像图标与删除按钮的紧凑标签。
 * 胶囊圆角背景（`corner_radius < 0` 自动 = 高度一半）；悬停调暗反馈；
 * 文本色 / 字号 / 删除钮颜色均可配。
 *
 * 继承扩展点（protected 虚函数）：`paint_background` / `paint_foreground`。
 * 对标 Flutter `Chip`、SwiftUI `Label`、WPF 徽章模式。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Chip : public LeafWidget {
  public:
    Chip() = default;
    explicit Chip(std::string label, Color bg = Color{ 230, 230, 230 }) : m_label(std::move(label)), m_bg(bg) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Chip"; }
    [[nodiscard]] auto label() const -> const std::string & { return m_label; }
    auto set_label(std::string l) -> Chip & {
        m_label = std::move(l);
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto background() const -> Color { return m_bg; }
    auto set_background(Color c) -> Chip & {
        m_bg = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置头像文本（显示在标签左侧，如 emoji 或首字母）。
    auto set_avatar(std::string av) -> Chip & {
        m_avatar = std::move(av);
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto avatar() const -> const std::string & { return m_avatar; }

    /// @brief 设置删除回调（非空时显示 × 按钮）。
    auto set_on_delete(std::function<void()> cb) -> Chip & {
        m_on_delete = std::move(cb);
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置文本色（链式）。
    auto set_text_color(Color c) -> Chip & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置删除钮颜色（链式）。
    auto set_delete_color(Color c) -> Chip & {
        m_delete_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置字号 pt（链式）。
    auto set_font_size(float s) -> Chip & {
        m_font_size = s > 0.0f ? s : 13.0f;
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置圆角半径 dp（链式；< 0 自动 = 高度一半胶囊，0 = 直角）。
    auto set_corner_radius(float r) -> Chip & {
        m_corner_radius = r;
        mark_needs_paint();
        return *this;
    }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Chip",
            .properties = {
                { .name = "label", .type = "string", .default_value = "\"\"", .required = false, .note = "标签文字", .json_type = "string" },
                { .name = "avatar", .type = "string", .default_value = "\"\"", .required = false, .note = "前导图标/文字", .json_type = "string" },
                { .name = "background", .type = "Color", .default_value = "Color(230,230,230)", .required = false, .note = "背景色", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "Color::black()", .required = false, .note = "文本色", .json_type = "array" },
                { .name = "delete_color", .type = "Color", .default_value = "{128,128,128,255}", .required = false, .note = "删除钮颜色", .json_type = "array" },
                { .name = "font_size", .type = "float", .default_value = "13.0", .required = false, .note = "字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "corner_radius", .type = "float", .default_value = "-1.0", .required = false, .note = "圆角半径(dp)；<0 自动=高度一半", .json_type = "number" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = { "on_delete" },
            .children_policy = "none",
            .examples = { "au::Chip(\"Tag\").set_avatar(\"★\").set_on_delete([]{ })" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (m_on_delete && e.action == MouseAction::Press && e.button == MouseButton::Left) {
            // 简化：点击右半区触发删除
            if (e.local_position.x > size().width * 0.7f) {
                m_on_delete();
                e.handled = true;
            }
        }
    }

    /// @brief 悬停反馈：背景调暗。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        mark_needs_paint();
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["label"] = m_label;
        if (!m_avatar.empty()) {
            props["avatar"] = m_avatar;
        }
        props["background"] = color_to_json(m_bg);
        props["text_color"] = color_to_json(m_text_color);
        props["delete_color"] = color_to_json(m_delete_color);
        props["font_size"] = m_font_size;
        props["corner_radius"] = m_corner_radius;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("label")) {
            m_label = props["label"].get<std::string>();
        }
        if (props.contains("avatar")) {
            m_avatar = props["avatar"].get<std::string>();
        }
        if (props.contains("background")) {
            m_bg = json_to_color(props["background"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("delete_color")) {
            m_delete_color = json_to_color(props["delete_color"]);
        }
        if (props.contains("font_size")) {
            m_font_size = props["font_size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            m_corner_radius = props["corner_radius"].get<float>();
        }
    }

  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        paint_background(p, bounds);
        paint_foreground(p, bounds, ctx);
    }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const Font f{ .size_pt = m_font_size };
        constexpr float h_pad = 10.0f;
        constexpr float v_pad = 4.0f;
        float w = h_pad * 2.0f;
        if (!m_avatar.empty()) {
            w += render::FontEngine::measure_width(m_avatar, f) + 4.0f;
        }
        w += render::FontEngine::measure_width(m_label, f);
        if (m_on_delete) {
            w += render::FontEngine::measure_width(" ×", f);
        }
        const float h = render::FontEngine::measure_height(f) + (v_pad * 2.0f);
        return c.constrain(Size{ .width = w, .height = h });
    }

    /// @brief 继承扩展点：绘制胶囊背景（悬停调暗）。
    virtual auto paint_background(Painter &p, const Rect &bounds) -> void {
        const float radius = m_corner_radius >= 0.0f ? m_corner_radius : bounds.size.height * 0.5f;
        const Color bg = hovered() ? m_bg.shaded(0.92f) : m_bg;
        if (radius > 0.0f) {
            p.fill_rounded_rect(bounds, radius, bg);
        } else {
            p.fill_rect(bounds, bg);
        }
    }

    /// @brief 继承扩展点：绘制头像 + 标签 + 删除钮。
    /// @note 名称不可用 `paint_content`：该名在 `Widget` 中是 `virtual` 的绘制管线核心
    ///       （`paint()` → `render_into()` → `paint_content()`），签名完全一致会被**意外覆盖**，
    ///       导致管线绕过 `on_paint`，胶囊背景永不绘制（历史缺陷）。
    virtual auto paint_foreground(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
        const Font f{ .size_pt = m_font_size };
        float x = bounds.origin.x + 10.0f;
        const float y = bounds.origin.y + 4.0f;
        if (!m_avatar.empty()) {
            p.draw_text(Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = 100.0f, .height = 20.0f } },
                        m_avatar, f, m_text_color);
            x += render::FontEngine::measure_width(m_avatar, f) + 4.0f;
        }
        p.draw_text(Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = 200.0f, .height = 20.0f } },
                    m_label, f, m_text_color);
        x += render::FontEngine::measure_width(m_label, f);
        if (m_on_delete) {
            p.draw_text(Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = 30.0f, .height = 20.0f } },
                        " ×", f, m_delete_color);
        }
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::string m_label;
    std::string m_avatar;
    Color m_bg{ 230, 230, 230 };
    std::function<void()> m_on_delete;
    Color m_text_color = Color::black();                ///< 文本色
    Color m_delete_color = Color{ 128, 128, 128, 255 }; ///< 删除钮颜色
    float m_font_size = 13.0f;                          ///< 字号 pt
    float m_corner_radius = -1.0f;                      ///< 圆角半径 dp；< 0 自动 = 高度一半
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

/**
 * @brief 角标/计数徽章。
 *
 * `Badge{count, child}` — 在子控件右上角叠加数字徽章。
 * 对标 Flutter `Badge`、SwiftUI `.badge()`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Badge : public SingleChild {
  public:
    Badge() = default;
    explicit Badge(int count, Node child = Node{}) : SingleChild(std::move(child)), m_count(count) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Badge"; }
    [[nodiscard]] auto count() const -> int { return m_count; }
    auto set_count(int c) -> Badge & {
        m_count = c;
        mark_needs_paint();
        return *this;
    }
    auto set_badge_color(Color c) -> Badge & {
        m_badge_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置徽章文字色（链式；默认白色）。
    auto set_text_color(Color c) -> Badge & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Badge",
            .properties = {
                { .name = "count", .type = "int", .default_value = "0", .required = false, .note = "徽章数字", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "badge_color", .type = "Color", .default_value = "Color(255,0,0)", .required = false, .note = "徽章背景色", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "Color::white()", .required = false, .note = "徽章文字色", .json_type = "array" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = {},
            .children_policy = "single",
            .examples = { "au::Badge(5, au::Text{\"Inbox\"})" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["count"] = m_count;
        props["badge_color"] = color_to_json(m_badge_color);
        props["text_color"] = color_to_json(m_text_color);
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("count")) {
            m_count = props["count"].get<int>();
        }
        if (props.contains("badge_color")) {
            m_badge_color = json_to_color(props["badge_color"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
    }

  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (m_child) {
            m_child.widget().paint(p, bounds, ctx);
        }
        if (m_count > 0) {
            constexpr float bw = 20.0f;
            constexpr float bh = 18.0f;
            const Rect badge_rect{ .origin =
                                       Point{ .x = bounds.origin.x + bounds.size.width - bw, .y = bounds.origin.y },
                                   .size = Size{ .width = bw, .height = bh } };
            p.fill_rounded_rect(badge_rect, bh * 0.5f, m_badge_color); // 胶囊徽章（对标 Material Badge）
            const Font f{ .size_pt = 11.0f, .weight = 700 };
            const std::string txt = (m_count > 99) ? "99+" : std::to_string(m_count);
            p.draw_text(badge_rect, txt, f, m_text_color);
        }
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size child_size{ .width = 0, .height = 0 };
        if (m_child) {
            child_size = m_child.widget().layout(c, ctx);
        }
        // 徽章额外空间
        constexpr float badge_w = 20.0f;
        constexpr float badge_h = 18.0f;
        return c.constrain(Size{ .width = std::max(child_size.width, badge_w),
                                 .height = child_size.height + (m_count > 0 ? badge_h * 0.5f : 0.0f) });
    }

    int m_count = 0;                     // NOLINT(*-non-private-member-variables-in-classes)
    Color m_badge_color = Color::red();  // NOLINT(*-non-private-member-variables-in-classes)
                                         // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    Color m_text_color = Color::white(); ///< 徽章文字色
};

} // namespace aurora
