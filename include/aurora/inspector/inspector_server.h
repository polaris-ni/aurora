#pragma once

#include <cstdint>
#include <functional>
#include <memory>

// 前向声明，避免重回头文件
namespace aurora {
class Node;
class Surface;
} // namespace aurora

namespace aurora {

/// @brief Minimal localhost-only HTTP server for remote Inspector access.
///
/// Exposes REST endpoints to query/modify the widget tree at runtime.
/// Uses pimpl to keep Winsock2 types out of this header.
///
/// @note Thread: thread-safe (background worker thread for HTTP)
/// @note Side-effects: none (network I/O)
/// @note Rebuildable: no
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): pimpl 类显式声明析构，拷贝/移动语义无意为禁用而非默认
class InspectorServer {
  public:
    explicit InspectorServer(std::function<Node()> root_getter);
    ~InspectorServer();

    /// @brief Start HTTP server on the given port (default 6280). Returns true on success.
    /// The server runs in a background worker thread.
    auto start(uint16_t port = 6280) const -> bool;

    /// @brief Stop the server and join the worker thread.
    void stop() const;

    /// @brief Check if the server is currently running.
    [[nodiscard]] auto is_running() const -> bool;

    /// @brief Get the port the server is listening on (0 if not started).
    [[nodiscard]] auto port() const -> uint16_t;

    /// @brief Inject a Surface getter so debug endpoints (snapshot / state)
    ///        can access the live runtime Surface. Optional; if unset, those
    ///        endpoints return 400. `pick` does not require it (falls back to
    ///        the root widget size). Thread-safe (called before `start()`).
    auto set_surface_getter(std::function<Surface *()> getter) const -> void;

    // Non-copyable, non-movable
    InspectorServer(const InspectorServer &) = delete;
    auto operator=(const InspectorServer &) -> InspectorServer & = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace aurora
