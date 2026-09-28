/// 测试类型: unit
/// 目标单元: include/aurora/app/clipboard.h
/// 测试说明: 经进程内 memory 测试后端（install/reset/remove 注入点）覆盖文本读写往返、
/// 覆盖写、空文本、图像 RGBA8 往返与清空语义；失败口径另设两条平台路径用例
/// （非法 UTF-8 载荷 → ClipboardWriteFailed 且不动既有内容；图像参数非法 → GeneralInvalidArgument），
/// 其余路径全程不写系统剪贴板（后端不可用时 SKIP）

#include <string>

#include "aurora/app/clipboard.h"
#include "aurora/core/platform.h"
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
    const auto empty = Clipboard::get_text();
    AURORA_TEST_REQUIRE(empty.ok());
    AURORA_TEST_CHECK_EQ(empty.value(), std::string{});

    // 重复安装幂等（内容被清空），仍返回 true。
    AURORA_TEST_REQUIRE(Clipboard::set_text("stale").ok());
    AURORA_TEST_CHECK_TRUE(Clipboard::install_test_backend());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{});
}

AURORA_TEST_CASE(text_roundtrip_through_memory_backend) {
    require_test_backend();

    const std::string payload = "hello aurora 剪贴板 UTF-8";
    AURORA_TEST_REQUIRE(Clipboard::set_text(payload).ok());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), payload);
}

AURORA_TEST_CASE(text_overwrite_returns_latest_value) {
    require_test_backend();

    AURORA_TEST_REQUIRE(Clipboard::set_text("first").ok());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{"first"});
    AURORA_TEST_REQUIRE(Clipboard::set_text("second").ok());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{"second"});
}

AURORA_TEST_CASE(empty_text_roundtrip) {
    require_test_backend();

    AURORA_TEST_REQUIRE(Clipboard::set_text(std::string{}).ok());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{});
}

AURORA_TEST_CASE(reset_test_backend_clears_content) {
    require_test_backend();

    AURORA_TEST_REQUIRE(Clipboard::set_text("payload").ok());
    AURORA_TEST_REQUIRE(Clipboard::set_image(make_image()).ok());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{"payload"});

    Clipboard::reset_test_backend();
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{});
    AURORA_TEST_CHECK_EQ(Clipboard::get_image().value().width, 0);
}

AURORA_TEST_CASE(image_roundtrip_through_memory_backend) {
    require_test_backend();

    const Image img = make_image();
    AURORA_TEST_REQUIRE(Clipboard::set_image(img).ok());

    const auto out = Clipboard::get_image();
    AURORA_TEST_REQUIRE(out.ok());
    AURORA_TEST_CHECK_EQ(out.value().width, 2);
    AURORA_TEST_CHECK_EQ(out.value().height, 2);
    AURORA_TEST_CHECK_EQ(out.value().pixels, img.pixels);
}

AURORA_TEST_CASE(image_absent_after_reset_returns_empty) {
    require_test_backend();

    // 未写入过图像（安装后内容为空）→ 读取返回 width==0 的空 Image；空内容不是失败。
    const auto out = Clipboard::get_image();
    AURORA_TEST_REQUIRE(out.ok());
    AURORA_TEST_CHECK_EQ(out.value().width, 0);
    AURORA_TEST_CHECK_EQ(out.value().height, 0);
    AURORA_TEST_CHECK_TRUE(out.value().pixels.empty());
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
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{});
}

/// 写入载荷非法时必须以机器可读的失败返回——这正是本 API 从 `void` 改成 `Result` 的动机：
/// 改前这里只有一行 stderr WARN 且乱码照上屏，改后调用方能按 code 分支。
/// 本用例只读不写系统剪贴板：非破坏性靠「非法载荷在 OpenClipboard 之前即被拒」保证，
/// 前后读数一致用来坐实它确实没动过剪贴板。该坐实步骤按环境让路：系统剪贴板可能被别的
/// 进程暂占（此时 `get_text` 返回 ClipboardAccessFailed），那是本 API 的正常失败语义、
/// 不是被测缺陷，故读数不可得时跳过坐实而非判红——写失败的断言与剪贴板是否可读无关。
AURORA_TEST_CASE(invalid_utf8_payload_surfaces_write_error) {
#ifdef AURORA_PLATFORM_WINDOWS
    (void)Clipboard::remove_test_backend();  // 走平台路径

    const auto before = Clipboard::get_text();  // 可能因外部占用而 Err，不作为前置要求
    const auto result = Clipboard::set_text(std::string{"\xF0\x28\x8C\x28"});  // 非法 UTF-8 序列
    AURORA_TEST_REQUIRE_FALSE(result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::ClipboardWriteFailed);
    AURORA_TEST_CHECK_EQ(result.error().code, std::string{"clipboard-write-failed"});
    AURORA_TEST_CHECK_FALSE(result.error().retryable);  // 载荷本身坏了，重试无益
    AURORA_TEST_CHECK_FALSE(result.error().message.empty());

    if (before.ok()) {
        const auto after = Clipboard::get_text();
        AURORA_TEST_REQUIRE(after.ok());
        AURORA_TEST_CHECK_EQ(after.value(), before.value());  // 失败的一次不得清空既有内容
    } else {
        AURORA_TEST_TRACE(
            "system clipboard unreadable before the write (external occupancy) — the non-destruction "
            "sub-assertion needs a baseline, so only it was skipped; the write-failure assertions above ran");
    }
#else
    AURORA_TEST_SKIP("转码拒绝分支在 Win32 的 MultiByteToWideChar 上，非 Windows 走 xsel/xclip 字节直传");
#endif
}

/// 调用方参数错误与平台能力缺失都必须以 code 区分开，而不是静默 no-op。
AURORA_TEST_CASE(bad_image_argument_is_not_silent) {
    (void)Clipboard::remove_test_backend();  // 走平台路径

    Image bad = make_image();
    bad.pixels.pop_back();  // 2x2 却只有 15 字节：缓冲与维度不一致
    const auto result = Clipboard::set_image(bad);
    AURORA_TEST_REQUIRE_FALSE(result.ok());
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
#else
    // 非 Windows 没有 CF_DIB 通路，先撞上「本平台无图像剪贴板实现」——同样是机器可见的失败。
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralNotSupported);
#endif
}

}  // namespace aurora::test_cases::utest_clipboard
