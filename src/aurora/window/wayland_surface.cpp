#include "aurora/window/wayland_surface.h"

#include "aurora/core/platform.h"
#include "aurora/window/title_bar_geometry.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/keysym_map.h"
#include "aurora/window/swizzle.h"
#include "aurora/window/window_state.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace aurora {
namespace {
/// @brief xkb keysym → 平台无关 KeyCode（Wayland 后端入口；映射逻辑见 detail::keysym_to_keycode）。
auto from_xkb_keysym(xkb_keysym_t ks) -> KeyCode { return detail::keysym_to_keycode(static_cast<unsigned long>(ks)); }
}  // namespace

/// @brief WaylandSurface 的全部平台状态（pimpl）：公共头零 Wayland 依赖。
/// 嵌套类型可访问外围类 protected 成员（notify_window_state/m_present_request），
/// C 回调经 static thunk 转发到本结构的成员函数。
struct WaylandSurface::Impl {
    WaylandSurface *self = nullptr;  ///< 反向指针：listener 内上抛 notify_*/m_present_request。
    // 核心 globals（registry 绑定）。
    wl_display *dpy = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    std::uint32_t compositor_version = 0;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    xdg_wm_base *wm_base = nullptr;
    zxdg_decoration_manager_v1 *deco_mgr = nullptr;
    // 窗口壳。
    wl_surface *surface = nullptr;
    xdg_surface *xsurface = nullptr;
    xdg_toplevel *toplevel = nullptr;
    zxdg_toplevel_decoration_v1 *deco = nullptr;
    // 输入设备。
    wl_pointer *pointer = nullptr;
    wl_keyboard *keyboard = nullptr;

    // 输出与缩放（wl_output scale 事件 + wl_surface enter 关联）。
    struct OutputInfo {
        wl_output *out = nullptr;
        int scale = 1;
    };

    std::vector<OutputInfo> outputs;
    // xkbcommon 键盘状态。
    xkb_context *xkb_ctx = nullptr;
    xkb_keymap *keymap = nullptr;
    xkb_state *xkb_st = nullptr;
    ModifierKey mods = ModifierKey::None;

    // wl_shm 双缓冲槽：attach 后 buffer 归合成器（busy），release 事件归还。
    struct Slot {
        wl_buffer *buf = nullptr;
        std::uint32_t *px = nullptr;
        std::size_t bytes = 0;
        int w = 0;
        int h = 0;
        bool busy = false;
    };

    Slot slots[2];

    Painter painter;
    std::vector<Rect> present_dirty;  ///< 本帧增量 damage 脏区（设备坐标；空=全量）。
    Size size{0.0F, 0.0F};  ///< 逻辑 dp（Wayland 表面坐标即逻辑坐标）。
    int scale = 1;
    bool configured = false;  ///< 收到首个 xdg_surface.configure 前不得 attach buffer。
    // xdg_toplevel.configure 暂存（ack 于 xdg_surface.configure 时统一应用）。
    std::int32_t pending_w = 0;
    std::int32_t pending_h = 0;
    bool pending_max = false;
    bool pending_fs = false;
    bool pending_susp = false;
    bool pending_act = true;
    bool close_requested = false;
    bool active = true;
    bool minimized = false;
    WindowState state = WindowState::Visible;
    WindowMode mode = WindowMode::Normal;
    Surface::EventHandler handler;
    // 自唤醒管道（request_wake → wait_events poll 立即返回）。
    int wake_fd[2] = {-1, -1};
    // 指针表面坐标（逻辑 px；Wayland 事件坐标天然为表面坐标，无需除 scale）。
    double ptr_x = 0.0;
    double ptr_y = 0.0;
    // 装饰策略解析结果（见 WindowStyleOptions::decoration / DecorationPolicy）。
    // 由 deco_mgr（合成器是否支持 xdg-decoration）与 style.decoration 共同决定：
    // - csd_title：绘制自绘标题栏（含关闭按钮），并提供标题栏拖拽移动。
    // - csd_border：绘制可拖拽缩放边框，并提供边缘拖拽缩放。
    // - mod_move：无标题栏时（Borderless/Frameless），按住修饰键（Super/Alt）拖拽任意处移动窗口。
    // 三者组合覆盖了 Auto/ServerSide/ClientSide/Borderless/Frameless 全部策略，
    // 确保「无标题栏也能移动/缩放/关闭」：KDE（SSD）全 false；GNOME（无 SSD）走 CSD 兜底。
    DecorationPolicy deco_policy = DecorationPolicy::Auto;
    bool csd_title = false;  ///< 自绘标题栏（移动 + 关闭按钮）
    bool csd_border = false;  ///< 自绘可缩放边框
    bool mod_move = false;  ///< 修饰键拖拽移动（无标题栏时）
    std::string title;  ///< 标题（用于 CSD 标题栏文字）
    TitleBarStyle tb_style{};  ///< CSD 标题栏样式（构造自 WindowStyleOptions::title_bar；运行期可热更）
    std::shared_ptr<Image> tb_icon;  ///< CSD 标题栏图标（shared_ptr 共享像素；set_title_bar_icon 存入）
    bool resizable = true;  ///< 可调大小（false = 固定尺寸，最大化按钮隐藏）
    int hovered_btn = -1;  ///< 当前悬停的标题栏按钮索引（-1 = 无悬停）
    bool fs_bar_revealed = false;  ///< 全屏揭示条是否展开（覆盖层语义，不回流布局）
    int border = 6;  ///< 可拖拽缩放边框厚度（逻辑 px）
    bool csd_grab = false;  ///< 当前是否处于 CSD/修饰键拖拽交互中（吞噬指针事件）
    std::uint32_t last_press_serial = 0;  ///< 最近按键 serial：控件经 begin_window_move/resize 同步调用时有效
    // 双击标题栏最大化检测（Wayland 不提供双击事件，客户端自行追踪）。
    std::uint32_t last_click_time = 0;  ///< 上次标题栏点击时间（ms，自某基准）
    double last_click_x = 0.0;  ///< 上次点击 X
    double last_click_y = 0.0;  ///< 上次点击 Y

    auto update_state() -> void {
        const WindowState want = compute_window_state(minimized, active);
        if (want != state) {
            state = want;
            self->notify_window_state(want);
        }
    }

    auto send_mouse(MouseAction action, MouseButton button, float lx, float ly) const -> void {
        if (!handler) {
            return;
        }
        MouseEvent e;
        e.action = action;
        e.button = button;
        e.position = Point{lx, ly};
        handler(e);
    }

    auto refresh_scale() -> void {
        // 简化模型：取所有输出的最大缩放（map 前 surface 尚未 enter 任何输出，
        // 以最大值渲染可避免高 DPI 屏首帧模糊；enter 后如有变化再重渲染）。
        int want = 1;
        for (const OutputInfo &o : outputs) {
            want = std::max(want, o.scale);
        }
        if (compositor_version < 3U) {
            want = 1;  // set_buffer_scale 需 wl_surface v3：不支持则退化 1x
        }
        if (want != scale) {
            scale = want;
            if (self->present_request_) {
                self->present_request_();
            }
        }
    }

    static auto release_slot(Slot &s) -> void {
        if (s.buf != nullptr) {
            wl_buffer_destroy(s.buf);
            s.buf = nullptr;
        }
        if (s.px != nullptr) {
            munmap(s.px, s.bytes);
            s.px = nullptr;
        }
        s.bytes = 0;
        s.w = 0;
        s.h = 0;
        s.busy = false;
    }

    auto ensure_slot(Slot &s, int w, int h) const -> bool;
    auto pick_slot(int w, int h) -> Slot *;

    // ---- 协议事件处理（static thunk → 成员函数） ----
    auto on_global(std::uint32_t name, const char *iface, std::uint32_t version) -> void;
    auto on_xdg_surface_configure(std::uint32_t serial) -> void;
    auto on_toplevel_configure(std::int32_t w, std::int32_t h, wl_array *states) -> void;
    auto on_seat_capabilities(std::uint32_t caps) -> void;
    auto on_key(std::uint32_t key, std::uint32_t state_v) const -> void;
    auto on_keymap(std::int32_t fd, std::uint32_t sz) -> void;
    auto on_modifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked, std::uint32_t group)
        -> void;
    /// 请求立即重绘（嵌套类可访基类 protected 的 m_present_request；供匿名空间自由函数复用）。
    auto request_repaint() -> void {
        if (self != nullptr && self->present_request_) {
            self->present_request_();
        }
    }
    auto draw_decoration(Painter &p) const -> void;  ///< 自绘装饰：标题栏（csd_title）+ 边框（csd_border）
};

namespace {
using Impl = WaylandSurface::Impl;

// ---- wl_buffer：release = 合成器归还缓冲（槽复用）。 ----
void buf_release(void *data, wl_buffer * /*b*/) { static_cast<Impl::Slot *>(data)->busy = false; }
constexpr wl_buffer_listener BUFFER_LISTENER = {buf_release};

// ---- xdg_wm_base：ping/pong 保活（不回应会被合成器判定无响应）。 ----
void wm_ping(void * /*data*/, xdg_wm_base *wb, std::uint32_t serial) { xdg_wm_base_pong(wb, serial); }
constexpr xdg_wm_base_listener WM_BASE_LISTENER = {wm_ping};

// ---- xdg_surface / xdg_toplevel：configure 驱动尺寸与几何态。 ----
void xs_configure(void *data, xdg_surface * /*xs*/, std::uint32_t serial) {
    static_cast<Impl *>(data)->on_xdg_surface_configure(serial);
}

constexpr xdg_surface_listener XDG_SURFACE_LISTENER = {xs_configure};

void tl_configure(void *data, xdg_toplevel * /*tl*/, std::int32_t w, std::int32_t h, wl_array *states) {
    static_cast<Impl *>(data)->on_toplevel_configure(w, h, states);
}

void tl_close(void *data, xdg_toplevel * /*tl*/) { static_cast<Impl *>(data)->close_requested = true; }

void tl_bounds(void * /*data*/, xdg_toplevel * /*tl*/, std::int32_t /*w*/, std::int32_t /*h*/) {}

void tl_caps(void * /*data*/, xdg_toplevel * /*tl*/, wl_array * /*caps*/) {}

constexpr xdg_toplevel_listener TOP_LEVEL_LISTENER = {tl_configure, tl_close, tl_bounds, tl_caps};

// ---- wl_pointer：进入/离开/移动/按键/滚轮 → MouseEvent/ScrollEvent。 ----
void ptr_enter(void *data, wl_pointer * /*p*/, std::uint32_t /*serial*/, wl_surface * /*s*/, wl_fixed_t sx,
               wl_fixed_t sy) {
    Impl &d = *static_cast<Impl *>(data);
    d.ptr_x = wl_fixed_to_double(sx);
    d.ptr_y = wl_fixed_to_double(sy);
    d.send_mouse(MouseAction::Move, MouseButton::Left, static_cast<float>(d.ptr_x), static_cast<float>(d.ptr_y));
}

void ptr_leave(void *data, wl_pointer * /*p*/, std::uint32_t /*serial*/, wl_surface * /*s*/) {
    // 光标离开窗口：合成一次远离窗口的 Move → 清除全部悬停态（对齐 WM_MOUSELEAVE / X11 LeaveNotify）。
    static_cast<Impl *>(data)->send_mouse(MouseAction::Move, MouseButton::Left, -10000.0F, -10000.0F);
}

void ptr_motion(void *data, wl_pointer * /*p*/, std::uint32_t /*time*/, wl_fixed_t sx, wl_fixed_t sy) {
    Impl &d = *static_cast<Impl *>(data);
    d.ptr_x = wl_fixed_to_double(sx);
    d.ptr_y = wl_fixed_to_double(sy);
    // ── CSD 悬停跟踪 + 全屏顶边揭示（仅驱动装饰重绘，不吞 Move 转发）──
    bool want_repaint = false;
    if (d.csd_title) {
        const double th = static_cast<double>(d.tb_style.height);
        int hov = -1;  // 序号约定：0=min / 1=max / 2=close（与 draw_decoration 消费端一致）
        if (d.ptr_y >= 0.0 && d.ptr_y < th) {
            const TitleBarGeometry g = title_bar_geometry(static_cast<float>(d.size.width), d.tb_style,
                                                          d.mode == WindowMode::Maximized, d.resizable);
            const auto in_btn = [](const Rect &r, double px, double py) {
                return r.size.width > 0.0F && px >= r.origin.x && px < r.origin.x + r.size.width && py >= r.origin.y &&
                       py < r.origin.y + r.size.height;
            };
            if (in_btn(g.minimize, d.ptr_x, d.ptr_y)) {
                hov = 0;
            } else if (in_btn(g.maximize, d.ptr_x, d.ptr_y)) {
                hov = 1;
            } else if (in_btn(g.close, d.ptr_x, d.ptr_y)) {
                hov = 2;
            }
        }
        if (hov != d.hovered_btn) {
            d.hovered_btn = hov;
            want_repaint = true;
        }
    }
    if (d.mode == WindowMode::FullScreen) {
        // 顶边揭示状态机：近顶 6px 展开；指针下离栏区（height+4 缓冲）后收回。
        bool reveal = d.fs_bar_revealed;
        if (!reveal && d.ptr_y < 6.0) {
            reveal = true;
        } else if (reveal && d.ptr_y > static_cast<double>(d.tb_style.height) + 4.0) {
            reveal = false;
        }
        if (reveal != d.fs_bar_revealed) {
            d.fs_bar_revealed = reveal;
            want_repaint = true;
        }
    } else if (d.fs_bar_revealed) {
        d.fs_bar_revealed = false;  // 非全屏一律复位揭示态
        want_repaint = true;
    }
    if (want_repaint) {
        d.request_repaint();  // 经 Impl 辅助方法触发重绘
    }
    if (d.csd_grab) {
        return;  // CSD 拖拽中：位置已记录，不转发悬停事件给应用
    }
    d.send_mouse(MouseAction::Move, MouseButton::Left, static_cast<float>(d.ptr_x), static_cast<float>(d.ptr_y));
}

void ptr_button(void *data, wl_pointer * /*p*/, std::uint32_t serial, std::uint32_t timestamp, std::uint32_t button,
                std::uint32_t state_v) {
    Impl &d = *static_cast<Impl *>(data);

    if (state_v == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (d.csd_grab) {
            // 结束 CSD 拖拽（move/resize/close 交互）：吞噬本次释放，不转发给应用。
            d.csd_grab = false;
            return;
        }
        d.send_mouse(MouseAction::Release, MouseButton::Left, static_cast<float>(d.ptr_x), static_cast<float>(d.ptr_y));
        return;
    }

    d.last_press_serial = serial;  // 缓存本次按键 serial：xdg move/resize 协议要求

    // ── 右键标题栏 → 合成器原生窗口菜单（xdg-shell 标准协议；GNOME/KDE 均实现）。
    //    仅在自绘标题栏上拦截；其余区域右键照常转发应用。──
    if (button == BTN_RIGHT && d.csd_title && d.toplevel != nullptr && d.seat != nullptr && d.ptr_y >= 0.0 &&
        d.ptr_y < static_cast<double>(d.tb_style.height)) {
        xdg_toplevel_show_window_menu(d.toplevel, d.seat, serial, static_cast<std::int32_t>(d.ptr_x),
                                      static_cast<std::int32_t>(d.ptr_y));
        wl_surface_commit(d.surface);
        wl_display_flush(d.dpy);
        return;
    }

    // 左键按下时按装饰策略尝试拖拽/关闭：区域命中则吞噬，不转发给应用（避免误触控件）。
    // 覆盖三种情形：csd_title（标题栏移动+关闭+边框缩放）、csd_border（边框缩放）、
    // mod_move（无标题栏时 Super/Alt + 拖拽移动）。
    if (button == BTN_LEFT && !d.csd_grab && d.toplevel != nullptr && d.seat != nullptr &&
        (d.csd_title || d.csd_border || d.mod_move)) {
        const double W = d.size.width, H = d.size.height;
        const int tb = static_cast<int>(d.tb_style.height), b = d.border;
        const double x = d.ptr_x, y = d.ptr_y;
        const bool in_left = (x >= 0.0 && x < static_cast<double>(b));
        const bool in_right = (x > W - static_cast<double>(b) && x < W);
        const bool in_top_border = (y >= 0.0 && y < static_cast<double>(b));
        const bool in_bottom = (y > H - static_cast<double>(b) && y < H);
        // 标题栏区域（整个标题栏高度 tb，不是边框厚度 b！）
        const bool in_title = (y >= 0.0 && y < static_cast<double>(tb));

        // ── 标题栏按钮命中（仅 csd_title；几何单一来源，热区=绘制矩形）──
        if (d.csd_title && in_title) {
            const TitleBarGeometry g =
                title_bar_geometry(static_cast<float>(W), d.tb_style, d.mode == WindowMode::Maximized, d.resizable);
            const auto hit_btn = [&](const Rect &r) {
                return r.size.width > 0.0F && x >= r.origin.x && x < r.origin.x + r.size.width && y >= r.origin.y &&
                       y < r.origin.y + r.size.height;
            };

            if (hit_btn(g.close)) {
                AURORA_LOG_INFO("window", "CSD: close button clicked");
                d.close_requested = true;
                d.csd_grab = true;
                return;
            }
            if (hit_btn(g.maximize)) {
                AURORA_LOG_INFO("window", "CSD: maximize button clicked (current mode=", static_cast<int>(d.mode), ")");
                d.csd_grab = true;
                if (d.mode == WindowMode::Maximized) {
                    xdg_toplevel_unset_maximized(d.toplevel);
                } else {
                    xdg_toplevel_set_maximized(d.toplevel);
                }
                // xdg-shell 协议：toplevel 状态请求须经 wl_surface.commit 才被合成器处理并回 configure。
                wl_surface_commit(d.surface);
                wl_display_flush(d.dpy);
                return;
            }
            if (hit_btn(g.minimize)) {
                AURORA_LOG_INFO("window", "CSD: minimize button clicked -> xdg_toplevel_set_minimized");
                d.csd_grab = true;
                xdg_toplevel_set_minimized(d.toplevel);
                wl_surface_commit(d.surface);
                wl_display_flush(d.dpy);
                return;
            }

            // 标题栏空白区 → 双击最大化 或 拖拽移动（客户端自行检测双击；Wayland 无原生双击事件）。
            const bool is_dblclick =
                (timestamp - d.last_click_time < 300) && std::hypot(x - d.last_click_x, y - d.last_click_y) < 5.0;
            d.last_click_time = timestamp;
            d.last_click_x = x;
            d.last_click_y = y;
            if (is_dblclick) {
                d.csd_grab = true;
                if (d.mode == WindowMode::Maximized) {
                    xdg_toplevel_unset_maximized(d.toplevel);
                } else {
                    xdg_toplevel_set_maximized(d.toplevel);
                }
                wl_surface_commit(d.surface);
                wl_display_flush(d.dpy);
                return;
            }
            // 单击：拖拽移动。
            d.csd_grab = true;
            xdg_toplevel_move(d.toplevel, d.seat, serial);
            return;
        }

        // ── 可缩放边框抓手（csd_title 或 csd_border）──
        if (d.csd_border || d.csd_title) {
            // 注意：有标题栏时顶部边框被标题栏覆盖（已由上面的 in_title 处理），
            // 此处 in_top_border 仅在无标题栏（Borderless）时生效。
            if (in_left || in_right || in_top_border || in_bottom) {
                xdg_toplevel_resize_edge edge = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
                if (in_left && in_top_border) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
                } else if (in_right && in_top_border) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
                } else if (in_left && in_bottom) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
                } else if (in_right && in_bottom) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
                } else if (in_left) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
                } else if (in_right) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
                } else if (in_top_border) {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP;
                } else {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
                }
                d.csd_grab = true;
                xdg_toplevel_resize(d.toplevel, d.seat, serial, edge);
                return;
            }
        }

        // 无标题栏时（Borderless/Frameless）：按住 Super/Alt 拖拽任意处移动窗口。
        const bool mod_held = ((d.mods & ModifierKey::Meta) != 0) || ((d.mods & ModifierKey::Alt) != 0);
        if (d.mod_move && !d.csd_title && mod_held) {
            d.csd_grab = true;
            xdg_toplevel_move(d.toplevel, d.seat, serial);
            return;
        }
    }

    const MouseButton mb = (button == BTN_RIGHT)    ? MouseButton::Right
                           : (button == BTN_MIDDLE) ? MouseButton::Middle
                                                    : MouseButton::Left;
    d.send_mouse(MouseAction::Press, mb, static_cast<float>(d.ptr_x), static_cast<float>(d.ptr_y));
}

void ptr_axis(void *data, wl_pointer * /*p*/, std::uint32_t /*time*/, std::uint32_t axis, wl_fixed_t value) {
    Impl &d = *static_cast<Impl *>(data);
    if (!d.handler) {
        return;
    }
    // Wayland axis 正值 = 内容向下/向右滚动，量纲为表面像素（一格滚轮约 10–15px）；
    // 归一到 aurora 约定：上滚为正、约一格 ±1（与 X11 Button4/5 幅度一致）。
    const float amount = static_cast<float>(wl_fixed_to_double(value)) / 10.0F;
    ScrollEvent se;
    se.position = Point{static_cast<float>(d.ptr_x), static_cast<float>(d.ptr_y)};
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        se.delta_y = -amount;
    } else {
        se.delta_x = amount;
    }
    d.handler(se);
}

void ptr_frame(void * /*data*/, wl_pointer * /*p*/) {}

void ptr_axis_source(void * /*d*/, wl_pointer * /*p*/, std::uint32_t /*src*/) {}

void ptr_axis_stop(void * /*d*/, wl_pointer * /*p*/, std::uint32_t /*t*/, std::uint32_t /*axis*/) {}

void ptr_axis_discrete(void * /*d*/, wl_pointer * /*p*/, std::uint32_t /*axis*/, std::int32_t /*n*/) {}

constexpr wl_pointer_listener POINTER_LISTENER = {ptr_enter, ptr_leave,       ptr_motion,    ptr_button,       ptr_axis,
                                                  ptr_frame, ptr_axis_source, ptr_axis_stop, ptr_axis_discrete};

// ---- wl_keyboard：keymap（xkbcommon）/enter/leave/key/modifiers。 ----
void kb_keymap(void *data, wl_keyboard * /*k*/, std::uint32_t format, std::int32_t fd, std::uint32_t sz) {
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        static_cast<Impl *>(data)->on_keymap(fd, sz);
    } else {
        ::close(fd);
    }
}

void kb_enter(void *data, wl_keyboard * /*k*/, std::uint32_t /*serial*/, wl_surface * /*s*/, wl_array * /*keys*/) {
    Impl &d = *static_cast<Impl *>(data);
    d.active = true;
    d.update_state();
}

void kb_leave(void *data, wl_keyboard * /*k*/, std::uint32_t /*serial*/, wl_surface * /*s*/) {
    Impl &d = *static_cast<Impl *>(data);
    d.active = false;
    d.update_state();
}

void kb_key(void *data, wl_keyboard * /*k*/, std::uint32_t /*serial*/, std::uint32_t /*time*/, std::uint32_t key,
            std::uint32_t state_v) {
    static_cast<Impl *>(data)->on_key(key, state_v);
}

void kb_modifiers(void *data, wl_keyboard * /*k*/, std::uint32_t /*serial*/, std::uint32_t depressed,
                  std::uint32_t latched, std::uint32_t locked, std::uint32_t group) {
    static_cast<Impl *>(data)->on_modifiers(depressed, latched, locked, group);
}

void kb_repeat(void * /*data*/, wl_keyboard * /*k*/, std::int32_t /*rate*/, std::int32_t /*delay*/) {
    // 键盘自动重复由客户端负责（Wayland 无服务端重复）：当前版本不合成重复 KeyEvent，
    // 文本编辑长按重复输入留待后续（与 X11 DetectableAutoRepeat 行为差异已知）。
}

constexpr wl_keyboard_listener KEYBOARD_LISTENER = {kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat};

// ---- wl_seat：能力增减 → 惰性获取 pointer/keyboard。 ----
void seat_caps(void *data, wl_seat * /*s*/, std::uint32_t caps) {
    static_cast<Impl *>(data)->on_seat_capabilities(caps);
}

void seat_name(void * /*data*/, wl_seat * /*s*/, const char * /*name*/) {}

constexpr wl_seat_listener SEAT_LISTENER = {seat_caps, seat_name};

// ---- wl_output：scale 事件（HiDPI）。 ----
void out_geometry(void * /*d*/, wl_output * /*o*/, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                  const char *, const char *, std::int32_t) {}

void out_mode(void * /*d*/, wl_output * /*o*/, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}

void out_done(void *data, wl_output * /*o*/) { static_cast<Impl *>(data)->refresh_scale(); }

void out_scale(void *data, wl_output *o, std::int32_t factor) {
    Impl &d = *static_cast<Impl *>(data);
    for (Impl::OutputInfo &info : d.outputs) {
        if (info.out == o) {
            info.scale = factor;
        }
    }
}

void out_name(void * /*d*/, wl_output * /*o*/, const char * /*name*/) {}

void out_desc(void * /*d*/, wl_output * /*o*/, const char * /*desc*/) {}

constexpr wl_output_listener OUTPUT_LISTENER = {out_geometry, out_mode, out_done, out_scale, out_name, out_desc};

// ---- wl_registry：globals 绑定。 ----
void reg_global(void *data, wl_registry * /*r*/, std::uint32_t name, const char *iface, std::uint32_t version) {
    static_cast<Impl *>(data)->on_global(name, iface, version);
}

void reg_global_remove(void * /*data*/, wl_registry * /*r*/, std::uint32_t /*name*/) {}

constexpr wl_registry_listener RETISTERY_LISTENER = {reg_global, reg_global_remove};
}  // namespace

auto WaylandSurface::Impl::on_global(std::uint32_t name, const char *iface, std::uint32_t version) -> void {
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor_version = std::min<std::uint32_t>(version, 4U);
        compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, compositor_version));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1U));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && seat == nullptr) {
        seat = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min<std::uint32_t>(version, 5U)));
        wl_seat_add_listener(seat, &SEAT_LISTENER, this);
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        wm_base = static_cast<xdg_wm_base *>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min<std::uint32_t>(version, 2U)));
        xdg_wm_base_add_listener(wm_base, &WM_BASE_LISTENER, this);
    } else if (std::strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        deco_mgr = static_cast<zxdg_decoration_manager_v1 *>(
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1U));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        OutputInfo info;
        info.out = static_cast<wl_output *>(
            wl_registry_bind(registry, name, &wl_output_interface, std::min<std::uint32_t>(version, 2U)));
        outputs.push_back(info);
        wl_output_add_listener(outputs.back().out, &OUTPUT_LISTENER, this);
    }
}

auto WaylandSurface::Impl::on_xdg_surface_configure(std::uint32_t serial) -> void {
    xdg_surface_ack_configure(xsurface, serial);
    configured = true;
    // 应用 xdg_toplevel.configure 暂存：尺寸（0=客户端自定，保持现值）与几何态。
    bool resized = false;
    if (pending_w > 0 && pending_h > 0) {
        const Size want{static_cast<float>(pending_w), static_cast<float>(pending_h)};
        if (want.width != size.width || want.height != size.height) {
            size = want;
            resized = true;
        }
    }
    const WindowMode want_mode = pending_fs     ? WindowMode::FullScreen
                                 : pending_max  ? WindowMode::Maximized
                                 : pending_susp ? WindowMode::Minimized
                                                : WindowMode::Normal;
    if (want_mode != mode) {
        mode = want_mode;
        minimized = (want_mode == WindowMode::Minimized);
        self->notify_window_mode(want_mode);
        update_state();
    }
    if (active != pending_act) {
        active = pending_act;
        update_state();
    }
    if (resized && self->present_request_) {
        // 几何变化当下同步重渲染（对齐 Win32 WM_SIZE / X11 ConfigureNotify）：
        // 下一次 commit 的缓冲已为新尺寸内容，无黑边/残留。
        self->present_request_();
    }
}

auto WaylandSurface::Impl::on_toplevel_configure(std::int32_t w, std::int32_t h, wl_array *states) -> void {
    pending_w = w;
    pending_h = h;
    pending_max = false;
    pending_fs = false;
    pending_susp = false;
    pending_act = false;
    const auto *arr = static_cast<const std::uint32_t *>(states->data);
    const std::size_t n = states->size / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < n; ++i) {
        switch (arr[i]) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED:
                pending_max = true;
                break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:
                pending_fs = true;
                break;
            case XDG_TOPLEVEL_STATE_ACTIVATED:
                pending_act = true;
                break;
            case XDG_TOPLEVEL_STATE_SUSPENDED:
                pending_susp = true;
                break;
            default:
                break;
        }
    }
}

auto WaylandSurface::Impl::on_seat_capabilities(std::uint32_t caps) -> void {
    const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0U;
    const bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0U;
    if (has_ptr && pointer == nullptr) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &POINTER_LISTENER, this);
    } else if (!has_ptr && pointer != nullptr) {
        wl_pointer_destroy(pointer);
        pointer = nullptr;
    }
    if (has_kb && keyboard == nullptr) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &KEYBOARD_LISTENER, this);
    } else if (!has_kb && keyboard != nullptr) {
        wl_keyboard_destroy(keyboard);
        keyboard = nullptr;
    }
}

auto WaylandSurface::Impl::on_keymap(std::int32_t fd, std::uint32_t sz) -> void {
    if (sz == 0) {
        ::close(fd);
        return;
    }
    void *map = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        if (xkb_st != nullptr) {
            xkb_state_unref(xkb_st);
            xkb_st = nullptr;
        }
        if (keymap != nullptr) {
            xkb_keymap_unref(keymap);
            keymap = nullptr;
        }
        // 用 _from_buffer 而非 _from_string：后者对映射区隐式 strlen，
        // 若合成器（或恶意冒充的合成器）传来未以 NUL 结尾且恰好页对齐的 keymap，
        // 就会越过映射末尾读进未映射页（SIGSEGV / 读到相邻映射内容）。
        // 协议约定末字节为 NUL，故按 sz-1 作为有效长度传入。
        keymap = xkb_keymap_new_from_buffer(xkb_ctx, static_cast<const char *>(map), static_cast<std::size_t>(sz) - 1u,
                                            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap != nullptr) {
            xkb_st = xkb_state_new(keymap);
        }
        munmap(map, sz);
    }
    ::close(fd);
}

auto WaylandSurface::Impl::on_modifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                                        std::uint32_t group) -> void {
    if (xkb_st == nullptr) {
        return;
    }
    xkb_state_update_mask(xkb_st, depressed, latched, locked, 0, 0, group);
    ModifierKey m = ModifierKey::None;
    if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m = m | ModifierKey::Shift;
    }
    if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m = m | ModifierKey::Control;
    }
    if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m = m | ModifierKey::Alt;
    }
    if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m = m | ModifierKey::Meta;
    }
    mods = m;
}

auto WaylandSurface::Impl::on_key(std::uint32_t key, std::uint32_t state_v) const -> void {
    if (!handler || xkb_st == nullptr) {
        return;
    }
    const xkb_keycode_t kc = key + 8U;  // evdev → xkb keycode 偏移（约定）
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, kc);
    KeyEvent e;
    e.action = (state_v == WL_KEYBOARD_KEY_STATE_PRESSED) ? KeyAction::Down : KeyAction::Up;
    e.key = static_cast<int>(from_xkb_keysym(sym));
    e.modifiers = mods;
    handler(e);
    if (state_v == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // 可打印文本 → TextInputEvent；控制字符（回车/退格/Esc…）交给 KeyEvent（对齐 X11 路径）。
        char buf[64];
        // xkb_state_key_get_utf8 是 snprintf 语义：缓冲不够时**截断写入**但返回「所需字节数」。
        // 直接把返回值当已写长度会越界读栈（一个键可绑定多个 keysym，len 可任意大）。
        int len = xkb_state_key_get_utf8(xkb_st, kc, buf, sizeof(buf));
        len = std::clamp(len, 0, static_cast<int>(sizeof(buf)) - 1);
        if (len > 0 && !(len == 1 && (static_cast<unsigned char>(buf[0]) < 0x20 || buf[0] == 0x7F))) {
            TextInputEvent te;
            te.text.assign(buf, static_cast<std::size_t>(len));
            handler(te);
        }
    }
}

auto WaylandSurface::Impl::ensure_slot(Slot &s, int w, int h) const -> bool {
    if (s.buf != nullptr && s.w == w && s.h == h) {
        return true;
    }
    release_slot(s);
    const std::size_t stride = static_cast<std::size_t>(w) * 4U;
    const std::size_t bytes = stride * static_cast<std::size_t>(h);
    const int fd = memfd_create("aurora-wl-shm", MFD_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return false;
    }
    void *map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(bytes));
    s.buf = wl_shm_pool_create_buffer(pool, 0, w, h, static_cast<std::int32_t>(stride), WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);  // pool 已持有 fd 引用，本端句柄可关
    if (s.buf == nullptr) {
        munmap(map, bytes);
        return false;
    }
    wl_buffer_add_listener(s.buf, &BUFFER_LISTENER, &s);
    s.px = static_cast<std::uint32_t *>(map);
    s.bytes = bytes;
    s.w = w;
    s.h = h;
    s.busy = false;
    return true;
}

auto WaylandSurface::Impl::pick_slot(int w, int h) -> Slot * {
    // 双缓冲槽轮换：优先空闲槽；两个都被合成器持有时 roundtrip 等 release（有限次兜底）。
    for (int attempt = 0; attempt < 16; ++attempt) {
        for (Slot &s : slots) {
            if (!s.busy) {
                return ensure_slot(s, w, h) ? &s : nullptr;
            }
        }
        if (wl_display_roundtrip(dpy) < 0) {
            return nullptr;
        }
    }
    return nullptr;
}

// =============================================================================
// WaylandSurface：构造/析构与 Surface 接口实现
// =============================================================================

WaylandSurface::WaylandSurface(int w, int h, const std::string &title, const WindowStyleOptions &style)
    : impl_(std::make_unique<Impl>()) {
    Impl &d = *impl_;
    d.self = this;
    d.dpy = wl_display_connect(nullptr);
    if (d.dpy == nullptr) {
        AURORA_LOG_WARN("window",
                        "WaylandSurface: wl_display_connect failed (WAYLAND_DISPLAY unset or "
                        "unreachable); surface unavailable, factory will return an error.");
        return;
    }
    d.registry = wl_display_get_registry(d.dpy);
    wl_registry_add_listener(d.registry, &RETISTERY_LISTENER, &d);
    // 两次 roundtrip：第 1 次收 globals；第 2 次收绑定后首批事件（wl_output.scale/seat caps）。
    wl_display_roundtrip(d.dpy);
    wl_display_roundtrip(d.dpy);
    if (d.compositor == nullptr || d.shm == nullptr || d.wm_base == nullptr) {
        AURORA_LOG_WARN("window",
                        "WaylandSurface: required globals missing (wl_compositor/wl_shm/xdg_wm_base); "
                        "surface unavailable.");
        wl_display_disconnect(d.dpy);
        d.dpy = nullptr;
        return;
    }
    d.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // 窗口壳：wl_surface → xdg_surface → xdg_toplevel。
    d.surface = wl_compositor_create_surface(d.compositor);
    d.xsurface = xdg_wm_base_get_xdg_surface(d.wm_base, d.surface);
    xdg_surface_add_listener(d.xsurface, &XDG_SURFACE_LISTENER, &d);
    d.toplevel = xdg_surface_get_toplevel(d.xsurface);
    xdg_toplevel_add_listener(d.toplevel, &TOP_LEVEL_LISTENER, &d);
    xdg_toplevel_set_title(d.toplevel, title.c_str());
    xdg_toplevel_set_app_id(d.toplevel, "aurora");
    // 样式映射：尺寸限制经 xdg_toplevel（逻辑坐标）；always_on_top 无核心协议对应（忽略并告警）。
    if (!style.resizable) {
        xdg_toplevel_set_min_size(d.toplevel, w, h);
        xdg_toplevel_set_max_size(d.toplevel, w, h);
    } else {
        if (style.min_size.width > 0.0F || style.min_size.height > 0.0F) {
            xdg_toplevel_set_min_size(d.toplevel, static_cast<std::int32_t>(style.min_size.width),
                                      static_cast<std::int32_t>(style.min_size.height));
        }
        if (style.max_size.width > 0.0F || style.max_size.height > 0.0F) {
            xdg_toplevel_set_max_size(d.toplevel, static_cast<std::int32_t>(style.max_size.width),
                                      static_cast<std::int32_t>(style.max_size.height));
        }
    }
    if (style.always_on_top) {
        AURORA_LOG_WARN("window", "WaylandSurface: always_on_top has no core Wayland protocol; ignored.");
    }
    // 服务端装饰协商 + CSD 兜底策略解析（见 DecorationPolicy）。
    // frameless 语义等价于 Frameless；否则取 style.decoration。
    DecorationPolicy pol = style.frameless ? DecorationPolicy::Frameless : style.decoration;
    d.deco_policy = pol;
    const bool ssd_available = (d.deco_mgr != nullptr);
    if (ssd_available) {
        d.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(d.deco_mgr, d.toplevel);
        // 仅 ClientSide 强制客户端装饰；其余（Auto/ServerSide/Borderless/Frameless）请求服务端。
        const bool want_client = (pol == DecorationPolicy::ClientSide);
        zxdg_toplevel_decoration_v1_set_mode(d.deco, want_client ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                                                 : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    // 依策略决定自绘装饰形态：标题栏（移动+关闭）/ 边框（缩放）/ 修饰键拖拽移动。
    switch (pol) {
        case DecorationPolicy::Auto:  // 有 SSD 用原生；无 SSD（GNOME）自绘兜底。
        case DecorationPolicy::ServerSide:  // 强制服务端；不可用时退化为自绘兜底（避免不可操作）。
            d.csd_title = !ssd_available;
            d.csd_border = d.csd_title;
            break;
        case DecorationPolicy::ClientSide:  // 强制自绘标题栏（即便 KDE 支持 SSD）；不画边框。
            // 边缘缩放仍可用：ptr_button 的 `csd_border || csd_title` 分支保留左/右/下热区，
            // 顶部被标题栏覆盖；content_inset 仅报告标题栏，不额外留边框。
            d.csd_title = true;
            d.csd_border = false;
            break;
        case DecorationPolicy::Borderless:  // 无标题栏：可缩放边框；移动靠修饰键拖拽。
            d.csd_title = false;
            d.csd_border = true;
            d.mod_move = true;
            break;
        case DecorationPolicy::Frameless:  // 完全无装饰：应用自绘 + 程序化 API；移动靠修饰键拖拽。
            d.csd_title = false;
            d.csd_border = false;
            d.mod_move = true;
            break;
    }
    if (!ssd_available && pol != DecorationPolicy::Frameless && pol != DecorationPolicy::Borderless) {
        AURORA_LOG_INFO("window",
                        "WaylandSurface: compositor lacks xdg-decoration (GNOME): falling back to "
                        "client-side decoration (drawn title bar).");
    }
    d.title = title;
    d.tb_style = style.title_bar;  // CSD 标题栏样式（运行期可经 set_title_bar_style 热更）
    d.resizable = style.resizable;
    if (pipe2(d.wake_fd, O_NONBLOCK | O_CLOEXEC) != 0) {
        d.wake_fd[0] = d.wake_fd[1] = -1;
    }
    if (w <= 0) {
        w = 320;
    }
    if (h <= 0) {
        h = 240;
    }
    d.size = Size{static_cast<float>(w), static_cast<float>(h)};
    // 首次 commit（无缓冲）宣告表面存在 → 合成器回 configure；阻塞等到 configured
    // 才允许 attach（xdg-shell 协议要求，违者 protocol error 断链）。
    wl_surface_commit(d.surface);
    while (!d.configured && wl_display_dispatch(d.dpy) >= 0) {
        // 等待首个 xdg_surface.configure（正常合成器亚毫秒级返回）
    }
    d.refresh_scale();
}

WaylandSurface::~WaylandSurface() {
    Impl &d = *impl_;
    // 先断开全部上层回调再销毁（对齐 Win32/X11 析构次序教训：销毁期间不得回调已亡上层）。
    d.handler = nullptr;
    window_state_handler_ = nullptr;
    window_mode_handler_ = nullptr;
    present_request_ = nullptr;
    if (d.dpy != nullptr) {
        for (Impl::Slot &s : d.slots) {
            d.release_slot(s);
        }
        if (d.deco != nullptr) {
            zxdg_toplevel_decoration_v1_destroy(d.deco);
        }
        if (d.deco_mgr != nullptr) {
            zxdg_decoration_manager_v1_destroy(d.deco_mgr);
        }
        if (d.pointer != nullptr) {
            wl_pointer_destroy(d.pointer);
        }
        if (d.keyboard != nullptr) {
            wl_keyboard_destroy(d.keyboard);
        }
        if (d.seat != nullptr) {
            wl_seat_destroy(d.seat);
        }
        for (Impl::OutputInfo &o : d.outputs) {
            if (o.out != nullptr) {
                wl_output_destroy(o.out);
            }
        }
        if (d.toplevel != nullptr) {
            xdg_toplevel_destroy(d.toplevel);
        }
        if (d.xsurface != nullptr) {
            xdg_surface_destroy(d.xsurface);
        }
        if (d.surface != nullptr) {
            wl_surface_destroy(d.surface);
        }
        if (d.wm_base != nullptr) {
            xdg_wm_base_destroy(d.wm_base);
        }
        if (d.shm != nullptr) {
            wl_shm_destroy(d.shm);
        }
        if (d.compositor != nullptr) {
            wl_compositor_destroy(d.compositor);
        }
        if (d.registry != nullptr) {
            wl_registry_destroy(d.registry);
        }
        wl_display_flush(d.dpy);
        wl_display_disconnect(d.dpy);
    }
    if (d.xkb_st != nullptr) {
        xkb_state_unref(d.xkb_st);
    }
    if (d.keymap != nullptr) {
        xkb_keymap_unref(d.keymap);
    }
    if (d.xkb_ctx != nullptr) {
        xkb_context_unref(d.xkb_ctx);
    }
    if (d.wake_fd[0] >= 0) {
        ::close(d.wake_fd[0]);
    }
    if (d.wake_fd[1] >= 0) {
        ::close(d.wake_fd[1]);
    }
}

auto WaylandSurface::is_available() const -> bool { return impl_->dpy != nullptr && impl_->surface != nullptr; }

auto WaylandSurface::begin_frame(int width, int height) -> Result<bool> {
    Impl &d = *impl_;
    // 新帧默认全量上屏；present_root 会在 present 前重新 set_present_dirty。
    d.present_dirty.clear();
    d.painter.set_scale(static_cast<float>(d.scale));
    // configure 已把最新逻辑尺寸写入 d.size（对齐 Win32/X11「查真实几何」策略：
    // 调用方入参可能滞后一帧，以 d.size 为准保证缓冲与表面 1:1 吻合）。
    int lw = static_cast<int>(std::lround(d.size.width));
    int lh = static_cast<int>(std::lround(d.size.height));
    if (lw <= 0) {
        lw = width > 0 ? width : 1;
    }
    if (lh <= 0) {
        lh = height > 0 ? height : 1;
    }
    // Painter 缓冲按物理分辨率分配（逻辑 × scale）：与 painter.width()（物理）比较判重建。
    const int phys_w = lw * d.scale;
    const int phys_h = lh * d.scale;
    if (phys_w != d.painter.width() || phys_h != d.painter.height() || d.painter.data() == nullptr) {
        d.painter.begin(lw, lh);
    }
    // 浅色背景：默认文字为黑色，需浅色底才可见（与 Win32/GLFW/X11 后端一致）。
    d.painter.fill_rect(
        Rect{Point{0.0F, 0.0F}, Size{static_cast<float>(d.painter.width()), static_cast<float>(d.painter.height())}},
        Color{245, 245, 247, 255});
    return Result<bool>{true};
}

auto WaylandSurface::painter() -> Painter & { return impl_->painter; }

auto WaylandSurface::data() const -> const std::uint8_t * {
    if (impl_ && impl_->painter.data() != nullptr) {
        return impl_->painter.data();
    }
    return nullptr;
}

auto WaylandSurface::Impl::draw_decoration(Painter &p) const -> void {
    // 全屏默认隐藏标题栏；顶边悬停揭示由 ptr_motion 置 fs_bar_revealed 后经 present 重绘可见。
    if (mode == WindowMode::FullScreen && !fs_bar_revealed) {
        return;
    }
    const float W = static_cast<float>(size.width);
    // 几何单一来源：与 ptr_button 命中测试共用同一纯函数，杜绝热区与绘制错位。
    const TitleBarGeometry g = title_bar_geometry(W, tb_style, mode == WindowMode::Maximized, resizable);
    const Color bg = active ? tb_style.bg_active : tb_style.bg_inactive;
    const Color fg = active ? tb_style.fg_active : tb_style.fg_inactive;

    if (csd_title) {
        // 标题栏背景。
        p.fill_rect(Rect{Point{0.0F, 0.0F}, Size{W, tb_style.height}}, bg);

        // 图标槽（set_title_bar_icon 注入后显示；无图标留白，几何预留位不变）。
        if (tb_icon != nullptr && g.icon.size.width > 0.0F) {
            p.draw_image(*tb_icon, g.icon);
        }

        // 标题文字（draw_text 缺字体时回退内置位图字体，不依赖 FontEngine）。
        if (tb_style.show_title && !title.empty() && g.title.size.width > 0.0F) {
            Font f;
            f.size_pt = 13.0F;
            f.weight = 500;
            p.draw_text(g.title, title, f, fg);
        }

        // 悬停序号约定：0=minimize / 1=maximize / 2=close（ptr_motion 命中写入，与布局无关）。
        const int hb = hovered_btn;

        auto center_of = [](const Rect &r) {
            return Point{r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f};
        };
        auto glyph_min = [&](const Rect &r, float lw, const Color &c) {
            const Point m = center_of(r);
            const float e = r.size.width / 3.0F;
            p.draw_line(Point{m.x - e, m.y}, Point{m.x + e, m.y}, lw, c);
        };
        auto glyph_max = [&](const Rect &r, float lw, const Color &c) {
            const Point m = center_of(r);
            const float e = r.size.width / 3.6f;
            if (mode != WindowMode::Maximized) {
                // □ 空心方框。
                p.draw_line(Point{m.x - e, m.y - e}, Point{m.x + e, m.y - e}, lw, c);
                p.draw_line(Point{m.x + e, m.y - e}, Point{m.x + e, m.y + e}, lw, c);
                p.draw_line(Point{m.x + e, m.y + e}, Point{m.x - e, m.y + e}, lw, c);
                p.draw_line(Point{m.x - e, m.y + e}, Point{m.x - e, m.y - e}, lw, c);
            } else {
                // ▯ 还原：前实框 + 右上错位背框（双框表达「已最大化，点击还原」）。
                const float o = e * 0.45f;
                p.draw_line(Point{m.x - e, m.y + o - e}, Point{m.x + e, m.y + o - e}, lw, c);
                p.draw_line(Point{m.x + e, m.y + o - e}, Point{m.x + e, m.y + o + e}, lw, c);
                p.draw_line(Point{m.x + e, m.y + o + e}, Point{m.x - e, m.y + o + e}, lw, c);
                p.draw_line(Point{m.x - e, m.y + o + e}, Point{m.x - e, m.y + o - e}, lw, c);
                p.draw_line(Point{m.x - e + o, m.y - e}, Point{m.x + e + o, m.y - e}, lw, c);
                p.draw_line(Point{m.x + e + o, m.y - e}, Point{m.x + e + o, m.y + e}, lw, c);
                p.draw_line(Point{m.x + e + o, m.y + e}, Point{m.x - e + o, m.y + e}, lw, c);
                p.draw_line(Point{m.x - e + o, m.y + e}, Point{m.x - e + o, m.y - e}, lw, c);
            }
        };
        auto glyph_close = [&](const Rect &r, float lw, const Color &c) {
            const Point m = center_of(r);
            const float e = r.size.width / 3.0F;
            p.draw_line(Point{m.x - e, m.y - e}, Point{m.x + e, m.y + e}, lw, c);
            p.draw_line(Point{m.x + e, m.y - e}, Point{m.x - e, m.y + e}, lw, c);
        };

        if (tb_style.button_layout == TitleBarButtonLayout::Adwaita) {
            // Adwaita：扁平单色符号，悬停浮出圆形底（关闭钮红底为其视觉签名）。
            if (g.minimize.size.width > 0.0F) {
                if (hb == 0) {
                    p.fill_rounded_rect(g.minimize, g.minimize.size.width * 0.5f, tb_style.hover_tint);
                }
                glyph_min(g.minimize, 1.5f, fg);
            }
            if (g.maximize.size.width > 0.0F) {
                if (hb == 1) {
                    p.fill_rounded_rect(g.maximize, g.maximize.size.width * 0.5f, tb_style.hover_tint);
                }
                glyph_max(g.maximize, 1.5f, fg);
            }
            if (g.close.size.width > 0.0F) {
                if (hb == 2) {
                    p.fill_rounded_rect(g.close, g.close.size.width * 0.5f, tb_style.close_hover);
                }
                glyph_close(g.close, 1.5f, Color{255, 255, 255, 235});
            }
        } else if (tb_style.button_layout == TitleBarButtonLayout::Windows) {
            // Windows：整高矩形热区，悬停整块填充。
            if (g.minimize.size.width > 0.0F) {
                if (hb == 0) {
                    p.fill_rect(g.minimize, tb_style.hover_tint);
                }
                glyph_min(g.minimize, 1.2f, fg);
            }
            if (g.maximize.size.width > 0.0F) {
                if (hb == 1) {
                    p.fill_rect(g.maximize, tb_style.hover_tint);
                }
                glyph_max(g.maximize, 1.2f, fg);
            }
            if (g.close.size.width > 0.0F) {
                if (hb == 2) {
                    p.fill_rect(g.close, tb_style.close_hover);
                }
                glyph_close(g.close, 1.2f, Color{255, 255, 255, 235});
            }
        } else {
            // macOS：常显三色圆点，悬停浮现深色符号（close/min/max 左→右序由几何层保证）。
            const Color sym{0x3D, 0x3D, 0x3D, 210};
            if (g.minimize.size.width > 0.0F) {
                p.fill_rounded_rect(g.minimize, g.minimize.size.width * 0.5f, Color{0xFE, 0xBC, 0x2E, 255});
                if (hb == 0) {
                    glyph_min(g.minimize, 1.2f, sym);
                }
            }
            if (g.maximize.size.width > 0.0F) {
                p.fill_rounded_rect(g.maximize, g.maximize.size.width * 0.5f, Color{0x28, 0xC8, 0x40, 255});
                if (hb == 1) {
                    glyph_max(g.maximize, 1.2f, sym);
                }
            }
            if (g.close.size.width > 0.0F) {
                p.fill_rounded_rect(g.close, g.close.size.width * 0.5f, Color{0xFF, 0x5F, 0x57, 255});
                if (hb == 2) {
                    glyph_close(g.close, 1.2f, sym);
                }
            }
        }
    }
    // 可缩放边框：不画可见线（缩放由 ptr_button 边缘热区驱动，浅色背景上画线反而突兀）。
}

auto WaylandSurface::present() -> Result<bool> {
    Impl &d = *impl_;
    if (d.dpy != nullptr && d.configured && d.painter.data() != nullptr) {
        const int w = d.painter.width();  // 物理像素（Painter 按 scale 放大分配）
        const int h = d.painter.height();
        Impl::Slot *slot = d.pick_slot(w, h);
        if (slot == nullptr) {
            return make_error(ErrorCode::PlatformUnavailable, "WaylandSurface::present: no free wl_shm buffer slot.",
                              "Compositor may be unresponsive; retry next frame.", "aurora/window/wayland_surface.h");
        }
        if (d.csd_title || d.csd_border) {
            // 自绘装饰必须在 swizzle 前绘制到 painter（RGBA），随缓冲一同上屏。
            d.draw_decoration(d.painter);
        }
        const auto *src = reinterpret_cast<const std::uint32_t *>(d.painter.data());
        if (d.present_dirty.empty()) {
            // 全量：整幅 swizzle + 整面 damage（首帧/尺寸变化/布局帧）。
            swizzle_rgba_to_bgra(src, slot->px, static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            wl_surface_damage_buffer(d.surface, 0, 0, w, h);
        } else {
            // 增量：逐脏矩形仅 swizzle + damage 变化区（拖选/局部重绘帧）。
            // 注意：槽轮换后缓冲内容可能是上上帧，脏区外像素也需追平 → 整幅 swizzle 但仅 damage 脏区
            // 的代价与全量无异；此处取「整幅 swizzle + 精确 damage」保正确性（合成器仅回读 damage 区）。
            swizzle_rgba_to_bgra(src, slot->px, static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            for (const Rect &r : d.present_dirty) {
                const int x0 = std::max(0, static_cast<int>(std::floor(r.origin.x)));
                const int y0 = std::max(0, static_cast<int>(std::floor(r.origin.y)));
                const int x1 = std::min(w, static_cast<int>(std::ceil(r.right())));
                const int y1 = std::min(h, static_cast<int>(std::ceil(r.bottom())));
                if (x0 >= x1 || y0 >= y1) {
                    continue;
                }
                wl_surface_damage_buffer(d.surface, x0, y0, x1 - x0, y1 - y0);
            }
        }
        if (d.compositor_version >= 3U) {
            wl_surface_set_buffer_scale(d.surface, d.scale);
        }
        wl_surface_attach(d.surface, slot->buf, 0, 0);
        slot->busy = true;
        wl_surface_commit(d.surface);
        wl_display_flush(d.dpy);
    }
    // 脏区一次性消费：下一帧未重新设置则回到全量（安全兜底）。
    d.present_dirty.clear();
    return Result<bool>{true};
}

auto WaylandSurface::size() const -> Size { return impl_->size; }

auto WaylandSurface::scale_factor() const -> float { return static_cast<float>(impl_->scale); }

auto WaylandSurface::should_close() const -> bool { return impl_->close_requested; }

auto WaylandSurface::set_present_dirty(const std::vector<Rect> &device_rects) -> void {
    impl_->present_dirty = device_rects;
}

auto WaylandSurface::set_event_handler(const EventHandler &h) -> void { impl_->handler = h; }

auto WaylandSurface::set_title(const std::string &title) -> void {
    Impl &d = *impl_;
    d.title = title;
    if (d.toplevel != nullptr) {
        xdg_toplevel_set_title(d.toplevel, title.c_str());
        wl_display_flush(d.dpy);
    }
}

auto WaylandSurface::begin_window_move() -> void {
    // 控件（自绘 TitleBar 等）在 Press 派发栈内同步调用：last_press_serial 即触发键 serial。
    if (impl_->toplevel != nullptr && impl_->seat != nullptr) {
        xdg_toplevel_move(impl_->toplevel, impl_->seat, impl_->last_press_serial);
        wl_surface_commit(impl_->surface);
        wl_display_flush(impl_->dpy);
    }
}

auto WaylandSurface::begin_window_resize(WindowResizeEdge edge) -> void {
    // 序对应 WindowResizeEdge 枚举值序：None/Top/Bottom/Left/Right/TopLeft/TopRight/BottomLeft/BottomRight。
    static constexpr xdg_toplevel_resize_edge kMap[] = {
        XDG_TOPLEVEL_RESIZE_EDGE_NONE,      XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
        XDG_TOPLEVEL_RESIZE_EDGE_LEFT,      XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,
        XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
    };
    const auto idx = static_cast<std::size_t>(edge);
    if (idx == 0 || idx >= std::size(kMap) || impl_->toplevel == nullptr || impl_->seat == nullptr) {
        return;
    }
    xdg_toplevel_resize(impl_->toplevel, impl_->seat, impl_->last_press_serial, kMap[idx]);
    wl_surface_commit(impl_->surface);
    wl_display_flush(impl_->dpy);
}

auto WaylandSurface::set_title_bar_style(const TitleBarStyle &style) -> void {
    Impl &d = *impl_;
    d.tb_style = style;
    if (present_request_) {  // 立即重绘（回调挂在基类 self 上，仿 refresh_scale() 既有模式）
        present_request_();
    }
}

auto WaylandSurface::set_title_bar_icon(const std::shared_ptr<Image> &icon) -> void {
    Impl &d = *impl_;
    d.tb_icon = icon;  // 共享所有权存储；绘制层经 draw_decoration 取用
    if (present_request_) {  // 立即重绘，使新图标下帧可见
        present_request_();
    }
}

auto WaylandSurface::native_handle() const -> void * { return static_cast<void *>(impl_->surface); }

auto WaylandSurface::content_inset() const -> EdgeInsets {
    const Impl &d = *impl_;
    float tb = d.csd_title ? d.tb_style.height : 0.0F;
    if (d.mode == WindowMode::FullScreen) {
        tb = 0.0F;  // 全屏下标题栏退化为揭示条（覆盖层），不回流应用布局，故安全区 top 归零
    }
    const float b = d.csd_border ? static_cast<float>(d.border) : 0.0F;
    return EdgeInsets{b, tb, b, b};  // 顺序：left, top, right, bottom
}

auto WaylandSurface::close() -> void {
    Impl &d = *impl_;
    if (d.toplevel != nullptr) {
        d.close_requested = true;  // 下帧主循环据 should_close() 退出；等价于用户点 ×。
    }
}

auto WaylandSurface::minimize() -> void {
    Impl &d = *impl_;
    if (d.toplevel != nullptr) {
        xdg_toplevel_set_minimized(d.toplevel);
        // xdg-shell 协议：状态请求须经 commit 才被合成器处理。
        wl_surface_commit(d.surface);
        wl_display_flush(d.dpy);
    }
}

auto WaylandSurface::toggle_maximize() -> void {
    Impl &d = *impl_;
    if (d.toplevel == nullptr) {
        return;
    }
    if (d.mode == WindowMode::Maximized) {
        xdg_toplevel_unset_maximized(d.toplevel);
    } else {
        xdg_toplevel_set_maximized(d.toplevel);
    }
    wl_surface_commit(d.surface);
    wl_display_flush(d.dpy);
}

auto WaylandSurface::set_fullscreen(bool on) -> void {
    Impl &d = *impl_;
    if (d.toplevel == nullptr) {
        return;
    }
    if (on) {
        xdg_toplevel_set_fullscreen(d.toplevel, nullptr);
    } else {
        xdg_toplevel_unset_fullscreen(d.toplevel);
    }
    wl_surface_commit(d.surface);
    wl_display_flush(d.dpy);
}

auto WaylandSurface::poll_platform_events() -> void {
    Impl &d = *impl_;
    if (d.dpy == nullptr) {
        return;
    }
    // 非阻塞抽干：prepare_read/read_events 单线程范式（避免 dispatch 内部阻塞）。
    while (wl_display_prepare_read(d.dpy) != 0) {
        wl_display_dispatch_pending(d.dpy);
    }
    wl_display_flush(d.dpy);
    struct pollfd pfd{wl_display_get_fd(d.dpy), POLLIN, 0};
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
        wl_display_read_events(d.dpy);
    } else {
        wl_display_cancel_read(d.dpy);
    }
    if (wl_display_dispatch_pending(d.dpy) < 0) {
        // 连接错误（合成器退出/协议错误）：按关闭处理，帧循环可退出。
        AURORA_LOG_WARN("window", "WaylandSurface: display connection error; treating as close.");
        d.close_requested = true;
    }
}

auto WaylandSurface::wait_events(double timeout_ms) -> void {
    Impl &d = *impl_;
    if (d.dpy == nullptr || timeout_ms == 0.0 || d.close_requested) {
        return;
    }
    if (wl_display_prepare_read(d.dpy) != 0) {
        return;  // 队列已有未处理事件：立即回到帧循环消费
    }
    wl_display_flush(d.dpy);
    // 无限等待按 1000ms 分段兜底（对齐 Win32/X11）：唤醒渠道丢失也最迟 1s 自然醒。
    const double capped = (timeout_ms < 0.0 || timeout_ms > 1000.0) ? 1000.0 : timeout_ms;
    struct pollfd fds[2];
    nfds_t n = 0;
    fds[n].fd = wl_display_get_fd(d.dpy);
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    ++n;
    if (d.wake_fd[0] >= 0) {
        fds[n].fd = d.wake_fd[0];
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        ++n;
    }
    const int rc = ::poll(fds, n, static_cast<int>(std::ceil(capped)));
    if (rc > 0 && (fds[0].revents & POLLIN) != 0) {
        wl_display_read_events(d.dpy);
    } else {
        wl_display_cancel_read(d.dpy);
    }
    if (rc > 0 && n == 2 && (fds[1].revents & POLLIN) != 0) {
        char drain[64];
        while (::read(d.wake_fd[0], drain, sizeof(drain)) > 0) {
            // 排干唤醒字节（非阻塞读到 EAGAIN 为止），避免下次 wait 立即空醒。
        }
    }
}

auto WaylandSurface::request_wake() -> void {
    Impl &d = *impl_;
    if (d.wake_fd[1] >= 0) {
        constexpr char b = 1;
        [[maybe_unused]] const ssize_t rc = ::write(d.wake_fd[1], &b, 1);  // 满管道丢弃亦可：已有待读字节必醒
    }
}
}  // namespace aurora

#endif  // AURORA_BACKEND_WAYLAND / AURORA_PLATFORM_LINUX
