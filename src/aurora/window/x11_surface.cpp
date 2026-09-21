#include "aurora/window/x11_surface.h"

#include "aurora/core/platform.h"

// AT-SPI 头必须先于 Xlib（与首行 x11_surface.h 同理）：`AtspiPropValue::Kind::None`
// 会被 Xlib 的 `#define None 0L` 污染，且本头不可在 #undef None 之后再包含。
#include "aurora/window/detail/atspi_bridge.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

// aurora 头必须先于 Xlib：Xlib 会 #define None/Bool/Status 等通用词为宏，
// 若先包含会污染 aurora 枚举（如 ModifierKey::None）。取值后立即 #undef None。
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

// X11 的 <X11/X.h>（经 Xlib.h 引入）**无条件**定义对象宏 `CursorShape`：
//   `#define CursorShape 0  /* largest size that can be displayed */`
// 它与本项目公共类型名 `aurora::CursorShape`（core/enums.h）硬碰撞：不解除时该记号一律
// 被预处理器展开为 `0`，使 `CursorShape shape` 变成 `0 shape`，报出很难定位的
// `expected ')' before 'shape'`（仅在本文件真正以 AURORA_BACKEND_X11 编译时暴露）。
// 本项目不使用该 Xlib 常量（光标句柄一律经 XCreateFontCursor + cursorfont.h 的 XC_* 字形创建），
// 故直接解除。必须在 `#include "aurora/window/cursor_map.h"` 之前解除，否则其签名先被宏污染。
#undef CursorShape

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "aurora/core/a11y_text.h"  // a11y::detail::decode_cp（XIM 组合串的码点步进）
#include "aurora/core/log.h"
#include "aurora/core/version.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/render/png.h"
#include "aurora/window/cursor_map.h"
#include "aurora/window/detail/ime_composition.h"
#include "aurora/window/keysym_map.h"
#include "aurora/window/window_state.h"

namespace {
// X11 的 None 宏与 aurora::ModifierKey::None 冲突：先取值再解除宏定义。
constexpr long AURORA_X_NONE = None;
}  // namespace
#undef None

namespace aurora {

namespace {

/// @brief 掩码 → 位移：如 red_mask=0xFF0000 → 16。空掩码（异常 Visual）回退默认位移。
auto mask_shift(unsigned long mask, int fallback) -> int {
    if (mask == 0UL) {
        return fallback;
    }
    unsigned int s = 0;
    while (((mask >> s) & 1UL) == 0UL) {
        ++s;
    }
    return static_cast<int>(s);
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
    const double dpi = std::strtod(p + 8, nullptr);  // NOLINT(*-pro-bounds-pointer-arithmetic)
    if (dpi <= 0.0) {
        return 1.0F;
    }
    return std::clamp(static_cast<float>(dpi / 96.0), 0.5F, 4.0F);
}

/// @brief keysym → 平台无关 KeyCode（X11 后端入口；映射逻辑见 detail::keysym_to_keycode）。
auto from_keysym(KeySym ks) -> KeyCode { return detail::keysym_to_keycode(static_cast<unsigned long>(ks)); }

/// @brief XKeyEvent.state → 修饰键位组合（Mod1=Alt、Mod4=Super/Meta，X 惯例）。
auto mods_from_state(unsigned int state) -> ModifierKey {
    auto m = ModifierKey::None;
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
    // 输入法（XIM，R6 公共头）：`ime_setup()` 协商 XIMPreeditCallbacks 风格（组合事件回推），
    // IM 不支持则回退 XIMPreeditNothing（仅 commit 经 Xutf8LookupString）；XOpenIM 失败静默，
    // 仍有 keysym → KeyEvent 路径。
    XIM im = nullptr;
    XIC ic = nullptr;
    bool ime_cb_style = false;  ///< 协商到 PreeditCallbacks（观测面 + draw 回调仅在 cb 风格下触发）
    std::string ime_preedit;  ///< 最近 draw 回调的组合串（UTF-8，commit/清空时置空）
    std::function<Rect()> composition_caret_provider;  ///< 宿主注入的插入点查询（窗口逻辑 dp）
    int ime_draw_cbs = 0;  ///< preedit draw 回调次数
    int ime_spot_updates = 0;  ///< XNSpotLocation 实际下发次数（去重后）
    /// @brief 建立/重建 IM 通道（构造期与首次 FocusIn 各调一次，幂等）：XOpenIM → 风格协商 →
    /// XCreateIC → 注册 preedit 回调。任一步失败即降级（keysym 路径不受影响）。
    auto ime_setup() -> void;
    /// @brief 组合态变化：UTF-8 字节区间折算码点契约上抛 TextCompositionEvent，并同步锚点。
    auto ime_emit_preedit(const std::string &utf8, int caret, int sel_begin_byte, int sel_end_byte) -> void;
    /// @brief 把 provider 的插入点盒折算为客户窗口物理 px 写入 XNSpotLocation（变化才发）。
    auto ime_update_spot() -> void;
    XPoint ime_last_spot{};  ///< 上次下发的锚点（去重）
    bool ime_spot_valid = false;
    bool ime_focused = false;  ///< XSetICFocus 已发且未 XUnsetICFocus（观测面）
    // 自唤醒管道（request_wake → wait_events poll 立即返回）。
    int wake_fd[2] = {-1, -1};
    // AT-SPI2 无障碍桥（宿主惰性构造：首次 set_accessibility_root 时尝试连接 a11y 总线；
    // 失败永久降级为 nullptr——无 libdbus/无会话总线/NO_AT_BRIDGE 都是 Linux 常态）。
    std::unique_ptr<detail::AtspiBridge> atspi;
    bool atspi_attempted = false;
    std::string title;  ///< 最近标题缓存（桥构造时回填 FRAME 节点 Name）
    int origin_x = 0;  ///< 客户区左上角的屏幕物理 px（ConfigureNotify 时 XTranslateCoordinates）
    int origin_y = 0;
    // 光标形状：`XCreateFontCursor` 句柄按 CursorShape 取值序缓存（0 = 未创建）。
    // 每次创建都是新 X 资源，必须复用；析构统一 XFreeCursor。
    std::array<Cursor, AURORA_CURSOR_SHAPE_COUNT> cursors{};
    // Visual 掩码位移（present swizzle：RGBA → X 原生像素序）。
    int rshift = 16;
    int gshift = 8;
    int bshift = 0;

    Painter painter;
    std::vector<Rect> present_dirty;  ///< 本帧增量上屏脏区（设备坐标；空=全量）。
    Size size{.width = 0.0F, .height = 0.0F};  ///< 逻辑 dp（布局用）。
    float scale = 1.0F;
    bool close_requested = false;
    bool active = true;
    bool minimized = false;
    WindowState state = WindowState::Visible;
    WindowMode mode = WindowMode::Normal;
    EventHandler handler;

    auto apply_title(const std::string &title) -> void;
    auto ensure_image(int w, int h) -> bool;
    auto query_mode() -> WindowMode;
};

/// @brief 设置窗口标题：ICCCM `XStoreName`（latin1 兜底）+ EWMH `_NET_WM_NAME`（UTF-8，现代 WM 优先读）。
auto X11Surface::Impl::apply_title(const std::string &title_in) -> void {
    Impl &d = *this;
    d.title = title_in;  // 缓存给 AT-SPI 桥（FRAME Name；桥可能晚于 set_title 构造）
    XStoreName(d.dpy, d.win, title_in.c_str());
    XChangeProperty(d.dpy, d.win, d.net_wm_name, d.utf8_string, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(title_in.c_str()),  // NOLINT(*-pro-type-reinterpret-cast)
                    static_cast<int>(title_in.size()));
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
                            ZPixmap, 0, reinterpret_cast<char *>(d.xbuf.data()),  // NOLINT(*-pro-type-reinterpret-cast)
                            static_cast<unsigned int>(w), static_cast<unsigned int>(h), 32, w * 4);
    if (d.ximage == nullptr) {
        return false;
    }
    d.img_w = w;
    d.img_h = h;
    return true;
}

/// @brief RGBA（Painter 序）→ X 原生像素序逐段 swizzle（按 Visual 掩码位移；
/// 常见 BGRX 情形与 Win32 的 RGBA→BGRA 等价，-O3 自动向量化）。
static auto swizzle_rows(const std::uint32_t *src, std::uint32_t *dst, std::size_t count, int rs, int gs, int bs)
    -> void {
    for (std::size_t i = 0; i < count; ++i) {
        // NOLINTNEXTLINE(*-pro-bounds-pointer-arithmetic)
        const std::uint32_t px = src[i];  // 小端内存 R,G,B,A → px = A<<24|B<<16|G<<8|R
        const std::uint32_t r = px & 0xFFU;
        const std::uint32_t g = (px >> 8U) & 0xFFU;
        const std::uint32_t b = (px >> 16U) & 0xFFU;
        dst[i] = (r << rs) | (g << gs) | (b << bs);  // NOLINT(*-signed-bitwise, *-pro-bounds-pointer-arithmetic)
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
        // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
        const Atom *atoms = reinterpret_cast<Atom *>(data);
        bool maxv = false;
        bool maxh = false;
        // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
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
        // NOLINTEND(*-pro-bounds-pointer-arithmetic)
        if (m == WindowMode::Normal && maxv && maxh) {
            m = WindowMode::Maximized;
        }
        XFree(data);
    }
    return m;
}

// =============================================================================
// XIM 输入法桥（R6 公共头，零新增依赖）
// =============================================================================

namespace {

/// @brief XIMFeedback 位 → 是否属「目标段」（下划线/三类插入点皆视为高亮中的组合段，与
/// Wayland preedit 高亮段及 Win32 GCS_COMPATTR 目标段口径一致）。
constexpr auto ime_feedback_is_target(int fb) -> bool {
    constexpr int AURORA_BITS = XIMUnderline | XIMPrimary | XIMSecondary | XIMTertiary;
    return (fb & AURORA_BITS) != 0;
}

/// @brief XIMText（multi_byte 或 wide_char）→ UTF-8 串 + 逐码点反馈位。
/// 宽字符路经 wcstombs 走进程 UTF-8 locale（构造期 setlocale(LC_CTYPE, "") 已保证）。
/// 反馈数组按 XIM 惯例与「字符」（此处即码点）一一对应。
auto xim_text_to_utf8(const XIMText *t, std::vector<int> &feedback) -> std::string {
    std::string out;
    feedback.clear();
    if (t == nullptr || t->length == 0U) {
        return out;
    }
    if (t->encoding_is_wchar != 0) {
        const wchar_t *const w = t->string.wide_char;
        std::vector<wchar_t> src(w, w + t->length);  // 拷贝一份保证 NUL 终止（wcstombs 读入参）
        src.push_back(L'\0');
        std::vector<char> tmp(src.size() * 4U + 1U, '\0');
        const std::size_t conv = std::wcstombs(tmp.data(), src.data(), tmp.size() - 1U);  // NOLINT(mt-unsafe)
        if (conv != static_cast<std::size_t>(-1)) {
            out.assign(tmp.data(), conv);
        }  // 非法序列：留空串（宁可不显示也不吐半截字节）
    } else {
        out.assign(t->string.multi_byte, static_cast<std::size_t>(t->length));
    }
    if (t->feedback != nullptr) {
        for (unsigned short i = 0; i < t->length; ++i) {
            // NOLINTNEXTLINE(*-pro-bounds-pointer-arithmetic)
            feedback.push_back(static_cast<int>(t->feedback[i]));
        }
    }
    return out;
}

/// @brief UTF-8 串 → 逐码点起始字节偏移表（含尾哨兵，长度 = 码点数 + 1）。
auto ime_cp_offsets(const std::string &utf8) -> std::vector<std::size_t> {
    std::vector<std::size_t> offs;
    offs.push_back(0);
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto [cp, len] = a11y::detail::decode_cp(utf8, i);
        (void)cp;
        i += (len == 0) ? 1 : len;
        offs.push_back(i);
    }
    return offs;
}

/// @brief XNPreeditDrawCallback 跳板：XIMText → UTF-8 + 首个连续目标码点段（无反馈数组时
/// 全串标为目标段——ibus-x11 常不带 feedback）→ 字节区间交 Impl 折算码点契约。
/// R6 draw 无 action 枚举，全串重发即现状；空 text/零长 = 组合取消。
/// 签名按 XICProc（Bool 返回，值无实义——Xlib 契约对 draw 回调返回值不检查）。
auto ime_preedit_draw(XIC /*ic*/, XPointer cd, XIMPreeditDrawCallbackStruct *r) -> int {
    auto &d = *reinterpret_cast<X11Surface::Impl *>(cd);  // NOLINT(*-pro-type-reinterpret-cast)
    ++d.ime_draw_cbs;
    std::vector<int> fb;
    const std::string utf8 = (r != nullptr) ? xim_text_to_utf8(r->text, fb) : std::string{};
    const std::vector<std::size_t> offs = ime_cp_offsets(utf8);
    const std::size_t cps = offs.size() - 1U;
    std::size_t begin = std::string::npos;
    std::size_t end = 0;
    for (std::size_t c = 0; c < cps && begin == std::string::npos; ++c) {
        if (!ime_feedback_is_target(c < fb.size() ? fb[c] : 0)) {
            continue;
        }
        begin = c;
        end = c + 1;
        while (end < cps && ime_feedback_is_target(end < fb.size() ? fb[end] : 0)) {
            ++end;
        }
    }
    if (fb.empty() && cps > 0) {
        begin = 0;
        end = cps;
    }
    const int sel_begin = (begin == std::string::npos) ? -1 : static_cast<int>(offs[begin]);
    const int sel_end = (begin == std::string::npos) ? -1 : static_cast<int>(offs[end]);
    d.ime_emit_preedit(utf8, (r != nullptr) ? r->caret : -1, sel_begin, sel_end);
    return True;
}

/// @brief XNPreeditCaretCallback 跳板：IM 请求移动组合光标——以现串 + 新位重发并全量接受。
/// @return True = 接受移动（本端无横向滚动，恒接受；False 会让 IM 停在原位）。
auto ime_preedit_caret(XIC /*ic*/, XPointer cd, XIMPreeditCaretCallbackStruct *r) -> int {
    auto &d = *reinterpret_cast<X11Surface::Impl *>(cd);  // NOLINT(*-pro-type-reinterpret-cast)
    d.ime_emit_preedit(d.ime_preedit, (r != nullptr) ? r->position : -1, -1, -1);
    return True;
}

}  // namespace

/// @brief 建立/重建 IM 通道（幂等）：XOpenIM → XNQueryInputStyle 协商 → XCreateIC →
/// 注册 preedit 回调。任一步失败逐级降级：cb 风格不可用回退 PreeditNothing（commit 仍可经
/// Xutf8LookupString 到达），XOpenIM 失败则整桥缺席（keysym 路径不受影响）。
/// 构造期与首次 FocusIn 各调一次——XMODIFIERS/IM 服务器晚就绪是 Linux 桌面常态。
auto X11Surface::Impl::ime_setup() -> void {
    if (dpy == nullptr || win == 0) {
        return;
    }
    if (im == nullptr) {
        im = XOpenIM(dpy, nullptr, nullptr, nullptr);
        if (im == nullptr) {
            return;  // 无 XIM 服务器（XMODIFIERS 未设/IM 未起）：静默降级
        }
    }
    if (ic != nullptr) {
        return;  // 已建过：不重协商（XIM 属性变更须重建，属运维路径而非运行期路径）
    }
    // 协商输入风格：只有 IM 广告 XIMPreeditCallbacks 才走全事件路（draw/caret 回推）。
    // XGetIMValues 契约：成功返回 NULL（输出参数有效），失败返回错误字符串。
    bool cb_style = false;
    if (XIMStyles *styles = nullptr; XGetIMValues(im, XNQueryInputStyle, &styles, nullptr) == nullptr) {
        if (styles != nullptr) {
            // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
            for (int i = 0; i < styles->count_styles; ++i) {
                if (styles->supported_styles[i] == static_cast<unsigned long>(XIMPreeditCallbacks | XIMStatusNothing)) {
                    cb_style = true;
                }
            }
            // NOLINTEND(*-pro-bounds-pointer-arithmetic)
            XFree(styles);
        }
    } else if (styles != nullptr) {
        XFree(styles);  // 失败路径按惯例仍可能带回顾句柄，不留悬挂
    }
    const unsigned long style = cb_style ? static_cast<unsigned long>(XIMPreeditCallbacks | XIMStatusNothing)
                                         : static_cast<unsigned long>(XIMPreeditNothing | XIMStatusNothing);
    ic = XCreateIC(im, XNInputStyle, style, XNClientWindow, win, nullptr);
    if (ic == nullptr) {
        return;  // IM 掉线等极端场景：留 im 句柄，下次 FocusIn 再试
    }
    ime_cb_style = cb_style;
    if (cb_style) {
        // 回调经嵌套属性挂 XNPreeditAttributes（Xlib R6 固定形态）；嵌套列表由 Xlib 分配、
        // XFree 释放。draw 是 void 返回、caret 是 Bool 返回，均按 XICProc 形态注册（Xlib 的
        // XICCallback 联合式分发契约）。client_data 直接指向本 Impl：回调全在事件派发栈内
        // 同步触发，生命周期无忧。
        XICCallback draw{};
        draw.client_data = reinterpret_cast<XPointer>(this);  // NOLINT(*-pro-type-reinterpret-cast)
        // Xlib 把一切 IC 回调统一擦成 XICProc（第三参 XPointer），XIM 规范即要求此形态转换
        // （Qt/GTK 同款），派发时按注册名还原真实结构体指针。
        draw.callback = reinterpret_cast<XICProc>(ime_preedit_draw);  // NOLINT(*-pro-type-reinterpret-cast)
        XICCallback caret{};
        caret.client_data = draw.client_data;
        caret.callback = reinterpret_cast<XICProc>(ime_preedit_caret);  // NOLINT(*-pro-type-reinterpret-cast)
        if (XVaNestedList nested =
                XVaCreateNestedList(0, XNPreeditDrawCallback, &draw, XNPreeditCaretCallback, &caret, nullptr);
            nested != nullptr) {
            XSetICValues(ic, XNPreeditAttributes, nested, nullptr);
            XFree(nested);
        }
    }
    // 焦点补偿：XIC 建于映射之后时 WM 的 FocusIn 已错过（XIM 惯例，Qt/GTK 同款兜底）。
    if (active) {
        XSetICFocus(ic);
        ime_focused = true;
    }
}

/// @brief 组合态变化的统一出口：UTF-8 字节下标 → 码点契约（ime_composition 纯函数）上抛，
/// 并同步候选窗锚点。caret < 0（XIM caret=-1 隐藏/未知）按「串尾」折算；无选区传 -1/-1。
auto X11Surface::Impl::ime_emit_preedit(const std::string &utf8, int caret, int sel_begin_byte, int sel_end_byte)
    -> void {
    const bool was_composing = !ime_preedit.empty();
    ime_preedit = utf8;
    if (handler) {
        TextCompositionEvent e = ime::make_preedit_state_utf8(utf8, caret, sel_begin_byte, sel_end_byte);
        handler(e);
    }
    // 锚点只在组合期有意义（空串=取消，无须再摆候选窗）；provider 未接线的裸窗口路径自动跳过。
    if (!utf8.empty() || was_composing) {
        ime_update_spot();
    }
}

/// @brief 插入点盒（窗口逻辑 dp，y 向下）→ 客户窗口物理 px 写 XNSpotLocation；盒未变则去重。
auto X11Surface::Impl::ime_update_spot() -> void {
    if (ic == nullptr || !composition_caret_provider) {
        return;
    }
    const Rect box = composition_caret_provider();
    XPoint pt{};
    pt.x = static_cast<short>(std::lround(box.origin.x * scale));
    pt.y = static_cast<short>(std::lround((box.origin.y + box.size.height) * scale));  // 候选窗落在组合串下方
    if (ime_spot_valid && pt.x == ime_last_spot.x && pt.y == ime_last_spot.y) {
        return;
    }
    ime_last_spot = pt;
    ime_spot_valid = true;
    if (XVaNestedList nested = XVaCreateNestedList(0, XNSpotLocation, &pt, nullptr); nested != nullptr) {
        XSetICValues(ic, XNPreeditAttributes, nested, nullptr);
        XFree(nested);
        ++ime_spot_updates;
    }
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
                        reinterpret_cast<unsigned char *>(&above), 1);  // NOLINT(*-pro-type-reinterpret-cast)
    }
    if (style.frameless) {
        struct MotifHints {
            unsigned long flags;
            unsigned long functions;
            unsigned long decorations;
            long input_mode;
            unsigned long status;
        };
        MotifHints hints{.flags = 2UL /*MWM_HINTS_DECORATIONS*/,
                         .functions = 0UL,
                         .decorations = 0UL /*无装饰*/,
                         .input_mode = 0L,
                         .status = 0UL};
        const Atom motif = XInternAtom(d.dpy, "_MOTIF_WM_HINTS", 0);
        XChangeProperty(d.dpy, d.win, motif, motif, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&hints),
                        5);  // NOLINT(*-pro-type-reinterpret-cast)
    }
    // 尺寸限制（XSizeHints）：不可调大小 → min=max=创建尺寸。
    if (XSizeHints *sh = XAllocSizeHints()) {
        if (!style.resizable) {
            sh->flags = PMinSize | PMaxSize;  // NOLINT(*-signed-bitwise)
            sh->min_width = sh->max_width = pw;
            sh->min_height = sh->max_height = ph;
        } else {
            if (style.min_size.width > 0.0F || style.min_size.height > 0.0F) {
                sh->flags |= PMinSize;  // NOLINT(*-signed-bitwise)
                sh->min_width = static_cast<int>(std::lround(style.min_size.width * d.scale));
                sh->min_height = static_cast<int>(std::lround(style.min_size.height * d.scale));
            }
            if (style.max_size.width > 0.0F || style.max_size.height > 0.0F) {
                sh->flags |= PMaxSize;  // NOLINT(*-signed-bitwise)
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
    // 输入法桥（XIM）：协商 PreeditCallbacks 风格并注册回调；失败逐级降级（见 ime_setup）。
    // XMapWindow 之后才建 IC（XNClientWindow 需已映射窗口收 compose 事件——XIM 惯例）。
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
    d.ime_setup();
    d.size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};  // NOLINT(*-narrowing-conversions)
}

X11Surface::~X11Surface() {
    Impl &d = *impl_;
    // 先断开全部上层回调再销毁（对齐 Win32Host 析构次序教训：销毁期间不得回调已亡上层）。
    d.handler = nullptr;
    window_state_handler_ = nullptr;
    window_mode_handler_ = nullptr;
    present_request_ = nullptr;
    d.composition_caret_provider = nullptr;  // 捕获宿主 this 的回调：销毁前解绑（同 Win32 桥纪律）
    if (d.ximage != nullptr) {
        d.ximage->data = nullptr;  // 缓冲由 vector 持有，不得让 XDestroyImage free
        XDestroyImage(d.ximage);
    }
    if (d.dpy != nullptr) {
        if (d.ic != nullptr) {
            if (d.ime_focused) {
                XUnsetICFocus(d.ic);  // 先失焦再销毁：IM 侧组合状态随之释放，不留悬挂回调
            }
            XDestroyIC(d.ic);
        }
        if (d.im != nullptr) {
            XCloseIM(d.im);
        }
        if (d.gc != nullptr) {
            XFreeGC(d.dpy, d.gc);
        }
        // 光标句柄：XDefineCursor 后 X 服务端自持引用，此处只释放客户端引用（Xlib 契约）。
        for (Cursor c : d.cursors) {
            if (c != 0) {
                XFreeCursor(d.dpy, c);
            }
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

// ---- 光标形状：CursorShape → X 核心光标字体字形（<X11/cursorfont.h>）----

namespace {

/// @brief `CursorShape` → `XC_*` 字形码（`XCreateFontCursor` 的入参）。
/// X11 无「禁止」专用光标，`XC_X_cursor`（大叉号）为各工具包通行近似。
constexpr auto x11_cursor_glyph(CursorShape shape) -> unsigned int {
    switch (shape) {
        case CursorShape::Arrow:
            return XC_left_ptr;
        case CursorShape::IBeam:
            return XC_xterm;
        case CursorShape::PointingHand:
            return XC_hand2;
        case CursorShape::ResizeNS:
            return XC_sb_v_double_arrow;
        case CursorShape::ResizeEW:
            return XC_sb_h_double_arrow;
        case CursorShape::ResizeNWSE:
            return XC_top_left_corner;
        case CursorShape::ResizeNESW:
            return XC_top_right_corner;
        case CursorShape::Move:
            return XC_fleur;
        case CursorShape::Crosshair:
            return XC_crosshair;
        case CursorShape::NotAllowed:
            return XC_X_cursor;
        case CursorShape::Wait:
            return XC_watch;
    }
    return XC_left_ptr;
}

}  // namespace

auto X11Surface::set_cursor(CursorShape shape) -> void {
    Impl &d = *impl_;
    if (d.dpy == nullptr || d.win == 0) {
        return;  // 连接/窗口未建成：安全 no-op。
    }
    const auto idx = static_cast<std::size_t>(shape);
    if (idx >= d.cursors.size()) {
        return;
    }
    if (d.cursors.at(idx) == 0) {
        d.cursors.at(idx) = XCreateFontCursor(d.dpy, x11_cursor_glyph(shape));
    }
    if (d.cursors.at(idx) != 0) {
        XDefineCursor(d.dpy, d.win, d.cursors.at(idx));
        XFlush(d.dpy);  // 立即生效：光标不由后续事件驱动刷新（避免等到下次 poll）。
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
    // NOLINTBEGIN(*-narrowing-conversions)
    d.painter.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                             .size = Size{.width = static_cast<float>(d.painter.width()),
                                          .height = static_cast<float>(d.painter.height())}},
                        Color{245, 245, 247, 255});
    // NOLINTEND(*-narrowing-conversions)
    return Result<bool>{true};
}

auto X11Surface::painter() -> Painter & { return impl_->painter; }

// 头文件无条件声明 data() override（虚表槽位恒存在），故定义也必须无条件编译；
// Release 下回落 nullptr（save_snapshot 返回 disabled），与 Win32Surface 同构。
auto X11Surface::data() const -> const std::uint8_t * {
#ifdef AURORA_ENABLE_DEBUG
    if (impl_ && impl_->painter.data() != nullptr) {
        return impl_->painter.data();
    }
#endif
    return nullptr;
}

auto X11Surface::capture_window(const std::string &path) -> Result<bool> {
#ifdef AURORA_ENABLE_DEBUG
    Impl &d = *impl_;
    if (d.dpy == nullptr || d.win == 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: X11 surface not available")};
    }
    XWindowAttributes wa{};
    if (XGetWindowAttributes(d.dpy, d.win, &wa) == 0) {
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
    const unsigned long rm = ximg->red_mask;
    const unsigned long gm = ximg->green_mask;
    const unsigned long bm = ximg->blue_mask;
    auto shift = [](unsigned long mask, int fb) -> int {
        if (mask == 0UL) {
            return fb;
        }
        unsigned int s = 0;
        while (((mask >> s) & 1UL) == 0UL) {
            ++s;
        }
        return static_cast<int>(s);
    };
    const int rs = shift(rm, 16);
    const int gs = shift(gm, 8);
    const int bs = shift(bm, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned long pix = XGetPixel(ximg, x, y);
            const auto r = static_cast<std::uint8_t>((pix & rm) >> rs);
            const auto g = static_cast<std::uint8_t>((pix & gm) >> gs);
            const auto b = static_cast<std::uint8_t>((pix & bm) >> bs);
            const std::size_t o =
                ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4;
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
            // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
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
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x0);
                        swizzle_rows(src + off, d.xbuf.data() + off, static_cast<std::size_t>(x1 - x0), d.rshift,
                                     d.gshift, d.bshift);  // NOLINT(*-pro-bounds-pointer-arithmetic)
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

auto X11Surface::set_composition_caret_provider(std::function<Rect()> provider) -> void {
    impl_->composition_caret_provider = std::move(provider);
}

auto X11Surface::ime_state() const -> ImeState {
    const Impl &d = *impl_;
    ImeState s;
    s.im_open = (d.im != nullptr);
    s.ic_created = (d.ic != nullptr);
    s.preedit_callbacks = d.ime_cb_style;
    s.focused = d.ime_focused;
    s.preedit = d.ime_preedit;
    s.draw_callbacks = d.ime_draw_cbs;
    s.spot_updates = d.ime_spot_updates;
    return s;
}

auto X11Surface::set_title(const std::string &title) -> void {
    Impl &d = *impl_;
    if (d.dpy != nullptr && d.win != 0) {
        d.apply_title(title);
        XFlush(d.dpy);
    }
    if (d.atspi != nullptr) {
        d.atspi->set_window_title(title);  // FRAME 节点 Name（AT 客户端读回窗口标题）
    }
}

auto X11Surface::accessibility_provider() const -> a11y::Provider * { return impl_->atspi.get(); }

auto X11Surface::set_accessibility_root(Widget *root) -> void {
    Impl &d = *impl_;
    if (!d.atspi_attempted) {
        // 首帧根注入时构造桥（一次性尝试：失败 = 永久降级，无总线/无 libdbus 是常态）。
        d.atspi_attempted = true;
        detail::AtspiEnv env;
        env.app_name = "Aurora";
        env.window_title = d.title;
        env.toolkit_version = AURORA_VERSION_STRING;
        env.window_origin_x = d.origin_x;
        env.window_origin_y = d.origin_y;
        // DIP（窗口本地逻辑）→ 窗口本地物理 px：原点向下取整、终点向上取整（protocol 层同款口径）。
        env.to_window_px = [&d](const Rect &r) -> detail::AtspiRectI {
            const auto l = static_cast<std::int32_t>(std::floor(r.origin.x * d.scale));
            const auto t = static_cast<std::int32_t>(std::floor(r.origin.y * d.scale));
            const auto rr = static_cast<std::int32_t>(std::ceil((r.origin.x + r.size.width) * d.scale));
            const auto bb = static_cast<std::int32_t>(std::ceil((r.origin.y + r.size.height) * d.scale));
            return detail::AtspiRectI{.x = l, .y = t, .width = rr - l, .height = bb - t};
        };
        env.to_screen_px = [&d](const Rect &r) -> detail::AtspiRectI {
            const auto box = [&d](const Rect &q) -> detail::AtspiRectI {
                const auto l = static_cast<std::int32_t>(std::floor(q.origin.x * d.scale));
                const auto t = static_cast<std::int32_t>(std::floor(q.origin.y * d.scale));
                const auto rr = static_cast<std::int32_t>(std::ceil((q.origin.x + q.size.width) * d.scale));
                const auto bb = static_cast<std::int32_t>(std::ceil((q.origin.y + q.size.height) * d.scale));
                return detail::AtspiRectI{.x = l, .y = t, .width = rr - l, .height = bb - t};
            }(r);
            return detail::AtspiRectI{
                .x = box.x + d.origin_x, .y = box.y + d.origin_y, .width = box.width, .height = box.height};
        };
        env.perform = [](Widget *w, const AccessibilityActionRequest &req) -> bool {
            return w != nullptr && w->perform_accessibility_action(req);
        };
        d.atspi = detail::AtspiBridge::create(std::move(env));
    }
    if (d.atspi != nullptr) {
        d.atspi->set_root(root);
    }
}

auto X11Surface::native_handle() const -> void * {
    return reinterpret_cast<void *>(impl_->win);  // NOLINT(*-pro-type-reinterpret-cast, performance-no-int-to-ptr)
}

auto X11Surface::native_display() const -> void * {
    return impl_->dpy;  // Display* → void* 隐式转换；未连接时为 nullptr
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
        e.position = Point{.x = px / d.scale, .y = py / d.scale};
        d.handler(e);
    };
    XEvent ev;
    while (XPending(d.dpy) > 0) {
        XNextEvent(d.dpy, &ev);
        if (XFilterEvent(&ev, AURORA_X_NONE) != 0) {
            continue;  // 输入法预编辑消费（如拼音候选期间的按键）
        }
        // NOLINTBEGIN(*-pro-type-union-access)
        switch (ev.type) {
            case ButtonPress:
            case ButtonRelease: {
                const bool press = (ev.type == ButtonPress);
                const unsigned int btn = ev.xbutton.button;
                if (btn >= 4U && btn <= 7U) {
                    // X 惯例：滚轮为按键 4/5（垂直）与 6/7（水平），仅 Press 有意义。
                    if (press && d.handler) {
                        ScrollEvent se;
                        // NOLINTBEGIN(*-narrowing-conversions)
                        se.position = Point{.x = static_cast<float>(ev.xbutton.x) / d.scale,
                                            .y = static_cast<float>(ev.xbutton.y) / d.scale};
                        // NOLINTEND(*-narrowing-conversions)
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
                        heap_buf.resize(static_cast<std::size_t>(len) + 1U);
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
                    (len != 1 || (static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7F))) {
                    if (d.ime_cb_style && !d.ime_preedit.empty()) {
                        // PreeditCallbacks 风格下的 IM 提交串：与 Win32/Wayland 桥同走组合
                        // committed 通道（widget 侧同一条落字路径，preedit 随之清空）。
                        TextCompositionEvent ce;
                        ce.committed.assign(text, static_cast<std::size_t>(len));
                        d.ime_preedit.clear();
                        d.handler(ce);
                    } else {
                        TextInputEvent te;
                        te.text.assign(text, static_cast<std::size_t>(len));
                        d.handler(te);
                    }
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
                // 客户区屏幕原点（物理 px）：XTranslateCoordinates 对根窗口折算，规避
                // reparenting WM 下 xconfigure.x/y 相对父窗口的歧义；AT-SPI 几何回填用。
                {
                    ::Window child = 0;
                    int rx = 0;
                    int ry = 0;
                    if (XTranslateCoordinates(d.dpy, d.win, XDefaultRootWindow(d.dpy), 0, 0, &rx, &ry, &child) !=
                        False) {
                        d.origin_x = rx;
                        d.origin_y = ry;
                        if (d.atspi != nullptr) {
                            d.atspi->set_window_origin(rx, ry);
                        }
                    }
                }
                if (pw > 0 && ph > 0) {
                    // NOLINTBEGIN(*-narrowing-conversions)
                    const Size want{.width = static_cast<float>(pw) / d.scale,
                                    .height = static_cast<float>(ph) / d.scale};
                    // NOLINTEND(*-narrowing-conversions)
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
                // IM 焦点绑定（XSetICFocus 缺口修复）：不宣告则 IM 永不把组合事件路由进本 IC，
                // preedit 回调与 commit 都收不到。IC 晚建（IM 服务器迟就绪）时在此补建。
                if (d.ic == nullptr) {
                    d.ime_setup();
                }
                if (d.ic != nullptr) {
                    XSetICFocus(d.ic);
                    d.ime_focused = true;
                }
                break;
            case FocusOut:
                d.active = false;
                update_state();
                if (d.ic != nullptr) {
                    XUnsetICFocus(d.ic);
                    d.ime_focused = false;
                }
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
        // NOLINTEND(*-pro-type-union-access)
    }
}

auto X11Surface::wait_events(double timeout_ms) -> void {
    Impl &d = *impl_;
    if (d.atspi != nullptr) {
        d.atspi->pump();  // 先非阻塞消化 AT-SPI 在途消息（X 事件密集期桥不被饿死）
    }
    if (d.dpy == nullptr || timeout_ms == 0.0 || d.close_requested) {
        return;
    }
    if (XPending(d.dpy) > 0) {
        return;  // 队列已有未处理事件：立即回到帧循环消费
    }
    // 无限等待按 1000ms 分段兜底（对齐 Win32/默认实现）：唤醒渠道丢失也最迟 1s 自然醒。
    const double capped = (timeout_ms < 0.0 || timeout_ms > 1000.0) ? 1000.0 : timeout_ms;
    std::vector<pollfd> fds;
    fds.reserve(2 + (d.atspi != nullptr ? 2 : 0));
    fds.push_back(pollfd{ConnectionNumber(d.dpy), POLLIN, 0});
    int wake_idx = -1;
    if (d.wake_fd[0] >= 0) {
        wake_idx = static_cast<int>(fds.size());
        fds.push_back(pollfd{d.wake_fd[0], POLLIN, 0});
    }
    std::vector<detail::AtspiBridge::WatchFd> watches;
    if (d.atspi != nullptr) {
        watches = d.atspi->poll_watches();
        for (const auto &w : watches) {
            fds.push_back(pollfd{w.fd, w.events, 0});
        }
    }
    const int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(std::ceil(capped)));
    if (rc > 0 && wake_idx >= 0 &&
        (fds[static_cast<std::size_t>(wake_idx)].revents & POLLIN) != 0) {  // NOLINT(*-signed-bitwise)
        char drain[64];
        while (read(d.wake_fd[0], drain, sizeof(drain)) > 0) {
            // 排干唤醒字节（非阻塞读到 EAGAIN 为止），避免下次 wait 立即空醒。
        }
    }
    if (d.atspi != nullptr) {
        d.atspi->pump();  // watch fd 就绪 ⇒ 读入并派发 AT-SPI 方法调用（应答经同一 fd 写出）
    }
}

auto X11Surface::request_wake() -> void {
    const Impl &d = *impl_;
    if (d.wake_fd[1] >= 0) {
        constexpr char b = 1;
        [[maybe_unused]] const ssize_t rc = ::write(d.wake_fd[1], &b, 1);  // 满管道丢弃亦可：已有待读字节必醒
    }
}

}  // namespace aurora

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX
