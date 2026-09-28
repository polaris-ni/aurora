#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// 前向声明，避免重回头文件
namespace aurora {
class Node;
class Surface;
}  // namespace aurora

namespace aurora {

/// @brief Minimal localhost-only HTTP server for remote Inspector access.
/// Exposes REST endpoints to query/modify the widget tree at runtime.
/// Uses pimpl to keep Winsock2 types out of this header.
///
/// @note Thread: thread-safe (background worker thread for HTTP)
/// @note Side-effects: none (network I/O)
/// @note Rebuildable: no
/// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): pimpl 类显式声明析构，拷贝/移动语义无意为禁用而非默认
class InspectorServer {
  public:
    /// @brief 构造服务器并注入控件树取回回调。
    /// @param root_getter 返回主窗口控件树根 `Node` 的回调；树类端点每次请求时调用它取实时树。
    explicit InspectorServer(std::function<Node()> root_getter);
    /// @brief 析构前先执行 stop()：关闭监听套接字并 join HTTP 工作线程。
    ~InspectorServer();

    /// @brief Start HTTP server on the given port (default 6280). Returns true on success.
    /// The server runs in a background worker thread.
    ///
    /// @param port 监听的 TCP 端口（仅绑定 127.0.0.1）；传 0 由系统分配，实际端口经 port() 查询。
    /// @return 启动成功为 true；已在运行、Winsock 初始化或 socket/bind/listen 失败为 false。
    [[nodiscard]] auto start(uint16_t port = 6280) const -> bool;

    /// @brief Stop the server and join the worker thread.
    void stop() const;

    /// @brief Check if the server is currently running.
    /// @return start 成功后、stop（或析构）之前为 true。
    [[nodiscard]] auto is_running() const -> bool;

    /// @brief Get the port the server is listening on (0 if not started).
    /// @return 实际绑定的 TCP 端口（start 传 0 时为系统分配值）；未启动或已 stop 为 0。
    [[nodiscard]] auto port() const -> uint16_t;

    /// @brief Inject a Surface getter so debug endpoints (snapshot / state)
    ///        can access the live runtime Surface. Optional; if unset, those
    ///        endpoints return 400. `pick` does not require it (falls back to
    ///        the root widget size). Thread-safe (called before `start()`).
    /// @param getter 返回实时 Surface 的回调（非拥有指针，所有权归宿主）。
    auto set_surface_getter(std::function<Surface *()> getter) const -> void;

    /// @brief 注册窗口 id 枚举回调（多窗口调试）。
    /// 注册后 `GET /api/windows` 返回 `{"count":N,"windows":[id,...]}`；未注册时返回空数组
    /// （单窗口用法无影响）。**既有端点行为不变**：它们仍走构造时注入的 `root_getter`，
    /// 通常绑定主窗口。按窗口 id 取树请用 `GET /api/tree?window=<id>`（需配合
    /// `set_window_tree_getter`）。
    ///
    /// 回调在 HTTP 工作线程内调用，实现方须自行保证线程安全；实践上返回宿主 id 的**快照**最安全
    /// （宿主枚举通常须在 UI 线程执行）。
    ///
    /// @param getter 返回当前宿主窗口 id 列表的回调。
    auto set_window_ids_getter(std::function<std::vector<std::uint32_t>()> getter) const -> void;

    /// @brief 注册「按窗口 id 取树根」回调（多窗口调试）。
    /// 注册后 `GET /api/tree?window=<id>` 返回指定窗口的控件树 JSON；未注册时带 `window`
    /// 参数的请求回 400。回调在 HTTP 工作线程内经主线程 marshal 执行（与 `Surface` 的
    /// main-thread-only 约束一致）；无效 id 应返回**空 `Node`**，路由层据此回 404。无
    /// `window` 参数时 `/api/tree` 仍走构造时注入的 `root_getter`（主窗口），向后兼容。
    ///
    /// 典型注册（应用侧）：
    /// ```cpp
    /// server.set_window_tree_getter([&](std::uint32_t id) -> Node {
    ///     if (auto *h = app.window_host(id)) return h->scene().root_node();
    ///     return Node{};
    /// });
    /// ```
    ///
    /// @param getter 按窗口 id 返回该窗口控件树根 `Node` 的回调；无效 id 应返回空 `Node`（路由层据此回 404）。
    auto set_window_tree_getter(std::function<Node(std::uint32_t)> getter) const -> void;

    // Non-copyable, non-movable
    InspectorServer(const InspectorServer &) = delete;
    auto operator=(const InspectorServer &) -> InspectorServer & = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora
