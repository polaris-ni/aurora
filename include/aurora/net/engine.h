#pragma once

/// @brief 传输引擎抽象与进程级入口（CHG-010 底座切片：事件驱动形态的执行载体）。
/// @file net/engine.h
///
/// 本库自带引擎的默认形态是**自持一条常驻 poll 线程**（提案 §2）：等待就绪不发生在 UI 线程，
/// 任务与到期定时器在引擎线程执行，面向使用者的完成投递一律经主线程投递器回投。这样自带引擎的
/// 行为**不依赖各平台主循环的等待形态差异**（Win32 消息泵、X11/Wayland `poll`、GLFW 事件等待、
/// macOS run loop、wasm rAF 形态各异），代价是每操作多一次跨线程投递（微秒级，与 `Task<T>` 同量级）。
///
/// 四条契约（规范性，落码与文档回写以此为准）：
/// - **惰性启动**：引擎线程随首次需要它的调用（`post` / `post_after`）启动；`shutdown()` 幂等，
///   停止接收新任务并收敛线程，此后再次 `post` 会重新惰性启动。
/// - **退出收敛是显式的**：进程退出路径不自动收敛（单例刻意泄漏，理由见 `src/aurora/net/engine.cpp`
///   的 `shared_engine()`）——静态析构阶段的加锁与线程 join 已不可依赖，实测会在拆除路径上段错误。
///   需要确定性收敛的宿主（应用关闭流程、真机探针、集成用例）自己调 `shutdown()`。
/// - **投递恒回主线程**：引擎线程绝不触碰 widget / `State`；就绪回调经
///   `app::EventSourceRegistry` 的 `Dispatcher` 接缝投递，引擎启动时装入
///   `aurora::detail::post_to_main` 形态（宿主已装则不改，见 §8.8.2）。
/// - **一条事件源只由一个驱动方等待**：常驻引擎循环与宿主自带等待集对同一注册口是**互斥**的两种
///   驱动方式，同时启用会让一次就绪投递两次。缺省由引擎驱动；宿主接管时调
///   `app::EventSourceRegistry::set_host_driven(true)`，引擎每轮等待都尊重该开关（置回假即接管）。
///
/// 引擎不暴露任何平台句柄形态；`Capabilities` 是运行期唯一能力真值源。
///
/// @code
/// auto &engine = aurora::net::Engine::instance();
/// if (engine.capabilities().event_source) {
///     (void)engine.post([] { /* 在引擎线程执行 */ });
/// }
/// @endcode
/// @note Thread: thread-safe（`instance()` 与全部成员可跨线程调用；任务体在引擎线程执行）
/// @note Side-effects: `post` / `post_after` 可触发引擎线程惰性启动并写入进程级注册口接缝

#include <chrono>
#include <functional>
#include <string_view>

#include "aurora/core/result.h"
#include "aurora/net/capabilities.h"

namespace aurora::net {

/// @brief 传输引擎的实现侧接口（构建期装配、运行期可查询）。
/// @note Thread: thread-safe
/// @note Side-effects: none（除显式调用成员外不产生副作用）
class Engine {
  protected:
    /// @brief 构造：仅引擎实现侧（子类）可构造，公共面只经 `instance()` 取用。
    Engine() = default;

  public:
    Engine(const Engine &) = delete;
    auto operator=(const Engine &) -> Engine & = delete;
    /// @brief 移动构造禁用：引擎是进程级单例的实现侧基座，值语义不适用（Rule of Five 的显式表态）。
    Engine(Engine &&) = delete;
    /// @brief 移动赋值禁用：同 `Engine(Engine &&)`。
    auto operator=(Engine &&) -> Engine & = delete;

    /// @brief 析构：实现侧负责收敛自有线程与句柄；进程级单例刻意不析构，故本析构只在调用方自己
    ///        持有的引擎实例（用例替身、宿主接管形态）上发生。
    virtual ~Engine() = default;

    /// @brief 引擎种类名（运行期可观测，供 Inspector / 日志区分实现）。
    /// @return 实现名：自带引擎为 `"poll"`；不具备网络语义的腿为 `"none"`（后续原生引擎各自回报）。
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// @brief 本腿能力矩阵（唯一真值源；使用方据此降级而非猜平台宏）。
    /// @return 按当前运行期状态组装的能力位集合。
    [[nodiscard]] virtual auto capabilities() const -> Capabilities = 0;

    /// @brief 把一个任务投给引擎线程执行（惰性启动引擎线程）。
    ///
    /// 任务在引擎线程按登记次序执行；执行体不得触碰 widget / `State`（需要回主线程时自行经
    /// `aurora::detail::post_to_main` 投递）。
    /// @param task 待执行体；引擎线程持强引用直到执行完毕。
    /// @return 已受理为 OK；本腿无引擎语义或已停机后无法重启为 `net-platform-unavailable`（不静默丢弃）。
    [[nodiscard]] virtual auto post(std::function<void()> task) -> Result<void> = 0;

    /// @brief 把一个任务登记为「`delay` 之后在引擎线程执行」。
    ///
    /// 到期时刻按 `steady_clock` 计算，与 `wait_ready` 的等待窗口共用同一条循环，不额外起线程。
    /// 停机时未到期的定时器**被丢弃**（不执行、不报错）：定时器只承载「放弃等待」语义，
    /// 由上层操作自行以取消令牌表达失败——静默执行一个已过期的续体反而会造成错误的完成时序。
    /// @param delay 相对到期时长；非正值等价于立即到期（下一轮循环执行）。
    /// @param task 到期执行体。
    /// @return 已登记为 OK；本腿无引擎语义为 `net-platform-unavailable`。
    [[nodiscard]] virtual auto post_after(std::chrono::milliseconds delay, std::function<void()> task)
        -> Result<void> = 0;

    /// @brief 引擎线程当前是否在跑（测试与自省用，不作为功能开关）。
    /// @return 已惰性启动且未收敛时为 true。
    [[nodiscard]] virtual auto running() const -> bool = 0;

    /// @brief 收敛引擎线程：停止接收新任务，跑完已入队任务后 join（幂等，未启动时为空操作）。
    ///
    /// 已投递到主线程队列的完成事件不受本调用影响——它们的排空属宿主退出路径的职责（§2）。
    virtual auto shutdown() -> void = 0;

    /// @brief 进程级引擎单例（自带 poll 线程引擎；无 fd 语义的腿回报不支持形态）。
    /// @return 全局唯一引擎实例引用；本调用本身不启动线程（惰性启动由 `post` 触发）。
    [[nodiscard]] static auto instance() -> Engine &;
};

}  // namespace aurora::net
