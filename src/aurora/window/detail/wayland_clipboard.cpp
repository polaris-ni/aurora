// Wayland CLIPBOARD 剪贴板桥实现（契约与门控见同名头文件）。
//
// 协议要点（wl_data_device / wl_data_source，核心协议；字段布局与规范同源，不在此重述规范文本）：
//   * 读入（target）：selection 事件携带一枚 `wl_data_offer`；`wl_data_offer_receive(offer, mime, fd)`
//     让对端往管道写，本端同步读完（剪贴板读取不挂帧循环排空点，见头文件注记）。
//   * 写侧（source）：建 `wl_data_source` → `offer(name)` 逐个广告 → `wl_data_device_set_selection`
//     声明所有权（serial 必须是本端某次真实输入事件的 serial：libwayland 公开 API 不提供「当前
//     serial」查询，故由表层记账经 hooks 传入；无 serial 时如实报写失败、不声明所有权）；
//     对端要数据时 `send(mime, fd)` 到达，本端把字节写回该 fd，写侧非阻塞 + `tick()` 逐帧推进。
//   * 所有权丢失：本端的 `wl_data_source` 被取代时收 `cancelled`（协议保证的权威信号）。
//   * 内容变化：读侧每次 selection 事件携带新 offer（或 null 表示清空）即触发 `on_change`。
//
// 与 `WaylandDragBridge` 共用 `wl_data_device`（每 seat 唯一）：本桥不复刻 device，只读侧与通知经
// 拖放桥的 selection 转发器到达；写侧只额外持有自己的 `wl_data_source`。

#include "wayland_clipboard.h"

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

// aurora 头先于 wayland 头（与 `wayland_drag.cpp` / `x11_drag.cpp` 同款纪律：平台协议头关在单个 TU 内）。
// clang-format off
#include "aurora/app/clipboard.h"
#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/window/detail/wayland_drag.h"

#include <wayland-client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <fcntl.h>
#include <poll.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>
// clang-format on

namespace aurora::detail {
namespace {

/// @brief 管道单块大小（字节）。
constexpr std::size_t AURORA_WL_PIPE_CHUNK = 4096U;

/// @brief 读入同步等待的对端写超时（毫秒）：剪贴板读取是用户发起、就地同步，给一个上界防钉死。
constexpr std::chrono::milliseconds AURORA_WL_READ_TIMEOUT = std::chrono::milliseconds{5000};

/// @brief 进程级存活桥清单（`void *` 形态：命名空间作用域不提私有嵌套类型名）。
auto live_backends() -> std::vector<void *> & {
    static std::vector<void *> live;
    return live;
}

/// @brief 把 fd 设为非阻塞（写侧用）。
auto set_nonblocking(int fd) -> void {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

}  // namespace

// ---- 协议状态机 ----
struct WaylandClipboardBridge::Impl {
    wl_display *dpy = nullptr;
    wl_data_device_manager *mgr = nullptr;
    std::uint32_t mgr_version = 0;
    wl_data_device *device = nullptr;  ///< 与拖放桥共享的那一枚
    wl_seat *seat = nullptr;
    Hooks hooks{};
    bool installed = false;

    bool owns = false;  ///< 本进程是否持有剪贴板所有权（claim 置位、`cancelled` 权威清除）
    wl_data_offer *self_offer = nullptr;  ///< 本端声明所有权时合成器回环的那枚 offer（仅需销毁）

    wl_data_source *source = nullptr;  ///< 写侧源（owns 时非空）
    wl_data_offer *foreign = nullptr;  ///< 读侧当前 offer（不 owns 时非空）
    std::vector<std::string> foreign_names;  ///< 当前 offer 的线上 MIME 名清单

    std::unordered_map<std::string, std::vector<std::uint8_t>> data_;  ///< 立即写（mime → 字节）
    std::optional<ClipboardOffer> offer_;  ///< 惰性要约

    /// @brief 在途写出作业（至多一个：同一时刻只有一个 `send` 事件）。
    struct Send {
        int fd = -1;
        std::vector<std::uint8_t> data;
        std::size_t offset = 0;
    } send;

    // ---- 安装 / 卸载 ----
    auto install() -> bool;
    auto uninstall() -> void;

    // ---- selection 转发（来自拖放桥）----
    auto on_selection(wl_data_offer *offer, const std::vector<std::string> &names) -> void;

    // ---- 所有权 ----
    [[nodiscard]] auto claim() -> bool;
    auto teardown_source() -> void;

    // ---- 写侧（wl_data_source）----
    auto on_source_send(const char *mime_type, int fd) -> void;
    auto on_source_cancelled() -> void;
    auto pump_send() -> void;

    // ---- 读入（同步）----
    [[nodiscard]] auto read_sync(wl_data_offer *offer, const std::string &mime) -> Result<std::vector<std::uint8_t>>;

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

namespace {

// ---- wl_data_source 监听器（写侧的取数 / 所有权丢失）----
void source_send(void *data, wl_data_source *source, const char *mime_type, std::int32_t fd) {
    auto *self = static_cast<WaylandClipboardBridge::Impl *>(data);
    if (self->source != source) {
        if (fd >= 0) {
            ::close(fd);  // 旧 source 的迟到 send：本端已换源，拒绝服务（fd 须由本端关闭）
        }
        return;
    }
    self->on_source_send(mime_type, fd);
}

void source_cancelled(void *data, wl_data_source *source) {
    auto *self = static_cast<WaylandClipboardBridge::Impl *>(data);
    if (self->source != source) {
        return;  // 被本端替换/销毁的旧 source 的迟到 cancelled：不得拆掉当前 source
    }
    self->on_source_cancelled();
}

constexpr wl_data_source_listener AURORA_CB_SOURCE_LISTENER = {
    .target = nullptr,  // 剪贴板源不需 target 协商
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = nullptr,  // 剪贴板源不收 drop 终态事件
    .dnd_finished = nullptr,
    .action = nullptr,
};

}  // namespace

// ---- 安装 / 卸载 ----

auto WaylandClipboardBridge::Impl::install() -> bool {
    if (installed || mgr == nullptr || device == nullptr || seat == nullptr || dpy == nullptr) {
        return false;
    }
    // 登记后端表并挂 selection 转发器：本桥与拖放桥共用同一枚 wl_data_device，剪贴板桥不能
    // 另加监听器（协议只允许一枚），读侧与变更通知全经该转发器到达。
    installed = true;
    register_backend(this);
    return true;
}

auto WaylandClipboardBridge::Impl::uninstall() -> void {
    if (!installed) {
        return;  // 未安装成功：既无后端表也无转发器可撤
    }
    installed = false;
    teardown_source();
    if (foreign != nullptr) {
        wl_data_offer_destroy(foreign);
        foreign = nullptr;
    }
    foreign_names.clear();
    data_.clear();
    offer_.reset();
    hooks.on_change = nullptr;
    // 最后撤后端表 / 转发器：二者都捕获本桥自指针，先撤再让桥进入析构。
    unregister_backend(this);
}

// ---- selection 转发 ----

auto WaylandClipboardBridge::Impl::on_selection(wl_data_offer *offer, const std::vector<std::string> &names) -> void {
    if (owns) {
        // 本端已声明选择权：此刻到达的 selection 只能是本端 source 的回环（协议事件按序送达，
        // 外来选择必先经 `cancelled` 夺权）。回环 offer 只需记账待销毁，不改所有权、不重复通知。
        if (offer != nullptr && offer != self_offer) {
            if (self_offer != nullptr) {
                wl_data_offer_destroy(self_offer);
            }
            self_offer = offer;
        }
        return;
    }
    if (offer == nullptr) {
        // 选择区被清空（无人持有）：如实清态并通知。
        if (foreign != nullptr) {
            wl_data_offer_destroy(foreign);
        }
        foreign = nullptr;
        foreign_names.clear();
        if (hooks.on_change != nullptr) {
            hooks.on_change();
        }
        return;
    }
    if (offer == foreign) {
        // 同一 offer 的迟到名称（转发器的增量补发）：只追加清单，不重复通知。
        foreign_names.insert(foreign_names.end(), names.begin(), names.end());
        return;
    }
    // 外来 selection：刷新当前读侧 offer 并触发一次变更通知。
    if (foreign != nullptr) {
        wl_data_offer_destroy(foreign);
    }
    foreign = offer;
    foreign_names = names;
    if (hooks.on_change != nullptr) {
        hooks.on_change();
    }
}

// ---- 所有权 ----

auto WaylandClipboardBridge::Impl::claim() -> bool {
    // `set_selection` 的 serial 必须是本客户端某次真实输入事件的 serial（协议要求）；没有输入
    // serial 时宁可不声明，也不拿编造值去换一个静默失效的选择权。
    const std::uint32_t serial = hooks.input_serial != nullptr ? hooks.input_serial() : 0U;
    if (serial == 0U) {
        AURORA_LOG_WARN("clipboard", "Wayland: no input serial; clipboard selection not claimed.");
        return false;
    }
    // 换内容须新建源（协议不允许在同一源上追加已广告名）：旧源、其回环 offer 与在途写一并收掉。
    teardown_source();
    source = wl_data_device_manager_create_data_source(mgr);
    if (source == nullptr) {
        AURORA_LOG_WARN("clipboard", "Wayland: create_data_source failed; cannot claim clipboard.");
        return false;
    }
    wl_data_source_add_listener(source, &AURORA_CB_SOURCE_LISTENER, this);
    // 广告全部可提供的线上名（立即写 + 惰性要约）；线名即 MIME 原串（剪贴板不映射到 Wayland 专有名）。
    for (const auto &kv : data_) {
        wl_data_source_offer(source, kv.first.c_str());
    }
    if (offer_.has_value()) {
        for (const auto &mime : offer_->formats) {
            wl_data_source_offer(source, mime.c_str());
        }
    }
    wl_data_device_set_selection(device, source, serial);
    wl_display_flush(dpy);
    owns = true;
    return true;
}

auto WaylandClipboardBridge::Impl::teardown_source() -> void {
    if (send.fd >= 0) {
        ::close(send.fd);
        send = Send{};
    }
    if (self_offer != nullptr) {
        wl_data_offer_destroy(self_offer);
        self_offer = nullptr;
    }
    if (source != nullptr) {
        wl_data_source_destroy(source);
        source = nullptr;
    }
    owns = false;
}

// ---- 写侧 ----

auto WaylandClipboardBridge::Impl::on_source_send(const char *mime_type, int fd) -> void {
    if (fd < 0 || !owns || source == nullptr) {
        if (fd >= 0) {
            ::close(fd);
        }
        return;
    }
    set_nonblocking(fd);
    if (mime_type == nullptr) {
        ::close(fd);  // 目标放弃这次传输
        return;
    }
    const std::string mime{mime_type};
    std::optional<std::vector<std::uint8_t>> bytes;
    if (const auto it = data_.find(mime); it != data_.end()) {
        bytes = it->second;
    } else if (offer_.has_value() && offer_->provide != nullptr) {
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
                ::close(fd);
                return;  // 惰性提供失败：不谎报成功
            }
            bytes = produced.value();
        }
    }
    if (!bytes.has_value()) {
        ::close(fd);
        return;
    }
    if (send.fd >= 0) {
        ::close(send.fd);  // 上一笔未写完：丢弃（同一时刻只应有一笔，防御性收口）
        send = Send{};
    }
    send.fd = fd;
    send.data = std::move(*bytes);
    send.offset = 0;
    pump_send();
}

auto WaylandClipboardBridge::Impl::on_source_cancelled() -> void {
    teardown_source();  // 源被取代 ⇒ 本进程不再持有剪贴板
    clipboard::detail::fire_ownership_lost();
}

auto WaylandClipboardBridge::Impl::pump_send() -> void {
    if (send.fd < 0) {
        return;
    }
    for (;;) {
        if (send.offset >= send.data.size()) {
            ::close(send.fd);
            send = Send{};
            return;
        }
        const auto *base = std::next(send.data.data(), static_cast<std::ptrdiff_t>(send.offset));
        const auto left = static_cast<std::size_t>(send.data.size() - send.offset);
        const ssize_t wrote = ::write(send.fd, base, left);
        if (wrote > 0) {
            send.offset += static_cast<std::size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // 对端还没读：等下一帧
        }
        ::close(send.fd);
        send = Send{};
        return;
    }
}

// ---- 读入 ----

auto WaylandClipboardBridge::Impl::read_sync(wl_data_offer *offer, const std::string &mime)
    -> Result<std::vector<std::uint8_t>> {
    if (offer == nullptr) {
        return std::vector<std::uint8_t>{};  // 无 offer = 空内容
    }
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", "pipe() failed for receive"}});
    }
    wl_data_offer_receive(offer, mime.c_str(), fds[1]);
    ::close(fds[1]);
    set_nonblocking(fds[0]);
    wl_display_flush(dpy);
    std::vector<std::uint8_t> out;
    const auto deadline = std::chrono::steady_clock::now() + AURORA_WL_READ_TIMEOUT;
    for (;;) {
        std::array<std::uint8_t, AURORA_WL_PIPE_CHUNK> chunk{};
        const ssize_t got = ::read(fds[0], chunk.data(), chunk.size());
        if (got > 0) {
            out.insert(out.end(), chunk.begin(), std::next(chunk.begin(), static_cast<std::ptrdiff_t>(got)));
            continue;
        }
        if (got == 0) {
            break;  // EOF：对端写完
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto remain =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            if (remain <= 0) {
                break;  // 超时：返回已收到的部分
            }
            pollfd pfd{fds[0], POLLIN, 0};
            ::poll(&pfd, 1, static_cast<int>(remain));
            continue;
        }
        break;  // 其余错误：返回已收到的部分
    }
    ::close(fds[0]);
    return out;
}

// ---- 后端九入口 ----

auto WaylandClipboardBridge::Impl::backend_set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes)
    -> Result<bool> {
    if (mime.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, ErrorParams{{"detail", "set_payload: mime empty"}});
    }
    if (bytes.empty()) {
        return Result<bool>{true};  // 契约：空字节 no-op，保留既有内容
    }
    std::optional<std::vector<std::uint8_t>> previous;
    if (const auto it = data_.find(mime); it != data_.end()) {
        previous = it->second;
    }
    data_[mime] = bytes;  // 立即写覆盖同 mime 的惰性声明
    if (!claim()) {
        // 声明失败即本次写未生效：回滚本地态，避免下次成功声明把这次失败的内容一并广告出去。
        if (previous.has_value()) {
            data_[mime] = std::move(*previous);
        } else {
            data_.erase(mime);
        }
        return make_error(ErrorCode::ClipboardWriteFailed,
                          ErrorParams{{"detail", "Wayland: no input serial for wl_data_device.set_selection"}});
    }
    return Result<bool>{true};
}

auto WaylandClipboardBridge::Impl::backend_get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    if (mime.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, ErrorParams{{"detail", "get_payload: mime empty"}});
    }
    if (owns) {
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
        return std::vector<std::uint8_t>{};  // 自己持有但无此格式 = 空字节
    }
    // 读侧：线名即 MIME 原串
    bool present = false;
    for (const auto &n : foreign_names) {
        if (n == mime) {
            present = true;
            break;
        }
    }
    if (!present) {
        return std::vector<std::uint8_t>{};  // 对端无此格式 = 空字节（空内容不是失败）
    }
    return read_sync(foreign, mime);
}

auto WaylandClipboardBridge::Impl::backend_set_files(const std::vector<std::string> &paths) -> Result<bool> {
    const auto encoded = uri_list::encode(paths);
    const std::vector<std::uint8_t> bytes{encoded.begin(), encoded.end()};
    return backend_set_payload(std::string{AURORA_MIME_URI_LIST}, bytes);
}

auto WaylandClipboardBridge::Impl::backend_get_files() -> Result<std::vector<std::string>> {
    const auto raw = backend_get_payload(std::string{AURORA_MIME_URI_LIST});
    if (!raw) {
        return raw.error();  // 原码透传：provider-failed / access-failed 不在桥内改写
    }
    const std::string text{raw.value().begin(), raw.value().end()};
    return uri_list::decode(text);
}

auto WaylandClipboardBridge::Impl::backend_set_html(const std::string &html) -> Result<bool> {
    const std::vector<std::uint8_t> bytes{html.begin(), html.end()};
    return backend_set_payload(std::string{AURORA_MIME_TEXT_HTML}, bytes);
}

auto WaylandClipboardBridge::Impl::backend_get_html() -> Result<std::string> {
    const auto raw = backend_get_payload(std::string{AURORA_MIME_TEXT_HTML});
    if (!raw) {
        return raw.error();  // 原码透传：provider-failed / access-failed 不在桥内改写
    }
    return std::string{raw.value().begin(), raw.value().end()};
}

auto WaylandClipboardBridge::Impl::backend_set_offer(const ClipboardOffer &offer) -> Result<void> {
    if (offer.formats.empty() || offer.provide == nullptr) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          ErrorParams{{"detail", "set_offer: formats empty or provide null"}});
    }
    const std::optional<ClipboardOffer> previous = offer_;
    offer_ = offer;  // 惰性要约与立即写并存（后写者接管同 mime）
    if (!claim()) {
        offer_ = previous;  // 声明失败即本次写未生效：回滚，见 set_payload 同款理由
        return make_error(ErrorCode::ClipboardWriteFailed,
                          ErrorParams{{"detail", "Wayland: no input serial for wl_data_device.set_selection"}});
    }
    return Result<void>{};
}

auto WaylandClipboardBridge::Impl::backend_available_formats() -> Result<std::vector<std::string>> {
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
    return foreign_names;  // 读侧：如实列出当前 offer 的线上名
}

auto WaylandClipboardBridge::Impl::backend_set_change_listener(std::function<void()> callback) -> Result<void> {
    // Wayland 的 selection 事件始终可达（无需额外扩展），故变更监听恒支持、不谎报。
    hooks.on_change = std::move(callback);
    return Result<void>{};
}

// ---- 桥的公共面 ----

WaylandClipboardBridge::WaylandClipboardBridge(void *display, void *manager, std::uint32_t manager_version,
                                               void *device, void *seat, Hooks hooks)
    : impl_(std::make_unique<Impl>()) {
    impl_->dpy = static_cast<wl_display *>(display);
    impl_->mgr = static_cast<wl_data_device_manager *>(manager);
    impl_->mgr_version = manager_version;
    impl_->device = static_cast<wl_data_device *>(device);
    impl_->seat = static_cast<wl_seat *>(seat);
    impl_->hooks = std::move(hooks);
}

WaylandClipboardBridge::~WaylandClipboardBridge() { uninstall(); }

auto WaylandClipboardBridge::install() -> bool { return impl_->install(); }

auto WaylandClipboardBridge::uninstall() -> void { impl_->uninstall(); }

auto WaylandClipboardBridge::tick() -> void { impl_->pump_send(); }

auto WaylandClipboardBridge::owns() const -> bool { return impl_->owns; }

auto WaylandClipboardBridge::install_backend_for(Impl *self) -> void {
    // 转发器与后端表都只认「当前桥」：后登记的窗口接管，注销时改挂最近存活桥。
    WaylandDragBridge::set_clipboard_forwarder(
        [self](wl_data_offer *offer, const std::vector<std::string> &names) { self->on_selection(offer, names); });
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

auto WaylandClipboardBridge::register_backend(Impl *self) -> void {
    auto &live = live_backends();
    auto *slot = static_cast<void *>(self);
    if (std::find(live.begin(), live.end(), slot) == live.end()) {
        live.push_back(slot);
    }
    install_backend_for(self);
}

auto WaylandClipboardBridge::unregister_backend(Impl *self) -> void {
    auto &live = live_backends();
    auto *slot = static_cast<void *>(self);
    live.erase(std::remove(live.begin(), live.end(), slot), live.end());
    if (live.empty()) {
        WaylandDragBridge::set_clipboard_forwarder({});  // 无存活桥：摘掉捕获已亡桥的转发器
        clipboard::detail::remove_backend();
        return;
    }
    install_backend_for(static_cast<WaylandClipboardBridge::Impl *>(live.back()));
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_WAYLAND
