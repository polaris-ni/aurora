// 验证基础设施三件套：UndoStack（撤销/重做/宏命令）、FileWatcher（轮询变化）、
// compare_snapshots（快照像素对比）。
// ── API 覆盖映射 ─────────────────────────────
// core/file_watcher.h(FileWatcher 轮询)、state/undo_stack.h(UndoStack 撤销/重做/宏命令)、
// render/snapshot_diff.h(compare_snapshots) → 本文件三段分别覆盖。

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "aurora/core/file_watcher.h"
#include "aurora/core/image.h"
#include "aurora/render/snapshot_diff.h"
#include "aurora/state/undo_stack.h"

#include "test_harness.h"

using aurora::compare_snapshots;
using aurora::FileChange;
using aurora::FileWatcher;
using aurora::Image;
using aurora::UndoCommand;
using aurora::UndoStack;

namespace {

/// 临时目录：ctest 把 CWD 设为 build/，相对路径 "build/..." 会失效；用系统临时目录。
auto tmp_dir() -> std::string {
    static const std::string dir = []() -> std::string {
        const std::string d = (std::filesystem::temp_directory_path() / "aurora_watch_test").string();
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

auto write_file(const std::string &path, const std::string &content) -> void {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

auto make_image(int w, int h, std::uint8_t v) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, v);
    return img;
}

} // namespace

AURORA_TEST() {
    // ==================== UndoStack ====================

    // ---- 1. push 执行 redo，undo/redo 往返 ----
    {
        UndoStack stack;
        int value = 0;
        AURORA_TEST_CHECK(!stack.can_undo());
        AURORA_TEST_CHECK(!stack.can_redo());

        stack.push(
            { .redo = [&]() -> void { value = 1; }, .undo = [&]() -> void { value = 0; }, .description = "set 1" });
        AURORA_TEST_CHECK(value == 1);
        AURORA_TEST_CHECK(stack.can_undo());

        stack.push(
            { .redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "set 2" });
        AURORA_TEST_CHECK(value == 2);

        AURORA_TEST_CHECK(stack.undo());
        AURORA_TEST_CHECK(value == 1);
        AURORA_TEST_CHECK(stack.can_redo());

        AURORA_TEST_CHECK(stack.undo());
        AURORA_TEST_CHECK(value == 0);
        AURORA_TEST_CHECK(!stack.can_undo());
        AURORA_TEST_CHECK(!stack.undo()); // 到底无操作

        AURORA_TEST_CHECK(stack.redo());
        AURORA_TEST_CHECK(value == 1);
        AURORA_TEST_CHECK(stack.redo());
        AURORA_TEST_CHECK(value == 2);
        AURORA_TEST_CHECK(!stack.redo());
    }

    // ---- 2. push 截断重做分支 ----
    {
        UndoStack stack;
        int value = 0;
        stack.push({ .redo = [&]() -> void { value = 1; }, .undo = [&]() -> void { value = 0; }, .description = "a" });
        stack.push({ .redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "b" });
        stack.undo(); // 回到 1
        stack.push({ .redo = [&]() -> void { value = 9; },
                     .undo = [&]() -> void { value = 1; },
                     .description = "c" }); // 分支截断
        AURORA_TEST_CHECK(value == 9);
        AURORA_TEST_CHECK(!stack.can_redo()); // b 被丢弃
        AURORA_TEST_CHECK(stack.count() == 2);
    }

    // ---- 3. 描述与深度上限 ----
    {
        UndoStack stack;
        stack.set_limit(2);
        int v = 0;
        stack.push({ .redo = [&]() -> void { ++v; }, .undo = [&]() -> void { --v; }, .description = "one" });
        stack.push({ .redo = [&]() -> void { ++v; }, .undo = [&]() -> void { --v; }, .description = "two" });
        stack.push(
            { .redo = [&]() -> void { ++v; }, .undo = [&]() -> void { --v; }, .description = "three" }); // 挤掉 one
        AURORA_TEST_CHECK(stack.count() == 2);
        AURORA_TEST_CHECK(stack.undo_description() == "three");
        stack.undo();
        AURORA_TEST_CHECK(stack.redo_description() == "three");
        AURORA_TEST_CHECK(stack.undo_description() == "two");
    }

    // ---- 4. 宏命令整组执行/逆序撤销 ----
    {
        UndoStack stack;
        std::vector<int> order;
        std::vector<UndoCommand> cmds;
        cmds.push_back({ .redo = [&]() -> void { order.push_back(1); },
                         .undo = [&]() -> void { order.push_back(-1); },
                         .description = "s1" });
        cmds.push_back({ .redo = [&]() -> void { order.push_back(2); },
                         .undo = [&]() -> void { order.push_back(-2); },
                         .description = "s2" });

        stack.push(UndoStack::macro(std::move(cmds), "combo"));
        AURORA_TEST_CHECK(order.size() == 2 && order[0] == 1 && order[1] == 2);

        stack.undo(); // 逆序：-2 先于 -1
        AURORA_TEST_CHECK(order.size() == 4 && order[2] == -2 && order[3] == -1);
        AURORA_TEST_CHECK(stack.redo_description() == "combo");
    }

    // ==================== FileWatcher ====================

    // ---- 5. Modified 检测 ----
    {
        const std::string path = tmp_dir() + "/watch_target.txt";
        write_file(path, "v1");

        FileWatcher fw;
        fw.watch(path);
        AURORA_TEST_CHECK(fw.count() == 1);
        AURORA_TEST_CHECK(fw.poll().empty()); // 无变化

        // mtime 分辨率保护：稍等再改写
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        write_file(path, "v2 longer content");

        auto changes = fw.poll();
        AURORA_TEST_CHECK(changes.size() == 1);
        AURORA_TEST_CHECK(changes[0].second == FileChange::Modified);
        AURORA_TEST_CHECK(fw.poll().empty()); // 基线已更新
    }

    // ---- 6. Removed / Created 检测 + 回调 ----
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

    // ---- 7. unwatch ----
    {
        FileWatcher fw;
        fw.watch(tmp_dir() + "/whatever.txt");
        fw.unwatch(tmp_dir() + "/whatever.txt");
        AURORA_TEST_CHECK(fw.count() == 0);
    }

    // ==================== compare_snapshots ====================

    // ---- 8. 相同图零差异 ----
    {
        const Image a = make_image(10, 10, 128);
        const Image b = make_image(10, 10, 128);
        const auto diff = compare_snapshots(a, b);
        AURORA_TEST_CHECK(!diff.size_mismatch);
        AURORA_TEST_CHECK(diff.pixel_diff_count == 0);
        AURORA_TEST_CHECK(diff.max_color_delta == 0);
        AURORA_TEST_CHECK(diff.passed());
    }

    // ---- 9. 部分差异 + 容差 ----
    {
        Image a = make_image(10, 10, 100);
        Image b = make_image(10, 10, 100);
        // 改 5 个像素
        for (int i = 0; i < 5; ++i) {
            b.pixels[static_cast<std::size_t>(i) * 4] = 200; // R 通道 +100
        }
        const auto strict = compare_snapshots(a, b, 0);
        AURORA_TEST_CHECK(strict.pixel_diff_count == 5);
        AURORA_TEST_CHECK(strict.max_color_delta == 100);
        AURORA_TEST_CHECK(std::abs(strict.diff_ratio - 0.05) < 1e-9);
        AURORA_TEST_CHECK(!strict.passed());
        AURORA_TEST_CHECK(strict.passed(0.10)); // 10% 阈值内通过

        // 容差 100：全部视为相同
        const auto tolerant = compare_snapshots(a, b, 100);
        AURORA_TEST_CHECK(tolerant.pixel_diff_count == 0);
        AURORA_TEST_CHECK(tolerant.passed());
    }

    // ---- 10. 尺寸不一致 + 差异图可视化 ----
    {
        const Image a = make_image(10, 10, 0);
        const Image c = make_image(8, 8, 0);
        AURORA_TEST_CHECK(compare_snapshots(a, c).size_mismatch);
        AURORA_TEST_CHECK(!compare_snapshots(a, c).passed());

        Image b = make_image(10, 10, 0);
        b.pixels[0] = 255; // 第一个像素差异
        const auto diff = compare_snapshots(a, b);
        // 差异图：像素 0 红色
        AURORA_TEST_CHECK(diff.diff_image.pixels[0] == 255);
        AURORA_TEST_CHECK(diff.diff_image.pixels[1] == 0);
        // 其余淡化
        AURORA_TEST_CHECK(diff.diff_image.pixels[7] == 255); // alpha
    }
}
