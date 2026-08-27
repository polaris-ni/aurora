#pragma once

#include <functional>
#include <map>
#include <utility>

namespace aurora {

/**
 * @brief 值类型事件流（规格 §2.5）。对外暴露 `subscribe` / `emit`，以响应式方式订阅 UI 事件。
 *
 * 单线程 UI 假设（与库一致），故未加锁。`subscribe` 返回 `Subscription`，其析构自动退订。
 */
template<typename T> class EventStream {
    using Fn = std::function<void(const T &)>;

  public:
    /// @brief 订阅句柄；析构时自动从流中退订。
    class Subscription {
      public:
        Subscription() = default;
        Subscription(std::size_t id, EventStream *host) : m_id(id), m_host(host) {}
        ~Subscription() { reset(); }

        auto reset() -> void {
            if (m_host != nullptr) {
                m_host->unsubscribe(m_id);
            }
            m_host = nullptr;
        }
        [[nodiscard]] explicit operator bool() const { return m_host != nullptr; }

      private:
        std::size_t m_id = 0;
        EventStream *m_host = nullptr;
    };

    /// @brief 订阅事件；返回订阅句柄（RAII 退订）。
    [[nodiscard]] auto subscribe(Fn cb) -> Subscription {
        const std::size_t id = ++m_next_id;
        m_map[id] = std::move(cb);
        return Subscription(id, this);
    }

    /// @brief 发射一个事件值给所有订阅者。
    auto emit(const T &v) -> void {
        for (auto &kv : m_map) {
            kv.second(v);
        }
    }

    auto unsubscribe(std::size_t id) -> void { m_map.erase(id); }

  private:
    std::map<std::size_t, Fn> m_map;
    std::size_t m_next_id = 0;
};

} // namespace aurora
