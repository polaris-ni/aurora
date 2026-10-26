// X11 Xdnd 拖放桥实现（契约与门控见同名头文件）。
//
// 协议要点（Xdnd 规范 v5；字段布局与既有实现同源，不在此重述规范文本）：
//   * 目标侧：广告 `XdndAware`（XA_ATOM/32/版本）；收 `XdndEnter` / `XdndPosition` / `XdndLeave` /
//     `XdndDrop`；回 `XdndStatus`（接受 + 空矩形 + 动作）与 `XdndFinished`（v5 起携带受理位与动作）。
//   * 源侧：grab 指针 → 取得 `XdndSelection` 所有权 → 发 `XdndEnter` / `XdndPosition` →
//     释放键时发 `XdndDrop` → 应答目标发来的 `SelectionRequest` → 收 `XdndFinished` 定终局。
//   * 数据一律经 selection round-trip（`XConvertSelection` → `SelectionNotify`），超过直发上限时走
//     ICCCM `INCR` 分片（请求方删属性启动、属主替换式发块、零长收尾）。
//
// 为何装 X 错误处理器：`XSendEvent` / `XChangeProperty` / `XSelectInput` / `XGetWindowProperty`
// 全部打向**外来窗口**，对端随时可能销毁；Xlib 默认处理器直接 `exit(1)`，一次竞态即带走整个
// 进程。故安装期装一个「吞掉全部错误」的处理器并**不配对恢复**——「谁最后卸载谁恢复」在多窗口
// 下自身就有竞态，而连接保持可用（Xlib 的错误不影响后续请求），各 Xdnd 实现同款处置。

#include "aurora/window/detail/x11_drag.h"

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

// aurora 头必须先于 Xlib（理由见头文件注记：Xlib 的 `None` / `CursorShape` 宏污染）。
//
// 整段包含用 `clang-format off` 圈住：格式化器按「系统头优先」重排包含时，会把
// `<X11/...>` 提到 aurora 头之前，`None` 宏随即污染 `ModifierKey::None` / `DragAction::None`
// 与折算层的 `Win32DropEffect::None`（症状是 `expected identifier before numeric constant`）。
// 顺序是本文件的语义，不由排版器决定（同 `win32_host.cpp` 的既有处置）。
// clang-format off
#include "aurora/app/mime_types.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/event/detail/system_drag_backend.h"
#include "aurora/event/detail/system_drag_ingress.h"
#include "aurora/event/keycode.h"
#include "aurora/window/detail/system_drag_translate.h"
#include "aurora/window/detail/x11_modifiers.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace {
// X11 的 None 宏与 aurora::ModifierKey::None 冲突：先取值再解除宏定义（既有纪律）。
constexpr unsigned long AURORA_X_NONE = None;
constexpr int AURORA_X_PROP_NEW_VALUE = PropertyNewValue;
constexpr int AURORA_X_PROP_DELETE = PropertyDelete;
}  // namespace
#undef None

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
// 豁免范围：XEvent 的 xclient.data.l / xselection 是 X 协议固有的 tagged union 字段；Xlib 属性
// 读写以 unsigned char* 为字节口，两者都没有类型安全的替代写法。

namespace aurora::detail {
namespace {

/// @brief 本库广告的 Xdnd 协议版本（v5：`XdndFinished` 起携带受理位与执行动作）。
constexpr int AURORA_XDND_VERSION = 5;
/// @brief 直发上限（字节）：超过它改走 `INCR` 分片（与同步 / 异步读取阈值**语义无关**，不复用）。
constexpr std::size_t AURORA_XDND_DIRECT_LIMIT = static_cast<std::size_t>(64U) * 1024U;
/// @brief `INCR` 单片大小（字节）。
constexpr std::size_t AURORA_XDND_CHUNK = static_cast<std::size_t>(16U) * 1024U;
/// @brief 上溯 `XQueryTree` 找 `XdndAware` 的层数上限（畸形 / 超深窗口树不得把 UI 线程钉住）。
constexpr int AURORA_XDND_TREE_DEPTH = 32;
/// @brief 目标缓存条目上限（child → target 的映射；超出即整体丢弃，不无界增长）。
constexpr std::size_t AURORA_XDND_CACHE_LIMIT = 64U;
// 根坐标掩码：`XdndPosition` 的 `l[2]` 里两个坐标各占 16 位**有符号**整数（高位 X、低位 Y）。
constexpr unsigned long AURORA_XDND_COORD_MASK = 0xFFFFUL;

/// @brief X 错误处理器：吞掉全部异步错误（理由见文件头）。
///
/// 形参必须留名（读者据此知道吞掉的是什么），`[[maybe_unused]]` 说明「刻意不读」。
auto xdnd_error_handler([[maybe_unused]] Display *display, [[maybe_unused]] XErrorEvent *event) -> int { return 0; }

/// @brief 进程级发起槽（多窗口下由宿主在事件泵入口刷新）。
auto initiator_slot() -> X11DragBridge *& {
    static X11DragBridge *slot = nullptr;
    return slot;
}

/// @brief 错误处理器是否已装（进程级一次性）。
auto error_handler_installed() -> bool & {
    static bool installed = false;
    return installed;
}

/// @brief 拖出后端是否已注册（进程级一次性；事件层另按「先到者优先」去重）。
auto backend_registered() -> bool & {
    static bool registered = false;
    return registered;
}

}  // namespace

/// @brief 协议状态机：atom 表 + 双向会话态（定义在实现内，故可使用 Xlib 类型）。
struct X11DragBridge::Impl {
    /// @brief Xdnd 协议 atom（install 期一次性 intern；同一 display 多窗口重复 intern 幂等）。
    struct Atoms {
        unsigned long aware = 0;  ///< `XdndAware`
        unsigned long enter = 0;  ///< `XdndEnter`
        unsigned long position = 0;  ///< `XdndPosition`
        unsigned long status = 0;  ///< `XdndStatus`
        unsigned long leave = 0;  ///< `XdndLeave`
        unsigned long drop = 0;  ///< `XdndDrop`
        unsigned long finished = 0;  ///< `XdndFinished`
        unsigned long selection = 0;  ///< `XdndSelection`
        unsigned long type_list = 0;  ///< `XdndTypeList`
        unsigned long incr = 0;  ///< `INCR`
        unsigned long action_copy = 0;  ///< `XdndActionCopy`
        unsigned long action_move = 0;  ///< `XdndActionMove`
        unsigned long action_link = 0;  ///< `XdndActionLink`
    };

    /// @brief 拖入（本窗口是 target）会话态。
    struct DragIn {
        bool active = false;  ///< 会话已建立（`XdndEnter` 起、收口止）
        bool entered = false;  ///< 已向控件派发过 `Enter`（首个 `XdndPosition` 才派发，见下）
        bool dropped = false;  ///< 已收 `XdndDrop`、正在读数据
        unsigned long source = 0;  ///< 源窗口 XID
        int version = 0;  ///< 协商版本（`min(源广告, 本库广告)`）
        std::vector<std::string> mimes;  ///< 折算后的 MIME 清单（顺序即优先级）
        std::size_t index = 0;  ///< 下一个待读取的 MIME 下标
        enum class ReadPhase : std::uint8_t { Idle, Awaiting, Incr } phase = ReadPhase::Idle;  ///< 读取阶段
        unsigned long read_atom = 0;  ///< 当前请求的目标 atom（亦作结果属性名）
        std::string current_mime;  ///< 当前读取中的 MIME
        std::vector<std::uint8_t> buffer;  ///< `INCR` 累积缓冲
        std::unordered_map<std::string, std::vector<std::uint8_t>> entries;  ///< 已读到的 MIME → 字节
        ModifierKey mods = ModifierKey::None;  ///< 最近一次 `XdndPosition` 的修饰态
        DragAction os_action = DragAction::None;  ///< 源建议动作（`XdndPosition` 的 `l[4]`）
        DragAction status_action = DragAction::None;  ///< 最近一次回给源的动作（`Enter` / `Over` 派发后）
        int client_x = 0;  ///< 最近一次落点（客户区物理像素）
        int client_y = 0;
        unsigned long drop_time = 0;  ///< `XdndDrop` 的时间戳（selection 请求沿用）
        std::chrono::steady_clock::time_point started;  ///< 数据读取起点（超时看守）
    };

    /// @brief 拖出（本窗口是 source）会话态。
    struct DragOut {
        bool active = false;  ///< 会话在途
        bool drop_sent = false;  ///< `XdndDrop` 已发（等 `XdndFinished`）
        bool grabbed = false;  ///< 当前持有指针 grab
        bool owns_selection = false;  ///< 当前持有 `XdndSelection` 所有权
        bool target_accepts = false;  ///< 最近一次 `XdndStatus` 的受理位
        bool cursor_applied = false;  ///< 光标是否已按受理态应用过（惰性创建后首次必应用）
        DragAction target_action = DragAction::None;  ///< 最近一次 `XdndStatus` 的动作
        DragAction sent_action = DragAction::None;  ///< 最近一次 `XdndPosition` 发出的动作
        DragOffer offer;  ///< 控件要约（会话期内只读）
        std::vector<std::string> mimes;  ///< 可广告的 MIME（要约格式 ∩ 登记表）
        unsigned long target = 0;  ///< 当前目标窗口 XID（0 = 无目标）
        int target_version = 0;  ///< 目标广告的版本
        bool incr_active = false;  ///< 正在向请求方 `INCR` 分片
        unsigned long incr_requestor = 0;  ///< `INCR` 请求方窗口
        unsigned long incr_property = 0;  ///< `INCR` 结果属性名
        unsigned long incr_target = 0;  ///< `INCR` 目标 atom（作属性类型）
        std::vector<std::uint8_t> incr_data;  ///< `INCR` 待发字节
        std::size_t incr_offset = 0;  ///< `INCR` 已发偏移
        std::chrono::steady_clock::time_point drop_at;  ///< `XdndDrop` 发出时刻（超时看守）
        std::unordered_map<unsigned long, unsigned long> target_cache;  ///< child 窗口 → Xdnd 目标窗口
    };

    Display *dpy = nullptr;
    ::Window win = 0;
    X11DragBridge::Hooks hooks;
    X11DragBridge *owner = nullptr;
    Atoms atoms;
    std::unordered_map<unsigned long, std::string> atom_names;  ///< atom → 名（X 服务器往返昂贵，缓存）
    std::unordered_map<std::string, unsigned long> known_atoms;  ///< 已登记名 → atom（install 期预 intern）
    DragIn in;
    DragOut out;
    Cursor cursor_accept = 0;  ///< 受理态光标（`XC_target`；惰性创建，析构释放）
    Cursor cursor_reject = 0;  ///< 拒绝态光标（`XC_pirate`）
    bool installed = false;
    bool handled = false;  ///< 最近一次派发是否被控件消费（Drop 受理信号的唯一来源）

    // ---- atom ----
    /// @brief intern 一个 atom 并缓存正反向映射。
    auto intern(const std::string &name) -> unsigned long {
        const auto it = known_atoms.find(name);
        if (it != known_atoms.end()) {
            return it->second;
        }
        const Atom a = XInternAtom(dpy, name.c_str(), False);
        known_atoms.emplace(name, static_cast<unsigned long>(a));
        atom_names.emplace(static_cast<unsigned long>(a), name);
        return static_cast<unsigned long>(a);
    }

    /// @brief atom → 名（带缓存；失败返回空串，不猜）。
    auto atom_name(unsigned long atom) -> std::string {
        if (atom == AURORA_X_NONE) {
            return {};
        }
        const auto it = atom_names.find(atom);
        if (it != atom_names.end()) {
            return it->second;
        }
        const char *raw = XGetAtomName(dpy, static_cast<Atom>(atom));
        if (raw == nullptr) {
            return {};
        }
        std::string name{raw};
        XFree(
            const_cast<char *>(raw));  // NOLINT(cppcoreguidelines-pro-type-const-cast) Xlib 的 XGetAtomName 返回 char*
        atom_names.emplace(atom, name);
        return name;
    }

    // ---- 消息发送 ----
    /// @brief 向某窗口发一条 32 位格式的 ClientMessage（Xdnd 全族共用）。
    auto send_client(unsigned long dest, unsigned long type, long l0, long l1, long l2, long l3, long l4) const
        -> void {
        XEvent ev{};
        ev.type = ClientMessage;
        ev.xclient.display = dpy;
        ev.xclient.window = static_cast<::Window>(dest);
        ev.xclient.message_type = static_cast<Atom>(type);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = l0;
        ev.xclient.data.l[1] = l1;
        ev.xclient.data.l[2] = l2;
        ev.xclient.data.l[3] = l3;
        ev.xclient.data.l[4] = l4;
        XSendEvent(dpy, static_cast<::Window>(dest), False, 0, &ev);
        XFlush(dpy);
    }

    // ---- 安装 / 卸载 ----
    auto install() -> bool {
        if (dpy == nullptr || win == 0 || installed) {
            return false;
        }
        atoms.aware = intern("XdndAware");
        atoms.enter = intern("XdndEnter");
        atoms.position = intern("XdndPosition");
        atoms.status = intern("XdndStatus");
        atoms.leave = intern("XdndLeave");
        atoms.drop = intern("XdndDrop");
        atoms.finished = intern("XdndFinished");
        atoms.selection = intern("XdndSelection");
        atoms.type_list = intern("XdndTypeList");
        atoms.incr = intern("INCR");
        atoms.action_copy = intern("XdndActionCopy");
        atoms.action_move = intern("XdndActionMove");
        atoms.action_link = intern("XdndActionLink");
        // 动作与目标名表由折算层给出（单一真源）：桥只是把它们 intern 成 atom。
        for (const std::string &name : drag::x11_registered_target_names()) {
            (void)intern(name);
        }
        for (const std::string &name : drag::x11_registered_action_names()) {
            (void)intern(name);
        }
        auto version = static_cast<unsigned long>(AURORA_XDND_VERSION);
        XChangeProperty(dpy, win, static_cast<Atom>(atoms.aware), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(&version), 1);
        if (!error_handler_installed()) {
            error_handler_installed() = true;
            (void)XSetErrorHandler(&xdnd_error_handler);
        }
        installed = true;
        X11DragBridge::register_backend();
        return true;
    }

    auto uninstall() -> void {
        if (installed && dpy != nullptr) {
            XDeleteProperty(dpy, win, static_cast<Atom>(atoms.aware));
        }
        installed = false;
        if (out.active) {
            teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
        }
        reset_drag_in();
        if (initiator_slot() == owner) {
            initiator_slot() = nullptr;
        }
        if (cursor_accept != 0) {
            XFreeCursor(dpy, cursor_accept);
            cursor_accept = 0;
        }
        if (cursor_reject != 0) {
            XFreeCursor(dpy, cursor_reject);
            cursor_reject = 0;
        }
    }

    // ---- 动作 atom ----
    [[nodiscard]] auto action_atom(DragAction action) const -> unsigned long {
        switch (action) {
            case DragAction::Move:
                return atoms.action_move;
            case DragAction::Link:
                return atoms.action_link;
            case DragAction::Copy:
                return atoms.action_copy;
            case DragAction::None:
                break;
        }
        return AURORA_X_NONE;
    }

    /// @brief 把动作约束进「本要约可广告」的集合：`Move` 需源侧能确认删除，否则退 `Copy`。
    [[nodiscard]] static auto constrain_action(const DragOffer &offer, DragAction want) -> DragAction {
        if (want == DragAction::Move && offer.on_move_confirmed == nullptr) {
            return DragAction::Copy;  // 无 Move 确认回调 ⇒ 不向 OS 声明可移动（数据丢失风险）
        }
        return want;
    }

    // ---- 拖入 ----
    auto on_client_message(unsigned long message_type, const std::array<long, 5> &data) -> void {
        if (!installed) {
            return;
        }
        if (message_type == atoms.enter) {
            handle_enter(data);
        } else if (message_type == atoms.position) {
            handle_position(data);
        } else if (message_type == atoms.leave) {
            handle_leave();
        } else if (message_type == atoms.drop) {
            handle_drop(data);
        } else if (message_type == atoms.status) {
            handle_status(data);
        } else if (message_type == atoms.finished) {
            handle_finished(data);
        }
    }

    auto handle_enter(const std::array<long, 5> &data) -> void {
        const auto source = static_cast<unsigned long>(data[0]);
        const auto flags = static_cast<unsigned long>(data[1]);
        const int version = static_cast<int>((flags >> 24) & 0xFFUL);
        std::vector<std::string> names;
        if ((flags & 0x1UL) != 0UL) {
            // 三类型以上：全清单在源窗口的 `XdndTypeList` 属性里（消息内三槽不再承载）。
            Atom actual_type = AURORA_X_NONE;
            int actual_format = 0;
            unsigned long nitems = 0;
            unsigned long after = 0;
            unsigned char *prop = nullptr;
            if (XGetWindowProperty(dpy, static_cast<::Window>(source), static_cast<Atom>(atoms.type_list), 0, 0x10000UL,
                                   False, XA_ATOM, &actual_type, &actual_format, &nitems, &after, &prop) == Success &&
                prop != nullptr) {
                if (actual_format == 32 && nitems > 0) {
                    // Xlib 属性以 unsigned char* 为字节口：先按 unsigned long 整块拷出（Xlib 保证
                    // 8 字节对齐），再逐项查名——比在原地做指针算术更直白，下标也受容器约束。
                    std::vector<unsigned long> values(nitems, 0UL);
                    std::memcpy(values.data(), prop, nitems * sizeof(unsigned long));
                    for (const unsigned long value : values) {
                        names.push_back(atom_name(value));
                    }
                }
                XFree(prop);
            }
        } else {
            for (std::size_t i = 2; i <= 4; ++i) {
                if (data.at(i) != 0) {
                    names.push_back(atom_name(static_cast<unsigned long>(data.at(i))));
                }
            }
        }
        const std::vector<std::string> mimes = drag::mimes_from_x11_target_names(names);
        // 源未走时先收口旧会话：同一指针不可能同时在两场拖放里，留着会让两个源的状态互相覆盖。
        if (in.active) {
            dispatch_drag_in(SystemDropPhase::Leave, nullptr);
            reset_drag_in();
        }
        if (mimes.empty()) {
            return;  // 无一个本库登记的格式：不建立会话、不回 Status（回也无格式可协商）
        }
        in.active = true;
        in.entered = false;
        in.source = source;
        in.version = std::min(version, AURORA_XDND_VERSION);
        in.mimes = mimes;
        // `Enter` 不在此派发：Xdnd 的 Enter 消息**不带坐标**，此刻派发会把 (0,0) 当作落点，
        // 命中测试随之落到左上角控件上。首个 `XdndPosition` 才带真实坐标，届时再派发 Enter。
    }

    auto handle_position(const std::array<long, 5> &data) -> void {
        const auto source = static_cast<unsigned long>(data[0]);
        if (!in.active || source != in.source) {
            return;
        }
        // 修饰态：Xdnd 把 `state` 放在 `l[1]` 的 bits 0-7（与 X 事件的 `state` 同语义）。
        const auto state = static_cast<unsigned int>(static_cast<unsigned long>(data[1]) & 0xFFUL);
        const auto packed = static_cast<unsigned long>(data[2]);
        const int x_root = static_cast<int>(static_cast<std::int16_t>((packed >> 16) & AURORA_XDND_COORD_MASK));
        const int y_root = static_cast<int>(static_cast<std::int16_t>(packed & AURORA_XDND_COORD_MASK));
        in.drop_time = static_cast<unsigned long>(data[3]);
        in.os_action = drag::x11_action_from_name(atom_name(static_cast<unsigned long>(data[4])));
        in.mods = mods_from_x11_state(state);
        int cx = 0;
        int cy = 0;
        ::Window child = 0;
        if (XTranslateCoordinates(dpy, XDefaultRootWindow(dpy), win, x_root, y_root, &cx, &cy, &child) == False) {
            cx = 0;
            cy = 0;
        }
        in.client_x = cx;
        in.client_y = cy;
        if (in.dropped) {
            return;  // 数据读取期内的迟到 Position：不打断、也不改变已发 Status 的语义
        }
        const SystemDropPhase phase = in.entered ? SystemDropPhase::Over : SystemDropPhase::Enter;
        in.entered = true;
        const DragAction allowed = dispatch_drag_in(phase, nullptr);
        in.status_action = allowed;
        send_status(allowed != DragAction::None, allowed);
    }

    auto handle_leave() -> void {
        if (!in.active) {
            return;
        }
        if (!in.dropped) {
            dispatch_drag_in(SystemDropPhase::Leave, nullptr);
        }
        reset_drag_in();
    }

    auto handle_drop(const std::array<long, 5> &data) -> void {
        const auto source = static_cast<unsigned long>(data[0]);
        if (!in.active || in.dropped || source != in.source) {
            return;
        }
        in.dropped = true;
        in.index = 0;
        in.phase = DragIn::ReadPhase::Idle;
        in.entries.clear();
        in.started = std::chrono::steady_clock::now();
        if (data[2] != 0) {
            in.drop_time = static_cast<unsigned long>(data[2]);
        }
        if (in.mimes.empty()) {
            finish_drag_in_reads();  // 无格式可读取：照样回 Finished（源在等它）
            return;
        }
        // 未曾派发过 Enter（源 drop 前只发过 Enter）时补一次：控件无从得知有过拖入。
        if (!in.entered) {
            in.entered = true;
            in.status_action = dispatch_drag_in(SystemDropPhase::Enter, nullptr);
        }
        start_next_read();
    }

    auto handle_status(const std::array<long, 5> &data) -> void {
        if (!out.active || out.target == 0) {
            return;
        }
        // 先落到一个具名变量再比：直接比 `static_cast` 结果时，检查会剥掉显式转换、
        // 按 `long` 与 `unsigned long` 判成符号不一致（XID 双方都是 32 位非负值）。
        const auto source = static_cast<unsigned long>(data.at(0));
        if (source != out.target) {
            return;
        }
        const auto flags = static_cast<unsigned long>(data[1]);
        out.target_accepts = (flags & 0x1UL) != 0UL;
        out.target_action = drag::x11_action_from_name(atom_name(static_cast<unsigned long>(data[4])));
        update_cursor(out.target_accepts);
    }

    auto handle_finished(const std::array<long, 5> &data) -> void {
        if (!out.active || !out.drop_sent || out.target == 0) {
            return;
        }
        // 先落到一个具名变量再比：直接比 `static_cast` 结果时，检查会剥掉显式转换、
        // 按 `long` 与 `unsigned long` 判成符号不一致（XID 双方都是 32 位非负值）。
        const auto source = static_cast<unsigned long>(data.at(0));
        if (source != out.target) {
            return;
        }
        // v5 起 `l[1]` bit0 才是「已受理并执行」；协商版本更早时该位未定义，按置位处理
        // （旧版协议下源只能假定落放已执行），动作则取最后一次发出的动作。
        const bool versioned = out.target_version >= 5;
        const auto flags = static_cast<unsigned long>(data[1]);
        const bool accepted = !versioned || (flags & 0x1UL) != 0UL;
        DragAction action = DragAction::None;
        if (accepted) {
            const DragAction reported = versioned
                                            ? drag::x11_action_from_name(atom_name(static_cast<unsigned long>(data[2])))
                                            : DragAction::None;
            action = reported != DragAction::None ? reported : out.sent_action;
        }
        teardown_drag_out(accepted ? SystemDragOutcome::Succeeded : SystemDragOutcome::Rejected, action);
    }

    /// @brief 折算并派发一次拖入回调，返回派发器写定的 `allowed`（`Drop` 态恒为原值）。
    auto dispatch_drag_in(SystemDropPhase phase, std::shared_ptr<const DragPayload> payload) -> DragAction {
        drag::DragInFrame frame;
        frame.mimes = in.mimes;
        frame.modifiers = in.mods;
        frame.os_suggested = in.os_action;
        frame.client_x = in.client_x;
        frame.client_y = in.client_y;
        frame.scale = hooks.scale_factor != nullptr ? hooks.scale_factor() : 1.0F;
        SystemDropEvent event = drag::make_drop_event(phase, frame);
        event.payload = std::move(payload);
        handled = false;
        return system_drag::ingress::dispatch(event, [this](SystemDropEvent &e) -> DragAction {
            if (hooks.emit != nullptr) {
                hooks.emit(e);  // 派发器在调用内写定 `e.allowed`（Enter / Over 两态）
            }
            handled = e.is_handled;  // Drop 态的受理信号：派发器不写 allowed，消费与否即受理与否
            return e.allowed;
        });
    }

    auto send_status(bool accept, DragAction action) const -> void {
        // `l[1]` bit0 = 受理；bit1 = 要 Position 消息。矩形发空（全零）⇒ 两读法下都等价于
        // 「指针每动一次就发」——不发 Position 就得靠 pointer motion 自己推，自动滚边会饿死。
        send_client(in.source, atoms.status, static_cast<long>(win), accept ? 3L : 2L, 0L, 0L,
                    static_cast<long>(action_atom(action)));
    }

    auto send_finished(bool accepted, DragAction action) const -> void {
        send_client(in.source, atoms.finished, static_cast<long>(win), accepted ? 1L : 0L,
                    accepted ? static_cast<long>(action_atom(action)) : 0L, 0L, 0L);
    }

    auto reset_drag_in() -> void { in = DragIn{}; }

    /// @brief 发起下一个 MIME 的 selection round-trip；全部读完则收口并派发 `Drop`。
    auto start_next_read() -> void {
        if (in.index >= in.mimes.size()) {
            finish_drag_in_reads();
            return;
        }
        const std::string &mime = in.mimes[in.index];
        const std::vector<std::string> names = drag::x11_target_names_from_mime(mime);
        std::string chosen;
        for (const std::string &name : names) {
            if (known_atoms.contains(name)) {
                chosen = name;
                break;  // 顺序即优先级：取首个已登记名（与 Win32 数值表同纪律）
            }
        }
        if (chosen.empty()) {
            ++in.index;
            start_next_read();
            return;
        }
        const unsigned long target = known_atoms[chosen];
        in.current_mime = mime;
        in.read_atom = target;
        in.buffer.clear();
        in.phase = DragIn::ReadPhase::Awaiting;
        // 结果属性名取目标 atom 名、置于**本窗口**（ICCCM 惯例，避免与外来属性撞名）。
        XConvertSelection(dpy, static_cast<Atom>(atoms.selection), static_cast<Atom>(target), static_cast<Atom>(target),
                          win, in.drop_time);
        XFlush(dpy);
    }

    /// @brief 结束当前 MIME 的读取（无数据时按空口径收下），推进到下一个。
    auto advance_read() -> void {
        if (!in.current_mime.empty()) {
            in.entries.insert_or_assign(in.current_mime, std::move(in.buffer));
        }
        in.buffer.clear();
        in.phase = DragIn::ReadPhase::Idle;
        in.current_mime.clear();
        ++in.index;
        start_next_read();
    }

    /// @brief 数据齐（或超时 / 无格式）→ 派发 `Drop` 并回 `XdndFinished`。
    auto finish_drag_in_reads() -> void {
        if (!in.active) {
            return;
        }
        auto payload = DragPayload::from_bytes(std::move(in.entries));
        const unsigned long source = in.source;
        (void)dispatch_drag_in(SystemDropPhase::Drop, std::move(payload));
        // 受理 ⇒ 回写执行动作；`Enter` / `Over` 从未派发过（源 drop 前不发 Position）时退回源建议。
        const DragAction action = in.status_action != DragAction::None ? in.status_action : in.os_action;
        send_client(source, atoms.finished, static_cast<long>(win), handled ? 1L : 0L,
                    handled ? static_cast<long>(action_atom(action)) : 0L, 0L, 0L);
        reset_drag_in();
    }

    auto on_selection_notify(unsigned long selection, unsigned long target, unsigned long property) -> void {
        if (!in.dropped || in.phase != DragIn::ReadPhase::Awaiting) {
            return;
        }
        if (selection != atoms.selection || target != in.read_atom) {
            return;  // 与本轮请求无关（迟到的旧请求 / 他人 selection）：不读、不改状态机
        }
        if (property == AURORA_X_NONE) {
            advance_read();  // 源不给该格式：按空口径收下（空内容不是失败）
            return;
        }
        Atom actual_type = AURORA_X_NONE;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long after = 0;
        unsigned char *prop = nullptr;
        // delete=True：读完即删（ICCCM 惯例），`INCR` 亦由此启动（删属性即握手的第一步）。
        const int rc = XGetWindowProperty(dpy, win, static_cast<Atom>(property), 0, 0x0FFFFFFFUL, True, AnyPropertyType,
                                          &actual_type, &actual_format, &nitems, &after, &prop);
        if (rc != Success || prop == nullptr) {
            if (prop != nullptr) {
                XFree(prop);
            }
            advance_read();
            return;
        }
        if (actual_type == static_cast<Atom>(atoms.incr)) {
            // INCR：本轮数据由后续 `PropertyNotify(NewValue)` 分片送达，零长分片即结束。
            XFree(prop);
            in.buffer.clear();
            in.phase = DragIn::ReadPhase::Incr;
            return;
        }
        if (actual_format == 8 && nitems > 0) {
            std::copy_n(prop, static_cast<std::ptrdiff_t>(nitems), std::back_inserter(in.buffer));
        }
        XFree(prop);
        advance_read();
    }

    // ---- INCR 分片（双向） ----
    auto on_property_notify(unsigned long window, unsigned long atom, int state) -> void {
        if (!installed) {
            return;
        }
        if (window == static_cast<unsigned long>(win) && in.dropped && in.phase == DragIn::ReadPhase::Incr &&
            atom == in.read_atom && state == AURORA_X_PROP_NEW_VALUE) {
            // 拖入收块：零长即收尾（ICCCM：分片以零长属性结束）。
            Atom actual_type = AURORA_X_NONE;
            int actual_format = 0;
            unsigned long nitems = 0;
            unsigned long after = 0;
            unsigned char *prop = nullptr;
            const int rc = XGetWindowProperty(dpy, win, static_cast<Atom>(atom), 0, 0x0FFFFFFFUL, True, AnyPropertyType,
                                              &actual_type, &actual_format, &nitems, &after, &prop);
            if (rc == Success && prop != nullptr) {
                if (nitems == 0) {
                    XFree(prop);
                    advance_read();
                    return;
                }
                if (actual_format == 8) {
                    std::copy_n(prop, static_cast<std::ptrdiff_t>(nitems), std::back_inserter(in.buffer));
                }
                XFree(prop);
            } else if (prop != nullptr) {
                XFree(prop);
            }
            return;
        }
        if (out.incr_active && window == out.incr_requestor && atom == out.incr_property &&
            state == AURORA_X_PROP_DELETE) {
            push_incr_chunk();  // 请求方已取走上一块（删除 = 握手），发下一块或收尾
        }
    }

    /// @brief 源侧 `INCR`：发下一块（偏移到尾则发零长收尾）。
    auto push_incr_chunk() -> void {
        if (!out.incr_active) {
            return;
        }
        const std::size_t remaining = out.incr_data.size() - out.incr_offset;
        const std::size_t n = std::min(AURORA_XDND_CHUNK, remaining);
        const unsigned char *src =
            out.incr_offset < out.incr_data.size() ? &out.incr_data.at(out.incr_offset) : nullptr;
        XChangeProperty(
            dpy, static_cast<::Window>(out.incr_requestor), static_cast<Atom>(out.incr_property),
            static_cast<Atom>(out.incr_target), 8, PropModeReplace,
            const_cast<unsigned char *>(src),  // NOLINT(cppcoreguidelines-pro-type-const-cast) Xlib 只读该缓冲
            static_cast<int>(n));
        XFlush(dpy);
        out.incr_offset += n;
        if (n == 0) {
            out.incr_active = false;
            out.incr_data.clear();
            out.incr_offset = 0;
        }
    }

    // ---- 拖出应答 ----
    auto on_selection_request(unsigned long requestor, unsigned long selection, unsigned long target,
                              unsigned long property, unsigned long time) -> void {
        if (!out.active || selection != atoms.selection || property == AURORA_X_NONE) {
            refuse_selection(requestor, selection, target, time);
            return;
        }
        const std::string name = atom_name(target);
        const auto mime = drag::mime_from_x11_target_name(name);
        if (!mime.has_value()) {
            refuse_selection(requestor, selection, target, time);
            return;
        }
        if (std::find(out.mimes.begin(), out.mimes.end(), *mime) == out.mimes.end()) {
            refuse_selection(requestor, selection, target, time);  // 未广告的格式：如实拒绝（不猜）
            return;
        }
        // 要约先拷一份：`provide` 失败会走会话收口（清 `out.offer`），不得在回调后读已清成员。
        const DragOffer offer = out.offer;
        if (offer.provide == nullptr) {
            refuse_selection(requestor, selection, target, time);
            return;
        }
        const Result<std::vector<std::uint8_t>> bytes = offer.provide(*mime);
        if (!bytes) {
            // 广告了却给不出：与 Win32 侧同罪（会话按失败结束），不把失败伪装成格式不存在。
            refuse_selection(requestor, selection, target, time);
            teardown_drag_out(SystemDragOutcome::Failed, DragAction::None);
            return;
        }
        const std::vector<std::uint8_t> &data = bytes.value();
        if (data.size() > AURORA_XDND_DIRECT_LIMIT) {
            // INCR：先回一个 `INCR` 类型的属性（内容是建议单片大小），握手由请求方删属性启动。
            auto chunk = static_cast<unsigned long>(AURORA_XDND_CHUNK);
            XChangeProperty(dpy, static_cast<::Window>(requestor), static_cast<Atom>(property),
                            static_cast<Atom>(atoms.incr), 32, PropModeReplace,
                            reinterpret_cast<unsigned char *>(&chunk), 1);
            // 监听请求方窗口的属性变化（外来窗口也允许 XSelectInput；删除通知据此到达本客户端）。
            XSelectInput(dpy, static_cast<::Window>(requestor), PropertyChangeMask);
            out.incr_active = true;
            out.incr_requestor = requestor;
            out.incr_property = property;
            out.incr_target = target;
            out.incr_data = data;
            out.incr_offset = 0;
        } else {
            const unsigned char *src = data.empty() ? nullptr : data.data();
            XChangeProperty(
                dpy, static_cast<::Window>(requestor), static_cast<Atom>(property), static_cast<Atom>(target), 8,
                PropModeReplace,
                const_cast<unsigned char *>(src),  // NOLINT(cppcoreguidelines-pro-type-const-cast) Xlib 只读该缓冲
                static_cast<int>(data.size()));
        }
        send_selection_notify(requestor, selection, target, property, time);
    }

    auto send_selection_notify(unsigned long requestor, unsigned long selection, unsigned long target,
                               unsigned long property, unsigned long time) const -> void {
        XEvent ev{};
        ev.type = SelectionNotify;
        ev.xselection.display = dpy;
        ev.xselection.requestor = static_cast<::Window>(requestor);
        ev.xselection.selection = static_cast<Atom>(selection);
        ev.xselection.target = static_cast<Atom>(target);
        ev.xselection.property = static_cast<Atom>(property);
        ev.xselection.time = static_cast<Time>(time);
        XSendEvent(dpy, static_cast<::Window>(requestor), False, 0, &ev);
        XFlush(dpy);
    }

    auto refuse_selection(unsigned long requestor, unsigned long selection, unsigned long target,
                          unsigned long time) const -> void {
        send_selection_notify(requestor, selection, target, AURORA_X_NONE, time);  // property=None = 拒绝
    }

    // ---- 拖出驱动 ----
    auto on_motion(unsigned int state, int x_root, int y_root, unsigned long time) -> void {
        if (!out.active) {
            return;
        }
        int rx = x_root;
        int ry = y_root;
        const unsigned long target = find_target(&rx, &ry);
        if (target != out.target) {
            if (out.target != 0) {
                send_client(out.target, atoms.leave, static_cast<long>(win), 0L, 0L, 0L, 0L);
            }
            out.target = target;
            out.target_version = target != 0 ? window_version(target) : 0;
            out.target_accepts = false;
            out.target_action = DragAction::None;
            if (target != 0) {
                send_enter(target);
            }
        }
        if (out.target == 0) {
            update_cursor(false);
            return;
        }
        const DragAction want = constrain_action(
            out.offer, system_drag::proposed_action_for(mods_from_x11_state(state), out.offer.default_action));
        out.sent_action = want;
        send_position(out.target, rx, ry, time, want);
    }

    auto on_button_release(unsigned long time) -> void {
        if (!out.active) {
            return;
        }
        if (out.target == 0) {
            teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);  // 全程无目标：用户放弃
            return;
        }
        if (!out.target_accepts) {
            send_client(out.target, atoms.leave, static_cast<long>(win), 0L, 0L, 0L, 0L);
            teardown_drag_out(SystemDragOutcome::Rejected, DragAction::None);  // 有目标但它不接受
            return;
        }
        send_client(out.target, atoms.drop, static_cast<long>(win), 0L, static_cast<long>(time), 0L, 0L);
        out.drop_sent = true;
        out.drop_at = std::chrono::steady_clock::now();
        // 立即解 grab：目标随后要发 `SelectionRequest` 并等我们的属性写入，grab 留着只挡事
        // （规范亦建议 drop 后尽快释放），会话余下部分由 `XdndFinished` 收尾。
        if (out.grabbed) {
            XUngrabPointer(dpy, CurrentTime);
            out.grabbed = false;
        }
    }

    auto on_key_press(int key_code) -> bool {
        if (!out.active) {
            return false;
        }
        if (key_code == static_cast<int>(KeyCode::Escape)) {
            teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
            return true;
        }
        return true;  // 会话期内一律吞噬：拖放是模态手势，按键不得漏给页面
    }

    auto window_version(unsigned long window) const -> int {
        Atom actual_type = AURORA_X_NONE;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long after = 0;
        unsigned char *prop = nullptr;
        if (XGetWindowProperty(dpy, static_cast<::Window>(window), static_cast<Atom>(atoms.aware), 0, 1, False, XA_ATOM,
                               &actual_type, &actual_format, &nitems, &after, &prop) != Success) {
            return 0;
        }
        int version = 0;
        if (prop != nullptr) {
            if (actual_type == XA_ATOM && actual_format == 32 && nitems >= 1) {
                unsigned long value = 0;
                std::memcpy(&value, prop, sizeof(value));
                version = static_cast<int>(value);
            }
            XFree(prop);
        }
        return version;
    }

    /// @brief 由根坐标下的最深子窗上溯，找第一个广告 `XdndAware` 的窗口（带 child → target 缓存）。
    auto find_target(int *x_root, int *y_root) -> unsigned long {
        const ::Window root = XDefaultRootWindow(dpy);
        ::Window rr = 0;
        ::Window child = 0;
        int rx = 0;
        int ry = 0;
        int cx = 0;
        int cy = 0;
        unsigned int mask = 0;
        if (XQueryPointer(dpy, root, &rr, &child, &rx, &ry, &cx, &cy, &mask) != False) {
            // 以服务器当前指针位置为准（事件坐标在压缩 / 延迟投递下可能与真值有偏）。
            *x_root = rx;
            *y_root = ry;
        }
        if (child == 0) {
            return 0;
        }
        const auto cached = out.target_cache.find(static_cast<unsigned long>(child));
        if (cached != out.target_cache.end()) {
            return cached->second;
        }
        unsigned long found = 0;
        auto w = static_cast<unsigned long>(child);
        for (int i = 0; i < AURORA_XDND_TREE_DEPTH && w != 0; ++i) {
            if (window_version(w) > 0) {
                found = w;
                break;
            }
            ::Window parent = 0;
            ::Window tree_root = 0;
            ::Window *children = nullptr;
            unsigned int count = 0;
            if (XQueryTree(dpy, static_cast<::Window>(w), &tree_root, &parent, &children, &count) == 0) {
                break;
            }
            if (children != nullptr) {
                XFree(children);
            }
            w = static_cast<unsigned long>(parent);
        }
        if (out.target_cache.size() >= AURORA_XDND_CACHE_LIMIT) {
            out.target_cache.clear();
        }
        out.target_cache.emplace(static_cast<unsigned long>(child), found);
        return found;
    }

    auto send_enter(unsigned long target) -> void {
        const std::size_t count = out.mimes.size();
        std::vector<unsigned long> types;
        types.reserve(count);
        for (const std::string &mime : out.mimes) {
            const std::vector<std::string> names = drag::x11_target_names_from_mime(mime);
            if (names.empty()) {
                continue;
            }
            types.push_back(intern(names.front()));  // 每个 MIME 只广告首选目标名（避免清单爆炸）
        }
        const bool many = types.size() > 3;
        const long flags = (static_cast<long>(out.target_version) << 24) | (many ? 1L : 0L);
        long l2 = 0;
        long l3 = 0;
        long l4 = 0;
        if (!many) {
            if (!types.empty()) {
                l2 = static_cast<long>(types[0]);
            }
            if (types.size() > 1) {
                l3 = static_cast<long>(types[1]);
            }
            if (types.size() > 2) {
                l4 = static_cast<long>(types[2]);
            }
        }
        send_client(target, atoms.enter, static_cast<long>(win), flags, l2, l3, l4);
        if (many) {
            // 三类型以上：全清单写进**源窗口**的 `XdndTypeList`（目标侧据此读取，见 handle_enter）。
            std::vector<unsigned long> values = types;
            XChangeProperty(dpy, win, static_cast<Atom>(atoms.type_list), XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char *>(values.data()), static_cast<int>(values.size()));
        }
    }

    auto send_position(unsigned long target, int x_root, int y_root, unsigned long time, DragAction action) const
        -> void {
        const long packed = (static_cast<long>(x_root & 0xFFFF) << 16) | static_cast<long>(y_root & 0xFFFF);
        // `l[1]` 恒 0：修饰键的语义已由 `l[4]` 的动作 atom 承载（源侧完成翻译），保留位不再复用。
        send_client(target, atoms.position, static_cast<long>(win), 0L, packed, static_cast<long>(time),
                    static_cast<long>(action_atom(action)));
    }

    auto update_cursor(bool accepts) -> void {
        if (!out.grabbed) {
            return;
        }
        if (out.cursor_applied && accepts == out.target_accepts) {
            return;
        }
        out.cursor_applied = true;
        out.target_accepts = accepts;
        Cursor &slot = accepts ? cursor_accept : cursor_reject;
        if (slot == 0) {
            slot = XCreateFontCursor(dpy, accepts ? XC_target : XC_pirate);  // 惰性创建：不拖放则零 X 资源
        }
        if (slot != 0) {
            // 事件掩码必须是 grab 时掩码的子集（X 协议要求），否则换光标即 BadValue。
            XChangeActivePointerGrab(dpy, PointerMotionMask | ButtonReleaseMask, slot, CurrentTime);
        }
    }

    auto begin_drag_out(const DragOffer &offer) -> bool {
        if (out.active || dpy == nullptr) {
            return false;
        }
        std::vector<std::string> mimes;
        for (const std::string &mime : offer.formats) {
            if (!drag::x11_target_names_from_mime(mime).empty() && offer.provide != nullptr) {
                mimes.push_back(mime);
            }
        }
        if (mimes.empty()) {
            return false;  // 无一格式可广告到 X11：如实不发起（不谎报会话已建立）
        }
        reset_drag_out_state();
        out.offer = offer;
        out.mimes = std::move(mimes);
        const int gs = XGrabPointer(dpy, win, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                                    GrabModeAsync, GrabModeAsync, AURORA_X_NONE, AURORA_X_NONE, CurrentTime);
        if (gs != GrabSuccess) {
            // 日志是流拼接（`log_concat`），不是 printf 格式：`%d` 不会被替换，直接把值拼上去。
            AURORA_LOG_WARN("system-drag", "XGrabPointer failed: status=", gs);
            reset_drag_out_state();
            return false;
        }
        out.grabbed = true;
        XSetSelectionOwner(dpy, static_cast<Atom>(atoms.selection), win, CurrentTime);
        out.owns_selection = XGetSelectionOwner(dpy, static_cast<Atom>(atoms.selection)) == win;
        if (!out.owns_selection) {
            AURORA_LOG_WARN("system-drag", "XdndSelection ownership not acquired");
            teardown_drag_out(SystemDragOutcome::Failed, DragAction::None);
            return false;
        }
        int rx = 0;
        int ry = 0;
        out.target = find_target(&rx, &ry);
        out.target_version = out.target != 0 ? window_version(out.target) : 0;
        if (out.target != 0) {
            send_enter(out.target);
            out.sent_action = constrain_action(
                out.offer, out.offer.default_action == DragAction::None ? DragAction::Copy : out.offer.default_action);
            send_position(out.target, rx, ry, CurrentTime, out.sent_action);
        }
        update_cursor(false);
        out.active = true;
        return true;
    }

    auto cancel_drag_out() -> void {
        if (!out.active) {
            return;
        }
        teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
    }

    auto teardown_drag_out(SystemDragOutcome outcome, DragAction action) -> void {
        if (!out.active) {
            return;
        }
        if (out.grabbed) {
            XUngrabPointer(dpy, CurrentTime);
        }
        if (out.owns_selection && XGetSelectionOwner(dpy, static_cast<Atom>(atoms.selection)) == win) {
            XSetSelectionOwner(dpy, static_cast<Atom>(atoms.selection), AURORA_X_NONE, CurrentTime);
        }
        const bool notify = true;
        reset_drag_out_state();
        if (notify) {
            // 终态登记的**唯一**通道（事件层在帧循环排空内投递 `on_end`，含 Move 确认次序）。
            complete_drag_out_session(outcome, action);
        }
    }

    /// @brief 清在途会话态（保留目标缓存与已建光标：前者跨会话有效，后者是 X 资源须复用）。
    auto reset_drag_out_state() -> void {
        auto cache = std::move(out.target_cache);
        out = DragOut{};
        out.target_cache = std::move(cache);
    }

    auto tick() -> void {
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::milliseconds timeout = system_drag_read_timeout();
        // 拖入：源迟迟不给数据 ⇒ 按已读到的部分派发 `Drop` 并回 Finished（不悬挂、不谎报成功）。
        if (in.dropped && now - in.started > timeout) {
            finish_drag_in_reads();
        }
        // 拖出：目标收了 drop 却永不回 Finished（对端崩溃 / 不合规实现）⇒ 按取消收口。
        if (out.active && out.drop_sent && now - out.drop_at > timeout) {
            teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
        }
    }
};

// ---- 桥的公共面（全部转发给状态机） ----

X11DragBridge::X11DragBridge(void *display, unsigned long window, Hooks hooks)
    : impl_(std::make_unique<Impl>()), display_(display), window_(window), hooks_(std::move(hooks)) {
    impl_->dpy = static_cast<Display *>(display);
    impl_->win = static_cast<::Window>(window);
    impl_->hooks = hooks_;
    impl_->owner = this;
}

X11DragBridge::~X11DragBridge() { uninstall(); }

auto X11DragBridge::install() -> bool { return impl_->install(); }

auto X11DragBridge::uninstall() -> void { impl_->uninstall(); }

auto X11DragBridge::on_client_message(unsigned long message_type, const std::array<long, 5> &data) -> void {
    impl_->on_client_message(message_type, data);
}

auto X11DragBridge::on_selection_notify(unsigned long selection, unsigned long target, unsigned long property) -> void {
    impl_->on_selection_notify(selection, target, property);
}

auto X11DragBridge::on_selection_request(unsigned long requestor, unsigned long selection, unsigned long target,
                                         unsigned long property, unsigned long time) -> void {
    impl_->on_selection_request(requestor, selection, target, property, time);
}

auto X11DragBridge::on_property_notify(unsigned long window, unsigned long atom, int state) -> void {
    impl_->on_property_notify(window, atom, state);
}

auto X11DragBridge::on_motion(unsigned int state, int x_root, int y_root, unsigned long time) -> void {
    impl_->on_motion(state, x_root, y_root, time);
}

auto X11DragBridge::on_button_release(unsigned long time) -> void { impl_->on_button_release(time); }

auto X11DragBridge::on_key_press(int key_code) -> bool { return impl_->on_key_press(key_code); }

auto X11DragBridge::tick() -> void { impl_->tick(); }

auto X11DragBridge::drag_out_active() const -> bool { return impl_->out.active; }

auto X11DragBridge::drag_in_active() const -> bool { return impl_->in.active; }

auto X11DragBridge::note_initiator(X11DragBridge *bridge) -> void { initiator_slot() = bridge; }

auto X11DragBridge::initiator() -> X11DragBridge * { return initiator_slot(); }

auto X11DragBridge::begin_drag_out(const DragOffer &offer) -> bool { return impl_->begin_drag_out(offer); }

auto X11DragBridge::cancel_drag_out() -> void { impl_->cancel_drag_out(); }

auto X11DragBridge::register_backend() -> void {
    if (backend_registered()) {
        return;
    }
    backend_registered() = true;
    register_system_drag_out_backend(SystemDragOutBackend{
        .begin = [](const DragOffer &offer) -> bool {
            X11DragBridge *self = initiator();
            return self != nullptr && self->begin_drag_out(offer);
        },
        .cancel = []() -> void {
            X11DragBridge *self = initiator();
            if (self != nullptr) {
                self->cancel_drag_out();
            }
        },
    });
}

}  // namespace aurora::detail

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX
