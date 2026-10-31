#pragma once

// ============================================================
// 无头用例的值班级主循环替身（tests/framework/main_queue_pump.h）
// ------------------------------------------------------------
// 自此，`async()`/`Task::then()`/协程续体的用户完成一律经进程级唯一队列
// （`aurora::MainQueue`）回投主线程：发起须有人在**岗**（否则判纯编程错误硬失败），
// 落地须有人**排空**（否则跨线程迟到的完成无人接手）。真实宿主是 `Application` 帧循环；
// 无头单测没有它，就得自带替身。
//
// 分工：网络族用 `net_wait.h` 的 `MainQueueLoop`（就绪派发点在本线程按需排空，绝不起后台
// 线程——避免把协程 resume 打到别处与帧拆除相竞）。本头服务**promise/future 型完成用例**：
// 回调只是 `set_value`/计数，线程无关，后台排水线程把队列持续排空即可，用例只等有界条件。
//
// 三件替身，由松到紧：
//   - `wait_until`：有界轮询谓词（禁止无界阻塞），与队列无关，纯等待原语。
//   - `DutyOnly`：只领一枚空唤醒租约、不自排空——「已排进队列但尚未执行」的可控现场由用例
//     线程自行 `take_batch`（含 `pump_until` 成员：在本线程排空直到谓词成立）。
//   - `DutyPump`：领租约 + 起后台排水线程，令 promise/future 型完成用例无需手动排空即收尾。
//
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。
// ============================================================

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/main_queue.h"

namespace aurora::test::mq {

// @brief 有界轮询：每 1ms 轮询一次 `pred`，超时返回最后一次判定（禁止无界阻塞）。
// @tparam Pred 可调用体，返回可转 `bool` 的值。
// @param pred 目标条件；在轮询循环内可能被多次调用。
// @param budget 预算，缺省 2000ms。
// @return 预算内命中为 true。
template <typename Pred>
// pred 在轮询循环内可能被多次调用，不能按「一次性转发」用 std::forward（对带状态可调用体
// 转成右值引用会误移动，破坏后续再次调用），刻意始终以左值形式反复调用，故抑制该告警。
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
auto wait_until(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

/// @brief 在岗夹具（仅领空唤醒租约，不自排空）：令 `async()`/`co_async` 发起过闸、回投入进程级
///        全局队列，由用例线程自行 `take_batch` 排空——队列就是「已排进主线程但尚未执行」的可控现场。
struct DutyOnly {
    DutyOnly() = default;
    std::optional<aurora::MainQueue::Lease> duty{aurora::MainQueue::on_duty([]() -> void {})};
    DutyOnly(const DutyOnly &) = delete;
    auto operator=(const DutyOnly &) -> DutyOnly & = delete;
    DutyOnly(DutyOnly &&) = delete;
    auto operator=(DutyOnly &&) -> DutyOnly & = delete;
    ~DutyOnly() {
        if (duty.has_value()) {
            duty->release();
        }
    }

    // @brief 在本线程排空全局队列直到 `pred()` 成立（每轮 `take_batch` 后执行；空队列睡 1ms）。
    // @tparam Pred 可调用体，返回可转 `bool` 的值。
    // @param pred 终止判据。
    // @param budget 有界等待预算，缺省 3000ms。
    // @return 预算内命中为 true。
    template <typename Pred>
    // pred 反复以左值调用，不做一次性转发（对带状态可调用体转右值会误移动）。
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    auto pump_until(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds{3000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto &fn : aurora::MainQueue::take_batch()) {
                if (fn) {
                    fn();
                }
            }
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        for (auto &fn : aurora::MainQueue::take_batch()) {
            if (fn) {
                fn();
            }
        }
        return pred();
    }
};

/// @brief 在岗 + 后台排水夹具：持一枚空唤醒租约并起一条排水线程，持续把全局队列排空执行，
///        令 `promise/future` 型完成用例无需手动排空即可收尾（续体在排水线程执行，用例只等 future）。
/// @note 仅适用于回调线程无关（只 `set_value`/计数）的用例；触碰 State 或协程帧的续体改用
///       `DutyOnly` 由用例线程自排，或网络族用 `net_wait.h` 的 `MainQueueLoop`。
struct DutyPump {
    std::optional<aurora::MainQueue::Lease> duty{aurora::MainQueue::on_duty([]() -> void {})};
    std::atomic<bool> stop{false};
    std::thread pump;
    DutyPump()
        : pump([this]() -> void {
              while (!stop.load(std::memory_order_acquire)) {
                  for (auto &fn : aurora::MainQueue::take_batch()) {
                      if (fn) {
                          fn();
                      }
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds{1});
              }
          }) {}
    DutyPump(const DutyPump &) = delete;
    auto operator=(const DutyPump &) -> DutyPump & = delete;
    DutyPump(DutyPump &&) = delete;
    auto operator=(DutyPump &&) -> DutyPump & = delete;
    // 末次排空会执行已入队闭包（可能抛），与 `Application::loop_end` 同口径：收尾路径不外抛。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~DutyPump() {
        stop.store(true, std::memory_order_release);
        if (pump.joinable()) {
            pump.join();
        }
        for (auto &fn : aurora::MainQueue::take_batch()) {  // 末次收尾排空（stop 竞态窗内滞留项）
            if (fn) {
                fn();
            }
        }
        duty.reset();
    }
};

}  // namespace aurora::test::mq
