#include "aurora/render/glyph_atlas.h"

namespace aurora::render {

auto GlyphAtlas::find(std::uint64_t key) -> const Entry * {
    // 未命中→插入→再查找是字形光栅化的固定序列：命中缓存的条目指针时
    // 免去一次哈希查找（淘汰/清空会同步置空该缓存）。
    if (last_hit_ != nullptr && last_hit_key_ == key) {
        last_hit_->last_used = ++use_clock_;
        return &last_hit_->entry;
    }
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return nullptr;
    }
    it->second.last_used = ++use_clock_;
    last_hit_key_ = key;
    last_hit_ = &it->second;
    return &it->second.entry;
}

auto GlyphAtlas::insert(std::uint64_t key, Entry entry) -> const Entry * {
    const std::uint64_t stamp = ++use_clock_;
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
}

auto GlyphAtlas::evict_oldest() -> void {
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

}  // namespace aurora::render
