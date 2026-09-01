#pragma once

#include <cassert>
#include <thread>

namespace aurora {

/**
 * @brief 单线程 UI 的编译期/运行期守卫（ARCHITECTURE.md §3.1）。
 *
 * Aurora 是单线程 UI：事件、布局、绘制、状态变更均须在同一线程（通常是主线程）完成。
 * `MainThreadOnly<T>` 包装一个值，并在 debug 下断言访问发生在构造它的同一线程；
 * 配合 `AURORA_MAIN_THREAD` 标注的入口（`Application::run` / `Window::present_root` / `Window::run`），
 * 静态分析 / 文档工具可识别「必须在主线程调用」的契约。
 *
 * @tparam T 被包装的值类型。
 * @tparam Check 是否启用线程检查。设为 false 时**零开销**（不存储 owner 线程、不断言），
 *               用于性能敏感路径关闭检查（与 `AURORA_ASSERT` 的 debug-only 语义一致）。
 */
template<typename T, bool Check = true> class MainThreadOnly {
  public:
    explicit MainThreadOnly(T value) : m_value(std::move(value)), m_owner(std::this_thread::get_id()) {}

    [[nodiscard]] auto get() & -> T & {
        assert_owner();
        return m_value;
    }
    [[nodiscard]] auto get() const & -> const T & {
        assert_owner();
        return m_value;
    }

    /// @brief 仅在主线程可写。
    auto set(T value) -> void {
        assert_owner();
        m_value = std::move(value);
    }

  private:
    auto assert_owner() const -> void {
        if constexpr (Check) {
            assert(std::this_thread::get_id() == m_owner &&
                   "MainThreadOnly: accessed from a thread other than the owning (main) thread");
        }
    }

    T m_value;
    std::thread::id m_owner;
};

/// @brief 零开销特化：Check=false 时不存储 owner、不断言。
template<typename T> class MainThreadOnly<T, false> {
  public:
    explicit MainThreadOnly(T value) : m_value(std::move(value)) {}
    [[nodiscard]] auto get() & -> T & { return m_value; }
    [[nodiscard]] auto get() const & -> const T & { return m_value; }
    auto set(T value) -> void { m_value = std::move(value); }

  private:
    T m_value;
};

} // namespace aurora

/// @brief 标注函数「必须在主线程调用」（ARCHITECTURE.md §3.1）。
///
/// clang 用 `annotate` 属性（可被静态分析 / 文档工具读取）；GCC 不支持 `annotate`
/// 属性（会触发 `-Wattributes`），故 GCC 与其它编译器均为 no-op；运行期契约仍由
/// `MainThreadOnly` 守卫。
#ifdef __clang__
#define AURORA_MAIN_THREAD [[clang::annotate("au::main_thread")]]
#else
#define AURORA_MAIN_THREAD /* no-op: main-thread contract enforced at runtime via MainThreadOnly */
#endif
