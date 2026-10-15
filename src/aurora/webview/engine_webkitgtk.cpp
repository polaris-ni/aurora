// webview/engine_webkitgtk.cpp — Linux WebKitGTK 后端（GTK3 + WebKitGTK 4.1）。
//
// 定位与先例：本实现是「自声明最小 ABI + dlopen 运行期绑定」路线的第三个实例
// （前有 media/audio_alsa.cpp 绑 libasound、app/notification.cpp 绑 libnotify/dbus、
// window/detail/atspi_bridge.cpp 绑 libdbus）：不引任何 GTK/WebKit 头、不链任何
// 库，全部符号运行期 dlopen/dlsym 取得 ⇒ 仓库无构建期桌面依赖，缺库即降级。
//
// 版本锁定：Ubuntu 26.04 仓库只有 WebKitGTK 4.1（无 6.0），故锁 GTK3 + 4.1。
// 4.1 的 `script-message-received` 载荷是 `WebKitJavascriptResult*`（6.0 改
// `JSCValue*`）——本文件的 ABI 声明对应 4.1，换版须重新核对载荷类型。
//
// 嵌入机制（GTK 收养外部 X 窗口，全部经探针实证）：
//   1. `gdk_x11_window_foreign_new_for_display` 包装宿主 X 窗口得 foreign GdkWindow；
//   2. `gdk_window_new` 在其下建 CHILD container（X 子窗口）；
//   3. `gtk_window_new` + `set_has_window(TRUE)`，realize 回调里把 container 换成
//       widget 的 window（顺带销毁 GTK 自建的默认窗）+ `gtk_widget_register_window`；
//   4. webview 作子 widget 挂入。真实指针/键盘事件经 X 直达 webview 自己的
//      GdkWindow（GTK 连接选的事件掩码），与宿主连接各收各的、互不抢占。
//
// 显隐语义（实测关键点）：GDK 把 X 请求排队、只在主循环迭代时 flush，故显隐必须
// `gdk_window_show/hide` + `gdk_display_flush` 双管齐下（零 Xlib 依赖）。
//
// 快照模式：容器保持**不映射**，页面照常加载渲染（`webkit_web_view_get_snapshot`
// 的 VISIBLE 区域对未映射窗口仍产出像素——引擎侧固有限制：只有可视口有像素）。
//
// teardown 定版（三十轮实证）：`gtk_widget_destroy(win)` 时 GTK 的
// `gtk_widget_real_unrealize` 对 has_window=TRUE 的 widget 会**自行**
// `gdk_window_destroy(priv->window)` 并释放其最终引用 ⇒ 引擎**绝不再** unref /
// destroy container（双重释放 = 时崩时不崩的 UAF）；foreign 包装的引用是引擎自己
// 的，detach 时释放。user content manager 同理。
//
// 事件循环并入：`poll_watches()` 从 GLib 默认主上下文取出待监听 fd（旧式
// `g_main_context_query` 两段调用：先问数量再取数组），`pump_events()` 以非阻塞
// `g_main_context_iteration` 推进——与 AT-SPI 桥的帧循环集成完全同构。
//
// 降级归因（诊断面可分辨）：dlopen/dlsym 任一关键符号缺失 ⇒ 工厂返回空 ⇒ 回落
// Null 桩（`Disabled` → webview-runtime-missing）；非 X11 backend（如 Wayland）
// ⇒ 引擎装上但 `probe()` 报 `UnsupportedPlatform`（交互模式的 GTK 收养在
// Wayland 不成立；快照模式未在 Wayland 实证，故一并不可用）→ webview-unsupported-mode。

#include "aurora/core/platform.h"  // NOLINT

#ifdef AURORA_ENABLE_WEBVIEW_WEBKITGTK

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/webview/native_view_host.h"

// FFI 边界区间豁免：dlsym 只能返回 `void *`（转函数指针必然是 reinterpret_cast），
// GTK/WebKit 对象在本 TU 一律以 `void *` 传递（static_cast 自 `void *` 的用户数据
// 恢复）。逐点抑制不成比例，按区间豁免（与 app/notification.cpp 同口径）。
// NOLINTBEGIN(*-pro-type-*)

namespace aurora::webview {
namespace {

// ---- 自声明 ABI：基础类型（GLib/GObject 标量在 LP64 上的宽度） ----

using gboolean = int;
using gint = int;
using guint = unsigned int;
using gushort = unsigned short;
using gdouble = double;
using gssize = long;  // ptrdiff_t（LP64）
using GType = unsigned long;  // gsize
using GQuark = guint;
using gulong = unsigned long;
using GCallback = void (*)();
using GClosureNotify = void (*)(void *, void *);
// GAsyncReadyCallback：GLib 原型 `void (*)(GObject*, GAsyncResult*, gpointer)`，
// 本 TU 的源/结果参数均以 void* 传递（webkit 的 finish 自行按真实类型解释）。
using GAsyncReadyCallback = void (*)(void *, void *, void *);

// ---- 自声明 ABI：枚举常量（取值取自本机系统头，逐一注明来源） ----
// gdk/gdkwindow.h：GdkWindowType（ROOT=0 TOPLEVEL=1 CHILD=2 …）。
constexpr int AURORA_GDK_WINDOW_CHILD = 2;
// gdk/gdkwindow.h：GdkWindowWindowClass（INPUT_OUTPUT=0 INPUT_ONLY=1）。
constexpr int AURORA_GDK_INPUT_OUTPUT = 0;
// gdk/gdkwindow.h：GdkWindowAttributesType（WA_TITLE=1<<1 WA_X=1<<2 WA_Y=1<<3 …）。
constexpr int AURORA_GDK_WA_X = 1 << 2;
constexpr int AURORA_GDK_WA_Y = 1 << 3;
// gtk/gtkwindow.h：GtkWindowType（TOPLEVEL=0 POPUP=1）。
constexpr int AURORA_GTK_WINDOW_TOPLEVEL = 0;
// webkit2/WebKitWebView.h：WebKitLoadEvent（STARTED=0 REDIRECTED=1 COMMITTED=2 FINISHED=3）。
constexpr int AURORA_WEBKIT_LOAD_STARTED = 0;
constexpr int AURORA_WEBKIT_LOAD_COMMITTED = 2;
constexpr int AURORA_WEBKIT_LOAD_FINISHED = 3;
// webkit2/WebKitWebView.h：WebKitSnapshotRegion（VISIBLE=0 FULL_DOCUMENT=1）。
constexpr int AURORA_WEBKIT_SNAPSHOT_REGION_VISIBLE = 0;
// webkit2/WebKitWebView.h：WebKitSnapshotOptions（NONE=0 …）。
constexpr int AURORA_WEBKIT_SNAPSHOT_OPTIONS_NONE = 0;
// webkit2/WebKitUserContent.h：WebKitUserContentInjectedFrames（ALL_FRAMES=0 TOP_FRAME=1）。
constexpr int AURORA_WEBKIT_INJECT_ALL_FRAMES = 0;
// webkit2/WebKitUserContent.h：WebKitUserScriptInjectionTime（AT_DOCUMENT_START=0 END=1）。
constexpr int AURORA_WEBKIT_INJECT_DOC_START = 0;
// webkit2/WebKitWebView.h：WebKitProcessTerminationReason（CRASHED=0 EXCEEDED_MEMORY_LIMIT=1 BY_API=2）。
constexpr int AURORA_WEBKIT_PROCESS_CRASHED = 0;
constexpr int AURORA_WEBKIT_PROCESS_OOM = 1;
// glib/gpoll.h：GIOChannel 条件位（G_IO_IN=1 … G_IO_OUT=4；与 poll(2) 的
// POLLIN/POLLOUT 同值，GLib 刻意如此设计）。
constexpr int AURORA_G_IO_IN = 1;
constexpr int AURORA_G_IO_OUT = 4;
// cairo.h：cairo_format_t（CAIRO_FORMAT_ARGB32=0 …）。
constexpr int AURORA_CAIRO_FORMAT_ARGB32 = 0;

// 新建视图的占位尺寸（宿主首帧 sync_geometry 即以真实矩形覆盖）。
constexpr int AURORA_WEBKIT_INIT_WIDTH = 400;
constexpr int AURORA_WEBKIT_INIT_HEIGHT = 300;

// 页→Native 消息通道 shim（语义与 WebView2 后端一致，通道换成 GTK 的
// messageHandlers）：postMessage 把对象 JSON.stringify 后投出，Native 侧经 UCM 的
// script-message-received 收 JSON 文本；反 "_receive" 由 Native 经 evaluate 调用，
// JSON.parse 后转交 window.aurora.onmessage。
constexpr std::string_view AURORA_WEBKIT_SHIM =
    "(function(){if(!window.aurora){window.aurora={"
    "postMessage:function(o){if(window.webkit&&window.webkit.messageHandlers&&window.webkit."
    "messageHandlers.aurora)"
    "{window.webkit.messageHandlers.aurora.postMessage(JSON.stringify(o));}},"
    "onmessage:null,"
    "_receive:function(json){var d=json;try{d=JSON.parse(json);}catch(e){}"
    "if(window.aurora.onmessage){try{window.aurora.onmessage(d);}catch(e){}}}"
    "};}})();";

// ---- 自声明 ABI：结构布局（逐字段照抄本机系统头，字段序与类型错一位即 UB） ----

// gdk/gdkwindow.h 的 GdkWindowAttr（GTK 3.24.52）：x/y 是 gint（早期版本为
// gint16）；wclass 在 width/height 之后、window_type 在 visual 之后——顺序特殊。
struct GdkWindowAttr {
    char *title;
    int event_mask;
    int x;
    int y;
    int width;
    int height;
    int wclass;  // GdkWindowWindowClass
    void *visual;
    int window_type;  // GdkWindowType
    void *cursor;
    char *wmclass_name;
    char *wmclass_class;
    int override_redirect;  // gboolean
    int type_hint;  // GdkWindowTypeHint
};

// gdk/gdktypes.h 的 GdkRectangle；gtk/gtkwidget.h 的 GtkAllocation 即其 typedef。
struct GdkRectangle {
    int x;
    int y;
    int width;
    int height;
};

// glib/gpoll.h 的 GPollFD（非 Win32 分支）。
struct GPollFD {
    int fd;
    unsigned short events;
    unsigned short revents;
};

// glib/gerror.h 的 GError。
struct GError {
    unsigned int domain;  // GQuark
    int code;
    char *message;
};

// ---- 自声明 ABI：函数指针类型（按宿主库分组） ----

using FnMainContextIteration = gboolean (*)(void *, gboolean);
using FnMainContextQuery = gint (*)(void *, gint, gint *, GPollFD *, gint);
using FnFree = void (*)(void *);
using FnErrorFree = void (*)(GError *);
using FnSetenv = gboolean (*)(const char *, const char *, gboolean);
using FnGetenv = const char *(*)(const char *);

using FnObjectUnref = void (*)(void *);
using FnTypeCheckInstanceIsA = gboolean (*)(void *, GType);
using FnCClosureNew = void *(*)(GCallback, void *, GClosureNotify);
using FnSignalConnectClosure = gulong (*)(void *, const char *, void *, gboolean);

using FnDisplayGetDefault = void *(*)();
using FnDisplayFlush = void (*)(void *);
using FnWindowNew = void *(*)(void *, const GdkWindowAttr *, int);
using FnWindowMoveResize = void (*)(void *, gint, gint, gint, gint);
using FnWindowShow = void (*)(void *);
using FnWindowHide = void (*)(void *);
using FnWindowDestroy = void (*)(void *);
using FnWindowGetUserData = void (*)(void *, void **);
using FnX11WindowForeignNewForDisplay = void *(*)(void *, unsigned long);
using FnX11DisplayGetType = GType (*)();

using FnInitCheck = gboolean (*)(int *, char ***);
using FnWindowNewGtk = void *(*)(int);  // 形参 GtkWindowType
using FnWidgetSetHasWindow = void (*)(void *, gboolean);
using FnWidgetRealize = void (*)(void *);
using FnWidgetDestroy = void (*)(void *);
using FnWidgetRegisterWindow = void (*)(void *, void *);
using FnWidgetGetWindow = void *(*)(void *);
using FnWidgetSetWindow = void (*)(void *, void *);
using FnWidgetSetHexpand = void (*)(void *, gboolean);
using FnWidgetSetVexpand = void (*)(void *, gboolean);
using FnContainerAdd = void (*)(void *, void *);
using FnWidgetShow = void (*)(void *);
using FnWidgetHide = void (*)(void *);  ///< `gtk_widget_hide`
using FnWidgetSizeAllocate = void (*)(void *, const GdkRectangle *);

using FnWebViewNew = void *(*)();
using FnWebViewNewWithUcm = void *(*)(void *);
using FnWebViewLoadUri = void (*)(void *, const char *);
using FnWebViewLoadHtml = void (*)(void *, const char *, const char *);
using FnWebViewReload = void (*)(void *);
using FnWebViewStopLoading = void (*)(void *);
using FnWebViewGoBack = void (*)(void *);
using FnWebViewGoForward = void (*)(void *);
using FnWebViewCanGoBack = gboolean (*)(void *);
using FnWebViewCanGoForward = gboolean (*)(void *);
using FnWebViewGetUri = char *(*)(void *);
using FnWebViewGetTitle = char *(*)(void *);
using FnWebViewGetSettings = void *(*)(void *);
using FnSettingsSetJavascript = void (*)(void *, gboolean);
using FnWebViewSetZoomLevel = void (*)(void *, gdouble);
using FnWebViewEvaluateJavascript = void (*)(void *, const char *, gssize, const char *, const char *, void *,
                                             GAsyncReadyCallback, void *);
using FnWebViewEvaluateFinish = void *(*)(void *, void *, GError **);
using FnWebViewGetSnapshot = void (*)(void *, int, int, void *, GAsyncReadyCallback, void *);
using FnWebViewGetSnapshotFinish = void *(*)(void *, void *, GError **);
using FnUcmNew = void *(*)();
using FnUcmRegisterHandler = void (*)(void *, const char *);
using FnUcmAddScript = void (*)(void *, void *);
using FnUserScriptNew = void *(*)(const char *, int, int, const char *const *, const char *const *);
using FnUserScriptUnref = void (*)(void *);  ///< `webkit_user_script_unref`
using FnJsResultGetValue = void *(*)(void *);

using FnJscToString = char *(*)(void *);
using FnJscToJson = char *(*)(void *, guint);

using FnSurfaceGetData = unsigned char *(*)(void *);
using FnSurfaceGetWidth = int (*)(void *);
using FnSurfaceGetHeight = int (*)(void *);
using FnSurfaceGetStride = int (*)(void *);
using FnSurfaceGetFormat = int (*)(void *);
using FnSurfaceDestroy = void (*)(void *);

/// @brief 七个库的句柄与全部入口点（resolve 成功即视为不可缺失）。
struct WebKitGtkApi {
    void *gtk = nullptr;  ///< `dlopen("libgtk-3.so.0")`
    void *gdk = nullptr;  ///< `dlopen("libgdk-3.so.0")`
    void *gobject = nullptr;  ///< `dlopen("libgobject-2.0.so.0")`
    void *glib = nullptr;  ///< `dlopen("libglib-2.0.so.0")`
    void *webkit = nullptr;  ///< `dlopen("libwebkit2gtk-4.1.so.0")`
    void *jsc = nullptr;  ///< `dlopen("libjavascriptcoregtk-4.1.so.0")`
    void *cairo = nullptr;  ///< `dlopen("libcairo.so.2")`

    FnMainContextIteration main_context_iteration = nullptr;  ///< `g_main_context_iteration`
    FnMainContextQuery main_context_query = nullptr;  ///< `g_main_context_query`
    FnFree free = nullptr;  ///< `g_free`
    FnErrorFree error_free = nullptr;  ///< `g_error_free`
    FnSetenv setenv = nullptr;  ///< `g_setenv`
    FnGetenv getenv = nullptr;  ///< `g_getenv`

    FnObjectUnref object_unref = nullptr;  ///< `g_object_unref`
    FnTypeCheckInstanceIsA type_check_instance_is_a = nullptr;  ///< `g_type_check_instance_is_a`
    FnCClosureNew cclosure_new = nullptr;  ///< `g_cclosure_new`
    FnSignalConnectClosure signal_connect_closure = nullptr;  ///< `g_signal_connect_closure`

    FnDisplayGetDefault display_get_default = nullptr;  ///< `gdk_display_get_default`
    FnDisplayFlush display_flush = nullptr;  ///< `gdk_display_flush`
    FnWindowNew window_new = nullptr;  ///< `gdk_window_new`
    FnWindowMoveResize window_move_resize = nullptr;  ///< `gdk_window_move_resize`
    FnWindowShow window_show = nullptr;  ///< `gdk_window_show`
    FnWindowHide window_hide = nullptr;  ///< `gdk_window_hide`
    FnWindowDestroy window_destroy = nullptr;  ///< `gdk_window_destroy`
    FnWindowGetUserData window_get_user_data = nullptr;  ///< `gdk_window_get_user_data`
    FnX11WindowForeignNewForDisplay x11_foreign_new = nullptr;  ///< `gdk_x11_window_foreign_new_for_display`
    FnX11DisplayGetType x11_display_get_type = nullptr;  ///< `gdk_x11_display_get_type`

    FnInitCheck init_check = nullptr;  ///< `gtk_init_check`
    FnWindowNewGtk window_new_gtk = nullptr;  ///< `gtk_window_new`
    FnWidgetSetHasWindow set_has_window = nullptr;  ///< `gtk_widget_set_has_window`
    FnWidgetRealize widget_realize = nullptr;  ///< `gtk_widget_realize`
    FnWidgetDestroy widget_destroy = nullptr;  ///< `gtk_widget_destroy`
    FnWidgetRegisterWindow register_window = nullptr;  ///< `gtk_widget_register_window`
    FnWidgetGetWindow widget_get_window = nullptr;  ///< `gtk_widget_get_window`
    FnWidgetSetWindow widget_set_window = nullptr;  ///< `gtk_widget_set_window`
    FnWidgetSetHexpand set_hexpand = nullptr;  ///< `gtk_widget_set_hexpand`
    FnWidgetSetVexpand set_vexpand = nullptr;  ///< `gtk_widget_set_vexpand`
    FnContainerAdd container_add = nullptr;  ///< `gtk_container_add`
    FnWidgetShow widget_show = nullptr;  ///< `gtk_widget_show`
    FnWidgetHide widget_hide = nullptr;  ///< `gtk_widget_hide`
    FnWidgetSizeAllocate size_allocate = nullptr;  ///< `gtk_widget_size_allocate`

    FnWebViewNew web_view_new = nullptr;  ///< `webkit_web_view_new`
    FnWebViewNewWithUcm web_view_new_with_ucm = nullptr;  ///< `webkit_web_view_new_with_user_content_manager`
    FnWebViewLoadUri load_uri = nullptr;  ///< `webkit_web_view_load_uri`
    FnWebViewLoadHtml load_html = nullptr;  ///< `webkit_web_view_load_html`
    FnWebViewReload reload = nullptr;  ///< `webkit_web_view_reload`
    FnWebViewStopLoading stop_loading = nullptr;  ///< `webkit_web_view_stop_loading`
    FnWebViewGoBack go_back = nullptr;  ///< `webkit_web_view_go_back`
    FnWebViewGoForward go_forward = nullptr;  ///< `webkit_web_view_go_forward`
    FnWebViewCanGoBack can_go_back = nullptr;  ///< `webkit_web_view_can_go_back`
    FnWebViewCanGoForward can_go_forward = nullptr;  ///< `webkit_web_view_can_go_forward`
    FnWebViewGetUri get_uri = nullptr;  ///< `webkit_web_view_get_uri`
    FnWebViewGetTitle get_title = nullptr;  ///< `webkit_web_view_get_title`
    FnWebViewGetSettings get_settings = nullptr;  ///< `webkit_web_view_get_settings`
    FnSettingsSetJavascript set_javascript = nullptr;  ///< `webkit_settings_set_enable_javascript`
    FnWebViewSetZoomLevel set_zoom_level = nullptr;  ///< `webkit_web_view_set_zoom_level`
    FnWebViewEvaluateJavascript evaluate_javascript = nullptr;  ///< `webkit_web_view_evaluate_javascript`
    FnWebViewEvaluateFinish evaluate_finish = nullptr;  ///< `webkit_web_view_evaluate_javascript_finish`
    FnWebViewGetSnapshot get_snapshot = nullptr;  ///< `webkit_web_view_get_snapshot`
    FnWebViewGetSnapshotFinish get_snapshot_finish = nullptr;  ///< `webkit_web_view_get_snapshot_finish`
    FnUcmNew ucm_new = nullptr;  ///< `webkit_user_content_manager_new`
    FnUcmRegisterHandler ucm_register_handler = nullptr;  ///< `..._register_script_message_handler`
    FnUcmAddScript ucm_add_script = nullptr;  ///< `webkit_user_content_manager_add_script`
    FnUserScriptNew user_script_new = nullptr;  ///< `webkit_user_script_new`
    FnUserScriptUnref user_script_unref = nullptr;  ///< `webkit_user_script_unref`
    FnJsResultGetValue js_result_get_value = nullptr;  ///< `webkit_javascript_result_get_js_value`

    FnJscToString jsc_to_string = nullptr;  ///< `jsc_value_to_string`
    FnJscToJson jsc_to_json = nullptr;  ///< `jsc_value_to_json`

    FnSurfaceGetData surface_get_data = nullptr;  ///< `cairo_image_surface_get_data`
    FnSurfaceGetWidth surface_get_width = nullptr;  ///< `cairo_image_surface_get_width`
    FnSurfaceGetHeight surface_get_height = nullptr;  ///< `cairo_image_surface_get_height`
    FnSurfaceGetStride surface_get_stride = nullptr;  ///< `cairo_image_surface_get_stride`
    FnSurfaceGetFormat surface_get_format = nullptr;  ///< `cairo_image_surface_get_format`
    FnSurfaceDestroy surface_destroy = nullptr;  ///< `cairo_surface_destroy`
};

/// @brief 把符号解析成函数指针（POSIX dlsym 契约：返回对象指针 `void *`，
/// 目标是函数指针——C++ 定为条件支持，POSIX 明确要求成立）。
/// @tparam Fn 目标函数指针类型。
/// @param handle `dlopen` 返回的库句柄。
/// @param name 符号名（C 名字，未改编）。
/// @return 符号地址；符号不存在时为 nullptr。
template <typename Fn>
[[nodiscard]] auto resolve(void *handle, const char *name) -> Fn {
    void *symbol = dlsym(handle, name);
    Fn fn = nullptr;
    if (symbol != nullptr) {
        // 逐字节 memcpy 落值：避开 reinterpret_cast 红线又满足 ABI（同 notification.cpp）。
        std::memcpy(&fn, &symbol, sizeof(fn));
    }
    return fn;
}

/// @brief 打开七个库并解析全部入口点；任一关键符号缺失即整体失败（降级由调用方接）。
/// @return 完整的 ABI 表；失败为 `std::nullopt`（此时不保留任何句柄）。
[[nodiscard]] auto resolve_api() -> std::optional<WebKitGtkApi> {
    constexpr int kDlFlags = RTLD_LAZY | RTLD_LOCAL;  // 与 audio_alsa 同口径
    WebKitGtkApi api;
    api.gtk = dlopen("libgtk-3.so.0", kDlFlags);
    api.gdk = dlopen("libgdk-3.so.0", kDlFlags);
    api.gobject = dlopen("libgobject-2.0.so.0", kDlFlags);
    api.glib = dlopen("libglib-2.0.so.0", kDlFlags);
    api.webkit = dlopen("libwebkit2gtk-4.1.so.0", kDlFlags);
    api.jsc = dlopen("libjavascriptcoregtk-4.1.so.0", kDlFlags);
    api.cairo = dlopen("libcairo.so.2", kDlFlags);
    const bool libs_ok = api.gtk != nullptr && api.gdk != nullptr && api.gobject != nullptr && api.glib != nullptr &&
                         api.webkit != nullptr && api.jsc != nullptr && api.cairo != nullptr;
    if (!libs_ok) {
        if (api.gtk != nullptr) {
            dlclose(api.gtk);
        }
        if (api.gdk != nullptr) {
            dlclose(api.gdk);
        }
        if (api.gobject != nullptr) {
            dlclose(api.gobject);
        }
        if (api.glib != nullptr) {
            dlclose(api.glib);
        }
        if (api.webkit != nullptr) {
            dlclose(api.webkit);
        }
        if (api.jsc != nullptr) {
            dlclose(api.jsc);
        }
        if (api.cairo != nullptr) {
            dlclose(api.cairo);
        }
        return std::nullopt;
    }

    api.main_context_iteration = resolve<FnMainContextIteration>(api.glib, "g_main_context_iteration");
    api.main_context_query = resolve<FnMainContextQuery>(api.glib, "g_main_context_query");
    api.free = resolve<FnFree>(api.glib, "g_free");
    api.error_free = resolve<FnErrorFree>(api.glib, "g_error_free");
    api.setenv = resolve<FnSetenv>(api.glib, "g_setenv");
    api.getenv = resolve<FnGetenv>(api.glib, "g_getenv");

    api.object_unref = resolve<FnObjectUnref>(api.gobject, "g_object_unref");
    api.type_check_instance_is_a = resolve<FnTypeCheckInstanceIsA>(api.gobject, "g_type_check_instance_is_a");
    api.cclosure_new = resolve<FnCClosureNew>(api.gobject, "g_cclosure_new");
    api.signal_connect_closure = resolve<FnSignalConnectClosure>(api.gobject, "g_signal_connect_closure");

    api.display_get_default = resolve<FnDisplayGetDefault>(api.gdk, "gdk_display_get_default");
    api.display_flush = resolve<FnDisplayFlush>(api.gdk, "gdk_display_flush");
    api.window_new = resolve<FnWindowNew>(api.gdk, "gdk_window_new");
    api.window_move_resize = resolve<FnWindowMoveResize>(api.gdk, "gdk_window_move_resize");
    api.window_show = resolve<FnWindowShow>(api.gdk, "gdk_window_show");
    api.window_hide = resolve<FnWindowHide>(api.gdk, "gdk_window_hide");
    api.window_destroy = resolve<FnWindowDestroy>(api.gdk, "gdk_window_destroy");
    api.window_get_user_data = resolve<FnWindowGetUserData>(api.gdk, "gdk_window_get_user_data");
    api.x11_foreign_new = resolve<FnX11WindowForeignNewForDisplay>(api.gdk, "gdk_x11_window_foreign_new_for_display");
    api.x11_display_get_type = resolve<FnX11DisplayGetType>(api.gdk, "gdk_x11_display_get_type");

    api.init_check = resolve<FnInitCheck>(api.gtk, "gtk_init_check");
    api.window_new_gtk = resolve<FnWindowNewGtk>(api.gtk, "gtk_window_new");
    api.set_has_window = resolve<FnWidgetSetHasWindow>(api.gtk, "gtk_widget_set_has_window");
    api.widget_realize = resolve<FnWidgetRealize>(api.gtk, "gtk_widget_realize");
    api.widget_destroy = resolve<FnWidgetDestroy>(api.gtk, "gtk_widget_destroy");
    api.register_window = resolve<FnWidgetRegisterWindow>(api.gtk, "gtk_widget_register_window");
    api.widget_get_window = resolve<FnWidgetGetWindow>(api.gtk, "gtk_widget_get_window");
    api.widget_set_window = resolve<FnWidgetSetWindow>(api.gtk, "gtk_widget_set_window");
    api.set_hexpand = resolve<FnWidgetSetHexpand>(api.gtk, "gtk_widget_set_hexpand");
    api.set_vexpand = resolve<FnWidgetSetVexpand>(api.gtk, "gtk_widget_set_vexpand");
    api.container_add = resolve<FnContainerAdd>(api.gtk, "gtk_container_add");
    api.widget_show = resolve<FnWidgetShow>(api.gtk, "gtk_widget_show");
    api.widget_hide = resolve<FnWidgetHide>(api.gtk, "gtk_widget_hide");
    api.size_allocate = resolve<FnWidgetSizeAllocate>(api.gtk, "gtk_widget_size_allocate");

    api.web_view_new = resolve<FnWebViewNew>(api.webkit, "webkit_web_view_new");
    api.web_view_new_with_ucm =
        resolve<FnWebViewNewWithUcm>(api.webkit, "webkit_web_view_new_with_user_content_manager");
    api.load_uri = resolve<FnWebViewLoadUri>(api.webkit, "webkit_web_view_load_uri");
    api.load_html = resolve<FnWebViewLoadHtml>(api.webkit, "webkit_web_view_load_html");
    api.reload = resolve<FnWebViewReload>(api.webkit, "webkit_web_view_reload");
    api.stop_loading = resolve<FnWebViewStopLoading>(api.webkit, "webkit_web_view_stop_loading");
    api.go_back = resolve<FnWebViewGoBack>(api.webkit, "webkit_web_view_go_back");
    api.go_forward = resolve<FnWebViewGoForward>(api.webkit, "webkit_web_view_go_forward");
    api.can_go_back = resolve<FnWebViewCanGoBack>(api.webkit, "webkit_web_view_can_go_back");
    api.can_go_forward = resolve<FnWebViewCanGoForward>(api.webkit, "webkit_web_view_can_go_forward");
    api.get_uri = resolve<FnWebViewGetUri>(api.webkit, "webkit_web_view_get_uri");
    api.get_title = resolve<FnWebViewGetTitle>(api.webkit, "webkit_web_view_get_title");
    api.get_settings = resolve<FnWebViewGetSettings>(api.webkit, "webkit_web_view_get_settings");
    api.set_javascript = resolve<FnSettingsSetJavascript>(api.webkit, "webkit_settings_set_enable_javascript");
    api.set_zoom_level = resolve<FnWebViewSetZoomLevel>(api.webkit, "webkit_web_view_set_zoom_level");
    api.evaluate_javascript = resolve<FnWebViewEvaluateJavascript>(api.webkit, "webkit_web_view_evaluate_javascript");
    api.evaluate_finish = resolve<FnWebViewEvaluateFinish>(api.webkit, "webkit_web_view_evaluate_javascript_finish");
    api.get_snapshot = resolve<FnWebViewGetSnapshot>(api.webkit, "webkit_web_view_get_snapshot");
    api.get_snapshot_finish = resolve<FnWebViewGetSnapshotFinish>(api.webkit, "webkit_web_view_get_snapshot_finish");
    api.ucm_new = resolve<FnUcmNew>(api.webkit, "webkit_user_content_manager_new");
    api.ucm_register_handler =
        resolve<FnUcmRegisterHandler>(api.webkit, "webkit_user_content_manager_register_script_message_handler");
    api.ucm_add_script = resolve<FnUcmAddScript>(api.webkit, "webkit_user_content_manager_add_script");
    api.user_script_new = resolve<FnUserScriptNew>(api.webkit, "webkit_user_script_new");
    api.user_script_unref = resolve<FnUserScriptUnref>(api.webkit, "webkit_user_script_unref");
    api.js_result_get_value = resolve<FnJsResultGetValue>(api.webkit, "webkit_javascript_result_get_js_value");

    api.jsc_to_string = resolve<FnJscToString>(api.jsc, "jsc_value_to_string");
    api.jsc_to_json = resolve<FnJscToJson>(api.jsc, "jsc_value_to_json");

    api.surface_get_data = resolve<FnSurfaceGetData>(api.cairo, "cairo_image_surface_get_data");
    api.surface_get_width = resolve<FnSurfaceGetWidth>(api.cairo, "cairo_image_surface_get_width");
    api.surface_get_height = resolve<FnSurfaceGetHeight>(api.cairo, "cairo_image_surface_get_height");
    api.surface_get_stride = resolve<FnSurfaceGetStride>(api.cairo, "cairo_image_surface_get_stride");
    api.surface_get_format = resolve<FnSurfaceGetFormat>(api.cairo, "cairo_image_surface_get_format");
    api.surface_destroy = resolve<FnSurfaceDestroy>(api.cairo, "cairo_surface_destroy");

    // 必需入口点全量核对（缺一即降级；不存在「可选符号」）。
    const bool syms_ok =
        api.main_context_iteration != nullptr && api.main_context_query != nullptr && api.free != nullptr &&
        api.error_free != nullptr && api.setenv != nullptr && api.getenv != nullptr && api.object_unref != nullptr &&
        api.type_check_instance_is_a != nullptr && api.cclosure_new != nullptr &&
        api.signal_connect_closure != nullptr && api.display_get_default != nullptr && api.display_flush != nullptr &&
        api.window_new != nullptr && api.window_move_resize != nullptr && api.window_show != nullptr &&
        api.window_hide != nullptr && api.window_destroy != nullptr && api.window_get_user_data != nullptr &&
        api.x11_foreign_new != nullptr && api.x11_display_get_type != nullptr && api.init_check != nullptr &&
        api.window_new_gtk != nullptr && api.set_has_window != nullptr && api.widget_realize != nullptr &&
        api.widget_destroy != nullptr && api.register_window != nullptr && api.widget_get_window != nullptr &&
        api.widget_set_window != nullptr && api.set_hexpand != nullptr && api.set_vexpand != nullptr &&
        api.container_add != nullptr && api.widget_show != nullptr && api.widget_hide != nullptr &&
        api.size_allocate != nullptr &&
        api.web_view_new != nullptr && api.web_view_new_with_ucm != nullptr && api.load_uri != nullptr &&
        api.load_html != nullptr && api.reload != nullptr && api.stop_loading != nullptr && api.go_back != nullptr &&
        api.go_forward != nullptr && api.can_go_back != nullptr && api.can_go_forward != nullptr &&
        api.get_uri != nullptr && api.get_title != nullptr && api.get_settings != nullptr &&
        api.set_javascript != nullptr && api.set_zoom_level != nullptr && api.evaluate_javascript != nullptr &&
        api.evaluate_finish != nullptr && api.get_snapshot != nullptr && api.get_snapshot_finish != nullptr &&
        api.ucm_new != nullptr && api.ucm_register_handler != nullptr && api.ucm_add_script != nullptr &&
        api.user_script_new != nullptr && api.js_result_get_value != nullptr && api.jsc_to_string != nullptr &&
        api.jsc_to_json != nullptr && api.surface_get_data != nullptr && api.surface_get_width != nullptr &&
        api.surface_get_height != nullptr && api.surface_get_stride != nullptr && api.surface_get_format != nullptr &&
        api.surface_destroy != nullptr;
    if (!syms_ok) {
        dlclose(api.gtk);
        dlclose(api.gdk);
        dlclose(api.gobject);
        dlclose(api.glib);
        dlclose(api.webkit);
        dlclose(api.jsc);
        dlclose(api.cairo);
        return std::nullopt;
    }
    return api;
}

// ---- 纯函数小工具 ----

/// @brief JS 字符串字面量编码：把 JSON 文本安全包进单引号 JS 字符串（页侧 `_receive`
/// 的入参形态）。引号 / 反斜杠 / 控制字符逐一转义，其余字节原样透传。
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

/// @brief cairo 预乘通道反算：cairo 存 `round(c * a / 255)`，反算
/// `round(premul * 255 / a)`，量化误差 ≤ 1（视觉不可辨）。
/// @param premul 预乘后的通道值。
/// @param alpha 不透明度（> 0）。
/// @return 非预乘通道值（已夹至 255）。
[[nodiscard]] auto unpremul_channel(unsigned premul, unsigned alpha) -> unsigned char {
    const unsigned v = (premul * 255U + alpha / 2U) / alpha;
    return static_cast<unsigned char>(v > 255U ? 255U : v);
}

/// @brief cairo ARGB32 surface → RGBA8 位图。内存布局为小端 BGRA 且**预乘**，
/// 与 `Image` 契约（RGBA8 非预乘）相差两个维度，逐像素反转。
/// @param api ABI 表。
/// @param surface `cairo_surface_t*`（调用方保留所有权）。
/// @return 转换结果；格式不符或数据为空时返回空图。
[[nodiscard]] auto surface_to_image(const WebKitGtkApi &api, void *surface) -> Image {
    Image img;
    if (surface == nullptr) {
        return img;
    }
    const unsigned char *data = api.surface_get_data(surface);
    const int w = api.surface_get_width(surface);
    const int h = api.surface_get_height(surface);
    const int stride = api.surface_get_stride(surface);
    const int format = api.surface_get_format(surface);
    if (data == nullptr || w <= 0 || h <= 0 || stride < w * 4 || format != AURORA_CAIRO_FORMAT_ARGB32) {
        return img;
    }
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char *row = data + static_cast<std::ptrdiff_t>(y) * static_cast<std::ptrdiff_t>(stride);
        for (int x = 0; x < w; ++x) {
            const unsigned char *px = row + static_cast<std::ptrdiff_t>(x) * 4;
            const std::size_t o =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
            const unsigned a = px[3];
            img.pixels[o + 3] = static_cast<unsigned char>(a);
            if (a == 0U) {
                img.pixels[o + 0] = 0;
                img.pixels[o + 1] = 0;
                img.pixels[o + 2] = 0;
                continue;
            }
            img.pixels[o + 0] = unpremul_channel(px[2], a);
            img.pixels[o + 1] = unpremul_channel(px[1], a);
            img.pixels[o + 2] = unpremul_channel(px[0], a);
        }
    }
    return img;
}

// ---- 共享可变状态 ----

// sink、五个信号 closure 与两类异步回调都经它访问，覆盖「控件先亡 / 宿主先亡 /
// 异步回调迟到」三种时序：weak_ptr 门管回调判活（见 WebViewCallbacks 注释），
// detached 管 detach 后静默，pending 队列管「异步结果必须兜底回调」的接口契约。
struct WebKitGtkState {
    std::atomic<bool> detached{false};
    std::shared_ptr<const WebKitGtkApi> api;  ///< 进程级 ABI 表（寿命覆盖全部视图）
    void *display = nullptr;  ///< GdkDisplay*（default；flush 用）
    void *foreign_win = nullptr;  ///< GdkWindow*（foreign 包装；**本结构的引用**，detach 时释放）
    void *container = nullptr;  ///< GdkWindow*（被收养的 CHILD 容器；只几何/显隐，**绝不销毁**）
    void *gtk_win = nullptr;  ///< GtkWidget*（GtkWindow toplevel）
    void *web_view = nullptr;  ///< WebKitWebView*
    void *ucm = nullptr;  ///< WebKitUserContentManager*
    std::weak_ptr<WebViewCallbacks> callbacks;  ///< 入向回调（弱引用，判活门）
    std::string last_title;  ///< 标题去重（契约是「变化」才报）
    struct PendingEval {
        std::function<void(std::string)> done;
    };
    std::vector<PendingEval> pending_eval;  ///< 异步求值结果（FIFO 配对）
    /// 在途异步快照。
    ///
    /// `NativeViewSink::capture` 的契约是「捕获失败或尚未就绪时 `done` 必须仍被调用且携空图，
    /// 调用方不得靠『没收到回调』推断结果」。GTK 侧同样存在回调永不到达的情形，故每条登记
    /// 发起时刻，由 `WebKitGtkEngine::pump_events`（宿主每帧驱动）按墙钟超时兜底兑现。
    /// 按 `id` 精确配对而非 FIFO——超时会先摘走队首，若真回调仍配对队首就会错位到下一次请求。
    struct PendingCapture {
        std::uint64_t id = 0;
        std::function<void(Image)> done;
        /// 竞态门：完成回调与超时兜底谁先到谁兑现，后到者见 `true` 即退（不得回调两次）。
        std::shared_ptr<std::atomic<bool>> settled;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    };
    std::vector<PendingCapture> pending_capture;  ///< 异步快照结果（按 id 配对）
};

/// 捕获回调的墙钟等待上限（对位 WebView2 侧同名义常量）：正常捕获是毫秒级的一次合成，
/// 这个量级只为兜住「永不回调」，不是给慢回调设的期限。
constexpr auto AURORA_WEBKIT_CAPTURE_TIMEOUT = std::chrono::seconds(15);

/// 快照配对 id 发号器（UI 线程单线程取号）。
[[nodiscard]] auto next_capture_id() -> std::uint64_t {
    static std::uint64_t counter = 0;
    return ++counter;
}

/// `get_snapshot` 的 user_data：回调据此精确摘走自己那一条（FIFO 会被超时兜底打乱）。
struct SnapshotKey {
    std::shared_ptr<WebKitGtkState> state;
    std::uint64_t id = 0;
};

// ---- 入向回调转发（弱引用判活，控件已析构即静默丢弃） ----

void fire_navigation(WebKitGtkState &st, std::string uri, WebViewNavigationState state) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_navigation) {
            cb->on_navigation(std::move(uri), state);
        }
    }
}

void fire_title(WebKitGtkState &st, std::string title) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_title_changed) {
            cb->on_title_changed(std::move(title));
        }
    }
}

void fire_web_message(WebKitGtkState &st, std::string json) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_web_message) {
            cb->on_web_message(std::move(json));
        }
    }
}

void fire_process_failed(WebKitGtkState &st, std::string detail) {
    if (const std::shared_ptr<WebViewCallbacks> cb = st.callbacks.lock()) {
        if (cb->on_process_failed) {
            cb->on_process_failed(std::move(detail));
        }
    }
}

/// @brief 标题变化上报（去重后）。
/// @param st 视图状态。
/// @param title 当前标题（get_title 的产物，可能为空串）。
void report_title(WebKitGtkState &st, const std::string &title) {
    if (title == st.last_title) {
        return;  // 契约是「变化」才报；导航重复时 GTK 会多次给相同标题
    }
    st.last_title = title;
    fire_title(st, title);
}

/// @brief 导航 URI 上报辅助：取当前 uri（g_free 归还）。
/// @param api ABI 表。
/// @param web_view 视图。
/// @return 当前 uri；无则为空串。
[[nodiscard]] auto current_uri(const WebKitGtkApi &api, void *web_view) -> std::string {
    char *uri = api.get_uri(web_view);
    if (uri == nullptr) {
        return {};
    }
    // 注：webkit_web_view_get_uri 返回的串由 WebView 持有（不可 g_free），仅拷贝即可。
    std::string out(uri);
    return out;
}

// ---- 信号 closure 的 data 协议 ----
// 每个连接 / 每次异步调用都分配一个独立的 `shared_ptr<State>` 作 key：closure 析构
// （widget 销毁 / UCM 释放）或一次性回调完成时经 GClosureNotify / 手动删除释放，
// 与 GLib 的引用语义精确配对，不共享 key（共享会双重释放）。

/// @brief GClosureNotify：closure 析构时释放 key。
void on_closure_notify(void *data, void * /*closure*/) { delete static_cast<std::shared_ptr<WebKitGtkState> *>(data); }

/// @brief 从回调 user_data 取状态引用（key 必非空）。
[[nodiscard]] auto state_from(void *user_data) -> WebKitGtkState & {
    return **static_cast<std::shared_ptr<WebKitGtkState> *>(user_data);
}

/// @brief 连接一个信号（分配独立 key；closure 由 signal 系统持有，随实例析构释放）。
/// @param api ABI 表。
/// @param instance 信号所属对象。
/// @param signal 信号名（含 detail，如 "notify::title"）。
/// @param fn 回调（函数指针；GLib 经 GCallback 统一调用约定，调用处显式转型）。
/// @param state 状态（key 内容）。
/// @return closure 指针；分配失败为 nullptr（调用方按降级路径处理）。
auto connect_signal(const WebKitGtkApi &api, void *instance, const char *signal, GCallback fn,
                    const std::shared_ptr<WebKitGtkState> &state) -> void * {
    auto *key = new std::shared_ptr<WebKitGtkState>(state);
    void *closure = api.cclosure_new(fn, key, &on_closure_notify);
    if (closure == nullptr) {
        delete key;
        return nullptr;
    }
    api.signal_connect_closure(instance, signal, closure, 0);
    return closure;
}

// ---- 平台回调（全部 main-thread；GLib 信号与 GAsyncReadyCallback） ----

// GtkWindow realize：把被收养的 container 设为 widget 的 window，销毁 GTK 自建的
// 默认窗（set_window 前的 priv->window），并按标准嵌入配方 register_window。
void on_gtk_win_realize(void *widget, void *user_data) {
    WebKitGtkState &st = state_from(user_data);
    const WebKitGtkApi &api = *st.api;
    void *old = api.widget_get_window(widget);
    api.widget_set_window(widget, st.container);
    if (old != nullptr && old != st.container) {
        // 默认窗是 GTK 为 toplevel 自建的 GdkWindow（从未映射）：此时 widget 已换窗，
        // 销毁它即是收养配方的既有步骤（三十轮实证序列）。
        api.window_destroy(old);
    }
    void *ud = nullptr;
    api.window_get_user_data(st.container, &ud);
    if (ud == nullptr) {
        api.register_window(widget, st.container);
    }
}

// load-changed 三态：STARTED → Loading；FINISHED → Finished（并顺带上报标题）。
// COMMITTED / REDIRECTED 不是终态，不映射（失败走 load-failed）。
void on_load_changed(void *web_view, int event, void *user_data) {
    WebKitGtkState &st = state_from(user_data);
    if (st.detached) {
        return;
    }
    const WebKitGtkApi &api = *st.api;
    if (event == AURORA_WEBKIT_LOAD_STARTED) {
        fire_navigation(st, current_uri(api, web_view), WebViewNavigationState::Loading);
        return;
    }
    if (event == AURORA_WEBKIT_LOAD_FINISHED) {
        fire_navigation(st, current_uri(api, web_view), WebViewNavigationState::Finished);
        // 标题一律由 notify::title 上报（见 on_title_notify）——load-changed 内 get_title 实测
        // 在 FINISHED 仍返回 null，且其返回串由 WebView 持有（不可 g_free），故此处不轮询。
        return;
    }
}

// notify::title：标题属性变化即上报（WebKit 在 <title> 解析后触发，早于或等于
// load-changed(FINISHED)）；比在 load-changed 里轮询 get_title 更可靠（后者在
// FINISHED 时实测仍返回 null）。
void on_title_notify(void *web_view, void * /*pspec*/, void *user_data) {
    WebKitGtkState &st = state_from(user_data);
    if (st.detached) {
        return;
    }
    const WebKitGtkApi &api = *st.api;
    char *title = api.get_title(web_view);
    if (title != nullptr) {
        // get_title 返回的串由 WebView 持有（不可 g_free），仅拷贝上报。
        report_title(st, std::string(title));
    }
}

// load-failed：导航失败态；返回 1（gboolean TRUE）阻止 GTK 继续传播（错误自管）。
// 失败详情暂无回调面承载（导航回调只带 uri 与 state），故不取 GError。
int on_load_failed(void * /*web_view*/, int /*event*/, const char *uri, void * /*error*/, void *user_data) {
    WebKitGtkState &st = state_from(user_data);
    if (st.detached) {
        return 1;
    }
    fire_navigation(st, uri != nullptr ? std::string(uri) : std::string{}, WebViewNavigationState::Failed);
    return 1;
}

// script-message-received：页面经 shim 的 postMessage 投出 JSON 文本。
void on_script_message(void * /*manager*/, void *result, void *user_data) {
    WebKitGtkState &st = state_from(user_data);
    if (st.detached) {
        return;
    }
    const WebKitGtkApi &api = *st.api;
    void *value = api.js_result_get_value(result);
    if (value == nullptr) {
        return;
    }
    char *text = api.jsc_to_string(value);
    if (text != nullptr) {
        fire_web_message(st, std::string(text));
        api.free(text);
    }
}

// web-process-terminated：渲染进程崩溃 / 超内存 / API 终止。
void on_process_terminated(void * /*web_view*/, int reason, void *user_data) {
    WebKitGtkState &st = state_from(user_data);
    if (st.detached) {
        return;
    }
    const char *detail = "webkit web process terminated";
    if (reason == AURORA_WEBKIT_PROCESS_CRASHED) {
        detail = "webkit web process crashed";
    } else if (reason == AURORA_WEBKIT_PROCESS_OOM) {
        detail = "webkit web process exceeded memory limit";
    }
    fire_process_failed(st, detail);
}

// 异步求值完成：webkit 的 GAsyncResult 经默认主上下文回到 UI 线程。
void on_eval_done(void *src, void *res, void *user_data) {
    auto *key = static_cast<std::shared_ptr<WebKitGtkState> *>(user_data);
    std::shared_ptr<WebKitGtkState> st = *key;  // 先取持有再删 key（回调路径唯一）
    delete key;
    if (st->detached) {
        return;  // done 已由 detach 兜底调用
    }
    const WebKitGtkApi &api = *st->api;
    std::function<void(std::string)> done;
    if (!st->pending_eval.empty()) {
        done = std::move(st->pending_eval.front().done);
        st->pending_eval.erase(st->pending_eval.begin());
    }
    GError *error = nullptr;
    void *value = api.evaluate_finish(src, res, &error);
    if (error != nullptr) {
        api.error_free(error);
    }
    if (value == nullptr) {
        if (done) {
            done(std::string{});  // 契约：失败时空串
        }
        return;
    }
    char *json = api.jsc_to_json(value, 0);
    if (done) {
        done(json != nullptr ? std::string(json) : std::string{});
    }
    if (json != nullptr) {
        api.free(json);
    }
    api.object_unref(value);  // finish 返回 transfer full
}

// 异步快照完成：cairo surface → RGBA8（反预乘），按配对 id 摘走自己那一条。
void on_snapshot_done(void *src, void *res, void *user_data) {
    auto *key = static_cast<SnapshotKey *>(user_data);
    std::shared_ptr<WebKitGtkState> st = std::move(key->state);
    const std::uint64_t id = key->id;
    delete key;
    if (st->detached) {
        return;
    }
    const WebKitGtkApi &api = *st->api;
    // 先 finish：`res` 是 transfer full，无论后面能否配上都得收下并释放（超时兜底已兑现的那
    // 一类回调同样会抵达这里，那时没有 done 可交，但 surface 仍要还回去）。
    GError *error = nullptr;
    void *surface = api.get_snapshot_finish(src, res, &error);
    if (error != nullptr) {
        api.error_free(error);
    }
    std::function<void(Image)> done;
    std::shared_ptr<std::atomic<bool>> settled;
    for (auto it = st->pending_capture.begin(); it != st->pending_capture.end(); ++it) {
        if (it->id == id) {
            done = std::move(it->done);
            settled = std::move(it->settled);
            st->pending_capture.erase(it);
            break;
        }
    }
    const bool stale = !done || (settled && settled->exchange(true));
    if (stale) {
        if (surface != nullptr) {
            api.surface_destroy(surface);  // 已由超时兜底兑现（或视图已拆）：不得再回调一次
        }
        return;
    }
    done(surface == nullptr ? Image{} : surface_to_image(api, surface));  // 空 surface ⇒ 空图（契约）
    if (surface != nullptr) {
        api.surface_destroy(surface);  // finish 返回 transfer full
    }
}

// ---- 视图拆毁（定版 teardown）----
// detach（宿主摘除登记条目）与 create_view 失败回退共用同一序列：先置 detached
// 门，再按「destroy win（GTK 自毁 container）→ flush → unref UCM → unref foreign」
// 释放，最后把异步完成项以空结果兜底。**container 自始至终不碰**——其最终引用已由
// GTK 的 real_unrealize 随 gdk_window_destroy(priv->window) 释放（见文件头）。
void teardown_view(const WebKitGtkApi &api, const std::shared_ptr<WebKitGtkState> &st) {
    if (st->detached) {
        return;  // 幂等
    }
    st->detached = true;
    if (st->gtk_win != nullptr) {
        api.widget_destroy(st->gtk_win);
        st->gtk_win = nullptr;
        st->web_view = nullptr;  // 子 widget，随 win 递归销毁
        st->container = nullptr;  // GTK 已销毁；置空防误用
        api.display_flush(st->display);
    } else if (st->container != nullptr) {
        // 未收养（adopt 中途失败）：container 的引用仍归引擎，显式销毁。
        // 收养成功后的路径绝不走这里——那时 container 归 GTK（见文件头）。
        api.window_destroy(st->container);
        st->container = nullptr;
    }
    if (st->ucm != nullptr) {
        api.object_unref(st->ucm);  // 我们的引用（webview 那份随其销毁释放）
        st->ucm = nullptr;
    }
    if (st->foreign_win != nullptr) {
        api.object_unref(st->foreign_win);  // foreign 包装的引用是引擎自己的
        st->foreign_win = nullptr;
    }
    // 异步完成项兜底：契约要求「捕获失败或尚未就绪时 done 必须被调用且携空图」。
    for (WebKitGtkState::PendingCapture &pc : st->pending_capture) {
        if (pc.settled && !pc.settled->exchange(true) && pc.done) {
            pc.done(Image{});
        }
    }
    st->pending_capture.clear();
    for (WebKitGtkState::PendingEval &p : st->pending_eval) {
        if (p.done) {
            p.done(std::string{});
        }
    }
    st->pending_eval.clear();
}

// ---- 控件侧 WebViewSink 实现 ----

class WebKitGtkSink final : public WebViewSink {
  public:
    explicit WebKitGtkSink(std::shared_ptr<WebKitGtkState> state) : state_(std::move(state)) {}
    ~WebKitGtkSink() override = default;
    // 控件经 unique_ptr 持有的唯一视图实现，不可拷贝 / 移动。
    WebKitGtkSink(const WebKitGtkSink &) = delete;
    auto operator=(const WebKitGtkSink &) -> WebKitGtkSink & = delete;
    WebKitGtkSink(WebKitGtkSink &&) = delete;
    auto operator=(WebKitGtkSink &&) -> WebKitGtkSink & = delete;

    auto set_native_rect(const NativeViewRect &rect) -> void override {
        if (state_->detached || state_->gtk_win == nullptr || state_->container == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        // win 的分配用容器内坐标（0,0,w,h）；容器自身按宿主物理坐标定位。
        const GdkRectangle al{.x = 0, .y = 0, .width = rect.width, .height = rect.height};
        api.size_allocate(state_->gtk_win, &al);
        api.window_move_resize(state_->container, rect.x, rect.y, rect.width, rect.height);
        api.display_flush(state_->display);  // GDK 只在主循环迭代 flush，几何也要显式推
    }

    auto set_native_visible(bool visible) -> void override {
        if (state_->detached || state_->gtk_win == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        // 经顶层窗 show/hide 触发 GTK 完整 map 级联（gtk_win → web_view realize+map），
        // 使 WebKit 在交互模式真正上屏并接收输入；快照模式永不调用本函数 ⇒ 恒离屏。
        if (visible) {
            api.widget_show(state_->gtk_win);
        } else {
            api.widget_hide(state_->gtk_win);
        }
        api.display_flush(state_->display);  // 显隐必须配显式 flush（GDK 排队语义）
    }

    auto capture(std::function<void(Image)> done) -> void override {
        if (state_->detached) {
            if (done) {
                done(Image{});  // 契约：done 必须被调用，失败携空图
            }
            return;
        }
        if (done == nullptr || state_->web_view == nullptr) {
            if (done) {
                done(Image{});
            }
            return;
        }
        const std::uint64_t id = next_capture_id();
        state_->pending_capture.push_back(
            WebKitGtkState::PendingCapture{.id = id, .done = done, .settled = std::make_shared<std::atomic<bool>>(false)});
        auto *key = new SnapshotKey{.state = state_, .id = id};
        const WebKitGtkApi &api = *state_->api;
        api.get_snapshot(state_->web_view, AURORA_WEBKIT_SNAPSHOT_REGION_VISIBLE, AURORA_WEBKIT_SNAPSHOT_OPTIONS_NONE,
                         nullptr, &on_snapshot_done, key);
    }

    auto detach() -> void override { teardown_view(*state_->api, state_); }

    auto navigate(const std::string &uri) -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        api.load_uri(state_->web_view, uri.c_str());
    }

    auto navigate_to_string(const std::string &html, const std::string &base_uri) -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        api.load_html(state_->web_view, html.c_str(), base_uri.empty() ? nullptr : base_uri.c_str());
    }

    auto reload() -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        api.reload(state_->web_view);
    }

    auto stop() -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        api.stop_loading(state_->web_view);
    }

    auto go_back() -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        if (api.can_go_back(state_->web_view) != 0) {
            api.go_back(state_->web_view);  // 无历史时空操作（契约）
        }
    }

    auto go_forward() -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        if (api.can_go_forward(state_->web_view) != 0) {
            api.go_forward(state_->web_view);
        }
    }

    auto set_zoom_factor(float zoom) -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        api.set_zoom_level(state_->web_view, static_cast<gdouble>(zoom));  // 1.0 = 原始尺寸
    }

    auto set_javascript_enabled(bool enabled) -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;
        }
        const WebKitGtkApi &api = *state_->api;
        void *settings = api.get_settings(state_->web_view);
        if (settings != nullptr) {
            api.set_javascript(settings, enabled ? 1 : 0);
        }
    }

    auto evaluate_script(const std::string &js, std::function<void(std::string)> done) -> void override {
        if (state_->detached) {
            if (done) {
                done(std::string{});
            }
            return;
        }
        if (state_->web_view == nullptr) {
            if (done) {
                done(std::string{});
            }
            return;
        }
        if (done != nullptr) {
            state_->pending_eval.push_back(WebKitGtkState::PendingEval{.done = std::move(done)});
        }
        auto *key = new std::shared_ptr<WebKitGtkState>(state_);
        const WebKitGtkApi &api = *state_->api;
        api.evaluate_javascript(state_->web_view, js.c_str(), -1, nullptr, nullptr, nullptr, &on_eval_done, key);
    }

    auto post_message(const std::string &json) -> void override {
        if (state_->detached || state_->web_view == nullptr) {
            return;  // 契约：通道未建立时丢弃（不排队、不泄漏）
        }
        // native→page 走 evaluate 调 shim 的 _receive；载荷是 JS 单引号字符串里的
        // JSON 文本（页侧 JSON.parse 后转 onmessage）。
        std::string script = "window.aurora._receive('";
        js_escape(json, script);
        script += "');";
        evaluate_script(script, nullptr);
    }

    auto set_callbacks(std::weak_ptr<WebViewCallbacks> callbacks) -> void override {
        state_->callbacks = std::move(callbacks);
    }

  private:
    std::shared_ptr<WebKitGtkState> state_;
};

// ---- 进程级引擎 ----

class WebKitGtkEngine final : public NativeViewEngine {
  public:
    explicit WebKitGtkEngine(std::shared_ptr<const WebKitGtkApi> api) : api_(std::move(api)) {}
    ~WebKitGtkEngine() override = default;
    // 进程级单例引擎，经 shared_ptr 管理，不可拷贝 / 移动。
    WebKitGtkEngine(const WebKitGtkEngine &) = delete;
    auto operator=(const WebKitGtkEngine &) -> WebKitGtkEngine & = delete;
    WebKitGtkEngine(WebKitGtkEngine &&) = delete;
    auto operator=(WebKitGtkEngine &&) -> WebKitGtkEngine & = delete;

    [[nodiscard]] auto id() const -> WebViewEngine override { return WebViewEngine::WebKitGtk; }

    [[nodiscard]] auto probe() const -> WebViewCapabilities override {
        static const WebViewCapabilities CAPS = probe_once(*api_);
        return CAPS;
    }

    [[nodiscard]] auto create_view(void *parent_window) -> std::unique_ptr<WebViewSink> override;

    [[nodiscard]] auto poll_watches() const -> std::vector<WatchFd> override;

    auto pump_events() -> void override {
        if (api_ != nullptr && api_->main_context_iteration != nullptr) {
            api_->main_context_iteration(nullptr, 0);  // 非阻塞推进一轮 GLib 源
        }
        // 另一半职责（对位 WebView2 引擎）：把超时的在途快照兑现成空图。`get_snapshot` 同样
        // 存在完成回调永不到达的情形，没有这一跳，调用方就只能靠「没收到回调」猜结果——
        // 正是 `NativeViewSink::capture` 禁止的。
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::weak_ptr<WebKitGtkState>> live;
        live.reserve(live_states_.size());
        for (std::weak_ptr<WebKitGtkState> &w : live_states_) {
            const std::shared_ptr<WebKitGtkState> st = w.lock();
            if (st == nullptr) {
                continue;  // 视图已拆：一并从表里摘掉
            }
            live.push_back(w);
            std::vector<WebKitGtkState::PendingCapture> keep;
            keep.reserve(st->pending_capture.size());
            for (WebKitGtkState::PendingCapture &pc : st->pending_capture) {
                if (now - pc.started >= AURORA_WEBKIT_CAPTURE_TIMEOUT) {
                    if (pc.settled && !pc.settled->exchange(true) && pc.done) {
                        pc.done(Image{});
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
    [[nodiscard]] static auto probe_once(const WebKitGtkApi &api) -> WebViewCapabilities;
    std::vector<std::weak_ptr<WebKitGtkState>> live_states_;  ///< 活着的视图（引擎心跳据此扫在途快照）

    std::shared_ptr<const WebKitGtkApi> api_;
};

[[nodiscard]] auto WebKitGtkEngine::probe_once(const WebKitGtkApi &api) -> WebViewCapabilities {
    // 交互模式依赖 GTK 的 X11 backend（收养外部 X 窗口）；Wayland 下无法把另一客户端
    // 的 surface 作定位子 surface，收养不成立；快照模式未在 Wayland 实证，故一并
    // 不可用——归因 UnsupportedPlatform（与「缺运行时」的 MissingRuntime 区分）。
    if (api.getenv("GDK_BACKEND") == nullptr) {
        // 仅在宿主未表态时强制 X11；已设（含显式 wayland）则尊重。
        api.setenv("GDK_BACKEND", "x11", 0);
    }
    if (api.init_check(nullptr, nullptr) == 0) {
        return WebViewCapabilities{.engine = WebViewEngine::WebKitGtk,
                                   .availability = WebViewAvailability::MissingRuntime};
    }
    void *display = api.display_get_default();
    if (display == nullptr) {
        return WebViewCapabilities{.engine = WebViewEngine::WebKitGtk,
                                   .availability = WebViewAvailability::MissingRuntime};
    }
    if (api.type_check_instance_is_a(display, api.x11_display_get_type()) == 0) {
        return WebViewCapabilities{.engine = WebViewEngine::WebKitGtk,
                                   .availability = WebViewAvailability::UnsupportedPlatform};
    }
    return WebViewCapabilities{.interactive = true,
                               .snapshot = true,
                               .engine = WebViewEngine::WebKitGtk,
                               .availability = WebViewAvailability::Available};
}

// 建视图的阶段一：收养宿主 X 窗口（foreign 包装 → CHILD container → GtkWindow 换窗）。
// 失败即返回 false，**不做部分清理**——已建资源由调用方经 teardown_view 统一释放
// （该函数按「已收养 / 未收养」区分 container 的归属）。
[[nodiscard]] auto adopt_window(const WebKitGtkApi &api, const std::shared_ptr<WebKitGtkState> &st,
                                unsigned long host_xid) -> bool {
    st->display = api.display_get_default();
    if (st->display == nullptr) {
        return false;
    }
    st->foreign_win = api.x11_foreign_new(st->display, host_xid);
    if (st->foreign_win == nullptr) {
        return false;
    }
    GdkWindowAttr attr;  // 宽高在 CHILD 窗口恒生效，无需 WA_WIDTH/HEIGHT（实测）
    std::memset(&attr, 0, sizeof attr);
    attr.window_type = AURORA_GDK_WINDOW_CHILD;
    attr.x = 0;
    attr.y = 0;
    attr.width = AURORA_WEBKIT_INIT_WIDTH;
    attr.height = AURORA_WEBKIT_INIT_HEIGHT;
    attr.wclass = AURORA_GDK_INPUT_OUTPUT;
    st->container = api.window_new(st->foreign_win, &attr, AURORA_GDK_WA_X | AURORA_GDK_WA_Y);
    if (st->container == nullptr) {
        return false;
    }
    st->gtk_win = api.window_new_gtk(AURORA_GTK_WINDOW_TOPLEVEL);
    if (st->gtk_win == nullptr) {
        return false;
    }
    api.set_has_window(st->gtk_win, 1);
    if (connect_signal(api, st->gtk_win, "realize", reinterpret_cast<GCallback>(&on_gtk_win_realize), st) == nullptr) {
        return false;
    }
    api.widget_realize(st->gtk_win);
    // realize 回调（同步触发）应已把 container 换成 win 的 window；未成则收养失败。
    return api.widget_get_window(st->gtk_win) == st->container;
}

[[nodiscard]] auto build_web_view(const WebKitGtkApi &api, const std::shared_ptr<WebKitGtkState> &st) -> bool {
    st->ucm = api.ucm_new();
    if (st->ucm == nullptr) {
        return false;
    }
    // 官方顺序：先连 script-message-received 再注册 handler（4.1 要求）。
    if (connect_signal(api, st->ucm, "script-message-received", reinterpret_cast<GCallback>(&on_script_message), st) ==
        nullptr) {
        return false;
    }
    api.ucm_register_handler(st->ucm, "aurora");
    void *user_script = api.user_script_new(std::string(AURORA_WEBKIT_SHIM).c_str(), AURORA_WEBKIT_INJECT_ALL_FRAMES,
                                            AURORA_WEBKIT_INJECT_DOC_START, nullptr, nullptr);
    if (user_script == nullptr) {
        return false;
    }
    api.ucm_add_script(st->ucm, user_script);
    api.user_script_unref(user_script);  // WebKitUserScript 非 GObject，须专用 unref

    st->web_view = api.web_view_new_with_ucm(st->ucm);
    if (st->web_view == nullptr) {
        return false;
    }
    // hexpand/vexpand 必设：否则 webview 自然请求 0x0，快照模式分到 400x1（实测）。
    api.set_hexpand(st->web_view, 1);
    api.set_vexpand(st->web_view, 1);
    api.container_add(st->gtk_win, st->web_view);
    // 仅标记 web_view 可见（布局用）；顶层 gtk_win 刻意不 shown —— 视图缺省不可见，
    // 可见性由宿主经 set_native_visible 授予（交互模式 show 顶层触发整树 map，快照模式
    // 永不调用 ⇒ 恒离屏）。若此处 show 顶层，快照模式也会被映射上屏，违背契约。
    api.widget_show(st->web_view);
    const GdkRectangle al{.x = 0, .y = 0, .width = AURORA_WEBKIT_INIT_WIDTH, .height = AURORA_WEBKIT_INIT_HEIGHT};
    api.size_allocate(st->gtk_win, &al);
    api.window_move_resize(st->container, 0, 0, AURORA_WEBKIT_INIT_WIDTH, AURORA_WEBKIT_INIT_HEIGHT);
    api.display_flush(st->display);
    return true;
}

void connect_view_signals(const WebKitGtkApi &api, const std::shared_ptr<WebKitGtkState> &st) {
    connect_signal(api, st->web_view, "load-changed", reinterpret_cast<GCallback>(&on_load_changed), st);
    connect_signal(api, st->web_view, "load-failed", reinterpret_cast<GCallback>(&on_load_failed), st);
    connect_signal(api, st->web_view, "web-process-terminated", reinterpret_cast<GCallback>(&on_process_terminated),
                   st);
    connect_signal(api, st->web_view, "notify::title", reinterpret_cast<GCallback>(&on_title_notify), st);
}

auto WebKitGtkEngine::create_view(void *parent_window) -> std::unique_ptr<WebViewSink> {
    if (parent_window == nullptr || api_ == nullptr) {
        return nullptr;
    }
    // parent 的 Linux 语义 = X11 Window（宿主 XID）；由 WindowHost 经 bind_window 传入。
    const unsigned long host_xid = static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(parent_window));
    auto state = std::make_shared<WebKitGtkState>();
    state->api = api_;
    if (!adopt_window(*api_, state, host_xid)) {
        teardown_view(*api_, state);
        return nullptr;
    }
    if (!build_web_view(*api_, state)) {
        teardown_view(*api_, state);
        return nullptr;
    }
    connect_view_signals(*api_, state);
    live_states_.push_back(state);  // 引擎心跳据此扫在途快照（超时兜底）
    return std::make_unique<WebKitGtkSink>(std::move(state));
}

[[nodiscard]] auto WebKitGtkEngine::poll_watches() const -> std::vector<WatchFd> {
    std::vector<WatchFd> out;
    if (api_ == nullptr || api_->main_context_query == nullptr) {
        return out;
    }
    const WebKitGtkApi &api = *api_;
    // 旧式两段调用：先问所需条目数（传空缓冲），再取数组。第三参是 timeout 出参、
    // 返回才是 fd 数（探针实测）。
    gint timeout = 0;
    const gint needed = api.main_context_query(nullptr, 0, &timeout, nullptr, 0);
    if (needed <= 0) {
        return out;
    }
    std::vector<GPollFD> buf(static_cast<std::size_t>(needed));
    const gint got = api.main_context_query(nullptr, 0, &timeout, buf.data(), needed);
    const gint count = got > 0 ? (got < needed ? got : needed) : needed;
    out.reserve(static_cast<std::size_t>(count));
    for (gint i = 0; i < count; ++i) {
        if (buf[static_cast<std::size_t>(i)].fd < 0) {
            continue;  // 无效条目（宿主跳过）
        }
        WatchFd w;
        w.fd = buf[static_cast<std::size_t>(i)].fd;
        w.readable = (buf[static_cast<std::size_t>(i)].events & AURORA_G_IO_IN) != 0;
        w.writable = (buf[static_cast<std::size_t>(i)].events & AURORA_G_IO_OUT) != 0;
        out.push_back(w);
    }
    return out;
}

}  // namespace

auto create_webkitgtk_engine() -> std::shared_ptr<NativeViewEngine> {
    std::optional<WebKitGtkApi> api = resolve_api();
    if (!api.has_value()) {
        return nullptr;
    }
    // ABI 表交 shared_ptr：视图状态也持一份，引擎撤装 / 析构后迟到回调仍安全解引用。
    return std::make_shared<WebKitGtkEngine>(std::make_shared<const WebKitGtkApi>(std::move(*api)));
}

}  // namespace aurora::webview

// NOLINTEND(*-pro-type-*)

#endif  // AURORA_ENABLE_WEBVIEW_WEBKITGTK
