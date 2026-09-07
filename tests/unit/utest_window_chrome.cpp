/// 测试类型: unit
/// 目标单元: include/aurora/window/window_chrome.h
/// 测试说明: utest_window_chrome 单元测试
///

#include <memory>

#include "aurora/aurora.h"
#include "aurora/window/surface.h"
#include "aurora/window/window_chrome.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_window_chrome {

AURORA_TEST() {
    // 空 surface：valid() 为 false，所有动作 no-op 不崩溃
    const au::WindowChrome empty{nullptr};
    AURORA_TEST_CHECK_FALSE(empty.valid());
    empty.begin_move();
    empty.begin_resize(au::WindowResizeEdge::Right);
    empty.minimize();
    empty.toggle_maximize();
    empty.set_fullscreen(true);
    empty.close();
    AURORA_TEST_CHECK(empty.content_inset().left == 0.0F);
    AURORA_TEST_CHECK(empty.content_inset().top == 0.0F);
    AURORA_TEST_CHECK(empty.content_inset().right == 0.0F);
    AURORA_TEST_CHECK(empty.content_inset().bottom == 0.0F);

    // 真实 HeadlessSurface：valid() 为 true，动作转发到 surface（Headless 实现为 no-op）
    const auto surf = std::make_unique<au::HeadlessSurface>();
    const au::WindowChrome chrome{surf.get()};
    AURORA_TEST_CHECK(chrome.valid());
    AURORA_TEST_CHECK(chrome.content_inset().left == 0.0F);
    AURORA_TEST_CHECK(chrome.content_inset().top == 0.0F);
    chrome.begin_move();
    chrome.minimize();
    chrome.close();
}

}  // namespace aurora::test_cases::utest_window_chrome
