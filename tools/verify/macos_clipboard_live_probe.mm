/* macOS 剪贴板富格式后端真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ `Clipboard::set_payload` / `get_payload` 经 `NSPasteboard` 往返（标准 MIME 与自定义 MIME）；
//   ✅ 与既有 `pbcopy` / `pbpaste` 文本通路互操作（NSPasteboardTypeString 即系统文本档）；
//   ✅ `set_html` / `get_html` 往返，`available_formats` 如实列出 text/html；
//   ✅ `set_files` / `get_files` 往返（原生 `public.file-url` + `text/uri-list` 双档）；
//   ✅ 惰性提供：`set_offer` 后 `get_payload` 触发 `provide`，读回字节一致；
//   ✅ 所有权丢失：持有要约时被本进程的立即写入接管，`on_ownership_lost` 回调被触发一次，
//      要约状态归零；`on_change` 如实返回 `clipboard-listen-unsupported`（NSPasteboard 无变更通知）。
//
//   ❌ 不证明**另一个应用**读取本进程的惰性要约（跨进程场景需要人手动复制粘贴）；本探针用
//      同进程读取触发真实的 data provider 回调，不冒充跨应用验收。
//
// ⚠️ 本探针会改写系统剪贴板。运行前保存文本并在结束时尽力恢复；非文本内容无法恢复，请在
//    可丢弃剪贴板的会话中运行（虚拟机 / 专用测试机）。
//
// 构建与运行：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_clipboard
//   ./build-macos/aurora_verify_macos_clipboard
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_clipboard 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/clipboard.h"
#include "aurora/app/detail/macos_clipboard.h"
#include "aurora/app/mime_types.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "verify_print.h"

namespace {

int g_checks = 0;
int g_failures = 0;

auto check(bool ok, const std::string &name, const std::string &detail = {}) -> void {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 48), detail, "\n");
}

auto bytes_of(const std::string &text) -> std::vector<std::uint8_t> { return {text.begin(), text.end()}; }

auto text_of(const std::vector<std::uint8_t> &bytes) -> std::string { return {bytes.begin(), bytes.end()}; }

auto contains(const std::vector<std::string> &formats, const std::string &mime) -> bool {
    return std::find(formats.begin(), formats.end(), mime) != formats.end();
}

}  // namespace

auto main() -> int {
    @autoreleasepool {
        const auto saved = aurora::Clipboard::get_text();
        const std::string restore_text = saved.ok() ? saved.value() : std::string{};

        // ---- 原始载荷：标准 MIME 往返 ----
        const auto set_plain = aurora::Clipboard::set_payload(std::string{aurora::AURORA_MIME_TEXT_PLAIN}, bytes_of("aurora"));
        check(set_plain.ok() && set_plain.value(), "set_payload(text/plain) succeeds");
        const auto get_plain = aurora::Clipboard::get_payload(std::string{aurora::AURORA_MIME_TEXT_PLAIN});
        check(get_plain.ok() && text_of(get_plain.value()) == "aurora", "get_payload(text/plain) round-trips");
        const auto pbpaste_text = aurora::Clipboard::get_text();
        check(pbpaste_text.ok() && pbpaste_text.value() == "aurora", "NSPasteboard text is visible to pbpaste path");
        const auto formats_after_text = aurora::Clipboard::available_formats();
        check(formats_after_text.ok() &&
                  contains(formats_after_text.value(), std::string{aurora::AURORA_MIME_TEXT_PLAIN}),
              "available_formats lists text/plain");

        // ---- 反向互操作：pbcopy 写入的文本能被 NSPasteboard 读到 ----
        check(aurora::Clipboard::set_text("from-pbcopy").ok(), "set_text via pbcopy succeeds");
        const auto get_copied = aurora::Clipboard::get_payload(std::string{aurora::AURORA_MIME_TEXT_PLAIN});
        check(get_copied.ok() && text_of(get_copied.value()) == "from-pbcopy",
              "pbcopy-written text is visible to get_payload");

        // ---- HTML ----
        const std::string html = "<b>aurora</b>";
        check(aurora::Clipboard::set_html(html).ok(), "set_html succeeds");
        const auto got_html = aurora::Clipboard::get_html();
        check(got_html.ok() && got_html.value() == html, "get_html round-trips");
        const auto formats_after_html = aurora::Clipboard::available_formats();
        check(formats_after_html.ok() && contains(formats_after_html.value(), std::string{aurora::AURORA_MIME_TEXT_HTML}),
              "available_formats lists text/html");

        // ---- 自定义 MIME ----
        const std::string custom_mime = "application/x-aurora-probe";
        const std::vector<std::uint8_t> custom_bytes{0x00, 0x01, 0xFE, 0xFF};
        check(aurora::Clipboard::set_payload(custom_mime, custom_bytes).ok(), "set_payload(custom MIME) succeeds");
        const auto got_custom = aurora::Clipboard::get_payload(custom_mime);
        check(got_custom.ok() && got_custom.value() == custom_bytes, "custom MIME round-trips byte-exactly");

        // ---- 文件清单 ----
        const auto temp_dir = std::filesystem::temp_directory_path() / "aurora_clipboard_probe";
        std::filesystem::create_directories(temp_dir);
        const auto file_a = temp_dir / "a.txt";
        const auto file_b = temp_dir / "b.txt";
        { std::ofstream(file_a) << "a"; }
        { std::ofstream(file_b) << "b"; }
        const std::vector<std::string> files{file_a.string(), file_b.string()};
        check(aurora::Clipboard::set_files(files).ok(), "set_files succeeds");
        const auto got_files = aurora::Clipboard::get_files();
        check(got_files.ok() && got_files.value() == files, "get_files round-trips the path list");
        const auto formats_after_files = aurora::Clipboard::available_formats();
        check(formats_after_files.ok() &&
                  contains(formats_after_files.value(), std::string{aurora::AURORA_MIME_URI_LIST}),
              "available_formats lists text/uri-list for a file list");

        // ---- 惰性提供 ----
        int provide_calls = 0;
        aurora::ClipboardOffer offer;
        offer.formats = {std::string{aurora::AURORA_MIME_TEXT_PLAIN}, std::string{aurora::AURORA_MIME_TEXT_HTML}};
        offer.provide = [&provide_calls](const std::string &mime) -> aurora::Result<std::vector<std::uint8_t>> {
            ++provide_calls;
            if (mime == aurora::AURORA_MIME_TEXT_PLAIN) {
                return bytes_of("lazy-text");
            }
            if (mime == aurora::AURORA_MIME_TEXT_HTML) {
                return bytes_of("<i>lazy</i>");
            }
            return aurora::make_error(aurora::ErrorCode::ClipboardFormatUnsupported, "unexpected mime");
        };
        int ownership_lost = 0;
        aurora::Clipboard::on_ownership_lost([&ownership_lost]() -> void { ++ownership_lost; });
        check(aurora::Clipboard::set_offer(offer).ok(), "set_offer succeeds");
        check(aurora::detail::macos_clipboard_has_offer(), "bridge reports a live offer");
        const auto lazy_text = aurora::Clipboard::get_payload(std::string{aurora::AURORA_MIME_TEXT_PLAIN});
        check(lazy_text.ok() && text_of(lazy_text.value()) == "lazy-text" && provide_calls == 1,
              "lazy text is materialized through provide");
        const auto lazy_html = aurora::Clipboard::get_html();
        check(lazy_html.ok() && lazy_html.value() == "<i>lazy</i>" && provide_calls == 2,
              "lazy html is materialized through provide");

        // 立即写入接管：旧要约失效并触发一次所有权丢失（与 Win32 EmptyClipboard 同口径）。
        check(aurora::Clipboard::set_payload(std::string{aurora::AURORA_MIME_TEXT_PLAIN}, bytes_of("direct")).ok(),
              "a direct write supersedes the lazy offer");
        check(!aurora::detail::macos_clipboard_has_offer(), "superseded offer is no longer reported");
        check(ownership_lost == 1, "ownership-lost callback fired once", "count=" + aurora_verify::format_int(ownership_lost));

        // ---- 无变更监听的如实申报 ----
        const auto listener = aurora::Clipboard::on_change([]() -> void {});
        check(!listener.ok() && listener.error().code_enum == aurora::ErrorCode::ClipboardListenUnsupported,
              "on_change reports clipboard-listen-unsupported on macOS");
        aurora::Clipboard::on_change(nullptr);
        aurora::Clipboard::on_ownership_lost(nullptr);

        // ---- 清空 ----
        check(aurora::Clipboard::clear().ok(), "clear succeeds");
        const auto after_clear = aurora::Clipboard::get_payload(std::string{aurora::AURORA_MIME_TEXT_PLAIN});
        check(after_clear.ok() && after_clear.value().empty(), "clear leaves no readable text payload");

        // 尽力恢复运行前的文本内容（非文本内容无法恢复）。
        if (!restore_text.empty()) {
            static_cast<void>(aurora::Clipboard::set_text(restore_text));
        }
        std::error_code ignored;
        std::filesystem::remove_all(temp_dir, ignored);

        AURORA_LOG_RAW("verify", "\n", g_failures == 0 ? "ALL PASS" : "FAILURES", " (",
                       aurora_verify::format_int(g_checks), " checks, ", aurora_verify::format_int(g_failures),
                       " failed)\n");
    }
    return g_failures == 0 ? 0 : 1;
}
