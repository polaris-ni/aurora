/// 测试类型: unit
/// 目标单元: include/aurora/app/clipboard.h
/// 测试说明: 经进程内 memory 测试后端（install/reset/remove 注入点）覆盖文本读写往返、
/// 覆盖写、空文本、图像 RGBA8 往返与清空语义；全程不触碰系统剪贴板（后端不可用时 SKIP）

#include <string>

#include "aurora/app/clipboard.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_clipboard {

namespace {

/// 安装 memory 后端（同时清空内容）；双宏未齐备的构建下跳过本用例。
auto require_test_backend() -> void {
    if (!Clipboard::install_test_backend()) {
        AURORA_TEST_SKIP("clipboard memory 测试后端不可用（AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS 未齐备）");
    }
}

/// 2x2 RGBA8 测试图像，像素字节 0..15 各不相同。
auto make_image() -> Image {
    Image img;
    img.width = 2;
    img.height = 2;
    img.pixels = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U};
    return img;
}

}  // namespace

AURORA_TEST_CASE(install_test_backend_activates_memory_backend) {
    require_test_backend();

    // 安装成功即处于活动状态：set/get 均改走内存（此处以空文本读取可立即返回验证）。
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{});

    // 重复安装幂等（内容被清空），仍返回 true。
    Clipboard::set_text("stale");
    AURORA_TEST_CHECK_TRUE(Clipboard::install_test_backend());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{});
}

AURORA_TEST_CASE(text_roundtrip_through_memory_backend) {
    require_test_backend();

    const std::string payload = "hello aurora 剪贴板 UTF-8";
    Clipboard::set_text(payload);
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), payload);
}

AURORA_TEST_CASE(text_overwrite_returns_latest_value) {
    require_test_backend();

    Clipboard::set_text("first");
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{"first"});
    Clipboard::set_text("second");
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{"second"});
}

AURORA_TEST_CASE(empty_text_roundtrip) {
    require_test_backend();

    Clipboard::set_text(std::string{});
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{});
}

AURORA_TEST_CASE(reset_test_backend_clears_content) {
    require_test_backend();

    Clipboard::set_text("payload");
    Image img = make_image();
    Clipboard::set_image(img);
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{"payload"});

    Clipboard::reset_test_backend();
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{});
    AURORA_TEST_CHECK_EQ(Clipboard::get_image().width, 0);
}

AURORA_TEST_CASE(image_roundtrip_through_memory_backend) {
    require_test_backend();

    const Image img = make_image();
    Clipboard::set_image(img);

    const Image out = Clipboard::get_image();
    AURORA_TEST_CHECK_EQ(out.width, 2);
    AURORA_TEST_CHECK_EQ(out.height, 2);
    AURORA_TEST_CHECK_EQ(out.pixels, img.pixels);
}

AURORA_TEST_CASE(image_absent_after_reset_returns_empty) {
    require_test_backend();

    // 未写入过图像（安装后内容为空）→ 读取返回 width==0 的空 Image。
    const Image out = Clipboard::get_image();
    AURORA_TEST_CHECK_EQ(out.width, 0);
    AURORA_TEST_CHECK_EQ(out.height, 0);
    AURORA_TEST_CHECK_TRUE(out.pixels.empty());
}

AURORA_TEST_CASE(remove_test_backend_reports_state_transitions) {
    // 复位入口状态：前序用例可能已把后端留在安装态；此刻不读取剪贴板内容。
    (void)Clipboard::remove_test_backend();

    // 未安装时卸载为 no-op，返回 false（不触碰平台实现）。
    AURORA_TEST_CHECK_FALSE(Clipboard::remove_test_backend());

    require_test_backend();
    AURORA_TEST_CHECK_TRUE(Clipboard::remove_test_backend());
    // 已卸载后再卸载仍为 false。
    AURORA_TEST_CHECK_FALSE(Clipboard::remove_test_backend());

    // 重新安装：内容被清空（不读平台剪贴板，随即恢复内存后端）。
    AURORA_TEST_CHECK_TRUE(Clipboard::install_test_backend());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text(), std::string{});
}

}  // namespace aurora::test_cases::utest_clipboard
