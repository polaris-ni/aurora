// X11 CLIPBOARD 剪贴板桥实现（契约与门控见同名头文件）。
//
// 协议要点（ICCCM 的 CLIPBOARD selection，规范同源，不在此重述规范文本）：
//   * 写入是**声明式**：`set_payload` / `set_offer` 只经 `XSetSelectionOwner` 把 CLIPBOARD 选择
//     权交给本窗口；真正的数据在别的进程经 `XConvertSelection` 请求时才由本桥在 `SelectionRequest`
//     里经 `XChangeProperty` 送达（惰性，与 Win32 `WM_RENDERFORMAT` 同款哲学）。
//   * 读入是**请求式**：`get_payload` 向当前 owner 发 `XConvertSelection`，随后在原地泵事件等
//     `SelectionNotify`（不引入异步通道——剪贴板读取时机由应用自己选，不在帧循环关键路径上，
//     与 CHG-023 为剪贴板定的「不引入异步读取」口径一致）。
//   * 大块数据走 **INCR**：超过单属性上限的载荷先回一枚 `INCR` 类型的长度提示，再由请求方删属性、
//     本桥在 `PropertyNotify` 里续发分片，末片零长收尾。读入侧对称处理。
//   * 所有权丢失：`SelectionClear` ⇒ 清本地数据并触发 `on_ownership_lost`。
//   * 内容变化：`XFIXES` 的 selection owner notify（本桥在 `install()` 探测能力，无则
//     `on_change` 注册即失败、不谎报）。
//
// 与 Win32 桥的同异：Win32 把「文本 / 图像」也走 `OpenClipboard` 直接写；本库 Linux 的
// `set_text` / `get_text` / `set_image` / `get_image` / `clear` 仍走 xsel/xclip（提案为 Linux 定的
// 口径，见 `clipboard.h`），故本桥只承载**富格式**（文件清单 / HTML / 自定义 MIME / 惰性要约）与
// `text/plain` 经 `set_payload` 的入口——两路都落到同一枚 CLIPBOARD 选择，后写者接管，与 Windows
// 下「文本与富格式共用系统剪贴板」同构。

#include "x11_clipboard.h"

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

// aurora 头先于 Xlib 头（与 `x11_drag.cpp` 同款纪律：平台协议头关在单个 TU 内，并拿回「包含顺序
// 即语义」的主动权，避免排版器把 `<X11/X.h>` 的 `#define None 0` 漂移到其它 TU）。
// clang-format off
#include "aurora/app/clipboard.h"
#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
// clang-format on

#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aurora::detail {
namespace {

/// @brief 单属性直送上限（字节）：超过即走 INCR 分片。
///
/// 取 X 协议属性上限的经验安全值（远小于理论 2^24-1，规避对端小缓冲），与拖放 / 剪贴板互操作的
/// 主流实现一致。
constexpr std::size_t AURORA_X11_MAX_DIRECT = 256U * 1024U;

/// @brief 读入同步等待上界：对端挂死时不把 UI 线程永久钉住（与 Wayland 桥 5s 同口径）。
constexpr std::chrono::milliseconds AURORA_X11_READ_TIMEOUT{5000};

/// @brief 进程级存活桥清单（`void *` 形态：命名空间作用域不提私有嵌套类型名）。
auto live_backends() -> std::vector<void *> & {
    static std::vector<void *> live;
    return live;
}

/// @brief 读入等待谓词的键（selection + 本桥私有属性 + 本窗口）。
///
/// 独立于 `Impl` 定义：谓词是 `XCheckIfEvent` 的自由函数，不能依赖 `Impl` 的私有嵌套类型名。
struct ReadKey {
    Window win = 0;
    Atom selection = 0;
    Atom property = 0;
};

/// @brief 读入等待谓词：只匹配「本窗口的 CLIPBOARD SelectionNotify」或「本私有属性的
/// PropertyNotify」，不吞掉其它事件（`XCheckIfEvent` 语义，见 `wait_for_read`）。
int read_pred(Display * /*dpy*/, XEvent *ev, XPointer arg) {
    const auto *key = reinterpret_cast<const ReadKey *>(arg);  // NOLINT(*-pro-type-reinterpret-cast)
    if (ev->type == SelectionNotify && ev->xselection.selection == key->selection &&
        ev->xselection.requestor == key->win) {
        return True;
    }
    if (ev->type == PropertyNotify && ev->xproperty.window == key->win && ev->xproperty.atom == key->property) {
        return True;
    }
    return False;
}

}  // namespace

// ---- 协议状态机 ----
struct X11ClipboardBridge::Impl {
    Display *dpy = nullptr;
    Window win = 0;
    Hooks hooks{};
    bool installed = false;
    bool owns = false;  ///< 本进程当前是否持有 CLIPBOARD 选择权

    // 缓存的 atom（install 时 intern）
    Atom a_clipboard = 0;
    Atom a_targets = 0;
    Atom a_timestamp = 0;
    Atom a_multiple = 0;
    Atom a_incre = 0;
    Atom a_utf8 = 0;
    Atom a_string = 0;
    Atom a_text = 0;
    Atom a_compound = 0;
    Atom a_property = 0;  ///< 本桥读入用的私有属性名

    // XFIXES 能力
    bool xfixes = false;
    int xfixes_event_base = 0;
    int xfixes_error_base = 0;

    // 内容态
    std::unordered_map<std::string, std::vector<std::uint8_t>> data_;  ///< 立即写（mime → 字节）
    std::optional<ClipboardOffer> offer_;  ///< 惰性要约（formats + provide）

    // 在途 INCR 写出发送（请求方窗口 → 分片态）；同一时刻通常仅一个，但键化以容多请求
    struct IncrSend {
        Window requestor = 0;
        Atom property = 0;
        Atom type = 0;  ///< 分片属性的类型（数据目标是目标 atom；TARGETS 是 XA_ATOM）
        std::vector<std::uint8_t> bytes;
        std::size_t offset = 0;
    };
    std::unordered_map<unsigned long, IncrSend> incr_sends;  ///< 键：requestor 窗口 XID

    // ---- 安装 / 卸载 ----
    auto install() -> bool;
    auto uninstall() -> void;

    // ---- atom 工具 ----
    [[nodiscard]] auto intern(const char *name) -> Atom;
    [[nodiscard]] auto atom_for_mime(const std::string &mime) -> Atom;
    [[nodiscard]] auto mime_for_target(Atom target) -> std::optional<std::string>;

    // ---- 所有权 ----
    auto claim() -> void;
    auto release() -> void;

    // ---- 写出（请求方索取时）----
    auto on_selection_request(Window requestor, Atom selection, Atom target, Atom property, Time time) -> void;
    auto serve_target(Window requestor, Atom target, Atom property, Time time) -> void;
    auto send_notify(Window requestor, Atom target, Atom property, Time time) -> void;
    auto send_bytes(Window requestor, Atom property, Atom target, const std::vector<std::uint8_t> &bytes,
                    Atom prop_type = None) -> void;
    auto begin_incr(Window requestor, Atom property, Atom target, Atom prop_type, std::vector<std::uint8_t> bytes)
        -> void;
    auto pump_incr(Window requestor, Atom property) -> void;

    // ---- 读入（本进程向 owner 请求）----
    [[nodiscard]] auto wait_for_read(Atom selection, Atom property, XEvent &out,
                                     std::chrono::steady_clock::time_point deadline) -> bool;
    [[nodiscard]] auto read_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>>;
    [[nodiscard]] auto read_property(Atom property, std::chrono::steady_clock::time_point deadline)
        -> Result<std::vector<std::uint8_t>>;
    [[nodiscard]] auto read_targets() -> Result<std::vector<std::string>>;

    // ---- 写出辅助 ----
    auto refuse(Window requestor, Atom target, Atom property, Time time) -> void;

    // ---- 事件 ----
    auto on_selection_clear(Atom selection, Time time) -> void;
    auto on_selection_notify(Atom selection, Atom target, Atom property) -> void;
    auto on_property_notify(Window window, Atom atom, int state) -> void;
    auto on_fixes_selection_notify(int subtype, Atom selection, Window owner) -> void;
    [[nodiscard]] auto on_x_event(const XEvent &event) -> bool;

    // ---- 后端九入口 ----
    auto backend_set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes) -> Result<bool>;
    auto backend_get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>>;
    auto backend_set_files(const std::vector<std::string> &paths) -> Result<bool>;
    auto backend_get_files() -> Result<std::vector<std::string>>;
    auto backend_set_html(const std::string &html) -> Result<bool>;
    auto backend_get_html() -> Result<std::string>;
    auto backend_set_offer(const ClipboardOffer &offer) -> Result<void>;
    auto backend_available_formats() -> Result<std::vector<std::string>>;
    auto backend_set_change_listener(std::function<void()> callback) -> Result<void>;
};

// ---- 安装 / 卸载 ----

auto X11ClipboardBridge::Impl::install() -> bool {
    if (installed || dpy == nullptr || win == 0) {
        return false;
    }
    // 不在此重设客户窗口的事件掩码：XSelectInput 是**整表替换**，会把宿主的键盘/指针等掩码
    // 一并清掉。宿主（`x11_surface.cpp`）已在建窗时选入 PropertyChangeMask——INCR 读入靠本窗口
    // 私有属性的 PropertyNotify 推进；写出发块的 PropertyNotify 来自请求方窗口，见 begin_incr。
    a_clipboard = intern("CLIPBOARD");
    a_targets = intern("TARGETS");
    a_timestamp = intern("TIMESTAMP");
    a_multiple = intern("MULTIPLE");
    a_incre = intern("INCR");
    a_utf8 = intern("UTF8_STRING");
    a_string = intern("STRING");
    a_text = intern("TEXT");
    a_compound = intern("COMPOUND_TEXT");
    a_property = intern("AURORA_CLIPBOARD_PROPERTY");
    if (a_clipboard == 0 || a_targets == 0 || a_property == 0) {
        return false;
    }
    // XFIXES 能力探测（失败即「无变更监听机制」，不谎报）
    if (XFixesQueryExtension(dpy, &xfixes_event_base, &xfixes_error_base) == True) {
        xfixes = true;
    }
    installed = true;
    register_backend(this);
    return true;
}

auto X11ClipboardBridge::Impl::uninstall() -> void {
    if (!installed) {
        return;  // 未安装成功：既无后端表也无选择权可撤
    }
    installed = false;
    release();
    // 后端表捕获本桥自指针：撤表或改挂存活桥，避免悬垂。
    unregister_backend(this);
}

// ---- atom 工具 ----

auto X11ClipboardBridge::Impl::intern(const char *name) -> Atom { return XInternAtom(dpy, name, False); }

auto X11ClipboardBridge::Impl::atom_for_mime(const std::string &mime) -> Atom {
    if (mime == AURORA_MIME_TEXT_PLAIN) {
        return a_utf8;  // 文本统一以 UTF8_STRING 落地（跨进程互操作基线）
    }
    return intern(mime.c_str());
}

auto X11ClipboardBridge::Impl::mime_for_target(Atom target) -> std::optional<std::string> {
    if (target == a_utf8 || target == a_string || target == a_text || target == a_compound) {
        return std::string{AURORA_MIME_TEXT_PLAIN};
    }
    if (target == a_targets || target == a_timestamp || target == a_multiple || target == a_incre) {
        return std::nullopt;  // 元目标不是数据格式
    }
    // 其余按 atom 名反查（含 text/uri-list / image/png / text/html / 自定 MIME）
    if (target == 0 || target == None) {
        return std::nullopt;
    }
    char *name = XGetAtomName(dpy, target);
    if (name == nullptr) {
        return std::nullopt;
    }
    std::string mime = name;
    XFree(name);
    return mime;
}

// ---- 所有权 ----

auto X11ClipboardBridge::Impl::claim() -> void {
    XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
    owns = (XGetSelectionOwner(dpy, a_clipboard) == win);
}

auto X11ClipboardBridge::Impl::release() -> void {
    if (owns) {
        // 放弃所有权：把选择权交还（请求方变成「无主」，别的进程可接管）。
        XSetSelectionOwner(dpy, a_clipboard, None, CurrentTime);
        owns = false;
    }
    data_.clear();
    offer_.reset();
    incr_sends.clear();
}

// ---- 写出（请求方索取时）----

auto X11ClipboardBridge::Impl::on_selection_request(Window requestor, Atom selection, Atom target, Atom property,
                                                    Time time) -> void {
    if (selection != a_clipboard || !owns) {
        return;  // 不是本桥的 CLIPBOARD 选择，或已不持有 → 交 DefWindowProc 式处理（不响应）
    }
    if (property == 0 || property == None) {
        property = a_property;  // 请求方给的 property 为 0 时回退到规约属性
    }
    serve_target(requestor, target, property, time);
}

auto X11ClipboardBridge::Impl::serve_target(Window requestor, Atom target, Atom property, Time time) -> void {
    // TARGETS：列出本桥能提供的全部目标 atom
    if (target == a_targets) {
        std::vector<Atom> targets;
        targets.push_back(a_targets);
        targets.push_back(a_timestamp);
        // 只广告真正可取到的目标：UTF8_STRING（text/plain）须立即写或惰性要约真的声明了它，
        // 否则严格读取方会拿到「广告了却拒绝」的不一致视图。
        bool has_text = data_.find(std::string{AURORA_MIME_TEXT_PLAIN}) != data_.end();
        if (!has_text && offer_.has_value()) {
            for (const auto &mime : offer_->formats) {
                if (mime == AURORA_MIME_TEXT_PLAIN) {
                    has_text = true;
                    break;
                }
            }
        }
        if (has_text) {
            targets.push_back(a_utf8);
        }
        for (const auto &kv : data_) {
            const Atom a = atom_for_mime(kv.first);
            if (a != 0 && std::find(targets.begin(), targets.end(), a) == targets.end()) {
                targets.push_back(a);
            }
        }
        if (offer_.has_value()) {
            for (const auto &mime : offer_->formats) {
                const Atom a = atom_for_mime(mime);
                if (a != 0 && std::find(targets.begin(), targets.end(), a) == targets.end()) {
                    targets.push_back(a);
                }
            }
        }
        // TARGETS 的属性类型必须是 XA_ATOM / 32 位（ICCCM）：按裸字节流写字面值会被严格读取方拒绝。
        XChangeProperty(dpy, requestor, property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(targets.data()),  // NOLINT
                        static_cast<int>(targets.size()));
        send_notify(requestor, a_targets, property, time);
        return;
    }
    if (target == a_timestamp) {
        // TIMESTAMP 是 32 位整数（X 协议时间），不是 64 位的 Time：按 format 32 投递，
        // 值取请求事件的时间（与 xclip 等实现同口径；CurrentTime 原样回给请求方）。
        const long stamp = static_cast<long>(time);
        XChangeProperty(dpy, requestor, property, XA_INTEGER, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&stamp), 1);  // NOLINT
        send_notify(requestor, a_timestamp, property, time);
        return;
    }
    if (target == a_multiple) {
        // MULTIPLE 是「一批目标/属性对」：逐对递归处理（属性即各目标的结果落点）。
        Atom actual_type = 0;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *prop = nullptr;
        if (XGetWindowProperty(dpy, requestor, property, 0, 65536, False, AnyPropertyType, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) == Success &&
            prop != nullptr && (nitems % 2) == 0) {
            const auto *pairs = reinterpret_cast<const Atom *>(prop);  // NOLINT
            for (unsigned long i = 0; i + 1 < nitems; i += 2) {
                serve_target(requestor, pairs[i], pairs[i + 1], time);
            }
            XFree(prop);
        }
        // MULTIPLE 本身以 SelectionNotify 收尾
        send_notify(requestor, target, property, time);
        return;
    }
    // 数据目标：解析 mime，取字节
    const auto mime = mime_for_target(target);
    if (!mime.has_value()) {
        refuse(requestor, target, property, time);  // 不认识的目标 → 拒绝
        return;
    }
    std::optional<std::vector<std::uint8_t>> bytes;
    if (const auto it = data_.find(*mime); it != data_.end()) {
        bytes = it->second;
    } else if (offer_.has_value() && offer_->provide != nullptr) {
        bool declared = false;
        for (const auto &m : offer_->formats) {
            if (m == *mime) {
                declared = true;
                break;
            }
        }
        if (declared) {
            const auto produced = offer_->provide(*mime);
            if (!produced) {
                // 惰性提供失败：拒绝此次请求（不谎报成功）
                refuse(requestor, target, property, time);
                return;
            }
            bytes = produced.value();
        }
    }
    if (!bytes.has_value()) {
        refuse(requestor, target, property, time);
        return;
    }
    send_bytes(requestor, property, target, *bytes);
}

auto X11ClipboardBridge::Impl::send_notify(Window requestor, Atom target, Atom property, Time time) -> void {
    XSelectionEvent sev{};
    sev.type = SelectionNotify;
    sev.display = dpy;
    sev.requestor = requestor;
    sev.selection = a_clipboard;
    sev.target = target;
    sev.property = property;  // None = 拒绝
    sev.time = time;
    XSendEvent(dpy, requestor, False, 0, reinterpret_cast<XEvent *>(&sev));  // NOLINT
    XFlush(dpy);
}

auto X11ClipboardBridge::Impl::refuse(Window requestor, Atom target, Atom property, Time time) -> void {
    (void)property;  // 拒绝时结果属性恒为 None（ICCCM），请求方指定的名字不再使用
    send_notify(requestor, target, None, time);
}

auto X11ClipboardBridge::Impl::send_bytes(Window requestor, Atom property, Atom target,
                                          const std::vector<std::uint8_t> &bytes, Atom prop_type) -> void {
    if (prop_type == None) {
        prop_type = target;  // 数据目标以目标 atom 作属性类型（UTF8_STRING / text/uri-list 等）
    }
    if (bytes.size() <= AURORA_X11_MAX_DIRECT) {
        XChangeProperty(dpy, requestor, property, prop_type, 8, PropModeReplace, bytes.data(),
                        static_cast<int>(bytes.size()));
        send_notify(requestor, target, property, CurrentTime);
    } else {
        begin_incr(requestor, property, target, prop_type, bytes);
    }
}

auto X11ClipboardBridge::Impl::begin_incr(Window requestor, Atom property, Atom target, Atom prop_type,
                                          std::vector<std::uint8_t> bytes) -> void {
    // 首片：INCR 类型 + 32 位总长提示（按规范，长度可填 0 表示未知）。
    const std::uint32_t total = static_cast<std::uint32_t>(bytes.size());
    XChangeProperty(dpy, requestor, property, a_incre, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&total), 1);  // NOLINT
    IncrSend send;
    send.requestor = requestor;
    send.property = property;
    send.type = prop_type;
    send.bytes = std::move(bytes);
    send.offset = 0;
    incr_sends[requestor] = std::move(send);
    // 续片由请求方删属性触发：必须对该窗口选入 PropertyChangeMask（事件选择按客户端独立计，
    // 只影响本连接，不动请求方与其他客户端的选择）。
    XSelectInput(dpy, requestor, PropertyChangeMask);
    send_notify(requestor, target, property, CurrentTime);
    // 请求方删属性后即触发下一分片（见 on_property_notify）
}

auto X11ClipboardBridge::Impl::pump_incr(Window requestor, Atom property) -> void {
    auto it = incr_sends.find(requestor);
    if (it == incr_sends.end() || it->second.property != property) {
        return;
    }
    auto &send = it->second;
    const auto *base = send.bytes.data() + send.offset;
    const auto left = send.bytes.size() - send.offset;
    const auto chunk = left > AURORA_X11_MAX_DIRECT ? AURORA_X11_MAX_DIRECT : left;
    if (chunk == 0) {
        // 末片零长：收尾并清态
        XChangeProperty(dpy, requestor, property, send.type, 8, PropModeReplace, nullptr, 0);
        incr_sends.erase(it);
        XFlush(dpy);
        return;
    }
    XChangeProperty(dpy, requestor, property, send.type, 8, PropModeReplace, base, static_cast<int>(chunk));
    send.offset += chunk;
    XFlush(dpy);
}

// ---- 读入（本进程向 owner 请求）----

auto X11ClipboardBridge::Impl::wait_for_read(Atom selection, Atom property, XEvent &out,
                                             std::chrono::steady_clock::time_point deadline) -> bool {
    ReadKey key{win, selection, property};
    for (;;) {
        // XCheckIfEvent 只取匹配项：未命中的事件留在队列里交回主泵。XIfEvent 会顺带丢弃它们，
        // 对「在控件回调里同步读剪贴板」而言等于吞掉当事窗口的输入事件。
        if (XCheckIfEvent(dpy, &out, read_pred, reinterpret_cast<XPointer>(&key)) != 0) {  // NOLINT
            return true;
        }
        const auto remain =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) {
            return false;  // 超时：不悬挂，调用方按「无可读数据」如实收尾
        }
        // XPending 顺带从连接读入已到达的事件；队列非空但无匹配项时短等让出，避免空转。
        pollfd pfd{ConnectionNumber(dpy), POLLIN, 0};
        const int wait_ms = XPending(dpy) > 0 ? 10 : static_cast<int>(remain);
        if (::poll(&pfd, 1, wait_ms) < 0 && errno != EINTR) {
            return false;
        }
    }
}

auto X11ClipboardBridge::Impl::read_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    if (!owns) {
        // 本进程另一窗口持有选择权时直接取它的本地态：本窗口的同步等待里没人泵那个窗口的
        // SelectionRequest，经 X 往返必然超时。
        if (auto *peer = X11ClipboardBridge::owner_bridge(this); peer != nullptr && peer != this) {
            return peer->read_payload(mime);
        }
        const Atom target = atom_for_mime(mime);
        if (target == 0) {
            return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", mime}});
        }
        const Window owner = XGetSelectionOwner(dpy, a_clipboard);
        if (owner == 0 || owner == None) {
            return std::vector<std::uint8_t>{};  // 当前无 owner = 空内容
        }
        const auto deadline = std::chrono::steady_clock::now() + AURORA_X11_READ_TIMEOUT;
        XConvertSelection(dpy, a_clipboard, target, a_property, win, CurrentTime);
        XFlush(dpy);
        XEvent ev{};
        for (;;) {
            if (!wait_for_read(a_clipboard, a_property, ev, deadline)) {
                return make_error(ErrorCode::ClipboardAccessFailed,
                                  ErrorParams{{"detail", "timeout waiting for SelectionNotify"}});
            }
            // 队列里可能还有上一轮 INCR 的删除回声（PropertyNotify）或别的目标的迟到 notify：
            // 只认本次目标，其余丢弃继续等（丢弃安全：调用方此刻未挂任何在途 INCR 态）。
            if (ev.type == SelectionNotify && ev.xselection.target == target) {
                break;
            }
        }
        if (ev.xselection.property == 0 || ev.xselection.property == None) {
            return std::vector<std::uint8_t>{};  // 拒绝 = 空字节
        }
        return read_property(ev.xselection.property, deadline);
    }
    // 读自己刚写的：直接取本地态（避免绕一圈 X 往返）
    if (const auto it = data_.find(mime); it != data_.end()) {
        return it->second;
    }
    if (offer_.has_value() && offer_->provide != nullptr) {
        bool declared = false;
        for (const auto &m : offer_->formats) {
            if (m == mime) {
                declared = true;
                break;
            }
        }
        if (declared) {
            const auto produced = offer_->provide(mime);
            if (!produced) {
                return make_error(ErrorCode::ClipboardProviderFailed, ErrorParams{{"mime", mime}});
            }
            return produced.value();
        }
    }
    return std::vector<std::uint8_t>{};  // 自己持有但无此格式 = 空字节（空内容不是失败）
}

auto X11ClipboardBridge::Impl::read_property(Atom property, std::chrono::steady_clock::time_point deadline)
    -> Result<std::vector<std::uint8_t>> {
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, win, property, 0, 0, False, AnyPropertyType, &actual_type, &actual_format, &nitems,
                           &bytes_after, &prop) != Success) {
        return std::vector<std::uint8_t>{};
    }
    if (prop != nullptr) {
        XFree(prop);
        prop = nullptr;
    }
    std::vector<std::uint8_t> out;
    if (actual_type == a_incre) {
        // INCR：读长度提示后删属性，循环收分片
        unsigned long hint = 0;
        unsigned long hint_items = 0;
        unsigned long hint_after = 0;
        unsigned char *hint_prop = nullptr;
        if (XGetWindowProperty(dpy, win, property, 0, 1, False, a_incre, &actual_type, &actual_format, &hint_items,
                               &hint_after, &hint_prop) == Success &&
            hint_prop != nullptr) {
            if (hint_items >= 1) {
                hint = (reinterpret_cast<const std::uint32_t *>(hint_prop))[0];  // NOLINT
            }
            XFree(hint_prop);
        }
        (void)hint;  // 长度提示仅用于预分配；逐片读取不依赖它
        // 删掉 INCR 标记属性 = 向 owner 要第一片；此后「消费一片 → 再删」循环推进。
        // 不在每次迭代开头无条件删：本端 XDeleteProperty 的 PropertyDelete 回声会在队列里，
        // 若收到回声就又删一次，会把 owner 刚写好的下一片一起删掉（丢数据）。
        XDeleteProperty(dpy, win, property);
        XFlush(dpy);
        for (;;) {
            XEvent ev2{};
            if (!wait_for_read(a_clipboard, property, ev2, deadline)) {
                return make_error(ErrorCode::ClipboardAccessFailed,
                                  ErrorParams{{"detail", "timeout receiving INCR chunk"}});
            }
            if (ev2.type != PropertyNotify || ev2.xproperty.atom != property) {
                continue;  // 本属性之外的噪声事件：继续等
            }
            if (ev2.xproperty.state != PropertyNewValue) {
                continue;  // 本端删除动作的回声：只等 owner 写入
            }
            const std::size_t before = out.size();
            bool aborted = false;
            bool ended = false;
            for (long offset_items = 0;;) {
                Atom ct = 0;
                int cf = 0;
                unsigned long cn = 0;
                unsigned long ca = 0;
                unsigned char *cp = nullptr;
                // 分片类型由 owner 决定（规范建议 = 目标 atom，但互操作实现常用 STRING）：
                // 按 AnyPropertyType 读，避免类型不一致被 Xlib 折算成零长而静默截断。
                if (XGetWindowProperty(dpy, win, property, offset_items, 65536, False, AnyPropertyType, &ct, &cf, &cn,
                                       &ca, &cp) != Success) {
                    aborted = true;
                    break;
                }
                if (cp == nullptr) {
                    ended = true;  // 属性已消失：对端已收尾
                    break;
                }
                const auto len = cn * static_cast<unsigned long>(cf > 0 ? cf / 8 : 0);
                out.insert(out.end(), cp, cp + len);
                XFree(cp);
                offset_items += static_cast<long>(cn);
                if (ca == 0) {
                    break;
                }
            }
            if (aborted) {
                break;  // 读取失败：把已收到的部分如实返回（与直送路径同口径）
            }
            XDeleteProperty(dpy, win, property);  // 已消费本片：请 owner 续发下一片
            XFlush(dpy);
            if (ended || out.size() == before) {
                break;  // 零长分片 = 收尾
            }
        }
        return out;
    }
    // 直送：一次读全（bytes_after>0 时循环读完，规避大属性截断）
    long offset = 0;
    for (;;) {
        if (XGetWindowProperty(dpy, win, property, offset, 65536, False, AnyPropertyType, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) != Success) {
            break;
        }
        if (prop == nullptr) {
            break;
        }
        const auto len = nitems * static_cast<unsigned long>(actual_format > 0 ? actual_format / 8 : 0);
        out.insert(out.end(), prop, prop + len);
        XFree(prop);
        prop = nullptr;
        if (bytes_after == 0) {
            break;
        }
        offset += static_cast<long>(nitems);
    }
    return out;
}

auto X11ClipboardBridge::Impl::read_targets() -> Result<std::vector<std::string>> {
    const Window owner = XGetSelectionOwner(dpy, a_clipboard);
    if (owner == 0 || owner == None) {
        return std::vector<std::string>{};
    }
    const auto deadline = std::chrono::steady_clock::now() + AURORA_X11_READ_TIMEOUT;
    XConvertSelection(dpy, a_clipboard, a_targets, a_property, win, CurrentTime);
    XFlush(dpy);
    XEvent ev{};
    for (;;) {
        if (!wait_for_read(a_clipboard, a_property, ev, deadline)) {
            return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", "timeout waiting for TARGETS"}});
        }
        if (ev.type == SelectionNotify && ev.xselection.target == a_targets) {
            break;
        }
    }
    if (ev.xselection.property == 0 || ev.xselection.property == None) {
        return std::vector<std::string>{};
    }
    const auto raw = read_property(ev.xselection.property, deadline);
    if (!raw) {
        return std::vector<std::string>{};
    }
    std::vector<std::string> formats;
    const auto &raw_bytes = raw.value();
    const std::size_t count = raw_bytes.size() / sizeof(Atom);
    std::vector<Atom> atoms(count);
    if (count > 0) {
        std::memcpy(atoms.data(), raw_bytes.data(), count * sizeof(Atom));  // 字节流 → Atom 需按对齐拷贝
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto mime = mime_for_target(atoms[i]);
        if (mime.has_value()) {
            formats.push_back(*mime);
        }
    }
    return formats;
}

// ---- 事件 ----

auto X11ClipboardBridge::Impl::on_selection_clear(Atom selection, Time /*time*/) -> void {
    if (selection != a_clipboard) {
        return;
    }
    owns = false;
    data_.clear();
    offer_.reset();
    incr_sends.clear();
    clipboard::detail::fire_ownership_lost();
}

auto X11ClipboardBridge::Impl::on_selection_notify(Atom selection, Atom /*target*/, Atom /*property*/) -> void {
    // 读入侧的 SelectionNotify 由 `read_payload` / `read_targets` 内在的 XIfEvent 直接消费，
    // 此处仅作防御性占位（不应在非读入上下文收到针对本窗口的 CLIPBOARD SelectionNotify）。
    (void)selection;
}

auto X11ClipboardBridge::Impl::on_property_notify(Window window, Atom atom, int state) -> void {
    if (state != PropertyDelete) {
        return;  // INCR 只在请求方「删属性 = 已消费上一分片」时推进
    }
    if (incr_sends.empty()) {
        return;
    }
    // window 是请求方窗口；属性是其在 SelectionRequest 里指定的结果属性
    auto it = incr_sends.find(window);
    if (it != incr_sends.end() && it->second.property == atom) {
        pump_incr(window, atom);
    }
}

auto X11ClipboardBridge::Impl::on_fixes_selection_notify(int subtype, Atom selection, Window /*owner*/) -> void {
    if (!xfixes || selection != a_clipboard) {
        return;
    }
    if (subtype == XFixesSetSelectionOwnerNotify || subtype == XFixesSelectionClientCloseNotify ||
        subtype == XFixesSelectionWindowDestroyNotify) {
        if (hooks.on_change != nullptr) {
            hooks.on_change();  // 内容变化（含被夺 / 关闭）：如实触发一次变更回调
        }
    }
}

auto X11ClipboardBridge::Impl::on_x_event(const XEvent &event) -> bool {
    if (!xfixes || event.type != xfixes_event_base + XFixesSelectionNotify) {
        return false;
    }
    // XEvent 联合里没有 XFIXES 的事件成员：按结构体前导布局逐字节取一份（避免 union 类型双关）。
    XFixesSelectionNotifyEvent sev{};
    std::memcpy(&sev, &event, sizeof(sev));
    on_fixes_selection_notify(sev.subtype, sev.selection, sev.owner);
    return true;
}

// ---- 后端九入口 ----

auto X11ClipboardBridge::Impl::backend_set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes)
    -> Result<bool> {
    if (mime.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, ErrorParams{{"detail", "set_payload: mime empty"}});
    }
    if (bytes.empty()) {
        return Result<bool>{true};  // 契约：空字节 no-op，保留既有内容
    }
    data_[mime] = bytes;  // 立即写覆盖同 mime 的惰性声明（后写者接管）
    claim();
    return Result<bool>{true};
}

auto X11ClipboardBridge::Impl::backend_get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    if (mime.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, ErrorParams{{"detail", "get_payload: mime empty"}});
    }
    return read_payload(mime);
}

auto X11ClipboardBridge::Impl::backend_set_files(const std::vector<std::string> &paths) -> Result<bool> {
    const auto encoded = uri_list::encode(paths);
    const std::vector<std::uint8_t> bytes{encoded.begin(), encoded.end()};
    return backend_set_payload(std::string{AURORA_MIME_URI_LIST}, bytes);
}

auto X11ClipboardBridge::Impl::backend_get_files() -> Result<std::vector<std::string>> {
    const auto raw = read_payload(std::string{AURORA_MIME_URI_LIST});
    if (!raw) {
        return raw.error();  // 原码透传：provider-failed / access-failed 不在桥内改写
    }
    const std::string text{raw.value().begin(), raw.value().end()};
    return uri_list::decode(text);
}

auto X11ClipboardBridge::Impl::backend_set_html(const std::string &html) -> Result<bool> {
    const std::vector<std::uint8_t> bytes{html.begin(), html.end()};
    return backend_set_payload(std::string{AURORA_MIME_TEXT_HTML}, bytes);
}

auto X11ClipboardBridge::Impl::backend_get_html() -> Result<std::string> {
    const auto raw = read_payload(std::string{AURORA_MIME_TEXT_HTML});
    if (!raw) {
        return raw.error();  // 原码透传：provider-failed / access-failed 不在桥内改写
    }
    return std::string{raw.value().begin(), raw.value().end()};
}

auto X11ClipboardBridge::Impl::backend_set_offer(const ClipboardOffer &offer) -> Result<void> {
    if (offer.formats.empty() || offer.provide == nullptr) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          ErrorParams{{"detail", "set_offer: formats empty or provide null"}});
    }
    offer_ = offer;  // 惰性要约与立即写并存（后写者接管同 mime）
    claim();
    return Result<void>{};
}

auto X11ClipboardBridge::Impl::backend_available_formats() -> Result<std::vector<std::string>> {
    if (!owns) {
        if (auto *peer = X11ClipboardBridge::owner_bridge(this); peer != nullptr && peer != this) {
            return peer->backend_available_formats();  // peer->owns == true ⇒ 返回本地清单
        }
    }
    if (owns) {
        std::vector<std::string> formats;
        for (const auto &kv : data_) {
            formats.push_back(kv.first);
        }
        if (offer_.has_value()) {
            for (const auto &mime : offer_->formats) {
                if (std::find(formats.begin(), formats.end(), mime) == formats.end()) {
                    formats.push_back(mime);
                }
            }
        }
        return formats;
    }
    return read_targets();
}

auto X11ClipboardBridge::Impl::backend_set_change_listener(std::function<void()> callback) -> Result<void> {
    if (!callback) {
        if (xfixes) {
            XFixesSelectSelectionInput(dpy, win, a_clipboard, 0);  // 注销监听（与是否持有所有权无关）
        }
        hooks.on_change = nullptr;
        return Result<void>{};
    }
    if (!xfixes) {
        return make_error(ErrorCode::ClipboardListenUnsupported,
                          ErrorParams{{"detail", "XFIXES unavailable: no clipboard change listener"}});
    }
    hooks.on_change = std::move(callback);
    XFixesSelectSelectionInput(
        dpy, win, a_clipboard,
        XFixesSetSelectionOwnerNotify | XFixesSelectionClientCloseNotify | XFixesSelectionWindowDestroyNotify);
    return Result<void>{};
}

// ---- 桥的公共面（全部转发给状态机） ----

X11ClipboardBridge::X11ClipboardBridge(void *display, unsigned long window, Hooks hooks)
    : impl_(std::make_unique<Impl>()) {
    impl_->dpy = static_cast<Display *>(display);
    impl_->win = static_cast<Window>(window);
    impl_->hooks = std::move(hooks);
}

X11ClipboardBridge::~X11ClipboardBridge() { uninstall(); }

auto X11ClipboardBridge::install() -> bool { return impl_->install(); }

auto X11ClipboardBridge::uninstall() -> void { impl_->uninstall(); }

auto X11ClipboardBridge::on_selection_request(unsigned long requestor, unsigned long selection, unsigned long target,
                                              unsigned long property, unsigned long time) -> void {
    impl_->on_selection_request(static_cast<Window>(requestor), static_cast<Atom>(selection), static_cast<Atom>(target),
                                static_cast<Atom>(property), static_cast<Time>(time));
}

auto X11ClipboardBridge::on_selection_clear(unsigned long selection, unsigned long time) -> void {
    impl_->on_selection_clear(static_cast<Atom>(selection), static_cast<Time>(time));
}

auto X11ClipboardBridge::on_selection_notify(unsigned long selection, unsigned long target, unsigned long property)
    -> void {
    impl_->on_selection_notify(static_cast<Atom>(selection), static_cast<Atom>(target), static_cast<Atom>(property));
}

auto X11ClipboardBridge::on_property_notify(unsigned long window, unsigned long atom, int state) -> void {
    impl_->on_property_notify(static_cast<Window>(window), static_cast<Atom>(atom), state);
}

auto X11ClipboardBridge::owns() const -> bool { return impl_->owns; }

auto X11ClipboardBridge::on_x_event(const void *xevent) -> bool {
    if (xevent == nullptr) {
        return false;
    }
    return impl_->on_x_event(*static_cast<const XEvent *>(xevent));
}

auto X11ClipboardBridge::owner_bridge(Impl *self) -> Impl * {
    // 选择权的 owner 以服务器为准（本桥自己的 `owns` 只反映本连接已派发到的状态）：
    // XID 与 atom 都是服务器级标识，跨连接比较在同一 server 上成立。
    const Window sel_owner = XGetSelectionOwner(self->dpy, self->a_clipboard);
    if (sel_owner == 0 || sel_owner == None) {
        return nullptr;
    }
    for (void *slot : live_backends()) {
        auto *impl = static_cast<Impl *>(slot);
        if (impl->owns && impl->win == sel_owner) {
            return impl;
        }
    }
    return nullptr;
}

auto X11ClipboardBridge::install_backend_for(Impl *self) -> void {
    clipboard::detail::install_backend(clipboard::detail::PlatformBackend{
        .set_payload = [self](
                           const std::string &mime,
                           const std::vector<std::uint8_t> &bytes) { return self->backend_set_payload(mime, bytes); },
        .get_payload = [self](const std::string &mime) { return self->backend_get_payload(mime); },
        .set_files = [self](const std::vector<std::string> &paths) { return self->backend_set_files(paths); },
        .get_files = [self]() { return self->backend_get_files(); },
        .set_html = [self](const std::string &html) { return self->backend_set_html(html); },
        .get_html = [self]() { return self->backend_get_html(); },
        .set_offer = [self](const ClipboardOffer &offer) { return self->backend_set_offer(offer); },
        .available_formats = [self]() { return self->backend_available_formats(); },
        .set_change_listener =
            [self](std::function<void()> callback) { return self->backend_set_change_listener(std::move(callback)); },
    });
}

auto X11ClipboardBridge::register_backend(Impl *self) -> void {
    auto &live = live_backends();
    auto *slot = static_cast<void *>(self);
    if (std::find(live.begin(), live.end(), slot) == live.end()) {
        live.push_back(slot);
    }
    install_backend_for(self);  // 最近登记者接管（多窗口共享进程级单槽）
}

auto X11ClipboardBridge::unregister_backend(Impl *self) -> void {
    auto &live = live_backends();
    auto *slot = static_cast<void *>(self);
    live.erase(std::remove(live.begin(), live.end(), slot), live.end());
    if (live.empty()) {
        clipboard::detail::remove_backend();
        return;
    }
    install_backend_for(static_cast<Impl *>(live.back()));
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_X11
