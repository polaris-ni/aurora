/* Wayland 系统拖放桥 —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖接缝：
//   拖入（本窗口是 target）：`wl_data_device` 的 `data_offer`/`enter`/`motion`/`drop` +
//   `wl_data_offer` 的 `offer`/`source_actions`/`action` → `detail::WaylandDragBridge` →
//   折算层（`detail/system_drag_translate`）→ 拖入入口（`event/detail/system_drag_ingress`）
//   → 真派发链（`EventDispatcher` + 探针自带的落放控件）；落放后经**管道**（`wl_data_offer.receive`
//   给写端、本端非阻塞逐帧读）取数落进 `DragPayload`，收口时 `finish` + `destroy`。目标侧应答
//   `accept(serial, mime)` 与 `set_actions(dest_actions, preferred)`。
//   拖出（本窗口是 source）：`system_drag::begin` → 拖出后端（平台桥经
//   `detail::register_system_drag_out_backend` 登记）→ `wl_data_source.offer` + `set_actions` +
//   `wl_data_device.start_drag(source, origin_surface, null, serial)`；`send` 到达时调
//   `DragOffer::provide` 并把字节非阻塞写回对端 fd；终态经 `complete_drag_out_session`
//   登记、帧循环 `pump_reads()` 投递 `on_system_drag_end`。
//
// 为什么自动段跑在**进程内 mock 合成器**上（而不是连真实合成器）：
//   Wayland 的拖放会话由**合成器**按「指针下的表面」派发，而 Wayland 没有客户端可用的指针
//   传送 API——「把指针泊进本窗口」这件事在无头与无人值守环境下都做不到（X11 侧同一约束的
//   表现是 Xwayland 下 `XQueryPointer` 看不到真实窗口栈，见 x11_drag_live_probe.cpp）。
//   于是把合成器**搬进本进程**：服务端自建 `wl_display` 并实现 `wl_compositor` /
//   `wl_seat` / `wl_data_device_manager` 三段（数据设备 / 数据源 / 数据要约各自的服务端面），
//   客户端经 socketpair 连上（registry 绑定拿到与桥同侧的真实代理对象）。这样「外部源发起
//   拖入」「目标索取数据」「源应答取数」全部可确定性驱动，且走的是**真实的 wl_data_device
//   协议报文**——桥的状态机、折算层、管道读写与终态折算都被真实协议代码执行，只是对端从
//   GNOME 换成了本进程的 mock。代价：mock 不覆盖「真实合成器上装得上、不断链」，那由本探针
//   的可选冒烟腿（有 `WAYLAND_DISPLAY` 时跑）与人工交互腿补。
//
// 自动段（无需人工拖文件）逐项验收：
//   ① 拖入四态 + 载荷往返：mock 源广告 `text/plain;charset=utf-8` 与 `text/uri-list` →
//      `enter`（表面本地坐标）→ `source_actions` → `action(Move)` → `motion` → `drop`；
//      断言 Enter 恰一次、formats 折算为两个规范 MIME 且保序、accept 查询发生、Enter 位置
//      正确、`action` 事件报的动作进入 `proposed` 且与目标接受动作求交出 `allowed`、`accept`
//      以首个已登记线上名调用、Drop 载荷文本与 mock 写入的字节一致、Drop 位置取最后一次
//      motion、两个 MIME 都经管道取回、`finish` 被调用、会话态收清。
//   ② 拖出成功终态：`begin` → 源广告两个线上名 + `set_actions(Copy|Move)` → mock 发
//      `target` / `action(Copy)` / `dnd_drop_performed` / `send` → 断言 provide 被以规范
//      MIME 调用（线上名参数被剥）、字节经管道原样到达 mock、`dnd_finished` 后终态
//      `Succeeded` + `Copy`、会话态收清。
//   ③ 拖出取消终态：`system_drag::cancel()` → 终态 `Cancelled` + `None`（不冒充失败）。
//   ④ 冒烟腿（可选）：有 `WAYLAND_DISPLAY` 时核对能力位并建一个真实 `WaylandSurface` 泵
//      若干帧，证明桥在真实合成器上装得上、不断链（无则记 PENDING，不算失败）。
//
// 人工段（`--interactive`，需真实合成器）：窗口常驻，逐帧打印拖入四态与拖出终态。
//   拖入：从文件管理器拖文件进上面板，肉眼核对四态与路径往返；拖出：从下面板按住左键拖到
//   本窗外松开，期望 begin → end(Succeeded, Copy)；拖动中按 Esc，期望 end(Cancelled, None)。
//
// 构建（须已开启 Wayland 后端与探针开关，且系统装有 wayland-server 开发包）：
//   cmake -S . -B build-verify -G Ninja -DAURORA_BACKEND_WAYLAND=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_wayland_drag
//   ./build-verify/aurora_verify_wayland_drag [--interactive]
//
// 退出码：
//   0  自动段全部通过（mock 三腿 + 若在场则真实合成器冒烟腿）
//   2  环境不可用（mock 服务端起不来 / socketpair 失败 / --interactive 无 WAYLAND_DISPLAY）
//   3  自动段无失败，但真实合成器缺席 ⇒ 真实拖放交互腿未验收
//   4  部分验收项不符 —— 见逐行 PASS/FAIL
// ============================================================================ */

#include "aurora/aurora.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
#error "aurora_verify_wayland_drag can only be built on desktop Linux (AURORA_PLATFORM_LINUX)"
#endif

#ifndef AURORA_BACKEND_WAYLAND
#error "AURORA_BACKEND_WAYLAND must be enabled"
#endif

// wayland 头排在全部 aurora 头之后（与 `wayland_drag.cpp` 同款纪律：平台协议头关在单个 TU 内）。
// 整段包含用 `clang-format off` 圈住：格式化器按「系统头优先」重排包含时会把 wayland 头提到
// aurora 头之前。顺序是本文件的语义，不由排版器决定。
// 注意：C++ 里 `wl_*_interface` 必须写 elaborated 形式（`struct wl_*_interface`）——协议头同时
// 声明了同名变量 `extern const struct wl_interface wl_*_interface;`，未加 `struct` 的名字查找
// 命中变量而非类型。
// clang-format off
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/window/detail/system_drag_translate.h"
#include "aurora/window/detail/wayland_drag.h"
#include "aurora/window/platform.h"
#include "aurora/window/wayland_surface.h"

#include "verify_args.h"
// clang-format on

namespace {

// 库类型与折算层都在 `aurora` 命名空间；本匿名 namespace 在全局作用域，逐限定会把探针逻辑
// 埋进 `aurora::` 前缀里。引入后 `drag::` / `system_drag::` 即 `aurora::drag` / `aurora::system_drag`。
using namespace aurora;

// ---------------------------------------------------------------------------
// 判定账本与输出
// ---------------------------------------------------------------------------

/// @brief 判定账本：计数与失败数由调用方显式传递，不留命名空间级可变状态。
struct Ledger {
    int checks = 0;
    int failures = 0;
};

auto note(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

auto check(bool ok, const std::string &name, Ledger &ledger, const std::string &detail = {}) -> void {
    ++ledger.checks;
    note(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + name + (detail.empty() ? std::string{} : "  -- " + detail));
    if (!ok) {
        ++ledger.failures;
    }
}

/// @brief 终态结局 → 可读名（探针输出用；库侧无字符串化助手）。
[[nodiscard]] auto outcome_name(SystemDragOutcome outcome) -> const char * {
    switch (outcome) {
        case SystemDragOutcome::Succeeded:
            return "Succeeded";
        case SystemDragOutcome::Cancelled:
            return "Cancelled";
        case SystemDragOutcome::Rejected:
            return "Rejected";
        case SystemDragOutcome::Failed:
            return "Failed";
    }
    return "?";
}

/// @brief 拖放动作 → 可读名（探针输出用；库侧无字符串化助手）。
[[nodiscard]] auto action_name(DragAction action) -> const char * {
    switch (action) {
        case DragAction::None:
            return "None";
        case DragAction::Copy:
            return "Copy";
        case DragAction::Move:
            return "Move";
        case DragAction::Link:
            return "Link";
    }
    return "?";
}

/// @brief 阶段 → 可读名（探针输出用）。
[[nodiscard]] auto phase_name(SystemDropPhase phase) -> const char * {
    switch (phase) {
        case SystemDropPhase::Enter:
            return "Enter";
        case SystemDropPhase::Over:
            return "Over";
        case SystemDropPhase::Leave:
            return "Leave";
        case SystemDropPhase::Drop:
            return "Drop";
    }
    return "?";
}

/// @brief 探针载荷文本（拖入往返 / 拖出 provide 同一份；长度按编译期常量算，不依赖 strlen）。
constexpr char AURORA_PROBE_TEXT[] = "aurora-wayland-drag-probe";
constexpr std::size_t AURORA_PROBE_TEXT_LEN = sizeof(AURORA_PROBE_TEXT) - 1U;

// ---------------------------------------------------------------------------
// 落放面板（拖入自动段的派发链终点；人工段同时是真窗口里的落点控件）
// ---------------------------------------------------------------------------

/// @brief 落放面板：记录拖入四态与最近一次的格式 / 位置 / 文本 / allowed；accept 查询可调。
class DropPanel final : public LeafWidget {
  public:
    std::vector<std::string> phase_log;  ///< 收到的阶段名（按序）
    std::vector<std::string> last_formats;  ///< 最近一次事件的格式清单
    Point last_position{};  ///< 最近一次事件的落点（表面逻辑坐标）
    std::vector<std::string> last_files;  ///< 最近一次落放的文件路径（人工段打印用）
    std::string last_text;  ///< 最近一次落放的纯文本（text/plain 解码；管道往返判据）
    bool has_text = false;  ///< 落放是否带回了文本
    DragAction last_allowed = DragAction::None;  ///< 派发器写定的允许动作
    DragAction last_proposed = DragAction::None;  ///< 最近一次事件的源建议动作
    DragAction accept = DragAction::Copy;  ///< `query_system_drag_accept` 的返回值
    int accept_queries = 0;  ///< accept 查询次数

    auto type_name() const -> const char * override { return "DropPanel"; }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        size_ = c.constrain(Size{.width = 480.0F, .height = 200.0F});
        return size_;
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

    auto on_system_drop(SystemDropEvent &e) -> void override {
        phase_log.emplace_back(phase_name(e.phase));
        last_formats = e.formats;
        last_position = e.position;
        last_proposed = e.proposed;
        last_allowed = e.allowed;
        if (e.phase == SystemDropPhase::Drop && e.payload != nullptr) {
            const Result<std::vector<std::string>> files = e.files();
            if (files) {
                last_files = files.value();
            }
            const Result<std::string> text = e.text();
            if (text) {
                last_text = text.value();
                has_text = true;
            }
        }
        e.is_handled = true;  // 消费即受理：目标侧据此决定是否 `finish`
    }

    [[nodiscard]] auto query_system_drag_accept(const std::vector<std::string> & /*formats*/) -> DragAction override {
        ++accept_queries;
        return accept;
    }
};

// ---------------------------------------------------------------------------
// 进程内 mock 合成器（服务端）
// ---------------------------------------------------------------------------

/// @brief mock 侧的数据源对象（客户端经 `create_data_source` 建的）。
struct MockSource {
    wl_resource *res = nullptr;
    std::vector<std::string> offers;  ///< 客户端广告的线上名
    std::uint32_t actions = 0;  ///< 客户端 `set_actions` 的能力位
};

/// @brief mock 侧的数据要约对象（服务端经 `data_offer` 事件推给目标客户端的）。
struct MockOffer {
    wl_resource *res = nullptr;
    std::vector<std::string> mimes;  ///< 服务端广告的线上名
    std::uint32_t source_actions = 0;  ///< 源侧能力位（`source_actions` 事件）
    std::uint32_t preferred = 0;  ///< 目标 `set_actions` 的首选位
    std::string accepted;  ///< 目标 `accept` 的线上名（空 = 未接受 / 显式拒绝）
    bool finish_called = false;  ///< 目标是否 `finish`
    std::vector<std::string> received;  ///< 目标 `receive` 请求的线上名（按序）
};

/// @brief 进程内 mock 合成器：服务端三段 globals + 客户端侧与桥同侧的真实代理。
///
/// 两端都持有：服务端 `wl_resource`（mock 发事件、收请求用）与客户端 `wl_*` 代理
/// （桥的协议调用用）。桥经客户端代理调 `wl_data_device_manager_get_data_device` 等，
/// mock 经服务端资源发 `enter` / `send` 等——同一条真实协议报文的两侧。
struct MockCompositor {
    // ---- 服务端 ----
    wl_display *display = nullptr;
    wl_event_loop *loop = nullptr;
    wl_client *client = nullptr;
    int peer_fd = -1;  ///< 服务端侧 fd（交 `wl_client_create`）
    int client_fd = -1;  ///< 客户端侧 fd（交 `wl_display_connect_to_fd`）

    wl_resource *srv_compositor = nullptr;
    wl_resource *srv_seat = nullptr;
    wl_resource *srv_ddm = nullptr;
    wl_resource *srv_surface = nullptr;  ///< 客户端 surface 的服务端资源（`enter` 的 origin）
    wl_resource *srv_device = nullptr;  ///< 桥经 `get_data_device` 建的那个

    std::vector<MockSource> sources;
    std::vector<MockOffer> offers;
    std::uint32_t next_serial = 1000;

    // `start_drag` 记录（拖出腿）
    wl_resource *drag_source = nullptr;
    std::uint32_t drag_serial = 0;
    bool drag_started = false;

    // `receive` 时服务端写进管道的载荷（拖入腿）
    std::string receive_payload;

    // ---- 客户端（探针与桥同侧的代理）----
    wl_display *cli_display = nullptr;
    wl_compositor *cli_compositor = nullptr;
    wl_seat *cli_seat = nullptr;
    wl_data_device_manager *cli_ddm = nullptr;
    wl_surface *cli_surface = nullptr;
};

// ---- 请求实现（no-op 的那些只为「别因未实现请求而被服务端杀客户端」）----

void surface_noop_0(wl_client * /*c*/, wl_resource * /*r*/) {}
void surface_noop_1(wl_client * /*c*/, wl_resource * /*r*/, std::uint32_t /*a*/) {}
void surface_noop_1i(wl_client * /*c*/, wl_resource * /*r*/, std::int32_t /*a*/) {}
void surface_noop_res(wl_client * /*c*/, wl_resource * /*r*/, wl_resource * /*o*/) {}
void surface_noop_4i(wl_client * /*c*/, wl_resource * /*r*/, std::int32_t /*a*/, std::int32_t /*b*/, std::int32_t /*c*/,
                     std::int32_t /*d*/) {}
void surface_noop_attach(wl_client * /*c*/, wl_resource * /*r*/, wl_resource * /*buf*/, std::int32_t /*x*/,
                         std::int32_t /*y*/) {}

// `struct` elaborated：协议头同名变量 `extern const struct wl_interface wl_*_interface;`
// 会遮蔽未加 `struct` 的类型名查找（见文件头注记）。
// 成员签名逐条对齐 `wl_surface_interface`（1.26 协议头；本 mock 只绑 v4，故只覆前 10 项）：
// `damage` 与 `damage_buffer` 都是四 int（x/y/width/height）、`set_buffer_transform` /
// `set_buffer_scale` 各一 int、`frame` 一 uint。
constexpr struct wl_surface_interface AURORA_MOCK_SURFACE_IMPL = {
    .destroy = surface_noop_0,
    .attach = surface_noop_attach,
    .damage = surface_noop_4i,
    .frame = surface_noop_1,
    .set_opaque_region = surface_noop_res,
    .set_input_region = surface_noop_res,
    .commit = surface_noop_0,
    .set_buffer_transform = surface_noop_1i,
    .set_buffer_scale = surface_noop_1i,
    .damage_buffer = surface_noop_4i,
};

void seat_noop_ptr(wl_client * /*c*/, wl_resource * /*r*/, std::uint32_t /*id*/) {}
void seat_noop_kbd(wl_client * /*c*/, wl_resource * /*r*/, std::uint32_t /*id*/) {}
void seat_noop_touch(wl_client * /*c*/, wl_resource * /*r*/, std::uint32_t /*id*/) {}

constexpr struct wl_seat_interface AURORA_MOCK_SEAT_IMPL = {
    .get_pointer = seat_noop_ptr,
    .get_keyboard = seat_noop_kbd,
    .get_touch = seat_noop_touch,
    .release = surface_noop_0,
};

// ---- wl_data_source（拖出腿：客户端是源）----

void source_offer(wl_client * /*client*/, wl_resource *res, const char *mime_type) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockSource &src : m->sources) {
        if (src.res == res) {
            if (mime_type != nullptr) {
                src.offers.emplace_back(mime_type);
            }
            return;
        }
    }
}

void source_destroy(wl_client * /*client*/, wl_resource *res) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockSource &src : m->sources) {
        if (src.res == res) {
            src.res = nullptr;
        }
    }
    wl_resource_destroy(res);
}

void source_set_actions(wl_client * /*client*/, wl_resource *res, std::uint32_t actions) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockSource &src : m->sources) {
        if (src.res == res) {
            src.actions = actions;
            return;
        }
    }
}

constexpr struct wl_data_source_interface AURORA_MOCK_SOURCE_IMPL = {
    .offer = source_offer,
    .destroy = source_destroy,
    .set_actions = source_set_actions,
};

// ---- wl_data_device ----

void device_start_drag(wl_client * /*client*/, wl_resource *res, wl_resource *source, wl_resource * /*origin*/,
                       wl_resource * /*icon*/, std::uint32_t serial) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    m->drag_started = true;
    m->drag_source = source;
    m->drag_serial = serial;
}

void device_set_selection(wl_client * /*client*/, wl_resource * /*res*/, wl_resource * /*source*/,
                          std::uint32_t /*serial*/) {}

constexpr struct wl_data_device_interface AURORA_MOCK_DEVICE_IMPL = {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = surface_noop_0,
};

// ---- wl_data_offer（拖入腿：服务端推给目标）----

void offer_accept(wl_client * /*client*/, wl_resource *res, std::uint32_t /*serial*/, const char *mime_type) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockOffer &o : m->offers) {
        if (o.res == res) {
            // mime 为 null = 显式拒绝；协议语义即「空名表示不接受任何格式」。
            o.accepted = (mime_type != nullptr) ? std::string(mime_type) : std::string{};
            return;
        }
    }
}

void offer_receive(wl_client * /*client*/, wl_resource *res, const char *mime_type, std::int32_t fd) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockOffer &o : m->offers) {
        if (o.res != res) {
            continue;
        }
        if (mime_type != nullptr) {
            o.received.emplace_back(mime_type);
        }
        // 服务端把载荷写进对端给的管道写端，然后关掉它——目标侧读到 EOF 即收齐。
        const std::string &payload = m->receive_payload;
        std::size_t written = 0;
        while (written < payload.size()) {
            const ssize_t n =
                ::write(fd, std::next(payload.data(), static_cast<std::ptrdiff_t>(written)), payload.size() - written);
            if (n <= 0) {
                break;
            }
            written += static_cast<std::size_t>(n);
        }
        ::close(fd);
        return;
    }
    ::close(fd);  // 未知要约：照样关 fd（不泄漏）
}

void offer_destroy(wl_client * /*client*/, wl_resource *res) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockOffer &o : m->offers) {
        if (o.res == res) {
            o.res = nullptr;
        }
    }
    wl_resource_destroy(res);
}

void offer_finish(wl_client * /*client*/, wl_resource *res) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockOffer &o : m->offers) {
        if (o.res == res) {
            o.finish_called = true;
            return;
        }
    }
}

void offer_set_actions(wl_client * /*client*/, wl_resource *res, std::uint32_t dest_actions, std::uint32_t preferred) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(res));
    for (MockOffer &o : m->offers) {
        if (o.res == res) {
            // 记进 source_actions 槽位复用：目标侧声明写在另一个字段更清晰，但 mock 只需
            // 「收到了即可」，故直接覆写专用槽。
            o.source_actions = dest_actions;
            o.preferred = preferred;
            return;
        }
    }
}

constexpr struct wl_data_offer_interface AURORA_MOCK_OFFER_IMPL = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

// ---- wl_compositor ----

void compositor_create_surface(wl_client *client, wl_resource *compositor, std::uint32_t id) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(compositor));
    wl_resource *res = wl_resource_create(client, &wl_surface_interface, 4, static_cast<int>(id));
    if (res == nullptr) {
        return;
    }
    wl_resource_set_implementation(res, &AURORA_MOCK_SURFACE_IMPL, m, nullptr);
    m->srv_surface = res;
}

void compositor_create_region(wl_client * /*client*/, wl_resource * /*compositor*/, std::uint32_t /*id*/) {}

constexpr struct wl_compositor_interface AURORA_MOCK_COMPOSITOR_IMPL = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

// ---- wl_data_device_manager ----

void ddm_create_data_source(wl_client *client, wl_resource *ddm, std::uint32_t id) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(ddm));
    MockSource src;
    src.res = wl_resource_create(client, &wl_data_source_interface, 3, static_cast<int>(id));
    if (src.res == nullptr) {
        return;
    }
    wl_resource_set_implementation(src.res, &AURORA_MOCK_SOURCE_IMPL, m, nullptr);
    m->sources.push_back(src);
}

void ddm_get_data_device(wl_client *client, wl_resource *ddm, std::uint32_t id, wl_resource * /*seat*/) {
    auto *m = static_cast<MockCompositor *>(wl_resource_get_user_data(ddm));
    wl_resource *res = wl_resource_create(client, &wl_data_device_interface, 3, static_cast<int>(id));
    if (res == nullptr) {
        return;
    }
    wl_resource_set_implementation(res, &AURORA_MOCK_DEVICE_IMPL, m, nullptr);
    m->srv_device = res;
}

constexpr struct wl_data_device_manager_interface AURORA_MOCK_DDM_IMPL = {
    .create_data_source = ddm_create_data_source,
    .get_data_device = ddm_get_data_device,
// release 请求随 wayland-client 1.25 进入协议头（manager 界面版本 4）。旧版头没有该成员，
// 指定初始化写上去即编译错误（CI 的 ubuntu-26.04 镜像装的是 1.24，实测报错后补此守卫）。
// 语义上无损失：本 mock 把 ddm 限在 v3（globals 广告版本 3，客户端 min(v,3) 绑定），
// release 是 v4 请求、永不到达；新版头下留零值 handler 同样安全。
#ifdef WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION
    .release = surface_noop_0,
#endif
};

// ---- globals 绑定（服务端侧：客户端 bind 时落服务端资源）----

void bind_compositor(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
    auto *m = static_cast<MockCompositor *>(data);
    wl_resource *res =
        wl_resource_create(client, &wl_compositor_interface, static_cast<int>(version), static_cast<int>(id));
    if (res == nullptr) {
        return;
    }
    wl_resource_set_implementation(res, &AURORA_MOCK_COMPOSITOR_IMPL, data, nullptr);
    m->srv_compositor = res;
}

void bind_seat(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
    auto *m = static_cast<MockCompositor *>(data);
    wl_resource *res = wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), static_cast<int>(id));
    if (res == nullptr) {
        return;
    }
    wl_resource_set_implementation(res, &AURORA_MOCK_SEAT_IMPL, data, nullptr);
    m->srv_seat = res;
}

void bind_ddm(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
    auto *m = static_cast<MockCompositor *>(data);
    wl_resource *res =
        wl_resource_create(client, &wl_data_device_manager_interface, static_cast<int>(version), static_cast<int>(id));
    if (res == nullptr) {
        return;
    }
    wl_resource_set_implementation(res, &AURORA_MOCK_DDM_IMPL, data, nullptr);
    m->srv_ddm = res;
}

// ---- 客户端 registry（探针侧：bind globals 拿与桥同侧的代理）----

void client_registry_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                            std::uint32_t version) {
    auto *m = static_cast<MockCompositor *>(data);
    const std::string iface{interface != nullptr ? interface : ""};
    if (iface == "wl_compositor" && m->cli_compositor == nullptr) {
        m->cli_compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, static_cast<int>(std::min(version, 4U))));
    } else if (iface == "wl_seat" && m->cli_seat == nullptr) {
        m->cli_seat = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, static_cast<int>(std::min(version, 5U))));
    } else if (iface == "wl_data_device_manager" && m->cli_ddm == nullptr) {
        m->cli_ddm = static_cast<wl_data_device_manager *>(wl_registry_bind(
            registry, name, &wl_data_device_manager_interface, static_cast<int>(std::min(version, 3U))));
    }
}

void client_registry_global_remove(void * /*data*/, wl_registry * /*registry*/, std::uint32_t /*name*/) {}

constexpr struct wl_registry_listener AURORA_CLIENT_REGISTRY_LISTENER = {
    .global = client_registry_global,
    .global_remove = client_registry_global_remove,
};

// ---- 双向泵：客户端请求 → 服务端处理 → 服务端事件 → 客户端处理 ----

/// @brief 客户端侧非阻塞读 + 派发（两端同线程驱动，不能阻塞在 read 上）。
///
/// `wl_display_prepare_read` 返回 -1 表示事件队列非空，此时不该读、直接派发排队事件。
auto pump_client(MockCompositor &m) -> int {
    if (m.cli_display == nullptr) {
        return -1;
    }
    if (wl_display_flush(m.cli_display) < 0) {
        return -1;
    }
    if (wl_display_prepare_read(m.cli_display) != 0) {
        return wl_display_dispatch_pending(m.cli_display);
    }
    struct pollfd pfd{};
    pfd.fd = m.client_fd;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, 0);
    if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        if (wl_display_read_events(m.cli_display) < 0) {
            return -1;
        }
    } else {
        wl_display_cancel_read(m.cli_display);
    }
    return wl_display_dispatch_pending(m.cli_display);
}

/// @brief 双方各泵若干轮：flush 客户端 → 服务端派发 → flush 服务端事件 → 客户端读派发。
auto pump_both(MockCompositor &m) -> int {
    int last = 0;
    for (int i = 0; i < 8; ++i) {
        if (m.cli_display == nullptr) {
            return -1;
        }
        if (wl_display_flush(m.cli_display) < 0) {
            return -1;
        }
        wl_event_loop_dispatch(m.loop, 0);
        wl_display_flush_clients(m.display);
        last = pump_client(m);
        if (last < 0) {
            return -1;
        }
    }
    return last;
}

/// @brief sync roundtrip（不用 `wl_display_roundtrip`：它阻塞在读上，而服务端在同一线程）。
struct SyncState {
    bool done = false;
};

void sync_callback_done(void *data, wl_callback *cb, [[maybe_unused]] std::uint32_t time_ms) {
    static_cast<SyncState *>(data)->done = true;
    wl_callback_destroy(cb);
}

constexpr struct wl_callback_listener AURORA_SYNC_LISTENER = {.done = sync_callback_done};

auto client_roundtrip(MockCompositor &m) -> bool {
    SyncState state;
    wl_callback *cb = wl_display_sync(m.cli_display);
    if (cb == nullptr) {
        return false;
    }
    wl_callback_add_listener(cb, &AURORA_SYNC_LISTENER, &state);
    for (int i = 0; i < 128 && !state.done; ++i) {
        if (pump_both(m) < 0) {
            return false;
        }
    }
    return state.done;
}

/// @brief 起 mock 合成器并把客户端（探针与桥同侧）绑上去。
auto mock_boot(MockCompositor &m) -> bool {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        return false;
    }
    m.peer_fd = fds[0];
    m.client_fd = fds[1];
    m.display = wl_display_create();
    if (m.display == nullptr) {
        return false;
    }
    m.loop = wl_display_get_event_loop(m.display);
    wl_global_create(m.display, &wl_compositor_interface, 4, &m, bind_compositor);
    wl_global_create(m.display, &wl_seat_interface, 5, &m, bind_seat);
    wl_global_create(m.display, &wl_data_device_manager_interface, 3, &m, bind_ddm);
    m.client = wl_client_create(m.display, m.peer_fd);
    if (m.client == nullptr) {
        return false;
    }
    m.cli_display = wl_display_connect_to_fd(m.client_fd);
    if (m.cli_display == nullptr) {
        return false;
    }
    // 客户端绑定 globals（registry 往返）：服务端资源与客户端代理成对建立。
    wl_registry *registry = wl_display_get_registry(m.cli_display);
    if (registry == nullptr) {
        return false;
    }
    wl_registry_add_listener(registry, &AURORA_CLIENT_REGISTRY_LISTENER, &m);
    if (!client_roundtrip(m)) {
        return false;
    }
    if (m.cli_compositor == nullptr || m.cli_seat == nullptr || m.cli_ddm == nullptr || m.srv_compositor == nullptr ||
        m.srv_seat == nullptr || m.srv_ddm == nullptr) {
        return false;  // globals 没到 / bind 没被服务端处理
    }
    // 客户端 surface：拖出 `start_drag` 的 origin surface（服务端资源由 create_surface 落）。
    m.cli_surface = wl_compositor_create_surface(m.cli_compositor);
    if (m.cli_surface == nullptr) {
        return false;
    }
    return client_roundtrip(m) && m.srv_surface != nullptr;
}

auto mock_shutdown(MockCompositor &m) -> void {
    if (m.cli_display != nullptr) {
        wl_display_disconnect(m.cli_display);  // 连带 flush 未发出的请求（destroy 等）
        m.cli_display = nullptr;
    }
    if (m.display != nullptr) {
        wl_display_destroy(m.display);  // 连带销毁 client 与全部资源
        m.display = nullptr;
    }
    m = MockCompositor{};
}

/// @brief 把 mock 推出的 offer 推给客户端的数据设备（`data_offer` + `offer` 事件）。
///
/// `source_actions` / `action` **不在此发**：真实协议序是 `enter` 之后才报能力与首选动作，
/// 而桥的 `on_enter` 先 `reset_drag_in()` 再收状态，早到的事件会被清掉。由调用方按序补发。
auto mock_push_offer(MockCompositor &m, const std::vector<std::string> &mimes) -> MockOffer * {
    if (m.srv_device == nullptr) {
        return nullptr;
    }
    m.offers.reserve(m.offers.size() + 1);  // 指针稳定性：腿内持 MockOffer* 跨后续 push
    MockOffer offer;
    // id 传 0：由 libwayland 从**服务端段**（0xff000000+）自动分配。服务端自建对象用客户端段
    // id 会与客户端自己的代理分配撞号（协议规定两端 id 分域）。
    offer.res = wl_resource_create(wl_resource_get_client(m.srv_device), &wl_data_offer_interface, 3, 0);
    if (offer.res == nullptr) {
        return nullptr;
    }
    offer.mimes = mimes;
    wl_resource_set_implementation(offer.res, &AURORA_MOCK_OFFER_IMPL, &m, nullptr);
    m.offers.push_back(std::move(offer));
    MockOffer *stored = &m.offers.back();
    wl_data_device_send_data_offer(m.srv_device, stored->res);
    for (const std::string &mime : stored->mimes) {
        wl_data_offer_send_offer(stored->res, mime.c_str());
    }
    return stored;
}

// ---------------------------------------------------------------------------
// 自动段：mock 合成器上的三腿 + 可选冒烟腿
// ---------------------------------------------------------------------------

/// @brief 拖入腿：mock 源发起一次完整拖入会话，断言四态 / 折算 / 管道载荷 / 回写。
auto leg_drag_in(MockCompositor &m, detail::WaylandDragBridge &bridge, DropPanel &panel, Ledger &ledger) -> bool {
    note("leg 1/4: drag-in via the in-process mock compositor (this window is the target)");
    const float enter_x = 12.0F;
    const float enter_y = 24.0F;
    const float motion_x = enter_x + 5.0F;
    const float motion_y = enter_y + 3.0F;
    m.receive_payload = AURORA_PROBE_TEXT;
    // 目标接受 Move：与合成器随后在 `action` 事件里报的 Move 一致（求交才不为 None）。
    panel.accept = DragAction::Move;
    panel.phase_log.clear();
    const int accept_queries_before = panel.accept_queries;

    MockOffer *offer = mock_push_offer(m, {"text/plain;charset=utf-8", "text/uri-list"});
    if (offer == nullptr) {
        check(false, "mock compositor pushes a data offer to the bridge", ledger);
        return false;
    }
    // 真实协议序：`data_offer` + `offer` 事件（mock_push_offer）先于 `enter`；`source_actions`
    // 与 `action` 在 `enter` 之后（合成器侧先 enter 再报源侧能力与首选动作）。
    wl_data_device_send_enter(m.srv_device, ++m.next_serial, m.srv_surface,
                              wl_fixed_from_int(static_cast<int>(enter_x)),
                              wl_fixed_from_int(static_cast<int>(enter_y)), offer->res);
    pump_both(m);

    const auto enters = static_cast<int>(std::count(panel.phase_log.begin(), panel.phase_log.end(), "Enter"));
    check(enters == 1, "drag-in dispatches exactly one Enter", ledger, "count=" + std::to_string(enters));
    check(panel.last_formats.size() == 2 && panel.last_formats[0] == AURORA_MIME_TEXT_PLAIN &&
              panel.last_formats[1] == AURORA_MIME_URI_LIST,
          "Enter formats fold to canonical MIMEs in priority order", ledger,
          "n=" + std::to_string(panel.last_formats.size()));
    check(panel.accept_queries > accept_queries_before, "accept query happens on Enter (negotiation has a source)",
          ledger, "queries=" + std::to_string(panel.accept_queries));
    check(std::abs(static_cast<double>(panel.last_position.x) - static_cast<double>(enter_x)) < 0.6 &&
              std::abs(static_cast<double>(panel.last_position.y) - static_cast<double>(enter_y)) < 0.6,
          "Enter position is the surface-local point from the protocol", ledger,
          "x=" + std::to_string(static_cast<double>(panel.last_position.x)) +
              " y=" + std::to_string(static_cast<double>(panel.last_position.y)));

    // 源侧能力与首选动作（enter 之后，协议序）：`action(Move)` 模拟用户按住 Shift 改判移动。
    wl_data_offer_send_source_actions(offer->res, static_cast<std::uint32_t>(drag::WaylandDndAction::Copy) |
                                                      static_cast<std::uint32_t>(drag::WaylandDndAction::Move));
    wl_data_offer_send_action(offer->res, static_cast<std::uint32_t>(drag::WaylandDndAction::Move));
    pump_both(m);

    // 悬停：位置变化 → Over
    wl_data_device_send_motion(m.srv_device, 7, wl_fixed_from_int(static_cast<int>(motion_x)),
                               wl_fixed_from_int(static_cast<int>(motion_y)));
    pump_both(m);
    const auto overs = static_cast<int>(std::count(panel.phase_log.begin(), panel.phase_log.end(), "Over"));
    check(overs >= 1, "drag-in dispatches Over on motion", ledger, "count=" + std::to_string(overs));
    check(panel.last_proposed == DragAction::Move,
          "Over proposed takes the compositor-reported action (not the Copy fallback)", ledger,
          std::string{"proposed="} + action_name(panel.last_proposed));
    check(panel.last_allowed == DragAction::Move, "Over allowed is the intersection of proposed and accept", ledger,
          std::string{"allowed="} + action_name(panel.last_allowed));
    check(offer->accepted == "text/plain;charset=utf-8", "target accepts the first advertised name", ledger,
          "accepted=" + (offer->accepted.empty() ? std::string{"<none>"} : offer->accepted));

    // 落放
    wl_data_device_send_drop(m.srv_device);
    pump_both(m);
    // 管道非阻塞：逐帧驱动直到 Drop 派发出来（读端由桥在 tick 里排空）。
    bool dropped = false;
    for (int i = 0; i < 128 && !dropped; ++i) {
        bridge.tick();
        pump_both(m);
        dropped = std::find(panel.phase_log.begin(), panel.phase_log.end(), "Drop") != panel.phase_log.end();
    }
    check(dropped, "drag-in dispatches Drop with payload after pipe drain", ledger);
    if (dropped) {
        check(panel.has_text && panel.last_text == AURORA_PROBE_TEXT, "Drop payload text round-trips through the pipe",
              ledger, "text=" + (panel.has_text ? panel.last_text : std::string{"<none>"}));
        check(panel.last_formats.size() == 2, "Drop carries the same format list as Enter", ledger,
              "n=" + std::to_string(panel.last_formats.size()));
        check(std::abs(static_cast<double>(panel.last_position.x) - static_cast<double>(motion_x)) < 0.6 &&
                  std::abs(static_cast<double>(panel.last_position.y) - static_cast<double>(motion_y)) < 0.6,
              "Drop position is the last motion in surface-local logical coords", ledger,
              "x=" + std::to_string(static_cast<double>(panel.last_position.x)) +
                  " y=" + std::to_string(static_cast<double>(panel.last_position.y)));
    }
    check(offer->received.size() == 2 && offer->received[0] == "text/plain;charset=utf-8" &&
              offer->received[1] == "text/uri-list",
          "target requests every offered MIME through the pipe in priority order", ledger,
          "n=" + std::to_string(offer->received.size()));
    check(offer->finish_called, "target calls wl_data_offer.finish after the transfer", ledger);
    check(!bridge.drag_in_active(), "bridge clears the drag-in session after Drop", ledger);
    return ledger.failures == 0;
}

/// @brief 拖出成功腿：begin → start_drag → send → dnd_finished → Succeeded/Copy。
auto leg_drag_out_ok(MockCompositor &m, detail::WaylandDragBridge &bridge, Ledger &ledger) -> bool {
    note("leg 2/4: drag-out success terminal (this window is the source)");
    SystemDragOutcome outcome = SystemDragOutcome::Failed;
    DragAction action = DragAction::None;
    bool ended = false;
    std::string provided_mime;
    std::string provided_text;

    DragOffer offer;
    offer.formats = {std::string{AURORA_MIME_TEXT_PLAIN}, std::string{AURORA_MIME_URI_LIST}};
    offer.default_action = DragAction::Copy;
    offer.provide = [&provided_mime, &provided_text](const std::string &mime) -> Result<std::vector<std::uint8_t>> {
        provided_mime = mime;
        provided_text = AURORA_PROBE_TEXT;
        return Result<std::vector<std::uint8_t>>{std::vector<std::uint8_t>(provided_text.begin(), provided_text.end())};
    };

    bridge.note_press_serial(4242U);
    const auto began = system_drag::begin(offer, [&](SystemDragOutcome o, DragAction a) {
        outcome = o;
        action = a;
        ended = true;
    });
    check(began.ok(), "system_drag::begin starts the session via the registered Wayland backend", ledger,
          began.ok() ? std::string{"Ok"} : began.error().message);
    if (!began.ok()) {
        return false;
    }
    pump_both(m);
    check(m.drag_started && m.drag_serial == 4242U, "bridge calls wl_data_device.start_drag with the press serial",
          ledger, "serial=" + std::to_string(m.drag_serial));
    check(!m.sources.empty() && m.sources.back().offers.size() == 3,
          "source advertises every registered name for the two offered MIMEs (2 text/plain + 1 uri-list)", ledger,
          "n=" + std::to_string(m.sources.empty() ? 0 : m.sources.back().offers.size()));
    if (m.sources.empty()) {
        return false;
    }
    check(m.sources.back().actions == drag::wayland_supported_dnd_actions(),
          "source declares Copy|Move via set_actions (v3)", ledger, "bits=" + std::to_string(m.sources.back().actions));

    // mock 合成器侧：目标接受 → 协商动作 → 落放发生 → 索取数据 → 会话收尾。
    wl_resource *source = m.drag_source;
    if (source == nullptr) {
        check(false, "mock compositor recorded the started drag source", ledger);
        return false;
    }
    wl_data_source_send_target(source, "text/plain;charset=utf-8");
    wl_data_source_send_action(source, static_cast<std::uint32_t>(drag::WaylandDndAction::Copy));
    wl_data_source_send_dnd_drop_performed(source);
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        check(false, "mock creates a pipe for the send event", ledger);
        return false;
    }
    wl_data_source_send_send(source, "text/plain;charset=utf-8", pipe_fds[1]);
    ::close(pipe_fds[1]);
    // 桥非阻塞写、逐帧推进：在此逐块读直到 EOF。
    std::string received;
    for (int i = 0; i < 128 && received.size() < AURORA_PROBE_TEXT_LEN; ++i) {
        bridge.tick();
        pump_both(m);
        char buf[256]{};
        const ssize_t n = ::read(pipe_fds[0], buf, sizeof(buf));
        if (n > 0) {
            received.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        }
    }
    ::close(pipe_fds[0]);
    check(provided_mime == AURORA_MIME_TEXT_PLAIN, "provide is called with the canonical MIME (parameters stripped)",
          ledger, "mime=" + provided_mime);
    check(provided_text == AURORA_PROBE_TEXT, "provide returns the probe payload", ledger, "text=" + provided_text);
    check(received == AURORA_PROBE_TEXT, "bytes written back through the peer pipe match the offer", ledger,
          "got=" + received);

    wl_data_source_send_dnd_finished(source);
    for (int i = 0; i < 64 && !ended; ++i) {
        system_drag::pump_reads();
        pump_both(m);
    }
    check(ended, "on_system_drag_end is delivered through pump_reads", ledger);
    check(outcome == SystemDragOutcome::Succeeded, "terminal outcome is Succeeded", ledger,
          std::string{"outcome="} + outcome_name(outcome));
    check(action == DragAction::Copy, "terminal action is the negotiated Copy", ledger,
          std::string{"action="} + action_name(action));
    check(!bridge.drag_out_active(), "bridge clears the drag-out session after dnd_finished", ledger);
    return ledger.failures == 0;
}

/// @brief 拖出取消腿：程序化取消 → Cancelled/None（不冒充失败）。
auto leg_drag_out_cancel(MockCompositor &m, detail::WaylandDragBridge &bridge, Ledger &ledger) -> bool {
    note("leg 3/4: drag-out programmatic cancel");
    SystemDragOutcome outcome = SystemDragOutcome::Failed;
    DragAction action = DragAction::None;
    bool ended = false;
    DragOffer offer;
    offer.formats = {std::string{AURORA_MIME_TEXT_PLAIN}};
    offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> {
        return Result<std::vector<std::uint8_t>>{std::vector<std::uint8_t>{'x'}};
    };
    bridge.note_press_serial(7U);
    const auto began = system_drag::begin(offer, [&](SystemDragOutcome o, DragAction a) {
        outcome = o;
        action = a;
        ended = true;
    });
    check(began.ok(), "second session starts", ledger, began.ok() ? std::string{"Ok"} : began.error().message);
    if (!began.ok()) {
        return false;
    }
    pump_both(m);
    system_drag::cancel();
    for (int i = 0; i < 64 && !ended; ++i) {
        system_drag::pump_reads();
        pump_both(m);
    }
    check(ended && outcome == SystemDragOutcome::Cancelled, "cancel ends the session as Cancelled", ledger,
          std::string{"ended="} + (ended ? outcome_name(outcome) : "no"));
    check(action == DragAction::None, "cancel carries no action", ledger, std::string{"action="} + action_name(action));
    check(!bridge.drag_out_active(), "bridge clears the drag-out session after cancel", ledger);
    return ledger.failures == 0;
}

/// @brief 可选冒烟腿：有 `WAYLAND_DISPLAY` 时核对能力位并建真实 `WaylandSurface` 泵帧。
auto leg_real_compositor_smoke(Ledger &ledger) -> bool {
    const char *display = ::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || *display == '\0') {
        note("leg 4/4 (optional): WAYLAND_DISPLAY unset -> real-compositor smoke leg PENDING");
        return true;
    }
    note("leg 4/4: real compositor smoke (WAYLAND_DISPLAY=" + std::string{display} + ")");
    const PlatformCapabilities caps = platform().capabilities();
    check(caps.system_drag_in && caps.system_drag_out, "capability bits are true on the Wayland backend", ledger,
          std::string{"in="} + (caps.system_drag_in ? "1" : "0") + " out=" + (caps.system_drag_out ? "1" : "0"));

    WaylandSurface surface(320, 240, "aurora_verify_wayland_drag", WindowStyleOptions{}, WindowVisibility::Hidden);
    if (!surface.is_available()) {
        note("real WaylandSurface unavailable in this session -> smoke leg PENDING (not a failure)");
        return true;
    }
    int pumped = 0;
    for (int i = 0; i < 30; ++i) {
        surface.poll_platform_events();
        ++pumped;
    }
    check(pumped == 30, "real Wayland surface pumps frames with the drag bridge attached", ledger,
          "frames=" + std::to_string(pumped));
    return ledger.failures == 0;
}

// ---------------------------------------------------------------------------
// 拖源面板（人工段：按住左键从本面板拖出）
// ---------------------------------------------------------------------------

class DragSourcePanel final : public LeafWidget {
  public:
    std::vector<std::string> out_log;  ///< 拖出态序列（begin / cancel / end:<outcome>:<action>）

    auto type_name() const -> const char * override { return "DragSourcePanel"; }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        size_ = c.constrain(Size{.width = 480.0F, .height = 100.0F});
        return size_;
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

    [[nodiscard]] auto on_provide_drag() -> std::optional<DragOffer> override {
        DragOffer offer;
        offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
        offer.default_action = DragAction::Copy;
        offer.provide = [](const std::string &mime) -> Result<std::vector<std::uint8_t>> {
            if (mime != AURORA_MIME_TEXT_PLAIN) {
                return make_error(ErrorCode::SystemDragReadFailed,
                                  ErrorParams{{"mime", mime}, {"detail", "probe offers text/plain only"}});
            }
            const std::string text = AURORA_PROBE_TEXT;
            return std::vector<std::uint8_t>(text.begin(), text.end());
        };
        return offer;
    }

    auto on_system_drag_begin() -> void override { out_log.emplace_back("begin"); }

    auto on_system_drag_cancel() -> void override { out_log.emplace_back("cancel"); }

    auto on_system_drag_end(SystemDragOutcome outcome, DragAction action) -> void override {
        out_log.emplace_back(std::string{"end:"} + outcome_name(outcome) + ":" + action_name(action));
    }
};

// ---------------------------------------------------------------------------
// 人工段：真实合成器 + 常驻窗口 + 逐帧打印
// ---------------------------------------------------------------------------

/// @brief 人工段：逐帧打印新增的四态与最近一次落放内容（格式同 X11 探针，便于两批记录表对照）。
auto report_interactive(DropPanel &panel, std::size_t &printed, int &last_files_count, int &last_text_count) -> void {
    while (panel.phase_log.size() > printed) {
        note(std::string("[drag] phase=") + panel.phase_log[printed] +
             " formats=" + std::to_string(panel.last_formats.size()) + " proposed=" + action_name(panel.last_proposed) +
             " allowed=" + action_name(panel.last_allowed));
        ++printed;
    }
    const int files = static_cast<int>(panel.last_files.size());
    if (files != last_files_count) {
        last_files_count = files;
        note(std::string("[drag] drop files=") + std::to_string(files));
        for (const std::string &path : panel.last_files) {
            note(std::string("[drag]   ") + path);
        }
    }
    // 文本长度单独一行：Wayland 侧载荷经管道流式取回，人工段只能靠字符数判断有没有被截断。
    const int chars = static_cast<int>(panel.last_text.size());
    if (chars != last_text_count) {
        last_text_count = chars;
        note(std::string("[drag] drop text=") + std::to_string(chars) + " chars");
    }
}

auto run_interactive() -> int {
    auto panel = std::make_shared<DropPanel>();
    auto source = std::make_shared<DragSourcePanel>();
    panel->layout(Constraints{}, BuildContext{});
    source->layout(Constraints{}, BuildContext{});

    auto surface = std::make_unique<WaylandSurface>(480, 360, "aurora_verify_wayland_drag", WindowStyleOptions{},
                                                    WindowVisibility::Normal);
    if (!surface->is_available()) {
        AURORA_LOG_ERROR("verify", "WaylandSurface unavailable (compositor connect failed or shm/xdg_wm_base missing)");
        return 2;
    }
    // 两种模式都不设帧预算：power_saving 关掉保证帧循环持续泵平台消息——Wayland 会话回调经
    // 事件队列到达，空闲阻塞等待会把拖放事件的派发推迟到人工操作之后。
    Application app(Scene{Node{Column(ColumnProps{.children = {Node{panel}, Node{source}}})}}, std::move(surface),
                    WindowOptions{.size = Size{.width = 480.0F, .height = 300.0F},
                                  .title = "Aurora Wayland drag verify",
                                  .max_frames = -1,
                                  .power_saving = false});
    std::size_t printed = 0;
    std::size_t printed_out = 0;
    int last_files_count = 0;
    int last_text_count = 0;
    app.set_on_frame([&]() -> void {
        report_interactive(*panel, printed, last_files_count, last_text_count);
        while (source->out_log.size() > printed_out) {
            note(std::string("[drag-out] ") + source->out_log[printed_out]);
            ++printed_out;
        }
    });
    note("---- Manual section ----");
    note("Drag-in: drag files from the file manager onto the upper panel. Expect Enter on entry ->");
    note("        repeated Over while moving -> Leave on exit -> Drop on release; dropped text is");
    note("        printed below. Hold Shift to see the proposed action change to Move.");
    note("Drag-out: press and hold the left button on the lower panel, drag outside this window");
    note("        (text editor / file manager), then release: expect begin -> end(Succeeded, Copy).");
    note("        Press Esc mid-drag: begin -> cancel -> end(Cancelled, None).");
    note("(no automated verdict in this section; closing the window exits)");
    app.run();
    return 0;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常 → 非零退出码呈现（捕获反而把它压成 0），与 demo 入口同口径。
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char **argv) {
    const auto cli = aurora_verify::parse_interactive("Wayland system drag-and-drop live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    if (interactive) {
        const char *display = ::getenv("WAYLAND_DISPLAY");
        if (display == nullptr || *display == '\0') {
            note("--interactive needs a real compositor (WAYLAND_DISPLAY unset)");
            return 2;
        }
        return run_interactive();
    }

    // 自动段：进程内 mock 合成器 + 桥 + 真派发链。
    MockCompositor mock;
    if (!mock_boot(mock)) {
        note("mock compositor boot failed (socketpair / wl_display_create / registry bind)");
        return 2;
    }
    Ledger ledger;
    check(mock.cli_ddm != nullptr && mock.cli_seat != nullptr && mock.cli_compositor != nullptr &&
              mock.srv_ddm != nullptr && mock.srv_seat != nullptr && mock.srv_compositor != nullptr &&
              mock.srv_surface != nullptr,
          "mock advertises wl_compositor / wl_seat / wl_data_device_manager and both sides are bound", ledger);

    auto panel = std::make_shared<DropPanel>();
    panel->layout(Constraints{}, BuildContext{});

    detail::WaylandDragBridge bridge(mock.cli_display, mock.cli_ddm, 3U, mock.cli_seat, mock.cli_surface,
                                     detail::WaylandDragBridge::Hooks{
                                         .emit = [&panel](Event &e) -> void {
                                             auto *drop = dynamic_cast<SystemDropEvent *>(&e);
                                             if (drop == nullptr) {
                                                 return;
                                             }
                                             // 经**真派发链**投递：命中测试 → accept 查询求交 → on_system_drop。
                                             // `allowed` 由派发器写定，桥据此回写 OS 光标（Wayland 侧即 accept
                                             // 的线上名）。
                                             (void)EventDispatcher::dispatch(*panel, *drop);
                                         },
                                         .scale_factor = []() -> float { return 1.0F; },
                                         .modifiers = []() -> ModifierKey { return ModifierKey::None; },
                                     });
    check(bridge.install(), "bridge installs against the mock data device manager", ledger);
    // `get_data_device` 是客户端请求：要被服务端处理完，mock 才有 srv_device 可发事件。
    const bool device_ready = client_roundtrip(mock) && mock.srv_device != nullptr;
    check(device_ready, "mock compositor created the data device for the bridge", ledger);
    detail::WaylandDragBridge::note_initiator(&bridge);

    if (device_ready) {
        (void)leg_drag_in(mock, bridge, *panel, ledger);
        (void)leg_drag_out_ok(mock, bridge, ledger);
        (void)leg_drag_out_cancel(mock, bridge, ledger);
    }
    (void)leg_real_compositor_smoke(ledger);

    bridge.uninstall();
    mock_shutdown(mock);

    note("---------------------------------------------------------------");
    note("checks=" + std::to_string(ledger.checks) + " failures=" + std::to_string(ledger.failures));
    if (ledger.failures > 0) {
        return 4;
    }
    const char *display = ::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || *display == '\0') {
        note("automated legs passed; real-compositor interactive legs NOT verified (no WAYLAND_DISPLAY)");
        note("-> re-run with --interactive on a Wayland session");
        return 3;
    }
    note("automated legs passed; real drag-in/drag-out still need a human (--interactive)");
    return 0;
}
