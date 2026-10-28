// macos_clipboard.cpp — macOS 剪贴板富格式后端（NSPasteboard）。
//
// 门面的函数表在此逐项落到 general pasteboard；MIME ↔ pasteboard type 的映射、文件清单的双档
// 写入、惰性提供的 data provider 与所有权丢失判定都在本 TU。契约与两处平台差异（无变更监听、
// 整块替换）见 `macos_clipboard.h` 文件头。
#include "aurora/app/detail/macos_clipboard.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/clipboard.h"
#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

namespace aurora::detail {

/// @brief 桥的进程级状态：当前惰性要约与所有权记账（单线程 UI 契约，无锁）。
struct ClipboardBridgeState {
    bool has_offer = false;  ///< 是否持有惰性提供要约（即是否持有剪贴板所有权）
    ClipboardOffer offer;  ///< 惰性要约（格式清单 + provide 回调）
    NSInteger change_count = 0;  ///< 写入要约后的 pasteboard changeCount（区分「数据已提供」与「所有权易主」）
    void *provider = nullptr;  ///< 当前 data provider（本状态持有一份；void * 避开本结构对 ObjC 类的依赖）
    unsigned long long generation = 0;  ///< 要约代号：旧 provider 的迟到回调据此丢弃
};

/// @brief 进程唯一状态（函数内 static，规避静态初始化顺序问题）。
auto clipboard_state() -> ClipboardBridgeState & {
    static ClipboardBridgeState s;
    return s;
}

namespace {

/// @brief UTF-8 字符串 → NSString；非法 UTF-8 返回 nil。
[[nodiscard]] auto ns_string(const std::string &text) -> NSString * {
    return [NSString stringWithUTF8String:text.c_str()];
}

/// @brief NSString → UTF-8 字符串；nil 返回空串。
[[nodiscard]] auto to_utf8(NSString *text) -> std::string {
    if (text == nil) {
        return {};
    }
    const char *utf8 = [text UTF8String];
    return utf8 != nullptr ? std::string{utf8} : std::string{};
}

/// @brief MIME → pasteboard type。
///
/// 标准档走 AppKit 的公开 type 常量；其余 MIME 以字面量作自定义 type（与 Win32 以 MIME 注册
/// 自定义剪贴板格式同口径）。MIME 非法（非 UTF-8）时返回 nil。
[[nodiscard]] auto type_of_mime(const std::string &mime) -> NSString * {
    if (mime == AURORA_MIME_TEXT_PLAIN) {
        return NSPasteboardTypeString;
    }
    if (mime == AURORA_MIME_IMAGE_PNG) {
        return NSPasteboardTypePNG;
    }
    if (mime == AURORA_MIME_TEXT_HTML) {
        return NSPasteboardTypeHTML;
    }
    if (mime == AURORA_MIME_URI_LIST) {
        return @"text/uri-list";
    }
    return ns_string(mime);
}

/// @brief pasteboard type → MIME（供 `available_formats` 如实枚举）。不认识的 type 返回 nullopt。
[[nodiscard]] auto mime_of_type(NSString *type) -> std::optional<std::string> {
    if (type == nil) {
        return std::nullopt;
    }
    if ([type isEqualToString:NSPasteboardTypeString]) {
        return std::string{AURORA_MIME_TEXT_PLAIN};
    }
    if ([type isEqualToString:NSPasteboardTypePNG]) {
        return std::string{AURORA_MIME_IMAGE_PNG};
    }
    if ([type isEqualToString:NSPasteboardTypeHTML]) {
        return std::string{AURORA_MIME_TEXT_HTML};
    }
    // 文件清单在 pasteboard 上的三种历史形态统一折算为 uri-list（本库消费方只认这一档）。
    if ([type isEqualToString:@"text/uri-list"] || [type isEqualToString:NSPasteboardTypeFileURL] ||
        [type isEqualToString:@"NSFilenamesPboardType"]) {
        return std::string{AURORA_MIME_URI_LIST};
    }
    // 自定义 MIME 以字面量作 type：含 "/" 的非 UTI 串按 MIME 交回（UTI 不含斜杠）。
    if ([type containsString:@"/"]) {
        return to_utf8(type);
    }
    return std::nullopt;
}

/// @brief 撤销当前惰性要约（若存在），并如实触发一次所有权丢失。
///
/// 与 Win32 `EmptyClipboard` 的语义对齐：本进程的写入接管整块剪贴板，旧要约随旧内容一起失效。
/// 必须先于 `clearContents` 调用：此时 pasteboard 仍归本进程所有，回调语义是「你主动放弃了它」。
auto discard_offer() -> void {
    auto &state = clipboard_state();
    if (!state.has_offer) {
        return;
    }
    state.has_offer = false;
    state.offer = ClipboardOffer{};
    ++state.generation;  // 旧 provider 的迟到回调据此丢弃
    if (state.provider != nullptr) {
        [(id)state.provider release];
        state.provider = nullptr;
    }
    clipboard::detail::fire_ownership_lost();
}

}  // namespace

}  // namespace aurora::detail

// 惰性提供的 data provider：每个要约一个实例，格式请求到达时按 MIME 调 offer.provide。
// 声明与实现放在 C++ 命名空间之外（ObjC 类不进命名空间），状态经 `owner` 以 void * 回指。
@interface AuroraMacPasteboardProvider : NSObject <NSPasteboardItemDataProvider> {
  @public
    void *owner;
    unsigned long long generation;
}
@end

@implementation AuroraMacPasteboardProvider

- (void)pasteboard:(NSPasteboard *)pasteboard item:(NSPasteboardItem *)item provideDataForType:(NSPasteboardType)type {
    auto *state = static_cast<aurora::detail::ClipboardBridgeState *>(owner);
    if (state == nullptr || !state->has_offer || generation != state->generation) {
        return;  // 要约已被替换 / 撤销：迟到的请求不兑现
    }
    const auto mime = aurora::detail::mime_of_type(type);
    if (!mime) {
        return;
    }
    if (std::find(state->offer.formats.begin(), state->offer.formats.end(), *mime) == state->offer.formats.end()) {
        return;  // 该格式不由本要约声明
    }
    const auto produced = state->offer.provide(*mime);
    if (!produced || produced.value().empty()) {
        // 提供失败不提交数据：请求方得到「读不到」，而不是一份空数据（与 Win32 延迟渲染同口径）。
        return;
    }
    const auto &bytes = produced.value();
    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    [item setData:data forType:type];
}

- (void)pasteboardFinishedWithDataProvider:(NSPasteboard *)pasteboard {
    auto *state = static_cast<aurora::detail::ClipboardBridgeState *>(owner);
    if (state == nullptr || !state->has_offer || generation != state->generation) {
        return;
    }
    if ([pasteboard changeCount] == state->change_count) {
        return;  // 全部数据已提供，所有权仍在本进程：不是所有权丢失
    }
    state->has_offer = false;
    state->offer = aurora::ClipboardOffer{};
    ++state->generation;  // 后续迟到回调一律丢弃
    // 所有权已易主：如实通知应用。AppKit 不保证回调线程，统一折回主线程（框架的回调契约）。
    if ([NSThread isMainThread]) {
        aurora::clipboard::detail::fire_ownership_lost();
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
          aurora::clipboard::detail::fire_ownership_lost();
        });
    }
}

@end

namespace aurora::detail {
namespace {

// ---- 进程级后端单槽的实现面 ----

auto platform_set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes) -> Result<bool> {
    @autoreleasepool {
        NSString *type = type_of_mime(mime);
        if (type == nil) {
            return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", mime}});
        }
        if (bytes.empty()) {
            return Result<bool>{true};  // 空载荷 no-op（门面已先行判定，本层同口径兜底）
        }
        NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        if (data == nil) {
            return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", "NSData allocation failed"}});
        }
        discard_offer();
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];  // 平台路径的语义是「后写者接管整块剪贴板」
        if ([pasteboard setData:data forType:type] == NO) {
            return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"mime", mime}});
        }
        return Result<bool>{true};
    }
}

auto platform_get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    @autoreleasepool {
        NSString *type = type_of_mime(mime);
        if (type == nil) {
            return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", mime}});
        }
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSData *data = [pasteboard dataForType:type];  // 惰性要约在此触发 provide
        if (data == nil) {
            return std::vector<std::uint8_t>{};  // 无该格式 = Ok(空字节)，不是失败
        }
        const auto *begin = static_cast<const std::uint8_t *>([data bytes]);
        return std::vector<std::uint8_t>(begin, begin + static_cast<std::size_t>([data length]));
    }
}

auto platform_set_files(const std::vector<std::string> &paths) -> Result<bool> {
    @autoreleasepool {
        NSMutableArray<NSURL *> *urls = [[NSMutableArray alloc] init];
        for (const auto &path : paths) {
            NSString *text = ns_string(path);
            if (text == nil || [text length] == 0) {
                [urls release];
                return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", "path is not valid UTF-8"}});
            }
            [urls addObject:[NSURL fileURLWithPath:text]];
        }
        discard_offer();
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        BOOL written = YES;
        if ([urls count] > 0) {
            written = [pasteboard writeObjects:urls];  // 原生文件清单（public.file-url）
        }
        if (written != NO && [urls count] > 0) {
            // 同一次写入附带 uri-list 文本：与 Win32 同时放 CF_HDROP 与 uri-list 的两档口径一致。
            NSString *encoded = ns_string(uri_list::encode(paths));
            if (encoded != nil) {
                [pasteboard setString:encoded forType:@"text/uri-list"];
            }
        }
        [urls release];
        if (written == NO) {
            return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", "writeObjects failed"}});
        }
        return Result<bool>{true};
    }
}

auto platform_get_files() -> Result<std::vector<std::string>> {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                           options:@{
                                                               NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                           }];
        std::vector<std::string> paths;
        for (NSURL *url in urls) {
            if ([url isFileURL] == NO) {
                continue;
            }
            const std::string path = to_utf8([url path]);
            if (!path.empty()) {
                paths.push_back(path);
            }
        }
        if (!paths.empty()) {
            return paths;
        }
        // 无原生文件清单时回退 uri-list 文本档（本库自己的写入两档并存，跨应用只剩文本时也能读）。
        NSString *encoded = [pasteboard stringForType:@"text/uri-list"];
        if (encoded == nil) {
            return std::vector<std::string>{};  // 无文件清单 = Ok(空清单)
        }
        return uri_list::decode(to_utf8(encoded));
    }
}

auto platform_set_html(const std::string &html) -> Result<bool> {
    @autoreleasepool {
        NSString *text = ns_string(html);
        if (text == nil) {
            return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", "html is not valid UTF-8"}});
        }
        discard_offer();
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        if ([pasteboard setString:text forType:NSPasteboardTypeHTML] == NO) {
            return make_error(ErrorCode::ClipboardWriteFailed,
                              ErrorParams{{"mime", std::string{AURORA_MIME_TEXT_HTML}}});
        }
        return Result<bool>{true};
    }
}

auto platform_get_html() -> Result<std::string> {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSString *text = [pasteboard stringForType:NSPasteboardTypeHTML];
        return to_utf8(text);  // 无 HTML = Ok("")
    }
}

auto platform_set_offer(const ClipboardOffer &offer) -> Result<void> {
    @autoreleasepool {
        if (offer.formats.empty() || !offer.provide) {
            return make_error(ErrorCode::ClipboardFormatUnsupported,
                              ErrorParams{{"detail", "offer has no formats or no provide callback"}});
        }
        auto &state = clipboard_state();
        discard_offer();  // 自替换：旧要约随新写入失效并如实触发所有权丢失
        AuroraMacPasteboardProvider *provider = [[AuroraMacPasteboardProvider alloc] init];
        provider->owner = &state;
        ++state.generation;
        provider->generation = state.generation;
        NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
        NSMutableArray<NSString *> *types = [[NSMutableArray alloc] init];
        for (const auto &mime : offer.formats) {
            NSString *type = type_of_mime(mime);
            if (type == nil) {
                [types release];
                [item release];
                [provider release];
                return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", mime}});
            }
            [types addObject:type];
        }
        [item setDataProvider:provider forTypes:types];
        [types release];
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        const BOOL written = [pasteboard writeObjects:@[ item ]];
        [item release];
        if (written == NO) {
            [provider release];
            return make_error(ErrorCode::ClipboardWriteFailed,
                              ErrorParams{{"detail", "writing the lazy pasteboard item failed"}});
        }
        state.change_count = [pasteboard changeCount];
        state.offer = offer;
        state.has_offer = true;
        state.provider = provider;  // alloc 的 +1 转给状态（直到被下一次写入替换）
        AURORA_LOG_DEBUG("clipboard", "registered a lazy offer with ", offer.formats.size(), " formats");
        return Result<void>{};
    }
}

auto platform_available_formats() -> Result<std::vector<std::string>> {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        std::vector<std::string> formats;
        for (NSString *type in [pasteboard types]) {
            const auto mime = mime_of_type(type);
            if (!mime) {
                continue;  // 本库不认识的 type 不入列：清单回答的是「你能读到什么」
            }
            if (std::find(formats.begin(), formats.end(), *mime) == formats.end()) {
                formats.push_back(*mime);
            }
        }
        return formats;
    }
}

auto install_platform_backend() -> void {
    clipboard::detail::install_backend(clipboard::detail::PlatformBackend{
        .set_payload = platform_set_payload,
        .get_payload = platform_get_payload,
        .set_files = platform_set_files,
        .get_files = platform_get_files,
        .set_html = platform_set_html,
        .get_html = platform_get_html,
        .set_offer = platform_set_offer,
        .available_formats = platform_available_formats,
        // 不填 set_change_listener：NSPasteboard 无变更通知机制，注册监听如实报不支持。
    });
}

}  // namespace

auto macos_clipboard_ensure_registered() -> bool {
    // 进程级一次性登记：NSPasteboard 是应用级资源，与窗口宿主无关。
    static const bool registered = []() -> bool {
        install_platform_backend();
        return true;
    }();
    return registered;
}

auto macos_clipboard_has_offer() -> bool { return clipboard_state().has_offer; }

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
