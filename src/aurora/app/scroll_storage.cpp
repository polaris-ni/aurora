#include "aurora/app/scroll_storage.h"

#include <utility>

#include "aurora/core/diagnostics.h"

namespace aurora {

namespace {

/// 作用域与键的分隔符：取不可打印控制符（US，Unit Separator），避免与用户键内容冲突。
constexpr char AURORA_SCOPE_SEP = '\x1f';

}  // namespace

auto ScrollStorage::instance() -> ScrollStorage & {
    // 单线程 UI：单例无需加锁（与 Preferences 的跨线程加锁策略不同，见类注释）。
    static ScrollStorage storage;
    return storage;
}

// ---------- Scope ----------

ScrollStorage::Scope::Scope(std::string_view scope) {
    auto &current = instance().scope_;
    if (current == scope) {
        return;  // 同值（含「以 current_scope() 自身为参数」的别名情形）：不动、不分配
    }
    saved_ = current;  // 拷贝而非移动：scope 可能视图到 current 的缓冲
    current.assign(scope);
    changed_ = true;
}

// 析构体只做「把保存的作用域串移动回单例」：std::string 的移动赋值按标准在分配器
// is_always_equal（std::allocator 即如此）时为 noexcept。告警来自 instance() 的函数内
// static 惰性构造可能 bad_alloc——而 Scope 构造已经调用过它，析构期不会再触发初始化。
// NOLINTNEXTLINE(bugprone-exception-escape)
ScrollStorage::Scope::~Scope() {
    if (changed_) {
        instance().scope_ = std::move(saved_);
    }
}

// ---------- 会话内读写 ----------

auto ScrollStorage::scoped_key(std::string_view key) const -> std::string {
    if (scope_.empty()) {
        return std::string(key);
    }
    std::string out;
    out.reserve(scope_.size() + 1U + key.size());
    out += scope_;
    out += AURORA_SCOPE_SEP;
    out.append(key);
    return out;
}

auto ScrollStorage::write(std::string_view key, float offset) -> void {
    const std::string k = scoped_key(key);
    offsets_[k] = offset;  // 键已存在时无重新分配
    dirty_[k] = offset;  // 待落盘（未 attach 时也有界：条目数 ≤ 键数）
    removed_.erase(k);  // 覆盖写撤销此前可能存在的墓碑
}

auto ScrollStorage::read(std::string_view key) -> std::optional<float> {
    const std::string k = scoped_key(key);
    if (const auto it = offsets_.find(k); it != offsets_.end()) {
        return it->second;
    }
    if (prefs_ == nullptr) {
        return std::nullopt;
    }
    // 懒回读：Preferences 中已有记录时填充内存缓存（**不**计入待落盘项——它已在那里）。
    auto group = prefs_->group(AURORA_GROUP_NAME);
    if (!group.contains(k)) {
        return std::nullopt;
    }
    const auto stored = group.get<float>(k, 0.0F);
    offsets_[k] = stored;
    return stored;
}

auto ScrollStorage::clear(std::string_view key) -> void {
    const std::string k = scoped_key(key);
    offsets_.erase(k);
    dirty_.erase(k);
    removed_.insert(k);  // 墓碑：sync 时从 Preferences 侧删除
}

auto ScrollStorage::clear_all() -> void {
    offsets_.clear();
    dirty_.clear();
    removed_.clear();
    owners_.clear();
    warned_.clear();
    prefs_ = nullptr;
    scope_.clear();
}

// ---------- 持久化 ----------

auto ScrollStorage::attach(preferences::Preferences &prefs) -> void { prefs_ = &prefs; }

auto ScrollStorage::detach() -> void { prefs_ = nullptr; }

auto ScrollStorage::sync() -> void {
    if (prefs_ == nullptr) {
        return;  // 无落盘目标：保留待落盘队列（attach 后再提交），不丢内存值
    }
    auto group = prefs_->group(AURORA_GROUP_NAME);
    for (const auto &[k, v] : dirty_) {
        group.set(k, v);  // 仅写 Preferences 内存 + 通知订阅者；文件落盘由 App 的 flush() 决定
    }
    for (const auto &k : removed_) {
        group.remove(k);
    }
    dirty_.clear();
    removed_.clear();
}

// ---------- 键认领 ----------

auto ScrollStorage::claim(std::string_view key, const void *owner) -> void {
    const std::string k = scoped_key(key);
    auto &owner_set = owners_[k];
    owner_set.insert(owner);
    if (owner_set.size() > 1U && !warned_.contains(k)) {
        warned_.insert(k);  // 每键一次：不在滚动热路径上重复刷屏
        Diagnostics::warn("同一个 restore_key 被多个滚动控件认领，恢复位置将以最后活跃者为准", "scroll_storage");
    }
}

auto ScrollStorage::release(std::string_view key, const void *owner) -> void {
    const std::string k = scoped_key(key);
    const auto it = owners_.find(k);
    if (it == owners_.end()) {
        return;
    }
    it->second.erase(owner);
    if (it->second.empty()) {
        owners_.erase(it);  // warned_ 保留：提示按「每键一次」去重，释放后不再重复提示
    }
}

auto ScrollStorage::owner_count(std::string_view key) const -> std::size_t {
    const std::string k = scoped_key(key);
    const auto it = owners_.find(k);
    return it == owners_.end() ? 0U : it->second.size();
}

}  // namespace aurora
