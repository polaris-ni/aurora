#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/environment/environment.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/title_bar_style.h"
#include "aurora/window/window_chrome.h"
#include "aurora/window/window_state.h"

namespace aurora {

/// @brief 标题栏动作项（声明式；渲染为文本按钮，右对齐排列于窗口控制钮左侧）。
struct TitleBarAction {
    std::string label;  ///< 显示文本
    std::function<void()> on_click;  ///< 点击回调（可空）
};

/**
 * @brief 声明式标题栏控件（对标 libadwaita HeaderBar / WinUI TitleBar / Flutter AppBar）。
 *
 * 与 Surface 内置 CSD 栏互补：内置栏服务 DecorationPolicy::ClientSide/Auto 的兜底绘制；
 * 本控件供 Frameless/Borderless 策略下应用自绘完整标题栏（可组合图标/标题/副标题/
 * 动作区 + 内置窗口控制钮），窗口动作经 Environment 注入的 WindowChrome 服务下发
 * （headless 下 chrome 缺失，交互安全 no-op）。
 *
 * 交互：空白区单击拖拽移动（Surface 缓存按键 serial）、双击切换最大化、
 * 控制钮 最小化/最大化还原/关闭；失焦自动变暗。
 * Snap 弹窗（降级版）：悬停最大化钮 ≥400ms 弹出动作菜单——内置
 * 最大化还原/最小化/全屏/关闭 + add_snap_action 追加的自定义项
 * （⚠️ xdg-shell 客户端无法自我定位，真半屏平铺在原生 Wayland 不可实现，
 * 自定义项供应用自行实现可行动作，如经 D-Bus/合成器扩展）。
 *
 * v1 范围决策：leading 为图标位图（非任意 Node）；动作为文本 chip（非任意 Node）——
 * 任意子树槽位留待 v2。视觉仅 Adwaita 单色符号形态（Mac/Windows 三分支由内置栏承担）。
 * 自定义 Snap 动作含回调，不参与 serialize_props 往返。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json（自定义 Snap 动作除外）
 */
class TitleBar : public Widget {
  public:
    TitleBar() = default;

    [[nodiscard]] auto type_name() const -> const char * override { return "TitleBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "TitleBar",
            .properties =
                {
                    {.name = "height",
                     .type = "float",
                     .default_value = "36.0",
                     .required = false,
                     .note = "标题栏高度(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "title",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "标题文本",
                     .json_type = "string"},
                    {.name = "subtitle",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "副标题文本(可选)",
                     .json_type = "string"},
                    {.name = "window_controls",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否渲染内置 最小化/最大化/关闭 钮",
                     .json_type = "boolean"},
                },
            .events = {},
            .children_policy = "none",
            .examples = {R"(au::TitleBar{}.set_title("文档").add_action({"菜单", fn}))"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    // ---- 链式 setter ----
    /// @brief 设置应用图标（左上角槽位；空指针不占位）。
    auto set_icon(std::shared_ptr<Image> icon) -> TitleBar & {
        icon_ = std::move(icon);
        mark_needs_paint();
        return *this;
    }
    auto set_title(std::string t) -> TitleBar & {
        title_ = std::move(t);
        mark_needs_paint();
        return *this;
    }
    auto set_subtitle(std::string s) -> TitleBar & {
        subtitle_ = std::move(s);
        mark_needs_paint();
        return *this;
    }
    /// @brief 追加动作 chip（渲染于控制钮左侧，按追加序从右往左排）。
    auto add_action(TitleBarAction action) -> TitleBar & {
        actions_.push_back(std::move(action));
        mark_needs_layout();
        return *this;
    }
    /// @brief 追加 Snap 弹窗自定义项（排在四个内置项之后；悬停最大化钮触发弹窗）。
    auto add_snap_action(TitleBarAction action) -> TitleBar & {
        snap_actions_.push_back(std::move(action));
        return *this;
    }
    auto set_window_controls(bool on) -> TitleBar & {
        window_controls_ = on;
        mark_needs_layout();
        return *this;
    }
    auto set_height(float h) -> TitleBar & {
        style_.height = h > 0.0F ? h : 36.0F;
        mark_needs_layout();
        return *this;
    }
    auto set_style(const TitleBarStyle &s) -> TitleBar & {
        style_ = s;
        mark_needs_paint();
        return *this;
    }

    [[nodiscard]] auto title() const -> const std::string & { return title_; }
    [[nodiscard]] auto subtitle() const -> const std::string & { return subtitle_; }
    [[nodiscard]] auto window_controls() const -> bool { return window_controls_; }
    [[nodiscard]] auto action_count() const -> std::size_t { return actions_.size(); }
    /// @brief Snap 弹窗是否展开（测试观测点）。
    [[nodiscard]] auto snap_open() const -> bool { return snap_open_; }
    /// @brief 当前 Snap 弹窗条目数（内置 4 + 自定义；未展开时亦反映将弹出数）。
    [[nodiscard]] auto snap_entry_count() const -> std::size_t { return snap_actions_.size() + 4; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["height"] = style_.height;
        props["title"] = title_;
        props["subtitle"] = subtitle_;
        props["window_controls"] = window_controls_;
        props["snap_action_count"] = snap_actions_.size();  // 仅计数：回调不可序列化
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("height")) {
            style_.height = props["height"].get<float>();
        }
        if (props.contains("title")) {
            title_ = props["title"].get<std::string>();
        }
        if (props.contains("subtitle")) {
            subtitle_ = props["subtitle"].get<std::string>();
        }
        if (props.contains("window_controls")) {
            window_controls_ = props["window_controls"].get<bool>();
        }
        mark_needs_layout();
    }

    /// @brief 指针交互：控制钮/动作 chip 命中 → chrome 动作；空白 → 双击最大化或拖拽移动；
    /// Move 驱动最大化钮悬停计时（≥400ms 开 Snap 弹窗）；弹窗展开时点击项执行并收起。
    auto on_pointer_event(MouseEvent &e) -> void override {
        const WindowChrome *chrome = nullptr;
        if (env_ != nullptr) {
            chrome = env_->get<WindowChrome>();
        }

        // ---- Move：悬停跟踪 + 弹窗开合（不消费，继续走基类冒泡）----
        if (e.action == MouseAction::Move) {
            update_snap_hover(e.local_position);
            Widget::on_pointer_event(e);
            return;
        }
        // ---- 弹窗展开时优先处理点击（点项执行 / 点外收起且不透传拖拽）----
        if (snap_open_ && e.action == MouseAction::Press && e.button == MouseButton::Left) {
            for (std::size_t i = 0; i < flyout_item_rects_.size(); ++i) {
                if (hit_rect(flyout_item_rects_[i], e.local_position)) {
                    fire_snap_entry(i);
                    close_snap_flyout();
                    e.is_handled = true;
                    return;
                }
            }
            close_snap_flyout();
            e.is_handled = true;  // 点外仅收起（Windows 行为），不发起拖拽
            return;
        }
        if (e.action != MouseAction::Press || e.button != MouseButton::Left) {
            Widget::on_pointer_event(e);
            return;
        }
        // 1) 窗口控制钮（几何单一来源：与内置栏共用 title_bar_geometry）。
        if (window_controls_ && hit_rect(geom_.close, e.local_position)) {
            if (chrome != nullptr) {
                chrome->close();
            }
            e.is_handled = true;
            return;
        }
        if (window_controls_ && hit_rect(geom_.maximize, e.local_position)) {
            if (chrome != nullptr) {
                chrome->toggle_maximize();
            }
            e.is_handled = true;
            return;
        }
        if (window_controls_ && hit_rect(geom_.minimize, e.local_position)) {
            if (chrome != nullptr) {
                chrome->minimize();
            }
            e.is_handled = true;
            return;
        }
        // 2) 动作 chips（布局期缓存矩形）。
        for (std::size_t i = 0; i < actions_.size() && i < action_rects_.size(); ++i) {
            if (hit_rect(action_rects_[i], e.local_position)) {
                if (actions_[i].on_click) {
                    actions_[i].on_click();
                }
                e.is_handled = true;
                return;
            }
        }
        // 3) 空白区：双击切换最大化（steady_clock 自计时，MouseEvent 无时间戳），
        //    否则发起拖拽移动（须在 Press 派发栈内同步调用——Wayland serial 时效）。
        const auto now = std::chrono::steady_clock::now();
        const bool dbl = last_press_.has_value() &&
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_press_).count() < 300 &&
                         std::hypot(e.local_position.x - last_pos_.x, e.local_position.y - last_pos_.y) < 5.0F;
        last_press_ = now;
        last_pos_ = e.local_position;
        if (dbl) {
            if (chrome != nullptr) {
                chrome->toggle_maximize();
            }
        } else if (chrome != nullptr) {
            chrome->begin_move();
        }
        e.is_handled = true;
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        env_ = ctx.env;  // 事件阶段读取 chrome 用（树存活期内环境链稳定）
        const float w = c.max.is_finite() ? c.max.width : 640.0F;
        const float h = style_.height;

        // 控制钮几何：复用纯函数（热区=绘制，杜绝错位）。resizable 恒 true（控件层无此约束源）。
        const bool maximized =
            env_ != nullptr && env_->get<WindowMode>() != nullptr && *env_->get<WindowMode>() == WindowMode::Maximized;
        geom_ = title_bar_geometry(w, style_, maximized, true);
        if (!window_controls_) {
            geom_.close = Rect{};
            geom_.maximize = Rect{};
            geom_.minimize = Rect{};
        }

        // 动作 chips：自控制钮左侧起向左排（无控制钮则自右缘内边距 12 起）。
        action_rects_.assign(actions_.size(), Rect{});
        float cursor =
            window_controls_ && geom_.minimize.size.width > 0.0F ? geom_.minimize.origin.x - 8.0F : w - 12.0F;
        Font f;
        f.size_pt = 13.0F;
        for (std::size_t i = 0; i < actions_.size(); ++i) {
            const float tw = render::FontEngine::measure_width(actions_[i].label, f) + 16.0F;
            const float x = cursor - tw;
            if (x < 8.0F) {
                break;  // 放不下即止（窄窗退化）
            }
            action_rects_[i] = Rect{.origin = Point{.x = x, .y = 0.0F}, .size = Size{.width = tw, .height = h}};
            cursor = x - 4.0F;
        }
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 失焦变暗（对齐内置栏行为）：Occluded 视为非激活。
        const auto *st = ctx.environment<WindowState>();
        const bool active = st == nullptr || *st == WindowState::Visible;
        const Color bg = active ? style_.bg_active : style_.bg_inactive;
        const Color fg = active ? style_.fg_active : style_.fg_inactive;
        const float h = style_.height;

        p.fill_rect(Rect{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = h}}, bg);

        // 图标槽。
        if (icon_ != nullptr) {
            const float side = std::min(16.0F, h - 20.0F);
            if (side > 0.0F) {
                p.draw_image(*icon_, Rect{.origin = Point{.x = bounds.origin.x + 12.0F,
                                                          .y = bounds.origin.y + ((h - side) * 0.5F)},
                                          .size = Size{.width = side, .height = side}});
            }
        }

        // 标题/副标题两行（无副标题则单行垂直居中）。
        Font tf;
        tf.size_pt = 13.0F;
        tf.weight = 500;
        Font sf;
        sf.size_pt = 11.0F;
        const float text_x = icon_ != nullptr ? bounds.origin.x + 36.0F : bounds.origin.x + 12.0F;
        if (!title_.empty()) {
            if (!subtitle_.empty()) {
                p.draw_text(Rect{.origin = Point{.x = text_x, .y = bounds.origin.y + 4.0F},
                                 .size = Size{.width = 200.0F, .height = h * 0.5F}},
                            title_, tf, fg);
                p.draw_text(Rect{.origin = Point{.x = text_x, .y = bounds.origin.y + (h * 0.5F)},
                                 .size = Size{.width = 200.0F, .height = (h * 0.5F) - 2.0F}},
                            subtitle_, sf, fg);
            } else {
                p.draw_text(Rect{.origin = Point{.x = text_x, .y = bounds.origin.y},
                                 .size = Size{.width = 260.0F, .height = h}},
                            title_, tf, fg);
            }
        }

        // 动作 chips：悬停语义简化为常显文本（v1 无悬停跟踪，记录为 v2 增强）。
        for (std::size_t i = 0; i < actions_.size() && i < action_rects_.size(); ++i) {
            const Rect &r = action_rects_[i];
            if (r.size.width <= 0.0F) {
                continue;
            }
            p.fill_rounded_rect(
                Rect{.origin = Point{.x = bounds.origin.x + r.origin.x, .y = bounds.origin.y + r.origin.y},
                     .size = r.size},
                r.size.height * 0.5F, style_.hover_tint);
            p.draw_text(
                Rect{.origin = Point{.x = bounds.origin.x + r.origin.x + 8.0F, .y = bounds.origin.y + r.origin.y},
                     .size = Size{.width = r.size.width - 16.0F, .height = r.size.height}},
                actions_[i].label, tf, fg);
        }

        // 窗口控制钮：Adwaita 单色符号（− □/▯ ×）。
        if (window_controls_) {
            if (geom_.minimize.size.width > 0.0F) {
                glyph_line(p, geom_.minimize, bounds.origin, fg);
            }
            if (geom_.maximize.size.width > 0.0F) {
                glyph_box(p, geom_.maximize, bounds.origin, fg);
            }
            if (geom_.close.size.width > 0.0F) {
                glyph_cross(p, geom_.close, bounds.origin, Color{255, 255, 255, 235});
            }
        }

        // Snap 弹窗（覆盖绘制于栏下方；展开态由 Move/Press 维护）。
        if (snap_open_) {
            paint_snap_flyout(p, bounds, bg, fg);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        // 命中区 = 标题栏条 + 展开的弹窗面板（弹窗覆盖于内容上方，须拦截其区域点击）。
        if (Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) &&
            ((local.y >= 0.0F && local.y < style_.height) || (snap_open_ && flyout_rect_.contains(local)))) {
            return this;
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> std::vector<HitNode> override {
        return {};  // 自身即最深命中（基类前置 this）
    }

  private:
    [[nodiscard]] static auto hit_rect(const Rect &r, const Point &p) -> bool {
        return r.size.width > 0.0F && p.x >= r.origin.x && p.x < r.origin.x + r.size.width && p.y >= r.origin.y &&
               p.y < r.origin.y + r.size.height;
    }

    /// @brief Snap 条目表：内置 4 项（按当前模式动态文案）+ 自定义追加项。
    [[nodiscard]] auto build_snap_entries() const -> std::vector<TitleBarAction> {
        std::vector<TitleBarAction> out;
        const WindowMode *mode = env_ != nullptr ? env_->get<WindowMode>() : nullptr;
        const bool is_max = mode != nullptr && *mode == WindowMode::Maximized;
        const bool is_fs = mode != nullptr && *mode == WindowMode::FullScreen;
        const WindowChrome *chrome = env_ != nullptr ? env_->get<WindowChrome>() : nullptr;
        // NOLINTNEXTLINE(*-use-trailing-return-type)
        out.push_back(TitleBarAction{.label = is_max ? "还原" : "最大化", .on_click = [this] {
                                         if (const WindowChrome *c =
                                                 env_ != nullptr ? env_->get<WindowChrome>() : nullptr) {
                                             c->toggle_maximize();
                                         }
                                     }});
        (void)chrome;
        // NOLINTNEXTLINE(*-use-trailing-return-type)
        out.push_back(TitleBarAction{.label = "最小化", .on_click = [this] {
                                         if (const WindowChrome *c =
                                                 env_ != nullptr ? env_->get<WindowChrome>() : nullptr) {
                                             c->minimize();
                                         }
                                     }});

        out.push_back(  // NOLINTNEXTLINE(*-use-trailing-return-type)
            TitleBarAction{.label = is_fs ? "退出全屏" : "全屏", .on_click = [this] {
                               if (const WindowChrome *c = env_ != nullptr ? env_->get<WindowChrome>() : nullptr) {
                                   const WindowMode *m = env_ != nullptr ? env_->get<WindowMode>() : nullptr;
                                   c->set_fullscreen(m == nullptr || *m != WindowMode::FullScreen);
                               }
                           }});
        // NOLINTNEXTLINE(*-use-trailing-return-type)
        out.push_back(TitleBarAction{.label = "关闭", .on_click = [this] {
                                         if (const WindowChrome *c =
                                                 env_ != nullptr ? env_->get<WindowChrome>() : nullptr) {
                                             c->close();
                                         }
                                     }});
        for (const auto &a : snap_actions_) {
            out.push_back(a);
        }
        return out;
    }

    /// @brief 展开 Snap 弹窗（锚定最大化钮正下方右对齐）。
    auto open_snap_flyout() -> void {
        constexpr float item_h = 26.0F;
        constexpr float width = 120.0F;
        const auto entries = build_snap_entries();
        const float anchor_x =
            window_controls_ && geom_.maximize.size.width > 0.0F ? geom_.maximize.origin.x : style_.height - width;
        flyout_rect_ = Rect{.origin = Point{.x = anchor_x, .y = style_.height + 4.0F},
                            .size = Size{.width = width, .height = static_cast<float>(entries.size()) * item_h}};
        flyout_item_rects_.clear();
        flyout_labels_.clear();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            flyout_item_rects_.push_back(Rect{
                .origin =
                    Point{.x = flyout_rect_.origin.x, .y = flyout_rect_.origin.y + (static_cast<float>(i) * item_h)},
                .size = Size{.width = width, .height = item_h}});
            flyout_labels_.push_back(entries[i].label);
        }
        snap_open_ = true;
        mark_needs_paint();
    }
    auto close_snap_flyout() -> void {
        if (!snap_open_) {
            return;
        }
        snap_open_ = false;
        max_hover_start_.reset();
        mark_needs_paint();
    }

    /// @brief 执行第 i 个 Snap 条目（与 build_snap_entries 同序重建回调后触发）。
    auto fire_snap_entry(std::size_t i) const -> void {
        const auto entries = build_snap_entries();
        if (i < entries.size() && entries[i].on_click) {
            entries[i].on_click();
        }
    }

    /// @brief Move 驱动：最大化钮悬停 ≥400ms 展开；指针离开「栏+弹窗」区收起。
    auto update_snap_hover(const Point &pos) -> void {
        if (!window_controls_ || geom_.maximize.size.width <= 0.0F) {
            return;
        }
        const bool over_max = hit_rect(geom_.maximize, pos);
        const bool inside = over_max || (pos.y >= 0.0F &&
                                         pos.y < style_.height + (snap_open_ ? flyout_rect_.size.height + 4.0F : 0.0F));
        const auto now = std::chrono::steady_clock::now();
        if (over_max) {
            if (!max_hover_start_.has_value()) {
                {
                    max_hover_start_ = now;
                }
            } else if (!snap_open_ &&
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - *max_hover_start_).count() >= 400) {
                open_snap_flyout();
            }
        } else if (snap_open_ && !inside) {
            close_snap_flyout();
        } else if (!over_max && !snap_open_) {
            max_hover_start_.reset();  // 未开窗即离开：重置计时
        }
    }

    auto paint_snap_flyout(Painter &p, const Rect &bounds, const Color &bg, const Color &fg) const -> void {
        const Rect panel{
            .origin = Point{.x = bounds.origin.x + flyout_rect_.origin.x, .y = bounds.origin.y + flyout_rect_.origin.y},
            .size = flyout_rect_.size};
        p.draw_shadow(panel, 0.0F, 2.0F, 8.0F, Color{0, 0, 0, 48});
        p.fill_rect(panel, bg);
        Font tf;
        tf.size_pt = 12.0F;
        for (std::size_t i = 0; i < flyout_item_rects_.size() && i < flyout_labels_.size(); ++i) {
            const Rect ir{.origin = Point{.x = bounds.origin.x + flyout_item_rects_[i].origin.x,
                                          .y = bounds.origin.y + flyout_item_rects_[i].origin.y},
                          .size = flyout_item_rects_[i].size};
            p.draw_text(Rect{.origin = Point{.x = ir.origin.x + 10.0F, .y = ir.origin.y},
                             .size = Size{.width = ir.size.width - 20.0F, .height = ir.size.height}},
                        flyout_labels_[i], tf, fg);
        }
    }

    static void glyph_line(Painter &p, const Rect &r, const Point &off, const Color &c) {
        const float cx = off.x + r.origin.x + (r.size.width * 0.5F);
        const float cy = off.y + r.origin.y + (r.size.height * 0.5F);
        const float e = r.size.width / 3.0F;
        p.draw_line(Point{.x = cx - e, .y = cy}, Point{.x = cx + e, .y = cy}, 1.5F, c);
    }
    static void glyph_cross(Painter &p, const Rect &r, const Point &off, const Color &c) {
        const float cx = off.x + r.origin.x + (r.size.width * 0.5F);
        const float cy = off.y + r.origin.y + (r.size.height * 0.5F);
        const float e = r.size.width / 3.0F;
        p.draw_line(Point{.x = cx - e, .y = cy - e}, Point{.x = cx + e, .y = cy + e}, 1.5F, c);
        p.draw_line(Point{.x = cx + e, .y = cy - e}, Point{.x = cx - e, .y = cy + e}, 1.5F, c);
    }
    static void glyph_box(Painter &p, const Rect &r, const Point &off, const Color &c) {
        const float cx = off.x + r.origin.x + (r.size.width * 0.5F);
        const float cy = off.y + r.origin.y + (r.size.height * 0.5F);
        const float e = r.size.width / 3.6F;
        p.draw_line(Point{.x = cx - e, .y = cy - e}, Point{.x = cx + e, .y = cy - e}, 1.5F, c);
        p.draw_line(Point{.x = cx + e, .y = cy - e}, Point{.x = cx + e, .y = cy + e}, 1.5F, c);
        p.draw_line(Point{.x = cx + e, .y = cy + e}, Point{.x = cx - e, .y = cy + e}, 1.5F, c);
        p.draw_line(Point{.x = cx - e, .y = cy + e}, Point{.x = cx - e, .y = cy - e}, 1.5F, c);
    }

    std::shared_ptr<Image> icon_;
    std::string title_;
    std::string subtitle_;
    std::vector<TitleBarAction> actions_;
    std::vector<TitleBarAction> snap_actions_;  ///< Snap 弹窗自定义追加项
    bool window_controls_ = true;
    TitleBarStyle style_{};  ///< 默认 adwaita_dark（含 height=36）
    TitleBarGeometry geom_{};  ///< 布局期缓存（paint/hit 共用）
    std::vector<Rect> action_rects_;  ///< 布局期缓存
    const Environment *env_ = nullptr;  ///< 事件阶段读 WindowChrome（on_layout 时刷新）
    std::optional<std::chrono::steady_clock::time_point> last_press_;
    Point last_pos_{.x = 0.0F, .y = 0.0F};
    // ---- Snap 弹窗状态 ----
    bool snap_open_ = false;
    std::optional<std::chrono::steady_clock::time_point> max_hover_start_;
    Rect flyout_rect_{};  ///< 弹窗面板（本控件局部坐标）
    std::vector<Rect> flyout_item_rects_;  ///< 条目矩形缓存
    std::vector<std::string> flyout_labels_;  ///< 条目文案缓存
};

}  // namespace aurora
