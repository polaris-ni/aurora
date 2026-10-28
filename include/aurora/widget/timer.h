#pragma once

#include <chrono>
#include <functional>
#include <utility>

#include "aurora/app/scheduler.h"
#include "aurora/core/diagnostics.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 组件级定时任务控件（控制流 Widget，声明式、响应式为主 + 可选回调）。
///
/// 包裹一棵子树，挂载时向运行中的应用级 `Scheduler`（`Scheduler::current()`）注册周期任务；
/// 每次 tick 自增内部 `State<int>`（经 `ticks()` 暴露为 `ReadOnlyState<int>`）驱动子 UI 自动重绘，
/// 并可选调用 `on_tick(int)` 回调处理命令式副作用。卸载/销毁时自动 `cancel()` 句柄。
///
/// 跨框架对照：`setTimeout` / `setInterval`（JS）、`Timer.periodic`（Flutter）、`Disposable`/`Handler`（Android）。
///
/// 用法：
/// @code
/// // 响应式：时钟每秒刷新（子 UI 经只读句柄派生）
/// au::Timer(std::chrono::seconds(1), [](au::ReadOnlyState<int> tick) {
/// return au::Text(au::computed([tick] { return "tick: " + std::to_string(tick.get()); }));
/// });
///
/// // 命令式：可选 on_tick 回调
/// au::Timer(std::chrono::seconds(1), au::Timer::child([] { return au::Text("..."); }),
/// [](int n) { poll(n); });
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Timer : public SingleChild {
  public:
    /// @brief tick 构建器回调类型：接收 tick 计数只读句柄、返回子树。
    /// @param ReadOnlyState 回调入参类型：按值交接的 tick 计数只读句柄。
    using TickBuilder = std::function<Node(ReadOnlyState<int>)>;

    /// @brief 默认构造：无周期（默认 duration）、无回调，tick 计数初始为 0。
    Timer() = default;

    /// @brief 构造周期定时控件。
    /// @param period   tick 周期（按 `std::chrono::steady_clock`）。
    /// @param builder  子 UI 构造器：接收 `ticks()` 只读句柄，构建一次、内部经响应式派生刷新。
    /// @param on_tick  可选命令式回调，每次 tick 携带计数（从 1 开始）。
    Timer(std::chrono::steady_clock::duration period, const TickBuilder &builder, std::function<void(int)> on_tick = {})
        : SingleChild(Node{}), period_(period), on_tick_(std::move(on_tick)) {
        if (builder) {
            child_ = builder(ticks());
        }
    }

    /// @brief 析构：取消已向 Scheduler 注册的周期任务句柄，销毁后不再触发 tick。
    ~Timer() override { handle_.cancel(); }

    /// @brief 拷贝构造禁用：Scheduler 句柄与内部 State 独占，不可复制订阅。
    Timer(const Timer &) = delete;
    /// @brief 拷贝赋值禁用：同拷贝构造，独占资源不可复制。
    /// @return 按赋值签名声明 Timer & 返回类型（永不被调用）。
    auto operator=(const Timer &) -> Timer & = delete;
    /// @brief 移动构造默认：逐成员接管周期、回调与句柄。
    Timer(Timer &&) = default;
    /// @brief 移动赋值默认：逐成员（含 handle_）移动赋值。
    /// @return 按赋值签名返回自身引用。
    auto operator=(Timer &&) -> Timer & = default;

    /// @brief tick 计数信号（从注册后首次 tick 起为 1,2,3…）；子 UI 经其响应式派生刷新。
    /// @return 内部 tick 来源的只读句柄（按值交接，复制共享同一来源）。
    [[nodiscard]] auto ticks() const -> ReadOnlyState<int> { return tick_; }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Timer"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Timer"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Timer 的控件描述符（period 必填、on_tick 可选回调与示例）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Timer",
            .properties =
                {
                    {.name = "period",
                     .type = "std::chrono::steady_clock::duration",
                     .default_value = "—",
                     .required = true,
                     .note = "Tick period (steady_clock based)"},
                    {.name = "on_tick",
                     .type = "std::function<void(int)>",
                     .default_value = "nullptr",
                     .required = false,
                     .note = "Optional imperative callback carrying the tick count (starting at 1)"},
                },
            .examples =
                {
                    "au::Timer(1s, [](au::ReadOnlyState<int> t){ return au::Text(au::computed([t]{ "
                    "return std::to_string(t.get()); })); });",
                    "au::Timer(1s, au::Timer::child([]{ return au::Text(\"tick\"); }), [](int n){ poll(n); });",
                },
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Timer 的控件描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号：收录内部 tick 计数来源。
    /// @param out 输出参数，收录本控件持有的读写句柄。
    auto collect_signals(StateSink &out) -> void override { out.push(tick_); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        return child_ ? child_.widget().layout(c, ctx) : Size{};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        if (Scheduler *s = Scheduler::current()) {
            handle_ = s->set_interval(period_, [this]() -> void {
                const int n = tick_.get() + 1;
                tick_.set(n);
                if (on_tick_) {
                    on_tick_(n);
                }
            });
        } else {
            // 降级：无运行中 App（如纯 render_to_png / 未进 run 循环），记告警并以 tick=0 渲染子 UI，不崩溃。
            Diagnostics::warn("timer",
                              "Timer mounted without a running Scheduler (Scheduler::current() == nullptr); "
                              "ticks will not fire.");
        }
        child_.widget().mount(ctx);
    }

    /// @brief 取消挂载期注册的周期任务并卸载子节点（与 `on_mount` 对称）。
    ///
    /// 取消是必需的而非可选：否则一只被摘下（换宿主重挂、或宿主显式卸载）的 `Timer` 仍留在
    /// `Scheduler` 的任务表里，闭包持着 `this` 继续每期触发——既漏又可能打到已不在树上的控件。
    /// 换宿主重挂时 `on_mount` 会重新注册，句柄随之替换。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        handle_.cancel();
        if (child_) {
            child_.widget().unmount();
        }
    }

  private:
    std::chrono::steady_clock::duration period_{};
    std::function<void(int)> on_tick_;
    State<int> tick_{0};  ///< 内部 tick 计数（响应式驱动子 UI）。
    TimerHandle handle_;  ///< 注册到 Scheduler 的句柄，析构时取消。
};

}  // namespace aurora
