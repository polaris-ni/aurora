#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "aurora/core/assert.h"

namespace aurora {

/**
 * @brief 携带错误值的包装（参考 std::unexpected，C++23 前本库自带实现）。
 * @tparam E 错误类型（本库为 aurora::Error）。
 */
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
    explicit expected(const unexpected<E> &u) : has_value_(false) { ::new (&error_) E(u.error()); }
    explicit expected(unexpected<E> &&u) : has_value_(false) { ::new (&error_) E(std::move(u.error())); }

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
        AURORA_ASSERT(has_value_, "expected::value() called on error state");
        return value_;
    }
    [[nodiscard]] T &value() & {
        AURORA_ASSERT(has_value_, "expected::value() called on error state");
        return value_;
    }

    [[nodiscard]] auto error() const & -> const E & {
        AURORA_ASSERT(!has_value_, "expected::error() called on value state");
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

}  // namespace aurora
