/// 测试类型: unit
/// 目标单元: include/aurora/window/window_chrome.h
/// 测试说明: 覆盖 WindowChrome 空指针守卫（valid/content_inset/动作 no-op 安全）、
/// 全部窗口动作到 Surface 的参数保真转发、content_inset 镜像与值语义拷贝

#include <optional>

#include "aurora/window/window_chrome.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_chrome {

namespace {

/// @brief 记录型 Surface 桩：只记录 chrome 转发调用，不触任何平台 API。
class RecordingSurface final : public Surface {
  public:
    bool begin_move_called = false;
    bool close_called = false;
    bool minimize_called = false;
    bool toggle_maximize_called = false;
    int fullscreen_last = -1;  // -1 未调用 / 1 set_fullscreen(true) / 0 set_fullscreen(false)
    std::optional<WindowResizeEdge> resize_last;
    EdgeInsets inset_val{};

    auto begin_frame(int /*width*/, int /*height*/) -> Result<bool> override { return Result<bool>{true}; }
    auto painter() -> Painter & override { return painter_; }
    auto present() -> Result<bool> override { return Result<bool>{true}; }
    auto size() const -> Size override { return Size{}; }
    auto content_inset() const -> EdgeInsets override { return inset_val; }
    auto close() -> void override { close_called = true; }
    auto minimize() -> void override { minimize_called = true; }
    auto toggle_maximize() -> void override { toggle_maximize_called = true; }
    auto set_fullscreen(bool on) -> void override { fullscreen_last = on ? 1 : 0; }
    auto begin_window_move() -> void override { begin_move_called = true; }
    auto begin_window_resize(WindowResizeEdge edge) -> void override { resize_last = edge; }

  private:
    Painter painter_;
};

}  // namespace

AURORA_TEST_CASE(null_chrome_reports_invalid_and_zero_inset) {
    // 空 Surface 指针：valid() 为 false；装饰内边距回落为全零（而非崩溃）。
    const WindowChrome chrome{nullptr};
    AURORA_TEST_CHECK_FALSE(chrome.valid());
    const EdgeInsets inset = chrome.content_inset();
    AURORA_TEST_CHECK_NEAR(inset.left, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.top, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.right, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.bottom, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(null_chrome_actions_are_safe_noops) {
    // 空 Surface 上调用全部窗口动作必须为安全 no-op（自绘标题栏在无后端环境也能跑）。
    const WindowChrome chrome{nullptr};
    AURORA_TEST_CHECK_NO_THROW(chrome.begin_move());
    AURORA_TEST_CHECK_NO_THROW(chrome.begin_resize(WindowResizeEdge::Left));
    AURORA_TEST_CHECK_NO_THROW(chrome.minimize());
    AURORA_TEST_CHECK_NO_THROW(chrome.toggle_maximize());
    AURORA_TEST_CHECK_NO_THROW(chrome.set_fullscreen(true));
    AURORA_TEST_CHECK_NO_THROW(chrome.close());
}

AURORA_TEST_CASE(valid_chrome_reports_surface_presence) {
    RecordingSurface surf;
    const WindowChrome chrome{&surf};
    AURORA_TEST_CHECK_TRUE(chrome.valid());
}

AURORA_TEST_CASE(chrome_forwards_move_and_resize_edge) {
    RecordingSurface surf;
    const WindowChrome chrome{&surf};
    chrome.begin_move();
    chrome.begin_resize(WindowResizeEdge::Left);
    AURORA_TEST_CHECK_TRUE(surf.begin_move_called);
    AURORA_TEST_REQUIRE_TRUE(surf.resize_last.has_value());
    AURORA_TEST_CHECK_EQ(*surf.resize_last, WindowResizeEdge::Left);
}

AURORA_TEST_CASE(chrome_forwards_minimize_maximize_fullscreen) {
    RecordingSurface surf;
    const WindowChrome chrome{&surf};
    chrome.minimize();
    chrome.toggle_maximize();
    chrome.set_fullscreen(true);
    AURORA_TEST_CHECK_TRUE(surf.minimize_called);
    AURORA_TEST_CHECK_TRUE(surf.toggle_maximize_called);
    AURORA_TEST_CHECK_EQ(surf.fullscreen_last, 1);
    chrome.set_fullscreen(false);
    AURORA_TEST_CHECK_EQ(surf.fullscreen_last, 0);
}

AURORA_TEST_CASE(chrome_forwards_close) {
    RecordingSurface surf;
    const WindowChrome chrome{&surf};
    AURORA_TEST_CHECK_FALSE(surf.close_called);
    chrome.close();
    AURORA_TEST_CHECK_TRUE(surf.close_called);
}

AURORA_TEST_CASE(chrome_content_inset_mirrors_surface) {
    // chrome 的 content_inset 是 Surface 同名值的逐字段镜像（CSD 标题栏避让数据源）。
    RecordingSurface surf;
    surf.inset_val = EdgeInsets{.left = 2.0F, .top = 36.0F, .right = 2.0F, .bottom = 1.0F};
    const WindowChrome chrome{&surf};
    const EdgeInsets inset = chrome.content_inset();
    AURORA_TEST_CHECK_NEAR(inset.left, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.top, 36.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.right, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.bottom, 1.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.horizontal(), 4.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.vertical(), 37.0F, 1e-4F);
}

AURORA_TEST_CASE(chrome_is_copyable_value_semantics) {
    // WindowChrome 经 Environment 按值传递：拷贝出的 chrome 仍指向同一 Surface 并正常转发。
    RecordingSurface surf;
    const WindowChrome chrome{&surf};
    const WindowChrome copy = chrome;
    AURORA_TEST_CHECK_TRUE(copy.valid());
    copy.close();
    AURORA_TEST_CHECK_TRUE(surf.close_called);
    copy.begin_resize(WindowResizeEdge::BottomRight);
    AURORA_TEST_REQUIRE_TRUE(surf.resize_last.has_value());
    AURORA_TEST_CHECK_EQ(*surf.resize_last, WindowResizeEdge::BottomRight);
}

}  // namespace aurora::test_cases::utest_window_chrome
