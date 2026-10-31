#pragma once

/// @brief 应用身份与单实例守卫：进程级「我是谁」与「是否已有同名实例在跑」。
/// @file
///
/// 定位：URL 方案 / 文件关联 / 登录项三项注册都要拿「本应用的可执行文件路径」填 OS 命令模板，
/// 单实例要拿一份命令行快照才谈得上「第二个进程收到的 URI 转交给首个实例」。故本头是那几项的
/// **先行件**：身份由调用方在 `main()` 处显式传入（本库不代读 `GetModuleFileName` / `readlink` /
/// `NSBundle`，三座平台桥共用同一份身份，不各自再造一份）。典型用法：
///
/// @code
/// int main(int argc, char **argv) {
///     aurora::AppIdentity identity;
///     identity.executable_path = argv[0];
///     identity.arguments.assign(argv + 1, argv + argc);  // 不含 argv[0]
///     auto outcome = aurora::SingleInstance::instance().acquire(identity);
///     if (!outcome || *outcome == aurora::SingleInstanceOutcome::Forwarded) return 0;
///     aurora::SingleInstance::instance().set_on_uri_received([](std::string_view uri) { navigate(uri); });
///     // ... 正常启动 UI；Linux 上每帧调用一次 poll()，Windows / macOS 由 OS 消息泵代劳
/// }
/// @endcode
///
/// 交付语义（与 `codespec/specification/06-app-platform.md` 的 URL 方案一节一致）：
/// - 第二实例把命令行 URI 转交给首个实例并**等待确认**；超时或对端明确失败时回落为
///   `FallbackNewInstance`（按新实例正常启动）——「第二实例默默退出」被禁，它表现为
///   「点了链接什么都没发生」。
/// - Linux 侧锁文件与套接字落在用户私有运行时目录（`$XDG_RUNTIME_DIR` 优先）并以 `0700`
///   创建；不得用 `/tmp` 下可预测的名字（同机其他用户可预占名字做拒绝服务或抢先监听）。
///   Windows 有名互斥量与 macOS 唤起通道由 OS 兜底，这是如实申报的不对称。
/// - 转交来的 URI 是不可信输入：本层只把它交给 `Navigator::open_uri` 的既有路由语义，
///   不拼进任何待执行的命令行、不替导航层兜底。
///
/// @note Thread: main-thread only（`acquire` / `poll` / 回调；Linux 套接字由 `poll()` 在调用线程排空）
/// @note Side-effects: `acquire` 触碰 OS 互斥量 / 锁文件 / 套接字；`release` 释放它们
/// @note Rebuildable: no（进程级会话状态）

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

/// @brief 应用身份：可执行路径 + 命令行快照 + 可选的 bundle 标识与实例键覆盖。
///
/// 由调用方在进程入口构造并显式传入。`instance_key` 留空时从 `executable_path` 派生
/// （见 `derive_instance_key`）；显式给出时以调用方值为准（例如同一可执行文件按配置区分实例）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct AppIdentity {
    std::string executable_path;  ///< 可执行文件路径（绝对路径最佳；相对路径按调用时 CWD 解析）
    std::vector<std::string> arguments;  ///< 命令行参数快照（**不含** argv[0]；原样保存，不 shell 解析）
    std::string bundle_identifier;  ///< macOS bundle identifier（`com.example.app`）；其它平台可空
    std::string instance_key;  ///< 显式实例键；空则从可执行路径派生（稳定、不含版本 / mtime）
};

/// @brief 由可执行路径派生稳定实例键（16 位十六进制 FNV-1a）。
///
/// 规范化：分隔符统一、词法消解 `.` / `..`、Windows / macOS 大小写折叠、存在时解析符号链接。
/// **不**混入版本号、mtime 或命令行内容——键一变，互斥量与锁文件就认不出自己，表现为
/// 「每次都当成首个实例」。
/// @param executable_path 可执行文件路径。
/// @return 稳定实例键（空输入亦返回非空串）。
/// @note Thread: thread-safe
/// @note Side-effects: 可能读取文件系统（符号链接解析），不写。
[[nodiscard]] auto derive_instance_key(std::string_view executable_path) -> std::string;

/// @brief 取 `AppIdentity` 的有效实例键（显式值优先，否则派生）。
/// @param identity 应用身份。
/// @return 实例键。
/// @note Thread: thread-safe
/// @note Side-effects: 同 `derive_instance_key`。
[[nodiscard]] auto resolve_instance_key(const AppIdentity &identity) -> std::string;

/// @brief 单实例获取结果。
enum class SingleInstanceOutcome : std::uint8_t {
    Primary = 0,  ///< 本进程是首个实例；后续唤起经 `set_on_uri_received` 交付。
    Forwarded = 1,  ///< 已有实例在跑，本进程的命令行 URI 已送达并确认。
    FallbackNewInstance = 2,  ///< 转交失败（对端退出 / 拒绝 / 超时），按新实例正常启动。
};

/// @brief 进程级单实例守卫（懒构造单例，与 `RecentDocuments::instance` 同格）。
///
/// 平台通道：
/// - **Windows**：`Local\\` 有名互斥量判首实例，跨进程消息（`WM_COPYDATA`）转交 URI。
/// - **macOS**：AppKit 应用级唤起通道（`kAEGetURL` 一系）。
/// - **Linux**：`$XDG_RUNTIME_DIR` 下的锁文件 + 用户私有 Unix 套接字；README 口径见类注释上方。
/// - **Headless / Wasm**：无跨进程通道，`acquire` 如实返回 `Primary`（不谎报「已转发」）。
///
/// @note Thread: main-thread only（`acquire` / `poll` / 回调）
/// @note Side-effects: 触碰 OS 互斥量 / 锁文件 / 套接字；`release` 释放
/// @note Rebuildable: no（进程级会话状态）
class SingleInstance {
  public:
    /// @brief 进程级单例。
    /// @return 单例引用（首次调用时构造）。
    [[nodiscard]] static auto instance() -> SingleInstance &;

    SingleInstance(const SingleInstance &) = delete;
    auto operator=(const SingleInstance &) -> SingleInstance & = delete;
    SingleInstance(SingleInstance &&) = delete;
    auto operator=(SingleInstance &&) -> SingleInstance & = delete;
    /// @brief 析构：释放平台资源（与 `release()` 同款，幂等）。
    ~SingleInstance();

    /// @brief 尝试成为首个实例；已有实例时把命令行 URI 转交并等待确认。
    ///
    /// 重复调用幂等：已 `acquire` 过时返回上次结果。
    /// @param identity 应用身份（命令行参数里含 URI 时会被提取并转交 / 入队）。
    /// @return 获取结果；平台通道故障（互斥量 / 锁文件 / 套接字不可用）为结构化错误。
    /// @note 首实例 argparse 里的 URI 也会入队，回调登记后按到达顺序交付。
    [[nodiscard]] auto acquire(const AppIdentity &identity) -> Result<SingleInstanceOutcome>;

    /// @brief 登记 URI 接收回调（首个实例存续期内有效；重复登记替换旧回调）。
    /// @param callback 每次收到一条转交 URI 时调用一次；空函数清除回调。
    /// @note Thread: 在调用本方法的线程（主线程）上同步调用。
    auto set_on_uri_received(std::function<void(std::string_view)> callback) -> void;

    /// @brief 排空平台通道上的待处理 URI 并交付回调。
    ///
    /// Windows / macOS 由 OS 消息泵推进、本方法只搬运已入队项；**Linux 必须由应用每帧调用**
    /// （库不代开线程，回调始终落在调用线程）。未登记回调时 URI 留在队列里，不丢。
    /// @note Thread: main-thread only
    auto poll() -> void;

    /// @brief 释放平台资源（互斥量 / 锁文件 / 套接字 / 消息钩子）。幂等。
    auto release() -> void;

    /// @brief 本进程当前是否为已获取的首实例。
    /// @return 已 `acquire` 且结果为 `Primary` 时为 true。
    [[nodiscard]] auto is_primary() const -> bool;

    /// @brief 当前实例键（未 `acquire` 时为空串）。
    /// @return 有效实例键。
    [[nodiscard]] auto instance_key() const -> std::string;

    /// @brief 清空全部状态（内存队列 / 回调 / 平台资源）。测试与重置用。
    auto clear_all() -> void;

  private:
    SingleInstance() = default;

    bool acquired_ = false;  ///< 是否已 `acquire`
    bool primary_ = false;  ///< 是否为已确认的首实例
    SingleInstanceOutcome outcome_ = SingleInstanceOutcome::FallbackNewInstance;  ///< 最近一次获取结果
    std::string key_;  ///< 当前实例键
    std::vector<std::string> pending_uris_;  ///< 待交付 URI（按到达序）
    std::function<void(std::string_view)> on_uri_;  ///< URI 接收回调
};

}  // namespace aurora
