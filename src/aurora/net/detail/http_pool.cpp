#include "http_pool.h"

#include <algorithm>
#include <utility>

namespace aurora::net::detail {

auto HttpConnectionPool::sweep_expired(const HttpPoolKey &key, const http::PoolOptions &options,
                                       std::chrono::steady_clock::time_point now) -> void {
    // 淘汰三算：配置代次不匹配（对照调用方**当前**的代次——换代之后所有旧代条目对本次都算过期）、
    // 空闲超时、最长寿命。阈值非正值与「不淘汰」同义（与 `Timeouts` 各档同口径）。
    const bool idle_expiry = options.idle_timeout.count() > 0;
    const bool age_expiry = options.max_age.count() > 0;
    idle_.erase(std::remove_if(idle_.begin(), idle_.end(),
                               [&](IdleEntry &entry) {
                                   bool expired = entry.key.tls_generation != key.tls_generation;
                                   if (idle_expiry && now - entry.idle_since >= options.idle_timeout) {
                                       expired = true;
                                   }
                                   if (age_expiry && now - entry.born >= options.max_age) {
                                       expired = true;
                                   }
                                   if (expired) {
                                       // 计数随之让出：这条连接被关掉了（shared_ptr 析构即关流）。
                                       auto slot = live_.find(entry.key);
                                       if (slot != live_.end() && slot->second > 0) {
                                           --slot->second;
                                       }
                                   }
                                   return expired;
                               }),
              idle_.end());
}

auto HttpConnectionPool::acquire(const HttpPoolKey &key, const http::PoolOptions &options,
                                 std::chrono::steady_clock::time_point now) -> HttpPoolAcquired {
    const std::scoped_lock guard{mutex_};
    sweep_expired(key, options, now);
    // 命中即取走：空闲表里键全等（含代次）的最旧一条（FIFO——空闲超时口径对它最不利）。
    for (auto it = idle_.begin(); it != idle_.end(); ++it) {
        if (it->key == key) {
            HttpPoolAcquired out;
            out.verdict = HttpPoolAcquire::Ready;
            out.connection = std::move(it->connection);
            out.born = it->born;
            idle_.erase(it);
            return out;
        }
    }
    const bool unlimited = options.max_connections_per_host == 0;
    const auto slot = live_.find(key);
    const std::size_t alive = slot != live_.end() ? slot->second : 0;
    if (unlimited || alive < options.max_connections_per_host) {
        ++live_[key];  // 名额在建连之前就占住：并发交换不能同时越过上限
        HttpPoolAcquired out;
        out.verdict = HttpPoolAcquire::OpenNew;
        out.born = now;  // 新建连接的出生点就是此刻（握手耗时忽略：寿命是分钟级口径）
        return out;
    }
    return HttpPoolAcquired{};  // Queue
}

auto HttpConnectionPool::release(const HttpPoolKey &key, std::optional<HttpPoolConnection> connection,
                                 std::chrono::steady_clock::time_point born, const http::PoolOptions &options,
                                 std::chrono::steady_clock::time_point now) -> void {
    const std::scoped_lock guard{mutex_};
    sweep_expired(key, options, now);
    if (!connection.has_value() || connection->empty()) {
        auto slot = live_.find(key);
        if (slot != live_.end() && slot->second > 0) {
            --slot->second;  // 连接已死或不干净：关掉（析构即关流）、让出名额
        }
        return;
    }
    idle_.push_back(IdleEntry{.key = key,
                              .connection = std::move(*connection),
                              .born = born,
                              .idle_since = now});
}

auto HttpConnectionPool::clear() -> void {
    const std::scoped_lock guard{mutex_};
    idle_.clear();  // 空闲条目的 shared_ptr 在此析构，连接随之关闭
    live_.clear();
}

}  // namespace aurora::net::detail
