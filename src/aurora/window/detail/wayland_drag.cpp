// Wayland 系统拖放桥实现（契约与门控见同名头文件）。
//
// 协议要点（wl_data_device / wl_data_source，核心协议；字段布局与规范同源，不在此重述规范文本）：
//   * 拖入（target）：`data_offer` 引入 offer → `offer` 事件逐条给出 MIME 名 → `enter` 携带
//     serial 与表面坐标 → `motion` 更新位置 → `drop` 落放。目标侧应答 `accept(serial, mime)`
//     （mime 为 null = 拒绝）与 `set_actions(dest_actions, preferred)`（v3），落放后逐 MIME 调
//     `receive(mime, fd)` 让对端往管道里写，本端读完再 `finish()` + `destroy()`。
//   * 拖出（source）：建 `wl_data_source` → `offer(name)` 逐个广告 → `set_actions(supported)`
//     （v3）→ `start_drag(source, origin_surface, null, serial)` 起手（serial 必须是本客户端
//     某次 `wl_pointer.button` 按下的 serial）。对端要数据时 `send(mime, fd)` 到达，本端把字节
//     写进该 fd 后关闭。
//   * 终态：v3 下 `dnd_drop_performed` + `dnd_finished` ⇒ 成功（动作取 `action` 事件）；
//     `cancelled` ⇒ 用户取消。v1/v2 无前三个事件，`cancelled` 一词二义（取消 / 会话结束），
//     故按「是否应答过 send」分叉折算，判据见 `on_source_cancelled`。
//
// 为何取数 / 发数都走**非阻塞管道 + 帧内推进**：`receive` 给的写端与 `send` 给的读端都由
// **对端进程**控制读写节奏，同步 `read()` / `write()` 一整个大载荷即是一帧的整段停顿（本库是
// 单线程 UI + 帧循环），对端不读还会把本端钉死在 `write()` 上。故两端都设非阻塞、在 `tick()`
// 里逐帧推进，并按墙钟超时兑现——不悬挂、不谎报。

#include "aurora/window/detail/wayland_drag.h"

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

// aurora 头先于 wayland 头（与 `x11_drag.cpp` 同款纪律：平台协议头关在单个 TU 内，
// 并把「包含顺序即语义」这件事从排版器手里拿回来）。
// clang-format off
#include "aurora/app/mime_types.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/event/detail/system_drag_backend.h"
#include "aurora/event/detail/system_drag_ingress.h"
#include "aurora/window/detail/system_drag_translate.h"

#include <wayland-client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
// clang-format on

namespace aurora::detail {
namespace {

/// @brief 管道单块大小（字节）：两端读写都用它，与同步 / 异步阈值语义无关。
constexpr std::size_t AURORA_WL_PIPE_CHUNK = 4096U;

/// @brief 进程级发起槽（多窗口下由宿主在事件泵入口刷新）。
auto initiator_slot() -> WaylandDragBridge *& {
    static WaylandDragBridge *slot = nullptr;
    return slot;
}

/// @brief 拖出后端是否已注册（进程级一次性；事件层另按「先到者优先」去重）。
auto backend_registered() -> bool & {
    static bool registered = false;
    return registered;
}

/// @brief 剪贴板 selection 转发器槽（进程级一次性；由 `WaylandClipboardBridge` 登记）。
auto clipboard_forwarder_slot() -> std::function<void(wl_data_offer *, const std::vector<std::string> &)> & {
    static std::function<void(wl_data_offer *, const std::vector<std::string> &)> forwarder;
    return forwarder;
}

/// @brief 把 fd 设为非阻塞（读端用；失败不影响正确性，只影响「会不会阻塞一帧」）。
auto set_nonblocking(int fd) -> void {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/// @brief 把动作约束进「本要约可声明」的集合：`Move` 需源侧能确认删除，否则退 `Copy`。
///
/// 与 X11 桥的 `constrain_action` 同口径：没有 `on_move_confirmed` 就向 OS 声明可移动，
/// 等于承诺「落放后删原数据」却没有删的路，数据丢失风险由调用方承担。
[[nodiscard]] auto constrain_action(const DragOffer &offer, DragAction want) -> DragAction {
    if (want == DragAction::Move && offer.on_move_confirmed == nullptr) {
        return DragAction::Copy;
    }
    return want;
}

}  // namespace

// ---- 协议状态机（协议态与时序集中于此；C 回调 thunk 只做「还原 Impl* 后转发」）----
// 必须定义在 `aurora::detail` 而非匿名 namespace：嵌套类的定义点要能「包住」外围类
// （`WaylandDragBridge` 在 `aurora::detail`），塞进匿名 namespace 即
// 「declaration ... in a namespace which does not enclose the class」。
struct WaylandDragBridge::Impl {
    wl_display *dpy = nullptr;
    wl_data_device_manager *mgr = nullptr;
    std::uint32_t mgr_version = 0;
    wl_seat *seat = nullptr;
    wl_surface *surface = nullptr;
    wl_data_device *device = nullptr;
    Hooks hooks{};
    WaylandDragBridge *owner = nullptr;
    bool installed = false;

    /// @brief 拖入侧会话态（本窗口是 target）。
    struct DragIn {
        wl_data_offer *offer = nullptr;  ///< 当前 offer（owned）
        std::vector<std::string> offer_names;  ///< 对端广告的线上 MIME 名（`offer` 事件累积）
        std::vector<std::string> mimes;  ///< 折算后的规范 MIME 清单（顺序即优先级）
        std::uint32_t source_actions = 0;  ///< 对端（源）声明的能力位
        std::uint32_t enter_serial = 0;  ///< `enter` 事件的 serial（`accept` 必需）
        bool active = false;  ///< 会话进行中（enter 已到、leave/drop 未到）
        bool entered = false;  ///< 已派发过 `Enter`
        bool dropped = false;  ///< 已收到 `drop`
        double x = 0.0;  ///< 表面本地坐标（逻辑）
        double y = 0.0;
        DragAction os_action = DragAction::None;  ///< 合成器报的动作（`action` 事件，v3）
        DragAction allowed = DragAction::None;  ///< 派发器写定的允许动作（`accept` 的依据）
        std::size_t index = 0;  ///< 下一个待读 MIME 的下标
        std::string current_mime;  ///< 正在读的 MIME
        int read_fd = -1;  ///< `receive` 管道的读端（-1 = 无在途读取）
        std::vector<std::uint8_t> buffer;  ///< 当前 MIME 已读到的字节
        std::unordered_map<std::string, std::vector<std::uint8_t>> entries;  ///< 已读完的 MIME → 字节
        std::chrono::steady_clock::time_point started;  ///< 落放时刻（超时判据的起点）
    };

    /// @brief 拖出侧会话态（本窗口是 source）。
    struct DragOut {
        bool active = false;  ///< 会话进行中
        DragOffer offer{};  ///< 控件要约（`begin` 时整份拷入，见下方注记）
        std::vector<std::string> mimes;  ///< 已广告的规范 MIME（只含可映射到线上的）
        wl_data_source *source = nullptr;  ///< 数据源对象（owned）
        std::uint32_t press_serial = 0;  ///< 发起用的按键 serial（宿主在按下时刷新）
        bool target_accepts = false;  ///< `target` 事件给了非空 mime
        std::string target_mime;  ///< 目标接受的那个线上名
        bool sent_any = false;  ///< 应答过至少一次 `send`
        bool drop_performed = false;  ///< 收到过 `dnd_drop_performed`（v3）
        DragAction last_action = DragAction::None;  ///< 最后一次 `action` 事件折算的动作（v3）
        /// @brief 在途写出作业（至多一个：同一时刻只有一个 `send` 事件）。
        struct Send {
            int fd = -1;  ///< `send` 给的写端（-1 = 无在途）
            std::vector<std::uint8_t> data;  ///< 待写字节
            std::size_t offset = 0;  ///< 已写字节数
        } send;
    };

    /// @brief 刚由 `data_offer` 引入、尚未被 `enter` / `selection` 采用的 offer。
    ///
    /// 之所以要 pending：`data_offer` 与随后的 `offer` 事件**早于** `enter` 到达，名称必须在
    /// 采用前有个落脚处；而剪贴板 selection 的 offer 也会走同一条事件流，不采用就得销毁
    /// （协议要求客户端销毁自己不用的 data offer）。
    wl_data_offer *pending_offer = nullptr;
    std::vector<std::string> pending_names;

    /// @brief 最近一次经转发器交给剪贴板桥的 selection offer（**不拥有**：归剪贴板桥销毁）。
    ///
    /// 只用于两件事：① `on_new_offer` / `uninstall` 不误销毁它；② 该 offer 的迟到 `offer`
    /// 事件按增量补发给剪贴板桥（见 `offer_offer`）。
    wl_data_offer *clipboard_offer = nullptr;

    DragIn in;
    DragOut out;
    bool handled = false;  ///< 派发器写定的 `is_handled`（Drop 态的受理信号）

    // ---- 安装 / 卸载 ----
    auto install() -> bool;
    auto uninstall() -> void;

    // ---- 拖入 ----
    auto on_new_offer(wl_data_offer *offer) -> void;
    auto on_selection(wl_data_offer *offer) -> void;
    auto on_enter(std::uint32_t serial, wl_fixed_t x, wl_fixed_t y, wl_data_offer *offer) -> void;
    auto on_leave() -> void;
    auto on_motion(wl_fixed_t x, wl_fixed_t y) -> void;
    auto on_drop() -> void;
    auto apply_accept(DragAction allowed) -> void;
    [[nodiscard]] auto dispatch_drag_in(SystemDropPhase phase, std::shared_ptr<const DragPayload> payload)
        -> DragAction;
    auto start_next_read() -> void;
    auto pump_read() -> void;
    auto finish_drag_in_reads() -> void;
    auto reset_drag_in() -> void;

    // ---- 拖出 ----
    auto begin_drag_out(const DragOffer &offer) -> bool;
    auto cancel_drag_out() -> void;
    auto teardown_drag_out(SystemDragOutcome outcome, DragAction action) -> void;
    auto on_source_target(const char *mime_type) -> void;
    auto on_source_send(const char *mime_type, int fd) -> void;
    auto on_source_cancelled() -> void;
    auto on_source_drop_performed() -> void;
    auto on_source_finished() -> void;
    auto on_source_action(std::uint32_t dnd_action) -> void;
    auto pump_send() -> void;

    // ---- 帧循环看守 ----
    auto tick() -> void;
};

namespace {

// ---- wl_data_offer 监听器（target 侧的格式清单 / 能力位 / 动作）----
void offer_offer(void *data, wl_data_offer *offer, const char *mime_type) {
    auto *self = static_cast<WaylandDragBridge::Impl *>(data);
    if (mime_type == nullptr) {
        return;
    }
    // 已被剪贴板桥采用的 offer 的迟到名称：按增量补发（协议规定名称先于 selection 到达，
    // 此分支只兜不合规合成器；不补发就会静默丢格式）。
    auto &forwarder = clipboard_forwarder_slot();
    if (forwarder != nullptr && offer == self->clipboard_offer) {
        forwarder(offer, {std::string{mime_type}});
        return;
    }
    self->pending_names.emplace_back(mime_type);
}

void offer_source_actions(void *data, wl_data_offer * /*offer*/, std::uint32_t source_actions) {
    static_cast<WaylandDragBridge::Impl *>(data)->in.source_actions = source_actions;
}

void offer_action(void *data, wl_data_offer * /*offer*/, std::uint32_t dnd_action) {
    static_cast<WaylandDragBridge::Impl *>(data)->in.os_action = drag::action_from_wayland_dnd_action(dnd_action);
}

constexpr wl_data_offer_listener AURORA_OFFER_LISTENER = {
    .offer = offer_offer, .source_actions = offer_source_actions, .action = offer_action};

// ---- wl_data_device 监听器（target 侧四态 + selection 兜底）----
void device_data_offer(void *data, wl_data_device * /*device*/, wl_data_offer *offer) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_new_offer(offer);
}

void device_enter(void *data, wl_data_device * /*device*/, std::uint32_t serial, wl_surface * /*surface*/, wl_fixed_t x,
                  wl_fixed_t y, wl_data_offer *offer) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_enter(serial, x, y, offer);
}

void device_leave(void *data, wl_data_device * /*device*/) { static_cast<WaylandDragBridge::Impl *>(data)->on_leave(); }

void device_motion(void *data, wl_data_device * /*device*/, std::uint32_t /*time*/, wl_fixed_t x, wl_fixed_t y) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_motion(x, y);
}

void device_drop(void *data, wl_data_device * /*device*/) { static_cast<WaylandDragBridge::Impl *>(data)->on_drop(); }

void device_selection(void *data, wl_data_device * /*device*/, wl_data_offer *offer) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_selection(offer);
}

constexpr wl_data_device_listener AURORA_DATA_DEVICE_LISTENER = {.data_offer = device_data_offer,
                                                                 .enter = device_enter,
                                                                 .leave = device_leave,
                                                                 .motion = device_motion,
                                                                 .drop = device_drop,
                                                                 .selection = device_selection};

// ---- wl_data_source 监听器（source 侧的取数 / 终态 / 动作协商）----
void source_target(void *data, wl_data_source * /*source*/, const char *mime_type) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_source_target(mime_type);
}

void source_send(void *data, wl_data_source * /*source*/, const char *mime_type, std::int32_t fd) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_source_send(mime_type, fd);
}

void source_cancelled(void *data, wl_data_source * /*source*/) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_source_cancelled();
}

void source_dnd_drop_performed(void *data, wl_data_source * /*source*/) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_source_drop_performed();
}

void source_dnd_finished(void *data, wl_data_source * /*source*/) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_source_finished();
}

void source_action(void *data, wl_data_source * /*source*/, std::uint32_t dnd_action) {
    static_cast<WaylandDragBridge::Impl *>(data)->on_source_action(dnd_action);
}

constexpr wl_data_source_listener AURORA_DATA_SOURCE_LISTENER = {.target = source_target,
                                                                 .send = source_send,
                                                                 .cancelled = source_cancelled,
                                                                 .dnd_drop_performed = source_dnd_drop_performed,
                                                                 .dnd_finished = source_dnd_finished,
                                                                 .action = source_action};

}  // namespace

// ---- 安装 / 卸载 ----

auto WaylandDragBridge::Impl::install() -> bool {
    if (installed || mgr == nullptr || seat == nullptr || dpy == nullptr) {
        return false;
    }
    device = wl_data_device_manager_get_data_device(mgr, seat);
    if (device == nullptr) {
        AURORA_LOG_WARN("system-drag", "Wayland: get_data_device returned null; drag unavailable.");
        return false;
    }
    wl_data_device_add_listener(device, &AURORA_DATA_DEVICE_LISTENER, this);
    installed = true;
    WaylandDragBridge::register_backend();
    return true;
}

auto WaylandDragBridge::Impl::uninstall() -> void {
    installed = false;
    if (out.active) {
        teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
    }
    reset_drag_in();
    if (pending_offer != nullptr && pending_offer != clipboard_offer) {
        wl_data_offer_destroy(pending_offer);
        pending_offer = nullptr;
        pending_names.clear();
    }
    // `clipboard_offer` 归剪贴板桥销毁（宿主保证先销毁剪贴板桥再销毁本桥）。
    clipboard_offer = nullptr;
    if (device != nullptr) {
        wl_data_device_destroy(device);
        device = nullptr;
    }
    if (initiator_slot() == owner) {
        initiator_slot() = nullptr;
    }
}

// ---- 拖入 ----

auto WaylandDragBridge::Impl::on_new_offer(wl_data_offer *offer) -> void {
    if (offer == nullptr) {
        return;
    }
    // 上一个未被采用的 offer（剪贴板 selection 的）即刻销毁：协议要求客户端销毁自己不用的
    // data offer，不销毁即每换一次剪贴板漏一个服务端对象。
    if (pending_offer != nullptr && pending_offer != offer && pending_offer != clipboard_offer) {
        wl_data_offer_destroy(pending_offer);
    }
    pending_offer = offer;
    pending_names.clear();
    wl_data_offer_add_listener(offer, &AURORA_OFFER_LISTENER, this);
}

auto WaylandDragBridge::Impl::on_selection(wl_data_offer *offer) -> void {
    // selection 事件永远不是拖放 offer（拖放走 enter）；它是剪贴板 selection。本桥与剪贴板桥共用
    // 同一枚 `wl_data_device`（每 seat 唯一），剪贴板桥无法再加自己的监听器，故把 offer 与已收集
    // 的名称交给剪贴板桥持有 / 销毁，本桥不再管理（避免重复销毁协议对象）。无剪贴板桥时沿用原行为
    // 丢弃非拖放 offer。
    //
    // 名称与 selection 的先后：协议规定 `data_offer` 之后**立即**逐个发 `offer`，`selection`
    // 在其后到达，故 `pending_names` 此刻就是该 offer 的完整清单；若 offer 未经本桥的
    // `data_offer`（pending 对不上），只交 offer 本身，迟到名称由 `offer_offer` 补发。
    auto &forwarder = clipboard_forwarder_slot();
    if (forwarder != nullptr) {
        if (offer == nullptr && pending_offer != nullptr) {
            // 清空事件不携带 offer：未被采用的待定 offer 自此无人认领，就地销毁。
            wl_data_offer_destroy(pending_offer);
        }
        if (offer != nullptr && offer != pending_offer) {
            forwarder(offer, {});
        } else {
            forwarder(offer, pending_names);
        }
        clipboard_offer = offer;
        pending_offer = nullptr;
        pending_names.clear();
        return;
    }
    if (offer != nullptr && offer != in.offer && pending_offer == offer) {
        wl_data_offer_destroy(offer);
        pending_offer = nullptr;
        pending_names.clear();
    }
}

auto WaylandDragBridge::Impl::on_enter(std::uint32_t serial, wl_fixed_t x, wl_fixed_t y, wl_data_offer *offer) -> void {
    // 异常路径（leave 未达就来了新 enter）：先收摊上一个会话。
    reset_drag_in();
    in.offer = offer;
    if (pending_offer == offer) {
        in.offer_names = std::move(pending_names);
        pending_names.clear();
        pending_offer = nullptr;
    } else {
        in.offer_names.clear();
    }
    in.active = true;
    in.entered = false;
    in.dropped = false;
    in.enter_serial = serial;
    in.x = wl_fixed_to_double(x);
    in.y = wl_fixed_to_double(y);
    in.mimes = drag::mimes_from_wayland_offer_names(in.offer_names);
    in.allowed = DragAction::None;
    handled = false;

    // 目标侧声明（v3）：本端支持 Copy|Move，与源侧能力求交。源未报能力（v1/v2 没有
    // `source_actions` 事件，恒 0）时按「其支持全集」处理——不求交就发全集，会让「源只支持
    // Move」的场景协商出一个源兑现不了的动作。
    const std::uint32_t supported = drag::wayland_supported_dnd_actions();
    const std::uint32_t dest_actions = in.source_actions == 0 ? supported : (supported & in.source_actions);
    // 首选按 OS 建议（合成器在 `action` 事件里给；未给按 Copy）。Move 的源侧删除确认不在
    // 目标侧管辖内，故此处不做 `constrain_action`（那是拖出侧的纪律）。
    const DragAction preferred = in.os_action != DragAction::None ? in.os_action : DragAction::Copy;
    if (mgr_version >= 3U && in.offer != nullptr) {
        wl_data_offer_set_actions(in.offer, dest_actions, drag::wayland_dnd_action_from(preferred));
    }
    in.allowed = dispatch_drag_in(SystemDropPhase::Enter, nullptr);
    apply_accept(in.allowed);
    // Enter 已派发：置位供 `on_leave`（未落放也补 Leave）与 `on_drop`（不补第二次 Enter）
    // 判读。X11 桥同款惯例（首个 Position 才派发 Enter，故在那里置位）。
    in.entered = true;
}

auto WaylandDragBridge::Impl::on_leave() -> void {
    if (!in.entered) {
        reset_drag_in();  // 从未派发过 Enter：无需补 Leave（控件不知道有过拖入）
        return;
    }
    // 先派发再清态：`dispatch_drag_in` 要读会话态（mimes / 位置 / OS 建议）。
    // `Leave` 的载荷契约允许为空——不放置即结束，没有数据可交。
    const std::shared_ptr<const DragPayload> no_payload;
    (void)dispatch_drag_in(SystemDropPhase::Leave, no_payload);
    reset_drag_in();
}

auto WaylandDragBridge::Impl::on_motion(wl_fixed_t x, wl_fixed_t y) -> void {
    if (!in.active) {
        return;
    }
    in.x = wl_fixed_to_double(x);
    in.y = wl_fixed_to_double(y);
    // 与 X11 桥同口径：每个悬停采样都派发一次 Over（`SystemDropPhase::Over` 的契约即
    // 「按指针采样频率到达，处理须轻量」——轻重由控件负责，桥不替它节流）。
    in.allowed = dispatch_drag_in(SystemDropPhase::Over, nullptr);
    apply_accept(in.allowed);
}

auto WaylandDragBridge::Impl::on_drop() -> void {
    if (!in.active) {
        return;
    }
    // 源可能在 drop 前只发过 Enter（不发 Position）：补一次，控件无从得知有过拖入。
    if (!in.entered) {
        in.entered = true;
        in.allowed = dispatch_drag_in(SystemDropPhase::Enter, nullptr);
        apply_accept(in.allowed);
    }
    in.dropped = true;
    in.index = 0;
    in.buffer.clear();
    in.entries.clear();
    in.started = std::chrono::steady_clock::now();
    if (in.mimes.empty()) {
        // 无格式可读：照样收口并回 `finish`（源在等它）。空载荷不是失败。
        finish_drag_in_reads();
        return;
    }
    start_next_read();
}

auto WaylandDragBridge::Impl::apply_accept(DragAction allowed) -> void {
    if (in.offer == nullptr) {
        return;
    }
    // `accept` 的 mime 形参是「本次接受哪个线上名」：libwayland 在调用内即拷进消息缓冲，
    // 故用 offer_names 里那个 string 的 c_str() 即可（它由本桥持有，活过本次调用）。
    const std::string *chosen = nullptr;
    if (allowed != DragAction::None) {
        for (const std::string &name : in.offer_names) {
            if (drag::mime_from_wayland_offer_name(name).has_value()) {
                chosen = &name;
                break;  // 顺序即优先级：取首个已登记名
            }
        }
    }
    wl_data_offer_accept(in.offer, in.enter_serial, chosen != nullptr ? chosen->c_str() : nullptr);
}

auto WaylandDragBridge::Impl::dispatch_drag_in(SystemDropPhase phase, std::shared_ptr<const DragPayload> payload)
    -> DragAction {
    const float scale = hooks.scale_factor != nullptr ? hooks.scale_factor() : 1.0F;
    drag::DragInFrame frame;
    frame.mimes = in.mimes;
    frame.modifiers = hooks.modifiers != nullptr ? hooks.modifiers() : ModifierKey::None;
    frame.os_suggested = in.os_action;
    // 表面本地坐标（逻辑）→ 客户区物理像素：折算层 `logical_from_client_px` 是拖放坐标契约的
    // 唯一折算入口（Wayland 事件坐标天然是表面坐标，与 Win32/X11 的物理像素不同域，故在此
    // 先乘 scale 把量纲归一，再交给同一个折算点）。
    frame.client_x = static_cast<int>(in.x * static_cast<double>(scale));
    frame.client_y = static_cast<int>(in.y * static_cast<double>(scale));
    frame.scale = scale;
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

auto WaylandDragBridge::Impl::start_next_read() -> void {
    if (in.index >= in.mimes.size()) {
        finish_drag_in_reads();
        return;
    }
    const std::string &mime = in.mimes[in.index];
    const std::vector<std::string> names = drag::wayland_offer_names_from_mime(mime);
    // 只请求对端**真的广告过**的线上名（按顺序取首个命中）。
    const std::string *chosen = nullptr;
    for (const std::string &name : names) {
        const auto hit = std::find(in.offer_names.begin(), in.offer_names.end(), name);
        if (hit != in.offer_names.end()) {
            chosen = &name;
            break;
        }
    }
    if (chosen == nullptr) {
        ++in.index;
        start_next_read();
        return;
    }
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        AURORA_LOG_WARN("system-drag", "Wayland: pipe() failed for receive; treating mime as empty.");
        ++in.index;
        start_next_read();
        return;
    }
    if (in.offer != nullptr) {
        wl_data_offer_receive(in.offer, chosen->c_str(), fds[1]);
    }
    ::close(fds[1]);
    set_nonblocking(fds[0]);
    in.current_mime = mime;
    in.read_fd = fds[0];
    in.buffer.clear();
    wl_display_flush(dpy);
}

auto WaylandDragBridge::Impl::pump_read() -> void {
    if (in.read_fd < 0) {
        return;
    }
    std::array<std::uint8_t, AURORA_WL_PIPE_CHUNK> chunk{};
    for (;;) {
        const ssize_t got = ::read(in.read_fd, chunk.data(), chunk.size());
        if (got > 0) {
            in.buffer.insert(in.buffer.end(), chunk.begin(),
                             std::next(chunk.begin(), static_cast<std::ptrdiff_t>(got)));
            continue;
        }
        if (got == 0) {
            // EOF：对端写完并关了写端，本 MIME 收齐。
            ::close(in.read_fd);
            in.read_fd = -1;
            if (!in.current_mime.empty()) {
                in.entries.insert_or_assign(in.current_mime, std::move(in.buffer));
            }
            in.buffer.clear();
            in.current_mime.clear();
            ++in.index;
            start_next_read();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // 对端还没写完：等下一帧（不阻塞帧循环）
        }
        // 其余错误按「已收到的部分」收下：不谎报失败，也不把管道错误升级成会话失败。
        AURORA_LOG_WARN("system-drag", "Wayland: receive pipe read error; accepting partial bytes.");
        ::close(in.read_fd);
        in.read_fd = -1;
        if (!in.current_mime.empty()) {
            in.entries.insert_or_assign(in.current_mime, std::move(in.buffer));
        }
        in.buffer.clear();
        in.current_mime.clear();
        ++in.index;
        start_next_read();
        return;
    }
}

auto WaylandDragBridge::Impl::finish_drag_in_reads() -> void {
    if (!in.active) {
        return;
    }
    auto payload = DragPayload::from_bytes(std::move(in.entries));
    (void)dispatch_drag_in(SystemDropPhase::Drop, std::move(payload));
    // 目标侧不向源回传执行动作：源侧从自己的 `wl_data_source.action` 事件得知协商结果
    // （与 X11 的 `XdndFinished` 回写动作、Win32 的 `pdwEffect` 不同——Wayland 协议没有
    // 目标 → 源的动作回程）。受理与否只影响本端是否 `finish`。
    if (in.offer != nullptr) {
        // `finish` 只在 v3 存在；数据已收齐，此刻通知源「传输完成」。
        if (mgr_version >= 3U) {
            wl_data_offer_finish(in.offer);
        }
        wl_data_offer_destroy(in.offer);
        in.offer = nullptr;
    }
    reset_drag_in();
}

auto WaylandDragBridge::Impl::reset_drag_in() -> void {
    if (in.read_fd >= 0) {
        ::close(in.read_fd);
        in.read_fd = -1;
    }
    if (in.offer != nullptr) {
        wl_data_offer_destroy(in.offer);
        in.offer = nullptr;
    }
    in = DragIn{};
}

// ---- 拖出 ----

auto WaylandDragBridge::Impl::begin_drag_out(const DragOffer &offer) -> bool {
    if (out.active || mgr == nullptr || device == nullptr || seat == nullptr || surface == nullptr || dpy == nullptr) {
        return false;
    }
    // 协议要求 `start_drag` 的 serial 是本客户端某次 `wl_pointer.button` 按下的 serial；
    // 没有它（程序化合成发起）宁可不发起，也不拿一个编造的 serial 去换 protocol error。
    if (out.press_serial == 0U) {
        AURORA_LOG_WARN("system-drag", "Wayland: no button serial for start_drag; drag not started.");
        return false;
    }
    std::vector<std::string> mimes;
    for (const std::string &mime : offer.formats) {
        if (!drag::wayland_offer_names_from_mime(mime).empty() && offer.provide != nullptr) {
            mimes.push_back(mime);
        }
    }
    if (mimes.empty()) {
        return false;  // 无一格式可广告到 Wayland：如实不发起（不谎报会话已建立）
    }
    wl_data_source *source = wl_data_device_manager_create_data_source(mgr);
    if (source == nullptr) {
        AURORA_LOG_WARN("system-drag", "Wayland: create_data_source failed.");
        return false;
    }
    out.source = source;
    out.mimes = std::move(mimes);
    // 要约整份拷入：`provide` 失败会走会话收口，不得在回调后读已被清掉的成员
    // （与 X11 桥 `on_selection_request` 同款纪律）。
    out.offer = offer;
    out.sent_any = false;
    out.drop_performed = false;
    out.last_action = DragAction::None;
    out.target_accepts = false;
    out.target_mime.clear();
    out.send = DragOut::Send{};
    wl_data_source_add_listener(source, &AURORA_DATA_SOURCE_LISTENER, this);
    // 只广告本会话能提供的线上名（广告了给不出 = 广告了却违约）。
    for (const std::string &mime : out.mimes) {
        for (const std::string &name : drag::wayland_offer_names_from_mime(mime)) {
            wl_data_source_offer(source, name.c_str());
        }
    }
    if (mgr_version >= 3U) {
        wl_data_source_set_actions(source, drag::wayland_supported_dnd_actions());
    }
    // icon 传 null：跟随光标的影像交给合成器默认形态（`DragOffer::icon` 是装饰性提示，
    // 不支持影像的后端静默忽略、不改变会话语义）。
    wl_data_device_start_drag(device, source, surface, nullptr, out.press_serial);
    wl_display_flush(dpy);
    out.active = true;
    out.press_serial = 0;  // serial 一次性：用过的不能再用来起第二次
    return true;
}

auto WaylandDragBridge::Impl::cancel_drag_out() -> void {
    if (!out.active) {
        return;
    }
    teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
}

auto WaylandDragBridge::Impl::teardown_drag_out(SystemDragOutcome outcome, DragAction action) -> void {
    if (!out.active) {
        return;
    }
    if (out.send.fd >= 0) {
        ::close(out.send.fd);
        out.send = DragOut::Send{};
    }
    if (out.source != nullptr) {
        wl_data_source_destroy(out.source);
        out.source = nullptr;
    }
    out.active = false;
    // 终态登记的**唯一**通道（事件层在帧循环排空内投递 `on_end`，含 Move 确认次序）。
    complete_drag_out_session(outcome, action);
}

auto WaylandDragBridge::Impl::on_source_target(const char *mime_type) -> void {
    out.target_accepts = (mime_type != nullptr);
    out.target_mime = (mime_type != nullptr) ? std::string(mime_type) : std::string{};
}

auto WaylandDragBridge::Impl::on_source_send(const char *mime_type, int fd) -> void {
    if (fd < 0) {
        return;
    }
    if (!out.active) {
        ::close(fd);  // 会话已收摊还来 send：对端迟到，关掉即止
        return;
    }
    set_nonblocking(fd);
    if (mime_type == nullptr) {
        ::close(fd);  // 目标放弃了这次传输（协议允许 send 带 null mime）
        return;
    }
    const auto mime = drag::mime_from_wayland_offer_name(mime_type);
    if (!mime.has_value()) {
        ::close(fd);  // 未登记的线上名：不可能广告过，如实不给数据（不猜）
        return;
    }
    if (out.offer.provide == nullptr) {
        ::close(fd);
        teardown_drag_out(SystemDragOutcome::Failed, DragAction::None);
        return;
    }
    const Result<std::vector<std::uint8_t>> bytes = out.offer.provide(*mime);
    if (!bytes) {
        // 广告了却给不出：与 X11 / Win32 侧同罪（会话按失败结束），不把失败伪装成格式不存在。
        AURORA_LOG_WARN("system-drag", "Wayland: provide failed for mime; ending session as failed.");
        ::close(fd);
        teardown_drag_out(SystemDragOutcome::Failed, DragAction::None);
        return;
    }
    out.sent_any = true;
    out.send.fd = fd;
    out.send.data = bytes.value();
    out.send.offset = 0;
    pump_send();
}

auto WaylandDragBridge::Impl::pump_send() -> void {
    if (out.send.fd < 0) {
        return;
    }
    for (;;) {
        if (out.send.offset >= out.send.data.size()) {
            ::close(out.send.fd);
            out.send = DragOut::Send{};
            return;
        }
        const auto *base = std::next(out.send.data.data(), static_cast<std::ptrdiff_t>(out.send.offset));
        const auto left = static_cast<std::size_t>(out.send.data.size() - out.send.offset);
        const ssize_t wrote = ::write(out.send.fd, base, left);
        if (wrote > 0) {
            out.send.offset += static_cast<std::size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // 对端还没读：等下一帧（同步 write 会把 UI 线程钉死在对端节奏上）
        }
        // 对端提前关闭 / 管道破裂：剩余字节无从投递，收摊本次传输（不悬挂、不重试）。
        AURORA_LOG_WARN("system-drag", "Wayland: receive pipe write failed; dropping remaining bytes.");
        ::close(out.send.fd);
        out.send = DragOut::Send{};
        return;
    }
}

auto WaylandDragBridge::Impl::on_source_cancelled() -> void {
    if (!out.active) {
        return;
    }
    // v3：`cancelled` 一词一义 = 用户取消（Esc / 松手回到源窗）⇒ Cancelled。
    // v1/v2：该事件同时承担「会话结束」——按「是否应答过 send」分叉：
    //   * 应答过 ⇒ 落放发生过（目标确实要过数据），会话按成功折算；
    //   * 未应答 ⇒ 目标从未要数据，即被拒绝 ⇒ Rejected。
    if (mgr_version >= 3U) {
        if (out.drop_performed) {
            // 落放已发生却没等来 `dnd_finished`：目标没有 `finish`，即拒绝。
            teardown_drag_out(SystemDragOutcome::Rejected, DragAction::None);
            return;
        }
        teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None);
        return;
    }
    if (out.sent_any) {
        const DragAction action = out.last_action != DragAction::None
                                      ? out.last_action
                                      : constrain_action(out.offer, out.offer.default_action);
        teardown_drag_out(SystemDragOutcome::Succeeded, action);
        return;
    }
    teardown_drag_out(SystemDragOutcome::Rejected, DragAction::None);
}

auto WaylandDragBridge::Impl::on_source_drop_performed() -> void { out.drop_performed = true; }

auto WaylandDragBridge::Impl::on_source_finished() -> void {
    if (!out.active) {
        return;
    }
    const DragAction action =
        out.last_action != DragAction::None ? out.last_action : constrain_action(out.offer, out.offer.default_action);
    teardown_drag_out(SystemDragOutcome::Succeeded, action);
}

auto WaylandDragBridge::Impl::on_source_action(std::uint32_t dnd_action) -> void {
    out.last_action = drag::action_from_wayland_dnd_action(dnd_action);
}

// ---- 帧循环看守 ----

auto WaylandDragBridge::Impl::tick() -> void {
    pump_send();
    pump_read();
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::milliseconds timeout = system_drag_read_timeout();
    // 源迟迟不给数据 ⇒ 按已读到的部分派发 `Drop` 并回 `finish`（不悬挂、不谎报成功）。
    if (in.dropped && (now - in.started) > timeout) {
        finish_drag_in_reads();
    }
}

}  // namespace aurora::detail

// ---- 桥的公共面（全部转发给状态机） ----

namespace aurora::detail {

WaylandDragBridge::WaylandDragBridge(void *display, void *manager, std::uint32_t manager_version, void *seat,
                                     void *surface, Hooks hooks)
    : impl_(std::make_unique<Impl>()), display_(display), manager_(manager), manager_version_(manager_version),
      seat_(seat), surface_(surface), hooks_(std::move(hooks)) {
    impl_->dpy = static_cast<wl_display *>(display);
    impl_->mgr = static_cast<wl_data_device_manager *>(manager);
    impl_->mgr_version = manager_version;
    impl_->seat = static_cast<wl_seat *>(seat);
    impl_->surface = static_cast<wl_surface *>(surface);
    impl_->hooks = hooks_;
    impl_->owner = this;
}

WaylandDragBridge::~WaylandDragBridge() { uninstall(); }

auto WaylandDragBridge::install() -> bool { return impl_->install(); }

auto WaylandDragBridge::uninstall() -> void { impl_->uninstall(); }

auto WaylandDragBridge::note_press_serial(std::uint32_t serial) -> void { impl_->out.press_serial = serial; }

auto WaylandDragBridge::tick() -> void { impl_->tick(); }

auto WaylandDragBridge::drag_out_active() const -> bool { return impl_->out.active; }

auto WaylandDragBridge::drag_in_active() const -> bool { return impl_->in.active; }

auto WaylandDragBridge::data_device() const -> void * { return impl_->device; }

auto WaylandDragBridge::note_initiator(WaylandDragBridge *bridge) -> void { initiator_slot() = bridge; }

auto WaylandDragBridge::set_clipboard_forwarder(
    std::function<void(wl_data_offer *, const std::vector<std::string> &)> forwarder) -> void {
    clipboard_forwarder_slot() = std::move(forwarder);
}

auto WaylandDragBridge::initiator() -> WaylandDragBridge * { return initiator_slot(); }

auto WaylandDragBridge::begin_drag_out(const DragOffer &offer) -> bool { return impl_->begin_drag_out(offer); }

auto WaylandDragBridge::cancel_drag_out() -> void { impl_->cancel_drag_out(); }

auto WaylandDragBridge::register_backend() -> void {
    if (backend_registered()) {
        return;
    }
    backend_registered() = true;
    register_system_drag_out_backend(SystemDragOutBackend{
        .begin = [](const DragOffer &offer) -> bool {
            WaylandDragBridge *self = initiator();
            return self != nullptr && self->begin_drag_out(offer);
        },
        .cancel = []() -> void {
            WaylandDragBridge *self = initiator();
            if (self != nullptr) {
                self->cancel_drag_out();
            }
        },
    });
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_WAYLAND
