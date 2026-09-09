/// 测试类型: unit
/// 目标单元: include/aurora/core/file_watcher.h
/// 测试说明: 轮询式文件监视器的基线记录与计数、Created/Removed/Modified（按大小变化与仅 mtime
/// 变化两条路径）检测、回调携带路径与变化类型、unwatch 停止上报（用框架隔离临时目录 + 截止时间轮询保证确定性）

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/file_watcher.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_file_watcher {

namespace fs = std::filesystem;

namespace {
/// @brief 在框架隔离的临时目录（temp_directory_path 经 TMP 环境变量接管）下创建
/// 本用例唯一子目录，析构时兜底清理。
class ScopedTempDir {
  public:
    explicit ScopedTempDir(const std::string& name) : path_{fs::temp_directory_path() / name} {
        std::error_code ec;
        fs::remove_all(path_, ec);  // 兜底清残留
        fs::create_directories(path_);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);  // 清理失败不影响用例结果
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    auto operator=(const ScopedTempDir&) -> ScopedTempDir& = delete;
    ScopedTempDir(ScopedTempDir&&) = delete;
    auto operator=(ScopedTempDir&&) -> ScopedTempDir& = delete;

    [[nodiscard]] auto file(const std::string& name) const -> std::string { return (path_ / name).string(); }

  private:
    fs::path path_;
};
}  // namespace

/// @brief 覆盖写文本文件。
static auto write_text(const std::string& path, const std::string& content) -> void {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// @brief 追加写文本文件（保证文件大小变化）。
static auto append_text(const std::string& path, const std::string& content) -> void {
    std::ofstream out{path, std::ios::binary | std::ios::app};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// @brief 截止时间轮询：最多 5s、每 10ms 一次，条件成立即返回 true，超时返回 false（由调用方 CHECK 判失败）。
/// 以 const 左值引用接收谓词：cond() 在循环内多次调用，转发引用的 move 语义不适用。
template <typename Cond>
static auto wait_until(const Cond& cond) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return cond();
}

AURORA_TEST_CASE(watch_records_baseline_and_count) {
    const ScopedTempDir dir{"aurora_utest_fw_baseline"};
    FileWatcher watcher;
    AURORA_TEST_CHECK_EQ(watcher.count(), std::size_t{0});

    const auto file = dir.file("a.txt");
    write_text(file, "v1");
    watcher.watch(file);  // 立即记录当前状态为基线
    AURORA_TEST_CHECK_EQ(watcher.count(), std::size_t{1});

    // 基线一致：首次 poll 无变化。
    AURORA_TEST_CHECK(watcher.poll().empty());

    watcher.unwatch(file);
    AURORA_TEST_CHECK_EQ(watcher.count(), std::size_t{0});
}

AURORA_TEST_CASE(poll_detects_created_file) {
    const ScopedTempDir dir{"aurora_utest_fw_created"};
    FileWatcher watcher;
    const auto file = dir.file("later.txt");
    watcher.watch(file);  // 监视尚不存在的路径：基线为“不存在”

    write_text(file, "born");
    const auto changes = watcher.poll();
    AURORA_TEST_REQUIRE_EQ(changes.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(changes[0].first, file);
    AURORA_TEST_CHECK(changes[0].second == FileChange::Created);

    // 状态已同步：不重复上报。
    AURORA_TEST_CHECK(watcher.poll().empty());
}

AURORA_TEST_CASE(poll_detects_removed_file) {
    const ScopedTempDir dir{"aurora_utest_fw_removed"};
    FileWatcher watcher;
    const auto file = dir.file("gone.txt");
    write_text(file, "doomed");
    watcher.watch(file);
    AURORA_TEST_CHECK(watcher.poll().empty());  // 同步基线

    std::error_code ec;
    fs::remove(file, ec);
    const auto changes = watcher.poll();
    AURORA_TEST_REQUIRE_EQ(changes.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(changes[0].first, file);
    AURORA_TEST_CHECK(changes[0].second == FileChange::Removed);
}

AURORA_TEST_CASE(poll_detects_modified_by_size_change) {
    const ScopedTempDir dir{"aurora_utest_fw_modified"};
    FileWatcher watcher;
    const auto file = dir.file("grow.txt");
    write_text(file, "v1");
    watcher.watch(file);
    AURORA_TEST_CHECK(watcher.poll().empty());

    append_text(file, "-longer");  // 大小变化：必然触发 Modified（无需等待时间戳粒度）
    const auto changes = watcher.poll();
    AURORA_TEST_REQUIRE_EQ(changes.size(), std::size_t{1});
    AURORA_TEST_CHECK(changes[0].second == FileChange::Modified);
    AURORA_TEST_CHECK(watcher.poll().empty());
}

AURORA_TEST_CASE(poll_detects_modified_by_mtime_only_with_deadline) {
    const ScopedTempDir dir{"aurora_utest_fw_mtime"};
    FileWatcher watcher;
    const auto file = dir.file("same-size.txt");
    write_text(file, "same-size-a");
    watcher.watch(file);
    AURORA_TEST_CHECK(watcher.poll().empty());

    // 等长改写：大小不变，仅 mtime 变化。文件系统时间戳有粒度（NTFS 100ns / FAT 最粗 2s），
    // 用截止时间循环反复改写并轮询（最多 5s、每 10ms 一次），超时即 CHECK 失败。
    const bool detected = wait_until([&]() -> bool {
        write_text(file, "same-size-b");
        return !watcher.poll().empty();
    });
    AURORA_TEST_CHECK_TRUE(detected);
}

AURORA_TEST_CASE(callback_receives_path_and_change_kind) {
    const ScopedTempDir dir{"aurora_utest_fw_callback"};
    std::vector<std::pair<std::string, FileChange>> seen;
    FileWatcher watcher{
        [&seen](const std::string& path, FileChange change) -> void { seen.emplace_back(path, change); }};

    const auto file = dir.file("cb.txt");
    watcher.watch(file);  // 不存在基线
    write_text(file, "hello");
    AURORA_TEST_CHECK(!watcher.poll().empty());  // 触发一次变化

    // set_on_change 覆盖构造注入的回调。
    std::vector<std::pair<std::string, FileChange>> latest;
    watcher.set_on_change(
        [&latest](const std::string& path, FileChange change) -> void { latest.emplace_back(path, change); });
    std::error_code ec;
    fs::remove(file, ec);
    AURORA_TEST_CHECK(!watcher.poll().empty());

    // 构造回调收到 Created，覆盖后的回调收到 Removed，路径逐字一致。
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(seen[0].first, file);
    AURORA_TEST_CHECK(seen[0].second == FileChange::Created);
    AURORA_TEST_REQUIRE_EQ(latest.size(), std::size_t{1});
    AURORA_TEST_CHECK(latest[0].second == FileChange::Removed);
}

AURORA_TEST_CASE(unwatch_stops_reporting_changes) {
    const ScopedTempDir dir{"aurora_utest_fw_unwatch"};
    FileWatcher watcher;
    const auto file = dir.file("ignored.txt");
    write_text(file, "v1");
    watcher.watch(file);
    watcher.unwatch(file);

    append_text(file, "-more");  // 已 unwatch：变化不进列表、不触发回调
    bool callback_invoked = false;
    watcher.set_on_change([&callback_invoked](const std::string&, FileChange) -> void { callback_invoked = true; });
    AURORA_TEST_CHECK(watcher.poll().empty());
    AURORA_TEST_CHECK_FALSE(callback_invoked);
}

}  // namespace aurora::test_cases::utest_file_watcher
