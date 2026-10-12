// Aurora net — 取消令牌实现（CHG-007 底座切片）。
//
// 两条实现约束值得写在文件头：
//   - **钩子必在锁外调用**：登记钩子的正是引擎，引擎在钩子里放行挂起点、释放句柄，甚至就地
//     重入 `cancel()`；持锁回调会把「取消 → 放行 → 再取消」这条正常路径变成死锁。
//   - **后置登记必触发**：`on_cancel()` 撞上「已取消」时在当前线程立即执行钩子，否则
//     「取消先发生、钩子后登记」的窄竞态会让挂起点永不放行——这类缺陷只在真实网络延迟下现形。

#include "aurora/net/cancellation.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace aurora::net {

namespace detail {

/// @brief 取消共享状态：一个转换位 + 至多一枚待触发钩子。
///
/// 实现侧内部件，只经 `Cancellation` 暴露；放在 detail 以免公共头拉入 `<mutex>`。
class CancellationState {
  public:
    /// @brief 无锁读取消位。
    /// @return 已完成「未取消 → 已取消」转换时为 true。
    [[nodiscard]] auto is_cancelled() const -> bool { return cancelled_.load(std::memory_order_acquire); }

    /// @brief 尝试取消：首次转换取出并执行钩子，重复调用无副作用。
    auto cancel() -> void {
        std::function<void()> hook;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if (cancelled_.load(std::memory_order_relaxed)) {
                return;
            }
            cancelled_.store(true, std::memory_order_release);
            hook = std::move(hook_);
            hook_ = nullptr;
        }
        if (hook) {
            hook();  // 锁外执行：钩子内可安全查询 is_cancelled() 或重入 cancel()
        }
    }

    /// @brief 登记钩子；已取消则立即在当前线程执行。
    /// @param hook 待登记可调用体；空值表示撤销登记。
    auto set_hook(std::function<void()> hook) -> void {
        if (!hook) {
            std::scoped_lock<std::mutex> lock(mutex_);
            hook_ = nullptr;
            return;
        }
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if (!cancelled_.load(std::memory_order_relaxed)) {
                hook_ = std::move(hook);
                return;
            }
        }
        hook();  // 取消先发生：不让钩子白登记一次
    }

  private:
    mutable std::mutex mutex_;  ///< 保护 hook_，并为取消位转换提供串行点
    std::atomic<bool> cancelled_{false};  ///< 取消位（acquire / release 语义对外可见）
    std::function<void()> hook_;  ///< 待触发钩子（至多一次，取出即清空）
};

}  // namespace detail

Cancellation::Cancellation() : state_(std::make_shared<detail::CancellationState>()) {}

Cancellation::~Cancellation() = default;

auto Cancellation::is_cancelled() const -> bool { return state_->is_cancelled(); }

auto Cancellation::cancel() -> void { state_->cancel(); }

auto Cancellation::on_cancel(std::function<void()> hook) -> void { state_->set_hook(std::move(hook)); }

auto Cancellation::shares_state_with(const Cancellation &other) const -> bool { return state_ == other.state_; }

}  // namespace aurora::net
