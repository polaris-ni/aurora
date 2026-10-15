/* WebKitGTK WebView —— Linux/X11 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖对象：`src/aurora/webview/engine_webkitgtk.cpp`（GTK3 + WebKitGTK 4.1，
// 自声明 ABI + dlopen 运行期绑定）经 `NativeViewHost` 宿主接线的**端到端**行为。
//
// 为什么必须真机：引擎的全部实质都在与真实 GTK/WebKit/X 栈的接缝上——
//   * 收养外部 X 窗口（foreign 包装 → CHILD container → GtkWindow 换窗）只能在
//     真实 X server 上成立（无头 CI 没有 X，Wayland 下该机制不成立）；
//   * 页面加载 / 标题 / 脚本求值 / 双向消息通道依赖真实 WebKit 渲染进程；
//   * 几何同步的终点是 **X server 里子窗口的真实几何**，只能由 X 协议读回；
//   * GLib 主上下文 fd 并入帧循环的接线（`Surface::wait_events` + pump）只能
//     由「异步回调真的抵达」证明——没有任何单测能替代。
// 单测覆盖的（本探针不重复）：`utest_web_view`（控件层降级/挂载/回调门/序列化）
// 与 `utest_native_view_host`（几何换算纯函数/去重/可见性翻转，桩引擎）。
//
// 探针原理（单一真实窗口，两个 WebView 同窗）：
//   1) 强制 X11 路径（清 WAYLAND_DISPLAY）后经 `Application` 建真实窗口——
//      WebView 需要 `WindowHost` 的原生子视图书架在场，故必须走 Application
//      （裸 `create_native_window` 不装宿主，控件会走 host-unbound 降级）；
//   2) 场景根为 Column{ 交互模式 WebView, 快照模式 WebView }：交互视图拿原生
//      可见性，快照视图只作像素来源（宿主恒不授予原生可见）；
//   3) 内联 HTML 页（file:// 落盘后导航）自带：确定标题、确定背景色、
//      DOMContentLoaded 时经 shim 向 Native 投一条消息、onmessage 反射global；
//   4) 帧循环由 `Application::run` 驱动（on_frame 里跑状态机）——异步证据
//      （导航态/标题/消息/求值结果/快照像素）只在循环推进后才抵达，这本身就是
//      「引擎 fd 已并入等待」的接线证明；
//   5) 观测侧用**独立 X 连接**读 X server 真相：子窗口数量、映射态、几何，
//      与 aurora 自己的 `to_native_rect(window_bounds())` 预期值闭环比对；
//   6) 运行期改交互视图的尺寸意图（width/height）→ 布局重排 → 几何再下发，
//      再次闭环比对（证明同步链路随布局变化真下发，而非仅首帧一次）；
//   7) 收尾：进程正常退出即 teardown 无恙（GTK 双销毁会 SIGSEGV，见引擎头注）。
//
// 构建：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_X11=ON -DAURORA_BUILD_VERIFY_TOOLS=ON \
//         -DAURORA_ENABLE_WEBVIEW=ON -DAURORA_ENABLE_WEBVIEW_WEBKITGTK=ON
//   cmake --build build-verify --target aurora_verify_webview_webkitgtk
//
// 运行（需 X11 显示；WSLg 下 XWayland 即 DISPLAY=:0）：
//   DISPLAY=:0 ./build-verify/aurora_verify_webview_webkitgtk
//   DISPLAY=:0 ./build-verify/aurora_verify_webview_webkitgtk --interactive[=秒]
//
// 自动段验收项（逐项打 `RES|pass|…` / `RES|fail|…` 行，末行汇总）：
//   engine-selected            —— 懒安装挑中的引擎是 WebKitGtk 且 Available
//   views-not-degraded         —— 两个视图均未走降级（建视图 + 登记成功）
//   nav-loading-then-finished  —— 交互视图导航态 Loading → Finished（无 Failed）
//   doc-title                  —— 标题回调收到文档标题（与 evaluate 读回一致）
//   eval-ready-and-title       —— evaluate_script 两次布尔往返均 true
//   page-to-native-message     —— 页面经 shim 投出的 JSON 抵达 on_web_message
//   native-to-page-message     —— post_json 经 shim _receive 抵达页面 onmessage
//   x11-child-windows          —— 宿主窗下出现两个 X 子窗口（两视图各一）
//   x11-interactive-mapped     —— 交互视图子窗 IsViewable 且几何等于预期矩形
//   x11-snapshot-unmapped      —— 快照视图子窗未映射（宿主不授予原生可见）
//   x11-geometry-follows-resize—— 改尺寸意图后子窗几何随新预期矩形更新
//   snapshot-bitmap            —— 快照位图非空、尺寸正确、背景色为主、有内容像素
//
// 退出码：
//   0  全部验收项通过 —— 引擎与宿主接线在真机成立
//   2  环境不可用（无 DISPLAY / 无 X 连接 / 建窗失败或回退无头）
//   3  探针无法触及目标（引擎不是 WebKitGtk / 视图意外降级 / 视图未建出）
//   5  部分验收项不符 —— 见逐行 fail 表
//
// 注意：
//   * 本探针**强制 X11**（清 WAYLAND_DISPLAY）：Wayland 下 GTK 收养机制不成立，
//     引擎按设计报 UnsupportedPlatform；WSLg 混合环境（Wayland + XWayland）下
//     若 Aurora 窗口走 Wayland 后端，引擎会因 GTK 能初始化 X11 而报 Available，
//     但拿到的父句柄是 wl_surface——该组合属已知未覆盖面（见提案「限制」节），
//     故探针只在 X11 路径上验收。
//   * 窗口以 NoActivate 建（不抢焦点）；退出即销毁，不留桌面残留。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_webview_webkitgtk can only be built on Linux/Unix (non-Apple)"
#endif
#ifndef AURORA_BACKEND_X11
#error "AURORA_BACKEND_X11 must be enabled"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/core/dimension.h"
#include "aurora/core/image.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/node.h"
#include "aurora/webview/native_view_host.h"
#include "aurora/webview/web_view.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "verify_args.h"
#include "verify_print.h"

// Xlib 必须后于全部 aurora 头：其 `#define None 0L` 会污染 aurora 侧以 None 为
// 枚举成员的声明（同 x11_cursor_live_probe 的口径）。
// clang-format off
#include <X11/Xlib.h>
// clang-format on

namespace {

constexpr auto AURORA_PROBE_TITLE = "aurora-verify-webview-webkitgtk";
constexpr auto AURORA_PROBE_DOC_TITLE = "Aurora WebView probe";

/// 页面背景色（快照像素判据的确定值）：#1F4E79。
constexpr std::uint8_t AURORA_PROBE_BG_R = 31;
constexpr std::uint8_t AURORA_PROBE_BG_G = 78;
constexpr std::uint8_t AURORA_PROBE_BG_B = 121;

/// 交互视图初始 / 改档后的尺寸意图（dp）：改档驱动几何再下发。
constexpr float AURORA_PROBE_VIEW_W0 = 300.0F;
constexpr float AURORA_PROBE_VIEW_H0 = 220.0F;
constexpr float AURORA_PROBE_VIEW_W1 = 380.0F;
constexpr float AURORA_PROBE_VIEW_H1 = 260.0F;

/// 各阶段墙钟上限（毫秒）。帧循环是忙转（实测 ~13k fps）⇒ **帧预算对异步等待无意义**
/// （WebKit IPC / GDK map / X 往返都以墙钟计），故一律以墙钟判，避免按帧早早误判超时。
constexpr int AURORA_PROBE_BOOT_MS = 30000;      ///< 挂载 + 能力落定 + 导航起
constexpr int AURORA_PROBE_NAV_MS = 40000;       ///< 导航 + 标题（WebKit IPC）
constexpr int AURORA_PROBE_SCRIPT_MS = 20000;    ///< 两次 evaluate 往返
constexpr int AURORA_PROBE_MESSAGE_MS = 20000;   ///< 两侧消息通道
constexpr int AURORA_PROBE_RESIZE_MS = 300;      ///< 改档后重排 + 再同步（纯本地布局，短墙钟即可）
constexpr int AURORA_PROBE_SNAPSHOT_MS = 30000;  ///< 快照异步捕获
/// 页侧反射求值的重试间隔（ms）：postMessage → 页 onmessage → 反射是异步的，须轮询。
constexpr int AURORA_PROBE_REFLECT_INTERVAL_MS = 120;

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 验收结果行（状态 / 项 / 明细），末了统一打表。
struct ResLine {
    std::string status;  ///< pass / fail
    std::string id;
    std::string detail;
};

/// @brief 结果累积器（探针主体唯一输出面）。
class Results {
  public:
    auto add(bool ok, std::string id, std::string detail) -> void {
        lines_.push_back(ResLine{.status = ok ? "pass" : "fail", .id = std::move(id), .detail = std::move(detail)});
    }
    auto info(std::string id, std::string detail) -> void {
        lines_.push_back(ResLine{.status = "info", .id = std::move(id), .detail = std::move(detail)});
    }
    [[nodiscard]] auto failures() const -> int {
        int n = 0;
        for (const ResLine &r : lines_) {
            if (r.status == "fail") {
                ++n;
            }
        }
        return n;
    }
    auto dump() const -> void {
        emit(aurora_verify::pad_right("status", 8) + aurora_verify::pad_right("check", 30) + "detail");
        for (const ResLine &r : lines_) {
            emit(aurora_verify::pad_right(r.status, 8) + aurora_verify::pad_right(r.id, 30) + r.detail);
        }
    }

  private:
    std::vector<ResLine> lines_;
};

/// @brief 探针页 HTML：确定标题 / 确定背景色 / DOMContentLoaded 投递一条消息 /
///        onmessage 把收到的载荷反射进 `window.__last_native`（供求值读回）。
[[nodiscard]] auto probe_page_html() -> std::string {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Aurora WebView probe</title>
<style>
  html, body { margin: 0; padding: 0; width: 100%; height: 100%; background: #1F4E79; }
  #tag { color: #FFFFFF; font: 14px sans-serif; padding: 8px; }
</style>
</head>
<body>
<div id="tag">aurora-webkitgtk-probe</div>
<script>
  window.__ready = false;
  window.__last_native = null;
  window.aurora.onmessage = function (d) { window.__last_native = JSON.stringify(d); };
  document.addEventListener('DOMContentLoaded', function () {
    window.__ready = true;
    window.aurora.postMessage({ hello: 'from-page', seq: 1, tag: 'aurora-webkitgtk-probe' });
  });
</script>
</body>
</html>
)HTML";
}

/// @brief 独立 X 连接（观测侧）：RAII 收尾，与 aurora 自己的 X 连接互不干扰。
class XConnection {
  public:
    explicit XConnection(const char *display_name) : dpy_(::XOpenDisplay(display_name)) {}
    ~XConnection() {
        if (dpy_ != nullptr) {
            ::XCloseDisplay(dpy_);
        }
    }
    XConnection(const XConnection &) = delete;
    auto operator=(const XConnection &) -> XConnection & = delete;
    XConnection(XConnection &&) = delete;
    auto operator=(XConnection &&) -> XConnection & = delete;

    [[nodiscard]] auto get() const -> Display * { return dpy_; }
    [[nodiscard]] auto ok() const -> bool { return dpy_ != nullptr; }

  private:
    Display *dpy_;
};

/// @brief 一个 X 子窗口的观测事实（几何为 root 相对，映射态读自属性）。
struct ChildInfo {
    ::Window xid = 0;
    int x = 0;
    int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    bool mapped = false;
};

/// @brief 列宿主窗的全部子窗（XQueryTree 保序返回，几何与映射态逐个补齐）。
[[nodiscard]] auto query_children(Display *dpy, ::Window host) -> std::vector<ChildInfo> {
    std::vector<ChildInfo> out;
    ::Window root = 0;
    ::Window parent = 0;
    ::Window *children = nullptr;
    unsigned int nchildren = 0;
    if (::XQueryTree(dpy, host, &root, &parent, &children, &nchildren) == 0 || children == nullptr) {
        return out;
    }
    for (unsigned int i = 0; i < nchildren; ++i) {
        ChildInfo ci;
        ci.xid = children[i];
        ::Window child_root = 0;
        int px = 0;
        int py = 0;
        unsigned int pw = 0;
        unsigned int ph = 0;
        unsigned int border = 0;
        unsigned int depth = 0;
        if (::XGetGeometry(dpy, children[i], &child_root, &px, &py, &pw, &ph, &border, &depth) != 0) {
            ci.x = px;
            ci.y = py;
            ci.width = pw;
            ci.height = ph;
        }
        XWindowAttributes attrs{};
        if (::XGetWindowAttributes(dpy, children[i], &attrs) != 0) {
            ci.mapped = attrs.map_state == IsViewable;
        }
        out.push_back(ci);
    }
    ::XFree(children);
    return out;
}

/// @brief 两个物理矩形是否在容差内相等（预期 vs X server 实测）。
[[nodiscard]] auto rect_close(const aurora::webview::NativeViewRect &expected, const ChildInfo &actual, int tol) -> bool {
    return std::abs(expected.x - actual.x) <= tol && std::abs(expected.y - actual.y) <= tol &&
           std::abs(expected.width - static_cast<int>(actual.width)) <= tol &&
           std::abs(expected.height - static_cast<int>(actual.height)) <= tol;
}

[[nodiscard]] auto child_text(const ChildInfo &c) -> std::string {
    return "xid=" + aurora_verify::format_hex(static_cast<std::uint64_t>(c.xid)) + " geom=(" +
           aurora_verify::format_int(c.x) + "," + aurora_verify::format_int(c.y) + " " +
           aurora_verify::format_uint(c.width) + "x" + aurora_verify::format_uint(c.height) + ") mapped=" +
           (c.mapped ? "yes" : "no");
}

[[nodiscard]] auto expected_text(const aurora::webview::NativeViewRect &r) -> std::string {
    return "(" + aurora_verify::format_int(r.x) + "," + aurora_verify::format_int(r.y) + " " +
           aurora_verify::format_int(r.width) + "x" + aurora_verify::format_int(r.height) + ")";
}

/// @brief 帧循环驱动的阶段状态机（语义化阶段名，不用编号标记）。
enum class Stage {
    Boot,         ///< 等挂载与能力落定
    Navigate,     ///< 等交互视图导航态与标题
    Script,       ///< evaluate 两次布尔往返
    PageToNative, ///< 等页面经 shim 投出的消息
    NativeToPage, ///< post_json 后等页面反射值
    Geometry,     ///< X 子窗口证据 + 预期矩形闭环比对
    Resize,       ///< 改尺寸意图 → 几何再下发 → 再次闭环比对
    Snapshot,     ///< 快照请求 → 像素判据
    Done,         ///< 收尾
};

/// @brief 探针主体状态（帧回调与 main 共享；全部主线程内访问）。
struct Probe {
    Results results;

    // ---- 异步证据（回调置位；状态机轮询）----
    bool nav_loading = false;
    bool nav_finished = false;
    bool nav_failed = false;
    std::string nav_failed_detail;
    bool title_seen = false;
    std::string title_value;
    bool page_message_seen = false;
    std::string page_message;
    bool eval_ready_done = false;
    std::string eval_ready_value;
    bool eval_title_done = false;
    std::string eval_title_value;
    bool eval_reflect_done = false;
    std::string eval_reflect_value;
    bool snapshot_done = false;
    aurora::Image snapshot_bitmap;

    // ---- 控件与窗口 ----
    std::shared_ptr<aurora::webview::WebView> interactive;
    std::shared_ptr<aurora::webview::WebView> snapshot_view;
    aurora::Window *window = nullptr;
    int frames = 0;
    Stage stage = Stage::Boot;
    bool stage_entered = false;
    std::chrono::steady_clock::time_point stage_start = std::chrono::steady_clock::now();
    int reflect_last_ms = -1;  ///< 上次发起反射求值的阶段相对毫秒（轮询节流用；-1=尚未发起）

    /// @brief 本阶段已消耗的墙钟毫秒（阶段由 `reset_stage()` 起算）。
    [[nodiscard]] auto stage_waited_ms() const -> int {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stage_start)
                .count());
    }

    // ---- 几何阶段的中间量 ----
    std::vector<ChildInfo> children_before;
    aurora::webview::NativeViewRect expected_before{};
    bool have_expected_before = false;

    auto reset_stage() -> void {
        stage_entered = false;
        stage_start = std::chrono::steady_clock::now();
        reflect_last_ms = -1;
    }
};

/// @brief 页面→Native 消息的载荷断言（键存在且值正确，不整串相等以防格式漂移）。
[[nodiscard]] auto page_message_ok(const std::string &json) -> bool {
    return json.find("from-page") != std::string::npos && json.find("\"seq\":1") != std::string::npos &&
           json.find("aurora-webkitgtk-probe") != std::string::npos;
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto spec = aurora::cli::CommandSpec{
        .name = "aurora_verify_webview_webkitgtk",
        .about = "WebKitGTK WebView live probe (Linux/X11)",
        .options = {aurora::cli::OptionSchema{.long_name = "interactive",
                                              .kind = aurora::cli::ValueKind::Int,
                                              .arity = aurora::cli::Arity::optional_one(),
                                              .help = "Stay alive for manual inspection after the automatic stage",
                                              .value_hint = "SECONDS",
                                              .default_text = "30",
                                              .minimum = 1}}};
    const auto cli = aurora_verify::parse_command_line(spec, argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    int keep_seconds = 0;
    if (cli.arguments->explicitly_given("interactive")) {
        const auto seconds = cli.arguments->get<int>("interactive");
        keep_seconds = seconds.ok() ? seconds.value() : 30;
    }

    // ---- 环境前置：强制 X11 路径（清 WAYLAND_DISPLAY ⇒ 工厂选 X11/XWayland）----
    ::unsetenv("WAYLAND_DISPLAY");
    if (::getenv("DISPLAY") == nullptr) {
        AURORA_LOG_ERROR("verify", "No DISPLAY -- this probe needs an X11 display (WSLg: DISPLAY=:0)");
        return 2;
    }

    // ---- 建真实窗口（Application 路径：WebView 需要 WindowHost 的原生子视图书架）----
    aurora::WindowOptions opts;
    opts.size = aurora::Size{.width = 560.0F, .height = 640.0F};
    opts.title = AURORA_PROBE_TITLE;
    opts.visibility = aurora::WindowVisibility::NoActivate;  // 可见但不抢焦点
    auto made = aurora::create_native_window(opts);
    if (!made) {
        AURORA_LOG_ERROR("verify", "create_native_window failed: " + made.error().message);
        return 2;
    }
    auto window_owner = std::move(made.value());
    if (window_owner->surface().native_handle() == nullptr) {
        AURORA_LOG_ERROR("verify", "Surface fell back to Headless (no X11 backend for this session)");
        return 2;
    }

    // ---- 场景：Column{ 交互视图, 快照视图 }（两者同窗，快照视图永不原生可见）----
    Probe probe;
    probe.interactive = std::make_shared<aurora::webview::WebView>(aurora::webview::WebViewProps{
        .url = {}, .html = {}, .mode = aurora::webview::WebViewMode::Interactive, .javascript_enabled = true});
    probe.snapshot_view = std::make_shared<aurora::webview::WebView>(aurora::webview::WebViewProps{
        .url = {}, .html = {}, .mode = aurora::webview::WebViewMode::Snapshot, .javascript_enabled = true});
    // 初始尺寸意图显式化：改档判据（Resize 阶段）据此产生确定增量。
    probe.interactive->width(aurora::px(AURORA_PROBE_VIEW_W0)).height(aurora::px(AURORA_PROBE_VIEW_H0));
    probe.snapshot_view->width(aurora::px(AURORA_PROBE_VIEW_W0)).height(aurora::px(AURORA_PROBE_VIEW_H0));

    // 页内消息与导航态：控件自持 shared_ptr 回调聚合，挂载前设置同样有效。
    probe.interactive->set_on_navigation([&probe](std::string_view uri, aurora::webview::WebViewNavigationState state) {
        if (state == aurora::webview::WebViewNavigationState::Loading) {
            probe.nav_loading = true;
        } else if (state == aurora::webview::WebViewNavigationState::Finished) {
            probe.nav_finished = true;
        } else {
            probe.nav_failed = true;
            probe.nav_failed_detail = uri;
        }
    });
    probe.interactive->set_on_title_changed([&probe](std::string title) {
        probe.title_seen = true;
        probe.title_value = std::move(title);
    });
    probe.interactive->set_on_web_message([&probe](std::string json) {
        probe.page_message_seen = true;
        probe.page_message = std::move(json);
    });
    probe.interactive->set_on_process_failed([&probe](std::string detail) {
        probe.nav_failed = true;
        probe.nav_failed_detail = std::move(detail);
    });

    aurora::Node root = aurora::Node{aurora::Column{aurora::Node{probe.interactive}, aurora::Node{probe.snapshot_view}}};
    aurora::Scene scene{std::move(root)};
    aurora::Application app{std::move(scene), std::move(window_owner), opts};
    probe.window = app.window();

    // ---- 落盘页并导航（file://：真实文档加载路径，FINISHED 语义完整）----
    const std::string page_path = "/tmp/aurora_verify_webview_webkitgtk_" + std::to_string(::getpid()) + ".html";
    {
        auto written = std::ofstream(page_path, std::ios::trunc);
        written << probe_page_html();
        if (!written) {
            AURORA_LOG_ERROR("verify", "Cannot write probe page to " + page_path);
            return 4;
        }
    }
    const std::string page_url = "file://" + page_path;

    // ---- 阶段状态机（每帧推进；异步证据只在循环推进后抵达）----
    auto enter_stage = [&probe](Stage s) {
        probe.stage = s;
        probe.reset_stage();
    };

    app.set_on_frame([&]() -> void {
        ++probe.frames;
        switch (probe.stage) {
            case Stage::Boot: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    // 挂载发生在 present_root 的树重建里：此刻两个视图已登记（或已降级）。
                    const aurora::webview::WebViewCapabilities caps = probe.interactive->capabilities();
                    probe.results.add(caps.engine == aurora::webview::WebViewEngine::WebKitGtk &&
                                          caps.availability == aurora::webview::WebViewAvailability::Available &&
                                          caps.interactive && caps.snapshot,
                                      "engine-selected",
                                      std::string("engine=") + (caps.engine == aurora::webview::WebViewEngine::WebKitGtk
                                                                   ? "WebKitGtk"
                                                                   : "not-WebKitGtk") +
                                          " availability=" + std::to_string(static_cast<int>(caps.availability)));
                    if (probe.interactive->degraded() || probe.snapshot_view->degraded()) {
                        probe.results.add(false, "views-not-degraded",
                                          std::string("interactive.degraded=") +
                                              (probe.interactive->degraded() ? "true" : "false") +
                                              " snapshot.degraded=" +
                                              (probe.snapshot_view->degraded() ? "true" : "false"));
                        enter_stage(Stage::Done);
                        return;
                    }
                    probe.results.add(true, "views-not-degraded", "both views registered with the host");
                    // 首帧挂载即发起初始导航（on_mount 的序：set_callbacks → zoom → js → navigate）。
                    probe.interactive->load_url(page_url);
                    probe.snapshot_view->load_url(page_url);
                }
                if (probe.nav_loading) {
                    enter_stage(Stage::Navigate);
                    return;
                }
                // 墙钟上限：WSLg 下 WebKit WebProcess 冷启动可能数秒，忙转帧数不反映墙钟。
                if (probe.stage_waited_ms() > AURORA_PROBE_BOOT_MS) {
                    probe.results.add(false, "nav-loading-then-finished",
                                      "no Loading state within boot wall-clock budget (view never navigated)");
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::Navigate: {
                if (probe.nav_failed) {
                    probe.results.add(false, "nav-loading-then-finished",
                                      "navigation reported Failed: " + probe.nav_failed_detail);
                    enter_stage(Stage::Done);
                    return;
                }
                if (probe.nav_finished && probe.title_seen) {
                    const bool title_ok = probe.title_value == AURORA_PROBE_DOC_TITLE;
                    probe.results.add(title_ok, "nav-loading-then-finished",
                                      "Loading -> Finished, title=\"" + probe.title_value + "\"");
                    enter_stage(Stage::Script);
                    return;
                }
                if (probe.stage_waited_ms() > AURORA_PROBE_NAV_MS) {
                    probe.results.add(false, "nav-loading-then-finished",
                                      std::string("timeout: loading=") + (probe.nav_loading ? "1" : "0") +
                                          " finished=" + (probe.nav_finished ? "1" : "0") + " title=" +
                                          (probe.title_seen ? "1" : "0"));
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::Script: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    probe.interactive->evaluate_script("window.__ready === true",
                                                       [&probe](std::string v) {
                                                           probe.eval_ready_done = true;
                                                           probe.eval_ready_value = std::move(v);
                                                       });
                    probe.interactive->evaluate_script(
                        std::string("document.title === '") + AURORA_PROBE_DOC_TITLE + "'",
                        [&probe](std::string v) {
                            probe.eval_title_done = true;
                            probe.eval_title_value = std::move(v);
                        });
                }
                if (probe.eval_ready_done && probe.eval_title_done) {
                    const bool ok = probe.eval_ready_value == "true" && probe.eval_title_value == "true";
                    probe.results.add(ok, "eval-ready-and-title",
                                      "ready=" + probe.eval_ready_value + " title=" + probe.eval_title_value);
                    enter_stage(Stage::PageToNative);
                    return;
                }
                if (probe.stage_waited_ms() > AURORA_PROBE_SCRIPT_MS) {
                    probe.results.add(false, "eval-ready-and-title", "timeout waiting for evaluate results");
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::PageToNative: {
                if (probe.page_message_seen) {
                    probe.results.add(page_message_ok(probe.page_message), "page-to-native-message",
                                      "payload=" + probe.page_message);
                    enter_stage(Stage::NativeToPage);
                    return;
                }
                if (probe.stage_waited_ms() > AURORA_PROBE_MESSAGE_MS) {
                    probe.results.add(false, "page-to-native-message",
                                      "timeout: page never posted via window.aurora.postMessage");
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::NativeToPage: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    probe.interactive->post_json(R"({"native":true,"seq":7})");
                }
                // 反射求值是异步的（postMessage → 页 onmessage → window.__last_native 更新 → 求值
                // → 结果回调），故**轮询**重发而非按帧一次性触发：帧率不受限时「第 20 帧」只有毫秒级，
                // 早于页侧更新即读到空值。成功（=true）或墙钟超时才收敛。
                const int waited = probe.stage_waited_ms();
                const bool due = probe.reflect_last_ms < 0 || waited - probe.reflect_last_ms >= AURORA_PROBE_REFLECT_INTERVAL_MS;
                if (!probe.eval_reflect_done && due) {
                    probe.reflect_last_ms = waited;
                    probe.eval_reflect_done = false;
                    probe.interactive->evaluate_script(R"(window.__last_native === '{"native":true,"seq":7}')",
                                                       [&probe](std::string v) {
                                                           probe.eval_reflect_done = true;
                                                           probe.eval_reflect_value = std::move(v);
                                                       });
                }
                if (probe.eval_reflect_done) {
                    if (probe.eval_reflect_value == "true") {
                        probe.results.add(true, "native-to-page-message",
                                          "page reflected: " + probe.eval_reflect_value);
                        enter_stage(Stage::Geometry);
                        return;
                    }
                    // 反射尚未落定（读到 false/空）：清标志，下一轮继续轮询。
                    probe.eval_reflect_done = false;
                }
                if (waited > AURORA_PROBE_MESSAGE_MS) {
                    probe.results.add(false, "native-to-page-message", "timeout waiting for reflection evaluate");
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::Geometry: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    aurora::Surface &surface = probe.window->surface();
                    const float scale = surface.scale_factor();
                    const aurora::EdgeInsets inset = surface.content_inset();
                    // 宿主 X 句柄：X11Surface 契约 = XID 存于 void*（只作身份，不解引用）。
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
                    const auto host_xid = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(
                        probe.window->surface().native_handle()));
                    XConnection obs(nullptr);
                    if (!obs.ok()) {
                        probe.results.add(false, "x11-child-windows", "observation-side XOpenDisplay failed");
                        enter_stage(Stage::Done);
                        return;
                    }
                    probe.children_before = query_children(obs.get(), host_xid);
                    probe.results.add(probe.children_before.size() >= 2, "x11-child-windows",
                                      "host child count=" + aurora_verify::format_uint(probe.children_before.size()));
                    for (const ChildInfo &c : probe.children_before) {
                        probe.results.info("x11-child", child_text(c));
                    }

                    // 预期矩形：aurora 自己的换算（窗口逻辑盒 × scale + CSD 内缩）。
                    const std::optional<aurora::Rect> ib = probe.interactive->window_bounds();
                    const std::optional<aurora::Rect> sb = probe.snapshot_view->window_bounds();
                    if (!ib.has_value() || !sb.has_value()) {
                        probe.results.add(false, "x11-interactive-mapped", "window_bounds unavailable after layout");
                        enter_stage(Stage::Done);
                        return;
                    }
                    const aurora::webview::NativeViewRect expected_i = aurora::webview::to_native_rect(*ib, scale, inset);
                    const aurora::webview::NativeViewRect expected_s = aurora::webview::to_native_rect(*sb, scale, inset);
                    probe.expected_before = expected_i;
                    probe.have_expected_before = true;
                    probe.results.info("expected-interactive", expected_text(expected_i));
                    probe.results.info("expected-snapshot", expected_text(expected_s));

                    const ChildInfo *match_i = nullptr;
                    const ChildInfo *match_s = nullptr;
                    for (const ChildInfo &c : probe.children_before) {
                        if (!match_i && rect_close(expected_i, c, 2)) {
                            match_i = &c;
                        } else if (!match_s && rect_close(expected_s, c, 2)) {
                            match_s = &c;
                        }
                    }
                    probe.results.add(match_i != nullptr && match_i->mapped, "x11-interactive-mapped",
                                      match_i == nullptr
                                          ? "no child matches expected rect " + expected_text(expected_i)
                                          : child_text(*match_i));
                    probe.results.add(match_s != nullptr && !match_s->mapped, "x11-snapshot-unmapped",
                                      match_s == nullptr
                                          ? "no child matches expected rect " + expected_text(expected_s)
                                          : child_text(*match_s));
                }
                enter_stage(Stage::Resize);
                return;
            }
            case Stage::Resize: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    // 改交互视图的尺寸意图 → 标脏布局 → 下一帧重排 → 几何再下发。
                    probe.interactive->width(aurora::px(AURORA_PROBE_VIEW_W1))
                        .height(aurora::px(AURORA_PROBE_VIEW_H1));
                }
                if (probe.stage_waited_ms() < AURORA_PROBE_RESIZE_MS) {
                    return;  // 等墙钟让重排与几何同步落定（帧忙转，帧数不代表墙钟）
                }
                aurora::Surface &surface = probe.window->surface();
                const float scale = surface.scale_factor();
                const aurora::EdgeInsets inset = surface.content_inset();
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
                const auto host_xid = static_cast<::Window>(
                    reinterpret_cast<std::uintptr_t>(probe.window->surface().native_handle()));
                XConnection obs(nullptr);
                if (!obs.ok()) {
                    probe.results.add(false, "x11-geometry-follows-resize",
                                      "observation-side XOpenDisplay failed");
                    enter_stage(Stage::Done);
                    return;
                }
                const std::vector<ChildInfo> after = query_children(obs.get(), host_xid);
                const std::optional<aurora::Rect> ib = probe.interactive->window_bounds();
                if (!ib.has_value()) {
                    probe.results.add(false, "x11-geometry-follows-resize", "window_bounds unavailable after resize");
                    enter_stage(Stage::Done);
                    return;
                }
                const aurora::webview::NativeViewRect expected_after = aurora::webview::to_native_rect(*ib, scale, inset);
                probe.results.info("expected-interactive-after", expected_text(expected_after));
                const ChildInfo *match = nullptr;
                for (const ChildInfo &c : after) {
                    if (rect_close(expected_after, c, 2)) {
                        match = &c;
                        break;
                    }
                }
                const bool moved = match != nullptr && probe.have_expected_before &&
                                   std::abs(static_cast<int>(match->width) - probe.expected_before.width) > 4;
                probe.results.add(match != nullptr && moved, "x11-geometry-follows-resize",
                                  match == nullptr
                                      ? "no child matches resized expected rect " + expected_text(expected_after)
                                      : child_text(*match) + " (was " + expected_text(probe.expected_before) + ")");
                enter_stage(Stage::Snapshot);
                return;
            }
            case Stage::Snapshot: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    probe.snapshot_view->request_snapshot([&probe](const aurora::Image &img) {
                        probe.snapshot_done = true;
                        probe.snapshot_bitmap = img;
                    });
                }
                if (probe.snapshot_done) {
                    const aurora::Image &img = probe.snapshot_bitmap;
                    const bool sized = img.width > 0 && img.height > 0 &&
                                       img.pixels.size() == static_cast<std::size_t>(img.width) *
                                                                 static_cast<std::size_t>(img.height) * 4U;
                    if (!sized) {
                        // 空图是引擎按契约兑现的「捕获未产出像素」（`NativeViewSink::capture`
                        // 要求失败也必须回调空图），不是「没收到回调」——后者才是缺陷。
                        probe.results.add(false, "snapshot-bitmap",
                                          std::string("no pixels captured (") +
                                              aurora_verify::format_int(img.width) + "x" +
                                              aurora_verify::format_int(img.height) +
                                              "); the page may not have produced a frame yet");
                        enter_stage(Stage::Done);
                        return;
                    }
                    // 背景色为主（页面铺满视口）+ 存在非背景像素（白色标签文字）。
                    std::size_t bg = 0;
                    std::size_t other = 0;
                    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
                        const int dr = std::abs(static_cast<int>(img.pixels[i + 0]) - AURORA_PROBE_BG_R);
                        const int dg = std::abs(static_cast<int>(img.pixels[i + 1]) - AURORA_PROBE_BG_G);
                        const int db = std::abs(static_cast<int>(img.pixels[i + 2]) - AURORA_PROBE_BG_B);
                        if (dr <= 10 && dg <= 10 && db <= 10) {
                            ++bg;
                        } else {
                            ++other;
                        }
                    }
                    const std::size_t total = bg + other;
                    const bool ok = total > 0 && bg * 100 >= total * 50 && other >= 20;
                    probe.results.add(ok, "snapshot-bitmap",
                                      std::to_string(img.width) + "x" + std::to_string(img.height) + " bg=" +
                                          aurora_verify::format_uint(bg) + "/" + aurora_verify::format_uint(total) +
                                          " other=" + aurora_verify::format_uint(other));
                    enter_stage(Stage::Done);
                    return;
                }
                if (probe.stage_waited_ms() > AURORA_PROBE_SNAPSHOT_MS) {
                    probe.results.add(false, "snapshot-bitmap", "timeout waiting for capture callback");
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::Done:
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    app.quit();
                }
                return;
        }
    });

    app.run();
    std::remove(page_path.c_str());

    // ---- 汇总 ----
    probe.results.dump();
    const int failures = probe.results.failures();
    emit("summary: frames=" + aurora_verify::format_int(probe.frames) + ", failures=" +
         aurora_verify::format_int(failures));
    if (failures > 0) {
        emit("FAILURES PRESENT (exit = 5)");
        return 5;
    }
    emit("ALL PASS -- WebKitGTK engine + host wiring verified on the real X11 stack");
    if (keep_seconds > 0) {
        emit("interactive: keeping the process alive for " + std::to_string(keep_seconds) +
             "s -- inspect the window manually (frame loop already stopped)");
        std::this_thread::sleep_for(std::chrono::seconds(keep_seconds));
    }
    return 0;
}
