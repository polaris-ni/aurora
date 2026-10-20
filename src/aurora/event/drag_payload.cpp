#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/event/event.h"
#include "aurora/image/image_codec.h"

#if defined(AURORA_ENABLE_DEBUG) && defined(AURORA_ENABLE_TEST_HOOKS)
#define AURORA_SYSTEM_DRAG_TEST_BACKEND 1  // NOLINT(*-macro-usage)
#endif

namespace aurora {

namespace {

/// @brief 会话载荷的 MIME 键：三条便利读取各自认的常量，取自 MIME 词汇表单一真源。
constexpr std::string_view AURORA_DRAG_MIME_URI_LIST = AURORA_MIME_URI_LIST;
constexpr std::string_view AURORA_DRAG_MIME_TEXT = AURORA_MIME_TEXT_PLAIN;
constexpr std::string_view AURORA_DRAG_MIME_PNG = AURORA_MIME_IMAGE_PNG;

/// @brief 把 `string_view` 形式的 MIME 常量折成 `unordered_map` 查找所需的 `std::string` 键。
[[nodiscard]] auto mime_key(std::string_view mime) -> std::string { return std::string{mime}; }

/// @brief 在途读取的默认墙钟超时（生产路径固定；测试经 `test_set_read_timeout` 调整）。
constexpr auto AURORA_SYSTEM_DRAG_READ_TIMEOUT = std::chrono::seconds{5};

/// @brief 一条在途读取：登记 → 帧循环排空兑现 → 摘除（回调只调一次）。
struct PendingRead {
    std::shared_ptr<const DragPayload> payload;  ///< 强引用：读取期间数据必须存活
    std::string mime;  ///< 待读取的 MIME
    std::function<void(Result<std::vector<std::uint8_t>>)> callback;  ///< 兑现回调（只调一次）
    std::chrono::steady_clock::time_point deadline;  ///< 墙钟超时点
};

/// @brief 在途读取登记表（进程级；与后端无关——内存载荷的读取同样经帧循环排空兑现）。
struct ReadRegistry {
    std::mutex mutex;  ///< 保护以下成员
    std::vector<PendingRead> pending;  ///< 在途项（按登记序）
    std::chrono::milliseconds timeout = AURORA_SYSTEM_DRAG_READ_TIMEOUT;  ///< 墙钟超时（测试可调）
    int drain_depth = 0;  ///< 排空嵌套深度（> 0 即处于排空阶段；嵌套排空不提前清标记）
};

/// @brief 进程唯一读取登记表（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto read_registry() -> ReadRegistry & {
    static ReadRegistry registry;
    return registry;
}

/// @brief 拖出会话状态（进程级；无后端时 `begin` 直接失败，故事态恒空）。
struct DragSession {
    std::mutex mutex;  ///< 保护以下成员
    bool active = false;  ///< 是否有在途会话
    DragOffer offer;  ///< 在途会话的要约（含影像 / 缺省动作 / Move 确认回调）
    std::function<void(SystemDragOutcome, DragAction)> on_end;  ///< 终结回调（帧循环投递）
    bool end_queued = false;  ///< 终态已产生、待帧循环投递
    SystemDragOutcome queued_outcome = SystemDragOutcome::Failed;  ///< 待投递结局
    DragAction queued_action = DragAction::None;  ///< 待投递动作
    std::optional<Error> last_error;  ///< 最近一次会话错误（Move 回滚上报，测试可读）
};

/// @brief 进程唯一会话状态（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto drag_session() -> DragSession & {
    static DragSession session;
    return session;
}

#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
/// @brief 测试注入点（test-only）：进程内 inert 后端，安装后 `begin` 不触碰平台。
struct SystemDragTestBackend {
    std::mutex mutex;  ///< 保护以下成员
    bool active = false;  ///< 后端是否已安装
    std::unordered_set<std::string> withheld;  ///< 被扣留的 MIME（读取保持未决直到超时）
    std::optional<DragOffer> last_offer;  ///< 最近一次 `begin` 登记的要约（测试断言字段往返）
};

[[nodiscard]] auto system_drag_test_backend() -> SystemDragTestBackend & {
    static SystemDragTestBackend backend;
    return backend;
}
#endif

/// @brief 在途读取的数据可兑现性：payload 未持有该格式 = 空口径可兑现；持有但被扣留 = 未决。
[[nodiscard]] auto read_data_ready(const PendingRead &entry) -> bool {
    if (!entry.payload->has(entry.mime)) {
        return true;  // 格式不存在 = Ok(空) 可兑现（空内容不是失败）
    }
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &backend = system_drag_test_backend();
    const std::scoped_lock lock{backend.mutex};
    return !backend.withheld.contains(entry.mime);
#else
    return true;
#endif
}

/// @brief 兑现一条在途读取：数据就绪 ⇒ 字节；空口径 ⇒ 空值；未到位 / 超时 ⇒ 读取失败。
[[nodiscard]] auto fulfill_read(const PendingRead &entry, bool data_ready, std::chrono::steady_clock::time_point now)
    -> Result<std::vector<std::uint8_t>> {
    if (!entry.payload->has(entry.mime)) {
        return std::vector<std::uint8_t>{};  // 空口径：格式不存在不是失败
    }
    if (!data_ready || entry.deadline <= now) {
        return make_error(ErrorCode::SystemDragReadFailed,
                          ErrorParams{{"mime", entry.mime},
                                      {"detail", data_ready ? "read deadline exceeded before the payload arrived"
                                                            : "session ended before the payload arrived"}});
    }
    return entry.payload->bytes(entry.mime);  // 同步读取：内存载荷即取即得
}

/// @brief 就地收口全部在途读取（`cancel` 用：不等帧循环，有数据即兑现、否则失败兑现，不悬挂）。
auto settle_all_reads() -> void {
    auto &registry = read_registry();
    std::vector<PendingRead> all;
    {
        const std::scoped_lock lock{registry.mutex};
        all.swap(registry.pending);
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto &entry : all) {
        const bool ready = read_data_ready(entry);
        entry.callback(fulfill_read(entry, ready, now));
    }
}

/// @brief 登记会话终态（待帧循环投递）；无在途会话时 no-op。
auto complete_session(SystemDragOutcome outcome, DragAction action) -> void {
    auto &session = drag_session();
    const std::scoped_lock lock{session.mutex};
    if (!session.active) {
        return;
    }
    session.end_queued = true;
    session.queued_outcome = outcome;
    session.queued_action = action;
}

/// @brief 记录会话错误（Move 回滚上报；测试可读）。
auto record_session_error(Error err) -> void {
    auto &session = drag_session();
    const std::scoped_lock lock{session.mutex};
    session.last_error = std::move(err);
}

/// @brief 投递在途会话终态：先 Move 确认（失败则终态改写并上报回滚），后 `on_end`。
///
/// 调用点顺序是契约的一部分：**源不得在收到确认前删除任何数据**，故事件驱动下「确认严格早于
/// `on_end(Succeeded, Move)`」从外部可断言。锁外调用两类回调（应用代码可能再次进入本命名空间）。
auto deliver_session_end() -> void {
    std::function<void(SystemDragOutcome, DragAction)> on_end;
    std::function<Result<void>()> confirm;
    SystemDragOutcome outcome = SystemDragOutcome::Failed;
    DragAction action = DragAction::None;
    {
        auto &session = drag_session();
        const std::scoped_lock lock{session.mutex};
        if (!session.end_queued) {
            return;
        }
        outcome = session.queued_outcome;
        action = session.queued_action;
        confirm = (action == DragAction::Move) ? session.offer.on_move_confirmed : nullptr;
        on_end = session.on_end;
        session.end_queued = false;
        session.active = false;
    }
    if (confirm) {
        const auto confirmed = confirm();
        if (!confirmed) {
            // Move 落放后源侧删除失败：终态改写为 Move 失败并上报回滚；目标侧数据保持不动。
            outcome = SystemDragOutcome::Failed;
            record_session_error(
                make_error(ErrorCode::SystemDragMoveRollback, ErrorParams{{"detail", confirmed.error().message}}));
        }
    }
    if (on_end) {
        on_end(outcome, action);
    }
}

}  // namespace

// ---- DragPayload（pimpl）----

struct DragPayload::Impl {
    std::unordered_map<std::string, std::vector<std::uint8_t>> data;  ///< MIME → 原始字节
};

DragPayload::DragPayload() : impl_(std::make_unique<Impl>()) {}

DragPayload::~DragPayload() = default;

auto DragPayload::from_bytes(std::unordered_map<std::string, std::vector<std::uint8_t>> data)
    -> std::shared_ptr<const DragPayload> {
    // 私有构造仅供本工厂使用：实例一律以「只读句柄」形态交出，调用方无法再改其内容。
    auto *raw = new DragPayload();  // NOLINT(*-owning-memory): 随即交 shared_ptr 接管
    raw->impl_->data = std::move(data);
    raw->impl_->data.erase(std::string{});  // 空 MIME 键无意义，直接丢弃
    return std::shared_ptr<const DragPayload>(raw);
}

auto DragPayload::has(const std::string &mime) const -> bool { return impl_->data.contains(mime); }

auto DragPayload::bytes(const std::string &mime) const -> Result<std::vector<std::uint8_t>> {
    const auto it = impl_->data.find(mime);
    if (it == impl_->data.end()) {
        return std::vector<std::uint8_t>{};  // 格式不存在 = Ok(空)：空内容不是失败
    }
    return it->second;
}

auto DragPayload::files() const -> Result<std::vector<std::string>> {
    const auto raw = bytes(mime_key(AURORA_DRAG_MIME_URI_LIST));
    if (!raw) {
        return raw.error();
    }
    // 解码只留可映射为本地路径的条目：非 file scheme 的行（http:// 等）静默跳过，
    // 完整原始行仍可经 bytes(uri-list) 取得。
    return uri_list::decode(
        std::string_view(reinterpret_cast<const char *>(raw.value().data()),  // NOLINT(*-pro-type-reinterpret-cast)
                         raw.value().size()));
}

auto DragPayload::text() const -> Result<std::string> {
    const auto raw = bytes(mime_key(AURORA_DRAG_MIME_TEXT));
    if (!raw) {
        return raw.error();
    }
    const auto &buf = raw.value();
    return std::string(buf.begin(), buf.end());
}

auto DragPayload::image() const -> Result<Image> {
    const auto raw = bytes(mime_key(AURORA_DRAG_MIME_PNG));
    if (!raw) {
        return raw.error();
    }
    if (raw.value().empty()) {
        return Image{};  // 无该格式 = Ok(空 Image)
    }
    auto decoded = image::decode_memory(raw.value());
    if (!decoded) {
        // 字节已取到但按 MIME 解码失败：专属错误码（同一份字节重试无意义，retryable = false）。
        return make_error(ErrorCode::SystemDragDecodeFailed,
                          ErrorParams{{"mime", mime_key(AURORA_DRAG_MIME_PNG)}, {"detail", decoded.error().message}});
    }
    return decoded.value();
}

auto DragPayload::read_async(const std::string &mime,
                             std::function<void(Result<std::vector<std::uint8_t>>)> callback) const -> void {
    // 登记在途项（句柄 const 只读，登记表是进程级外部状态，故无须 mutable 成员）：结果经
    // pump_reads() 在帧内排空兑现，宿主不排空 ⇒ 回调不到（不谎报）。
    auto &registry = read_registry();
    const std::scoped_lock lock{registry.mutex};
    registry.pending.push_back(PendingRead{
        .payload = shared_from_this(),
        .mime = mime,
        .callback = std::move(callback),
        .deadline = std::chrono::steady_clock::now() + registry.timeout,
    });
}

// ---- SystemDropEvent 的便利读取 ----
// 无载荷（enter / over / leave 态的常态）一律按空口径返回：不崩溃、不报错。

auto SystemDropEvent::files() const -> Result<std::vector<std::string>> {
    if (payload == nullptr) {
        return std::vector<std::string>{};
    }
    return payload->files();
}

auto SystemDropEvent::text() const -> Result<std::string> {
    if (payload == nullptr) {
        return std::string{};
    }
    return payload->text();
}

auto SystemDropEvent::image() const -> Result<Image> {
    if (payload == nullptr) {
        return Image{};
    }
    return payload->image();
}

auto SystemDropEvent::bytes(const std::string &mime) const -> Result<std::vector<std::uint8_t>> {
    if (payload == nullptr) {
        return std::vector<std::uint8_t>{};
    }
    return payload->bytes(mime);
}

// 非阻塞便利读取：无载荷时同步按空口径兑现（没有会话可排空）；有载荷时登记在途项，
// 经 system_drag::pump_reads() 在帧内排空兑现（回调只调一次）。

auto SystemDropEvent::files_async(std::function<void(Result<std::vector<std::string>>)> callback) const -> void {
    if (payload == nullptr) {
        callback(std::vector<std::string>{});
        return;
    }
    payload->read_async(mime_key(AURORA_DRAG_MIME_URI_LIST), [callback = std::move(callback)](
                                                                 const Result<std::vector<std::uint8_t>> &raw) {
        if (!raw) {
            callback(raw.error());
            return;
        }
        callback(uri_list::decode(
            std::string_view(reinterpret_cast<const char *>(raw.value().data()),  // NOLINT(*-pro-type-reinterpret-cast)
                             raw.value().size())));
    });
}

auto SystemDropEvent::text_async(std::function<void(Result<std::string>)> callback) const -> void {
    if (payload == nullptr) {
        callback(std::string{});
        return;
    }
    payload->read_async(mime_key(AURORA_DRAG_MIME_TEXT),
                        [callback = std::move(callback)](const Result<std::vector<std::uint8_t>> &raw) {
                            if (!raw) {
                                callback(raw.error());
                                return;
                            }
                            const auto &buf = raw.value();
                            callback(std::string(buf.begin(), buf.end()));
                        });
}

auto SystemDropEvent::image_async(std::function<void(Result<Image>)> callback) const -> void {
    if (payload == nullptr) {
        callback(Image{});
        return;
    }
    payload->read_async(mime_key(AURORA_DRAG_MIME_PNG), [callback = std::move(callback)](
                                                            const Result<std::vector<std::uint8_t>> &raw) {
        if (!raw) {
            callback(raw.error());
            return;
        }
        if (raw.value().empty()) {
            callback(Image{});  // 无该格式 = Ok(空 Image)
            return;
        }
        auto decoded = image::decode_memory(raw.value());
        if (!decoded) {
            // 字节已取到但按 MIME 解码失败：专属错误码（同一份字节重试无意义）。
            callback(make_error(ErrorCode::SystemDragDecodeFailed, ErrorParams{{"mime", mime_key(AURORA_DRAG_MIME_PNG)},
                                                                               {"detail", decoded.error().message}}));
            return;
        }
        callback(decoded.value());
    });
}

auto SystemDropEvent::bytes_async(const std::string &mime,
                                  std::function<void(Result<std::vector<std::uint8_t>>)> callback) const -> void {
    if (payload == nullptr) {
        callback(std::vector<std::uint8_t>{});
        return;
    }
    payload->read_async(mime, std::move(callback));
}

// ---- 系统拖出会话（事件驱动：发起即返回，终态与读取结果经帧循环排空兑现）----

auto system_drag::begin(const DragOffer &offer, std::function<void(SystemDragOutcome, DragAction)> on_end)
    -> Result<void> {
    // 要约不合法是调用方参数问题，与「平台没有机制」是两类失败，分开报。
    if (offer.formats.empty() || !offer.provide) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "system_drag::begin: empty formats or no provide callback");
    }
    // 机制缺失：没有任何后端接线（真实平台后端或测试 inert 后端）时落回 SystemDragNotSupported。
    // 过渡期所有 SurfaceKind 的 system_drag_in/out capability 均为 false（见同文件
    // capability_bits_for_system_drag_are_false_until_backends_land 用例），故此处恒报机制缺失；
    // 真实后端接线后会经同一入口放行（其 available 标志翻 true），届时本分支不再触发。
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    {
        auto &backend = system_drag_test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (!backend.active) {
            return make_error(ErrorCode::SystemDragNotSupported,
                              "system_drag::begin: no system drag-and-drop backend is wired on this platform yet");
        }
        backend.last_offer = offer;  // inert 后端记录要约（测试断言影像 / 缺省动作等字段往返）
    }
#else
    return make_error(ErrorCode::SystemDragNotSupported,
                      "system_drag::begin: no system drag-and-drop backend is wired on this platform yet");
#endif
    {
        auto &session = drag_session();
        const std::scoped_lock lock{session.mutex};
        if (session.active) {
            // 单线程 UI 下一次只能有一个拖出会话（OS 同款约束）。
            return make_error(ErrorCode::SystemDragSessionFailed,
                              "system_drag::begin: another drag session is already in flight");
        }
        session.active = true;
        session.offer = offer;
        session.on_end = std::move(on_end);
        session.end_queued = false;
        session.last_error.reset();
    }
    return Result<void>{};
}

auto system_drag::cancel() -> void {
    // ① 就地收口在途读取（不等帧循环）：有数据即兑现，否则按读取失败兑现，不悬挂。
    settle_all_reads();
    // ② 完成在途会话（终态 Cancelled，经帧循环投递 on_end）；无在途会话则 no-op。
    complete_session(SystemDragOutcome::Cancelled, DragAction::None);
}

auto system_drag::pump_reads() -> void {
    auto &registry = read_registry();
    std::vector<PendingRead> due;
    std::vector<PendingRead> retained;
    const auto now = std::chrono::steady_clock::now();
    {
        const std::scoped_lock lock{registry.mutex};
        ++registry.drain_depth;  // 排空阶段标记：回调在帧内兑现而非消息泵内同步回调
        for (auto &entry : registry.pending) {
            if (entry.deadline <= now || read_data_ready(entry)) {
                due.push_back(std::move(entry));
            } else {
                retained.push_back(std::move(entry));  // 数据未到位且未超时：留到下一拍
            }
        }
        registry.pending = std::move(retained);
    }
    // 深度随本轮兑现结束而递减（守卫出栈即减，含回调内再入排空的嵌套路径）。
    struct DrainGuard {
        ReadRegistry *reg;
        DrainGuard() = delete;
        explicit DrainGuard(ReadRegistry *r) : reg(r) {}
        DrainGuard(const DrainGuard &) = delete;
        DrainGuard &operator=(const DrainGuard &) = delete;
        DrainGuard(DrainGuard &&) = delete;
        DrainGuard &operator=(DrainGuard &&) = delete;
        ~DrainGuard() {
            const std::scoped_lock lock{reg->mutex};
            --reg->drain_depth;
        }
    } drain_guard{&registry};
    // 先兑现读取、后投递终态：本轮内完成「Move 确认早于 on_end」的顺序。
    for (auto &entry : due) {
        const bool ready = read_data_ready(entry);
        entry.callback(fulfill_read(entry, ready, now));
    }
    deliver_session_end();
}

auto system_drag::install_test_backend() -> bool {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    {
        auto &backend = system_drag_test_backend();
        const std::scoped_lock lock{backend.mutex};
        backend.active = true;
        backend.withheld.clear();
        backend.last_offer.reset();
    }
    {
        // 安装即全量复位：会话、在途读取与错误记录回到初态（在途项直接摘除，不兑现）。
        auto &session = drag_session();
        const std::scoped_lock lock{session.mutex};
        session.active = false;
        session.offer = DragOffer{};
        session.on_end = nullptr;
        session.end_queued = false;
        session.last_error.reset();
    }
    {
        auto &registry = read_registry();
        const std::scoped_lock lock{registry.mutex};
        registry.pending.clear();
        registry.timeout = AURORA_SYSTEM_DRAG_READ_TIMEOUT;
    }
    return true;
#else
    return false;
#endif
}

auto system_drag::reset_test_backend() -> void {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    {
        auto &backend = system_drag_test_backend();
        const std::scoped_lock lock{backend.mutex};
        backend.active = false;
        backend.withheld.clear();
        backend.last_offer.reset();
    }
#endif
    {
        auto &session = drag_session();
        const std::scoped_lock lock{session.mutex};
        session.active = false;
        session.offer = DragOffer{};
        session.on_end = nullptr;
        session.end_queued = false;
        session.last_error.reset();
    }
    {
        auto &registry = read_registry();
        const std::scoped_lock lock{registry.mutex};
        registry.pending.clear();  // 在途项直接摘除（不兑现）：测试边界复位用
        registry.timeout = AURORA_SYSTEM_DRAG_READ_TIMEOUT;
    }
}

auto system_drag::remove_test_backend() -> bool {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    bool was_active = false;
    {
        auto &backend = system_drag_test_backend();
        const std::scoped_lock lock{backend.mutex};
        was_active = backend.active;
        backend.active = false;
        backend.withheld.clear();
        backend.last_offer.reset();
    }
    {
        // 卸载即回到「无后端」语义：在途会话与在途读取一并摘除（不兑现）。
        auto &session = drag_session();
        const std::scoped_lock lock{session.mutex};
        session.active = false;
        session.offer = DragOffer{};
        session.on_end = nullptr;
        session.end_queued = false;
        session.last_error.reset();
    }
    {
        auto &registry = read_registry();
        const std::scoped_lock lock{registry.mutex};
        registry.pending.clear();
        registry.timeout = AURORA_SYSTEM_DRAG_READ_TIMEOUT;
    }
    return was_active;
#else
    return false;
#endif
}

auto system_drag::emit_session_end(SystemDragOutcome outcome, DragAction action) -> void {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    complete_session(outcome, action);  // 登记终态；经下一拍 pump_reads() 投递 on_end
#else
    (void)outcome;
    (void)action;
#endif
}

auto system_drag::test_withhold_format(const std::string &mime) -> void {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &backend = system_drag_test_backend();
    const std::scoped_lock lock{backend.mutex};
    backend.withheld.insert(mime);
#else
    (void)mime;
#endif
}

auto system_drag::test_set_read_timeout(std::chrono::milliseconds timeout) -> void {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &registry = read_registry();
    const std::scoped_lock lock{registry.mutex};
    registry.timeout = (timeout <= std::chrono::milliseconds::zero()) ? AURORA_SYSTEM_DRAG_READ_TIMEOUT : timeout;
#else
    (void)timeout;
#endif
}

auto system_drag::test_in_drain_phase() -> bool {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &registry = read_registry();
    const std::scoped_lock lock{registry.mutex};
    return registry.drain_depth > 0;
#else
    return false;
#endif
}

auto system_drag::test_last_session_error() -> std::optional<Error> {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &session = drag_session();
    const std::scoped_lock lock{session.mutex};
    return session.last_error;
#else
    return std::nullopt;
#endif
}

auto system_drag::test_last_offer() -> std::optional<DragOffer> {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &backend = system_drag_test_backend();
    const std::scoped_lock lock{backend.mutex};
    return backend.last_offer;
#else
    return std::nullopt;
#endif
}

#undef AURORA_SYSTEM_DRAG_TEST_BACKEND

}  // namespace aurora
