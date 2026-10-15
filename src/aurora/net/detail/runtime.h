#pragma once

/// @brief 平台网络运行时的 RAII 会话（CHG-018 §4 处置：由公共面降为库内细节）。
/// @file net/detail/runtime.h
///
/// **仅供 `src/aurora/net/` 实现侧使用，不占公共 API 预算。**存在理由三条：
/// 1. Windows 的 Winsock 在使用任何 socket API 前必须 `WSAStartup`、末个使用者退出时
///    `WSACleanup`；POSIX 无此概念。这条生命周期是平台事实，不会因为公共面少了一个类型而消失。
/// 2. 公共面曾有 `aurora::net::Session`，要求调用方自备会话；同步阻塞面迁进 `aurora::net::blocking`
///    时按 CHG-018 §4 改为「库内自持」——句柄创建点先经 `socket.cpp` 里的进程级钉住取得运行时，
///    于是工具与测试宿主不再需要感知本类型。
/// 3. 库内仍有「一时没有句柄、却要求运行时可用」的场景（引擎的整个生命周期、`local_ipc` 的运行期
///    探测），本类正是那枚作用域引用。计数与钉住共用同一份引用计数，两者叠加不会提前 `WSACleanup`。
///
/// 计数语义：第一个构造的会话触发初始化，最后一个析构的会话执行清理；嵌套/多会话并存安全。
/// 生命周期外用句柄的行为未定义（与平台一致）。
///
/// @note Thread: 构造/析构线程安全（内部加锁计数）
/// @note Side-effects: Windows 首次构造分配 Winsock 资源，末个持有者析构释放
namespace aurora::net::detail {

/// @brief 进程级 socket 运行时会话（引用计数 RAII）。
class Session {
  public:
    /// @brief 构造会话：Windows 侧首个活跃会话触发 `WSAStartup`，POSIX 侧置 `ok_ = true`。
    Session();
    /// @brief 析构会话：Windows 侧末个活跃会话执行 `WSACleanup`，POSIX 侧无操作。
    ~Session();

    // 纯作用域 guard：拷贝会产生重复清理配对，移动会打乱引用计数归属，一律禁用。
    Session(const Session &) = delete;
    auto operator=(const Session &) -> Session & = delete;
    Session(Session &&) = delete;
    auto operator=(Session &&) -> Session & = delete;

    /// @brief 运行时是否可用。
    /// @return Windows 上 `WSAStartup` 成功（或已有活跃会话）为 true；初始化失败为 false；POSIX 恒 true。
    [[nodiscard]] auto ok() const -> bool { return ok_; }

  private:
    bool ok_ = false;
};

}  // namespace aurora::net::detail
