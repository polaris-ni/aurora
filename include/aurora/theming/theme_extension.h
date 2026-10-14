#pragma once

#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace aurora {

/// @brief 主题扩展约束：应用自定义令牌须是可拷贝构造的值类型。
///
/// 对标 Flutter `ThemeExtension<T>`——但本库不要求继承基类、不带虚函数：扩展就是一个普通值
/// struct（如 `ChartPalette`），经 `ThemeExtensions` 以类型为键注册、随 `Theme` 值语义深拷贝。
/// @tparam T 候选扩展类型。
/// @return T 可拷贝构造时为 true。
template <class T>
concept ThemeExtension = std::is_copy_constructible_v<std::remove_cvref_t<T>>;

/// @brief 类型擦除、值语义的主题扩展注册表（`Theme` 的持有件；不含哈希容器）。
///
/// 以 `std::type_index` 为键、线性扫描查找（扩展数通常个位数，无哈希、无 unordered 容器，
/// 满足渲染路径「零哈希查找」约束）。空表不分配堆，故默认 `light()` / `dark()` 主题（无扩展）
/// 被拷贝时零额外开销。每个扩展按值持有：插入即 `new` 一份、拷贝时逐条深拷贝、析构时逐条释放，
/// 从而让宿主 `Theme` 保持纯值语义。
///
/// 豁免 bugprone-exception-escape：本类随 `Theme` 在控件回调网被值拷贝，拷贝构造里的逐条深拷贝
/// 仅可能因 `new` 抛 bad_alloc（进程级内存耗尽），由顶层兜底，非本类需就地吞掉的抛出面。
///
/// @note Thread: main-thread only (随 UI 单线程主题读写)
/// @note Side-effects: none (纯值容器)
/// NOLINTNEXTLINE(bugprone-exception-escape)
class ThemeExtensions {
  public:
    /// @brief 构造空注册表（零分配）。
    ThemeExtensions() = default;

    /// @brief 析构：逐条释放持有的扩展。
    ~ThemeExtensions() { clear(); }

    /// @brief 拷贝构造：逐条深拷贝源表持有的扩展（值语义）。
    /// @param other 源表。
    ThemeExtensions(const ThemeExtensions &other) {
        entries_.reserve(other.entries_.size());
        for (const auto &e : other.entries_) {
            entries_.push_back(e.copy());
        }
    }

    /// @brief 拷贝赋值：先深拷贝到临时表再交换，强异常安全。
    /// @param other 源表。
    /// @return 引用自身。
    auto operator=(const ThemeExtensions &other) -> ThemeExtensions & {
        if (this != &other) {
            ThemeExtensions tmp(other);
            swap(tmp);
        }
        return *this;
    }

    /// @brief 移动构造：窃取源表条目，源表转空。
    /// @param other 源表（移动后为空）。
    ThemeExtensions(ThemeExtensions &&other) noexcept : entries_(std::move(other.entries_)) { other.entries_.clear(); }

    /// @brief 移动赋值：释放自身后窃取源表，源表转空。
    /// @param other 源表（移动后为空）。
    /// @return 引用自身。
    auto operator=(ThemeExtensions &&other) noexcept -> ThemeExtensions & {
        if (this != &other) {
            clear();
            entries_ = std::move(other.entries_);
            other.entries_.clear();
        }
        return *this;
    }

    /// @brief 交换两表内容。
    /// @param other 对端表。
    auto swap(ThemeExtensions &other) noexcept -> void { entries_.swap(other.entries_); }

    /// @brief 注册/覆盖类型 T 的扩展（同类型整体替换）。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @param value 扩展值（移入，由本表持有其拷贝）。
    template <ThemeExtension T>
    auto set(T value) -> void {
        using D = std::remove_cvref_t<T>;
        remove<D>();
        Entry e;
        e.key = std::type_index(typeid(D));
        e.obj = new D(std::move(value));  // NOLINT(cppcoreguidelines-owning-memory)
        e.clone = [](const void *src) -> void * { return new D(*static_cast<const D *>(src)); };  // NOLINT
        e.drop = [](void *p) { delete static_cast<D *>(p); };  // NOLINT
        entries_.push_back(e);
    }

    /// @brief 读取类型 T 的扩展。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @return 指向本表持有的 `T` 的只读指针；未注册时为 `nullptr`（调用方显式判空，无字符串回落）。
    template <ThemeExtension T>
    [[nodiscard]] auto get() const -> const std::remove_cvref_t<T> * {
        using D = std::remove_cvref_t<T>;
        const std::type_index key(typeid(D));
        for (const auto &e : entries_) {
            if (e.key == key) {
                return static_cast<const D *>(e.obj);
            }
        }
        return nullptr;
    }

    /// @brief 移除类型 T 的扩展。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @return 曾存在并被移除时为 true，否则 false。
    template <ThemeExtension T>
    auto remove() -> bool {
        using D = std::remove_cvref_t<T>;
        const std::type_index key(typeid(D));
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key == key) {
                it->drop(it->obj);
                entries_.erase(it);
                return true;
            }
        }
        return false;
    }

  private:
    /// @brief 类型擦除持有项：typeid 键、对象指针、克隆子与释放子（按插入类型特化）。
    struct Entry {
        std::type_index key = std::type_index(typeid(void));  ///< 扩展类型的 typeid 键。
        void *obj = nullptr;  ///< 持有的扩展对象（本表所有）。
        void *(*clone)(const void *) = nullptr;  ///< 深拷贝 obj，产出新 obj 指针。
        void (*drop)(void *) = nullptr;  ///< 释放 obj。

        /// @brief 产出一个持有 obj 深拷贝的新项（复用同型 clone/drop）。
        /// @return 深拷贝后的 Entry。
        [[nodiscard]] auto copy() const -> Entry {
            return Entry{.key = key, .obj = clone != nullptr ? clone(obj) : nullptr, .clone = clone, .drop = drop};
        }
    };

    std::vector<Entry> entries_;  ///< 类型擦除的持有项列表。

    /// @brief 释放全部项并清空。
    auto clear() -> void {
        for (const auto &e : entries_) {
            if (e.drop != nullptr) {
                e.drop(e.obj);
            }
        }
        entries_.clear();
    }
};

}  // namespace aurora
