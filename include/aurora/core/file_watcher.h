#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aurora {

/// @brief 文件变化类型。
enum class FileChange : std::uint8_t {
    Modified, ///< 内容修改（mtime/大小变化）
    Created,  ///< 新出现
    Removed,  ///< 被删除
};

/**
 * @brief 文件监视器（specification/01-core.md §7）：轮询式文件系统变化监听。
 *
 * 显式 `poll()` 驱动（可挂到帧循环 / Scheduler::set_interval），确定性、
 * 跨平台（std::filesystem），无后台线程 —— 与单线程 UI 模型契合。
 * 热重载 / 配置自动重载 / 资源热更新的基础。
 *
 * 对标 Qt `QFileSystemWatcher`、.NET `FileSystemWatcher`（轮询模式）。
 */
class FileWatcher {
  public:
    using ChangeCallback = std::function<void(const std::string &path, FileChange change)>;

    FileWatcher() = default;
    explicit FileWatcher(ChangeCallback on_change) : m_on_change(std::move(on_change)) {}

    /// @brief 监视一个文件（立即记录当前状态为基线）。
    auto watch(const std::string &path) -> void {
        std::error_code ec;
        Entry e;
        e.exists = std::filesystem::exists(path, ec) && !ec;
        if (e.exists) {
            e.mtime = std::filesystem::last_write_time(path, ec);
            e.size = std::filesystem::file_size(path, ec);
        }
        m_entries[path] = e;
    }

    /// @brief 停止监视。
    auto unwatch(const std::string &path) -> void { m_entries.erase(path); }

    /// @brief 监视的文件数。
    [[nodiscard]] auto count() const -> std::size_t { return m_entries.size(); }

    /// @brief 设置变化回调。
    auto set_on_change(ChangeCallback cb) -> void { m_on_change = std::move(cb); }

    /// @brief 轮询一次：检测全部监视项的变化，触发回调并返回变化列表。
    auto poll() -> std::vector<std::pair<std::string, FileChange>> {
        std::vector<std::pair<std::string, FileChange>> changes;
        for (auto &kv : m_entries) {
            const std::string &path = kv.first;
            Entry &e = kv.second;
            std::error_code ec;
            const bool now_exists = std::filesystem::exists(path, ec) && !ec;

            if (!e.exists && now_exists) {
                e.exists = true;
                e.mtime = std::filesystem::last_write_time(path, ec);
                e.size = std::filesystem::file_size(path, ec);
                changes.emplace_back(path, FileChange::Created);
            } else if (e.exists && !now_exists) {
                e.exists = false;
                changes.emplace_back(path, FileChange::Removed);
            } else if (e.exists && now_exists) {
                const auto mtime = std::filesystem::last_write_time(path, ec);
                const auto size = std::filesystem::file_size(path, ec);
                if (mtime != e.mtime || size != e.size) {
                    e.mtime = mtime;
                    e.size = size;
                    changes.emplace_back(path, FileChange::Modified);
                }
            }
        }
        if (m_on_change) {
            for (const auto &[fst, snd] : changes) {
                m_on_change(fst, snd);
            }
        }
        return changes;
    }

  private:
    struct Entry {
        bool exists = false;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
    };

    std::map<std::string, Entry> m_entries;
    ChangeCallback m_on_change;
};

} // namespace aurora
