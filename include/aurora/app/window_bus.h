#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "aurora/state/subscription.h"
#include "aurora/window/window.h"

namespace aurora {

/**
 * @brief 跨窗口事件总线：类型化、主线程同步扇出的广播 / 点对点通道（多窗口协同）。
 *
 * 用途定位（与既有原语的分工）：
 * - **共享状态** → 直接用既有 `Store<S>`（Redux 式）：多个窗口共享同一 `Store` 实例即可，
 *   各自用 `au::connect(store, ...)` 订阅并在 `asSignal()` 上绑定控件，无需本类。
 * - **一次性通知 / 命令** → 本类：主窗口「打开某个文件」、辅助窗口「选中项变更」这类
 *   无状态归属的动作，用 `post()` 广播、按类型 `on()` 订阅。
 *
 * 语义约束：
 * - **主线程同步扇出**：`post()` 立即在当前调用栈内依次调用订阅者（与 `Application` 的
 *   同步事件派发模型一致，见 ARCHITECTURE.md §3.1）；不排队、不跨线程。
 * - 遍历前会拷贝订阅列表：订阅者可在回调内安全地新增 / 取消订阅（含自取消）。
 * - 订阅句柄 `Subscription` 内部持有共享状态（非裸 `this`），**总线先析构、句柄后析构**也安全。
 *
 * @code
 *   auto sub = app.bus().on<OpenFileRequested>([&](const OpenFileRequested &e, WindowId from) {
 *       AURORA_LOG_INFO("app", "open ", e.path, " from window ", from);
 *   });
 *   app.bus().post(OpenFileRequested{.path = "a.txt"}, sender_id);
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class WindowEventBus {
  public:
    using Token = std::uint64_t;

    WindowEventBus() : state_(std::make_shared<State>()) {}
    ~WindowEventBus() = default;
    WindowEventBus(const WindowEventBus &) = delete;
    auto operator=(const WindowEventBus &) -> WindowEventBus & = delete;
    WindowEventBus(WindowEventBus &&) = delete;
    auto operator=(WindowEventBus &&) -> WindowEventBus & = delete;

    /// @brief 订阅类型 `T` 的事件。
    /// @param cb 回调 `(payload, from)`；`from` 为发布者窗口 id（发布时未指定则 `kInvalidWindowId`）。
    /// @param filter_from 只接收来自该窗口的事件；`kInvalidWindowId`（默认）= 接收全部。
    /// @return RAII 订阅句柄：析构自动取消，可 `release()` 转为手动管理。
    template <typename T>
    [[nodiscard]] auto on(std::function<void(const T &, WindowId)> cb, WindowId filter_from = kInvalidWindowId)
        -> Subscription {
        const Token token = state_->next_token++;
        Entry entry;
        entry.token = token;
        entry.filter_from = filter_from;
        entry.invoke = [cb = std::move(cb)](const void *payload, WindowId from) -> void {
            cb(*static_cast<const T *>(payload), from);
        };
        state_->subs[std::type_index(typeid(T))].push_back(std::move(entry));
        const std::shared_ptr<State> st = state_;  // 捕获共享状态：总线先析构也安全
        return Subscription([st, token]() -> void { erase_token(*st, token); });
    }

    /// @brief 发布类型 `T` 的事件：同步扇出给全部匹配订阅者。
    /// @param from 发布者窗口 id（可选；订阅侧可据此点对点过滤）。
    template <typename T>
    auto post(const T &payload, WindowId from = kInvalidWindowId) -> void {
        const auto it = state_->subs.find(std::type_index(typeid(T)));
        if (it == state_->subs.end()) {
            return;
        }
        // 拷贝订阅列表再遍历：回调内可能新订阅 / 取消订阅，直接遍历原容器会使迭代器失效。
        const std::vector<Entry> entries = it->second;
        for (const Entry &e : entries) {
            if (e.filter_from == kInvalidWindowId || e.filter_from == from) {
                e.invoke(&payload, from);
            }
        }
    }

    /// @brief 取消订阅（幂等）；`Subscription` 析构时已自动调用。
    auto off(Token token) -> void { erase_token(*state_, token); }

    /// @brief 取消全部订阅（窗口销毁 / 应用收尾时清理）。
    auto clear() -> void { state_->subs.clear(); }

    /// @brief 当前订阅者总数（诊断 / 测试用）。
    [[nodiscard]] auto subscriber_count() const -> std::size_t {
        std::size_t n = 0;
        for (const auto &kv : state_->subs) {
            n += kv.second.size();
        }
        return n;
    }

  private:
    struct Entry {
        Token token = 0;
        WindowId filter_from = kInvalidWindowId;
        std::function<void(const void *, WindowId)> invoke;
    };
    struct State {
        std::unordered_map<std::type_index, std::vector<Entry>> subs;
        Token next_token = 1;
    };

    /// @brief 按 token 摘除订阅（跨全部类型）；不存在时为空操作。
    static auto erase_token(State &st, Token token) -> void;

    std::shared_ptr<State> state_;
};

}  // namespace aurora
