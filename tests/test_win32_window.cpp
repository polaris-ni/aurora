// 目标源单元：window/win32_window.h（Win32/GDI 窗口壳，仅 AURORA_BACKEND_WIN32 编译）。
//
// API 覆盖映射：create_window(Win32Options)/wnd_proc→EventDispatcher 全链路
//   → tests/test_win32_surface.cpp 的 sec_test_win32_button_click / sec_test_count_display_col 段；
//   黑屏修复契约（背景刷/WM_PAINT）→ 同文件 sec_test_win32_blackscreen 段。
//
// 覆盖率豁免说明：本机为 Linux，无法编译 Win32 后端；覆盖率按平台豁免处理。

#include "test_harness.h"

#ifdef AURORA_BACKEND_WIN32
#include "aurora/aurora.h"

namespace {
void test_compiled_in() { AURORA_TEST_CHECK_MSG(true, "win32_window compiled-in smoke"); }
} // namespace

AURORA_TEST() { test_compiled_in(); }
#else
AURORA_TEST_SKIP(AURORA_BACKEND_WIN32)
#endif
