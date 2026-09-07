/// 测试类型: unit
/// 目标单元: include/aurora/core/file_watcher.h
/// 测试说明: FileWatcher 轮询变化检测单元测试

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "aurora/core/file_watcher.h"
#include "aurora_test_harness.h"

using au::FileChange;
using au::FileWatcher;

namespace aurora::test_cases::utest_file_watcher {

namespace {

/// 临时目录：ctest 把 CWD 设为 build/，相对路径 "build/..." 会失效；用系统临时目录。
auto tmp_dir() -> std::string {
    static const std::string DIR = []() -> std::string {
        const std::string d = (std::filesystem::temp_directory_path() / "aurora_watch_test").string();
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return DIR;
}

auto write_file(const std::string &path, const std::string &content) -> void {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

}  // namespace

AURORA_TEST() {
    // ---- 1. Modified 检测 ----
    {
        const std::string path = tmp_dir() + "/watch_target.txt";
        write_file(path, "v1");

        FileWatcher fw;
        fw.watch(path);
        AURORA_TEST_CHECK(fw.count() == 1);
        AURORA_TEST_CHECK(fw.poll().empty());  // 无变化

        // mtime 分辨率保护：稍等再改写
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        write_file(path, "v2 longer content");

        auto changes = fw.poll();
        AURORA_TEST_CHECK(changes.size() == 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(changes[0].second == FileChange::Modified);
        AURORA_TEST_CHECK(fw.poll().empty());  // 基线已更新
    }

    // ---- 2. Removed / Created 检测 + 回调 ----
    {
        const std::string path = tmp_dir() + "/watch_lifecycle.txt";
        write_file(path, "x");

        int callback_count = 0;
        auto last_change = FileChange::Modified;
        FileWatcher fw([&](const std::string &, FileChange c) -> void {
            ++callback_count;
            last_change = c;
        });
        fw.watch(path);

        std::filesystem::remove(path);
        fw.poll();
        AURORA_TEST_CHECK(callback_count == 1);
        AURORA_TEST_CHECK(last_change == FileChange::Removed);

        write_file(path, "back");
        fw.poll();
        AURORA_TEST_CHECK(callback_count == 2);
        AURORA_TEST_CHECK(last_change == FileChange::Created);
    }

    // ---- 3. unwatch ----
    {
        FileWatcher fw;
        fw.watch(tmp_dir() + "/whatever.txt");
        fw.unwatch(tmp_dir() + "/whatever.txt");
        AURORA_TEST_CHECK(fw.count() == 0);
    }
}

}  // namespace aurora::test_cases::utest_file_watcher
