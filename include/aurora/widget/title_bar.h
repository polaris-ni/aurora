#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "aurora/core/image.h"
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
    std::string label;              ///< 显示文本
    std::function<void()> on_click; ///< 点击回调（可空）
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
            .properties = {
                { .name = "height", .type = "float", .default_value = "36.0", .required = false, .note = "标题栏高度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "title", .type = "string", .default_value = "", .required = false, .note = "标题文本", .json_type = "string" },
                { .name = "subtitle", .type = "string", .default_value = "", .required = false, .note = "副标题文本(可选)", .json_type = "string" },
                { .name = "window_controls", .type = "bool", .default_value = "true", .required = false, .note = "是否渲染内置 最小化/最大化/关闭 钮", .json_type = "boolean" },
            },
            .events = {},
            .children_policy = "none",
            .examples = { R"(au::TitleBar{}.set_title("文档").add_action({"菜单", fn}))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    // ---- 链式 setter ----
    /// @brief 设置应用图标（左上角槽位；空指针不占位）。
    auto set_icon(std::shared_ptr<Image> icon) -> TitleBar & {
        m_icon = std::move(icon);
        mark_needs_paint();
        return *this;
    }
    auto set_title(std::string t) -> TitleBar & {
        m_title = std::move(t);
        mark_needs_paint();
        return *this;
    }
    auto set_subtitle(std::string s) -> TitleBar & {
        m_subtitle = std::move(s);
        mark_needs_paint();
        return *this;
    }
    /// @brief 追加动作 chip（渲染于控制钮左侧，按追加序从右往左排）。
    auto add_action(TitleBarAction action) -> TitleBar & {
        m_actions.push_back(std::move(action));
        mark_needs_layout();
        return *this;
    }
    /// @brief 追加 Snap 弹窗自定义项（排在四个内置项之后；悬停最大化钮触发弹窗）。
    auto add_snap_action(TitleBarAction action) -> TitleBar & {
        m_snap_actions.push_back(std::move(action));
        return *this;
    }
    auto set_window_controls(bool on) -> TitleBar & {
        m_window_controls = on;
        mark_needs_layout();
        return *this;
    }
    auto set_height(float h) -> TitleBar & {
        m_style.height = h > 0.0f ? h : 36.0f;
        mark_needs_layout();
        return *this;
    }
    auto set_style(const TitleBarStyle &s) -> TitleBar & {
        m_style = s;
        mark_needs_paint();
        return *this;
    }

    [[nodiscard]] auto title() const -> const std::string & { return m_title; }
    [[nodiscard]] auto subtitle() const -> const std::string & { return m_subtitle; }
    [[nodiscard]] auto window_controls() const -> bool { return m_window_controls; }
    [[nodiscard]] auto action_count() const -> std::size_t { return m_actions.size(); }
    /// @brief Snap 弹窗是否展开（测试观测点）。
    [[nodiscard]] auto snap_open() const -> bool { return m_snap_open; }
    /// @brief 当前 Snap 弹窗条目数（内置 4 + 自定义；未展开时亦反映将弹出数）。
    [[nodiscard]] auto snap_entry_count() const -> std::size_t { return m_snap_actions.size() + 4; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["height"] = m_style.height;
        props["title"] = m_title;
        props["subtitle"] = m_subtitle;
        props["window_controls"] = m_window_controls;
        props["snap_action_count"] = m_snap_actions.size(); // 仅计数：回调不可序列化
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("height")) {
            m_style.height = props["height"].get<float>();
        }
        if (props.contains("title")) {
            m_title = props["title"].get<std::string>();
        }
        if (props.contains("subtitle")) {
            m_subtitle = props["subtitle"].get<std::string>();
        }
        if (props.contains("window_controls")) {
            m_window_controls = props["window_controls"].get<bool>();
        }
        mark_needs_layout();
    }

    /// @brief 指针交互：控制钮/动作 chip 命中 → chrome 动作；空白 → 双击最大化或拖拽移动；
    /// Move 驱动最大化钮悬停计时（≥400ms 开 Snap 弹窗）；弹窗展开时点击项执行并收起。
    auto on_pointer_event(MouseEvent &e) -> void override { // NOLINT(*-function-cognitive-complexity)
        const WindowChrome *chrome = nullptr;
        if (m_env != nullptr) {
            chrome = m_env->get<WindowChrome>();
        }

        // ---- Move：悬停跟踪 + 弹窗开合（不消费，继续走基类冒泡）----
        if (e.action == MouseAction::Move) {
            update_snap_hover(e.local_position);
            Widget::on_pointer_event(e);
            return;
        }
        // ---- 弹窗展开时优先处理点击（点项执行 / 点外收起且不透传拖拽）----
        if (m_snap_open && e.action == MouseAction::Press && e.button == MouseButton::Left) {
            for (std::size_t i = 0; i < m_flyout_item_rects.size(); ++i) {
                if (hit_rect(m_flyout_item_rects[i], e.local_position)) {
                    fire_snap_entry(i);
                    close_snap_flyout();
                    e.handled = true;
                    return;
                }
            }
            close_snap_flyout();
            e.handled = true; // 点外仅收起（Windows 行为），不发起拖拽
            return;
        }
        if (e.action != MouseAction::Press || e.button != MouseButton::Left) {
            Widget::on_pointer_event(e);
            return;
        }
        // 1) 窗口控制钮（几何单一来源：与内置栏共用 title_bar_geometry）。
        if (m_window_controls && hit_rect(m_geom.close, e.local_position)) {
            if (chrome != nullptr) {
                chrome->close();
            }
            e.handled = true;
            return;
        }
        if (m_window_controls && hit_rect(m_geom.maximize, e.local_position)) {
            if (chrome != nullptr) {
                chrome->toggle_maximize();
            }
            e.handled = true;
            return;
        }
        if (m_window_controls && hit_rect(m_geom.minimize, e.local_position)) {
            if (chrome != nullptr) {
                chrome->minimize();
            }
            e.handled = true;
            return;
        }
        // 2) 动作 chips（布局期缓存矩形）。
        for (std::size_t i = 0; i < m_actions.size() && i < m_action_rects.size(); ++i) {
            if (hit_rect(m_action_rects[i], e.local_position)) {
                if (m_actions[i].on_click) {
                    m_actions[i].on_click();
                }
                e.handled = true;
                return;
            }
        }
        // 3) 空白区：双击切换最大化（steady_clock 自计时，MouseEvent 无时间戳），
        //    否则发起拖拽移动（须在 Press 派发栈内同步调用——Wayland serial 时效）。
        const auto now = std::chrono::steady_clock::now();
        const bool dbl = m_last_press.has_value() &&
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - *m_last_press).count() < 300 &&
                         std::hypot(e.local_position.x - m_last_pos.x, e.local_position.y - m_last_pos.y) < 5.0f;
        m_last_press = now;
        m_last_pos = e.local_position;
        if (dbl) {
            if (chrome != nullptr) {
                chrome->toggle_maximize();
            }
        } else if (chrome != nullptr) {
            chrome->begin_move();
        }
        e.handled = true;
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        m_env = ctx.env; // 事件阶段读取 chrome 用（树存活期内环境链稳定）
        const float w = c.max.is_finite() ? c.max.width : 640.0f;
        const float h = m_style.height;

        // 控制钮几何：复用纯函数（热区=绘制，杜绝错位）。resizable 恒 true（控件层无此约束源）。
        const bool maximized = m_env != nullptr && m_env->get<WindowMode>() != nullptr &&
                               *m_env->get<WindowMode>() == WindowMode::Maximized;
        m_geom = title_bar_geometry(w, m_style, maximized, true);
        if (!m_window_controls) {
            m_geom.close = Rect{};
            m_geom.maximize = Rect{};
            m_geom.minimize = Rect{};
        }

        // 动作 chips：自控制钮左侧起向左排（无控制钮则自右缘内边距 12 起）。
        m_action_rects.assign(m_actions.size(), Rect{});
        float cursor =
            m_window_controls && m_geom.minimize.size.width > 0.0f ? m_geom.minimize.origin.x - 8.0f : w - 12.0f;
        Font f;
        f.size_pt = 13.0f;
        for (std::size_t i = 0; i < m_actions.size(); ++i) {
            const float tw = render::FontEngine::measure_width(m_actions[i].label, f) + 16.0f;
            const float x = cursor - tw;
            if (x < 8.0f) {
                break; // 放不下即止（窄窗退化）
            }
            m_action_rects[i] = Rect{ .origin = Point{ .x = x, .y = 0.0f }, .size = Size{ .width = tw, .height = h } };
            cursor = x - 4.0f;
        }
        return c.constrain(Size{ .width = w, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 失焦变暗（对齐内置栏行为）：Occluded 视为非激活。
        const auto *st = ctx.environment<WindowState>();
        const bool active = st == nullptr || *st == WindowState::Visible;
        const Color bg = active ? m_style.bg_active : m_style.bg_inactive;
        const Color fg = active ? m_style.fg_active : m_style.fg_inactive;
        const float h = m_style.height;

        p.fill_rect(Rect{ .origin = bounds.origin, .size = Size{ .width = bounds.size.width, .height = h } }, bg);

        // 图标槽。
        if (m_icon != nullptr) {
            const float side = std::min(16.0f, h - 20.0f);
            if (side > 0.0f) {
                p.draw_image(*m_icon, Rect{ .origin = Point{ .x = bounds.origin.x + 12.0f,
                                                             .y = bounds.origin.y + ((h - side) * 0.5f) },
                                            .size = Size{ .width = side, .height = side } });
            }
        }

        // 标题/副标题两行（无副标题则单行垂直居中）。
        Font tf;
        tf.size_pt = 13.0f;
        tf.weight = 500;
        Font sf;
        sf.size_pt = 11.0f;
        const float text_x = m_icon != nullptr ? bounds.origin.x + 36.0f : bounds.origin.x + 12.0f;
        if (!m_title.empty()) {
            if (!m_subtitle.empty()) {
                p.draw_text(Rect{ .origin = Point{ .x = text_x, .y = bounds.origin.y + 4.0f },
                                  .size = Size{ .width = 200.0f, .height = h * 0.5f } },
                            m_title, tf, fg);
                p.draw_text(Rect{ .origin = Point{ .x = text_x, .y = bounds.origin.y + (h * 0.5f) },
                                  .size = Size{ .width = 200.0f, .height = (h * 0.5f) - 2.0f } },
                            m_subtitle, sf, fg);
            } else {
                p.draw_text(Rect{ .origin = Point{ .x = text_x, .y = bounds.origin.y },
                                  .size = Size{ .width = 260.0f, .height = h } },
                            m_title, tf, fg);
            }
        }

        // 动作 chips：悬停语义简化为常显文本（v1 无悬停跟踪，记录为 v2 增强）。
        for (std::size_t i = 0; i < m_actions.size() && i < m_action_rects.size(); ++i) {
            const Rect &r = m_action_rects[i];
            if (r.size.width <= 0.0f) {
                continue;
            }
            p.fill_rounded_rect(
                Rect{ .origin = Point{ .x = bounds.origin.x + r.origin.x, .y = bounds.origin.y + r.origin.y },
                      .size = r.size },
                r.size.height * 0.5f, m_style.hover_tint);
            p.draw_text(
                Rect{ .origin = Point{ .x = bounds.origin.x + r.origin.x + 8.0f, .y = bounds.origin.y + r.origin.y },
                      .size = Size{ .width = r.size.width - 16.0f, .height = r.size.height } },
                m_actions[i].label, tf, fg);
        }

        // 窗口控制钮：Adwaita 单色符号（− □/▯ ×）。
        if (m_window_controls) {
            if (m_geom.minimize.size.width > 0.0f) {
                glyph_line(p, m_geom.minimize, bounds.origin, fg);
            }
            if (m_geom.maximize.size.width > 0.0f) {
                glyph_box(p, m_geom.maximize, bounds.origin, fg);
            }
            if (m_geom.close.size.width > 0.0f) {
                glyph_cross(p, m_geom.close, bounds.origin, Color{ 255, 255, 255, 235 });
            }
        }

        // Snap 弹窗（覆盖绘制于栏下方；展开态由 Move/Press 维护）。
        if (m_snap_open) {
            paint_snap_flyout(p, bounds, bg, fg);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        // 命中区 = 标题栏条 + 展开的弹窗面板（弹窗覆盖于内容上方，须拦截其区域点击）。
        if (Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) &&
            ((local.y >= 0.0f && local.y < m_style.height) || (m_snap_open && m_flyout_rect.contains(local)))) {
            return this;
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> std::vector<HitNode> override {
        return {}; // 自身即最深命中（基类前置 this）
    }

  private:
    [[nodiscard]] static auto hit_rect(const Rect &r, const Point &p) -> bool {
        return r.size.width > 0.0f && p.x >= r.origin.x && p.x < r.origin.x + r.size.width && p.y >= r.origin.y &&
               p.y < r.origin.y + r.size.height;
    }

    /// @brief Snap 条目表：内置 4 项（按当前模式动态文案）+ 自定义追加项。
    [[nodiscard]] auto build_snap_entries() const -> std::vector<TitleBarAction> {
        std::vector<TitleBarAction> out;
        const WindowMode *mode = m_env != nullptr ? m_env->get<WindowMode>() : nullptr;
        const bool is_max = mode != nullptr && *mode == WindowMode::Maximized;
        const bool is_fs = mode != nullptr && *mode == WindowMode::FullScreen;
        const WindowChrome *chrome = m_env != nullptr ? m_env->get<WindowChrome>() : nullptr;
        // NOLINTNEXTLINE(*-use-trailing-return-type)
        out.push_back(TitleBarAction{ .label = is_max ? "还原" : "最大化", .on_click = [this] {
                                         if (const WindowChrome *c =
                                                 m_env != nullptr ? m_env->get<WindowChrome>() : nullptr) {
                                             c->toggle_maximize();
                                         }
                                     } });
        (void)chrome;
        // NOLINTNEXTLINE(*-use-trailing-return-type)
        out.push_back(TitleBarAction{ .label = "最小化", .on_click = [this] {
                                         if (const WindowChrome *c =
                                                 m_env != nullptr ? m_env->get<WindowChrome>() : nullptr) {
                                             c->minimize();
                                         }
                                     } });

        out.push_back( // NOLINTNEXTLINE(*-use-trailing-return-type)
            TitleBarAction{ .label = is_fs ? "退出全屏" : "全屏", .on_click = [this] {
                               if (const WindowChrome *c = m_env != nullptr ? m_env->get<WindowChrome>() : nullptr) {
                                   const WindowMode *m = m_env != nullptr ? m_env->get<WindowMode>() : nullptr;
                                   c->set_fullscreen(m == nullptr || *m != WindowMode::FullScreen);
                               }
                           } });
        // NOLINTNEXTLINE(*-use-trailing-return-type)
        out.push_back(TitleBarAction{ .label = "关闭", .on_click = [this] {
                                         if (const WindowChrome *c =
                                                 m_env != nullptr ? m_env->get<WindowChrome>() : nullptr) {
                                             c->close();
                                         }
                                     } });
        for (const auto &a : m_snap_actions) {
            out.push_back(a);
        }
        return out;
    }

    /// @brief 展开 Snap 弹窗（锚定最大化钮正下方右对齐）。
    auto open_snap_flyout() -> void {
        constexpr float item_h = 26.0f;
        constexpr float width = 120.0f;
        const auto entries = build_snap_entries();
        const float anchor_x =
            m_window_controls && m_geom.maximize.size.width > 0.0f ? m_geom.maximize.origin.x : m_style.height - width;
        m_flyout_rect = Rect{ .origin = Point{ .x = anchor_x, .y = m_style.height + 4.0f },
                              .size = Size{ .width = width, .height = static_cast<float>(entries.size()) * item_h } };
        m_flyout_item_rects.clear();
        m_flyout_labels.clear();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            m_flyout_item_rects.push_back(
                Rect{ .origin = Point{ .x = m_flyout_rect.origin.x,
                                       .y = m_flyout_rect.origin.y + (static_cast<float>(i) * item_h) },
                      .size = Size{ .width = width, .height = item_h } });
            m_flyout_labels.push_back(entries[i].label);
        }
        m_snap_open = true;
        mark_needs_paint();
    }
    auto close_snap_flyout() -> void {
        if (!m_snap_open) {
            return;
        }
        m_snap_open = false;
        m_max_hover_start.reset();
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
        if (!m_window_controls || m_geom.maximize.size.width <= 0.0f) {
            return;
        }
        const bool over_max = hit_rect(m_geom.maximize, pos);
        const bool inside =
            over_max ||
            (pos.y >= 0.0f && pos.y < m_style.height + (m_snap_open ? m_flyout_rect.size.height + 4.0f : 0.0f));
        const auto now = std::chrono::steady_clock::now();
        if (over_max) {
            if (!m_max_hover_start.has_value()) {
                {
                    m_max_hover_start = now;
                }
            } else if (!m_snap_open &&
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - *m_max_hover_start).count() >= 400) {
                open_snap_flyout();
            }
        } else if (m_snap_open && !inside) {
            close_snap_flyout();
        } else if (!over_max && !m_snap_open) {
            m_max_hover_start.reset(); // 未开窗即离开：重置计时
        }
    }

    auto paint_snap_flyout(Painter &p, const Rect &bounds, const Color &bg, const Color &fg) const -> void {
        const Rect panel{ .origin = Point{ .x = bounds.origin.x + m_flyout_rect.origin.x,
                                           .y = bounds.origin.y + m_flyout_rect.origin.y },
                          .size = m_flyout_rect.size };
        p.draw_shadow(panel, 0.0f, 2.0f, 8.0f, Color{ 0, 0, 0, 48 });
        p.fill_rect(panel, bg);
        Font tf;
        tf.size_pt = 12.0f;
        for (std::size_t i = 0; i < m_flyout_item_rects.size() && i < m_flyout_labels.size(); ++i) {
            const Rect ir{ .origin = Point{ .x = bounds.origin.x + m_flyout_item_rects[i].origin.x,
                                            .y = bounds.origin.y + m_flyout_item_rects[i].origin.y },
                           .size = m_flyout_item_rects[i].size };
            p.draw_text(Rect{ .origin = Point{ .x = ir.origin.x + 10.0f, .y = ir.origin.y },
                              .size = Size{ .width = ir.size.width - 20.0f, .height = ir.size.height } },
                        m_flyout_labels[i], tf, fg);
        }
    }

    static void glyph_line(Painter &p, const Rect &r, const Point &off, const Color &c) {
        const float cx = off.x + r.origin.x + (r.size.width * 0.5f);
        const float cy = off.y + r.origin.y + (r.size.height * 0.5f);
        const float e = r.size.width / 3.0f;
        p.draw_line(Point{ .x = cx - e, .y = cy }, Point{ .x = cx + e, .y = cy }, 1.5f, c);
    }
    static void glyph_cross(Painter &p, const Rect &r, const Point &off, const Color &c) {
        const float cx = off.x + r.origin.x + (r.size.width * 0.5f);
        const float cy = off.y + r.origin.y + (r.size.height * 0.5f);
        const float e = r.size.width / 3.0f;
        p.draw_line(Point{ .x = cx - e, .y = cy - e }, Point{ .x = cx + e, .y = cy + e }, 1.5f, c);
        p.draw_line(Point{ .x = cx + e, .y = cy - e }, Point{ .x = cx - e, .y = cy + e }, 1.5f, c);
    }
    static void glyph_box(Painter &p, const Rect &r, const Point &off, const Color &c) {
        const float cx = off.x + r.origin.x + (r.size.width * 0.5f);
        const float cy = off.y + r.origin.y + (r.size.height * 0.5f);
        const float e = r.size.width / 3.6f;
        p.draw_line(Point{ .x = cx - e, .y = cy - e }, Point{ .x = cx + e, .y = cy - e }, 1.5f, c);
        p.draw_line(Point{ .x = cx + e, .y = cy - e }, Point{ .x = cx + e, .y = cy + e }, 1.5f, c);
        p.draw_line(Point{ .x = cx + e, .y = cy + e }, Point{ .x = cx - e, .y = cy + e }, 1.5f, c);
        p.draw_line(Point{ .x = cx - e, .y = cy + e }, Point{ .x = cx - e, .y = cy - e }, 1.5f, c);
    }

    std::shared_ptr<Image> m_icon;
    std::string m_title;
    std::string m_subtitle;
    std::vector<TitleBarAction> m_actions;
    std::vector<TitleBarAction> m_snap_actions; ///< Snap 弹窗自定义追加项
    bool m_window_controls = true;
    TitleBarStyle m_style{};            ///< 默认 adwaita_dark（含 height=36）
    TitleBarGeometry m_geom{};          ///< 布局期缓存（paint/hit 共用）
    std::vector<Rect> m_action_rects;   ///< 布局期缓存
    const Environment *m_env = nullptr; ///< 事件阶段读 WindowChrome（on_layout 时刷新）
    std::optional<std::chrono::steady_clock::time_point> m_last_press;
    Point m_last_pos{ .x = 0.0f, .y = 0.0f };
    // ---- Snap 弹窗状态 ----
    bool m_snap_open = false;
    std::optional<std::chrono::steady_clock::time_point> m_max_hover_start;
    Rect m_flyout_rect{};                     ///< 弹窗面板（本控件局部坐标）
    std::vector<Rect> m_flyout_item_rects;    ///< 条目矩形缓存
    std::vector<std::string> m_flyout_labels; ///< 条目文案缓存
};

} // namespace aurora
