#include "aurora/app/recent_documents.h"

#include <algorithm>
#include <utility>

#include "aurora/core/json.h"

namespace aurora {

namespace {

/// 信封内承载路径数组的键名：分组下只此一键，顺序由数组本身承载（不用索引当键）。
constexpr char AURORA_DOCUMENTS_KEY[] = "documents";

}  // namespace

auto RecentDocuments::instance() -> RecentDocuments & {
    // 单线程 UI：单例无需加锁（与 Preferences 的跨线程加锁策略不同，见类注释）。
    static RecentDocuments store;
    return store;
}

// ---------- 会话内读写 ----------

auto RecentDocuments::add(std::string_view path) -> void {
    if (path.empty()) {
        return;  // 空路径 no-op（不产生待落盘标记）
    }
    ensure_loaded();
    std::string entry{path};  // 非 const：下方置顶时移入列表，const 会让 std::move 退化成拷贝
    if (const auto existing = std::ranges::find(documents_, entry); existing != documents_.end()) {
        documents_.erase(existing);  // 去重：先摘除旧位置，随后统一插到队首
    } else if (documents_.size() >= AURORA_MAX_ENTRIES) {
        documents_.pop_back();  // 超容量：淘汰最旧（列表尾部）
    }
    documents_.insert(documents_.begin(), std::move(entry));  // 置顶：新 → 旧序
    removed_ = false;  // 新增撤销此前可能存在的墓碑
    dirty_ = true;
}

auto RecentDocuments::list() -> std::vector<std::string> {
    ensure_loaded();
    return documents_;
}

auto RecentDocuments::clear() -> void {
    documents_.clear();
    dirty_ = false;
    removed_ = true;  // 墓碑：sync 时从 Preferences 侧删除信封键
}

auto RecentDocuments::size() -> std::size_t {
    ensure_loaded();
    return documents_.size();
}

auto RecentDocuments::clear_all() -> void {
    documents_.clear();
    loaded_ = false;
    dirty_ = false;
    removed_ = false;
    prefs_ = nullptr;
}

// ---------- 持久化 ----------

auto RecentDocuments::attach(preferences::Preferences &prefs) -> void { prefs_ = &prefs; }

auto RecentDocuments::detach() -> void { prefs_ = nullptr; }

auto RecentDocuments::sync() -> void {
    if (prefs_ == nullptr) {
        return;  // 无落盘目标：保留待落盘标记（attach 后再提交），不丢内存列表
    }
    auto group = prefs_->group(AURORA_GROUP_NAME);
    if (removed_) {
        // 墓碑：删除信封键。读缺失即空列表，故「删键」与「写空数组」等价，取删键以免留下空信封。
        group.remove(AURORA_DOCUMENTS_KEY);
    } else if (dirty_) {
        auto envelope = json::Value::array();
        envelope.reserve(documents_.size());
        for (const auto &path : documents_) {
            envelope.push_back(json::Value(path));
        }
        group.set(AURORA_DOCUMENTS_KEY, std::move(envelope));  // 仅写 Preferences 内存 + 通知订阅者
    }
    dirty_ = false;
    removed_ = false;
}

auto RecentDocuments::ensure_loaded() -> void {
    if (loaded_ || prefs_ == nullptr) {
        return;  // 已回读过，或尚无后端（纯内存会话）：保持现状
    }
    if (dirty_ || removed_) {
        return;  // 本会话已改动且未 sync：内存为准，不让后端覆盖本会话结果
    }
    loaded_ = true;  // 无论读得到什么都只回读一次
    const auto stored = prefs_->group(AURORA_GROUP_NAME).get<json::Value>(AURORA_DOCUMENTS_KEY, json::Value());
    if (!stored.is_array()) {
        return;  // 读缺失 / 信封损坏（非数组）：空列表，不抛错、不清用户数据
    }
    documents_.clear();
    documents_.reserve(std::min(stored.size(), AURORA_MAX_ENTRIES));
    // json 数组的 begin() / end() 是 `const Value *`，按范围 for 遍历即可（不手写下标推进）。
    for (const auto &element : stored) {
        const auto text = element.as_string();
        if (!text.has_value()) {
            continue;  // 元素非字符串：跳过该条（宽容降级，其余条目照旧）
        }
        if (documents_.size() >= AURORA_MAX_ENTRIES) {
            break;  // 后端条目多于容量上限时截断，与写入侧口径一致
        }
        documents_.emplace_back(*text);
    }
}

}  // namespace aurora
