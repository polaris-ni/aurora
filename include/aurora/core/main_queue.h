#pragma once

// ============================================================================
// main_queue.h — 进程级唯一跨线程投递队列（specification/01-core.md §8.2）。
// ----------------------------------------------------------------------------
// 两半事实物理分离，各自只有一个读法：
//   - 队列：恒存在、归属唯一——「投进谁家队列」不再是问题；
//   - 值班：同一时刻至多一个循环登记（CAS 拒并发 run），决定投递成败与叫谁起床。
// 装/卸仅经 `on_duty` 的 RAII 租约进行；其余一切层只会 post / try_post / take_batch。
//
// 失败分两相（本机制的中枢）：
//   - 发起时（`async()`/`then()`，见 state/async.h）无值班 = 编程错误 → 硬失败；
//   - 落地时（本头的 post/try_post）无值班或世代失配 = 合法生命周期事实（打烊取消）
//     → 不投递、不 terminate，记 ERROR 级诊断与计数。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/log.h"
#include "aurora/core/strict_mode.h"

namespace aurora {

/// @brief 进程级唯一主线程投递队列（全静态纯门面；对标 `ThreadPool` 的宿主单例形态）。
///
/// 队列、值班位、世代计数、唤醒通道同址同锁：值班判定与入队在一把锁内完成，
/// 不存在「还租约后落孤儿项」的竞态。`core` 不认识 `Application`——唤醒方式由上岗者
/// 经 `on_duty` 注入，本类型只负责「叫谁起床」，不决定任务归属。
///
/// @note Thread: thread-safe（所有静态方法可任意线程调用）
/// @note Side-effects: none（不执行任务，仅排队与唤醒；执行方是 `take_batch` 的调用线程）
/// @note Rebuildable: yes（进程级单例，无实例状态）
class MainQueue {
  public:
    /// @brief 值班租约（RAII 值对象）：独占队列消费权的凭证，不可拷贝、可移动。
    ///
    /// 存在的意义是把「谁能改全局值班状态」从调用纪律变成所有权事实：
    ///  - `release()` 只认自己领回的那张（内部比对世代号），**结构性封死「冒卸他人值班」**——
    ///    旧机制 `loop_end` 无条件置空可打死外层循环的缺陷在类型层不可能再发生；
    ///  - 析构兜底：上岗者抛异常离开作用域时自动卸任，值班位不会永久泄漏。
    /// 租约归还后槽位可再次领取（`on_duty` 只拒「此刻有人」，不拒重入循环复用）。
    /// @note Thread: thread-safe
    /// @note Side-effects: none（仅登记位的领取与归还）
    /// @note Rebuildable: yes
    class Lease {
      public:
        Lease(const Lease &) = delete;
        auto operator=(const Lease &) -> Lease & = delete;
        /// @brief 移动构造：凭证易主，源即刻失去卸任资格（防双份凭证）。
        /// @param other 移动源（其后 `epoch_` 置 0）。
        Lease(Lease &&other) noexcept : epoch_(other.epoch_) { other.epoch_ = 0; }
        /// @brief 移动赋值：先卸现任（若在手），再接管来源凭证。
        /// @param other 移动源。
        /// @return 自身引用。
        auto operator=(Lease &&other) noexcept -> Lease & {
            if (this != &other) {
                release();
                epoch_ = other.epoch_;
                other.epoch_ = 0;
            }
            return *this;
        }
        /// @brief 析构即卸任（在手才生效；`release()` 已做幂等）。
        ~Lease() { release(); }

        /// @brief 显式卸任：仅当在岗世代号与自己的凭证一致才清空登记（幂等，无凭证时 no-op）。
        ///
        /// `noexcept` 是结构性要求：本函数是**全部**卸任路径的共同终点（`loop_end`、租约析构、
        /// 值班级替身），而互斥量加锁与首次惰性构造都可能抛出；让它在栈解退中抛出等于把「还值」
        /// 变成 `terminate`。系统级失败时本地凭证照样失效，不再外抛。
        auto release() noexcept -> void {
            if (epoch_ == 0) {
                return;  // 从未持有或已卸任：零锁开销
            }
            try {
                std::scoped_lock lk(state().mtx);
                if (state().epoch == epoch_) {
                    state().epoch = 0;
                    state().wake = nullptr;  // 唤醒通道随登记一并摘除
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // 加锁或首次构造的系统级失败不可恢复：收尾路径绝不外抛，本地凭证照样失效
                // （全局位由持证方各自的世代号比对兜底，不会让下一任上岗永久领不到）。
                // 此处的静默是本体的设计意图（不记录、不再抛），不是漏处理。
            }
            epoch_ = 0;
        }

      private:
        friend class MainQueue;
        /// @brief 由 `on_duty` 以领到的世代号构造（对外不可凭空持有凭证）。
        /// @param epoch 领取时分配到的值班世代号（≥1）。
        explicit Lease(std::uint64_t epoch) : epoch_(epoch) {}
        std::uint64_t epoch_ = 0;  ///< 在手凭证的世代号；0 = 未持有
    };

    /// @brief 打烊作用域守卫：`loop_end` 收尾排空的专属窗口标记（RAII，析构即关闭）。
    ///
    /// 窗口内 `current_epoch()==0` 且 `is_closing()==true`：发起侧据此走「出生即取消」
    /// （合法竞态：收尾排水中的续体链式再发起异步，正常关窗不得被打成崩溃），
    /// 守卫一析构，无值班发起即回到「编程错误硬失败」。**`closing` 有界是 fail-fast 的
    /// 边界**——绝不延到下次上岗才清，否则循环早已结束后的错误发起会被静默豁免。
    /// @note Thread: thread-safe
    /// @note Side-effects: none
    /// @note Rebuildable: yes
    class Closing {
      public:
        Closing(const Closing &) = delete;
        auto operator=(const Closing &) -> Closing & = delete;
        /// @brief 移动构造：窗口关闭权易主（源不再负责清位）。
        /// @param other 移动源。
        Closing(Closing &&other) noexcept : active_(other.active_) { other.active_ = false; }
        /// @brief 移动赋值：自身仍开着的窗口先关。
        /// @param other 移动源。
        /// @return 自身引用。
        auto operator=(Closing &&other) noexcept -> Closing & {
            if (this != &other) {
                close();
                active_ = other.active_;
                other.active_ = false;
            }
            return *this;
        }
        /// @brief 析构即关闭窗口。
        ~Closing() { close(); }

      private:
        friend class MainQueue;
        /// @brief 开窗（仅经 `closing_scope()`）：锁内置位打烊标志。
        Closing() : active_(true) {
            std::scoped_lock lk(state().mtx);
            state().closing = true;
        }
        /// @brief 关窗：仅在自己名开窗时清位（幂等）。
        ///
        /// `const noexcept` 同 `Lease::release()` 的收尾理由：本函数是析构与移动赋值的共同终点，
        /// 加锁或首次惰性构造失败时绝不外抛；只读 `active_`（重开由新建守卫承担），故可为 const。
        auto close() const noexcept -> void {
            if (!active_) {
                return;
            }
            try {
                std::scoped_lock lk(state().mtx);
                state().closing = false;
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // 收起窗口的系统级失败不可恢复：析构路径不外抛，守卫本身照常销毁。
                // 此处的静默是本体的设计意图（不记录、不再抛），不是漏处理。
            }
        }
        bool active_ = false;  ///< 本守卫是否开着窗口
    };

    // ---- 上岗登记（全机制唯一「写」入口）----

    /// @brief 独占上岗：登记唤醒通道并领回值班租约。
    /// @param wake 每次入队后在**投递者线程**调用的唤醒体（须轻量、不阻塞、实现侧在锁外调用）；
    ///             无唤醒需求方（WASM rAF 环/TestController/CLI 手动排空）传空。
    ///             唤醒只回答「叫谁起床」，不决定任务归属。
    /// @return 成功领回租约；**此刻**已有值班者时为空 `nullopt`（调用方据此拒绝并发 run）。
    [[nodiscard]] static auto on_duty(std::function<void()> wake) -> std::optional<Lease> {
        std::scoped_lock lk(state().mtx);
        if (state().epoch != 0) {
            return std::nullopt;  // CAS 只拒「此刻有人」；归还后的重入循环可再次领取
        }
        state().epoch = ++state().epoch_seq;  // 单调分配，永不重置/复用（防重入 run 世代撞旧）
        state().wake = std::move(wake);
        return Lease{state().epoch};
    }

    /// @brief 打开打烊窗口（仅 `loop_end` 用）：租约已还、正在收尾排空的有界标记。
    /// @return 作用域守卫；析构即关闭窗口。
    [[nodiscard]] static auto closing_scope() -> Closing { return Closing{}; }

    // ---- 写方（任意线程）----

    /// @brief 投递任务给值班循环：接受则入队并唤醒值班者。
    ///
    /// **返回 void 系刻意设计**：落地相调用方对失败无任何可行动作，可忽略的返回值只会
    /// 催生恒被忽略的分支或「那就就地执行」的新静默回退；需要失败知识的一方走 `try_post`。
    /// @param task 待值班循环在**主线程**执行的可调用体；本函数不执行它。
    static auto post(std::function<void()> task) -> void { post(std::move(task), 0); }

    /// @brief 带世代要求的投递（async 侧发起时捕获本世世代号传入）。
    /// @param task 同 `post`。
    /// @param expected_epoch 发起时捕获的世代号；0 = 代际无关（只要求有人值班）。
    static auto post(std::function<void()> task, std::uint64_t expected_epoch) -> void {
        std::function<void()> wake;
        {
            std::scoped_lock lk(state().mtx);
            if (!accept_locked(expected_epoch)) {
                reject_locked();  // 打烊取消：不投递、不 terminate，计数 + ERROR 日志
                return;
            }
            state().queue.push_back(std::move(task));
            wake = state().wake;  // 副本锁外调用：并发 release 后本拍 wake 只多敲一次空窗，无害
        }
        if (wake) {
            wake();
        }
    }

    /// @brief `post` 的无失败形：契约边界在外读点（UIA/Inspector）的降级判据。
    /// @param task 同 `post`。
    /// @return 入队成功为 true；无值班/世代失配为 false（不记诊断、绝不触发失败处理器——
    ///         外部客户端的迟到请求在边界处诚实降级，不带走宿主进程）。
    [[nodiscard]] static auto try_post(std::function<void()> task) -> bool { return try_post(std::move(task), 0); }

    /// @brief 带世代要求的无失败投递。
    /// @param task 同 `post`。
    /// @param expected_epoch 同 `post`。
    /// @return 同 `try_post`。
    [[nodiscard]] static auto try_post(std::function<void()> task, std::uint64_t expected_epoch) -> bool {
        std::function<void()> wake;
        {
            std::scoped_lock lk(state().mtx);
            if (!accept_locked(expected_epoch)) {
                return false;
            }
            state().queue.push_back(std::move(task));
            wake = state().wake;
        }
        if (wake) {
            wake();
        }
        return true;
    }

    // ---- 读方（值班者所在线程，即主线程）----

    /// @brief 摘出当前全部待执行任务（swap 出队；调用方在主线程逐个执行）。
    ///
    /// 不要求值班在手：`loop_end` 还租约后的收尾排空即走这里——先还后捞，
    /// 捞完后队列不可能再被写入（接受判据恒假），恒有定责。
    /// @return 本批任务（可能为空；空批 = 无事发生的帧照常返回空）。
    [[nodiscard]] static auto take_batch() -> std::vector<std::function<void()>> {
        std::vector<std::function<void()>> out;
        std::scoped_lock lk(state().mtx);
        out.reserve(state().queue.size());
        for (auto &task : state().queue) {
            out.push_back(std::move(task));
        }
        state().queue.clear();
        return out;
    }

    /// @brief 值班世代的全函数：在岗返回当前世代号（≥1），不在岗（未上岗/已 release）恒返回 0。
    /// @return 当前在岗世代号或 0。
    [[nodiscard]] static auto current_epoch() -> std::uint64_t {
        std::scoped_lock lk(state().mtx);
        return state().epoch;
    }

    /// @brief 是否有循环在岗（`current_epoch() != 0` 的可读别名，判据单源不漂移）。
    /// @return 有值班者为 true。
    [[nodiscard]] static auto has_on_duty() -> bool { return current_epoch() != 0; }

    /// @brief 队列是否有待排水的任务（供帧末等待决策：有待办即不深睡，立即进入下一帧排空）。
    /// @return 有滞留任务为 true。
    [[nodiscard]] static auto has_pending() -> bool {
        std::scoped_lock lk(state().mtx);
        return !state().queue.empty();
    }

    /// @brief 当前滞留待排水的任务数（观测口：测试据此造「已排进主线程但尚未执行」的时序）。
    /// @return 队列中尚未被 `take_batch` 取走的任务数。
    [[nodiscard]] static auto pending_count() -> std::size_t {
        std::scoped_lock lk(state().mtx);
        return state().queue.size();
    }

    /// @brief 是否处于打烊窗口（`loop_end` 收尾排空中；`is_closing() ⇒ current_epoch()==0`）。
    /// @return 打烊窗口内为 true。
    [[nodiscard]] static auto is_closing() -> bool {
        std::scoped_lock lk(state().mtx);
        return state().closing;
    }

    /// @brief 落地相打烊取消的累计计数（「丢弃可观测、非静默」的核对面）。
    /// @return 被拒绝的投递数。
    [[nodiscard]] static auto dropped_count() -> std::uint64_t {
        return state().dropped.load(std::memory_order_relaxed);
    }

  private:
    /// @brief 进程级单例状态（锁 + 队列 + 值班位 + 世代 + 打烊窗 + 计数，同址同锁）。
    struct State {
        std::mutex mtx;  ///< 罩住以下全部字段（含 wake 的调用副本除外）
        std::deque<std::function<void()>> queue;  ///< 待主线程执行的任务
        std::function<void()> wake;  ///< 在岗唤醒通道；空 = 未登记
        std::uint64_t epoch = 0;  ///< 当前在岗世代号；0 = 不在岗
        std::uint64_t epoch_seq = 0;  ///< 单调分配器（永不重置/复用/回退）
        bool closing = false;  ///< 打烊窗口标记（`Closing` 守卫有界开关）
        std::atomic<std::uint64_t> dropped{0};  ///< 落地相打烊取消计数（锁外读）
    };
    /// @brief 惰性构造的函数级单例；跨 TU 初始化顺序在此不存在。
    [[nodiscard]] static auto state() -> State & {
        // 仅浏览器口径命中——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
        // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
        static State s;
        return s;
    }
    /// @brief 接受判据（**须持 `state().mtx`**）：`在岗 && (代际无关 || 同世)`。
    /// @param expected_epoch 投递方声明的世代；0 = 代际无关。
    /// @return 可入队为 true。
    [[nodiscard]] static auto accept_locked(std::uint64_t expected_epoch) -> bool {
        return state().epoch != 0 && (expected_epoch == 0 || expected_epoch == state().epoch);
    }
    /// @brief 落地拒绝的观测面（**须持 `state().mtx`**）：计数 + ERROR 日志 + 诊断，绝不抛出。
    static auto reject_locked() -> void {
        state().dropped.fetch_add(1, std::memory_order_relaxed);
        AURORA_LOG_ERROR("main_queue", "posted task cancelled by loop closure (no on-duty main loop)");
        Diagnostics::warn("MainQueue: task dropped — no on-duty main loop (cancelled by closure)", "main_queue");
    }
};

}  // namespace aurora
