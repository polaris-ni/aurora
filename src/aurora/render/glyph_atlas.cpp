#include "aurora/render/glyph_atlas.h"

namespace aurora::render {

auto GlyphAtlas::find(std::uint64_t key) -> const Entry * {
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return nullptr;
    }
    touch(key);
    return &it->second;
}

auto GlyphAtlas::insert(std::uint64_t key, Entry entry) -> void {
    map_[key] = std::move(entry);
    // 若已存在则先移除旧位置
    const auto map_it = lru_map_.find(key);
    if (map_it != lru_map_.end()) {
        lru_.erase(map_it->second);
        map_it->second = lru_.end();
    }
    lru_.push_front(key);
    lru_map_[key] = lru_.begin();
    if (lru_.size() > AURORA_MAX_ENTRIES) {
        const std::uint64_t old = lru_.back();
        lru_.pop_back();
        lru_map_.erase(old);
        map_.erase(old);
    }
}

auto GlyphAtlas::clear() -> void {
    map_.clear();
    lru_.clear();
    lru_map_.clear();
}

auto GlyphAtlas::touch(std::uint64_t key) const -> void {
    const auto it = lru_map_.find(key);
    if (it == lru_map_.end()) {
        return;
    }
    lru_.erase(it->second);
    lru_.push_front(key);
    it->second = lru_.begin();
}

}  // namespace aurora::render
