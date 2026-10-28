#pragma once

#include <functional>
#include <utility>

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 订阅或副作用的存活凭证：析构即取消，`reset()` 是唯一的显式取消动词。
///
/// `State<T>` / `ReadOnlyState<T>` 的 `subscribe(fn)` 与自由函数 `effect(fn)` 都返回本类型。
/// 凭证持有运行记录及其读取的来源，因此：
/// - 保留返回值 → 订阅继续生效；
/// - 丢弃返回值 → 首值交付后不留后台订阅；
/// - 析构或 `reset()` → 立即解除观察边，此后不再回调。
///
/// 不可拷贝（同一订阅只会被取消一次），可移动（移交所有权）。需要多个持有者时
/// 由调用方自行以 `std::shared_ptr<Subscription>` 等容器表达，不把双重所有权塞进凭证。
///
/// @note Thread: main-thread only
/// @note Side-effects: none（析构/reset 时解除底层观察边）
/// @note Rebuildable: no
class Subscription {
  public:
    /// @brief 默认构造：不持有任何订阅（`active() == false`），可由移动赋值接管。
    Subscription() = default;

    /// @brief 由取消回调构造：仅供订阅/副作用工厂内部创建。
    /// @param cancel 取消回调：`reset()` 与析构时调用一次，须自行保证幂等。
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}

    /// @brief 析构：取消并释放所持订阅。
    ///
    /// 豁免 bugprone-exception-escape：`reset()` 调用的取消回调经 `std::function` 转发，
    /// 其调用与销毁均无 noexcept 规格——属本仓库记录在案的系统性假告警面。
    /// NOLINTNEXTLINE(bugprone-exception-escape)
    ~Subscription() { reset(); }

    Subscription(const Subscription &) = delete;
    auto operator=(const Subscription &) -> Subscription & = delete;

    /// @brief 移动构造：接管 `other` 的凭证，`other` 转为非激活态。
    /// @param other 源凭证；移动后其析构不再取消本订阅。
    Subscription(Subscription &&other) noexcept : cancel_(std::move(other.cancel_)) { other.cancel_ = nullptr; }

    /// @brief 移动赋值：先取消自身旧订阅，再接管 `other` 的凭证；自赋值安全。
    /// @param other 源凭证；赋值后其析构不再取消本订阅。
    /// @return 引用 `*this`。
    /// NOLINTNEXTLINE(bugprone-exception-escape)
    auto operator=(Subscription &&other) noexcept -> Subscription & {
        if (this != &other) {
            reset();
            cancel_ = std::move(other.cancel_);
            other.cancel_ = nullptr;
        }
        return *this;
    }

    /// @brief 是否仍持有有效订阅。
    /// @return 已建立且未取消时为 true；默认构造、已 reset 或已被移动后为 false。
    [[nodiscard]] auto active() const -> bool { return static_cast<bool>(cancel_); }

    /// @brief 立即取消订阅（幂等；重复调用安全）。
    auto reset() -> void {
        const auto cancel = std::exchange(cancel_, nullptr);
        if (cancel) {
            cancel();
        }
    }

  private:
    std::function<void()> cancel_;  ///< 取消回调；空表示无订阅或已取消
};

}  // namespace aurora
