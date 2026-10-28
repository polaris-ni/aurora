#include "aurora/state/detail/registry.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace aurora::detail {
namespace {

/// @brief 弱引用名册：只探测存活，不延长对象寿命。
/// @tparam T 被登记对象的类型。
template <typename T>
class WeakRoster {
  public:
    /// @brief 登记一个对象。
    /// @param object 对象的共享句柄（只取其弱引用）。
    auto add(const std::shared_ptr<T> &object) -> void {
        entries_.push_back(object);
        ++since_compact_;
        // 摊还压缩：自上次整理以来新增的条目达到表长一半时整理一次。表内多数存活时
        // 整理不动表长，但要隔一半表长才再触发，故单次登记摊还代价为 O(1)；
        // 表内多数已析构时整理会显著缩短表长，把「只增不减」压回与存活数同阶。
        if (since_compact_ * 2 >= entries_.size()) {
            compact();
        }
    }

    /// @brief 存活对象快照（附带整理失效条目）。
    /// @return 存活对象的共享句柄列表，顺序为登记顺序。
    [[nodiscard]] auto live() -> std::vector<std::shared_ptr<T>> {
        compact();
        std::vector<std::shared_ptr<T>> out;
        out.reserve(entries_.size());
        for (const auto &entry : entries_) {
            if (auto locked = entry.lock()) {
                out.push_back(std::move(locked));
            }
        }
        return out;
    }

  private:
    /// @brief 剔除失效条目并重置摊还计数。
    auto compact() -> void {
        const auto removed = std::remove_if(entries_.begin(), entries_.end(),
                                            [](const std::weak_ptr<T> &entry) -> bool { return entry.expired(); });
        entries_.erase(removed, entries_.end());
        since_compact_ = 0;
    }

    std::vector<std::weak_ptr<T>> entries_;  ///< 弱引用条目（登记顺序）
    std::size_t since_compact_ = 0;  ///< 上次整理以来新增的条目数
};

/// @brief 来源名册（进程级单例，首次调用惰性构造）。
/// @return 名册引用。
auto source_roster() -> WeakRoster<SourceNode> & {
    static WeakRoster<SourceNode> roster;
    return roster;
}

/// @brief 终端名册（进程级单例，首次调用惰性构造）。
/// @return 名册引用。
auto terminal_roster() -> WeakRoster<RunRecord> & {
    static WeakRoster<RunRecord> roster;
    return roster;
}

}  // namespace

auto register_source(const std::shared_ptr<SourceNode> &node) -> void {
    if (node) {
        source_roster().add(node);
    }
}

auto register_terminal(const std::shared_ptr<RunRecord> &record) -> void {
    if (record) {
        terminal_roster().add(record);
    }
}

auto live_sources() -> std::vector<std::shared_ptr<SourceNode>> { return source_roster().live(); }

auto live_terminals() -> std::vector<std::shared_ptr<RunRecord>> { return terminal_roster().live(); }

}  // namespace aurora::detail
