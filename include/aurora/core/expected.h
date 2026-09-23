#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "aurora_assert.h"

namespace aurora {

/**
 * @brief 携带错误值的包装（参考 std::unexpected，C++23 前本库自带实现）。
 * @tparam E 错误类型（本库为 aurora::Error）。
 */
// 以下两个类型是 `std::expected` / `std::unexpected` 的本库镜像（C++23 前的自带实现），两类豁免
// 就此区间点名，其余检查不受影响：
//   · `readability-identifier-naming`：类名小写、存储成员带尾下划线，都是刻意与标准同名同形，
//     改成 PascalCase 反而让人误以为这是另一套类型；
//   · `cppcoreguidelines-pro-type-union-access`：二态存储本就靠手工 union 实现（placement new、
//     显式析构、按 tag 分支赋值/移动）。换成 `std::variant` 即把「不额外抛、tag 就是 `has_value_`」
//     这两条契约换掉，且镜像语义（错误态取值 = 常开拦截后 UB）需要精确控制访问路径。
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access, readability-identifier-naming)
template <typename E>
class unexpected {
  public:
    explicit unexpected(const E &e) : error_(e) {}
    explicit unexpected(E &&e) : error_(std::move(e)) {}

    [[nodiscard]] const E &error() const & noexcept { return error_; }
    [[nodiscard]] E &error() & noexcept { return error_; }
    [[nodiscard]] E &&error() && noexcept { return std::move(error_); }

  private:
    E error_;
};

template <typename E>
unexpected(E) -> unexpected<E>;

/**
 * @brief 最小化的 expected（二态：值或错误），满足 aurora::Result<T> 所需接口。
 *
 * 仅实现用到的路径：值构造、unexpected 构造、拷贝/移动、operator bool、
 * value()、error()、value_or()。完整 std::expected 语义后续可替换。
 */
template <typename T, typename E>
class expected {
  public:
    explicit expected(const T &v) : has_value_(true) { ::new (&value_) T(v); }
    explicit expected(T &&v) : has_value_(true) { ::new (&value_) T(std::move(v)); }
    // `has_value_` 的类内默认初值已是 false，错误态构造不再重复写；取错误载荷走 `&&` 重载，
    // 明确「从实参整体移动」而非只对其成员_cast。
    explicit expected(const unexpected<E> &u) { ::new (&error_) E(u.error()); }
    explicit expected(unexpected<E> &&u) { ::new (&error_) E(std::move(u).error()); }

    expected(const expected &o) : has_value_(o.has_value_) {
        if (o.has_value_) {
            ::new (&value_) T(o.value_);
        } else {
            ::new (&error_) E(o.error_);
        }
    }

    expected(expected &&o) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        : has_value_(o.has_value_) {
        if (o.has_value_) {
            ::new (&value_) T(std::move(o.value_));
        } else {
            ::new (&error_) E(std::move(o.error_));
        }
    }

    ~expected() {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }

    expected &operator=(const expected &o) {
        if (this != &o) {
            if (has_value_ && o.has_value_) {
                value_ = o.value_;
            } else if (!has_value_ && !o.has_value_) {
                error_ = o.error_;
            } else {
                destroy();
                has_value_ = o.has_value_;
                if (o.has_value_) {
                    ::new (&value_) T(o.value_);
                } else {
                    ::new (&error_) E(o.error_);
                }
            }
        }
        return *this;
    }

    expected &operator=(expected &&o) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                               std::is_nothrow_move_constructible_v<E>) {
        if (this != &o) {
            if (has_value_ && o.has_value_) {
                value_ = std::move(o.value_);
            } else if (!has_value_ && !o.has_value_) {
                error_ = std::move(o.error_);
            } else {
                destroy();
                has_value_ = o.has_value_;
                if (o.has_value_) {
                    ::new (&value_) T(std::move(o.value_));
                } else {
                    ::new (&error_) E(std::move(o.error_));
                }
            }
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }
    [[nodiscard]] auto has_value() const noexcept -> bool { return has_value_; }

    [[nodiscard]] auto value() const & -> const T & {
        AURORA_CHECK(has_value_, "expected::value() called on error state");  // 错误态继续取值 = UB，常开拦截
        return value_;
    }
    [[nodiscard]] T &value() & {
        AURORA_CHECK(has_value_, "expected::value() called on error state");  // 错误态继续取值 = UB，常开拦截
        return value_;
    }

    [[nodiscard]] auto error() const & -> const E & {
        AURORA_CHECK(!has_value_, "expected::error() called on value state");  // 值态继续取错误 = UB，常开拦截
        return error_;
    }

    [[nodiscard]] T value_or(T &&def) const & { return has_value_ ? value_ : std::move(def); }

  private:
    auto destroy() noexcept -> void {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }

    bool has_value_ = false;
    union {
        T value_;
        E error_;
    };
};
// NOLINTEND(cppcoreguidelines-pro-type-union-access, readability-identifier-naming)

}  // namespace aurora
