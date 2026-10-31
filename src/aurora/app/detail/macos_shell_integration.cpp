// macos_shell_integration.cpp — macOS 桌面外壳集成桥（URL 方案 / 文件关联 / 登录项 /
// 单实例 / 打印会话）。
//
// 本文件的四条硬前提（与 `06-app-platform.md` 的 macOS 一节一致）：
// 1. URL 方案与文件关联在 macOS 上是**包内静态声明**（`CFBundleURLTypes` /
//    `CFBundleDocumentTypes`），运行期只能刷新 Launch Services 登记，不能凭空加一条；不在
//    `.app` 包内或声明缺失时如实返回 `X-failed` 并点名原因（机制在、桥已接、此刻装不进去）。
// 2. 登录项走 `SMAppService`（macOS 13+），沙箱形态与系统未注册形态如实报三态。
// 3. 单实例借用 AppKit 的应用级唤起通道（`kAEGetURL`）：URL 打开时 Launch Services 唤起
//    已在运行的实例并投递 Apple Event，不需要另造跨进程通道。
// 4. 打印走 AppKit 打印操作，一律不弹系统打印面板（本库是 UI 库，面板自绘；与 Windows 的
//    `StartDoc` 会话同语义）。
#include "aurora/app/detail/macos_shell_integration.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

namespace aurora::detail {

namespace {

constexpr double AURORA_POINTS_PER_MM = 72.0 / 25.4;

/// @brief UTF-8 → NSString（nil 非法 UTF-8）。
[[nodiscard]] auto ns_string(const std::string &text) -> NSString * {
    return [NSString stringWithUTF8String:text.c_str()];
}

/// @brief NSString → UTF-8（nil 为空串）。
[[nodiscard]] auto to_utf8(NSString *text) -> std::string {
    if (text == nil) {
        return {};
    }
    const char *utf8 = [text UTF8String];
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

/// @brief NSError → 失败明因（nil 为空串）。
[[nodiscard]] auto error_detail(NSError *error) -> std::string {
    if (error == nil) {
        return {};
    }
    NSString *text = [error localizedDescription];
    return to_utf8(text != nil ? text : [error description]);
}

/// @brief 当前进程的 bundle identifier；不在 `.app` 包内为 `std::nullopt`。
[[nodiscard]] auto bundle_identifier() -> std::optional<std::string> {
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *path = [bundle bundlePath];
    NSString *identifier = [bundle bundleIdentifier];
    if (path == nil || identifier == nil || ![path hasSuffix:@".app"]) {
        return std::nullopt;
    }
    return to_utf8(identifier);
}

/// @brief 取 bundle identifier，缺失时落 `X-failed` 并点名「不在 .app 包内」。
[[nodiscard]] auto require_bundle(const AppIdentity &identity, ErrorCode code, std::string_view scope)
    -> Result<std::string> {
    const auto actual = bundle_identifier();
    if (!actual.has_value()) {
        return make_error(code, std::string(scope) +
                                    ": the application is not running inside a .app bundle; macOS registers this "
                                    "through bundle metadata and cannot do it for a bare executable");
    }
    if (!identity.bundle_identifier.empty() && identity.bundle_identifier != *actual) {
        return make_error(code, std::string(scope) + ": AppIdentity.bundle_identifier (" + identity.bundle_identifier +
                                    ") does not match the running bundle (" + *actual + ")");
    }
    return *actual;
}

/// @brief Info.plist 的 `CFBundleURLTypes` 是否声明了该方案。
[[nodiscard]] auto bundle_declares_scheme(NSString *scheme) -> bool {
    NSArray *types = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleURLTypes"];
    for (NSDictionary *type in types) {
        NSArray *schemes = type[@"CFBundleURLSchemes"];
        for (NSString *declared in schemes) {
            if ([declared caseInsensitiveCompare:scheme] == NSOrderedSame) {
                return true;
            }
        }
    }
    return false;
}

/// @brief 扩展名 → UTI（Launch Services）；未知 UTI 返回动态 UTI（仍可用于查询）。
[[nodiscard]] auto uti_for_extension(const std::string &extension) -> std::string {
    NSString *tag = ns_string(extension);
    if (tag == nil) {
        return {};
    }
    CFStringRef uti =
        UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, (__bridge CFStringRef)tag, nullptr);
    if (uti == nullptr) {
        return {};
    }
    std::string result = to_utf8((__bridge NSString *)uti);
    CFRelease(uti);
    return result;
}

/// @brief Info.plist 的 `CFBundleDocumentTypes` 是否声明了该扩展名（扩展名或 UTI 命中）。
[[nodiscard]] auto bundle_declares_extension(const std::string &extension, const std::string &uti) -> bool {
    NSArray *types = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDocumentTypes"];
    for (NSDictionary *type in types) {
        for (NSString *declared in type[@"CFBundleTypeExtensions"]) {
            if (to_utf8(declared) == extension) {
                return true;
            }
        }
        for (NSString *declared in type[@"LSItemContentTypes"]) {
            if (!uti.empty() && to_utf8(declared) == uti) {
                return true;
            }
        }
    }
    return false;
}

// ---------- URL 方案 ----------

auto backend_register_scheme(std::string_view scheme, std::string_view display_name, const AppIdentity &identity)
    -> Result<void> {
    auto bundle = require_bundle(identity, ErrorCode::UrlSchemeFailed, "url-scheme");
    if (!bundle) {
        return bundle.error();
    }
    (void)display_name;  // macOS 的显示名来自包内 CFBundleName，运行期不改写
    NSString *scheme_text = ns_string(std::string(scheme));
    if (scheme_text == nil) {
        return make_error(ErrorCode::GeneralInvalidArgument, "url-scheme: scheme is not valid UTF-8");
    }
    if (!bundle_declares_scheme(scheme_text)) {
        return make_error(ErrorCode::UrlSchemeFailed,
                          "url-scheme: '" + std::string(scheme) +
                              "' is not declared in CFBundleURLTypes of the application's Info.plist; macOS cannot "
                              "register a scheme at runtime without that static declaration");
    }
    const OSStatus status =
        LSSetDefaultHandlerForURLScheme((__bridge CFStringRef)scheme_text, (__bridge CFStringRef)ns_string(*bundle));
    if (status != noErr) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: Launch Services rejected the registration for '" +
                                                          std::string(scheme) + "' (OSStatus " +
                                                          std::to_string(status) + ")");
    }
    return Result<void>{};
}

auto backend_unregister_scheme(std::string_view scheme, const AppIdentity &identity) -> Result<void> {
    const auto owner = [&scheme]() -> Result<std::string> {
        CFStringRef handler = LSCopyDefaultHandlerForURLScheme((__bridge CFStringRef)ns_string(std::string(scheme)));
        if (handler == nullptr) {
            return std::string{};
        }
        std::string value = to_utf8((__bridge NSString *)handler);
        CFRelease(handler);
        return value;
    }();
    if (!owner) {
        return owner.error();
    }
    if (owner.value().empty()) {
        return Result<void>{};  // 归属为空：幂等成功
    }
    const auto bundle = require_bundle(identity, ErrorCode::UrlSchemeFailed, "url-scheme");
    if (!bundle) {
        return bundle.error();
    }
    if (owner.value() != *bundle) {
        return make_error(ErrorCode::UrlSchemeFailed, "url-scheme: '" + std::string(scheme) +
                                                          "' is registered by another application: " + owner.value());
    }
    // Launch Services 没有「按方案撤销」的公开 API：CFBundleURLTypes 是包内静态声明。
    return make_error(ErrorCode::UrlSchemeUnsupported,
                      "url-scheme: macOS has no runtime API to remove a URL scheme handler; remove the scheme from "
                      "CFBundleURLTypes and rebuild the bundle");
}

auto backend_scheme_owner(std::string_view scheme) -> Result<std::string> {
    CFStringRef handler = LSCopyDefaultHandlerForURLScheme((__bridge CFStringRef)ns_string(std::string(scheme)));
    if (handler == nullptr) {
        return std::string{};  // 没有归属：空集合不是错误
    }
    std::string value = to_utf8((__bridge NSString *)handler);
    CFRelease(handler);
    return value;
}

// ---------- 文件关联 ----------

auto backend_query_association(std::string_view extension) -> Result<AssociationInfo> {
    const std::string uti = uti_for_extension(std::string(extension));
    if (uti.empty()) {
        return AssociationInfo{};  // 无可用 UTI：无关联，不是错误
    }
    CFStringRef uti_text = (__bridge CFStringRef)ns_string(uti);
    CFErrorRef error = nullptr;
    CFURLRef application = LSCopyDefaultApplicationURLForContentType(uti_text, kLSRolesAll, &error);
    if (application == nullptr) {
        if (error != nullptr) {
            CFRelease(error);
        }
        return AssociationInfo{};  // 没有默认应用：空集合不是错误
    }
    NSBundle *application_bundle = [NSBundle bundleWithURL:(__bridge NSURL *)application];
    NSString *identifier = [application_bundle bundleIdentifier];
    AssociationInfo info;
    info.application = identifier != nil ? to_utf8(identifier) : to_utf8([(__bridge NSURL *)application path]);
    info.source = "launch-services";
    CFRelease(application);
    return info;
}

auto backend_declare_association(const AssociationDeclaration &declaration, std::string_view mime,
                                 const AppIdentity &identity) -> Result<void> {
    auto bundle = require_bundle(identity, ErrorCode::FileAssociationFailed, "file-association");
    if (!bundle) {
        return bundle.error();
    }
    const std::string uti = uti_for_extension(declaration.extension);
    if (!bundle_declares_extension(declaration.extension, uti)) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: '." + declaration.extension +
                              "' is not declared in CFBundleDocumentTypes of the application's Info.plist; macOS "
                              "cannot declare a document type at runtime without that static declaration");
    }
    (void)mime;  // macOS 的声明口径是 UTI，不是 MIME
    if (LSRegisterURL((__bridge CFURLRef)[[NSBundle mainBundle] bundleURL], true) != noErr) {
        return make_error(ErrorCode::FileAssociationFailed,
                          "file-association: Launch Services rejected the bundle registration");
    }
    return Result<void>{};
}

auto backend_undeclare_association(std::string_view extension, const AppIdentity &identity) -> Result<void> {
    (void)identity;
    return make_error(ErrorCode::FileAssociationFailed,
                      "file-association: macOS document types are declared statically in CFBundleDocumentTypes; "
                      "remove the entry for '." +
                          std::string(extension) + "' and rebuild the bundle");
}

// ---------- 登录项 ----------

/// @brief 沙箱形态判定：沙箱应用的登录项服务不适用。
[[nodiscard]] auto is_sandboxed() -> bool {
    return [[[NSProcessInfo processInfo] environment] objectForKey:@"APP_SANDBOX_CONTAINER_ID"] != nil;
}

[[nodiscard]] auto backend_run_at_login_query(const AppIdentity &identity) -> RunAtLoginState {
    if (!bundle_identifier().has_value()) {
        return RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                               .detail = "run-at-login: the application is not running inside a .app bundle"};
    }
    if (is_sandboxed()) {
        return RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                               .detail = "run-at-login: sandboxed applications cannot register login items"};
    }
    if (@available(macOS 13.0, *)) {
        const SMAppServiceStatus status = [SMAppService mainAppService].status;
        switch (status) {
            case SMAppServiceStatusEnabled:
                return RunAtLoginState{.status = RunAtLoginStatus::Enabled};
            case SMAppServiceStatusRequiresApproval:
                // 已登记但用户尚未在系统设置里放行：OS 此刻不会启动本应用，按「未生效」报。
                return RunAtLoginState{.status = RunAtLoginStatus::Disabled};
            case SMAppServiceStatusNotRegistered:
            case SMAppServiceStatusNotFound:
            default:
                return RunAtLoginState{.status = RunAtLoginStatus::Disabled};
        }
    }
    (void)identity;
    return RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                           .detail = "run-at-login: SMAppService requires macOS 13 or newer"};
}

auto backend_run_at_login_set(bool enabled, const AppIdentity &identity) -> Result<void> {
    auto bundle = require_bundle(identity, ErrorCode::RunAtLoginFailed, "run-at-login");
    if (!bundle) {
        return bundle.error();
    }
    if (is_sandboxed()) {
        return make_error(ErrorCode::RunAtLoginUnsupported,
                          "run-at-login: sandboxed applications cannot register login items");
    }
    if (@available(macOS 13.0, *)) {
        NSError *error = nil;
        SMAppService *service = [SMAppService mainAppService];
        const BOOL ok = enabled ? [service registerAndReturnError:&error] : [service unregisterAndReturnError:&error];
        if (!ok) {
            const std::string detail = error_detail(error);
            return make_error(ErrorCode::RunAtLoginFailed, "run-at-login: the login item service rejected the request" +
                                                               (detail.empty() ? std::string() : ": " + detail));
        }
        return Result<void>{};
    }
    return make_error(ErrorCode::RunAtLoginUnsupported, "run-at-login: SMAppService requires macOS 13 or newer");
}

// ---------- 打印会话 ----------

/// @brief 一页待打印的位图（不透明 RGBA8）。
struct PrintPage {
    Image image;
};

// 打印视图：按页范围排版，把每页位图绘到纸张内容区（ObjC 接口不是 Doxygen 文档对象）。
@interface AuroraPrintView : NSView {
    NSArray *_images;  ///< NSImage 数组（按提交顺序）
    CGFloat _pageWidth;  ///< 纸张宽（点）
    CGFloat _pageHeight;  ///< 纸张高（点）
    CGFloat _insetLeft;  ///< 左边距（点）
    CGFloat _insetTop;  ///< 上边距（点）
    CGFloat _insetRight;  ///< 右边距（点）
    CGFloat _insetBottom;  ///< 下边距（点）
}
- (instancetype)initWithImages:(NSArray *)images
                     pageWidth:(CGFloat)pageWidth
                    pageHeight:(CGFloat)pageHeight
                     insetLeft:(CGFloat)insetLeft
                      insetTop:(CGFloat)insetTop
                    insetRight:(CGFloat)insetRight
                   insetBottom:(CGFloat)insetBottom;
@end

@implementation AuroraPrintView

- (instancetype)initWithImages:(NSArray *)images
                     pageWidth:(CGFloat)pageWidth
                    pageHeight:(CGFloat)pageHeight
                     insetLeft:(CGFloat)insetLeft
                      insetTop:(CGFloat)insetTop
                    insetRight:(CGFloat)insetRight
                   insetBottom:(CGFloat)insetBottom {
    self = [super initWithFrame:NSMakeRect(0, 0, pageWidth, pageHeight * (CGFloat)[images count])];
    if (self != nil) {
        _images = [images copy];
        _pageWidth = pageWidth;
        _pageHeight = pageHeight;
        _insetLeft = insetLeft;
        _insetTop = insetTop;
        _insetRight = insetRight;
        _insetBottom = insetBottom;
    }
    return self;
}

- (void)dealloc {
    [_images release];
    [super dealloc];
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)knowsPageRange:(NSRangePointer)range {
    *range = NSMakeRange(1, (NSUInteger)[_images count]);
    return YES;
}

- (NSRect)rectForPage:(NSInteger)page {
    return NSMakeRect(0, (CGFloat)(page - 1) * _pageHeight, _pageWidth, _pageHeight);
}

- (void)drawRect:(NSRect)dirtyRect {
    const auto page = (NSInteger)std::floor(NSMinY(dirtyRect) / _pageHeight);
    if (page < 0 || page >= (NSInteger)[_images count]) {
        return;
    }
    NSImage *image = [_images objectAtIndex:(NSUInteger)page];
    const NSRect content = NSMakeRect(_insetLeft, (CGFloat)page * _pageHeight + _insetTop,
                                      _pageWidth - _insetLeft - _insetRight, _pageHeight - _insetTop - _insetBottom);
    [image drawInRect:content fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0];
}

@end

/// @brief macOS 打印会话：累计页位图，`finish` 时交给 AppKit 打印操作（不弹系统面板）。
class MacPrintSession final : public desktop_shell::PrintSessionBackend {
  public:
    explicit MacPrintSession(PageSpec spec) : spec_(std::move(spec)) {}

    [[nodiscard]] auto submit_page(const Image &page) -> Result<void> override {
        if (finished_) {
            return make_error(ErrorCode::PrintSessionFailed, "print: session is no longer open");
        }
        if (page.width <= 0 || page.height <= 0 ||
            page.pixels.size() != static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height) * 4U) {
            return make_error(ErrorCode::GeneralInvalidArgument, "print: page image must be RGBA8");
        }
        PrintPage entry;
        entry.image = page;  // 值拷贝：调用方提交后可立即复用缓冲
        pages_.push_back(std::move(entry));
        return Result<void>{};
    }

    [[nodiscard]] auto finish() -> Result<void> override {
        if (finished_) {
            return Result<void>{};
        }
        finished_ = true;
        if (pages_.empty()) {
            return make_error(ErrorCode::PrintSessionFailed, "print: no page was submitted");
        }
        @autoreleasepool {
            NSMutableArray *images = [NSMutableArray arrayWithCapacity:pages_.size()];
            for (const auto &page : pages_) {
                NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                                pixelsWide:page.image.width
                                                                                pixelsHigh:page.image.height
                                                                             bitsPerSample:8
                                                                           samplesPerPixel:4
                                                                                  hasAlpha:YES
                                                                                  isPlanar:NO
                                                                            colorSpaceName:NSDeviceRGBColorSpace
                                                                               bytesPerRow:page.image.width * 4
                                                                              bitsPerPixel:32];
                if (rep == nil) {
                    return make_error(ErrorCode::PrintSessionFailed, "print: cannot allocate a bitmap for the page");
                }
                unsigned char *destination = [rep bitmapData];
                const NSUInteger stride = [rep bytesPerRow];
                for (int row = 0; row < page.image.height; ++row) {
                    std::memcpy(destination + static_cast<std::size_t>(row) * stride,
                                page.image.pixels.data() +
                                    static_cast<std::size_t>(row) * static_cast<std::size_t>(page.image.width) * 4U,
                                static_cast<std::size_t>(page.image.width) * 4U);
                }
                NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(page.image.width, page.image.height)];
                [image addRepresentation:rep];
                [rep release];
                [images addObject:image];
                [image release];
            }
            const CGFloat page_width = static_cast<CGFloat>(spec_.width_mm * AURORA_POINTS_PER_MM);
            const CGFloat page_height = static_cast<CGFloat>(spec_.height_mm * AURORA_POINTS_PER_MM);
            AuroraPrintView *view = [[AuroraPrintView alloc]
                initWithImages:images
                     pageWidth:page_width
                    pageHeight:page_height
                     insetLeft:static_cast<CGFloat>(spec_.margins.left_mm * AURORA_POINTS_PER_MM)
                      insetTop:static_cast<CGFloat>(spec_.margins.top_mm * AURORA_POINTS_PER_MM)
                    insetRight:static_cast<CGFloat>(spec_.margins.right_mm * AURORA_POINTS_PER_MM)
                   insetBottom:static_cast<CGFloat>(spec_.margins.bottom_mm * AURORA_POINTS_PER_MM)];

            NSPrintInfo *info = [[[NSPrintInfo sharedPrintInfo] copy] autorelease];
            [info setPaperSize:NSMakeSize(page_width, page_height)];
            [info setLeftMargin:0.0];
            [info setRightMargin:0.0];
            [info setTopMargin:0.0];
            [info setBottomMargin:0.0];
            [info setHorizontalPagination:NSClipPagination];
            [info setVerticalPagination:NSClipPagination];
            if (!printer_name_.empty()) {
                NSPrinter *printer = [NSPrinter printerWithName:ns_string(printer_name_)];
                if (printer == nil) {
                    [view release];
                    return make_error(ErrorCode::PrintSessionFailed,
                                      "print: printer '" + printer_name_ + "' is no longer available");
                }
                [info setPrinter:printer];
            }
            NSPrintOperation *operation = [NSPrintOperation printOperationWithView:view printInfo:info];
            [operation setShowsPrintPanel:NO];
            [operation setShowsProgressPanel:NO];
            const BOOL ok = [operation runOperation];
            [view release];
            if (!ok) {
                return make_error(ErrorCode::PrintSessionFailed, "print: the print operation failed");
            }
        }
        return Result<void>{};
    }

    auto abort() -> void override {
        pages_.clear();
        finished_ = true;
    }

    auto set_printer(std::string name) -> void { printer_name_ = std::move(name); }

  private:
    PageSpec spec_;  ///< 纸张描述
    std::vector<PrintPage> pages_;  ///< 已提交页（不透明 RGBA8）
    std::string printer_name_;  ///< 目标打印机名（空 = 系统默认）
    bool finished_ = false;  ///< 是否已结束 / 中止
};

[[nodiscard]] auto backend_print_targets() -> Result<std::vector<PrintTarget>> {
    NSArray<NSString *> *names = [NSPrinter printerNames];
    if (names == nil) {
        return std::vector<PrintTarget>{};  // 没有打印机：空集合不是错误
    }
    NSString *default_name = [[[NSPrintInfo sharedPrintInfo] printer] name];
    std::vector<PrintTarget> targets;
    targets.reserve([names count]);
    for (NSString *name in names) {
        PrintTarget target;
        target.name = to_utf8(name);
        target.id = target.name;
        target.is_default = default_name != nil && [name isEqualToString:default_name];
        targets.push_back(std::move(target));
    }
    return targets;
}

auto backend_open_print_session(std::string_view target_id, const PageSpec &spec)
    -> Result<std::unique_ptr<desktop_shell::PrintSessionBackend>> {
    if (!target_id.empty()) {
        NSPrinter *printer = [NSPrinter printerWithName:ns_string(std::string(target_id))];
        if (printer == nil) {
            return make_error(ErrorCode::PrintSessionFailed,
                              "print: printer '" + std::string(target_id) + "' is not available");
        }
    }
    auto session = std::make_unique<MacPrintSession>(spec);
    session->set_printer(std::string(target_id));
    return std::unique_ptr<desktop_shell::PrintSessionBackend>(std::move(session));
}

// ---------- 单实例 ----------

/// @brief 首实例的 URI 入队回调（Apple Event 处理函数取用）。
[[nodiscard]] auto instance_sink() -> std::function<void(std::string)> & {
    static std::function<void(std::string)> sink;
    return sink;
}

/// @brief 已登记的 Apple Event 处理器（非 ARC：手动持有 / 释放）。
id g_url_event_handler = nil;

// `kAEGetURL` 处理器：把 URL 交给首实例的入队回调（ObjC 接口不是 Doxygen 文档对象）。
@interface AuroraUrlEventHandler : NSObject
- (void)handleGetUrlEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)replyEvent;
@end

@implementation AuroraUrlEventHandler

- (void)handleGetUrlEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)replyEvent {
    (void)replyEvent;
    NSString *uri = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    if (uri == nil) {
        return;
    }
    const std::string text = to_utf8(uri);
    if (text.empty()) {
        return;
    }
    if (auto &sink = instance_sink(); sink) {
        sink(text);
    }
}

@end

auto backend_acquire_instance(const AppIdentity &identity, const std::vector<std::string> &uris,
                              const std::function<void(std::string)> &sink) -> Result<SingleInstanceOutcome> {
    (void)identity;
    (void)uris;  // 首实例的命令行 URI 由公共层入队；URL 唤起走 Apple Event
    instance_sink() = sink;
    if (g_url_event_handler == nil) {
        AuroraUrlEventHandler *handler = [[AuroraUrlEventHandler alloc] init];
        [[NSAppleEventManager sharedAppleEventManager] setEventHandler:handler
                                                           andSelector:@selector(handleGetUrlEvent:withReplyEvent:)
                                                         forEventClass:kInternetEventClass
                                                            andEventID:kAEGetURL];
        g_url_event_handler = handler;  // 非 ARC：由本桥持有到 release
    }
    // macOS 的 Launch Services 保证同一 .app 只有一个运行实例，唤起直接投递到本进程。
    return SingleInstanceOutcome::Primary;
}

auto backend_poll_instance() -> void {
    // Apple Event 由主线程 runloop 投递，本函数无事可做。
}

auto backend_release_instance() -> void {
    if (g_url_event_handler != nil) {
        [[NSAppleEventManager sharedAppleEventManager] removeEventHandlerForEventClass:kInternetEventClass
                                                                            andEventID:kAEGetURL];
        [(id)g_url_event_handler release];
        g_url_event_handler = nil;
    }
    instance_sink() = nullptr;
}

}  // namespace

auto macos_shell_integration_ensure() -> bool {
    static bool registered = false;
    if (registered) {
        return true;
    }
    registered = true;
    desktop_shell::register_url_scheme_backend(desktop_shell::UrlSchemeBackend{
        .register_scheme = &backend_register_scheme,
        .unregister_scheme = &backend_unregister_scheme,
        .owner = &backend_scheme_owner,
    });
    desktop_shell::register_file_association_backend(desktop_shell::FileAssociationBackend{
        .query = &backend_query_association,
        .declare = &backend_declare_association,
        .undeclare = &backend_undeclare_association,
    });
    desktop_shell::register_run_at_login_backend(desktop_shell::RunAtLoginBackend{
        .query = &backend_run_at_login_query,
        .set = &backend_run_at_login_set,
    });
    desktop_shell::register_print_backend(desktop_shell::PrintBackend{
        .targets = &backend_print_targets,
        .open = &backend_open_print_session,
    });
    desktop_shell::register_single_instance_backend(desktop_shell::SingleInstanceBackend{
        .acquire = &backend_acquire_instance,
        .poll = &backend_poll_instance,
        .release = &backend_release_instance,
    });
    AURORA_LOG_DEBUG("shell", "macos desktop shell integration bridges registered");
    return true;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
