#pragma once

#include <functional>
#include <map>
#include <utility>

namespace aurora {

/**
 * @brief 值类型事件流（specification/01-core.md §7）。对外暴露 `subscribe` / `emit`，以响应式方式订阅 UI 事件。
 *
 * 单线程 UI 假设（与库一致），故未加锁。`subscribe` 返回 `Subscription`，其析构自动退订。
 */
template <typename T>
class EventStream {
    using Fn = std::function<void(const T &)>;

  public:
    /// @brief 订阅句柄；析构时自动从流中退订。
    class Subscription {
      public:
        Subscription() = default;
        Subscription(std::size_t id, EventStream *host) : id_(id), host_(host) {}
        ~Subscription() { reset(); }

        Subscription(const Subscription &) = delete;
        auto operator=(const Subscription &) -> Subscription & = delete;

        Subscription(Subscription &&o) noexcept : id_(o.id_), host_(o.host_) { o.host_ = nullptr; }
        auto operator=(Subscription &&o) noexcept -> Subscription & {
            if (this != &o) {
                reset();
                id_ = o.id_;
                host_ = o.host_;
                o.host_ = nullptr;
            }
            return *this;
        }

        auto reset() -> void {
            if (host_ != nullptr) {
                host_->unsubscribe(id_);
            }
            host_ = nullptr;
        }
        [[nodiscard]] explicit operator bool() const { return host_ != nullptr; }

      private:
        std::size_t id_ = 0;
        EventStream *host_ = nullptr;
    };

    /// @brief 订阅事件；返回订阅句柄（RAII 退订）。
    [[nodiscard]] auto subscribe(Fn cb) -> Subscription {
        const std::size_t id = ++next_id_;
        map_[id] = std::move(cb);
        return Subscription(id, this);
    }

    /// @brief 发射一个事件值给所有订阅者。
    auto emit(const T &v) -> void {
        for (auto &kv : map_) {
            kv.second(v);
        }
    }

    auto unsubscribe(std::size_t id) -> void { map_.erase(id); }

  private:
    std::map<std::size_t, Fn> map_;
    std::size_t next_id_ = 0;
};

}  // namespace aurora
