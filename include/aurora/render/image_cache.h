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
 * @brief LRU 图片解码缓存（规格 §2.3）：按路径缓存解码后的 `Image`。
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
            std::scoped_lock lock(m_mutex);
            const auto it = m_index.find(path);
            if (it != m_index.end()) {
                // LRU 提升：移动到链表头
                m_lru.splice(m_lru.begin(), m_lru, it->second);
                ++m_hits;
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
        std::scoped_lock lock(m_mutex);
        // 覆盖旧条目
        const auto it = m_index.find(path);
        if (it != m_index.end()) {
            m_bytes -= image_bytes(it->second->image);
            m_lru.erase(it->second);
            m_index.erase(it);
        }
        // 单图超过总上限：不缓存（直接由调用方持有）
        if (bytes > m_max_bytes) {
            return;
        }
        m_lru.push_front(Entry{ .key = path, .image = std::move(img) });
        m_index[path] = m_lru.begin();
        m_bytes += bytes;
        evict_if_needed();
    }

    /// @brief 是否已缓存。
    [[nodiscard]] auto contains(const std::string &path) -> bool {
        std::scoped_lock lock(m_mutex);
        return m_index.contains(path);
    }

    /// @brief 移除指定条目。
    auto remove(const std::string &path) -> void {
        std::scoped_lock lock(m_mutex);
        const auto it = m_index.find(path);
        if (it != m_index.end()) {
            m_bytes -= image_bytes(it->second->image);
            m_lru.erase(it->second);
            m_index.erase(it);
        }
    }

    /// @brief 清空缓存。
    auto clear() -> void {
        std::scoped_lock lock(m_mutex);
        m_lru.clear();
        m_index.clear();
        m_bytes = 0;
    }

    /// @brief 设置内存上限（字节；立即按新上限淘汰）。
    auto set_max_bytes(std::size_t max_bytes) -> void {
        std::scoped_lock lock(m_mutex);
        m_max_bytes = max_bytes;
        evict_if_needed();
    }

    [[nodiscard]] auto max_bytes() const -> std::size_t { return m_max_bytes; }

    /// @brief 当前缓存占用字节数。
    [[nodiscard]] auto current_bytes() -> std::size_t {
        std::scoped_lock lock(m_mutex);
        return m_bytes;
    }

    /// @brief 当前缓存条目数。
    [[nodiscard]] auto count() -> std::size_t {
        std::scoped_lock lock(m_mutex);
        return m_index.size();
    }

    /// @brief 命中次数（诊断/性能覆盖层用）。
    [[nodiscard]] auto hit_count() const -> std::size_t { return m_hits; }

  private:
    struct Entry {
        std::string key;
        Image image;
    };

    [[nodiscard]] static auto image_bytes(const Image &img) -> std::size_t { return img.pixels.size(); }

    /// @brief 淘汰到上限内（须已持锁）。
    auto evict_if_needed() -> void {
        while (m_bytes > m_max_bytes && !m_lru.empty()) {
            const Entry &tail = m_lru.back();
            m_bytes -= image_bytes(tail.image);
            m_index.erase(tail.key);
            m_lru.pop_back();
        }
    }

    static constexpr std::size_t AURORA_DEFAULT_MAX = 64ULL * 1024 * 1024; ///< 默认 64MB

    std::mutex m_mutex;
    std::list<Entry> m_lru; ///< 头 = 最近使用
    std::unordered_map<std::string, std::list<Entry>::iterator> m_index;
    std::size_t m_bytes = 0;
    std::size_t m_max_bytes = AURORA_DEFAULT_MAX;
    std::size_t m_hits = 0;
};

} // namespace aurora
