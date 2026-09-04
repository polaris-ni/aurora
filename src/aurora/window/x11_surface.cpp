#include "aurora/window/x11_surface.h"

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

// aurora 头必须先于 Xlib：Xlib 会 #define None/Bool/Status 等通用词为宏，
// 若先包含会污染 aurora 枚举（如 ModifierKey::None）。取值后立即 #undef None。
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/render/png.h"
#include "aurora/window/keysym_map.h"
#include "aurora/window/window_state.h"

namespace {
// X11 的 None 宏与 aurora::ModifierKey::None 冲突：先取值再解除宏定义。
constexpr long k_x_none = None;
}  // namespace
#undef None

namespace aurora {

namespace {

/// @brief 掩码 → 位移：如 red_mask=0xFF0000 → 16。空掩码（异常 Visual）回退默认位移。
auto mask_shift(unsigned long mask, int fallback) -> int {
    if (mask == 0UL) {
        return fallback;
    }
    int s = 0;
    while (((mask >> s) & 1UL) == 0UL) {
        ++s;
    }
    return s;
}

/// @brief 解析 X 资源 `Xft.dpi` 得像素密度（dpi/96）；无声明/异常值回退 1.0。
/// Wayland 会话下 XWayland 会按桌面缩放同步该资源（GNOME/KDE 均如此）。
auto detect_scale(Display *dpy) -> float {
    const char *rm = XResourceManagerString(dpy);
    if (rm == nullptr) {
        return 1.0F;
    }
    const char *p = std::strstr(rm, "Xft.dpi:");
    if (p == nullptr) {
        return 1.0F;
    }
    const double dpi = std::atof(p + 8);
    if (dpi <= 0.0) {
        return 1.0F;
    }
    return std::clamp(static_cast<float>(dpi / 96.0), 0.5f, 4.0F);
}

/// @brief keysym → 平台无关 KeyCode（X11 后端入口；映射逻辑见 detail::keysym_to_keycode）。
auto from_keysym(KeySym ks) -> KeyCode { return detail::keysym_to_keycode(static_cast<unsigned long>(ks)); }

/// @brief XKeyEvent.state → 修饰键位组合（Mod1=Alt、Mod4=Super/Meta，X 惯例）。
auto mods_from_state(unsigned int state) -> ModifierKey {
    ModifierKey m = ModifierKey::None;
    if ((state & ShiftMask) != 0U) {
        m = m | ModifierKey::Shift;
    }
    if ((state & ControlMask) != 0U) {
        m = m | ModifierKey::Control;
    }
    if ((state & Mod1Mask) != 0U) {
        m = m | ModifierKey::Alt;
    }
    if ((state & Mod4Mask) != 0U) {
        m = m | ModifierKey::Meta;
    }
    return m;
}

}  // namespace

/// @brief X11Surface 的全部 Xlib 状态（pimpl）：公共头零 Xlib 依赖。
struct X11Surface::Impl {
    Display *dpy = nullptr;
    ::Window win = 0;
    GC gc = nullptr;
    // 上屏缓存：X 原生序像素缓冲 + 复用 XImage（尺寸变化时重建）。
    XImage *ximage = nullptr;
    std::vector<std::uint32_t> xbuf;
    int img_w = 0;
    int img_h = 0;
    // EWMH / ICCCM atoms（构造时一次性 intern）。
    Atom wm_delete = 0;
    Atom net_wm_state = 0;
    Atom st_hidden = 0;
    Atom st_max_v = 0;
    Atom st_max_h = 0;
    Atom st_fullscreen = 0;
    Atom net_wm_name = 0;
    Atom utf8_string = 0;
    // 输入法（可选；打开失败时退化为 XLookupString latin1 路径）。
    XIM im = nullptr;
    XIC ic = nullptr;
    // 自唤醒管道（request_wake → wait_events poll 立即返回）。
    int wake_fd[2] = {-1, -1};
    // Visual 掩码位移（present swizzle：RGBA → X 原生像素序）。
    int rshift = 16;
    int gshift = 8;
    int bshift = 0;

    Painter painter;
    std::vector<Rect> present_dirty;  ///< 本帧增量上屏脏区（设备坐标；空=全量）。
    Size size{0.0F, 0.0F};  ///< 逻辑 dp（布局用）。
    float scale = 1.0F;
    bool close_requested = false;
    bool active = true;
    bool minimized = false;
    WindowState state = WindowState::Visible;
    WindowMode mode = WindowMode::Normal;
    Surface::EventHandler handler;

    auto apply_title(const std::string &title) -> void;
    auto ensure_image(int w, int h) -> bool;
    auto query_mode() -> WindowMode;
};

/// @brief 设置窗口标题：ICCCM `XStoreName`（latin1 兜底）+ EWMH `_NET_WM_NAME`（UTF-8，现代 WM 优先读）。
auto X11Surface::Impl::apply_title(const std::string &title) -> void {
    Impl &d = *this;
    XStoreName(d.dpy, d.win, title.c_str());
    XChangeProperty(d.dpy, d.win, d.net_wm_name, d.utf8_string, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(title.c_str()), static_cast<int>(title.size()));
}

/// @brief 确保 XImage 与像素缓冲同尺寸（不同则重建）；失败返回 false。
/// XDestroyImage 会 free(data)：销毁前先摘掉 data 指针，缓冲由 vector 持有。
auto X11Surface::Impl::ensure_image(int w, int h) -> bool {
    Impl &d = *this;
    if (d.ximage != nullptr && d.img_w == w && d.img_h == h) {
        return true;
    }
    if (d.ximage != nullptr) {
        d.ximage->data = nullptr;
        XDestroyImage(d.ximage);
        d.ximage = nullptr;
    }
    d.xbuf.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0U);
    const int screen = DefaultScreen(d.dpy);
    d.ximage = XCreateImage(d.dpy, DefaultVisual(d.dpy, screen), static_cast<unsigned int>(DefaultDepth(d.dpy, screen)),
                            ZPixmap, 0, reinterpret_cast<char *>(d.xbuf.data()), static_cast<unsigned int>(w),
                            static_cast<unsigned int>(h), 32, w * 4);
    if (d.ximage == nullptr) {
        return false;
    }
    d.img_w = w;
    d.img_h = h;
    return true;
}

/// @brief RGBA（Painter 序）→ X 原生像素序逐段 swizzle（按 Visual 掩码位移；
/// 常见 BGRX 情形与 Win32 的 RGBA→BGRA 等价，-O3 自动向量化）。
auto swizzle_rows(const std::uint32_t *src, std::uint32_t *dst, std::size_t count, int rs, int gs, int bs) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];  // 小端内存 R,G,B,A → px = A<<24|B<<16|G<<8|R
        const std::uint32_t r = px & 0xFFu;
        const std::uint32_t g = (px >> 8) & 0xFFu;
        const std::uint32_t b = (px >> 16) & 0xFFu;
        dst[i] = (r << rs) | (g << gs) | (b << bs);
    }
}

/// @brief 查询 EWMH `_NET_WM_STATE` 推导窗口几何态（Hidden/Fullscreen/Maximized/Normal）。
auto X11Surface::Impl::query_mode() -> WindowMode {
    Impl &d = *this;
    WindowMode m = WindowMode::Normal;
    Atom actual = 0;
    int fmt = 0;
    unsigned long n = 0;
    unsigned long after = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(d.dpy, d.win, d.net_wm_state, 0, 64, 0, XA_ATOM, &actual, &fmt, &n, &after, &data) ==
            Success &&
        data != nullptr) {
        const Atom *atoms = reinterpret_cast<Atom *>(data);
        bool maxv = false;
        bool maxh = false;
        for (unsigned long i = 0; i < n; ++i) {
            if (atoms[i] == d.st_hidden) {
                m = WindowMode::Minimized;
            } else if (atoms[i] == d.st_fullscreen) {
                m = WindowMode::FullScreen;
            } else if (atoms[i] == d.st_max_v) {
                maxv = true;
            } else if (atoms[i] == d.st_max_h) {
                maxh = true;
            }
        }
        if (m == WindowMode::Normal && maxv && maxh) {
            m = WindowMode::Maximized;
        }
        XFree(data);
    }
    return m;
}

X11Surface::X11Surface(int w, int h, const std::string &title, const WindowStyleOptions &style)
    : impl_(std::make_unique<Impl>()) {
    Impl &d = *impl_;
    // XIM 依赖进程 locale（一次性）：否则 Xutf8LookupString 退化为 latin1，CJK 输入失效。
    static bool locale_done = false;
    if (!locale_done) {
        locale_done = true;
        std::setlocale(LC_CTYPE, "");
        XSetLocaleModifiers("");
    }
    d.dpy = XOpenDisplay(nullptr);
    if (d.dpy == nullptr) {
        AURORA_LOG_WARN("window",
                        "X11Surface: XOpenDisplay failed (DISPLAY unset or unreachable); "
                        "surface unavailable, factory will return an error.");
        return;
    }
    const int screen = DefaultScreen(d.dpy);
    d.scale = detect_scale(d.dpy);
    if (w <= 0) {
        w = 320;
    }
    if (h <= 0) {
        h = 240;
    }
    // DPI 感知：窗口物理尺寸 = 逻辑 dp × scale（对齐 Win32 语义）。
    const int pw = static_cast<int>(std::lround(w * d.scale));
    const int ph = static_cast<int>(std::lround(h * d.scale));
    d.win = XCreateSimpleWindow(d.dpy, RootWindow(d.dpy, screen), 0, 0, static_cast<unsigned int>(pw),
                                static_cast<unsigned int>(ph), 0, BlackPixel(d.dpy, screen), WhitePixel(d.dpy, screen));
    if (d.win == 0) {
        AURORA_LOG_WARN("window", "X11Surface: XCreateSimpleWindow failed; surface unavailable.");
        XCloseDisplay(d.dpy);
        d.dpy = nullptr;
        return;
    }
    XSelectInput(d.dpy, d.win,
                 ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask |
                     PropertyChangeMask);
    // 关闭协议（点「×」经 ClientMessage 通知而非直接断链）。
    d.wm_delete = XInternAtom(d.dpy, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(d.dpy, d.win, &d.wm_delete, 1);
    // EWMH atoms 一次性 intern。
    d.net_wm_state = XInternAtom(d.dpy, "_NET_WM_STATE", 0);
    d.st_hidden = XInternAtom(d.dpy, "_NET_WM_STATE_HIDDEN", 0);
    d.st_max_v = XInternAtom(d.dpy, "_NET_WM_STATE_MAXIMIZED_VERT", 0);
    d.st_max_h = XInternAtom(d.dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", 0);
    d.st_fullscreen = XInternAtom(d.dpy, "_NET_WM_STATE_FULLSCREEN", 0);
    d.net_wm_name = XInternAtom(d.dpy, "_NET_WM_NAME", 0);
    d.utf8_string = XInternAtom(d.dpy, "UTF8_STRING", 0);
    d.apply_title(title);
    // 高级样式映射：置顶 → _NET_WM_STATE_ABOVE；无边框 → _MOTIF_WM_HINTS。
    if (style.always_on_top) {
        Atom above = XInternAtom(d.dpy, "_NET_WM_STATE_ABOVE", 0);
        XChangeProperty(d.dpy, d.win, d.net_wm_state, XA_ATOM, 32, PropModeAppend,
                        reinterpret_cast<unsigned char *>(&above), 1);
    }
    if (style.frameless) {
        struct MotifHints {
            unsigned long flags;
            unsigned long functions;
            unsigned long decorations;
            long input_mode;
            unsigned long status;
        };
        MotifHints hints{2UL /*MWM_HINTS_DECORATIONS*/, 0UL, 0UL /*无装饰*/, 0L, 0UL};
        Atom motif = XInternAtom(d.dpy, "_MOTIF_WM_HINTS", 0);
        XChangeProperty(d.dpy, d.win, motif, motif, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&hints), 5);
    }
    // 尺寸限制（XSizeHints）：不可调大小 → min=max=创建尺寸。
    if (XSizeHints *sh = XAllocSizeHints()) {
        if (!style.resizable) {
            sh->flags = PMinSize | PMaxSize;
            sh->min_width = sh->max_width = pw;
            sh->min_height = sh->max_height = ph;
        } else {
            if (style.min_size.width > 0.0F || style.min_size.height > 0.0F) {
                sh->flags |= PMinSize;
                sh->min_width = static_cast<int>(std::lround(style.min_size.width * d.scale));
                sh->min_height = static_cast<int>(std::lround(style.min_size.height * d.scale));
            }
            if (style.max_size.width > 0.0F || style.max_size.height > 0.0F) {
                sh->flags |= PMaxSize;
                sh->max_width = static_cast<int>(std::lround(style.max_size.width * d.scale));
                sh->max_height = static_cast<int>(std::lround(style.max_size.height * d.scale));
            }
        }
        if (sh->flags != 0) {
            XSetWMNormalHints(d.dpy, d.win, sh);
        }
        XFree(sh);
    }
    // 可检测自动重复：按住键仅收重复 KeyPress，不再收伪 KeyRelease（免抖动过滤）。
    XkbSetDetectableAutoRepeat(d.dpy, 1, nullptr);
    // 输入法（可选）：失败静默，仍有 keysym → KeyEvent 路径。
    d.im = XOpenIM(d.dpy, nullptr, nullptr, nullptr);
    if (d.im != nullptr) {
        d.ic = XCreateIC(d.im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, d.win, nullptr);
    }
    // 自唤醒管道（非阻塞 + CLOEXEC）：request_wake 线程安全写端。
    if (pipe2(d.wake_fd, O_NONBLOCK | O_CLOEXEC) != 0) {
        d.wake_fd[0] = d.wake_fd[1] = -1;
    }
    // Visual 掩码 → swizzle 位移（XWayland/绝大多数 X 服务为 BGRX：r16/g8/b0）。
    Visual *vis = DefaultVisual(d.dpy, screen);
    d.rshift = mask_shift(vis->red_mask, 16);
    d.gshift = mask_shift(vis->green_mask, 8);
    d.bshift = mask_shift(vis->blue_mask, 0);
    d.gc = XCreateGC(d.dpy, d.win, 0, nullptr);
    XMapWindow(d.dpy, d.win);
    XFlush(d.dpy);
    d.size = Size{static_cast<float>(w), static_cast<float>(h)};
}

X11Surface::~X11Surface() {
    Impl &d = *impl_;
    // 先断开全部上层回调再销毁（对齐 Win32Window 析构次序教训：销毁期间不得回调已亡上层）。
    d.handler = nullptr;
    window_state_handler_ = nullptr;
    window_mode_handler_ = nullptr;
    present_request_ = nullptr;
    if (d.ximage != nullptr) {
        d.ximage->data = nullptr;  // 缓冲由 vector 持有，不得让 XDestroyImage free
        XDestroyImage(d.ximage);
    }
    if (d.dpy != nullptr) {
        if (d.ic != nullptr) {
            XDestroyIC(d.ic);
        }
        if (d.im != nullptr) {
            XCloseIM(d.im);
        }
        if (d.gc != nullptr) {
            XFreeGC(d.dpy, d.gc);
        }
        if (d.win != 0) {
            XDestroyWindow(d.dpy, d.win);
        }
        XCloseDisplay(d.dpy);
    }
    if (d.wake_fd[0] >= 0) {
        ::close(d.wake_fd[0]);
    }
    if (d.wake_fd[1] >= 0) {
        ::close(d.wake_fd[1]);
    }
}

auto X11Surface::is_available() const -> bool { return impl_->dpy != nullptr && impl_->win != 0; }

auto X11Surface::begin_frame(int width, int height) -> Result<bool> {
    Impl &d = *impl_;
    // 新帧默认全量上屏；present_root 会在 present 前重新 set_present_dirty。
    d.present_dirty.clear();
    d.painter.set_scale(d.scale);
    int phys_w = width > 0 ? static_cast<int>(std::lround(width * d.scale)) : 0;
    int phys_h = height > 0 ? static_cast<int>(std::lround(height * d.scale)) : 0;
    // 以窗口真实几何为准（ConfigureNotify 后调用方 size 可能滞后一帧），缓冲 1:1 贴窗口。
    if (d.dpy != nullptr && d.win != 0) {
        ::Window root_ret = 0;
        int xr = 0;
        int yr = 0;
        unsigned int cw = 0;
        unsigned int ch = 0;
        unsigned int bw = 0;
        unsigned int depth = 0;
        if (XGetGeometry(d.dpy, d.win, &root_ret, &xr, &yr, &cw, &ch, &bw, &depth) != 0) {
            if (cw > 0) {
                phys_w = static_cast<int>(cw);
            }
            if (ch > 0) {
                phys_h = static_cast<int>(ch);
            }
        }
    }
    if (phys_w <= 0) {
        phys_w = 1;
    }
    if (phys_h <= 0) {
        phys_h = 1;
    }
    if (phys_w != d.painter.width() || phys_h != d.painter.height()) {
        const int lw = static_cast<int>(std::lround(phys_w / d.scale));
        const int lh = static_cast<int>(std::lround(phys_h / d.scale));
        d.painter.begin(lw, lh);
    }
    // 浅色背景：默认文字为黑色，需浅色底才可见（与 Win32/GLFW 后端一致）。
    d.painter.fill_rect(
        Rect{Point{0.0F, 0.0F}, Size{static_cast<float>(d.painter.width()), static_cast<float>(d.painter.height())}},
        Color{245, 245, 247, 255});
    return Result<bool>{true};
}

auto X11Surface::painter() -> Painter & { return impl_->painter; }

#ifdef AURORA_ENABLE_DEBUG
auto X11Surface::data() const -> const std::uint8_t * {
    if (impl_ && impl_->painter.data() != nullptr) {
        return impl_->painter.data();
    }
    return nullptr;
}
#endif

auto X11Surface::capture_window(const std::string &path) -> Result<bool> {
#ifdef AURORA_ENABLE_DEBUG
    Impl &d = *impl_;
    if (d.dpy == nullptr || d.win == 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: X11 surface not available")};
    }
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(d.dpy, d.win, &wa)) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: XGetWindowAttributes failed")};
    }
    const int w = wa.width;
    const int h = wa.height;
    if (w <= 0 || h <= 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: zero-size window")};
    }
    XImage *ximg =
        XGetImage(d.dpy, d.win, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h), AllPlanes, ZPixmap);
    if (ximg == nullptr) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: XGetImage failed")};
    }
    // X 原生像素 → RGBA：按 Visual 掩码提取（XGetPixel 已处理字节序与掩码，慢但正确）。
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const unsigned long rm = ximg->red_mask, gm = ximg->green_mask, bm = ximg->blue_mask;
    auto shift = [](unsigned long mask, int fb) -> int {
        if (mask == 0UL) {
            return fb;
        }
        int s = 0;
        while (((mask >> s) & 1UL) == 0UL) {
            ++s;
        }
        return s;
    };
    const int rs = shift(rm, 16), gs = shift(gm, 8), bs = shift(bm, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned long pix = XGetPixel(ximg, x, y);
            const std::uint8_t r = static_cast<std::uint8_t>((pix & rm) >> rs);
            const std::uint8_t g = static_cast<std::uint8_t>((pix & gm) >> gs);
            const std::uint8_t b = static_cast<std::uint8_t>((pix & bm) >> bs);
            const std::size_t o =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
            rgba[o] = r;
            rgba[o + 1] = g;
            rgba[o + 2] = b;
            rgba[o + 3] = 255;
        }
    }
    XDestroyImage(ximg);
    if (write_png(path.c_str(), w, h, rgba.data())) {
        return Result<bool>{true};
    }
    return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: write_png failed")};
#else
    (void)path;
    return Result<bool>{
        make_error(ErrorCode::GeneralNotSupported, "capture_window: disabled (AURORA_ENABLE_DEBUG not enabled)")};
#endif
}

auto X11Surface::present() -> Result<bool> {
    Impl &d = *impl_;
    if (d.dpy != nullptr && d.win != 0 && d.painter.data() != nullptr) {
        const int w = d.painter.width();
        const int h = d.painter.height();
        if (d.ensure_image(w, h)) {
            const auto *src = reinterpret_cast<const std::uint32_t *>(d.painter.data());
            if (d.present_dirty.empty()) {
                // 全量：整幅 swizzle + 整窗 XPutImage（首帧/尺寸变化/布局帧）。
                swizzle_rows(src, d.xbuf.data(), static_cast<std::size_t>(w) * static_cast<std::size_t>(h), d.rshift,
                             d.gshift, d.bshift);
                XPutImage(d.dpy, d.win, d.gc, d.ximage, 0, 0, 0, 0, static_cast<unsigned int>(w),
                          static_cast<unsigned int>(h));
            } else {
                // 增量：逐脏矩形仅 swizzle + XPutImage 变化区（拖选/局部重绘帧）。
                for (const Rect &r : d.present_dirty) {
                    const int x0 = std::max(0, static_cast<int>(std::floor(r.origin.x)));
                    const int y0 = std::max(0, static_cast<int>(std::floor(r.origin.y)));
                    const int x1 = std::min(w, static_cast<int>(std::ceil(r.right())));
                    const int y1 = std::min(h, static_cast<int>(std::ceil(r.bottom())));
                    if (x0 >= x1 || y0 >= y1) {
                        continue;
                    }
                    for (int y = y0; y < y1; ++y) {
                        const std::size_t off =
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x0);
                        swizzle_rows(src + off, d.xbuf.data() + off, static_cast<std::size_t>(x1 - x0), d.rshift,
                                     d.gshift, d.bshift);
                    }
                    XPutImage(d.dpy, d.win, d.gc, d.ximage, x0, y0, x0, y0, static_cast<unsigned int>(x1 - x0),
                              static_cast<unsigned int>(y1 - y0));
                }
            }
        }
        XFlush(d.dpy);
    }
    // 脏区一次性消费：下一帧未重新设置则回到全量（安全兜底）。
    d.present_dirty.clear();
    return Result<bool>{true};
}

auto X11Surface::size() const -> Size { return impl_->size; }

auto X11Surface::scale_factor() const -> float { return impl_->scale; }

auto X11Surface::should_close() const -> bool { return impl_->close_requested; }

auto X11Surface::set_present_dirty(const std::vector<Rect> &device_rects) -> void {
    impl_->present_dirty = device_rects;
}

auto X11Surface::set_event_handler(const EventHandler &h) -> void { impl_->handler = h; }

auto X11Surface::set_title(const std::string &title) -> void {
    Impl &d = *impl_;
    if (d.dpy != nullptr && d.win != 0) {
        d.apply_title(title);
        XFlush(d.dpy);
    }
}

auto X11Surface::native_handle() const -> void * {
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(impl_->win));
}

auto X11Surface::poll_platform_events() -> void {
    Impl &d = *impl_;
    if (d.dpy == nullptr) {
        return;
    }
    // 可见性状态推导（对齐 Win32 update_window_state）：仅实际变化时上报。
    auto update_state = [&]() -> void {
        const WindowState want = compute_window_state(d.minimized, d.active);
        if (want != d.state) {
            d.state = want;
            notify_window_state(want);
        }
    };
    auto send_mouse = [&](MouseAction action, MouseButton button, float px, float py) -> void {
        if (!d.handler) {
            return;
        }
        MouseEvent e;
        e.action = action;
        e.button = button;
        e.position = Point{px / d.scale, py / d.scale};
        d.handler(e);
    };
    XEvent ev;
    while (XPending(d.dpy) > 0) {
        XNextEvent(d.dpy, &ev);
        if (XFilterEvent(&ev, static_cast<::Window>(k_x_none)) != 0) {
            continue;  // 输入法预编辑消费（如拼音候选期间的按键）
        }
        switch (ev.type) {
            case ButtonPress:
            case ButtonRelease: {
                const bool press = (ev.type == ButtonPress);
                const unsigned int btn = ev.xbutton.button;
                if (btn >= 4 && btn <= 7) {
                    // X 惯例：滚轮为按键 4/5（垂直）与 6/7（水平），仅 Press 有意义。
                    if (press && d.handler) {
                        ScrollEvent se;
                        se.position = Point{static_cast<float>(ev.xbutton.x) / d.scale,
                                            static_cast<float>(ev.xbutton.y) / d.scale};
                        if (btn == 4) {
                            se.delta_y = 1.0F;  // 上滚为正（见 event.h）
                        } else if (btn == 5) {
                            se.delta_y = -1.0F;
                        } else if (btn == 6) {
                            se.delta_x = -1.0F;
                        } else {
                            se.delta_x = 1.0F;
                        }
                        d.handler(se);
                    }
                    break;
                }
                const MouseButton mb = (btn == 3)   ? MouseButton::Right
                                       : (btn == 2) ? MouseButton::Middle
                                                    : MouseButton::Left;
                send_mouse(press ? MouseAction::Press : MouseAction::Release, mb, static_cast<float>(ev.xbutton.x),
                           static_cast<float>(ev.xbutton.y));
                break;
            }
            case MotionNotify: {
                // 移动事件压缩：连续 Motion 只保留最后一个（高频移动下避免逐事件全链派发）。
                while (XPending(d.dpy) > 0) {
                    XEvent nxt;
                    XPeekEvent(d.dpy, &nxt);
                    if (nxt.type == MotionNotify && nxt.xmotion.window == ev.xmotion.window) {
                        XNextEvent(d.dpy, &ev);
                    } else {
                        break;
                    }
                }
                send_mouse(MouseAction::Move, MouseButton::Left, static_cast<float>(ev.xmotion.x),
                           static_cast<float>(ev.xmotion.y));
                break;
            }
            case LeaveNotify:
                // 光标离开窗口：合成一次远离窗口的 Move → 命中空链清除全部悬停态（对齐 WM_MOUSELEAVE）。
                send_mouse(MouseAction::Move, MouseButton::Left, -10000.0F * d.scale, -10000.0F * d.scale);
                break;
            case KeyPress: {
                KeySym ks = 0;
                char buf[64];
                // 输入法提交串可超过栈缓冲：Xutf8LookupString 在 st == XBufferOverflow 时
                // **不写入 buf**，而是返回「所需字节数」。若把该返回值当成已写长度使用，
                // 就会把 buf 之外的未初始化栈内存当作文本交给 TextInput 渲染/复制出去。
                // 溢出时改用堆缓冲重查一次（一句中文即可轻易超过 63 字节，属日常路径）。
                std::vector<char> heap_buf;
                const char *text = buf;
                int len = 0;
                if (d.ic != nullptr) {
                    Status st = 0;
                    len = Xutf8LookupString(d.ic, &ev.xkey, buf, static_cast<int>(sizeof(buf)) - 1, &ks, &st);
                    if (st == XBufferOverflow && len > 0) {
                        heap_buf.resize(static_cast<std::size_t>(len) + 1u);
                        len = Xutf8LookupString(d.ic, &ev.xkey, heap_buf.data(), len, &ks, &st);
                        text = heap_buf.data();
                    }
                    // 兜底：任何情况下都不得把长度超出实际缓冲的值传给 assign。
                    const int cap = static_cast<int>(heap_buf.empty() ? sizeof(buf) - 1 : heap_buf.size() - 1);
                    len = std::clamp(len, 0, cap);
                } else {
                    len = XLookupString(&ev.xkey, buf, static_cast<int>(sizeof(buf)) - 1, &ks, nullptr);
                    len = std::clamp(len, 0, static_cast<int>(sizeof(buf)) - 1);
                }
                if (d.handler) {
                    KeyEvent e;
                    e.action = KeyAction::Down;
                    e.key = static_cast<int>(from_keysym(ks));
                    e.modifiers = mods_from_state(ev.xkey.state);
                    d.handler(e);
                }
                // 可打印文本 → TextInputEvent；控制字符（回车/退格/Esc…）交给 KeyEvent。
                if (d.handler && len > 0 &&
                    !(len == 1 && (static_cast<unsigned char>(text[0]) < 0x20 || text[0] == 0x7F))) {
                    TextInputEvent te;
                    te.text.assign(text, static_cast<std::size_t>(len));
                    d.handler(te);
                }
                break;
            }
            case KeyRelease: {
                if (d.handler) {
                    KeyEvent e;
                    e.action = KeyAction::Up;
                    e.key = static_cast<int>(from_keysym(XLookupKeysym(&ev.xkey, 0)));
                    e.modifiers = mods_from_state(ev.xkey.state);
                    d.handler(e);
                }
                break;
            }
            case ConfigureNotify: {
                const int pw = ev.xconfigure.width;
                const int ph = ev.xconfigure.height;
                if (pw > 0 && ph > 0) {
                    const Size want{static_cast<float>(pw) / d.scale, static_cast<float>(ph) / d.scale};
                    if (want.width != d.size.width || want.height != d.size.height) {
                        d.size = want;
                        // 几何变化当下同步重渲染（对齐 Win32 WM_SIZE）：缩放拖拽期间无黑边/残留。
                        if (present_request_) {
                            present_request_();
                        }
                    }
                }
                break;
            }
            case Expose:
                // 系统要求重绘（遮挡揭开/首次映射）：仅最后一片脏区（count==0）触发一次重呈现。
                if (ev.xexpose.count == 0 && present_request_) {
                    present_request_();
                }
                break;
            case FocusIn:
                d.active = true;
                update_state();
                break;
            case FocusOut:
                d.active = false;
                update_state();
                break;
            case MapNotify:
                d.minimized = false;
                update_state();
                break;
            case UnmapNotify:
                d.minimized = true;
                update_state();
                break;
            case PropertyNotify:
                // EWMH 几何态变化（最小化/最大化/全屏由 WM 改 _NET_WM_STATE 属性通告）。
                if (ev.xproperty.atom == d.net_wm_state) {
                    const WindowMode want = d.query_mode();
                    if (want != d.mode) {
                        d.mode = want;
                        d.minimized = (want == WindowMode::Minimized);
                        notify_window_mode(want);
                        update_state();
                    }
                }
                break;
            case ClientMessage:
                if (static_cast<Atom>(ev.xclient.data.l[0]) == d.wm_delete) {
                    d.close_requested = true;
                }
                break;
            case DestroyNotify:
                d.close_requested = true;
                break;
            default:
                break;
        }
    }
}

auto X11Surface::wait_events(double timeout_ms) -> void {
    Impl &d = *impl_;
    if (d.dpy == nullptr || timeout_ms == 0.0 || d.close_requested) {
        return;
    }
    if (XPending(d.dpy) > 0) {
        return;  // 队列已有未处理事件：立即回到帧循环消费
    }
    // 无限等待按 1000ms 分段兜底（对齐 Win32/默认实现）：唤醒渠道丢失也最迟 1s 自然醒。
    const double capped = (timeout_ms < 0.0 || timeout_ms > 1000.0) ? 1000.0 : timeout_ms;
    struct pollfd fds[2];
    nfds_t n = 0;
    fds[n].fd = ConnectionNumber(d.dpy);
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
    if (rc > 0 && n == 2 && (fds[1].revents & POLLIN) != 0) {
        char drain[64];
        while (::read(d.wake_fd[0], drain, sizeof(drain)) > 0) {
            // 排干唤醒字节（非阻塞读到 EAGAIN 为止），避免下次 wait 立即空醒。
        }
    }
}

auto X11Surface::request_wake() -> void {
    Impl &d = *impl_;
    if (d.wake_fd[1] >= 0) {
        const char b = 1;
        [[maybe_unused]] const ssize_t rc = ::write(d.wake_fd[1], &b, 1);  // 满管道丢弃亦可：已有待读字节必醒
    }
}

}  // namespace aurora

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX
