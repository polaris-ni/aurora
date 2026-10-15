// webview/engine_webview2.cpp — Windows WebView2 后端（运行期动态加载，零链接依赖）。
//
// 编译期不链 WebView2Loader：环境创建入口点经 `LoadLibraryA("WebView2Loader.dll")` +
// `GetProcAddress` 取得（对齐 win32_ua.cpp 的 UIAutomationCore 桥同口径——核心零链接
// 依赖、缺库即降级）。引擎自报可用性（`probe()`）：缺运行时即 `MissingRuntime`，由
// `WebView` 统一降级为占位矩形，不构建失败、不运行崩溃。
//
// 异步创建：WebView2 的环境与控制器创建都是异步的，故 `create_view` 同步返回 `WebViewSink`
// 而底层 HWND / webview 稍后就绪；就绪前到达的命令进入 `WebView2State::pending_*`
// 排队，就绪后 `flush_pending` 统一下发。`set_native_rect` / `set_native_visible` 同样排队
// ——宿主按「矩形变化才下发、可见性翻转才下发」去重，**不会重发**，故未就绪时静默忽略即
// 永久丢状态（首帧同步早于控制器就绪是常态，不是罕见竞态）。
//
// COM 线程模型：WebView2 要求调用线程处于 STA（`COINIT_APARTMENTTHREADED`）。本库 UI
// 线程此前若未 CoInitialize，由 `create_view` 显式初始化；失败（如已被初始化为 MTA）则
// 返回空视图，控件按「无视图」路径降级。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/webview/native_view_host.h"

#ifdef AURORA_ENABLE_WEBVIEW_WEBVIEW2

#ifndef WIN32_LEAN_AND_MEAN
// Windows SDK 规定的宏名，无法冠 AURORA_ 前缀，只能就地豁免。
// NOLINTNEXTLINE(readability-identifier-naming): SDK 既定宏名，不可改。
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ole2.h>  // `interface` 宏（WebView2.h 依赖）；须在 windows.h 之后
#include <windows.h>
#include <wrl/client.h>
// vendored WebView2.h 含一枚枚举值超出 int 表示范围（`-Wmicrosoft-enum-value`）。
// 三方头按项目惯例不纳入告警面（见 cmake/AuroraThirdParty.cmake 的第三方屏蔽口径），
// 头文件无法借 -isystem 单独屏蔽，故就地收口这一条，仅覆盖本 include。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmicrosoft-enum-value"
#include "webview2/WebView2.h"
#pragma clang diagnostic pop

#include "aurora/core/image.h"

using Microsoft::WRL::ComPtr;

// 入口点签名（与 webview2/WebView2.h 的 STDAPI 声明一致；运行期 GetProcAddress 取得）。
// 形参名按本仓 snake_case 约定书写（typedef 形参名仅作文档，与 SDK 声明无绑定）。
using PfnCreateEnv = HRESULT(STDMETHODCALLTYPE *)(PCWSTR browser_executable_folder, PCWSTR user_data_folder,
                                                  ICoreWebView2EnvironmentOptions *options,
                                                  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);
using PfnGetVersion = HRESULT(STDMETHODCALLTYPE *)(PCWSTR browser_executable_folder, LPWSTR *version_info);

namespace aurora::webview {
namespace {

[[nodiscard]] auto to_wide(std::string_view u8) -> std::wstring {
    if (u8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), w.data(), n);
    return w;
}

[[nodiscard]] auto from_wide(LPCWSTR w) -> std::string {
    if (w == nullptr) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

[[nodiscard]] auto from_wide_free(PWSTR w) -> std::string {
    if (w == nullptr) {
        return {};
    }
    std::string s = from_wide(w);
    CoTaskMemFree(w);
    return s;
}

// 共享可变状态：sink 与若干 COM 事件回调都经它访问，保证任意一方先亡都不悬垂
// （控件析构注销、宿主先亡、webview 异步回调三种时序都覆盖）。
struct WebView2State {
    std::atomic<bool> detached{false};  // detach 后置位：所有回调据此 no-op
    std::atomic<bool> ready{false};  // 环境 + 控制器异步就绪
    HWND parent = nullptr;  // 宿主父窗口（Aurora 窗口 HWND）
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::weak_ptr<WebViewCallbacks> callbacks;  // 入向回调（弱引用，判活门）
    // 就绪前到达的几何 / 可见性：宿主去重后不会重发，必须在此暂存、就绪时补送。
    std::optional<NativeViewRect> pending_rect;
    std::optional<bool> pending_visible;
    // 就绪前到达的命令进入排队，就绪后 flush。
    std::optional<std::string> pending_url;
    std::optional<std::string> pending_html;
    std::optional<float> pending_zoom;
    std::optional<bool> pending_js;
    struct PendingEval {
        std::string js;
        std::function<void(std::string)> done;
    };
    std::vector<PendingEval> pending_eval;
    std::vector<std::string> pending_post;
};

// 入向（引擎 → 控件）回调转发：经弱引用判活，控件已析构即静默丢弃。
void fire_navigation(WebView2State &st, std::string uri, WebViewNavigationState state) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_navigation) {
            cb->on_navigation(std::move(uri), state);
        }
    }
}
void fire_title(WebView2State &st, std::string title) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_title_changed) {
            cb->on_title_changed(std::move(title));
        }
    }
}
void fire_web_message(WebView2State &st, std::string json) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_web_message) {
            cb->on_web_message(std::move(json));
        }
    }
}
void fire_process_failed(WebView2State &st, std::string detail) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_process_failed) {
            cb->on_process_failed(std::move(detail));
        }
    }
}

// 转发声明（实现在文件后部，依赖各 handler 类）。
void on_controller_ready(const std::shared_ptr<WebView2State> &st, ICoreWebView2Controller *controller);
void run_eval_now(const std::shared_ptr<WebView2State> &st, const std::string &js,
                  std::function<void(std::string)> done);

// COM 引用计数统一口径：构造即 ref=0，API 侧 `add_*` / 完成回调据此 AddRef（→1）并在
// 注销 / 关闭 / Invoke 后 Release（→0 即 delete）。这一约定对「创建时 new、交给 API 拥有」
// 的两种回调（一次性完成回调、长期事件回调）都正确，无泄漏无野指针。
template <typename Iface>
class ComHandler : public Iface {
  public:
    explicit ComHandler(std::shared_ptr<WebView2State> state) : state_(std::move(state)) {}
    virtual ~ComHandler() = default;
    // COM 回调对象按引用计数自管理、经 `new` 交付 API 持有，不可拷贝 / 移动。
    ComHandler(const ComHandler &) = delete;
    auto operator=(const ComHandler &) -> ComHandler & = delete;
    ComHandler(ComHandler &&) = delete;
    auto operator=(ComHandler &&) -> ComHandler & = delete;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --ref_;
        if (n == 0) {
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(Iface)) {
            *ppv = static_cast<Iface *>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    /// @brief 同步失败路径的回收（API 未接手本对象时调用）。
    ///
    /// 上面那条口径的另一半：API **只在接手时 AddRef**。若 `ExecuteScript` / `CapturePreview` /
    /// `CreateCoreWebView2Controller` 同步返回失败，则本对象从未交给 API，此时必须直接销毁——
    /// 走 `Release()` 会从 ref=0 自减下溢成极大值，永不自毁且泄漏（且完成回调永不抵达）。
    auto discard() -> void { delete this; }

  protected:
    std::shared_ptr<WebView2State> state_;
    std::atomic<ULONG> ref_{0};
};

// 环境创建完成 → 据此创建控制器。
class EnvCompletedHandler final : public ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
  public:
    explicit EnvCompletedHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT /*error_code*/, ICoreWebView2Environment *env) override;
};

// 控制器创建完成 → 取 webview、挂事件、注 shim、flush 命令。
class ControllerCompletedHandler final : public ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
  public:
    explicit ControllerCompletedHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT /*error_code*/, ICoreWebView2Controller *controller) override;
};

// 导航发起（Loading 态）：`NavigationCompleted` 只报终态，起手态须由本事件补上，
// 否则 `WebViewNavigationState::Loading` 永远不会上报（引擎契约要求三态齐备）。
class NavigationStartingHandler final : public ComHandler<ICoreWebView2NavigationStartingEventHandler> {
  public:
    explicit NavigationStartingHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 * /*sender*/,
                                     ICoreWebView2NavigationStartingEventArgs *args) override {
        if (state_->detached || args == nullptr) {
            return S_OK;
        }
        PWSTR uri = nullptr;
        args->get_Uri(&uri);
        fire_navigation(*state_, from_wide_free(uri), WebViewNavigationState::Loading);
        return S_OK;
    }
};

// 导航完成（Finished / Failed）。
class NavigationCompletedHandler final : public ComHandler<ICoreWebView2NavigationCompletedEventHandler> {
  public:
    explicit NavigationCompletedHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) override {
        if (state_->detached) {
            return S_OK;
        }
        BOOL ok = FALSE;
        if (args != nullptr) {
            args->get_IsSuccess(&ok);
        }
        PWSTR uri = nullptr;
        if (sender != nullptr) {
            sender->get_Source(&uri);
        }
        fire_navigation(*state_, from_wide_free(uri),
                        (ok != FALSE) ? WebViewNavigationState::Finished : WebViewNavigationState::Failed);
        return S_OK;
    }
};

// Web → Native 消息（页面经 window.aurora.postMessage 投出的 JSON）。
class WebMessageHandler final : public ComHandler<ICoreWebView2WebMessageReceivedEventHandler> {
  public:
    explicit WebMessageHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 * /*sender*/,
                                     ICoreWebView2WebMessageReceivedEventArgs *args) override {
        if (state_->detached || args == nullptr) {
            return S_OK;
        }
        PWSTR json = nullptr;
        args->get_WebMessageAsJson(&json);
        fire_web_message(*state_, from_wide_free(json));
        return S_OK;
    }
};

// 文档标题变化。
class TitleChangedHandler final : public ComHandler<ICoreWebView2DocumentTitleChangedEventHandler> {
  public:
    explicit TitleChangedHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, IUnknown * /*args*/) override {
        if (state_->detached || sender == nullptr) {
            return S_OK;
        }
        PWSTR title = nullptr;
        sender->get_DocumentTitle(&title);
        fire_title(*state_, from_wide_free(title));
        return S_OK;
    }
};

// 引擎内容进程异常（浏览器进程 / 渲染进程退出或挂起）。
//
// 只上报**内容进程**的失败：浏览器进程或渲染进程（含 frame 级渲染进程）退出、渲染进程无响应，
// 都会让页面真正不可用。而 utility / GPU / sandbox helper / PPAPI 等辅助进程的退出属 Chromium
// 的**常规恢复路径**（浏览器自行重启它们，页面不受影响），把它们一并当作「引擎进程失败」上报
// 只会是噪声。这一取舍与 Linux 侧对位（WebKitGTK 只报 `web-process-terminated`）。
class ProcessFailedHandler final : public ComHandler<ICoreWebView2ProcessFailedEventHandler> {
  public:
    explicit ProcessFailedHandler(std::shared_ptr<WebView2State> state) : ComHandler(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 * /*sender*/, ICoreWebView2ProcessFailedEventArgs *args) override {
        if (state_->detached || args == nullptr) {
            return S_OK;
        }
        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
        args->get_ProcessFailedKind(&kind);
        const char *detail = nullptr;
        switch (kind) {
            case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
                detail = "webview2 browser process exited";
                break;
            case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
                detail = "webview2 renderer process crashed";
                break;
            case COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED:
                detail = "webview2 frame renderer process crashed";
                break;
            case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
                detail = "webview2 renderer process unresponsive";
                break;
            default:
                break;  // 可恢复的辅助进程退出：不上报
        }
        if (detail != nullptr) {
            fire_process_failed(*state_, detail);
        }
        return S_OK;
    }
};

// 执行脚本完成（一次性）。
//
// ⚠ `result_json` 是**借用指针**，不得 `CoTaskMemFree`：MIDL 把它声明成 `[in] LPCWSTR`
// （输入参数，非 `[out] LPWSTR*`），归属未转移给调用方。实测对其调 `from_wide_free` 会触发
// 堆破坏（cdb：`RtlFreeHeap` ← `from_wide_free` ← `ScriptCompletedHandler::Invoke`）。
// 与 `get_Source` / `get_DocumentTitle` / `get_WebMessageAsJson` 那类真 out 参数恰好相反。
class ScriptCompletedHandler final : public ComHandler<ICoreWebView2ExecuteScriptCompletedHandler> {
  public:
    ScriptCompletedHandler(std::shared_ptr<WebView2State> state, std::function<void(std::string)> done)
        : ComHandler(std::move(state)), done_(std::move(done)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT /*error_code*/, LPCWSTR result_json) override {
        if (done_) {
            done_(from_wide(result_json));  // 只拷贝，不释放（借用）
        }
        return S_OK;
    }

  private:
    std::function<void(std::string)> done_;
};

/// 捕获回调的墙钟等待上限：`CapturePreview` 返回 S_OK 后在合成器不产帧的环境里可能永不
/// 回调，超时即兑现空图，使调用方得以按 `pixels.empty()` 判失败。正常捕获是毫秒级的合成器
/// 截图，故这个量级已远宽于真实耗时——它只为兜住「永不」，不是给慢回调设的期限。
constexpr auto AURORA_WEBVIEW2_CAPTURE_TIMEOUT = std::chrono::seconds(15);

/// 在途异步捕获。
///
/// `NativeViewSink::capture` 的契约是「捕获失败或尚未就绪时 `done` 必须仍被调用且携空图，
/// 调用方不得靠『没收到回调』推断结果」。GPU 进程不可用的会话里（虚拟化 / 沙箱，chromium
/// 日志反复 `GPU process exited unexpectedly` 后 `FATAL ... isn't usable`）`CapturePreview`
/// 会返回 S_OK 却**永不回调**——那已不是「失败」而是「悬着」，快照模式的调用方将永久等待
/// 且收不到任何降级信号。故每次捕获在此登记一份，由 `WebView2Engine::pump_events`（宿主每帧
/// 驱动）按墙钟超时兜底兑现，视图 `detach` 时立即兑现。
struct PendingCapture {
    std::weak_ptr<WebView2State> state;
    /// 竞态门：完成回调与超时兜底谁先到谁兑现，后到者见 `true` 即退（不得回调两次）。
    std::shared_ptr<std::atomic<bool>> settled;
    std::function<void(Image)> done;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

/// 在途捕获表（UI 线程单线程访问；随进程静态析构，晚于仍可能兑现的回调）。
[[nodiscard]] auto pending_captures() -> std::vector<PendingCapture> & {
    static std::vector<PendingCapture> table;
    return table;
}

/// 兑现一枚在途捕获（空图）；已由对方兑现者跳过。
auto settle_pending_capture(PendingCapture &pc) -> void {
    if (pc.settled->exchange(true)) {
        return;  // 完成回调已先到
    }
    if (pc.done) {
        pc.done(Image{});
    }
}

// 快照完成（一次性）：IStream 里的 PNG → RGBA8 经 aurora 的 stb 解码。
class CaptureCompletedHandler final : public ComHandler<ICoreWebView2CapturePreviewCompletedHandler> {
  public:
    CaptureCompletedHandler(std::shared_ptr<WebView2State> state, ComPtr<IStream> stream,
                            std::function<void(Image)> done, std::shared_ptr<std::atomic<bool>> settled)
        : ComHandler(std::move(state)), stream_(std::move(stream)), done_(std::move(done)),
          settled_(std::move(settled)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT /*errorCode*/) override {
        if (settled_->exchange(true)) {
            return S_OK;  // 已由超时兜底兑现空图：不得再回调一次
        }
        Image img;
        if (!state_->detached && stream_ != nullptr) {
            LARGE_INTEGER zero{};
            ULARGE_INTEGER size{};
            if (SUCCEEDED(stream_->Seek(zero, STREAM_SEEK_END, &size)) &&
                SUCCEEDED(stream_->Seek(zero, STREAM_SEEK_SET, nullptr)) && size.QuadPart > 0) {
                std::vector<std::uint8_t> buf(static_cast<std::size_t>(size.QuadPart));
                ULONG got = 0;
                if (SUCCEEDED(stream_->Read(buf.data(), static_cast<ULONG>(buf.size()), &got))) {
                    if (auto r = detail::load_image_stb(buf, "webview-capture"); r) {
                        img = std::move(r.value());
                    }
                }
            }
        }
        if (done_) {
            done_(std::move(img));  // 失败 / 未就绪 → 空图（契约允许）
        }
        return S_OK;
    }

  private:
    ComPtr<IStream> stream_;
    std::function<void(Image)> done_;
    std::shared_ptr<std::atomic<bool>> settled_;
};

void run_eval_now(const std::shared_ptr<WebView2State> &st, const std::string &js,
                  std::function<void(std::string)> done) {
    if (st->webview == nullptr) {
        if (done) {
            done(std::string{});
        }
        return;
    }
    auto *h = new ScriptCompletedHandler(st, std::move(done));
    const HRESULT hr = st->webview->ExecuteScript(to_wide(js).c_str(), h);
    if (FAILED(hr)) {
        // 同步失败：API 未接手，不会 Invoke。契约要求失败也必须回调（空串即「结果失败」的
        // 既有编码），故先手工走一遍 Invoke（携空结果）再销毁。
        h->Invoke(hr, nullptr);
        h->discard();
    }
}

void on_controller_ready(const std::shared_ptr<WebView2State> &st, ICoreWebView2Controller *controller) {
    if (controller == nullptr) {
        return;
    }
    st->controller = controller;
    ComPtr<ICoreWebView2> wv;
    if (FAILED(controller->get_CoreWebView2(&wv)) || wv == nullptr) {
        return;
    }
    st->webview = wv;
    controller->put_ParentWindow(st->parent);
    // 契约：新建视图缺省**不可见**（可见性一律由宿主经 `set_native_visible` 授予，控件侧不得自显）。
    // WebView2 的控制器缺省可见，故在此显式压平；宿主若在就绪前已下发过可见性，下面补送。
    controller->put_IsVisible(FALSE);

    EventRegistrationToken tok{};
    wv->add_NavigationStarting(new NavigationStartingHandler(st), &tok);
    wv->add_NavigationCompleted(new NavigationCompletedHandler(st), &tok);
    wv->add_WebMessageReceived(new WebMessageHandler(st), &tok);
    wv->add_DocumentTitleChanged(new TitleChangedHandler(st), &tok);
    wv->add_ProcessFailed(new ProcessFailedHandler(st), &tok);

    // 注入 shim（与 Linux 侧对称）：出向 `window.aurora.postMessage(obj)`——WebView2 自行把对象
    // 序列化成 JSON，Native 侧 `get_WebMessageAsJson` 读回同一 JSON 文本；入向
    // `window.aurora.onmessage`——Native 的 `PostWebMessageAsJson` 落到 `chrome.webview` 的
    // message 事件上，此处转发给 onmessage（页侧常后置赋值，故调用时才判活）。
    static constexpr std::string_view SHIM_JS = R"JS((function () {
  if (!window.aurora) {
    window.aurora = {
      onmessage: null,
      postMessage: function (o) {
        if (window.chrome && window.chrome.webview) { window.chrome.webview.postMessage(o); }
      }
    };
  }
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', function (e) {
      if (typeof window.aurora.onmessage === 'function') { window.aurora.onmessage(e.data); }
    });
  }
})();)JS";
    wv->AddScriptToExecuteOnDocumentCreated(to_wide(SHIM_JS).c_str(), nullptr);

    st->ready = true;

    // 补送就绪前到达的几何 / 可见性：宿主按「矩形变化才下发、可见性翻转才下发」去重，不会重发，
    // 漏掉即永久停留在缺省态（首帧同步早于控制器就绪是常态）。
    if (st->pending_rect) {
        const NativeViewRect &rect = *st->pending_rect;
        const RECT r{.left = rect.x, .top = rect.y, .right = rect.x + rect.width, .bottom = rect.y + rect.height};
        controller->put_Bounds(r);
        st->pending_rect.reset();
    }
    if (st->pending_visible) {
        controller->put_IsVisible(*st->pending_visible ? TRUE : FALSE);
        st->pending_visible.reset();
    }

    if (st->pending_zoom) {
        controller->put_ZoomFactor(static_cast<double>(*st->pending_zoom));
        st->pending_zoom.reset();
    }
    if (st->pending_js) {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(wv->get_Settings(&settings))) {
            settings->put_IsScriptEnabled(*st->pending_js ? TRUE : FALSE);
        }
        st->pending_js.reset();
    }
    if (st->pending_url) {
        wv->Navigate(to_wide(*st->pending_url).c_str());
        st->pending_url.reset();
    } else if (st->pending_html) {
        wv->NavigateToString(to_wide(*st->pending_html).c_str());
        st->pending_html.reset();
    }
    for (WebView2State::PendingEval &e : st->pending_eval) {
        run_eval_now(st, e.js, std::move(e.done));  // js 按 const 引用读，done 移入处理器
    }
    st->pending_eval.clear();
    for (std::string &p : st->pending_post) {
        wv->PostWebMessageAsJson(to_wide(p).c_str());
    }
    st->pending_post.clear();
}

HRESULT EnvCompletedHandler::Invoke(HRESULT /*error_code*/, ICoreWebView2Environment *env) {
    if (state_->detached || env == nullptr) {
        return S_OK;
    }
    state_->env = env;
    auto *h = new ControllerCompletedHandler(state_);
    const HRESULT hr = env->CreateCoreWebView2Controller(state_->parent, h);
    if (FAILED(hr)) {
        h->discard();  // 同步失败：API 未接手（本路无下游回调，直接销毁即可）
    }
    return S_OK;
}

HRESULT ControllerCompletedHandler::Invoke(HRESULT /*error_code*/, ICoreWebView2Controller *controller) {
    on_controller_ready(state_, controller);
    return S_OK;
}

[[nodiscard]] auto make_user_data_folder() -> std::wstring {
    wchar_t tmp[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return {};
    }
    return std::wstring(tmp) + L"aurora-webview";
}

// 控件侧的 WebViewSink 实现：几何 / 可见性与内容命令在控制器就绪前一律排队（就绪时补送）。
class WebView2Sink final : public WebViewSink {
  public:
    explicit WebView2Sink(std::shared_ptr<WebView2State> state) : state_(std::move(state)) {}
    ~WebView2Sink() override = default;
    // 控件经 unique_ptr 持有的唯一视图实现，不可拷贝 / 移动。
    WebView2Sink(const WebView2Sink &) = delete;
    auto operator=(const WebView2Sink &) -> WebView2Sink & = delete;
    WebView2Sink(WebView2Sink &&) = delete;
    auto operator=(WebView2Sink &&) -> WebView2Sink & = delete;

    auto set_native_rect(const NativeViewRect &rect) -> void override {
        if (state_->detached) {
            return;
        }
        if (state_->controller == nullptr) {
            state_->pending_rect = rect;  // 宿主按「变化才下发」去重，不会重发 ⇒ 必须暂存
            return;
        }
        const RECT r{.left = rect.x, .top = rect.y, .right = rect.x + rect.width, .bottom = rect.y + rect.height};
        state_->controller->put_Bounds(r);
    }
    auto set_native_visible(bool visible) -> void override {
        if (state_->detached) {
            return;
        }
        if (state_->controller == nullptr) {
            state_->pending_visible = visible;  // 同上（可见性按翻转边沿下发，更不会重发）
            return;
        }
        state_->controller->put_IsVisible(visible ? TRUE : FALSE);
    }
    auto capture(std::function<void(Image)> done) -> void override {
        if (state_->detached || done == nullptr) {
            if (done) {
                done(Image{});
            }
            return;
        }
        if (state_->controller == nullptr) {
            done(Image{});
            return;
        }
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, 0);
        if (mem == nullptr) {
            done(Image{});
            return;
        }
        ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
            done(Image{});
            return;
        }
        auto settled = std::make_shared<std::atomic<bool>>(false);
        auto *h = new CaptureCompletedHandler(state_, stream, done, settled);
        const HRESULT hr =
            state_->webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(), h);
        if (FAILED(hr)) {
            // 同步失败：API 未接手，不会 Invoke。契约要求失败也必须回调空图（调用方据
            // `pixels.empty()` 判失败，不得靠「没收到回调」推断，见 NativeViewSink::capture），
            // 故先手工走一遍 Invoke（空流 ⇒ 空图）再销毁。
            h->Invoke(hr);
            h->discard();
            return;
        }
        // 异步路径：登记在途，由引擎心跳按墙钟超时兜底（合成器不产帧时本回调可能永不到）。
        pending_captures().push_back(
            PendingCapture{.state = state_, .settled = std::move(settled), .done = std::move(done)});
    }
    auto detach() -> void override {
        state_->detached = true;
        state_->ready = false;
        // 本视图的在途捕获就地兑现（契约：不得让调用方的回调悬着）；其余视图的留着。
        std::vector<PendingCapture> &table = pending_captures();
        std::vector<PendingCapture> rest;
        rest.reserve(table.size());
        for (PendingCapture &pc : table) {
            if (pc.state.lock().get() == state_.get()) {
                settle_pending_capture(pc);
            } else {
                rest.push_back(std::move(pc));
            }
        }
        table.swap(rest);
        if (state_->controller != nullptr) {
            state_->controller->Close();
        }
    }

    auto navigate(const std::string &uri) -> void override {
        if (state_->detached) {
            return;
        }
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->Navigate(to_wide(uri).c_str());
        } else {
            state_->pending_url = uri;
            state_->pending_html.reset();
        }
    }
    auto navigate_to_string(const std::string &html, const std::string &base_uri) -> void override {
        (void)base_uri;  // WebView2 的 NavigateToString 不接受 base；base 仅影响相对资源解析
        if (state_->detached) {
            return;
        }
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->NavigateToString(to_wide(html).c_str());
        } else {
            state_->pending_html = html;
            state_->pending_url.reset();
        }
    }
    auto reload() -> void override {
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->Reload();
        }
    }
    auto stop() -> void override {
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->Stop();
        }
    }
    auto go_back() -> void override {
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->GoBack();
        }
    }
    auto go_forward() -> void override {
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->GoForward();
        }
    }
    auto set_zoom_factor(float zoom) -> void override {
        if (state_->ready && state_->controller != nullptr) {
            state_->controller->put_ZoomFactor(static_cast<double>(zoom));
        } else {
            state_->pending_zoom = zoom;
        }
    }
    auto set_javascript_enabled(bool enabled) -> void override {
        if (state_->ready && state_->webview != nullptr) {
            ComPtr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(state_->webview->get_Settings(&settings))) {
                settings->put_IsScriptEnabled(enabled ? TRUE : FALSE);
            }
        } else {
            state_->pending_js = enabled;
        }
    }
    auto evaluate_script(const std::string &js, std::function<void(std::string)> done) -> void override {
        if (state_->detached) {
            if (done) {
                done(std::string{});
            }
            return;
        }
        if (state_->ready && state_->webview != nullptr) {
            run_eval_now(state_, js, std::move(done));
        } else {
            state_->pending_eval.push_back(WebView2State::PendingEval{.js = js, .done = std::move(done)});
        }
    }
    auto post_message(const std::string &json) -> void override {
        if (state_->detached) {
            return;
        }
        if (state_->ready && state_->webview != nullptr) {
            state_->webview->PostWebMessageAsJson(to_wide(json).c_str());
        } else {
            state_->pending_post.push_back(json);
        }
    }
    auto set_callbacks(std::weak_ptr<WebViewCallbacks> callbacks) -> void override {
        state_->callbacks = std::move(callbacks);
    }

  private:
    std::shared_ptr<WebView2State> state_;
};

// 进程级引擎：持 WebView2Loader 句柄与入口点；`probe()` 只探测一次并缓存。
class WebView2Engine final : public NativeViewEngine {
  public:
    WebView2Engine(HMODULE loader, PfnCreateEnv create_env) : loader_(loader), create_env_(create_env) {}
    ~WebView2Engine() override {
        if (loader_ != nullptr) {
            FreeLibrary(loader_);
        }
    }
    // 进程级单例引擎，经 shared_ptr 管理，不可拷贝 / 移动。
    WebView2Engine(const WebView2Engine &) = delete;
    auto operator=(const WebView2Engine &) -> WebView2Engine & = delete;
    WebView2Engine(WebView2Engine &&) = delete;
    auto operator=(WebView2Engine &&) -> WebView2Engine & = delete;
    [[nodiscard]] auto id() const -> WebViewEngine override { return WebViewEngine::WebView2; }
    [[nodiscard]] auto probe() const -> WebViewCapabilities override {
        static const WebViewCapabilities CAPS = probe_once();
        return CAPS;
    }
    [[nodiscard]] auto create_view(void *parent_window) -> std::unique_ptr<WebViewSink> override {
        if (parent_window == nullptr || create_env_ == nullptr) {
            return nullptr;
        }
        // WebView2 要求调用线程处于 STA。
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(co) && co != S_FALSE) {
            return nullptr;  // RPC_E_CHANGED_MODE 等：本线程不可用，控件按「无视图」降级
        }
        auto state = std::make_shared<WebView2State>();
        state->parent = static_cast<HWND>(parent_window);
        const std::wstring udf = make_user_data_folder();
        auto *h = new EnvCompletedHandler(state);
        const HRESULT hr = create_env_(nullptr, udf.empty() ? nullptr : udf.c_str(), nullptr, h);
        if (FAILED(hr)) {
            h->Release();
            return nullptr;
        }
        return std::make_unique<WebView2Sink>(state);
    }
    auto pump_events() -> void override {
        // Windows 侧几何 / 可见性是同步下发、事件经宿主消息泵分发，本心跳只担一件事：把
        // 超时的在途捕获兑现成空图。`CapturePreview` 在合成器不产帧时返回 S_OK 却永不回调，
        // 没有这一跳，调用方就只能靠「没收到回调」猜结果——正是契约禁止的（见
        // `NativeViewSink::capture`）。
        std::vector<PendingCapture> &table = pending_captures();
        if (table.empty()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::vector<PendingCapture> alive;
        alive.reserve(table.size());
        std::vector<PendingCapture> overdue;
        for (PendingCapture &pc : table) {
            if (now - pc.started >= AURORA_WEBVIEW2_CAPTURE_TIMEOUT) {
                overdue.push_back(std::move(pc));
            } else {
                alive.push_back(std::move(pc));
            }
        }
        table.swap(alive);
        for (PendingCapture &pc : overdue) {
            settle_pending_capture(pc);
        }
    }

  private:
    [[nodiscard]] static auto probe_once() -> WebViewCapabilities {
        HMODULE loader = LoadLibraryA("WebView2Loader.dll");
        if (loader == nullptr) {
            return WebViewCapabilities{.engine = WebViewEngine::WebView2,
                                       .availability = WebViewAvailability::MissingRuntime};
        }
        bool available = false;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 GetProcAddress 返回
        // void*，须还原成入口点函数指针类型（与 audio_wasapi 的 COM 边界同口径）。
        if (const auto get_ver =
                reinterpret_cast<PfnGetVersion>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
            get_ver != nullptr) {
            LPWSTR version = nullptr;
            if (SUCCEEDED(get_ver(nullptr, &version))) {
                available = version != nullptr && *version != L'\0';
                CoTaskMemFree(version);
            }
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        FreeLibrary(loader);
        if (available) {
            return WebViewCapabilities{.interactive = true,
                                       .snapshot = true,
                                       .engine = WebViewEngine::WebView2,
                                       .availability = WebViewAvailability::Available};
        }
        return WebViewCapabilities{.engine = WebViewEngine::WebView2,
                                   .availability = WebViewAvailability::MissingRuntime};
    }
    HMODULE loader_;
    PfnCreateEnv create_env_;
};

// 匿名 namespace 到此结束：以上为内部实现细节。`create_webview2_engine` 须有外部链接
// （native_view_host.cpp 前向声明并调用它），故在匿名 namespace 之外、仍于
// `namespace aurora::webview` 内定义。
}  // namespace

auto create_webview2_engine() -> std::shared_ptr<NativeViewEngine> {
    HMODULE loader = LoadLibraryA("WebView2Loader.dll");
    if (loader == nullptr) {
        return nullptr;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 GetProcAddress 返回 void*，须还原成
    // 环境创建入口点的函数指针类型（零链接依赖的关键一步）。
    const auto create_env =
        reinterpret_cast<PfnCreateEnv>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (create_env == nullptr) {
        FreeLibrary(loader);
        return nullptr;
    }
    // loader 句柄交给 engine 持有（其 dtor FreeLibrary）。
    return std::make_shared<WebView2Engine>(loader, create_env);
}

}  // namespace aurora::webview

#endif  // AURORA_ENABLE_WEBVIEW_WEBVIEW2
