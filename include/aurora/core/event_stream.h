#pragma once

#include <functional>
#include <map>
#include <utility>

namespace aurora {

/// @brief 值类型事件流（specification/01-core.md §7）。对外暴露 `subscribe` / `emit`，以响应式方式订阅 UI 事件。
/// @tparam T 事件载荷类型，订阅回调以 const 引用收到该值。
/// @note 单线程 UI 假设（与库一致），故未加锁。`subscribe` 返回 `Subscription`，其析构自动退订。
template <typename T>
class EventStream {
    using Fn = std::function<void(const T &)>;

  public:
    /// @brief 订阅句柄；析构时自动从流中退订。
    class Subscription {
      public:
        /// @brief 默认构造即无效句柄（host_ 为空，析构/复位均空操作）。
        Subscription() = default;
        /// @brief 绑定流构造订阅句柄（由 subscribe 调用）。
        /// @param id 本次订阅在流内的键。
        /// @param host 析构时负责退订的宿主流。
        Subscription(std::size_t id, EventStream *host) : id_(id), host_(host) {}
        /// @brief 析构时自动从宿主流退订（等价 reset()）。
        ~Subscription() { reset(); }

        /// @brief 禁止拷贝：订阅身份唯一，句柄不可复制。
        Subscription(const Subscription &) = delete;
        /// @brief 禁止拷贝赋值（同拷贝构造）。
        /// @return 无返回值语义（恒 delete）。
        auto operator=(const Subscription &) -> Subscription & = delete;

        /// @brief 移动构造：接管源句柄的订阅，源置无效（不再退订）。
        /// @param o 源句柄（移动后 host_ 为空）。
        Subscription(Subscription &&o) noexcept : id_(o.id_), host_(o.host_) { o.host_ = nullptr; }
        /// @brief 移动赋值：先退订旧订阅再接管源的；自赋值短路。
        /// @param o 源句柄（接管后源置无效）。
        /// @return 本句柄引用。
        auto operator=(Subscription &&o) noexcept -> Subscription & {
            if (this != &o) {
                reset();
                id_ = o.id_;
                host_ = o.host_;
                o.host_ = nullptr;
            }
            return *this;
        }

        /// @brief 主动退订并失效本句柄（幂等：已失效时为空操作）。
        auto reset() -> void {
            if (host_ != nullptr) {
                host_->unsubscribe(id_);
            }
            host_ = nullptr;
        }
        /// @brief 布尔语境：句柄是否仍持有有效订阅。
        /// @return host_ 非空（未退订）时为 true。
        [[nodiscard]] explicit operator bool() const { return host_ != nullptr; }

      private:
        std::size_t id_ = 0;
        EventStream *host_ = nullptr;
    };

    /// @brief 订阅事件；返回订阅句柄（RAII 退订）。
    /// @param cb 回调（move 进 map_，emit 时对每个订阅者调用）。
    /// @return 绑定本流的 Subscription 句柄；其析构自动退订。
    [[nodiscard]] auto subscribe(Fn cb) -> Subscription {
        const std::size_t id = ++next_id_;
        map_[id] = std::move(cb);
        return Subscription(id, this);
    }

    /// @brief 发射一个事件值给所有订阅者。
    /// @param v 事件值（按 const 引用转发给各回调）。
    auto emit(const T &v) -> void {
        for (auto &kv : map_) {
            kv.second(v);
        }
    }

    /// @brief 按 id 退订（Subscription 析构 / reset 的内部通道；幂等）。
    /// @param id 订阅句柄持有的序号键；不存在时 erase 为空操作。
    auto unsubscribe(std::size_t id) -> void { map_.erase(id); }

  private:
    std::map<std::size_t, Fn> map_;
    std::size_t next_id_ = 0;
};

}  // namespace aurora
