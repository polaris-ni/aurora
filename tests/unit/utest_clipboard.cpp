/// 测试类型: unit
/// 目标单元: include/aurora/app/clipboard.h
/// 测试说明: 经进程内 memory 测试后端（install/reset/remove 注入点）覆盖文本读写往返、
/// 覆盖写、空文本、图像 RGBA8 往返与清空语义；失败口径另设两条平台路径用例
/// （非法 UTF-8 载荷 → ClipboardWriteFailed 且不动既有内容；图像参数非法 → GeneralInvalidArgument），
/// 其余路径全程不写系统剪贴板（后端不可用时 SKIP）；
/// 富格式段覆盖自定义 MIME 载荷、files / html 便利封装、available_formats 如实反映已写格式、
/// 空值与空写口径、以及立骨期平台路径的 clipboard-format-unsupported

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/clipboard.h"
#include "aurora/app/mime_types.h"
#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_clipboard {

namespace {

/// 安装 memory 后端（同时清空内容）；双宏未齐备的构建下跳过本用例。
auto require_test_backend() -> void {
    if (!Clipboard::install_test_backend()) {
        AURORA_TEST_SKIP(
            "clipboard memory test backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
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

    const std::string payload = "hello aurora clipboard UTF-8";
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

// 写入载荷非法时必须以机器可读的失败返回——这正是本 API 从 `void` 改成 `Result` 的动机：
// 改前这里只有一行 stderr WARN 且乱码照上屏，改后调用方能按 code 分支。
// 本用例只读不写系统剪贴板：非破坏性靠「非法载荷在 OpenClipboard 之前即被拒」保证，
// 前后读数一致用来坐实它确实没动过剪贴板。该坐实步骤按环境让路：系统剪贴板可能被别的
// 进程暂占（此时 `get_text` 返回 ClipboardAccessFailed），那是本 API 的正常失败语义、
// 不是被测缺陷，故读数不可得时跳过坐实而非判红——写失败的断言与剪贴板是否可读无关。
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
    AURORA_TEST_SKIP(
        "the transcoding rejection branch lives in Win32 MultiByteToWideChar; non-Windows passes bytes to xsel/xclip");
#endif
}

// 调用方参数错误与平台能力缺失都必须以 code 区分开，而不是静默 no-op。
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

// ---- 富格式段（自定义 MIME / files / html / available_formats）----

namespace {

/// @brief 文本 → 字节载荷：富格式通道按字节读写，测试侧需与被测同一份折法。
auto to_bytes(const std::string &text) -> std::vector<std::uint8_t> { return {text.begin(), text.end()}; }

/// @brief 字节载荷 → 文本（uri-list / html 读取侧的还原）。
auto to_text(const std::vector<std::uint8_t> &bytes) -> std::string { return {bytes.begin(), bytes.end()}; }

/// @brief 判定格式清单里是否含有某个 MIME。
auto has_format(const std::vector<std::string> &formats, std::string_view mime) -> bool {
    return std::find(formats.begin(), formats.end(), mime) != formats.end();
}

/// @brief 立骨期富格式在平台路径上的统一失败（memory 后端已卸载时走此路）。
auto expect_format_unsupported(const Error &err, std::string_view mime) -> void {
    AURORA_TEST_CHECK_EQ(err.code_enum, ErrorCode::ClipboardFormatUnsupported);
    AURORA_TEST_CHECK_EQ(err.code, std::string{"clipboard-format-unsupported"});
    AURORA_TEST_CHECK_FALSE(err.retryable);  // 机制缺失是稳态，重试无益
    AURORA_TEST_CHECK_FALSE(err.message.empty());
    AURORA_TEST_CHECK_TRUE(err.message.find(std::string{mime}) != std::string::npos);  // 失败点名是哪个 MIME
}

}  // namespace

// 自定义 MIME 走通用字节通道：写入什么字节就读回什么字节，且如实进入 available_formats。
// 变异自证：把 `set_payload` 的存储键改成写死 `text/plain`，或把 `get_payload` 的查表改成
// 无视 MIME 直接返回首个条目，本用例即转红。
AURORA_TEST_CASE(payload_roundtrip_for_custom_mime) {
    require_test_backend();

    const std::string mime = "application/x-aurora-probe";
    const std::vector<std::uint8_t> payload{0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x7FU};
    AURORA_TEST_REQUIRE(Clipboard::set_payload(mime, payload).ok());

    const auto out = Clipboard::get_payload(mime);
    AURORA_TEST_REQUIRE(out.ok());
    AURORA_TEST_CHECK_EQ(out.value(), payload);

    const auto formats = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(formats.ok());
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), mime));
}

// set_files / get_files 是 `text/uri-list` 的便利封装：这里同时经 `get_payload` 读原始载荷，
// 用常量取值去索引——若实现里改成硬编码字面量而常量表被改，本用例即刻转红（常量表治理）。
// 变异自证（已实测）：把 `set_files` 的存储键从 `AURORA_MIME_URI_LIST` 换成 `AURORA_MIME_TEXT_PLAIN`，
// `get_payload(AURORA_MIME_URI_LIST)` 与 `get_files()` 会同时落空，本用例转红。
AURORA_TEST_CASE(files_roundtrip_via_uri_list_constant) {
    require_test_backend();

    const std::vector<std::string> paths{"/srv/aurora/aurora dir/one.txt", "/srv/aurora/two.txt"};
    AURORA_TEST_REQUIRE(Clipboard::set_files(paths).ok());

    const auto files = Clipboard::get_files();
    AURORA_TEST_REQUIRE(files.ok());
    AURORA_TEST_CHECK_EQ(files.value(), paths);

    // 原始载荷经常量索引可读回：证明两端共用同一张 MIME 表，而非各写一份字面量。
    const auto raw = Clipboard::get_payload(std::string{AURORA_MIME_URI_LIST});
    AURORA_TEST_REQUIRE(raw.ok());
    AURORA_TEST_CHECK_TRUE(to_text(raw.value()).starts_with("file:///srv/aurora/aurora%20dir/one.txt"));

    const auto formats = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(formats.ok());
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), AURORA_MIME_URI_LIST));
}

// 非 file scheme 的行不映射为路径：全被跳过时是 `Ok(空清单)`，而不是失败。
// 变异自证：把 `get_files` 的解码换成「整行百分号解码后一律当路径」，本用例即转红。
AURORA_TEST_CASE(get_files_skips_non_file_scheme_lines) {
    require_test_backend();

    const std::string payload = "http://example.com/a.txt\r\nfile:///srv/aurora/kept.txt\r\n";
    AURORA_TEST_REQUIRE(Clipboard::set_payload(std::string{AURORA_MIME_URI_LIST}, to_bytes(payload)).ok());

    const auto files = Clipboard::get_files();
    AURORA_TEST_REQUIRE(files.ok());
    AURORA_TEST_CHECK_EQ(files.value(), std::vector<std::string>{"/srv/aurora/kept.txt"});

    // 全部条目都不可映射为路径时同样是 Ok(空清单)：空内容不是失败。
    AURORA_TEST_REQUIRE(Clipboard::set_payload(std::string{AURORA_MIME_URI_LIST}, to_bytes("http://a/b\r\n")).ok());
    const auto none = Clipboard::get_files();
    AURORA_TEST_REQUIRE(none.ok());
    AURORA_TEST_CHECK_EQ(none.value(), std::vector<std::string>{});
}

// html 便利封装映射 `AURORA_MIME_TEXT_HTML`，与 `get_payload` 的字节视图同源。
// 变异自证：把 `set_html` 的存储键换成 `AURORA_MIME_TEXT_PLAIN`，两条读取通道即分叉转红。
AURORA_TEST_CASE(html_roundtrip_via_text_html_constant) {
    require_test_backend();

    const std::string html = "<b>bold</b> &amp; <i>italic</i>";
    AURORA_TEST_REQUIRE(Clipboard::set_html(html).ok());

    const auto out = Clipboard::get_html();
    AURORA_TEST_REQUIRE(out.ok());
    AURORA_TEST_CHECK_STREQ(out.value(), html);

    const auto raw = Clipboard::get_payload(std::string{AURORA_MIME_TEXT_HTML});
    AURORA_TEST_REQUIRE(raw.ok());
    AURORA_TEST_CHECK_EQ(raw.value(), to_bytes(html));
}

// available_formats 如实反映「此刻写进了什么」：reset 后为空，写了才在列，且不谎报未写入的格式。
// 变异自证：把 available_formats 改成「无条件返回全表常量」，未写入格式的断言即转红。
AURORA_TEST_CASE(available_formats_reflects_written_formats) {
    require_test_backend();

    // 刚安装（内容清空）：一个格式都不在列。
    const auto empty = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(empty.ok());
    AURORA_TEST_CHECK_EQ(empty.value(), std::vector<std::string>{});

    AURORA_TEST_REQUIRE(Clipboard::set_text("plain text").ok());
    AURORA_TEST_REQUIRE(Clipboard::set_html("<p>p</p>").ok());
    AURORA_TEST_REQUIRE(Clipboard::set_payload("application/x-probe", to_bytes("x")).ok());

    const auto formats = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(formats.ok());
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), AURORA_MIME_TEXT_PLAIN));
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), AURORA_MIME_TEXT_HTML));
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), "application/x-probe"));
    // 立骨期富格式未写入即不入列：新 API 已声明不等于格式已可用。
    AURORA_TEST_CHECK_FALSE(has_format(formats.value(), AURORA_MIME_URI_LIST));
    AURORA_TEST_CHECK_FALSE(has_format(formats.value(), AURORA_MIME_IMAGE_PNG));

    Clipboard::reset_test_backend();
    const auto cleared = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(cleared.ok());
    AURORA_TEST_CHECK_EQ(cleared.value(), std::vector<std::string>{});
}

// 空口径：读不存在的格式 = Ok(空值)，写空值 = no-op 且保留既有内容。两条都要 machine-visible 地区分开
// 「空」与「失败」——把 `Ok(空)` 改成 `ClipboardFormatUnsupported` 或用空写把既有内容清掉，本用例即转红。
AURORA_TEST_CASE(empty_values_and_empty_writes_follow_the_documented_contract) {
    require_test_backend();

    // 读：什么都没写过 → Ok(空值)，不是 Error。
    const auto missing = Clipboard::get_payload("application/x-never-written");
    AURORA_TEST_REQUIRE(missing.ok());
    AURORA_TEST_CHECK_EQ(missing.value(), std::vector<std::uint8_t>{});
    const auto no_files = Clipboard::get_files();
    AURORA_TEST_REQUIRE(no_files.ok());
    AURORA_TEST_CHECK_EQ(no_files.value(), std::vector<std::string>{});
    const auto no_html = Clipboard::get_html();
    AURORA_TEST_REQUIRE(no_html.ok());
    AURORA_TEST_CHECK_EQ(no_html.value(), std::string{});

    // 写：空字节 / 空清单 / 空串全是 no-op，既有内容必须留下。
    const std::vector<std::string> paths{"/srv/aurora/keep.txt"};
    const std::string html = "<p>keep</p>";
    const std::vector<std::uint8_t> bytes{0x01U, 0x02U};
    AURORA_TEST_REQUIRE(Clipboard::set_files(paths).ok());
    AURORA_TEST_REQUIRE(Clipboard::set_html(html).ok());
    AURORA_TEST_REQUIRE(Clipboard::set_payload("application/x-probe", bytes).ok());

    AURORA_TEST_REQUIRE(Clipboard::set_files({}).ok());
    AURORA_TEST_REQUIRE(Clipboard::set_html("").ok());
    AURORA_TEST_REQUIRE(Clipboard::set_payload("application/x-probe", {}).ok());

    AURORA_TEST_CHECK_EQ(Clipboard::get_files().value(), paths);
    AURORA_TEST_CHECK_EQ(Clipboard::get_html().value(), html);
    AURORA_TEST_CHECK_EQ(Clipboard::get_payload("application/x-probe").value(), bytes);
}

// 立骨期：富格式 API 在平台路径上（测试后端已卸载）一律报「本平台无该格式机制」，
// 且该错误码的 retryable 为 false——机制缺失是稳态，调用方该改走别的通道而不是重试。
// 变异自证：把平台路径的返回改成 `Ok(空值)`（谎称成功）或沿用 `GeneralNotSupported`，
// 本用例的 code_enum 断言即转红。
AURORA_TEST_CASE(rich_formats_report_unsupported_on_platform_path) {
    (void)Clipboard::remove_test_backend();  // 走平台路径

    const auto set_payload = Clipboard::set_payload("application/x-probe", to_bytes("x"));
    AURORA_TEST_REQUIRE_FALSE(set_payload.ok());
    expect_format_unsupported(set_payload.error(), "application/x-probe");

    const auto get_payload = Clipboard::get_payload(std::string{AURORA_MIME_URI_LIST});
    AURORA_TEST_REQUIRE_FALSE(get_payload.ok());
    expect_format_unsupported(get_payload.error(), AURORA_MIME_URI_LIST);

    const auto set_files = Clipboard::set_files({"/srv/aurora/a.txt"});
    AURORA_TEST_REQUIRE_FALSE(set_files.ok());
    expect_format_unsupported(set_files.error(), AURORA_MIME_URI_LIST);

    const auto get_files = Clipboard::get_files();
    AURORA_TEST_REQUIRE_FALSE(get_files.ok());
    expect_format_unsupported(get_files.error(), AURORA_MIME_URI_LIST);

    const auto set_html = Clipboard::set_html("<p>p</p>");
    AURORA_TEST_REQUIRE_FALSE(set_html.ok());
    expect_format_unsupported(set_html.error(), AURORA_MIME_TEXT_HTML);

    const auto get_html = Clipboard::get_html();
    AURORA_TEST_REQUIRE_FALSE(get_html.ok());
    expect_format_unsupported(get_html.error(), AURORA_MIME_TEXT_HTML);

    // 末尾恢复测试后端：本用例卸载了它，不能把「已卸载」这一状态泄漏给同进程的后继用例。
    (void)Clipboard::install_test_backend();
}

// MIME 为空不是「空内容」，而是调用方参数错：空串无法作为存储键，静默接受等于留下一个读不回来的格式。
AURORA_TEST_CASE(empty_mime_payload_is_an_argument_error) {
    require_test_backend();

    const auto result = Clipboard::set_payload("", to_bytes("x"));
    AURORA_TEST_REQUIRE_FALSE(result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
}

}  // namespace aurora::test_cases::utest_clipboard
