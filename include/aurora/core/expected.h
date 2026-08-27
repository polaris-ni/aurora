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
template<typename E> class unexpected {
  public:
    explicit unexpected(const E &e) : m_error(e) {}
    explicit unexpected(E &&e) : m_error(std::move(e)) {}

    [[nodiscard]] const E &error() const & noexcept { return m_error; }
    [[nodiscard]] E &error() & noexcept { return m_error; }
    [[nodiscard]] E &&error() && noexcept { return std::move(m_error); }

  private:
    E m_error;
};

template<typename E> unexpected(E) -> unexpected<E>;

/**
 * @brief 最小化的 expected（二态：值或错误），满足 aurora::Result<T> 所需接口。
 *
 * 仅实现 v0.1 用到的路径：值构造、unexpected 构造、拷贝/移动、operator bool、
 * value()、error()、value_or()。完整 std::expected 语义后续可替换。
 */
template<typename T, typename E> class expected {
  public:
    explicit expected(const T &v) : m_has_value(true) { ::new (&m_value) T(v); }
    explicit expected(T &&v) : m_has_value(true) { ::new (&m_value) T(std::move(v)); }
    explicit expected(const unexpected<E> &u) : m_has_value(false) { ::new (&m_error) E(u.error()); }
    explicit expected(unexpected<E> &&u) : m_has_value(false) { ::new (&m_error) E(std::move(u.error())); }

    expected(const expected &o) : m_has_value(o.m_has_value) {
        if (o.m_has_value) {
            ::new (&m_value) T(o.m_value);
        } else {
            ::new (&m_error) E(o.m_error);
        }
    }

    expected(expected &&o) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        : m_has_value(o.m_has_value) {
        if (o.m_has_value) {
            ::new (&m_value) T(std::move(o.m_value));
        } else {
            ::new (&m_error) E(std::move(o.m_error));
        }
    }

    ~expected() {
        if (m_has_value) {
            m_value.~T();
        } else {
            m_error.~E();
        }
    }

    expected &operator=(const expected &o) {
        if (this != &o) {
            if (m_has_value && o.m_has_value) {
                m_value = o.m_value;
            } else if (!m_has_value && !o.m_has_value) {
                m_error = o.m_error;
            } else {
                destroy();
                m_has_value = o.m_has_value;
                if (o.m_has_value) {
                    ::new (&m_value) T(o.m_value);
                } else {
                    ::new (&m_error) E(o.m_error);
                }
            }
        }
        return *this;
    }

    expected &operator=(expected &&o) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                               std::is_nothrow_move_constructible_v<E>) {
        if (this != &o) {
            if (m_has_value && o.m_has_value) {
                m_value = std::move(o.m_value);
            } else if (!m_has_value && !o.m_has_value) {
                m_error = std::move(o.m_error);
            } else {
                destroy();
                m_has_value = o.m_has_value;
                if (o.m_has_value) {
                    ::new (&m_value) T(std::move(o.m_value));
                } else {
                    ::new (&m_error) E(std::move(o.m_error));
                }
            }
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return m_has_value; }
    [[nodiscard]] auto has_value() const noexcept -> bool { return m_has_value; }

    [[nodiscard]] auto value() const & -> const T & {
        AURORA_ASSERT(m_has_value, "expected::value() called on error state");
        return m_value;
    }
    [[nodiscard]] T &value() & {
        AURORA_ASSERT(m_has_value, "expected::value() called on error state");
        return m_value;
    }

    [[nodiscard]] auto error() const & -> const E & {
        AURORA_ASSERT(!m_has_value, "expected::error() called on value state");
        return m_error;
    }

    [[nodiscard]] T value_or(T &&def) const & { return m_has_value ? m_value : std::move(def); }

  private:
    auto destroy() noexcept -> void {
        if (m_has_value) {
            m_value.~T();
        } else {
            m_error.~E();
        }
    }

    bool m_has_value = false;
    union {
        T m_value;
        E m_error;
    };
};

} // namespace aurora
