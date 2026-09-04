#include "aurora/widget/drawer.h"

#include <utility>

namespace aurora {

auto Drawer::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "Drawer",
        .properties =
            {
                {.name = "open",
                 .type = "bool",
                 .default_value = "false",
                 .required = false,
                 .note = "是否打开",
                 .json_type = "boolean"},
                {.name = "side",
                 .type = "DrawerSide",
                 .default_value = "Left",
                 .required = false,
                 .note = "停靠侧",
                 .json_type = "string",
                 .enum_values = {"Left", "Right"}},
                {.name = "panel_width",
                 .type = "float",
                 .default_value = "240.0",
                 .required = false,
                 .note = "面板宽度(dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "permanent",
                 .type = "bool",
                 .default_value = "false",
                 .required = false,
                 .note = "永久模式（始终可见、无遮罩）",
                 .json_type = "boolean"},
            },
        .events = {"on_toggle"},
        .children_policy = "multiple",
        .allowed_child_types = {},
        .examples = {"au::Drawer(content, sidebar, au::DrawerSide::Left, 240)"},
    };
}

auto Drawer::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{.width = 480.0F, .height = 320.0F};
    }
    const float pw = std::min(panel_width_, self.width);

    // 基础内容：永久模式让出面板宽度，否则占满
    if (content_) {
        const float cw = permanent_ ? std::max(0.0F, self.width - pw) : self.width;
        const float cx = (permanent_ && side_ == DrawerSide::Left) ? pw : 0.0F;
        const Constraints cc{.min = Size{.width = cw, .height = self.height},
                             .max = Size{.width = cw, .height = self.height}};
        content_.widget().layout(cc, ctx);
        content_.set_bounds(
            Rect{.origin = Point{.x = cx, .y = 0.0F}, .size = Size{.width = cw, .height = self.height}});
    }
    // 面板（打开或永久时布局）
    if (panel_ && (permanent_ || open_.get())) {
        const Constraints pc{.min = Size{.width = pw, .height = self.height},
                             .max = Size{.width = pw, .height = self.height}};
        panel_.widget().layout(pc, ctx);
        const float px = side_ == DrawerSide::Left ? 0.0F : self.width - pw;
        panel_.set_bounds(Rect{.origin = Point{.x = px, .y = 0.0F}, .size = Size{.width = pw, .height = self.height}});
    }
    return c.constrain(self);
}

auto Drawer::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    if (content_) {
        const Rect cb = content_.bounds();
        content_.widget().paint(
            p,
            Rect{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                 .size = cb.size},
            ctx);
    }
    const bool panel_visible = permanent_ || open_.get();
    if (panel_visible && panel_) {
        // 模态遮罩
        if (!permanent_) {
            p.fill_rect(bounds, Color(0, 0, 0, 96));
        }
        const Rect pb = panel_.bounds();
        const Rect global{.origin = Point{.x = bounds.origin.x + pb.origin.x, .y = bounds.origin.y + pb.origin.y},
                          .size = pb.size};
        p.draw_shadow(global, side_ == DrawerSide::Left ? 2.0F : -2.0F, 0.0F, 8.0F, Color(0, 0, 0, 60));
        p.fill_rect(global, Color(252, 252, 254, 255));
        panel_.widget().paint(p, global, ctx);
    }
}

auto Drawer::on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
    const bool panel_visible = permanent_ || open_.get();
    // 面板优先
    if (panel_visible && panel_) {
        const Rect pb = panel_.bounds();
        if (pb.contains(local)) {
            const Rect global{.origin = Point{.x = bounds.origin.x + pb.origin.x, .y = bounds.origin.y + pb.origin.y},
                              .size = pb.size};
            Widget *r = panel_.widget().hit_test(local - pb.origin, global, ctx);
            if (r != nullptr) {
                return r;
            }
            return this;
        }
        // 模态打开时遮罩命中自身（供关闭）
        if (!permanent_) {
            return this;
        }
    }
    if (content_) {
        const Rect cb = content_.bounds();
        if (cb.contains(local)) {
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            Widget *r = content_.widget().hit_test(local - cb.origin, global, ctx);
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
        .properties =
            {
                {.name = "message",
                 .type = "string",
                 .default_value = "\"\"",
                 .required = false,
                 .note = "Message text",
                 .json_type = "string"},
                {.name = "progress",
                 .type = "float",
                 .default_value = "-1",
                 .required = false,
                 .note = "Progress (0..1, -1=indeterminate)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "-1",
                 .max_value = "1"},
                {.name = "open",
                 .type = "bool",
                 .default_value = "false",
                 .required = false,
                 .note = "Whether to show",
                 .json_type = "boolean"},
                {.name = "cancellable",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "是否可取消",
                 .json_type = "boolean"},
            },
        .events = {"on_cancel"},
        .children_policy = "none",
        .invariants = {"progress >= -1 && progress <= 1"},
        .examples = {"au::ProgressDialog(\"Loading...\", true)"},
    };
}

auto ProgressDialog::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{.width = 400.0F, .height = 300.0F};
    }
    if (!open_) {
        return c.constrain(Size{.width = 0.0F, .height = 0.0F});
    }
    // 对话框盒（居中）
    const float bw = std::min(320.0F, self.width * 0.8F);
    constexpr float bh = 120.0F;
    const float bx = (self.width - bw) * 0.5F;
    const float by = (self.height - bh) * 0.5F;
    box_rect_ = Rect{.origin = Point{.x = bx, .y = by}, .size = Size{.width = bw, .height = bh}};
    cancel_rect_ = Rect{.origin = Point{.x = bx + bw - 90.0F, .y = by + bh - 40.0F},
                        .size = Size{.width = 80.0F, .height = 30.0F}};
    return c.constrain(self);
}

auto ProgressDialog::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    if (!open_) {
        return;
    }
    // 遮罩 + 对话框
    p.fill_rect(bounds, Color(0, 0, 0, 128));
    const Rect box{
        .origin = Point{.x = bounds.origin.x + box_rect_.origin.x, .y = bounds.origin.y + box_rect_.origin.y},
        .size = box_rect_.size};
    p.draw_shadow(box, 0.0F, 4.0F, 12.0F, Color(0, 0, 0, 80));
    p.fill_rect(box, Color(255, 255, 255, 255));

    Font f;
    f.size_pt = 13.0F;
    p.draw_text(Rect{.origin = Point{.x = box.origin.x + 16.0F, .y = box.origin.y + 14.0F},
                     .size = Size{.width = box.size.width - 32.0F, .height = 20.0F}},
                message_, f, Color(30, 30, 30, 255));

    // 进度条
    const Rect track{.origin = Point{.x = box.origin.x + 16.0F, .y = box.origin.y + 46.0F},
                     .size = Size{.width = box.size.width - 32.0F, .height = 8.0F}};
    p.fill_rect(track, Color(230, 230, 234, 255));
    const float prog = progress_.get();
    if (prog >= 0.0F) {
        p.fill_rect(
            Rect{.origin = track.origin, .size = Size{.width = track.size.width * prog, .height = track.size.height}},
            Color(0, 122, 255, 255));
    } else {
        // 不确定态：中段高亮块
        const float seg = track.size.width * 0.3F;
        p.fill_rect(Rect{.origin = Point{.x = track.origin.x + (track.size.width * 0.35F), .y = track.origin.y},
                         .size = Size{.width = seg, .height = track.size.height}},
                    Color(0, 122, 255, 160));
    }

    // 取消按钮
    if (cancellable_) {
        const Rect cbtn{
            .origin = Point{.x = bounds.origin.x + cancel_rect_.origin.x, .y = bounds.origin.y + cancel_rect_.origin.y},
            .size = cancel_rect_.size};
        p.fill_rect(cbtn, Color(240, 240, 244, 255));
        p.draw_rect(cbtn, Color(200, 200, 205, 255));
        p.draw_text(Rect{.origin = Point{.x = cbtn.origin.x + 18.0F, .y = cbtn.origin.y + 7.0F},
                         .size = Size{.width = cbtn.size.width - 20.0F, .height = cbtn.size.height - 14.0F}},
                    "Cancel", f, Color(60, 60, 65, 255));
    }
}

auto ProgressDialog::on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * {
    if (!open_) {
        return nullptr;
    }
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
}

auto PageView::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "PageView",
        .properties =
            {
                {.name = "current",
                 .type = "int",
                 .default_value = "0",
                 .required = false,
                 .note = "当前页码",
                 .json_type = "integer",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "show_indicator",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "是否显示圆点指示器",
                 .json_type = "boolean"},
            },
        .events = {"on_page_change"},
        .children_policy = "multiple",
        .allowed_child_types = {},
        .invariants = {"current >= 0"},
        .examples = {"au::PageView({ page1, page2, page3 })"},
    };
}

auto PageView::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{.width = 320.0F, .height = 240.0F};
    }
    // 仅布局当前页
    const int cur = current_.get();
    if (cur >= 0 && std::cmp_less(cur, children_.size())) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        Node &page = children_[static_cast<std::size_t>(cur)];
        const Constraints pc{.min = Size{.width = self.width, .height = self.height},
                             .max = Size{.width = self.width, .height = self.height}};
        page.widget().layout(pc, ctx);
        page.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
    }
    return c.constrain(self);
}

auto PageView::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const int cur = current_.get();
    if (cur >= 0 && std::cmp_less(cur, children_.size())) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        children_[static_cast<std::size_t>(cur)].widget().paint(p, bounds, ctx);
    }
    // 指示器圆点（底部居中；简化为小方块）
    if (show_indicator_ && children_.size() > 1) {
        constexpr float dot = 8.0F;
        constexpr float gap = 8.0F;
        const float total =
            (static_cast<float>(children_.size()) * dot) + (static_cast<float>(children_.size() - 1) * gap);
        float x = bounds.origin.x + ((bounds.size.width - total) * 0.5F);
        const float y = bounds.origin.y + bounds.size.height - 20.0F;
        for (std::size_t i = 0; i < children_.size(); ++i) {
            const bool active = std::cmp_equal(i, cur);
            p.fill_rect(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = dot, .height = dot}},
                        active ? Color(0, 122, 255, 255) : Color(0, 0, 0, 60));
            x += dot + gap;
        }
    }
}

auto PageView::on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
    const int cur = current_.get();
    if (cur >= 0 && std::cmp_less(cur, children_.size())) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        Node &page = children_[static_cast<std::size_t>(cur)];
        const Rect cb = page.bounds();
        if (cb.contains(local)) {
            Widget *r = page.widget().hit_test(local - cb.origin, bounds, ctx);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
}

}  // namespace aurora