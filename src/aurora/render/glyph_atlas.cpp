#include "aurora/render/glyph_atlas.h"

namespace aurora::render {

auto GlyphAtlas::touch(Record &record) -> const Entry * {
    record.last_used = ++use_clock_;
    return &record.entry;
}

auto GlyphAtlas::find_record(std::uint64_t key) -> Record * {
    // 未命中→插入→再查找是字形光栅化的固定序列：命中缓存的条目指针时
    // 免去一次哈希查找（淘汰/清空会同步置空该缓存）。
    if (last_hit_ != nullptr && last_hit_key_ == key) {
        touch(*last_hit_);
        return last_hit_;
    }
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return nullptr;
    }
    last_hit_key_ = key;
    last_hit_ = &it->second;
    touch(*last_hit_);
    return last_hit_;
}

auto GlyphAtlas::find(std::uint64_t key) -> const Entry * {
    const Record *record = find_record(key);
    return record != nullptr ? &record->entry : nullptr;
}

auto GlyphAtlas::insert(std::uint64_t key, Entry entry) -> const Entry * {
    const std::uint64_t stamp = ++use_clock_;
    // 任意插入都可能淘汰既有记录（新键溢出或同键覆盖），缓存方持有的句柄据此整体作废。
    ++generation_;
    auto it = map_.find(key);
    if (it == map_.end()) {
        it = map_.emplace(key, Record{.entry = std::move(entry), .last_used = stamp}).first;
    } else {
        it->second.entry = std::move(entry);
        it->second.last_used = stamp;
    }
    if (map_.size() > AURORA_MAX_ENTRIES) {
        evict_oldest();
    }
    // 新插入条目序号最大，不会被上面的淘汰选中，故 it 仍然有效。
    last_hit_key_ = key;
    last_hit_ = &it->second;
    return &it->second.entry;
}

auto GlyphAtlas::clear() -> void {
    map_.clear();
    use_clock_ = 0;
    last_hit_ = nullptr;
    last_hit_key_ = 0;
    // 世代保持单调（不复位为 0）：清空后旧句柄即使世代号巧合相等也不得复活。
    ++generation_;
}

auto GlyphAtlas::evict_oldest() -> void {
    // 冷路径：仅在插入把条目数顶出容量时调用一次。线性扫描换取命中路径的「一次标量写」
    // （理由见头文件 Record 的注释：命中远多于溢出插入，堆化会把比较搬到热路径上）。
    auto oldest = map_.begin();
    for (auto it = map_.begin(); it != map_.end(); ++it) {
        if (it->second.last_used < oldest->second.last_used) {
            oldest = it;
        }
    }
    if (last_hit_ != nullptr && oldest->first == last_hit_key_) {
        last_hit_ = nullptr;
    }
    map_.erase(oldest);
}

namespace detail {

auto GlyphAtlasAccess::lookup(GlyphAtlas &atlas, std::uint64_t key, void *&record) -> const GlyphAtlas::Entry * {
    GlyphAtlas::Record *found = atlas.find_record(key);
    if (found == nullptr) {
        return nullptr;
    }
    record = found;
    return &found->entry;
}

}  // namespace detail

}  // namespace aurora::render
