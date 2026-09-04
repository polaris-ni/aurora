#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "aurora/core/image.h"
#include "aurora/core/result.h"

namespace aurora {

/**
 * @brief LRU 图片解码缓存（specification/03-layout-render.md §8.4）：按路径缓存解码后的 `Image`。
 *
 * `get(path)` 命中直接返回（O(1) 提升到最近使用）；未命中经 `Image::load`
 * 解码后入缓存。内存上限 `set_max_bytes`（默认 64MB），超限 LRU 淘汰。
 * 线程安全（互斥锁保护；解码在锁外执行避免阻塞并发命中）。
 *
 * 对标 Flutter `ImageCache`、Qt `QPixmapCache`。
 */
class ImageCache {
  public:
    /// @brief 进程级单例。
    [[nodiscard]] static auto instance() -> ImageCache & {
        static ImageCache cache;
        return cache;
    }

    ImageCache() = default;

    /// @brief 取图（命中返回缓存；未命中解码并缓存）。失败返回 Error（不缓存失败结果）。
    [[nodiscard]] auto get(const std::string &path) -> Result<Image> {
        {
            std::scoped_lock lock(mutex_);
            const auto it = index_.find(path);
            if (it != index_.end()) {
                // LRU 提升：移动到链表头
                lru_.splice(lru_.begin(), lru_, it->second);
                ++hits_;
                return it->second->image;
            }
        }
        // 解码在锁外（避免大图解码阻塞其他线程命中查询）
        auto loaded = Image::load(path);
        if (!loaded) {
            return loaded;
        }
        put(path, loaded.value());
        return loaded;
    }

    /// @brief 手动放入缓存（覆盖同 key；超限触发 LRU 淘汰）。
    auto put(const std::string &path, Image img) -> void {
        const std::size_t bytes = image_bytes(img);
        std::scoped_lock lock(mutex_);
        // 覆盖旧条目
        const auto it = index_.find(path);
        if (it != index_.end()) {
            bytes_ -= image_bytes(it->second->image);
            lru_.erase(it->second);
            index_.erase(it);
        }
        // 单图超过总上限：不缓存（直接由调用方持有）
        if (bytes > max_bytes_) {
            return;
        }
        lru_.push_front(Entry{.key = path, .image = std::move(img)});
        index_[path] = lru_.begin();
        bytes_ += bytes;
        evict_if_needed();
    }

    /// @brief 是否已缓存。
    [[nodiscard]] auto contains(const std::string &path) -> bool {
        std::scoped_lock lock(mutex_);
        return index_.contains(path);
    }

    /// @brief 移除指定条目。
    auto remove(const std::string &path) -> void {
        std::scoped_lock lock(mutex_);
        const auto it = index_.find(path);
        if (it != index_.end()) {
            bytes_ -= image_bytes(it->second->image);
            lru_.erase(it->second);
            index_.erase(it);
        }
    }

    /// @brief 清空缓存。
    auto clear() -> void {
        std::scoped_lock lock(mutex_);
        lru_.clear();
        index_.clear();
        bytes_ = 0;
    }

    /// @brief 设置内存上限（字节；立即按新上限淘汰）。
    auto set_max_bytes(std::size_t max_bytes) -> void {
        std::scoped_lock lock(mutex_);
        max_bytes_ = max_bytes;
        evict_if_needed();
    }

    [[nodiscard]] auto max_bytes() const -> std::size_t { return max_bytes_; }

    /// @brief 当前缓存占用字节数。
    [[nodiscard]] auto current_bytes() -> std::size_t {
        std::scoped_lock lock(mutex_);
        return bytes_;
    }

    /// @brief 当前缓存条目数。
    [[nodiscard]] auto count() -> std::size_t {
        std::scoped_lock lock(mutex_);
        return index_.size();
    }

    /// @brief 命中次数（诊断/性能覆盖层用）。
    [[nodiscard]] auto hit_count() const -> std::size_t { return hits_; }

  private:
    struct Entry {
        std::string key;
        Image image;
    };

    [[nodiscard]] static auto image_bytes(const Image &img) -> std::size_t { return img.pixels.size(); }

    /// @brief 淘汰到上限内（须已持锁）。
    auto evict_if_needed() -> void {
        while (bytes_ > max_bytes_ && !lru_.empty()) {
            const Entry &tail = lru_.back();
            bytes_ -= image_bytes(tail.image);
            index_.erase(tail.key);
            lru_.pop_back();
        }
    }

    static constexpr std::size_t AURORA_DEFAULT_MAX = 64ULL * 1024 * 1024;  ///< 默认 64MB

    std::mutex mutex_;
    std::list<Entry> lru_;  ///< 头 = 最近使用
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
    std::size_t bytes_ = 0;
    std::size_t max_bytes_ = AURORA_DEFAULT_MAX;
    std::size_t hits_ = 0;
};

}  // namespace aurora
