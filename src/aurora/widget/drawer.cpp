#include "aurora/widget/drawer.h"

#include <utility>

namespace aurora {

auto Drawer::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "Drawer",
        .properties = {
            { .name = "open", .type = "bool", .default_value = "false", .required = false, .note = "是否打开", .json_type = "boolean" },
            { .name = "side", .type = "DrawerSide", .default_value = "Left", .required = false, .note = "停靠侧", .json_type = "string", .enum_values = {"Left", "Right"} },
            { .name = "panel_width", .type = "float", .default_value = "240.0", .required = false, .note = "面板宽度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
            { .name = "permanent", .type = "bool", .default_value = "false", .required = false, .note = "永久模式（始终可见、无遮罩）", .json_type = "boolean" },
        },
        .events = { "on_toggle" },
        .children_policy = "multiple",
        .allowed_child_types = {},
        .examples = { "au::Drawer(content, sidebar, au::DrawerSide::Left, 240)" },
    };
}

auto Drawer::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{ .width = 480.0f, .height = 320.0f };
    }
    const float pw = std::min(m_panel_width, self.width);

    // 基础内容：永久模式让出面板宽度，否则占满
    if (m_content) {
        const float cw = m_permanent ? std::max(0.0f, self.width - pw) : self.width;
        const float cx = (m_permanent && m_side == DrawerSide::Left) ? pw : 0.0f;
        const Constraints cc{ .min = Size{ .width = cw, .height = self.height },
                              .max = Size{ .width = cw, .height = self.height } };
        m_content.widget().layout(cc, ctx);
        m_content.set_bounds(
            Rect{ .origin = Point{ .x = cx, .y = 0.0f }, .size = Size{ .width = cw, .height = self.height } });
    }
    // 面板（打开或永久时布局）
    if (m_panel && (m_permanent || m_open.get())) {
        const Constraints pc{ .min = Size{ .width = pw, .height = self.height },
                              .max = Size{ .width = pw, .height = self.height } };
        m_panel.widget().layout(pc, ctx);
        const float px = m_side == DrawerSide::Left ? 0.0f : self.width - pw;
        m_panel.set_bounds(
            Rect{ .origin = Point{ .x = px, .y = 0.0f }, .size = Size{ .width = pw, .height = self.height } });
    }
    return c.constrain(self);
}

auto Drawer::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    if (m_content) {
        const Rect cb = m_content.bounds();
        m_content.widget().paint(
            p,
            Rect{ .origin = Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                  .size = cb.size },
            ctx);
    }
    const bool panel_visible = m_permanent || m_open.get();
    if (panel_visible && m_panel) {
        // 模态遮罩
        if (!m_permanent) {
            p.fill_rect(bounds, Color(0, 0, 0, 96));
        }
        const Rect pb = m_panel.bounds();
        const Rect global{ .origin = Point{ .x = bounds.origin.x + pb.origin.x, .y = bounds.origin.y + pb.origin.y },
                           .size = pb.size };
        p.draw_shadow(global, m_side == DrawerSide::Left ? 2.0f : -2.0f, 0.0f, 8.0f, Color(0, 0, 0, 60));
        p.fill_rect(global, Color(252, 252, 254, 255));
        m_panel.widget().paint(p, global, ctx);
    }
}

auto Drawer::on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
    const bool panel_visible = m_permanent || m_open.get();
    // 面板优先
    if (panel_visible && m_panel) {
        const Rect pb = m_panel.bounds();
        if (pb.contains(local)) {
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + pb.origin.x, .y = bounds.origin.y + pb.origin.y },
                               .size = pb.size };
            Widget *r = m_panel.widget().hit_test(local - pb.origin, global, ctx);
            if (r != nullptr) {
                return r;
            }
            return this;
        }
        // 模态打开时遮罩命中自身（供关闭）
        if (!m_permanent) {
            return this;
        }
    }
    if (m_content) {
        const Rect cb = m_content.bounds();
        if (cb.contains(local)) {
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            Widget *r = m_content.widget().hit_test(local - cb.origin, global, ctx);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return nullptr;
}

auto ProgressDialog::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "ProgressDialog",
        .properties = {
            { .name = "message", .type = "string", .default_value = "\"\"", .required = false, .note = "消息文本", .json_type = "string" },
            { .name = "progress", .type = "float", .default_value = "-1", .required = false, .note = "进度(0..1，-1=不确定)", .json_type = "number", .enum_values = {}, .min_value = "-1", .max_value = "1" },
            { .name = "open", .type = "bool", .default_value = "false", .required = false, .note = "是否显示", .json_type = "boolean" },
            { .name = "cancellable", .type = "bool", .default_value = "true", .required = false, .note = "是否可取消", .json_type = "boolean" },
        },
        .events = { "on_cancel" },
        .children_policy = "none",
        .invariants = { "progress >= -1 && progress <= 1" },
        .examples = { "au::ProgressDialog(\"Loading...\", true)" },
    };
}

auto ProgressDialog::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{ .width = 400.0f, .height = 300.0f };
    }
    if (!m_open) {
        return c.constrain(Size{ .width = 0.0f, .height = 0.0f });
    }
    // 对话框盒（居中）
    const float bw = std::min(320.0f, self.width * 0.8f);
    constexpr float bh = 120.0f;
    const float bx = (self.width - bw) * 0.5f;
    const float by = (self.height - bh) * 0.5f;
    m_box_rect = Rect{ .origin = Point{ .x = bx, .y = by }, .size = Size{ .width = bw, .height = bh } };
    m_cancel_rect = Rect{ .origin = Point{ .x = bx + bw - 90.0f, .y = by + bh - 40.0f },
                          .size = Size{ .width = 80.0f, .height = 30.0f } };
    return c.constrain(self);
}

auto ProgressDialog::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    if (!m_open) {
        return;
    }
    // 遮罩 + 对话框
    p.fill_rect(bounds, Color(0, 0, 0, 128));
    const Rect box{ .origin =
                        Point{ .x = bounds.origin.x + m_box_rect.origin.x, .y = bounds.origin.y + m_box_rect.origin.y },
                    .size = m_box_rect.size };
    p.draw_shadow(box, 0.0f, 4.0f, 12.0f, Color(0, 0, 0, 80));
    p.fill_rect(box, Color(255, 255, 255, 255));

    Font f;
    f.size_pt = 13.0f;
    p.draw_text(Rect{ .origin = Point{ .x = box.origin.x + 16.0f, .y = box.origin.y + 14.0f },
                      .size = Size{ .width = box.size.width - 32.0f, .height = 20.0f } },
                m_message, f, Color(30, 30, 30, 255));

    // 进度条
    const Rect track{ .origin = Point{ .x = box.origin.x + 16.0f, .y = box.origin.y + 46.0f },
                      .size = Size{ .width = box.size.width - 32.0f, .height = 8.0f } };
    p.fill_rect(track, Color(230, 230, 234, 255));
    const float prog = m_progress.get();
    if (prog >= 0.0f) {
        p.fill_rect(Rect{ .origin = track.origin,
                          .size = Size{ .width = track.size.width * prog, .height = track.size.height } },
                    Color(0, 122, 255, 255));
    } else {
        // 不确定态：中段高亮块
        const float seg = track.size.width * 0.3f;
        p.fill_rect(Rect{ .origin = Point{ .x = track.origin.x + (track.size.width * 0.35f), .y = track.origin.y },
                          .size = Size{ .width = seg, .height = track.size.height } },
                    Color(0, 122, 255, 160));
    }

    // 取消按钮
    if (m_cancellable) {
        const Rect cbtn{ .origin = Point{ .x = bounds.origin.x + m_cancel_rect.origin.x,
                                          .y = bounds.origin.y + m_cancel_rect.origin.y },
                         .size = m_cancel_rect.size };
        p.fill_rect(cbtn, Color(240, 240, 244, 255));
        p.draw_rect(cbtn, Color(200, 200, 205, 255));
        p.draw_text(Rect{ .origin = Point{ .x = cbtn.origin.x + 18.0f, .y = cbtn.origin.y + 7.0f },
                          .size = Size{ .width = cbtn.size.width - 20.0f, .height = cbtn.size.height - 14.0f } },
                    "Cancel", f, Color(60, 60, 65, 255));
    }
}

auto ProgressDialog::on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * {
    if (!m_open) {
        return nullptr;
    }
    return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
}

auto PageView::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "PageView",
        .properties = {
            { .name = "current", .type = "int", .default_value = "0", .required = false, .note = "当前页码", .json_type = "integer", .enum_values = {}, .min_value = "0" },
            { .name = "show_indicator", .type = "bool", .default_value = "true", .required = false, .note = "是否显示圆点指示器", .json_type = "boolean" },
        },
        .events = { "on_page_change" },
        .children_policy = "multiple",
        .allowed_child_types = {},
        .invariants = { "current >= 0" },
        .examples = { "au::PageView({ page1, page2, page3 })" },
    };
}

auto PageView::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{ .width = 320.0f, .height = 240.0f };
    }
    // 仅布局当前页
    const int cur = m_current.get();
    if (cur >= 0 && std::cmp_less(cur, m_children.size())) {
        Node &page = m_children[static_cast<std::size_t>(cur)];
        const Constraints pc{ .min = Size{ .width = self.width, .height = self.height },
                              .max = Size{ .width = self.width, .height = self.height } };
        page.widget().layout(pc, ctx);
        page.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = self });
    }
    return c.constrain(self);
}

auto PageView::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const int cur = m_current.get();
    if (cur >= 0 && std::cmp_less(cur, m_children.size())) {
        m_children[static_cast<std::size_t>(cur)].widget().paint(p, bounds, ctx);
    }
    // 指示器圆点（底部居中；简化为小方块）
    if (m_show_indicator && m_children.size() > 1) {
        constexpr float dot = 8.0f;
        constexpr float gap = 8.0f;
        const float total =
            (static_cast<float>(m_children.size()) * dot) + (static_cast<float>(m_children.size() - 1) * gap);
        float x = bounds.origin.x + ((bounds.size.width - total) * 0.5f);
        const float y = bounds.origin.y + bounds.size.height - 20.0f;
        for (std::size_t i = 0; i < m_children.size(); ++i) {
            const bool active = std::cmp_equal(i, cur);
            p.fill_rect(Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = dot, .height = dot } },
                        active ? Color(0, 122, 255, 255) : Color(0, 0, 0, 60));
            x += dot + gap;
        }
    }
}

auto PageView::on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
    const int cur = m_current.get();
    if (cur >= 0 && std::cmp_less(cur, m_children.size())) {
        Node &page = m_children[static_cast<std::size_t>(cur)];
        const Rect cb = page.bounds();
        if (cb.contains(local)) {
            Widget *r = page.widget().hit_test(local - cb.origin, bounds, ctx);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
}

} // namespace aurora
