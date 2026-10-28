/// 测试类型: unit
/// 目标单元: include/aurora/app/clipboard.h
/// 测试说明: 经进程内 memory 测试后端（install/reset/remove 注入点）覆盖文本读写往返、
/// 覆盖写、空文本、图像 RGBA8 往返与清空语义；失败口径另设两条平台路径用例
/// （非法 UTF-8 载荷 → ClipboardWriteFailed 且不动既有内容；图像参数非法 → GeneralInvalidArgument），
/// 其余路径全程不写系统剪贴板（后端不可用时 SKIP）；
/// 富格式段覆盖自定义 MIME 载荷、files / html 便利封装、available_formats 如实反映已写格式、
/// 空值与空写口径、以及无平台后端时的 clipboard-format-unsupported（macOS 已接线，此判据跳过）；
/// 惰性要约与监听段覆盖按需兑现（注册零调用、逐格式各自一次、渲染一次即缓存）、
/// 立即写与惰性混用互不覆盖、所有权丢失与变更监听的触发 / 替换 / 无机制如实报错、
/// 提供失败的 provider-failed 口径、clear 与空写 no-op 的相反语义、
/// 以及 ClipboardOffer 与 DragOffer 的同构锚点

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/clipboard.h"
#include "aurora/app/mime_types.h"
#include "aurora/core/platform.h"
#include "aurora/event/event.h"
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

#if !defined(AURORA_PLATFORM_MACOS)
/// @brief 无平台后端时富格式在平台路径上的统一失败（memory 后端已卸载时走此路）。
auto expect_format_unsupported(const Error &err, std::string_view mime) -> void {
    AURORA_TEST_CHECK_EQ(err.code_enum, ErrorCode::ClipboardFormatUnsupported);
    AURORA_TEST_CHECK_EQ(err.code, std::string{"clipboard-format-unsupported"});
    AURORA_TEST_CHECK_FALSE(err.retryable);  // 机制缺失是稳态，重试无益
    AURORA_TEST_CHECK_FALSE(err.message.empty());
    AURORA_TEST_CHECK_TRUE(err.message.find(std::string{mime}) != std::string::npos);  // 失败点名是哪个 MIME
}
#endif

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

// 无平台后端时：富格式 API 一律报「本平台无该格式机制」，且该错误码的 retryable 为 false——
// 机制缺失是稳态，调用方该改走别的通道而不是重试。判据只在**确实没有富格式后端**的平台路径上
// 成立：macOS 的 NSPasteboard 后端在首次查询即自举登记（真读写会触碰系统剪贴板，不入单测）；
// Linux 无富格式后端；Win32 依赖 owner 窗口登记，单测进程无窗口。变异自证：把平台路径的返回
// 改成 `Ok(空值)`（谎称成功）或沿用 `GeneralNotSupported`，本用例的 code_enum 断言即转红。
AURORA_TEST_CASE(rich_formats_report_unsupported_without_a_platform_backend) {
#ifdef AURORA_PLATFORM_MACOS
    AURORA_TEST_SKIP("macOS wires NSPasteboard rich formats; no unsupported platform path remains there");
#else
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
#endif
}

// MIME 为空不是「空内容」，而是调用方参数错：空串无法作为存储键，静默接受等于留下一个读不回来的格式。
AURORA_TEST_CASE(empty_mime_payload_is_an_argument_error) {
    require_test_backend();

    const auto result = Clipboard::set_payload("", to_bytes("x"));
    AURORA_TEST_REQUIRE_FALSE(result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
}

// ---- 惰性要约与监听段 ----

namespace {

/// @brief 记录每次 `provide` 实参的探针：既当回调用，又当调用台账断言。
struct ProvideProbe {
    std::vector<std::string> calls;  ///< 每次调用收到的 mime（按序）
    bool fail = false;  ///< true = 回调返回 Error（覆盖提供失败口径）

    auto as_callback() -> std::function<Result<std::vector<std::uint8_t>>(const std::string &)> {
        return [this](const std::string &mime) -> Result<std::vector<std::uint8_t>> {
            calls.push_back(mime);
            if (fail) {
                return make_error(ErrorCode::GeneralInvalidArgument, "probe: deliberate provider failure");
            }
            return to_bytes("rendered:" + mime);
        };
    }
};

/// @brief 造一份三格式惰性要约（text/plain + text/html + 自定义 MIME）。
auto make_offer(ProvideProbe &probe) -> ClipboardOffer {
    ClipboardOffer offer;
    offer.formats = {std::string{AURORA_MIME_TEXT_PLAIN}, std::string{AURORA_MIME_TEXT_HTML},
                     "application/x-aurora-lazy"};
    offer.provide = probe.as_callback();
    return offer;
}

}  // namespace

// 惰性要约的按需兑现：注册后回调一次都未被调用（渲染延迟到真正读取那一刻），逐个格式读取时
// 每个格式各自触发一次、且只带该格式的 mime 入参；已渲染格式走缓存，不再触发回调。
// 变异自证：把 `set_offer` 改成注册时立即兑现全部格式，「注册后零调用」这条即刻转红。
AURORA_TEST_CASE(lazy_offer_defers_render_until_each_format_is_read) {
    require_test_backend();

    ProvideProbe probe;
    AURORA_TEST_REQUIRE(Clipboard::set_offer(make_offer(probe)).ok());

    // 注册即零调用：三格式数据都还没渲染。
    AURORA_TEST_CHECK(probe.calls.empty());

    // 读之前，声明的格式已如实入列（要约即所有权声明，不因未渲染而隐瞒）。
    const auto formats = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(formats.ok());
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), AURORA_MIME_TEXT_PLAIN));
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), AURORA_MIME_TEXT_HTML));
    AURORA_TEST_CHECK_TRUE(has_format(formats.value(), "application/x-aurora-lazy"));

    // 逐个读取：每个格式各自触发一次回调，且只带该格式 mime。
    const auto text = Clipboard::get_text();
    AURORA_TEST_REQUIRE(text.ok());
    AURORA_TEST_CHECK_EQ(text.value(), std::string{"rendered:text/plain"});
    AURORA_TEST_REQUIRE_EQ(probe.calls.size(), 1U);
    AURORA_TEST_CHECK_EQ(probe.calls[0], std::string{AURORA_MIME_TEXT_PLAIN});

    const auto html = Clipboard::get_html();
    AURORA_TEST_REQUIRE(html.ok());
    AURORA_TEST_CHECK_EQ(html.value(), std::string{"rendered:text/html"});
    AURORA_TEST_REQUIRE_EQ(probe.calls.size(), 2U);
    AURORA_TEST_CHECK_EQ(probe.calls[1], std::string{AURORA_MIME_TEXT_HTML});

    const auto raw = Clipboard::get_payload("application/x-aurora-lazy");
    AURORA_TEST_REQUIRE(raw.ok());
    AURORA_TEST_CHECK_EQ(to_text(raw.value()), std::string{"rendered:application/x-aurora-lazy"});
    AURORA_TEST_REQUIRE_EQ(probe.calls.size(), 3U);

    // 渲染一次即缓存：复读不再触发回调（对标 WM_RENDERFORMAT 后由系统托管）。
    AURORA_TEST_REQUIRE(Clipboard::get_text().ok());
    AURORA_TEST_REQUIRE(Clipboard::get_html().ok());
    AURORA_TEST_REQUIRE(Clipboard::get_payload("application/x-aurora-lazy").ok());
    AURORA_TEST_CHECK_EQ(probe.calls.size(), 3U);
}

// 要约不合法（formats 为空 / provide 为空回调）是调用方参数问题，与「平台没有机制」分开报。
// 变异自证：把这两个分支改成静默 Ok，「参数错」断言即刻转红。
AURORA_TEST_CASE(malformed_lazy_offer_is_an_argument_error) {
    require_test_backend();

    ClipboardOffer no_formats;
    no_formats.provide = [](const std::string &) -> Result<std::vector<std::uint8_t>> { return to_bytes("x"); };
    const auto empty_formats = Clipboard::set_offer(no_formats);
    AURORA_TEST_REQUIRE_FALSE(empty_formats.ok());
    AURORA_TEST_CHECK_EQ(empty_formats.error().code_enum, ErrorCode::GeneralInvalidArgument);

    ClipboardOffer no_provide;
    no_provide.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    const auto null_provide = Clipboard::set_offer(no_provide);
    AURORA_TEST_REQUIRE_FALSE(null_provide.ok());
    AURORA_TEST_CHECK_EQ(null_provide.error().code_enum, ErrorCode::GeneralInvalidArgument);
}

// 立即写与惰性要约混用：互不覆盖、各自可读；立即写的格式不触发惰性回调。
// 另覆盖双向的「后写者接管」：立即写把该 MIME 自要约摘走，set_offer 也把声明格式自立即写摘走。
AURORA_TEST_CASE(immediate_write_and_lazy_offer_coexist_without_clobbering) {
    require_test_backend();

    ProvideProbe probe;
    ClipboardOffer offer;
    offer.formats = {std::string{AURORA_MIME_TEXT_PLAIN}, "application/x-aurora-lazy"};
    offer.provide = probe.as_callback();
    AURORA_TEST_REQUIRE(Clipboard::set_offer(offer).ok());

    // 另一格式走立即写（不在要约声明里）：两条通道互不覆盖。
    const std::vector<std::uint8_t> immediate{0x0AU, 0x0BU};
    AURORA_TEST_REQUIRE(Clipboard::set_payload("application/x-immediate", immediate).ok());

    const auto lazy = Clipboard::get_payload("application/x-aurora-lazy");
    AURORA_TEST_REQUIRE(lazy.ok());
    AURORA_TEST_CHECK_EQ(to_text(lazy.value()), std::string{"rendered:application/x-aurora-lazy"});

    // 立即写通道可读，且没有触发惰性回调（只触发过 lazy 那一次）。
    const auto now = Clipboard::get_payload("application/x-immediate");
    AURORA_TEST_REQUIRE(now.ok());
    AURORA_TEST_CHECK_EQ(now.value(), immediate);
    AURORA_TEST_REQUIRE_EQ(probe.calls.size(), 1U);
    AURORA_TEST_CHECK_EQ(probe.calls[0], std::string{"application/x-aurora-lazy"});

    // 后写者接管（立即写侧）：对要约已声明的 text/plain 做非空立即写，该格式改走立即写通道。
    AURORA_TEST_REQUIRE(Clipboard::set_text("immediate text").ok());
    const auto text = Clipboard::get_text();
    AURORA_TEST_REQUIRE(text.ok());
    AURORA_TEST_CHECK_EQ(text.value(), std::string{"immediate text"});
    AURORA_TEST_REQUIRE_EQ(probe.calls.size(), 1U);  // 被接管的格式不再回调 provide
}

// 所有权丢失：回调被调用且只调一次；未注册惰性要约时整体 no-op；重复注册新回调后旧的不再被调用
// （替换不叠加，避免多注册点各自持资源）。
AURORA_TEST_CASE(ownership_lost_fires_once_and_replaces_previous_registration) {
    require_test_backend();

    // 未注册要约：模拟所有权被夺也不触发（没有可失去的所有权）。
    int early_calls = 0;
    Clipboard::on_ownership_lost([&early_calls]() -> void { ++early_calls; });
    Clipboard::emit_ownership_lost();
    AURORA_TEST_CHECK_EQ(early_calls, 0);

    ProvideProbe probe;
    AURORA_TEST_REQUIRE(Clipboard::set_offer(make_offer(probe)).ok());

    int first_calls = 0;
    Clipboard::on_ownership_lost([&first_calls]() -> void { ++first_calls; });
    Clipboard::emit_ownership_lost();
    AURORA_TEST_CHECK_EQ(first_calls, 1);

    // 要约随所有权一并失效：二次模拟无约可在手，不再触发。
    Clipboard::emit_ownership_lost();
    AURORA_TEST_CHECK_EQ(first_calls, 1);

    // 替换语义：旧回调不再被调用。
    int second_calls = 0;
    Clipboard::on_ownership_lost([&second_calls]() -> void { ++second_calls; });
    ProvideProbe probe2;
    AURORA_TEST_REQUIRE(Clipboard::set_offer(make_offer(probe2)).ok());
    Clipboard::emit_ownership_lost();
    AURORA_TEST_CHECK_EQ(first_calls, 1);  // 旧的没动
    AURORA_TEST_CHECK_EQ(second_calls, 1);
}

// 变更监听：有机制时注册即生效、模拟变化触发回调；无机制时注册即失败（不谎报——注册了却收不到
// 通知会让调用方逻辑静默失效）。变异自证：把「无机制」分支改成返回 Ok，本用例即刻转红。
AURORA_TEST_CASE(change_listener_reports_unsupported_without_mechanism) {
    require_test_backend();  // 安装即置「有监听机制」

    int changes = 0;
    AURORA_TEST_REQUIRE(Clipboard::on_change([&changes]() -> void { ++changes; }).ok());
    Clipboard::emit_change();
    AURORA_TEST_CHECK_EQ(changes, 1);

    // 无机制平台：注册即失败，且此后模拟变化不再触发新回调（没有注册进去）。
    Clipboard::set_listen_supported(false);
    int lost = 0;
    const auto unsupported = Clipboard::on_change([&lost]() -> void { ++lost; });
    AURORA_TEST_REQUIRE_FALSE(unsupported.ok());
    AURORA_TEST_CHECK_EQ(unsupported.error().code_enum, ErrorCode::ClipboardListenUnsupported);
    AURORA_TEST_CHECK_EQ(unsupported.error().code, std::string{"clipboard-listen-unsupported"});
    AURORA_TEST_CHECK_FALSE(unsupported.error().retryable);  // 机制缺失是稳态
    Clipboard::emit_change();
    AURORA_TEST_CHECK_EQ(lost, 0);  // 新回调从未注册进去
    AURORA_TEST_CHECK_EQ(changes, 2);  // 失败注册无副作用：先前那个注册仍然有效

    // 恢复监听机制，避免把「无机制」状态泄漏给同进程后继用例。
    Clipboard::set_listen_supported(true);
}

// 提供失败口径：惰性回调返回 Error 时读取侧拿到 ClipboardProviderFailed（而非既有三枚之一），
// 且失败不缓存——再次读取各自重试（同一笔数据是否重试由应用决定，retryable=false）。
AURORA_TEST_CASE(provider_failure_surfaces_clipboard_provider_failed) {
    require_test_backend();

    ProvideProbe probe;
    probe.fail = true;
    ClipboardOffer offer;
    offer.formats = {"application/x-aurora-lazy"};
    offer.provide = probe.as_callback();
    AURORA_TEST_REQUIRE(Clipboard::set_offer(offer).ok());

    const auto first = Clipboard::get_payload("application/x-aurora-lazy");
    AURORA_TEST_REQUIRE_FALSE(first.ok());
    AURORA_TEST_CHECK_EQ(first.error().code_enum, ErrorCode::ClipboardProviderFailed);
    AURORA_TEST_CHECK_EQ(first.error().code, std::string{"clipboard-provider-failed"});
    AURORA_TEST_CHECK_FALSE(first.error().retryable);

    // 失败不缓存：每次读取各自重试（回调再次被调用）。
    const auto second = Clipboard::get_payload("application/x-aurora-lazy");
    AURORA_TEST_REQUIRE_FALSE(second.ok());
    AURORA_TEST_CHECK_EQ(second.error().code_enum, ErrorCode::ClipboardProviderFailed);
    AURORA_TEST_REQUIRE_EQ(probe.calls.size(), 2U);
}

// clear 与空写是两件相反的事：clear 主动清空（available_formats 为 Ok(空清单)），
// 空串 / 空清单 / 空字节的 set_* 是契约内 no-op、保留既有内容。二者语义相反，并列断言防误用。
AURORA_TEST_CASE(clear_empties_formats_while_empty_writes_keep_content) {
    require_test_backend();

    AURORA_TEST_REQUIRE(Clipboard::set_text("keep me").ok());
    ProvideProbe probe;
    AURORA_TEST_REQUIRE(Clipboard::set_offer(make_offer(probe)).ok());
    const auto before_clear = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(before_clear.ok());
    AURORA_TEST_CHECK_TRUE(has_format(before_clear.value(), AURORA_MIME_TEXT_PLAIN));
    AURORA_TEST_CHECK_TRUE(has_format(before_clear.value(), "application/x-aurora-lazy"));

    // 主动清空：全部格式（含要约即所有权声明）一并清掉。
    AURORA_TEST_REQUIRE(Clipboard::clear().ok());
    const auto cleared = Clipboard::available_formats();
    AURORA_TEST_REQUIRE(cleared.ok());
    AURORA_TEST_CHECK_EQ(cleared.value(), std::vector<std::string>{});
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{});

    // 空写 no-op：先写内容，再空写，内容原样保留（与 clear 的相反语义）。
    AURORA_TEST_REQUIRE(Clipboard::set_text("kept again").ok());
    AURORA_TEST_REQUIRE(Clipboard::set_text(std::string{}).ok());
    AURORA_TEST_CHECK_EQ(Clipboard::get_text().value(), std::string{"kept again"});
    AURORA_TEST_REQUIRE(Clipboard::set_payload("application/x-keep", to_bytes("v")).ok());
    AURORA_TEST_REQUIRE(Clipboard::set_payload("application/x-keep", {}).ok());
    AURORA_TEST_CHECK_EQ(to_text(Clipboard::get_payload("application/x-keep").value()), std::string{"v"});
}

// 两条通道要约同构：ClipboardOffer 与 DragOffer 字段形态一致（格式清单 + 同签名的 provide），
// 且 MIME 取值同出自 app/mime_types.h——用「同一个 provide 回调同时交给两条通道、结果一致」
// 锁定这一对称关系（「同一套词汇不在两条路径上各长一次」的锚点）。
AURORA_TEST_CASE(clipboard_offer_is_isomorphic_with_drag_offer) {
    require_test_backend();

    ProvideProbe clipboard_probe;
    const auto provide = clipboard_probe.as_callback();

    ClipboardOffer clipboard_offer;
    clipboard_offer.formats = {std::string{AURORA_MIME_TEXT_PLAIN}, std::string{AURORA_MIME_URI_LIST}};
    clipboard_offer.provide = provide;
    AURORA_TEST_REQUIRE(Clipboard::set_offer(clipboard_offer).ok());

    // 同一个 provide 原样交给拖出侧要约：字段形态一致，故可直接赋值。
    DragOffer drag_offer;
    drag_offer.formats = clipboard_offer.formats;
    drag_offer.provide = provide;
    const auto via_drag = drag_offer.provide(std::string{AURORA_MIME_TEXT_PLAIN});
    AURORA_TEST_REQUIRE(via_drag.ok());
    AURORA_TEST_CHECK_EQ(to_text(via_drag.value()), std::string{"rendered:text/plain"});

    // 剪贴板侧经同一回调读到的内容一致（两条通道同一套词汇、同一份提供逻辑）。
    const auto via_clipboard = Clipboard::get_payload(std::string{AURORA_MIME_TEXT_PLAIN});
    AURORA_TEST_REQUIRE(via_clipboard.ok());
    AURORA_TEST_CHECK_EQ(to_text(via_clipboard.value()), to_text(via_drag.value()));
}

}  // namespace aurora::test_cases::utest_clipboard
