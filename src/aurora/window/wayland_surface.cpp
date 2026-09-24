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
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/version.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/cursor_map.h"
#include "aurora/window/detail/atspi_bridge.h"
#include "aurora/window/detail/ime_composition.h"
#include "aurora/window/detail/title_bar_painter.h"
#include "aurora/window/keysym_map.h"
#include "aurora/window/swizzle.h"
#include "aurora/window/window_state.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

// text-input-unstable-v3 胶水由 CMake 在协议 XML 存在时代码生成并置宏（见 AuroraBackends.cmake）；
// 缺 XML/老 distro 编译期退化为「无 IME 桥」，其余 Wayland 功能不受影响。
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
#include "text-input-unstable-v3-client-protocol.h"
#endif

namespace aurora {
namespace {
/// @brief xkb keysym → 平台无关 KeyCode（Wayland 后端入口；映射逻辑见 detail::keysym_to_keycode）。
auto from_xkb_keysym(xkb_keysym_t ks) -> KeyCode { return detail::keysym_to_keycode(static_cast<unsigned long>(ks)); }
}  // namespace

/// @brief WaylandSurface 的全部平台状态（pimpl）：公共头零 Wayland 依赖。
/// 嵌套类型可访问外围类 protected 成员（notify_window_state/present_request_），
/// C 回调经 static thunk 转发到本结构的成员函数。
struct WaylandSurface::Impl {
    WaylandSurface *self = nullptr;  ///< 反向指针：listener 内上抛 notify_*/present_request_。
    // 核心 globals（registry 绑定）。
    wl_display *dpy = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    std::uint32_t compositor_version = 0;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    xdg_wm_base *wm_base = nullptr;
    zxdg_decoration_manager_v1 *deco_mgr = nullptr;
    // text-input 桥整体随代码生成宏进出：宏未置（协议 XML 缺失）时以下字段与桥方法都不存在。
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    // 输入管理器全局（text-input-unstable-v3；合成器未发布则恒 nullptr，IME 桥整体优雅缺席）。
    zwp_text_input_manager_v3 *text_input_mgr = nullptr;
#endif
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
    int presented = 0;  ///< 已上屏帧数（见 `WaylandSurface::frame_count()`）。
    /// @brief 本帧 attach 因 configure 打断而丢弃，需在下次事件泵补一帧（见 present() 内注释）。
    bool present_stale = false;
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
    WindowVisibility visibility = WindowVisibility::Normal;  ///< 构造期定档的可见性策略。
    WindowState state = WindowState::Visible;
    WindowMode mode = WindowMode::Normal;
    Surface::EventHandler handler;
    // 自唤醒管道（request_wake → wait_events poll 立即返回）。
    int wake_fd[2] = {-1, -1};
    // AT-SPI2 无障碍桥（宿主惰性构造：首次 set_accessibility_root 尝试；失败永久降级）。
    // 申报偏差：xdg-shell 不暴露窗口屏幕原点 ⇒ SCREEN 系几何按窗口本地 px 如实申报。
    std::unique_ptr<detail::AtspiBridge> atspi;
    bool atspi_attempted = false;
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
    // ---- 光标形状（客户端主题光标：libwayland-cursor 取位图 → cursor wl_surface → set_cursor）----
    CursorShape pending_cursor_shape = CursorShape::Arrow;  ///< 期望的语义形状（set_cursor 落盘）。
    std::uint32_t pointer_enter_serial = 0;  ///< 最近 wl_pointer.enter 的 serial（wl_pointer_set_cursor 必需）。
    wl_cursor_theme *cursor_theme = nullptr;  ///< 主题句柄（按 24×scale 设备像素加载，scale 变即重载）。
    int cursor_theme_size = 0;  ///< 已加载主题的尺寸（设备像素；0 = 未加载）。
    wl_surface *cursor_surface = nullptr;  ///< 专用于光标的独立表面（随实例复用，仅在首次下发时创建）。
    bool cursor_applied = false;  ///< 是否已成功提交过一次。
    CursorShape cursor_applied_shape = CursorShape::Arrow;  ///< 最近成功提交的形状（去重用）。
    int cursor_applied_scale = 0;  ///< 最近成功提交时的缩放（缩放变则须重提交）。
    std::string cursor_resolved_name;  ///< 主题侧实际命中的名字（wl_cursor::name）。
    int cursor_image_w = 0;
    int cursor_image_h = 0;
    int cursor_buffer_scale = 1;  ///< cursor 表面的 set_buffer_scale（图像尺寸非 scale 整数倍时退化 1）。
    int cursor_hotspot_x = 0;  ///< 热点（表面逻辑坐标）。
    int cursor_hotspot_y = 0;
    std::uint64_t cursor_buffer_id = 0;  ///< 提交的 wl_buffer 身份（指针值；主题持有，勿销毁）。
    int cursor_image_count = 0;  ///< 动画帧数（>1 = 动画光标，本端只取首帧）。
    int cursor_commits = 0;  ///< cursor 表面 commit 次数（观测面：证明「确有提交」且同形状幂等）。
    bool cursor_theme_warned = false;  ///< 主题/名字彻底缺失只 WARN 一次（不在悬停热路径刷屏）。
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    // ---- 输入法桥（text-input-unstable-v3）----
    // 与 X11 XIM / Win32 IME 的「服务端拥有输入」不同，v3 的输入焦点由客户端**声明**：
    // 每帧 present 拉取 caret provider（非零盒 = 焦点在文本控件）决定 enable/disable，
    // 组合/上屏事件由合成器侧输入法经 preedit_string/commit_string 回推。
    zwp_text_input_v3 *text_input = nullptr;  ///< 本 seat 的 text-input 对象（keyboard 能力首现时创建）。
    bool ti_entered = false;  ///< 收到过 enter（本表面持键盘输入焦点）。
    bool ti_ime_wanted = false;  ///< 键盘输入焦点在本表面（wl_keyboard 与 text-input 任一 leave 即 false：此刻 provider
                                 ///< 仍报非零盒，须强制 disable）。
    bool ti_enabled = false;  ///< 当前 enable 态（与 provider 判据同步去重）。
    std::string ti_preedit;  ///< 最近 preedit_string 原文（观测面 + leave 时清空）。
    Rect ti_last_caret{};  ///< 上次 set_cursor_rectangle 的盒（逻辑 dp，去重用）。
    int ti_commits = 0;  ///< 客户端 commit 轮次（观测面）。
    int ti_delete_requests = 0;  ///< delete_surrounding_text 折算事件数（观测面）。
    std::function<Rect()> composition_caret_provider;  ///< 宿主注入的插入点查询（窗口逻辑 dp）。

    /// @brief 依 provider 判据同步 enable/disable 态并发出客户端 commit（每组状态变更批量生效一次）。
    auto ti_refresh_enable() -> void;
    /// @brief 拉取插入点盒 → `set_cursor_rectangle`（表面本地物理 px；盒变化才发）。
    auto ti_update_cursor_rect() -> void;
    /// @brief text-input.enter：记entered，立即补发内容类型并刷新 enable 判据。
    auto ti_on_enter() -> void;
    /// @brief text-input.leave：输入焦点易主——清组合态、置不可用（v3 要求重入后全量重发）。
    auto ti_on_leave() -> void;
    /// @brief preedit_string：UTF-8 字节下标折算码点契约后经 handler 上抛组合事件。
    auto ti_on_preedit(const char *text, std::int32_t begin, std::int32_t end) -> void;
    /// @brief commit_string：上屏串经组合事件的 committed 通道落字（与 Win32 桥同一收敛路径）。
    auto ti_on_commit(const char *text) -> void;
    /// @brief delete_surrounding_text：折算为 Left×n + Backspace×m + Right×k 键事件（本端不回传
    /// surrounding text，恒为空上下文，n/k 实际恒 0，m 即整段删除）。
    auto ti_on_delete(std::uint32_t before_length, std::uint32_t after_length) -> void;
#endif
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
            // 光标主题按设备像素加载：缩放变了旧主题的位图就不再匹配，立即重载并重下发
            // （force=true 跨过「同形状同缩放」去重——此处缩放恰已变，去重键本身也已失效）。
            apply_cursor(true);
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

    /// @brief 光标逻辑尺寸（dp）：主题按 `AURORA_CURSOR_SIZE * scale` 设备像素加载，与 Win32/X11 的系统光标同量级。
    static constexpr int AURORA_CURSOR_SIZE = 24;
    /// @brief 确保主题已按当前缩放加载（scale 变化即销毁重载）。返回是否可用。
    auto ensure_cursor_theme() -> bool;
    /// @brief 把 `pending_cursor_shape` 的主题位图提交到 cursor 表面并交回合成器。
    /// 位图直接用 `wl_cursor_image_get_buffer()` 返回的主题自有 ARGB `wl_buffer`（本端不再二次上传）。
    /// @param force 忽略「同形状同缩放」去重（`wl_pointer.enter` 后必须走这条：合成器已回到默认光标）。
    /// @return 是否已提交（false = 资源/serial 未就绪，或主题彻底缺名字）。
    auto apply_cursor(bool force = false) -> bool;

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
    /// 请求立即重绘（嵌套类可访基类 protected 的 present_request_；供匿名空间自由函数复用）。
    auto request_repaint() -> void {
        if (self != nullptr && self->present_request_) {
            self->present_request_();
        }
    }
    auto draw_decoration(Painter &p) const -> void;  ///< 自绘装饰：标题栏（csd_title）+ 边框（csd_border）
    /// @brief 装配本帧装饰绘制状态（软件光栅与 GPU 录制两条路径共用的唯一装配点）。
    [[nodiscard]] auto decoration_state() const -> csd::TitleBarPaintState;
    /// @brief 装饰录制专用 Painter：不复用 `painter`——app 帧录制期间其录制栈非空，嵌套会污染帧 DL。
    Painter deco_recorder;
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
void ptr_enter(void *data, wl_pointer * /*p*/, std::uint32_t serial, wl_surface * /*s*/, wl_fixed_t sx, wl_fixed_t sy) {
    Impl &d = *static_cast<Impl *>(data);
    // 光标形状：捕获本次 enter 的 serial——wl_pointer.set_cursor 只接受 enter（或已有焦点）时的
    // serial，故 set_cursor 早于 enter 时须在此补一次下发；且合成器在指针重新进入表面时会回到
    // 默认光标，故每次 enter 都强制重下发（跨过同形状去重）。
    d.pointer_enter_serial = serial;
    d.apply_cursor(true);
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
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    d.ti_ime_wanted = true;  // 键盘焦点回到本表面：立刻按 provider 判据恢复 enable（不等下一帧）
    d.ti_refresh_enable();
#endif
}

void kb_leave(void *data, wl_keyboard * /*k*/, std::uint32_t /*serial*/, wl_surface * /*s*/) {
    Impl &d = *static_cast<Impl *>(data);
    d.active = false;
    d.update_state();
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    // 键盘输入焦点易主：显式 disable，否则输入法组合仍会打进本表面（v3 焦点由客户端声明）；
    // 此刻上层焦点未变、provider 仍报非零盒，故必须先翻 ti_ime_wanted 再刷新。
    d.ti_ime_wanted = false;
    d.ti_refresh_enable();
#endif
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

#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
// ---- zwp_text_input_v3：输入法组合通道（enter/leave/preedit/commit/delete/done）。 ----
void ti_enter(void *data, zwp_text_input_v3 * /*ti*/, wl_surface * /*s*/) { static_cast<Impl *>(data)->ti_on_enter(); }

void ti_leave(void *data, zwp_text_input_v3 * /*ti*/, wl_surface * /*s*/) { static_cast<Impl *>(data)->ti_on_leave(); }

void ti_preedit(void *data, zwp_text_input_v3 * /*ti*/, const char *text, std::int32_t begin, std::int32_t end) {
    static_cast<Impl *>(data)->ti_on_preedit(text, begin, end);
}

void ti_commit(void *data, zwp_text_input_v3 * /*ti*/, const char *text) {
    static_cast<Impl *>(data)->ti_on_commit(text);
}

void ti_delete(void *data, zwp_text_input_v3 * /*ti*/, std::uint32_t before_length, std::uint32_t after_length) {
    static_cast<Impl *>(data)->ti_on_delete(before_length, after_length);
}

void ti_done(void * /*data*/, zwp_text_input_v3 * /*ti*/, std::uint32_t /*serial*/) {
    // 服务端状态序列到此结束：插入点盒在 preedit 更新后可能已移位，随下一帧 present 的
    // set_cursor_rectangle 统一刷新，此处无须动作。
}

constexpr zwp_text_input_v3_listener TEXT_INPUT_LISTENER = {ti_enter,  ti_leave,  ti_preedit,
                                                            ti_commit, ti_delete, ti_done};
#endif

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
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    } else if (std::strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0 && text_input_mgr == nullptr) {
        text_input_mgr = static_cast<zwp_text_input_manager_v3 *>(
            wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1U));
#endif
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
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
        // text-input 对象按 seat 建（v3 与键盘能力同步出现；一窗口一 seat 的简化模型下即一实例）。
        if (text_input_mgr != nullptr && text_input == nullptr) {
            text_input = zwp_text_input_manager_v3_get_text_input(text_input_mgr, seat);
            zwp_text_input_v3_add_listener(text_input, &TEXT_INPUT_LISTENER, this);
        }
#endif
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

auto WaylandSurface::Impl::ensure_cursor_theme() -> bool {
    const int want = AURORA_CURSOR_SIZE * std::max(1, scale);  // 主题按设备像素加载
    if (cursor_theme != nullptr && cursor_theme_size == want) {
        return true;
    }
    if (cursor_theme != nullptr) {
        // 旧主题持有全部光标位图的 wl_buffer（cursor_surface 正 attach 着其中一个）；销毁后必须
        // 让下一次 apply 重新提交，否则 surface 上留着已失效的 buffer 身份。
        wl_cursor_theme_destroy(cursor_theme);
        cursor_theme = nullptr;
        cursor_theme_size = 0;
        cursor_applied = false;
    }
    if (shm == nullptr) {
        return false;
    }
    // 首参 nullptr = 跟随桌面主题（$XCURSOR_THEME，缺省 default → 继承链落到具体主题目录）。
    cursor_theme = wl_cursor_theme_load(nullptr, want, shm);
    if (cursor_theme == nullptr) {
        if (!cursor_theme_warned) {
            cursor_theme_warned = true;
            AURORA_LOG_WARN("window", "WaylandSurface: wl_cursor_theme_load(size=", want,
                            ") failed (no icon theme installed); cursor shapes stay on the "
                            "compositor default.");
        }
        return false;
    }
    cursor_theme_size = want;
    return true;
}

auto WaylandSurface::Impl::apply_cursor(bool force) -> bool {
    if (dpy == nullptr || compositor == nullptr || shm == nullptr || pointer == nullptr || !configured) {
        return false;
    }
    if (pointer_enter_serial == 0U) {
        return false;  // 指针尚未进入本表面：无合法 serial，留待 ptr_enter 内补下发
    }
    if (!force && cursor_applied && cursor_applied_shape == pending_cursor_shape && cursor_applied_scale == scale) {
        return true;  // 同一焦点期内同形状同缩放：已上屏，幂等不再 commit
    }
    if (!ensure_cursor_theme()) {
        return false;
    }
    const char *const want = cursor_rfc_name(pending_cursor_shape);
    wl_cursor *cur = wl_cursor_theme_get_cursor(cursor_theme, want);
    for (const char *fb : {"default", "left_ptr"}) {
        if (cur != nullptr) {
            break;
        }
        cur = wl_cursor_theme_get_cursor(cursor_theme, fb);  // 主题缺该形状：回退箭头语义
    }
    if (cur == nullptr || cur->image_count == 0U || cur->images[0] == nullptr) {
        if (!cursor_theme_warned) {
            cursor_theme_warned = true;
            AURORA_LOG_WARN("window", "WaylandSurface: cursor theme resolved neither \"", want,
                            "\" nor the default/left_ptr fallback; keeping the compositor cursor.");
        }
        return false;
    }
    wl_cursor_image *img = cur->images[0];  // 动画光标（wait/watch）取首帧静态图，本端不做逐帧重提交
    wl_buffer *buf = wl_cursor_image_get_buffer(img);
    if (buf == nullptr) {
        return false;
    }
    if (cursor_surface == nullptr) {
        cursor_surface = wl_compositor_create_surface(compositor);  // 一个实例一个，随实例复用
        if (cursor_surface == nullptr) {
            return false;
        }
    }
    const int w = static_cast<int>(img->width);
    const int h = static_cast<int>(img->height);
    if (w <= 0 || h <= 0) {
        return false;
    }
    // 仅当主题真的给出了高 DPI 位图（尺寸达设备像素目标且可被 scale 整除）才按 scale 上报缓冲
    // 缩放；否则退回 1x——宁可在缺尺寸的主题下偏小，也不把热点折算到图像之外。
    const int bscale =
        (scale > 1 && w % scale == 0 && h % scale == 0 && std::max(w, h) >= cursor_theme_size) ? scale : 1;
    if (compositor_version >= 3U) {
        wl_surface_set_buffer_scale(cursor_surface, bscale);
    }
    wl_surface_attach(cursor_surface, buf, 0, 0);
    if (compositor_version >= 3U) {
        wl_surface_damage_buffer(cursor_surface, 0, 0, w, h);  // buffer 坐标
    } else {
        wl_surface_damage(cursor_surface, 0, 0, w / bscale, h / bscale);  // 表面坐标（bscale 恒 1）
    }
    wl_surface_commit(cursor_surface);
    // 热点是图像设备像素，须按 bscale 折算成表面逻辑坐标（与 attach 的 buffer_scale 同口径）。
    wl_pointer_set_cursor(pointer, pointer_enter_serial, cursor_surface,
                          static_cast<std::int32_t>(img->hotspot_x) / bscale,
                          static_cast<std::int32_t>(img->hotspot_y) / bscale);
    wl_display_flush(dpy);
    cursor_applied = true;
    cursor_applied_shape = pending_cursor_shape;
    cursor_applied_scale = scale;
    cursor_resolved_name = (cur->name != nullptr) ? cur->name : want;
    cursor_image_w = w;
    cursor_image_h = h;
    cursor_buffer_scale = bscale;
    cursor_hotspot_x = static_cast<int>(img->hotspot_x) / bscale;
    cursor_hotspot_y = static_cast<int>(img->hotspot_y) / bscale;
    cursor_buffer_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buf));
    cursor_image_count = static_cast<int>(cur->image_count);
    ++cursor_commits;
    return true;
}

#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
// =============================================================================
// 输入法桥（text-input-unstable-v3）：状态声明 + 组合事件折算
// =============================================================================

auto WaylandSurface::Impl::ti_refresh_enable() -> void {
    if (text_input == nullptr) {
        return;
    }
    // 判据 = 「键盘焦点在本表面」且「宿主 provider 报非零插入点盒（焦点在文本控件）」。
    // provider 未接线时视为不接管（保持 disable，与 Win32 未接 provider 时仅剩系统默认行为对齐）。
    bool want = false;
    if (ti_ime_wanted && composition_caret_provider) {
        const Rect caret = composition_caret_provider();
        want = caret.size.width > 0.0F && caret.size.height > 0.0F;
    }
    if (want == ti_enabled) {
        return;  // 去重：同帧重复刷新不发协议请求
    }
    ti_enabled = want;
    if (want) {
        zwp_text_input_v3_enable(text_input);
        // 内容类型随 enable 一次性声明：本库当前只投正常文本（密码框等专用控件未落地，
        // HIDDEN_TEXT/PASSWORD 留待其出现时按控件自描述映射）。
        zwp_text_input_v3_set_content_type(text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                           ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    } else {
        zwp_text_input_v3_disable(text_input);
    }
    zwp_text_input_v3_commit(text_input);
    ++ti_commits;
    wl_display_flush(dpy);
}

auto WaylandSurface::Impl::ti_update_cursor_rect() -> void {
    if (text_input == nullptr || !ti_enabled || !composition_caret_provider) {
        return;
    }
    const Rect box = composition_caret_provider();
    if (box.origin.x == ti_last_caret.origin.x && box.origin.y == ti_last_caret.origin.y &&
        box.size.width == ti_last_caret.size.width && box.size.height == ti_last_caret.size.height) {
        return;  // 盒未变：不重复声明（每帧 present 都会走此路径）
    }
    ti_last_caret = box;
    // 逻辑 dp → 表面本地物理 px（set_cursor_rectangle 用表面坐标×buffer_scale 同口径）。
    const auto sc = static_cast<float>(scale);
    const auto x = static_cast<std::int32_t>(std::floor(box.origin.x * sc));
    const auto y = static_cast<std::int32_t>(std::floor(box.origin.y * sc));
    const auto w = static_cast<std::int32_t>(std::ceil(box.size.width * sc));
    const auto h = static_cast<std::int32_t>(std::ceil(box.size.height * sc));
    zwp_text_input_v3_set_cursor_rectangle(text_input, x, y, w, h);
    zwp_text_input_v3_commit(text_input);
    ++ti_commits;
}

auto WaylandSurface::Impl::ti_on_enter() -> void {
    ti_entered = true;
    ti_ime_wanted = true;
    // enter 后服务端状态视图重建（v3 语义）：清去重缓存，令 enable/content_type/盒随下次刷新重发。
    ti_enabled = false;
    ti_last_caret = Rect{};
    ti_refresh_enable();
}

auto WaylandSurface::Impl::ti_on_leave() -> void {
    ti_entered = false;
    ti_ime_wanted = false;
    ti_enabled = false;
    ti_last_caret = Rect{};
    // 组合中断：清空显示态并通知控件（与 Win32 WM_IME_COMPOSITION 空串、失焦取消同收敛路径）。
    if (!ti_preedit.empty()) {
        ti_preedit.clear();
        if (handler) {
            TextCompositionEvent e;  // 全默认字段 = preedit 空 + committed 空 → 控件 cancel_composition
            handler(e);
        }
    }
}

auto WaylandSurface::Impl::ti_on_preedit(const char *text, std::int32_t begin, std::int32_t end) -> void {
    const std::string_view sv = (text != nullptr) ? std::string_view{text} : std::string_view{};
    ti_preedit.assign(sv);
    if (!handler) {
        return;
    }
    // 字节下标 → 码点契约（ime::make_preedit_state_utf8：begin==end 为光标、异号为选区、
    // 双 -1 为隐藏光标——按「串尾无选区」折算）。
    TextCompositionEvent e = ime::make_preedit_state_utf8(sv, begin, begin, end);
    handler(e);
}

auto WaylandSurface::Impl::ti_on_commit(const char *text) -> void {
    ti_preedit.clear();  // 上屏即组合结束
    if (text == nullptr || text[0] == '\0' || !handler) {
        return;  // 空 commit（纯清 preedit 场景）不发事件
    }
    // 与 Win32 桥同一收敛路径：committed 走组合事件通道落字并通知组合结束（preedit 置空）。
    TextCompositionEvent e;
    e.committed = text;
    handler(e);
}

auto WaylandSurface::Impl::ti_on_delete(std::uint32_t before_length, std::uint32_t after_length) -> void {
    // 本端从不 set_surrounding_text（空上下文）：before/after 理论上恒为 0，非 0 即合成器/
    // 输入法基于我们给过的视图请求回删。折算成编辑键事件交控件处理（与物理退格同一派发路径，
    // 选择/剪贴板逻辑不旁路）。上限防恶意大值刷帧。
    ti_delete_requests += static_cast<int>(before_length + after_length);
    if (!handler) {
        return;
    }
    // 语义 = 「删掉光标前 before_length、光标后 after_length 字节」：先左移再退格等效。
    constexpr std::uint32_t AURORA_CAP = 64;
    for (std::uint32_t i = 0; i < std::min(before_length, AURORA_CAP); ++i) {
        KeyEvent left;
        left.action = KeyAction::Down;
        left.key = static_cast<int>(KeyCode::ArrowLeft);
        handler(left);
        KeyEvent up = left;
        up.action = KeyAction::Up;
        handler(up);
    }
    for (std::uint32_t i = 0; i < std::min(after_length, AURORA_CAP); ++i) {
        KeyEvent bs;
        bs.action = KeyAction::Down;
        bs.key = static_cast<int>(KeyCode::Backspace);
        handler(bs);
        KeyEvent up = bs;
        up.action = KeyAction::Up;
        handler(up);
    }
}
#endif

// =============================================================================
// WaylandSurface：构造/析构与 Surface 接口实现
// =============================================================================

WaylandSurface::WaylandSurface(int w, int h, const std::string &title, const WindowStyleOptions &style,
                               WindowVisibility visibility)
    : impl_(std::make_unique<Impl>()) {
    Impl &d = *impl_;
    d.self = this;
    d.visibility = visibility;
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
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
        // text-input 先于 seat/keyboard 销毁（它是 seat 的子对象）；manager 是 registry 子对象，最后销毁。
        if (d.text_input != nullptr) {
            zwp_text_input_v3_destroy(d.text_input);
            d.text_input = nullptr;
        }
        if (d.text_input_mgr != nullptr) {
            zwp_text_input_manager_v3_destroy(d.text_input_mgr);
            d.text_input_mgr = nullptr;
        }
#endif
        // cursor 表面先于主题销毁：cursor_surface 附着的是主题自有的 wl_buffer，主题销毁会连带
        // 释放它（表面被销毁时合成器已解除引用，顺序上无协议风险，但显式先表面后主题更易读）。
        if (d.cursor_surface != nullptr) {
            wl_surface_destroy(d.cursor_surface);
        }
        if (d.cursor_theme != nullptr) {
            wl_cursor_theme_destroy(d.cursor_theme);
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

auto WaylandSurface::Impl::decoration_state() const -> csd::TitleBarPaintState {
    csd::TitleBarPaintState s;
    s.width = static_cast<float>(size.width);
    s.mode = mode;
    s.fullscreen_bar_revealed = fs_bar_revealed;
    s.title_bar = csd_title;
    s.active = active;
    s.resizable = resizable;
    s.hovered_button = hovered_btn;  // 序号约定：0=min / 1=max / 2=close（ptr_motion 命中写入）
    s.title = title;
    s.icon = tb_icon;
    s.style = tb_style;
    return s;
}

auto WaylandSurface::Impl::draw_decoration(Painter &p) const -> void {
    // 绘制实现收敛在 `csd::paint_title_bar`——GPU 路径（WgpuWaylandSurface）录制同一份内容进帧，
    // 两条上屏路径必须画同一套装饰，故此处只装配状态、不持有绘制代码。
    csd::paint_title_bar(p, decoration_state());
}

auto WaylandSurface::record_client_decoration(DisplayList &dl) -> bool {
    Impl &d = *impl_;
    const csd::TitleBarPaintState s = d.decoration_state();
    if (!s.paints_anything()) {
        return false;
    }
    // 独立录制 Painter（见 Impl::deco_recorder 注）：调用点在外层 app 帧录制栈之上，二者互不干扰。
    d.deco_recorder.record(dl);
    csd::paint_title_bar(d.deco_recorder, s);
    d.deco_recorder.stop();
    return true;
}

auto WaylandSurface::present() -> Result<bool> {
    Impl &d = *impl_;
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    // IME 状态声明先于帧几何早退：插入点盒是「输入态」而非「像素」，即便本帧 attach 被 configure
    // 打断丢弃，enable 判据与候选窗位置也不该顺延一帧（内部去重，稳态零请求）。
    if (d.dpy != nullptr) {
        refresh_ime_input_state();
    }
#endif
    if (d.dpy != nullptr && d.configured && d.painter.data() != nullptr) {
        const int w = d.painter.width();  // 物理像素（Painter 按 scale 放大分配）
        const int h = d.painter.height();
        Impl::Slot *slot = d.pick_slot(w, h);
        if (slot == nullptr) {
            return make_error(ErrorCode::PlatformUnavailable, "WaylandSurface::present: no free wl_shm buffer slot.",
                              "Compositor may be unresponsive; retry next frame.", "aurora/window/wayland_surface.h");
        }
        // 本帧几何是否仍与合成器的 configure 态吻合：begin_frame 之后、attach 之前唯一的派发点是
        // pick_slot 的 roundtrip，其间的 xdg_toplevel.configure 会把 size 改成新尺寸，而它请求的同步重绘
        // 又被 present_root 的重入护栏吞掉，于是 painter/缓冲槽仍按旧几何分配。这副「旧尺寸 buffer + 新
        // configure 态」提交上去会被合成器判为协议错误并杀连接（Weston 实测报文的尺寸对即为
        // 「stale buffer vs 新 configure」）。故丢帧不 attach（脏区原样留给补帧），下次事件泵补一帧。
        const int want_w = static_cast<int>(std::lround(d.size.width)) * d.scale;
        const int want_h = static_cast<int>(std::lround(d.size.height)) * d.scale;
        if (w != want_w || h != want_h) {
            d.present_stale = true;
            return Result<bool>{true};
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
        // 可见性：Hidden 档跳过 attach + commit——表面永不进入合成器视野；帧缓冲仍照常
        // swizzle/更新，data() 读回不受影响。构造期那次「无缓冲 commit 宣告表面存在」必须保留
        // （xdg-shell 要求先收 configure 才能 attach），故只在此处拦截。
        // NoActivate 在 xdg-shell 无对应请求，与 Normal 同路。
        if (d.visibility != WindowVisibility::Hidden) {
            if (d.compositor_version >= 3U) {
                wl_surface_set_buffer_scale(d.surface, d.scale);
            }
            wl_surface_attach(d.surface, slot->buf, 0, 0);
            slot->busy = true;
            wl_surface_commit(d.surface);
            ++d.presented;  // 只计真正提交给合成器的帧
            wl_display_flush(d.dpy);
        }
    }
    // 脏区一次性消费：下一帧未重新设置则回到全量（安全兜底）。
    d.present_dirty.clear();
    return Result<bool>{true};
}

auto WaylandSurface::size() const -> Size { return impl_->size; }

auto WaylandSurface::frame_count() const -> int { return impl_->presented; }

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
    if (d.atspi != nullptr) {
        d.atspi->set_window_title(title);  // FRAME 节点 Name（AT 客户端读回窗口标题）
    }
}

auto WaylandSurface::accessibility_provider() const -> a11y::Provider * { return impl_->atspi.get(); }

auto WaylandSurface::set_accessibility_root(Widget *root) -> void {
    Impl &d = *impl_;
    if (!d.atspi_attempted) {
        // 首帧根注入时构造桥（一次性尝试：失败 = 永久降级，无总线/无 libdbus 是常态）。
        d.atspi_attempted = true;
        detail::AtspiEnv env;
        env.app_name = "Aurora";
        env.window_title = d.title;
        env.toolkit_version = AURORA_VERSION_STRING;
        // DIP（表面逻辑坐标）→ 表面本地物理 px：原点向下取整、终点向上取整。SCREEN 系
        // 与 WINDOW 系同源（协议不暴露屏幕原点，如实申报窗口本地口径）。
        env.to_window_px = [&d](const Rect &r) -> detail::AtspiRectI {
            const auto sc = static_cast<float>(d.scale);
            const auto l = static_cast<std::int32_t>(std::floor(r.origin.x * sc));
            const auto t = static_cast<std::int32_t>(std::floor(r.origin.y * sc));
            const auto rr = static_cast<std::int32_t>(std::ceil((r.origin.x + r.size.width) * sc));
            const auto bb = static_cast<std::int32_t>(std::ceil((r.origin.y + r.size.height) * sc));
            return detail::AtspiRectI{.x = l, .y = t, .width = rr - l, .height = bb - t};
        };
        env.to_screen_px = env.to_window_px;
        env.perform = [](Widget *w, const AccessibilityActionRequest &req) -> bool {
            return w != nullptr && w->perform_accessibility_action(req);
        };
        d.atspi = detail::AtspiBridge::create(std::move(env));
    }
    if (d.atspi != nullptr) {
        d.atspi->set_root(root);
    }
}

auto WaylandSurface::set_cursor(CursorShape shape) -> void {
    // 光标形状：落盘期望形状后即刻尝试下发（详见公共头 set_cursor 的协议说明）。
    // 尝试可能空手而归——指针还没进过本表面（无 enter serial）或合成器无指针，此时形状留在
    // pending 里，ptr_enter / refresh_scale 会补下发，调用方无须重试。
    Impl &d = *impl_;
    d.pending_cursor_shape = shape;
    if (!d.apply_cursor()) {
        AURORA_LOG_DEBUG("wayland_surface", "set_cursor(", cursor_rfc_name(shape),
                         ") deferred (enter_serial=", d.pointer_enter_serial, ")");
    }
}

auto WaylandSurface::cursor_state() const -> CursorState {
    const Impl &d = *impl_;
    CursorState s;
    s.applied = d.cursor_applied;
    s.pointer_entered = (d.pointer_enter_serial != 0U);
    s.shape = d.cursor_applied_shape;
    s.resolved_name = d.cursor_resolved_name;
    s.image_width = d.cursor_image_w;
    s.image_height = d.cursor_image_h;
    s.buffer_scale = d.cursor_buffer_scale;
    s.hotspot_x = d.cursor_hotspot_x;
    s.hotspot_y = d.cursor_hotspot_y;
    s.buffer_id = d.cursor_buffer_id;
    s.image_count = d.cursor_image_count;
    s.theme_size = d.cursor_theme_size;
    s.commits = d.cursor_commits;
    return s;
}

auto WaylandSurface::set_composition_caret_provider(std::function<Rect()> provider) -> void {
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    impl_->composition_caret_provider = std::move(provider);
    // 接线当下即刷一次：键盘焦点可能早已在本表面（首帧前 Tab 进文本框），不等下一帧。
    impl_->ti_refresh_enable();
#else
    (void)provider;  // 协议代码生成缺席：桥不存在，契约退化为基类 no-op
#endif
}

auto WaylandSurface::text_input_state() const -> TextInputState {
    TextInputState s;
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    const Impl &d = *impl_;
    s.manager_bound = (d.text_input_mgr != nullptr);
    s.input_created = (d.text_input != nullptr);
    s.entered = d.ti_entered;
    s.enabled = d.ti_enabled;
    s.preedit = d.ti_preedit;
    s.commits = d.ti_commits;
    s.delete_requests = d.ti_delete_requests;
#else
    s.protocol_disabled = true;
#endif
    return s;
}

auto WaylandSurface::refresh_ime_input_state() -> void {
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    impl_->ti_refresh_enable();
    impl_->ti_update_cursor_rect();
#endif
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
    static constexpr xdg_toplevel_resize_edge AURORA_MAP[] = {
        XDG_TOPLEVEL_RESIZE_EDGE_NONE,      XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
        XDG_TOPLEVEL_RESIZE_EDGE_LEFT,      XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,
        XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
    };
    const auto idx = static_cast<std::size_t>(edge);
    if (idx == 0 || idx >= std::size(AURORA_MAP) || impl_->toplevel == nullptr || impl_->seat == nullptr) {
        return;
    }
    xdg_toplevel_resize(impl_->toplevel, impl_->seat, impl_->last_press_serial, AURORA_MAP[idx]);
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

auto WaylandSurface::native_display() const -> void * { return static_cast<void *>(impl_->dpy); }

auto WaylandSurface::uses_client_decorations() const -> bool { return impl_->csd_title || impl_->csd_border; }

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
#if defined(AURORA_HAVE_WL_TEXT_INPUT) && AURORA_HAVE_WL_TEXT_INPUT
    // IME 输入态声明的兜底驱动点：present() 是软件上屏路径，GPU 宿主（WgpuWaylandSurface）的
    // 帧不经过它——本函数两条路径每帧必经（wgpu 宿主原样转发），故 enable 判据与候选窗盒在此
    // 再刷一次（两方法内部去重，稳态零协议流量）。
    refresh_ime_input_state();
#endif
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
    if (d.present_stale) {
        // 上一帧被派发点上的 configure 打断而丢帧（见 present()）：此刻已不在渲染栈内，重绘请求不再被
        // 重入护栏吞掉，故在此补一帧，避免窗口停在旧尺寸画面上直到下次脏帧自发重绘。
        d.present_stale = false;
        d.request_repaint();
    }
}

auto WaylandSurface::wait_events(double timeout_ms) -> void {
    Impl &d = *impl_;
    if (d.atspi != nullptr) {
        d.atspi->pump();  // 先非阻塞消化 AT-SPI 在途消息（wl 事件密集期桥不被饿死）
    }
    if (d.dpy == nullptr || timeout_ms == 0.0 || d.close_requested) {
        return;
    }
    if (wl_display_prepare_read(d.dpy) != 0) {
        return;  // 队列已有未处理事件：立即回到帧循环消费
    }
    wl_display_flush(d.dpy);
    // 无限等待按 1000ms 分段兜底（对齐 Win32/X11）：唤醒渠道丢失也最迟 1s 自然醒。
    const double capped = (timeout_ms < 0.0 || timeout_ms > 1000.0) ? 1000.0 : timeout_ms;
    std::vector<pollfd> fds;
    fds.reserve(2 + (d.atspi != nullptr ? 2 : 0));
    fds.push_back(pollfd{wl_display_get_fd(d.dpy), POLLIN, 0});
    int wake_idx = -1;
    if (d.wake_fd[0] >= 0) {
        wake_idx = static_cast<int>(fds.size());
        fds.push_back(pollfd{d.wake_fd[0], POLLIN, 0});
    }
    if (d.atspi != nullptr) {
        for (const auto &w : d.atspi->poll_watches()) {
            fds.push_back(pollfd{w.fd, w.events, 0});
        }
    }
    const int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(std::ceil(capped)));
    if (rc > 0 && (fds[0].revents & POLLIN) != 0) {
        wl_display_read_events(d.dpy);
    } else {
        wl_display_cancel_read(d.dpy);
    }
    if (rc > 0 && wake_idx >= 0 && (fds[static_cast<std::size_t>(wake_idx)].revents & POLLIN) != 0) {
        char drain[64];
        while (::read(d.wake_fd[0], drain, sizeof(drain)) > 0) {
            // 排干唤醒字节（非阻塞读到 EAGAIN 为止），避免下次 wait 立即空醒。
        }
    }
    if (d.atspi != nullptr) {
        d.atspi->pump();  // watch fd 就绪 ⇒ 读入并派发 AT-SPI 方法调用（应答经同一 fd 写出）
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
