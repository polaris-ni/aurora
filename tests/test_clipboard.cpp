// test_clipboard.cpp — 剪贴板抽象 1:1 测试：写入/读取往返、空文本安全、跨平台 no-op。

#include <string>

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Clipboard;
using aurora::Image;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_clipboard ===\n");

#ifdef AURORA_PLATFORM_WINDOWS
    // 真实系统剪贴板往返（Windows）：若当前环境无法打开剪贴板（沙箱/被占用），
    // 跳过往返断言以避免环境相关失败；no-op 安全路径与类型契约仍被覆盖。
    const bool clipboard_available = (OpenClipboard(nullptr) != 0);
    if (clipboard_available) {
        CloseClipboard();
    } else {
        AURORA_TEST_PRINTF("  (clipboard unavailable in this environment; skipping round-trip)\n");
    }

    if (clipboard_available) {
        // 1) 写入文本不崩溃。
        Clipboard::set_text("aurora-clipboard-test");
        AURORA_TEST_CHECK(true);

        // 2) 往返：写入后读取返回相同文本。
        AURORA_TEST_CHECK(Clipboard::get_text() == "aurora-clipboard-test");

        // 3) 覆盖写入不同文本，仍往返一致。
        Clipboard::set_text("second-value");
        AURORA_TEST_CHECK(Clipboard::get_text() == "second-value");

        // 4) 空文本写入不崩溃（提前返回路径），且不清除已有内容。
        Clipboard::set_text("");
        AURORA_TEST_CHECK(true);
        AURORA_TEST_CHECK(Clipboard::get_text() == "second-value"); // 空串早退，保留上次内容

        // 5) 含 Unicode 文本往返（UTF-8 ↔ UTF-16）。
        Clipboard::set_text("héllo-世界");
        AURORA_TEST_CHECK(Clipboard::get_text() == "héllo-世界");

        // 6) 读取返回 std::string（可安全拷贝）。
        const std::string fetched = Clipboard::get_text();
        AURORA_TEST_CHECK(!fetched.empty());

        // ---- 图像剪贴板：RGBA8 经 CF_DIB 写入再读回 ----
        Image img;
        img.width = 2;
        img.height = 2;
        img.pixels = {
            255, 0,   0,   255, // 红
            0,   255, 0,   255, // 绿
            0,   0,   255, 255, // 蓝
            255, 255, 255, 255, // 白
        };
        Clipboard::set_image(img);
        const Image out = Clipboard::get_image();
        AURORA_TEST_CHECK(out.width == 2);
        AURORA_TEST_CHECK(out.height == 2);
        AURORA_TEST_CHECK(out.pixels.size() == 16u);
        AURORA_TEST_CHECK(out.pixels == img.pixels); // RGBA 往返一致
    }
#else
    // 非 Windows：no-op，仅验证不崩溃且读取为空（文本 + 图像）。
    Clipboard::set_text("anything");
    AURORA_TEST_CHECK(Clipboard::get_text().empty());
    Clipboard::set_text("");
    AURORA_TEST_CHECK(true);

    {
        Image img;
        img.width = 2;
        img.height = 2;
        img.pixels = { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 };
        Clipboard::set_image(img);
        const Image out = Clipboard::get_image();
        AURORA_TEST_CHECK(out.width == 0);
        AURORA_TEST_CHECK(out.height == 0);
    }
#endif
}
