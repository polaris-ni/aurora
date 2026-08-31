#pragma once

#include <chrono>
#include <functional>
#include <utility>

#include "aurora/app/scheduler.h"
#include "aurora/core/diagnostics.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 组件级定时任务控件（控制流 Widget，声明式、响应式为主 + 可选回调）。
 *
 * 包裹一棵子树，挂载时向运行中的应用级 `Scheduler`（`Scheduler::current()`）注册周期任务；
 * 每次 tick 自增内部 `State<int>`（经 `ticks()` 暴露为 `SignalView<int>`）驱动子 UI 自动重绘，
 * 并可选调用 `on_tick(int)` 回调处理命令式副作用。卸载/销毁时自动 `cancel()` 句柄。
 *
 * 跨框架对照：`setTimeout` / `setInterval`（JS）、`Timer.periodic`（Flutter）、`Disposable`/`Handler`（Android）。
 *
 * 用法：
 * @code
 * // 响应式：时钟每秒刷新（子 UI 经 ticks() 绑定）
 * au::Timer(std::chrono::seconds(1), [](const au::SignalView<int> &tick) {
 *     return au::Text(au::Computed<std::string>{ [&] { return "tick: " + std::to_string(tick.get()); } });
 * });
 *
 * // 命令式：可选 on_tick 回调
 * au::Timer(std::chrono::seconds(1), au::Timer::child([] { return au::Text("..."); }),
 *           [](int n) { poll(n); });
 * @endcode
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Timer : public SingleChild {
  public:
    using TickBuilder = std::function<Node(const SignalView<int> &)>;

    Timer() : m_tick(std::make_shared<State<int>>(0)) {}

    /// @brief 构造周期定时控件。
    /// @param period   tick 周期（按 `std::chrono::steady_clock`）。
    /// @param builder  子 UI 构造器：接收 `ticks()` 信号，构建一次、内部经响应式绑定刷新。
    /// @param on_tick  可选命令式回调，每次 tick 携带计数（从 1 开始）。
    Timer(std::chrono::steady_clock::duration period, const TickBuilder &builder, std::function<void(int)> on_tick = {})
        : SingleChild(Node{}), m_period(period), m_on_tick(std::move(on_tick)),
          m_tick(std::make_shared<State<int>>(0)) {
        if (builder) {
            m_child = builder(*m_tick);
        }
    }

    ~Timer() override { m_handle.cancel(); }

    Timer(const Timer &) = delete;
    auto operator=(const Timer &) -> Timer & = delete;
    Timer(Timer &&) = default;
    auto operator=(Timer &&) -> Timer & = default;

    /// @brief tick 计数信号（从注册后首次 tick 起为 1,2,3…）；子 UI 经其响应式绑定刷新。
    [[nodiscard]] auto ticks() const -> const SignalView<int> & { return *m_tick; }

    [[nodiscard]] auto type_name() const -> const char * override { return "Timer"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Timer",
            .properties = {
                { .name = "period", .type = "std::chrono::steady_clock::duration", .default_value = "—", .required = true, .note = "tick 周期（按 steady_clock）" },
                { .name = "on_tick", .type = "std::function<void(int)>", .default_value = "nullptr", .required = false, .note = "可选命令式回调，每次 tick 携带计数（从 1 起）" },
            },
            .examples = {
                "au::Timer(1s, [](const au::SignalView<int>& t){ return au::Text(au::Computed<std::string>{ [&]{ return std::to_string(t.get()); } }); });",
                "au::Timer(1s, au::Timer::child([]{ return au::Text(\"tick\"); }), [](int n){ poll(n); });",
            },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(m_tick.get()); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        return m_child ? m_child.widget().layout(c, ctx) : Size{};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        if (Scheduler *s = Scheduler::current()) {
            m_handle = s->set_interval(m_period, [this]() -> void {
                const int n = m_tick->get() + 1;
                m_tick->set(n);
                if (m_on_tick) {
                    m_on_tick(n);
                }
            });
        } else {
            // 降级：无运行中 App（如纯 render_to_png / 未进 run 循环），记告警并以 tick=0 渲染子 UI，不崩溃。
            Diagnostics::warn("timer", "Timer mounted without a running Scheduler (Scheduler::current() == nullptr); "
                                       "ticks will not fire.");
        }
        m_child.widget().mount(ctx);
    }

  private:
    std::chrono::steady_clock::duration m_period{};
    std::function<void(int)> m_on_tick;
    std::shared_ptr<State<int>> m_tick; ///< 内部 tick 计数（响应式驱动子 UI）。
    TimerHandle m_handle;               ///< 注册到 Scheduler 的句柄，析构时取消。
};

} // namespace aurora
