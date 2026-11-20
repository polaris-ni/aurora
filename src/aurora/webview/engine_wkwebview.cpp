// webview/engine_wkwebview.cpp — macOS WKWebView 后端（系统 WebKit 框架，直链）。
//
// 定位与先例：WebView 引擎的第三腿（Windows WebView2 / Linux WebKitGTK / macOS WKWebView）。
// 与另两腿的「运行期 dlopen / LoadLibrary 逃逸」不同，WebKit 属系统 SDK 框架，恒在且无
// 「构建机缺 dev 包」问题，故取 CoreAudio 直链先例（`-framework WebKit`，见
// cmake/AuroraBackends.cmake）；运行期仍以 `NSClassFromString(@"WKWebView")` 探测一次并
// 缓存，缺类报 MissingRuntime，由 WebView 统一降级为占位矩形。
//
// 内存纪律：**MRC**（无 -fobjc-arc，与 macos_surface.cpp / macos_drag.cpp 同口径）。
// 视图与桥对象在创建时显式持有（alloc/init 的 +1），detach 时按「摘 KVO → 摘消息
// handler（断 UCC 强持有环）→ 摘委托 → 摘 superview → release」定序释放。
//
// 嵌入机制：`create_view` 的 parent_window 语义 = NSWindow*（MacOSSurface::native_handle()，
// 由 WindowHost 经 bind_window 传入）。WKWebView 加为 window.contentView 的子视图；
// 内容视图是 flipped（左上原点，见 AuroraMacSurfaceView.isFlipped），故宿主下发的物理
// 像素矩形只需按 backingScaleFactor 除回 point 即可 setFrame:，无需 Y 翻转。
//
// 快照模式：视图恒隐藏（宿主不授予原生可见），而 WKWebView 隐藏时不产帧——沿用
// WebView2 腿的屏外停放手法：捕获前临时授予可见并停到父客户区之外（窗口边界裁剪保证
// 不上屏），完成 / 超时后先隐藏再回位。在途捕获由引擎 pump_events 心跳按墙钟超时兜底
// 兑现空图（契约：done 必达，失败携空图），竞态门保证不回调两次。
//
// 版本口径：pageZoom 与 allowsContentJavaScript 均 macOS 11+，按 @available 守卫
// （与 SMAppService 的 13+ 守卫同口径），11 以下如实空操作；不取已弃用的
// WKPreferences.javaScriptEnabled。js 开关作用于后续导航（WKWebView 的网页偏好按导航
// 取值，已加载文档不即时重解析）——控件挂载序是「先设 js 再初始导航」，主路径不受影响。

#include "aurora/webview/native_view_host.h"

#ifdef AURORA_ENABLE_WEBVIEW_WKWEBVIEW

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/image.h"

// aurora 头先于平台头（同款纪律：平台头关在单个 TU 内，包含顺序即语义）。
// clang-format off
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
// clang-format on

// ObjC 类**必须**声明在全局作用域（「Objective-C declarations may only appear in global
// scope」——放进 C++ 命名空间即编译错误，与 macos_drag.cpp 同款实测注记），故前置声明
// 落在命名空间之外；定义在文件中部（状态结构与转发函数就绪之后）。
@class AuroraWkWebBridge;

namespace aurora::webview {
namespace {

// 新建视图的占位尺寸（宿主首帧 sync_geometry 即以真实矩形覆盖）。
constexpr int AURORA_WK_INIT_WIDTH = 400;
constexpr int AURORA_WK_INIT_HEIGHT = 300;

/// 捕获回调的墙钟等待上限（对位另两腿同名义常量）：正常捕获是毫秒级的一次合成，
/// 这个量级只为兜住「永不回调」，不是给慢回调设的期限。
constexpr auto AURORA_WK_CAPTURE_TIMEOUT = std::chrono::seconds(15);

/// 捕获期把视图停放到父客户区之外时的横向余量（物理像素），保证右边界也落在客户区左侧。
constexpr int AURORA_WK_PARK_MARGIN = 64;

// 页→Native 消息通道 shim（语义与另两腿一致，通道换成 WKScriptMessageHandler）：
// postMessage 把对象 JSON.stringify 后投出，Native 侧经名为 aurora 的 handler 收 JSON
// 文本；反向 "_receive" 由 Native 经 evaluate 调用，JSON.parse 后转交 window.aurora.onmessage。
constexpr std::string_view AURORA_WK_SHIM =
    "(function(){if(!window.aurora){window.aurora={"
    "postMessage:function(o){if(window.webkit&&window.webkit.messageHandlers&&window.webkit."
    "messageHandlers.aurora)"
    "{window.webkit.messageHandlers.aurora.postMessage(JSON.stringify(o));}},"
    "onmessage:null,"
    "_receive:function(json){var d=json;try{d=JSON.parse(json);}catch(e){}"
    "if(window.aurora.onmessage){try{window.aurora.onmessage(d);}catch(e){}}}"
    "};}})();";

// ---- 纯函数小工具 ----

/// @brief JS 字符串字面量编码：把 JSON 文本安全包进单引号 JS 字符串（页侧 `_receive`
/// 的入参形态）。引号 / 反斜杠 / 控制字符逐一转义，其余字节原样透传（与 WebKitGTK 腿同式）。
/// @param in JSON 文本。
/// @param out 编码结果（追加写入）。
void js_escape(std::string_view in, std::string &out) {
    for (char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\\' || c == '\'' || c == '"') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[8] = {};
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
}

/// @brief 预乘通道反算：存 `round(c * a / 255)`，反算 `round(premul * 255 / a)`，
/// 量化误差 ≤ 1（与 cairo 腿同式）。
/// @param premul 预乘后的通道值。
/// @param alpha 不透明度（> 0）。
/// @return 非预乘通道值（已夹至 255）。
[[nodiscard]] auto unpremul_channel(unsigned premul, unsigned alpha) -> unsigned char {
    const unsigned v = ((premul * 255U) + (alpha / 2U)) / alpha;
    return static_cast<unsigned char>(v > 255U ? 255U : v);
}

/// @brief NSString → UTF-8 std::string（nil 回空串）。
/// @param text Cocoa 字符串（可 nil）。
/// @return UTF-8 副本。
[[nodiscard]] auto ns_to_string(NSString *text) -> std::string {
    if (text == nil) {
        return {};
    }
    const char *utf8 = [text UTF8String];
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

/// @brief std::string_view → NSString（MRC：返回 +1 持有，调用方 release）。
/// `initWithBytes:` 的指针参数 nonnull：空 view 的 `data()` 可能为空，兜底空字面量。
/// @param text UTF-8 文本。
/// @return 新字符串；编码失败为 nil。
[[nodiscard]] auto ns_string(std::string_view text) -> NSString * {
    const char *bytes = text.empty() ? "" : text.data();
    return [[NSString alloc] initWithBytes:bytes length:text.size() encoding:NSUTF8StringEncoding];
}

/// @brief 当前导航 URI（webView.URL 的绝对串；无则空串）。
/// @param web_view 视图（可 nil）。
/// @return URI 文本。
[[nodiscard]] auto current_uri(WKWebView *web_view) -> std::string {
    if (web_view == nil) {
        return {};
    }
    return ns_to_string([web_view.URL absoluteString]);
}

// ---- 共享可变状态 ----

// sink、导航委托 / 消息 handler / KVO 与两类异步 block 都经它访问，覆盖「控件先亡 /
// 宿主先亡 / 异步回调迟到」三种时序：weak_ptr 门管回调判活，detached 管 detach 后静默，
// settled 竞态门管「完成回调与超时兜底谁先到谁兑现」。
struct WkWebViewState {
    std::atomic<bool> detached{false};
    WKWebView *web_view = nullptr;  ///< 本结构的 +1 引用（superview 另持一份；detach 时释放）
    AuroraWkWebBridge *bridge = nullptr;  ///< 本结构的 +1 引用（委托是弱引用，须自持）
    WKWebViewConfiguration *config = nullptr;  ///< 本结构的 +1 引用（js 开关经它作用于后续导航）
    bool title_observed = false;  ///< KVO 已登记（teardown 据此决定是否摘除，未登记摘除会抛）
    bool handler_registered = false;  ///< 消息 handler 已登记（同上）
    std::weak_ptr<WebViewCallbacks> callbacks;  ///< 入向回调（弱引用，判活门）
    std::string last_title;  ///< 标题去重（契约是「变化」才报）
    bool visible = false;  ///< 宿主授予的可见性意图（缺省不可见）
    std::optional<NativeViewRect> last_rect;  ///< 最近一次下发的真实矩形（捕获停放结束后据此回位）
    int capture_primes = 0;  ///< 捕获期临时授予可见的在途计数（>0 时真实几何暂缓下发）
    /// 在途异步求值（detach 时以空串兜底兑现；完成 block 按 id 精确配对）。
    struct PendingEval {
        std::uint64_t id = 0;
        std::shared_ptr<std::atomic<bool>> settled;
        std::function<void(std::string)> done;
    };
    std::vector<PendingEval> pending_eval;
    /// 在途异步快照（引擎心跳按墙钟超时兜底；按 id 配对而非 FIFO——超时会先摘走一条，
    /// FIFO 会让迟到的真回调错位到下一次请求）。
    struct PendingCapture {
        std::uint64_t id = 0;
        std::shared_ptr<std::atomic<bool>> settled;
        std::function<void(Image)> done;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    };
    std::vector<PendingCapture> pending_capture;
};

/// 异步配对 id 发号器（UI 线程单线程取号；求值与快照共用一个序列即可）。
[[nodiscard]] auto next_call_id() -> std::uint64_t {
    static std::uint64_t counter = 0;
    return ++counter;
}

// ---- 入向回调转发（弱引用判活，控件已析构即静默丢弃） ----

void fire_navigation(WkWebViewState &st, std::string uri, WebViewNavigationState state) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_navigation) {
            cb->on_navigation(std::move(uri), state);
        }
    }
}

void fire_title(WkWebViewState &st, std::string title) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_title_changed) {
            cb->on_title_changed(std::move(title));
        }
    }
}

void fire_web_message(WkWebViewState &st, std::string json) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_web_message) {
            cb->on_web_message(std::move(json));
        }
    }
}

void fire_process_failed(WkWebViewState &st, std::string detail) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_process_failed) {
            cb->on_process_failed(std::move(detail));
        }
    }
}

/// @brief 标题变化上报（去重后）。
/// @param st 视图状态。
/// @param title 当前标题（可能为空串）。
void report_title(WkWebViewState &st, const std::string &title) {
    if (title == st.last_title) {
        return;  // 契约是「变化」才报；KVO 在同值重设时也可能触发
    }
    st.last_title = title;
    fire_title(st, title);
}

// ---- 几何与快照停放 ----

/// @brief 物理像素 → point 的换算比例（窗口 backingScaleFactor；无窗口时取主屏，再兜底 1）。
/// @param st 视图状态。
/// @return 换算比例（恒 > 0）。
[[nodiscard]] auto backing_scale(const WkWebViewState &st) -> CGFloat {
    if (st.web_view != nil && st.web_view.window != nil) {
        const CGFloat s = st.web_view.window.backingScaleFactor;
        if (s > 0.0) {
            return s;
        }
    }
    NSScreen *screen = [NSScreen mainScreen];
    if (screen != nil && screen.backingScaleFactor > 0.0) {
        return screen.backingScaleFactor;
    }
    return 1.0;
}

/// @brief 下发物理矩形：除回 point 后 setFrame（superview flipped ⇒ 左上原点直用）。
/// @param st 视图状态。
/// @param rect 宿主物理像素矩形。
void apply_rect(WkWebViewState &st, const NativeViewRect &rect) {
    if (st.web_view == nil) {
        return;
    }
    const CGFloat s = backing_scale(st);
    [st.web_view setFrame:NSMakeRect(static_cast<CGFloat>(rect.x) / s, static_cast<CGFloat>(rect.y) / s,
                                     static_cast<CGFloat>(rect.width) / s, static_cast<CGFloat>(rect.height) / s)];
}

/// @brief 捕获期屏外停放矩形：尺寸不变，左边界整体挪到父客户区之外（对位 WebView2 腿）。
/// @param rect 真实矩形。
/// @return 停放矩形。
[[nodiscard]] auto parked_rect(const NativeViewRect &rect) -> NativeViewRect {
    return NativeViewRect{
        .x = -rect.width - AURORA_WK_PARK_MARGIN, .y = rect.y, .width = rect.width, .height = rect.height};
}

/// @brief 捕获前临时授予可见性并停放到屏外；无可停放几何时返回 false（维持原捕获路径）。
/// @param st 视图状态。
/// @return 本次捕获是否进入了停放态。
auto prime_capture(WkWebViewState &st) -> bool {
    if (st.visible || !st.last_rect || !st.last_rect->has_area() || st.web_view == nil) {
        return false;
    }
    ++st.capture_primes;
    apply_rect(st, parked_rect(*st.last_rect));
    [st.web_view setHidden:NO];
    return true;
}

/// @brief 还原捕获期的临时可见性 / 停放几何（最后一次在途捕获结束时调用）。
/// @param st 视图状态。
void finish_capture_prime(WkWebViewState &st) {
    if (st.capture_primes <= 0) {
        return;
    }
    if (--st.capture_primes > 0 || st.web_view == nil) {
        return;  // 仍有在途捕获需要保持停放
    }
    // 先隐藏再回位：反过来会在真实位置闪现一帧。
    [st.web_view setHidden:st.visible ? NO : YES];
    if (st.last_rect) {
        apply_rect(st, *st.last_rect);
    }
}

// ---- 快照像素折算 ----

/// @brief NSImage → RGBA8 非预乘位图（Image 契约）。经 CGBitmapContext 以
/// premultipliedLast + byteOrder32Big 落字节（内存序即 R,G,B,A），再逐像素反预乘；
/// 上下文先翻转 Y（CGBitmapContext 原点在左下，Image 行序自上而下）。
/// @param image 快照产物（可 nil）。
/// @return 转换结果；nil / 空尺寸 / 建上下文失败时返回空图。
[[nodiscard]] auto nsimage_to_image(NSImage *image) -> Image {
    Image img;
    if (image == nil) {
        return img;
    }
    NSRect proposed = NSMakeRect(0.0, 0.0, image.size.width, image.size.height);
    CGImageRef cg = [image CGImageForProposedRect:&proposed context:nil hints:nil];
    if (cg == nullptr) {
        return img;
    }
    const std::size_t w = CGImageGetWidth(cg);
    const std::size_t h = CGImageGetHeight(cg);
    if (w == 0 || h == 0) {
        return img;
    }
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.pixels.assign(w * h * 4U, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx =
        CGBitmapContextCreate(img.pixels.data(), w, h, 8, w * 4U, cs,
                              static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (ctx == nullptr) {
        img.pixels.clear();
        img.width = 0;
        img.height = 0;
        return img;
    }
    CGContextTranslateCTM(ctx, 0.0, static_cast<CGFloat>(h));
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0.0, 0.0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), cg);
    CGContextRelease(ctx);
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4U) {
        const unsigned a = img.pixels[i + 3];
        if (a == 0U) {
            img.pixels[i + 0] = 0;
            img.pixels[i + 1] = 0;
            img.pixels[i + 2] = 0;
            continue;
        }
        img.pixels[i + 0] = unpremul_channel(img.pixels[i + 0], a);
        img.pixels[i + 1] = unpremul_channel(img.pixels[i + 1], a);
        img.pixels[i + 2] = unpremul_channel(img.pixels[i + 2], a);
    }
    return img;
}

/// @brief evaluate 的 id 结果 → JSON 文本（契约：失败回空串）。nil / NSNull → "null"
/// （undefined 的 JSON 编码）；NSString / NSNumber 经 fragments 序列化（布尔得 true/false，
/// 与另两腿的求值回值口径一致）；字典 / 数组经标准序列化；其余类型（DOM 节点等）回空串。
/// @param value 求值产物（可 nil）。
/// @param error 求值错误（可 nil）。
/// @return JSON 文本；失败为空串。
[[nodiscard]] auto eval_result_json(id value, NSError *error) -> std::string {
    if (error != nil) {
        return {};
    }
    if (value == nil || value == [NSNull null]) {
        return "null";
    }
    const bool fragment =
        ([value isKindOfClass:[NSString class]] == YES) || ([value isKindOfClass:[NSNumber class]] == YES);
    if (fragment || [value isKindOfClass:[NSDictionary class]] == YES || [value isKindOfClass:[NSArray class]] == YES) {
        NSError *json_error = nil;
        NSData *data = [NSJSONSerialization dataWithJSONObject:value
                                                       options:fragment ? NSJSONWritingFragmentsAllowed : 0
                                                         error:&json_error];
        if (data != nil && json_error == nil) {
            return {static_cast<const char *>(data.bytes), data.length};
        }
    }
    return {};
}

// ---- ObjC 桥对象（导航委托 + 消息 handler + 标题 KVO；方法体只做「还原状态后转发」）----

}  // namespace
}  // namespace aurora::webview

// ObjC 类定义必须在全局作用域（见文件头前置声明处的注记），故此处先合上两层命名空间；
// 方法体以限定名引用匿名命名空间内的状态与转发函数（经 enclosing namespace 的隐式
// using directive，限定查找可达，与 macos_drag.cpp 的跨作用域引用同口径）。
// 注记为 `//` 而非 `///`：门禁的文档注释扫描不认 ObjC 的 `@interface` 为可文档声明
// （与 macos_drag.cpp 同口径）。C++ ivar（shared_ptr）由编译器合成的 .cxx_destruct 在
// 对象销毁时自动析构，MRC 下无需手工清理。ivar 与形参命名按本仓 lower_case 口径
// （对位 macos_ax_bridge.cpp 的 ivar 形态；选择器部分的驼峰是 API 定名，不可改）。
@interface AuroraWkWebBridge : NSObject <WKNavigationDelegate, WKScriptMessageHandler> {
    std::shared_ptr<aurora::webview::WkWebViewState> state;
}
- (instancetype)initWithState:(std::shared_ptr<aurora::webview::WkWebViewState>)initial_state;
@end

@implementation AuroraWkWebBridge

- (instancetype)initWithState:(std::shared_ptr<aurora::webview::WkWebViewState>)initial_state {
    self = [super init];
    if (self != nil) {
        state = std::move(initial_state);
    }
    return self;
}

- (void)dealloc {
    [super dealloc];
}

// 导航发起（Loading 态）：didFinish / didFail 只报终态，起手态须由本委托补上。
- (void)webView:(WKWebView *)web_view didStartProvisionalNavigation:(WKNavigation *)navigation {
    (void)navigation;
    if (state->detached) {
        return;
    }
    aurora::webview::fire_navigation(*state, aurora::webview::current_uri(web_view),
                                     aurora::webview::WebViewNavigationState::Loading);
}

- (void)webView:(WKWebView *)web_view didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    if (state->detached) {
        return;
    }
    aurora::webview::fire_navigation(*state, aurora::webview::current_uri(web_view),
                                     aurora::webview::WebViewNavigationState::Finished);
}

// 临时性失败（DNS / 连接层，文档尚未提交）与提交后失败都归 Failed 终态。
- (void)webView:(WKWebView *)web_view
    didFailProvisionalNavigation:(WKNavigation *)navigation
                       withError:(NSError *)error {
    (void)navigation;
    (void)error;
    if (state->detached) {
        return;
    }
    aurora::webview::fire_navigation(*state, aurora::webview::current_uri(web_view),
                                     aurora::webview::WebViewNavigationState::Failed);
}

- (void)webView:(WKWebView *)web_view didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)navigation;
    (void)error;
    if (state->detached) {
        return;
    }
    aurora::webview::fire_navigation(*state, aurora::webview::current_uri(web_view),
                                     aurora::webview::WebViewNavigationState::Failed);
}

// Web 内容进程终止（崩溃 / jetsam）：页面真正不可用，如实上报。
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web_view {
    (void)web_view;
    if (state->detached) {
        return;
    }
    aurora::webview::fire_process_failed(*state, "wkwebview web content process terminated");
}

// 页→Native 消息：shim 投来的是 JSON 文本（NSString）；页面绕过 shim 直投对象时
// 经 NSJSONSerialization 折算，折算不了即丢弃（不抛、不谎报）。
- (void)userContentController:(WKUserContentController *)user_content_controller
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)user_content_controller;
    if (state->detached) {
        return;
    }
    id body = message.body;
    if ([body isKindOfClass:[NSString class]] == YES) {
        aurora::webview::fire_web_message(*state, aurora::webview::ns_to_string(static_cast<NSString *>(body)));
        return;
    }
    if ([body isKindOfClass:[NSDictionary class]] == YES || [body isKindOfClass:[NSArray class]] == YES) {
        NSData *data = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
        if (data != nil) {
            aurora::webview::fire_web_message(*state, std::string(static_cast<const char *>(data.bytes), data.length));
        }
    }
}

// 标题 KVO：title 键路径变化即读回（去重后上报）。
- (void)observeValueForKeyPath:(NSString *)key_path
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    (void)object;
    (void)change;
    (void)context;
    if (state->detached) {
        return;
    }
    if ([key_path isEqualToString:@"title"] == YES) {
        aurora::webview::report_title(*state, aurora::webview::ns_to_string(state->web_view.title));
    }
}

@end

namespace aurora::webview {
namespace {

// ---- 视图拆毁（定版 teardown）----
// detach（宿主摘除登记条目）与 create_view 失败回退共用同一序列：先置 detached 门，
// 再按「摘 KVO → 摘消息 handler → 摘委托 → 摘 superview → release」释放，最后把异步
// 在途项以空结果兜底（契约：done 必达）。
void teardown_view(const std::shared_ptr<WkWebViewState> &st) {
    if (st->detached.exchange(true)) {
        return;  // 幂等
    }
    if (st->web_view != nil) {
        if (st->title_observed) {
            [st->web_view removeObserver:st->bridge forKeyPath:@"title"];
            st->title_observed = false;
        }
        if (st->handler_registered) {
            // UCC 强持有 handler：不摘即「桥 → 状态 → 桥」之外再加一条活环，视图永不析构。
            [st->web_view.configuration.userContentController removeScriptMessageHandlerForName:@"aurora"];
            st->handler_registered = false;
        }
        st->web_view.navigationDelegate = nil;
        [st->web_view removeFromSuperview];
        [st->web_view release];
        st->web_view = nil;
    }
    if (st->bridge != nil) {
        [st->bridge release];
        st->bridge = nil;
    }
    if (st->config != nil) {
        [st->config release];
        st->config = nil;
    }
    for (WkWebViewState::PendingCapture &pc : st->pending_capture) {
        if (pc.settled && !pc.settled->exchange(true) && pc.done) {
            pc.done(Image{});
        }
    }
    st->pending_capture.clear();
    for (WkWebViewState::PendingEval &pe : st->pending_eval) {
        if (pe.settled && !pe.settled->exchange(true) && pe.done) {
            pe.done(std::string{});
        }
    }
    st->pending_eval.clear();
}

// ---- 异步求值 ----

/// @brief 立即执行脚本并登记在途项（完成 block 按 id 配对；detach 时兜底空串）。
/// @param st 视图状态。
/// @param js 脚本源码。
/// @param done 结果回调（可空）。
void run_eval_now(const std::shared_ptr<WkWebViewState> &st, const std::string &js,
                  std::function<void(std::string)> done) {
    if (st->web_view == nil) {
        if (done) {
            done(std::string{});
        }
        return;
    }
    const std::uint64_t call_id = next_call_id();
    auto settled = std::make_shared<std::atomic<bool>>(false);
    if (done != nullptr) {
        st->pending_eval.push_back(
            WkWebViewState::PendingEval{.id = call_id, .settled = settled, .done = std::move(done)});
    }
    NSString *source = aurora::webview::ns_string(js);
    [st->web_view evaluateJavaScript:source
                   completionHandler:^(id result, NSError *error) {
                     if (settled->exchange(true)) {
                         return;  // 已由 detach 兜底兑现：不得再回调一次
                     }
                     std::function<void(std::string)> cb;
                     for (auto it = st->pending_eval.begin(); it != st->pending_eval.end(); ++it) {
                         if (it->id == call_id) {
                             cb = std::move(it->done);
                             st->pending_eval.erase(it);
                             break;
                         }
                     }
                     if (cb) {
                         cb(st->detached ? std::string{} : eval_result_json(result, error));
                     }
                   }];
    [source release];
}

// ---- 控件侧 WebViewSink 实现 ----

class WkWebViewSink final : public WebViewSink {
  public:
    explicit WkWebViewSink(std::shared_ptr<WkWebViewState> state) : state_(std::move(state)) {}
    ~WkWebViewSink() override = default;
    // 控件经 unique_ptr 持有的唯一视图实现，不可拷贝 / 移动。
    WkWebViewSink(const WkWebViewSink &) = delete;
    auto operator=(const WkWebViewSink &) -> WkWebViewSink & = delete;
    WkWebViewSink(WkWebViewSink &&) = delete;
    auto operator=(WkWebViewSink &&) -> WkWebViewSink & = delete;

    auto set_native_rect(const NativeViewRect &rect) -> void override {
        if (state_->detached || state_->web_view == nil) {
            return;
        }
        state_->last_rect = rect;  // 记账：捕获期停放结束后据此回位
        if (state_->capture_primes > 0) {
            return;  // 捕获期视图正停在屏外，真实矩形待捕获结束统一回位
        }
        apply_rect(*state_, rect);
    }

    auto set_native_visible(bool visible) -> void override {
        if (state_->detached || state_->web_view == nil) {
            return;
        }
        state_->visible = visible;
        if (state_->capture_primes > 0) {
            return;  // 捕获期临时可见中；捕获结束按 state_->visible 还原
        }
        [state_->web_view setHidden:visible ? NO : YES];
    }

    auto capture(std::function<void(Image)> done) -> void override {
        if (state_->detached || state_->web_view == nil || done == nullptr) {
            if (done) {
                done(Image{});  // 契约：done 必须被调用，失败携空图
            }
            return;
        }
        const std::uint64_t call_id = next_call_id();
        auto settled = std::make_shared<std::atomic<bool>>(false);
        state_->pending_capture.push_back(
            WkWebViewState::PendingCapture{.id = call_id, .settled = settled, .done = std::move(done)});
        // 快照模式视图恒隐藏：捕获前先临时可见并停到屏外，否则 WKWebView 不产帧、
        // 快照可能空白。完成 / 超时由 finish_capture_prime 还原。
        prime_capture(*state_);
        WKSnapshotConfiguration *config = [[WKSnapshotConfiguration alloc] init];
        // 像素宽取视口物理宽（与另两腿的快照像素口径对齐）；无几何账时按当前 bounds 折算。
        const double pixel_width = state_->last_rect && state_->last_rect->has_area()
                                       ? static_cast<double>(state_->last_rect->width)
                                       : state_->web_view.bounds.size.width * backing_scale(*state_);
        if (pixel_width > 0.0) {
            config.snapshotWidth = [NSNumber numberWithDouble:pixel_width];
        }
        std::shared_ptr<WkWebViewState> st = state_;
        [st->web_view takeSnapshotWithConfiguration:config
                                  completionHandler:^(NSImage *image, NSError *error) {
                                    (void)error;
                                    if (settled->exchange(true)) {
                                        return;  // 已由超时兜底兑现空图：不得再回调一次
                                    }
                                    finish_capture_prime(*st);
                                    std::function<void(Image)> cb;
                                    for (auto it = st->pending_capture.begin(); it != st->pending_capture.end(); ++it) {
                                        if (it->id == call_id) {
                                            cb = std::move(it->done);
                                            st->pending_capture.erase(it);
                                            break;
                                        }
                                    }
                                    if (cb) {
                                        cb(st->detached ? Image{} : nsimage_to_image(image));
                                    }
                                  }];
        [config release];
    }

    auto detach() -> void override { teardown_view(state_); }

    auto navigate(const std::string &uri) -> void override {
        if (state_->detached || state_->web_view == nil) {
            return;
        }
        NSString *text = ns_string(uri);
        NSURL *url = text != nil ? [NSURL URLWithString:text] : nil;
        if (url != nil) {
            if ([url isFileURL] == YES) {
                // file:// 走专用入口并授予所在目录读权限（loadRequest 对本地文件不授读权）。
                // 目录推导可能为 nil（无路径分量的极端形态）：兜底授予文件自身。
                NSURL *read_access = [url URLByDeletingLastPathComponent];
                if (read_access == nil) {
                    read_access = url;
                }
                [state_->web_view loadFileURL:url allowingReadAccessToURL:read_access];
            } else {
                [state_->web_view loadRequest:[NSURLRequest requestWithURL:url]];
            }
        }
        [text release];
    }

    auto navigate_to_string(const std::string &html, const std::string &base_uri) -> void override {
        if (state_->detached || state_->web_view == nil) {
            return;
        }
        NSString *document = ns_string(html);
        NSURL *base = nil;
        if (!base_uri.empty()) {
            NSString *base_text = ns_string(base_uri);
            base = base_text != nil ? [NSURL URLWithString:base_text] : nil;
            [base_text release];
        }
        if (document != nil) {
            [state_->web_view loadHTMLString:document baseURL:base];
        }
        [document release];
    }

    auto reload() -> void override {
        if (!state_->detached && state_->web_view != nil) {
            [state_->web_view reload];
        }
    }

    auto stop() -> void override {
        if (!state_->detached && state_->web_view != nil) {
            [state_->web_view stopLoading];
        }
    }

    auto go_back() -> void override {
        if (!state_->detached && state_->web_view != nil && state_->web_view.canGoBack == YES) {
            [state_->web_view goBack];  // 无历史时空操作（契约）
        }
    }

    auto go_forward() -> void override {
        if (!state_->detached && state_->web_view != nil && state_->web_view.canGoForward == YES) {
            [state_->web_view goForward];
        }
    }

    auto set_zoom_factor(float zoom) -> void override {
        if (state_->detached || state_->web_view == nil) {
            return;
        }
        if (@available(macOS 11.0, *)) {
            state_->web_view.pageZoom = static_cast<CGFloat>(zoom);  // 1.0 = 原始尺寸
        }
        // macOS 11 以下无公开 pageZoom：如实空操作（不引入 private API / 弃用 magnification 折算）。
    }

    auto set_javascript_enabled(bool enabled) -> void override {
        if (state_->detached || state_->config == nil) {
            return;
        }
        if (@available(macOS 11.0, *)) {
            // 网页偏好按导航取值：作用于后续导航（挂载序「先设 js 再初始导航」覆盖主路径）。
            state_->config.defaultWebpagePreferences.allowsContentJavaScript = enabled ? YES : NO;
        }
        // macOS 11 以下无公开的非弃用开关：如实空操作（WKPreferences.javaScriptEnabled 已弃用，不取）。
    }

    auto evaluate_script(const std::string &js, std::function<void(std::string)> done) -> void override {
        if (state_->detached) {
            if (done) {
                done(std::string{});
            }
            return;
        }
        run_eval_now(state_, js, std::move(done));
    }

    auto post_message(const std::string &json) -> void override {
        if (state_->detached || state_->web_view == nil) {
            return;  // 契约：通道未建立时丢弃（不排队、不泄漏）
        }
        // native→page 走 evaluate 调 shim 的 _receive；载荷是 JS 单引号字符串里的
        // JSON 文本（页侧 JSON.parse 后转 onmessage）。
        std::string script = "window.aurora._receive('";
        js_escape(json, script);
        script += "');";
        run_eval_now(state_, script, nullptr);
    }

    auto set_callbacks(std::weak_ptr<WebViewCallbacks> callbacks) -> void override {
        state_->callbacks = std::move(callbacks);
    }

  private:
    std::shared_ptr<WkWebViewState> state_;
};

// ---- 进程级引擎 ----

class WkWebViewEngine final : public NativeViewEngine {
  public:
    WkWebViewEngine() = default;
    ~WkWebViewEngine() override = default;
    // 进程级单例引擎，经 shared_ptr 管理，不可拷贝 / 移动。
    WkWebViewEngine(const WkWebViewEngine &) = delete;
    auto operator=(const WkWebViewEngine &) -> WkWebViewEngine & = delete;
    WkWebViewEngine(WkWebViewEngine &&) = delete;
    auto operator=(WkWebViewEngine &&) -> WkWebViewEngine & = delete;

    [[nodiscard]] auto id() const -> WebViewEngine override { return WebViewEngine::WkWebView; }

    [[nodiscard]] auto probe() const -> WebViewCapabilities override {
        static const WebViewCapabilities CAPS = probe_once();
        return CAPS;
    }

    [[nodiscard]] auto create_view(void *parent_window) -> std::unique_ptr<WebViewSink> override {
        if (parent_window == nullptr) {
            return nullptr;
        }
        // parent 的 macOS 语义 = NSWindow*（MacOSSurface::native_handle()）。
        auto *window = static_cast<NSWindow *>(parent_window);
        if ([window isKindOfClass:[NSWindow class]] == NO || window.contentView == nil) {
            return nullptr;
        }
        auto state = std::make_shared<WkWebViewState>();
        if (!build_view(state, window)) {
            teardown_view(state);
            return nullptr;
        }
        live_states_.push_back(state);  // 引擎心跳据此扫在途快照（超时兜底）
        return std::make_unique<WkWebViewSink>(std::move(state));
    }

    auto pump_events() -> void override {
        // macOS 侧几何 / 可见性同步下发、事件经 OS 消息泵分发（poll_watches 取默认空），
        // 本心跳只担一件事：把超时的在途快照兑现成空图（对位 WebView2 引擎）。
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::weak_ptr<WkWebViewState>> live;
        live.reserve(live_states_.size());
        for (std::weak_ptr<WkWebViewState> &w : live_states_) {
            const std::shared_ptr<WkWebViewState> st = w.lock();
            if (st == nullptr) {
                continue;  // 视图已拆：一并从表里摘掉
            }
            live.push_back(w);
            std::vector<WkWebViewState::PendingCapture> keep;
            keep.reserve(st->pending_capture.size());
            for (WkWebViewState::PendingCapture &pc : st->pending_capture) {
                if (now - pc.started >= AURORA_WK_CAPTURE_TIMEOUT) {
                    if (pc.settled && !pc.settled->exchange(true)) {
                        finish_capture_prime(*st);
                        if (pc.done) {
                            pc.done(Image{});
                        }
                    }
                } else {
                    keep.push_back(std::move(pc));
                }
            }
            st->pending_capture.swap(keep);
        }
        live_states_.swap(live);
    }

  private:
    [[nodiscard]] static auto probe_once() -> WebViewCapabilities {
        // 系统框架直链，运行期只判类存在（WebKit 框架缺失 / 被剪裁的极端形态）。
        if (NSClassFromString(@"WKWebView") == nil) {
            return WebViewCapabilities{.engine = WebViewEngine::WkWebView,
                                       .availability = WebViewAvailability::MissingRuntime};
        }
        return WebViewCapabilities{.interactive = true,
                                   .snapshot = true,
                                   .engine = WebViewEngine::WkWebView,
                                   .availability = WebViewAvailability::Available};
    }

    /// @brief 建视图：配置 + shim + 消息 handler + 委托 + KVO + 挂入内容视图。
    /// 失败即返回 false，已建资源由调用方经 teardown_view 统一释放（按登记标志摘除）。
    /// @param state 视图状态。
    /// @param window 宿主窗口。
    /// @return 建成与否。
    [[nodiscard]] static auto build_view(const std::shared_ptr<WkWebViewState> &state, NSWindow *window) -> bool {
        state->bridge = [[AuroraWkWebBridge alloc] initWithState:state];
        if (state->bridge == nil) {
            return false;
        }
        state->config = [[WKWebViewConfiguration alloc] init];
        if (state->config == nil) {
            return false;
        }
        WKUserContentController *ucc = state->config.userContentController;
        NSString *shim = ns_string(AURORA_WK_SHIM);
        WKUserScript *script = [[WKUserScript alloc] initWithSource:shim
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                   forMainFrameOnly:NO];
        [shim release];
        [ucc addUserScript:script];
        [script release];
        [ucc addScriptMessageHandler:state->bridge name:@"aurora"];
        state->handler_registered = true;

        state->web_view =
            [[WKWebView alloc] initWithFrame:NSMakeRect(0.0, 0.0, AURORA_WK_INIT_WIDTH, AURORA_WK_INIT_HEIGHT)
                               configuration:state->config];
        if (state->web_view == nil) {
            return false;
        }
        state->web_view.navigationDelegate = state->bridge;  // 弱引用，桥由 state 自持
        [state->web_view addObserver:state->bridge
                          forKeyPath:@"title"
                             options:NSKeyValueObservingOptionNew
                             context:nullptr];
        state->title_observed = true;
        // 契约：新建视图缺省不可见（可见性一律由宿主经 set_native_visible 授予）。
        [state->web_view setHidden:YES];
        [window.contentView addSubview:state->web_view];
        return true;
    }

    std::vector<std::weak_ptr<WkWebViewState>> live_states_;  ///< 活着的视图（引擎心跳据此扫在途快照）
};

// 匿名 namespace 到此结束：以上为内部实现细节。`create_wkwebview_engine` 须有外部链接
// （native_view_host.cpp 前向声明并调用它），故在匿名 namespace 之外、仍于
// `namespace aurora::webview` 内定义。
}  // namespace

auto create_wkwebview_engine() -> std::shared_ptr<NativeViewEngine> {
    // 系统框架直链：无句柄可加载，工厂恒给出引擎；可用性由 probe() 的运行期判类裁决。
    return std::make_shared<WkWebViewEngine>();
}

}  // namespace aurora::webview

#endif  // AURORA_ENABLE_WEBVIEW_WKWEBVIEW