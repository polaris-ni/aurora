// ============================================================================
// storage.cpp — 存储门面实现（用户唯一直接持有的句柄）
// ----------------------------------------------------------------------------
// 对标 Application 持有 Surface：门面负责信封封装、类型化路由、异步卸载到线程池、
// 变更通知与默认实例。后端经 `create` 注入。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include "aurora/storage/storage.h"

#include <algorithm>
#include <chrono>
#include <memory>

#include "aurora/core/result.h"
#include "aurora/preferences/preferences.h"
#include "aurora/storage/fs_backend.h"
#include "aurora/storage/memory_backend.h"

namespace aurora::storage {

namespace {
constexpr auto AURORA_RAW_TYPE = "__raw__";

auto now_tp() -> std::chrono::system_clock::time_point { return std::chrono::system_clock::now(); }

// 进程级默认实例槽位（懒构造）。
auto default_slot() -> auto & {
    static std::unique_ptr<Storage> s;
    return s;
}
} // namespace

// ---------- 工厂 ----------

auto Storage::create(FilesystemOptions opts) -> Result<Storage> {
    auto be = std::make_unique<FilesystemBackend>(std::move(opts));
    if (!be->is_open()) {
        return Result<Storage>{ make_error(
            ErrorCode::StorageBackendUnavailable,
            "Default filesystem storage open failed: directory not writable or lock acquisition failed") };
    }
    return Result{ Storage(std::move(be)) };
}

auto Storage::create(std::unique_ptr<StorageBackend> backend) -> Storage { return Storage(std::move(backend)); }

// ---------- 原始 JSON 记录 API ----------

auto Storage::put(const std::string &id, const Json &value) const -> Result<void> {
    StorageRecord rec;
    rec.id = id;
    rec.type = AURORA_RAW_TYPE;
    rec.version = 1;
    rec.encoding = StorageEncoding::Json;
    rec.payload = value;
    rec.mtime = now_tp();
    auto r = put_record(id, rec);
    return r;
}

auto Storage::get(const std::string &id) const -> Result<Json> {
    auto rec = m_backend->get_record(id);
    if (!rec) {
        return Result<Json>{ rec.error() };
    }
    if (rec.value().encoding != StorageEncoding::Json) {
        return Result<Json>{ make_error(ErrorCode::StorageEncodingMismatch,
                                        "Record stored in binary, cannot read via JSON channel: " + id) };
    }
    return Result{ std::get<Json>(rec.value().payload) };
}

auto Storage::remove(const std::string &id) const -> Result<void> {
    auto r = m_backend->remove(id);
    if (r) {
        emit_change({ .op = StorageChange::Operation::Remove, .id = id });
    }
    return r;
}

auto Storage::list() const -> Result<std::vector<std::string>> { return m_backend->list(); }

auto Storage::contains(const std::string &id) const -> Result<bool> { return m_backend->contains(id); }

auto Storage::clear() const -> Result<void> {
    auto r = m_backend->clear();
    if (r) {
        emit_change({ .op = StorageChange::Operation::Clear, .id = "" });
    }
    return r;
}

auto Storage::flush() const -> Result<void> { return m_backend->flush(); }

// ---------- 二进制载荷 API ----------

auto Storage::put(const std::string &id, const StorageBytes &value) const -> Result<void> {
    StorageRecord rec;
    rec.id = id;
    rec.type = AURORA_RAW_TYPE;
    rec.version = 1;
    rec.encoding = StorageEncoding::Binary;
    rec.payload = value;
    rec.mtime = now_tp();
    return put_record(id, rec);
}

auto Storage::get_bytes(const std::string &id) const -> Result<StorageBytes> {
    auto rec = m_backend->get_record(id);
    if (!rec) {
        return Result<StorageBytes>{ rec.error() };
    }
    if (rec.value().encoding != StorageEncoding::Binary) {
        return Result<StorageBytes>{ make_error(ErrorCode::StorageEncodingMismatch,
                                                "Record stored as JSON, cannot read via binary channel: " + id) };
    }
    return Result{ std::get<StorageBytes>(rec.value().payload) };
}

auto Storage::get_value(const std::string &id) const -> Result<StorageValue> {
    auto rec = m_backend->get_record(id);
    if (!rec) {
        return Result<StorageValue>{ rec.error() };
    }
    return Result{ rec.value().payload };
}

// ---------- 信封级 API ----------

auto Storage::put_record(const std::string &id, const StorageRecord &rec) const -> Result<void> {
    auto r = m_backend->put_record(id, rec);
    if (r) {
        emit_change({ .op = StorageChange::Operation::Put, .id = id });
    }
    return r;
}

auto Storage::get_record(const std::string &id) const -> Result<StorageRecord> { return m_backend->get_record(id); }

// ---------- 异步 API ----------

auto Storage::async_put(const std::string &id, const Json &value) const -> Task<void> {
    StorageRecord rec;
    rec.id = id;
    rec.type = AURORA_RAW_TYPE;
    rec.version = 1;
    rec.encoding = StorageEncoding::Json;
    rec.payload = value;
    rec.mtime = now_tp();
    auto *be = m_backend.get();
    const std::string &idc = id;
    auto task = async([be, idc, rec]() -> Result<void> { return be->put_record(idc, rec); });
    task.then([this, idc](const Result<void> &r) -> void {
        if (r.ok()) {
            emit_change({ .op = StorageChange::Operation::Put, .id = idc });
        }
    });
    return task;
}

auto Storage::async_get(const std::string &id) const -> Task<Json> {
    auto *be = m_backend.get();
    const std::string &idc = id;
    return async([be, idc]() -> Result<Json> {
        auto rec = be->get_record(idc);
        if (!rec) {
            return Result<Json>{ rec.error() };
        }
        if (rec.value().encoding != StorageEncoding::Json) {
            return Result<Json>{ make_error(ErrorCode::StorageEncodingMismatch,
                                            "Record stored in binary, cannot read via JSON channel: " + idc) };
        }
        return Result{ std::get<Json>(rec.value().payload) };
    });
}

auto Storage::async_put(const std::string &id, const StorageBytes &value) const -> Task<void> {
    StorageRecord rec;
    rec.id = id;
    rec.type = AURORA_RAW_TYPE;
    rec.version = 1;
    rec.encoding = StorageEncoding::Binary;
    rec.payload = value;
    rec.mtime = now_tp();
    auto *be = m_backend.get();
    const std::string &idc = id;
    auto task = async([be, idc, rec]() -> Result<void> { return be->put_record(idc, rec); });
    task.then([this, idc](const Result<void> &r) -> void {
        if (r.ok()) {
            emit_change({ .op = StorageChange::Operation::Put, .id = idc });
        }
    });
    return task;
}

auto Storage::async_get_value(const std::string &id) const -> Task<StorageValue> {
    auto *be = m_backend.get();
    const std::string &idc = id;
    return async([be, idc]() -> Result<StorageValue> {
        auto rec = be->get_record(idc);
        if (!rec) {
            return Result<StorageValue>{ rec.error() };
        }
        return Result<StorageValue>{ rec.value().payload };
    });
}

auto Storage::async_remove(const std::string &id) const -> Task<void> {
    auto *be = m_backend.get();
    const std::string &idc = id;
    auto task = async([be, idc]() -> Result<void> { return be->remove(idc); });
    task.then([this, idc](const Result<void> &r) -> void {
        if (r.ok()) {
            emit_change({ .op = StorageChange::Operation::Remove, .id = idc });
        }
    });
    return task;
}

auto Storage::async_list() const -> Task<std::vector<std::string>> {
    auto *be = m_backend.get();
    return async([be]() -> Result<std::vector<std::string>> { return be->list(); });
}

// ---------- 跨记录事务 ----------

auto Storage::transaction(std::function<Result<void>(Storage &)> body) -> Result<void> {
    // 抑制逐操作通知。RAII 守卫保证 body 抛异常时标志必然复位——否则此后所有
    // put/remove 的变更通知会被永久静默（此前直接置位/复位即存在此缺陷）。
    // 标志读写均经 m_listener_mutex：异步回调可能在 worker 线程并发发射变更。
    struct NotifySuppressGuard {
        Storage *s;
        explicit NotifySuppressGuard(Storage *storage) : s(storage) {
            std::scoped_lock lock(*s->m_listener_mutex);
            s->m_notify_suppressed = true;
        }
        ~NotifySuppressGuard() {
            std::scoped_lock lock(*s->m_listener_mutex);
            s->m_notify_suppressed = false;
        }
        NotifySuppressGuard(const NotifySuppressGuard &) = delete;
        auto operator=(const NotifySuppressGuard &) -> NotifySuppressGuard & = delete;
        NotifySuppressGuard(NotifySuppressGuard &&) = delete;
        auto operator=(NotifySuppressGuard &&) -> NotifySuppressGuard & = delete;
    } guard(this);
    auto r = m_backend->transaction([this, &body](StorageBackend &) -> Result<void> { return body(*this); });
    if (r) {
        emit_change({ .op = StorageChange::Operation::Batch, .id = "" });
    }
    return r;
}

// ---------- 响应式变更通知 ----------

auto Storage::on_change(StorageChangeCallback cb) -> Subscription {
    std::uint64_t id = 0;
    {
        std::scoped_lock lock(*m_listener_mutex);
        id = m_listener_seq++;
        m_listeners.push_back({ .id = id, .cb = std::move(cb) });
    }
    auto *self = this;
    return Subscription([self, id]() -> void {
        std::scoped_lock lock(*self->m_listener_mutex);
        const auto it = std::ranges::find_if(self->m_listeners, [id](const Listener &l) -> bool { return l.id == id; });
        if (it != self->m_listeners.end()) {
            self->m_listeners.erase(it);
        }
    });
}

void Storage::emit_change(const StorageChange &ch) const {
    std::vector<StorageChangeCallback> cbs;
    {
        std::scoped_lock lock(*m_listener_mutex);
        if (m_notify_suppressed) { // 事务抑制检查在锁内：与 transaction 的置位/复位互斥
            return;
        }
        cbs.reserve(m_listeners.size());
        for (const auto &l : m_listeners) {
            cbs.push_back(l.cb);
        }
    }
    for (const auto &cb : cbs) {
        cb(ch);
    }
}

// ---------- 进程级默认实例 ----------

auto Storage::set_default(Storage s) -> void { default_slot() = std::make_unique<Storage>(std::move(s)); }

auto Storage::default_instance() -> Storage & {
    auto &slot = default_slot();
    if (!slot) {
        auto r = create();
        if (r) {
            slot = std::make_unique<Storage>(std::move(r.value()));
        } else {
            // 无默认文件系统介质时不 fatal：退化为内存存储，保证 default_instance() 永远可用。
            slot = std::make_unique<Storage>(create(std::make_unique<MemoryBackend>()));
        }
    }
    return *slot;
}

} // namespace aurora::storage
