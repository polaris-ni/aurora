/* macOS WKWebView WebView —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖对象：`src/aurora/webview/engine_wkwebview.cpp`（系统 WebKit 框架直链，MRC）
// 经 `NativeViewHost` 宿主接线的**端到端**行为。
//
// 为什么必须真机：引擎的全部实质都在与真实 WebKit / AppKit 栈的接缝上——
//   * WKWebView 作为 contentView 子视图的挂入 / 显隐 / 几何只能在真实 AppKit 窗口上成立；
//   * 页面加载 / 标题 / 脚本求值 / 双向消息通道依赖真实 WebContent 进程；
//   * 几何同步的终点是 **NSView 层级里的真实 frame**，只能从 AppKit 读回；
//   * 快照的 takeSnapshot 异步回调与屏外停放手法只能由「像素真的产出」证明。
// 单测覆盖的（本探针不重复）：`utest_web_view`（控件层降级/挂载/回调门/序列化/引擎标识
// 透传）与 `utest_native_view_host`（几何换算纯函数/去重/可见性翻转，桩引擎）。
//
// 探针原理（单一真实窗口，两个 WebView 同窗；与 webkitgtk_webview_live_probe 同构）：
//   1) 经 `Application` 建真实窗口——WebView 需要 `WindowHost` 的原生子视图书架在场，
//      故必须走 Application（裸 `create_native_window` 不装宿主，控件会走 host-unbound 降级）；
//   2) 场景根为 Column{ 交互模式 WebView, 快照模式 WebView }：交互视图拿原生可见性，
//      快照视图只作像素来源（宿主恒不授予原生可见）；
//   3) 内联 HTML 页（file:// 落盘后导航）自带：确定标题、确定背景色、DOMContentLoaded
//      时经 shim 向 Native 投一条消息、onmessage 反射全局变量；
//   4) 帧循环由 `Application::run` 驱动（on_frame 里跑状态机）——异步证据只在循环推进后
//      才抵达，这本身就是「引擎事件经 OS 消息泵分发」的接线证明；
//   5) 观测侧用**独立 AppKit 读回**：从 native_handle() 的 NSWindow.contentView.subviews
//      找 WKWebView 实例，frame（point，superview flipped ⇒ 左上原点）× backingScaleFactor
//      与 aurora 自己的 `to_native_rect(window_bounds())` 预期值闭环比对；
//   6) 运行期改交互视图的尺寸意图（width/height）→ 布局重排 → 几何再下发，轮询到观测
//      几何落定（上限 10s）再闭环比对（证明同步链路随布局变化真下发，而非仅首帧一次）；
//   7) 收尾：进程正常退出即 teardown 无恙（KVO / 消息 handler / 委托的摘除序见引擎头注）。
//
// 构建：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON \
//         -DAURORA_ENABLE_WEBVIEW=ON
//   cmake --build build-macos --target aurora_verify_webview_wkwebview
//
// 运行（需图形会话；无 WindowServer 时以退出码 2 报环境不可用）：
//   ./build-macos/aurora_verify_webview_wkwebview
//   ./build-macos/aurora_verify_webview_wkwebview --interactive[=秒]
//
// 自动段验收项（逐项打 `RES|pass|…` / `RES|fail|…` 行，末行汇总）：
//   engine-selected             —— 懒安装挑中的引擎是 WkWebView 且 Available
//   views-not-degraded          —— 两个视图均未走降级（建视图 + 登记成功）
//   nav-loading-then-finished   —— 交互视图导航态 Loading → Finished（无 Failed），标题一致
//   eval-ready-and-title        —— evaluate_script 两次布尔往返均 true
//   page-to-native-message      —— 页面经 shim 投出的 JSON 抵达 on_web_message
//   native-to-page-message      —— post_json 经 shim _receive 抵达页面 onmessage
//   nsview-child-views          —— 宿主内容视图下出现两个 WKWebView 子视图
//   nsview-interactive-visible  —— 交互视图 hidden=NO 且几何等于预期矩形
//   nsview-snapshot-hidden      —— 快照视图 hidden=YES（宿主不授予原生可见）
//   nsview-geometry-follows-resize —— 改尺寸意图后子视图几何随新预期矩形更新
//   snapshot-bitmap             —— 快照位图非空、尺寸正确、背景色为主、有内容像素
//
// 退出码：
//   0  全部验收项通过 —— 引擎与宿主接线在真机成立
//   2  环境不可用（无图形会话 / 建窗失败或回退无头）
//   5  部分验收项不符 —— 见逐行 fail 表
//
// 注意：
//   * 窗口以 NoActivate 建（不抢焦点）；退出即销毁，不留桌面残留。
//   * 观测只读 NSView 层级（frame / hidden），不经截图证明「屏幕上真的显示了」——
//     与 macos_surface_live_probe 同口径（背景会话下 screencapture 不可用）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_webview_wkwebview can only be built on macOS"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "AURORA_BACKEND_MACOS must be enabled"
#endif
// AURORA_ENABLE_WEBVIEW 是 PRIVATE 宏、不传播到消费端：本探针只在
// `APPLE AND AURORA_BACKEND_MACOS AND AURORA_ENABLE_WEBVIEW` 时注册（AuroraVerify.cmake），
// 故此处不再重复守卫该宏（与 webkitgtk / webview2 两枚探针同口径）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/core/dimension.h"
#include "aurora/core/image.h"
#include "aurora/webview/native_view_host.h"
#include "aurora/webview/web_view.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/node.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "verify_args.h"
#include "verify_print.h"

// AppKit / WebKit 头后于全部 aurora 头（平台头关在单个 TU 内的同款纪律）。
// clang-format off
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
// clang-format on

namespace {

constexpr auto AURORA_PROBE_TITLE = "aurora-verify-webview-wkwebview";
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

/// 各阶段墙钟上限（毫秒）。WebKit IPC 以墙钟计，故一律以墙钟判，避免按帧早早误判超时。
constexpr int AURORA_PROBE_BOOT_MS = 30000;  ///< 挂载 + 能力落定 + 导航起
constexpr int AURORA_PROBE_NAV_MS = 40000;  ///< 导航 + 标题（WebKit IPC）
constexpr int AURORA_PROBE_SCRIPT_MS = 20000;  ///< 两次 evaluate 往返
constexpr int AURORA_PROBE_MESSAGE_MS = 20000;  ///< 两侧消息通道
constexpr int AURORA_PROBE_RESIZE_MS = 10000;  ///< 改档后重排 + 再同步（轮询到几何落定的上限）
constexpr int AURORA_PROBE_SNAPSHOT_MS = 30000;  ///< 快照异步捕获
/// 页侧反射求值的重试间隔（ms）：postMessage → 页 onmessage → 反射是异步的，须轮询。
constexpr int AURORA_PROBE_REFLECT_INTERVAL_MS = 120;

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 验收结果行（状态 / 项 / 明细），末了统一打表。
struct ResLine {
    std::string status;  ///< pass / fail / info
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
        emit(aurora_verify::pad_right("status", 8) + aurora_verify::pad_right("check", 32) + "detail");
        for (const ResLine &r : lines_) {
            emit(aurora_verify::pad_right(r.status, 8) + aurora_verify::pad_right(r.id, 32) + r.detail);
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
<div id="tag">aurora-wkwebview-probe</div>
<script>
  window.__ready = false;
  window.__last_native = null;
  window.aurora.onmessage = function (d) { window.__last_native = JSON.stringify(d); };
  document.addEventListener('DOMContentLoaded', function () {
    window.__ready = true;
    window.aurora.postMessage({ hello: 'from-page', seq: 1, tag: 'aurora-wkwebview-probe' });
  });
</script>
</body>
</html>
)HTML";
}

/// @brief 一个 WKWebView 子视图的观测事实（几何折算为物理像素，左上原点）。
struct ChildInfo {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool hidden = false;
};

/// @brief 列宿主窗口内容视图下的全部 WKWebView 子视图（独立 AppKit 读回）。
/// frame 是 point 且 superview flipped（左上原点），× backingScaleFactor 折算物理像素。
[[nodiscard]] auto query_children(NSWindow *window) -> std::vector<ChildInfo> {
    std::vector<ChildInfo> out;
    if (window == nil || window.contentView == nil) {
        return out;
    }
    const CGFloat scale = window.backingScaleFactor > 0.0 ? window.backingScaleFactor : 1.0;
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables): 快速枚举的循环变量由运行时初始化（同 macos_drag 口径）
    for (NSView *sub in window.contentView.subviews) {
        if ([sub isKindOfClass:[WKWebView class]] == NO) {
            continue;
        }
        const NSRect f = sub.frame;
        ChildInfo ci;
        ci.x = static_cast<int>(std::lround(f.origin.x * scale));
        ci.y = static_cast<int>(std::lround(f.origin.y * scale));
        ci.width = static_cast<int>(std::lround(f.size.width * scale));
        ci.height = static_cast<int>(std::lround(f.size.height * scale));
        ci.hidden = sub.hidden == YES;
        out.push_back(ci);
    }
    return out;
}

/// @brief 两个物理矩形是否在容差内相等（预期 vs AppKit 实测）。
[[nodiscard]] auto rect_close(const aurora::webview::NativeViewRect &expected, const ChildInfo &actual, int tol)
    -> bool {
    return std::abs(expected.x - actual.x) <= tol && std::abs(expected.y - actual.y) <= tol &&
           std::abs(expected.width - actual.width) <= tol && std::abs(expected.height - actual.height) <= tol;
}

[[nodiscard]] auto child_text(const ChildInfo &c) -> std::string {
    return "geom=(" + aurora_verify::format_int(c.x) + "," + aurora_verify::format_int(c.y) + " " +
           aurora_verify::format_int(c.width) + "x" + aurora_verify::format_int(c.height) +
           ") hidden=" + (c.hidden ? "yes" : "no");
}

[[nodiscard]] auto expected_text(const aurora::webview::NativeViewRect &r) -> std::string {
    return "(" + aurora_verify::format_int(r.x) + "," + aurora_verify::format_int(r.y) + " " +
           aurora_verify::format_int(r.width) + "x" + aurora_verify::format_int(r.height) + ")";
}

/// @brief 帧循环驱动的阶段状态机（语义化阶段名，不用编号标记）。
enum class Stage : std::uint8_t {
    Boot,  ///< 等挂载与能力落定
    Navigate,  ///< 等交互视图导航态与标题
    Script,  ///< evaluate 两次布尔往返
    PageToNative,  ///< 等页面经 shim 投出的消息
    NativeToPage,  ///< post_json 后等页面反射值
    Geometry,  ///< NSView 子视图证据 + 预期矩形闭环比对
    Resize,  ///< 改尺寸意图 → 几何再下发 → 轮询落定后再次闭环比对
    Snapshot,  ///< 快照请求 → 像素判据
    Done,  ///< 收尾
};

/// @brief 探针主体状态（帧回调与 main 共享；全部主线程内访问）。
/// 字段序按 clang-analyzer 的最优填充序排列（指针 / 时间戳 / 共享指针在前，标量与布尔殿后）。
struct Probe {
    // ---- 控件与窗口 ----
    aurora::Window *window = nullptr;
    std::chrono::steady_clock::time_point stage_start = std::chrono::steady_clock::now();
    std::shared_ptr<aurora::webview::WebView> interactive;
    std::shared_ptr<aurora::webview::WebView> snapshot_view;
    Results results;

    // ---- 异步证据（回调置位；状态机轮询）----
    std::string nav_failed_detail;
    std::string title_value;
    std::string page_message;
    std::string eval_ready_value;
    std::string eval_title_value;
    std::string eval_reflect_value;
    aurora::Image snapshot_bitmap;
    int frames = 0;
    Stage stage = Stage::Boot;
    int reflect_last_ms = -1;  ///< 上次发起反射求值的阶段相对毫秒（轮询节流用；-1=尚未发起）
    aurora::webview::NativeViewRect expected_before{};  ///< 几何阶段的改档前预期矩形
    bool nav_loading = false;
    bool nav_finished = false;
    bool nav_failed = false;
    bool title_seen = false;
    bool page_message_seen = false;
    bool eval_ready_done = false;
    bool eval_title_done = false;
    bool eval_reflect_done = false;
    bool snapshot_done = false;
    bool stage_entered = false;
    bool have_expected_before = false;

    /// @brief 本阶段已消耗的墙钟毫秒（阶段由 `reset_stage()` 起算）。
    [[nodiscard]] auto stage_waited_ms() const -> int {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stage_start)
                .count());
    }

    auto reset_stage() -> void {
        stage_entered = false;
        stage_start = std::chrono::steady_clock::now();
        reflect_last_ms = -1;
    }
};

/// @brief 页面→Native 消息的载荷断言（键存在且值正确，不整串相等以防格式漂移）。
[[nodiscard]] auto page_message_ok(const std::string &json) -> bool {
    return json.find("from-page") != std::string::npos && json.find("\"seq\":1") != std::string::npos &&
           json.find("aurora-wkwebview-probe") != std::string::npos;
}

/// @brief 取宿主 NSWindow（native_handle 的 macOS 语义；只作观测入口）。
[[nodiscard]] auto host_window(aurora::Window *window) -> NSWindow * {
    // ARC 下 void* → ObjC 指针须 __bridge（不转移所有权；句柄归 Surface 持有）。
    return (__bridge NSWindow *)window->surface().native_handle();
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto spec = aurora::cli::CommandSpec{
        .name = "aurora_verify_webview_wkwebview",
        .about = "WKWebView WebView live probe (macOS)",
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
        AURORA_LOG_ERROR("verify", "Surface fell back to Headless (no graphical session for this process)");
        return 2;
    }

    // ---- 场景：Column{ 交互视图, 快照视图 }（两者同窗，快照视图永不原生可见）----
    Probe probe;
    probe.interactive = std::make_shared<aurora::webview::WebView>(
        aurora::webview::WebViewProps().set_mode(aurora::webview::WebViewMode::Interactive));
    probe.snapshot_view = std::make_shared<aurora::webview::WebView>(
        aurora::webview::WebViewProps().set_mode(aurora::webview::WebViewMode::Snapshot));
    probe.interactive->width(aurora::px(AURORA_PROBE_VIEW_W0)).height(aurora::px(AURORA_PROBE_VIEW_H0));
    probe.snapshot_view->width(aurora::px(AURORA_PROBE_VIEW_W0)).height(aurora::px(AURORA_PROBE_VIEW_H0));

    // 页内消息与导航态：控件自持 shared_ptr 回调聚合，挂载前设置同样有效。
    probe.interactive->set_on_navigation([&probe](std::string uri, aurora::webview::WebViewNavigationState state) {
        if (state == aurora::webview::WebViewNavigationState::Loading) {
            probe.nav_loading = true;
        } else if (state == aurora::webview::WebViewNavigationState::Finished) {
            probe.nav_finished = true;
        } else {
            probe.nav_failed = true;
            probe.nav_failed_detail = std::move(uri);
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

    aurora::Node root =
        aurora::Node{aurora::Column{aurora::Node{probe.interactive}, aurora::Node{probe.snapshot_view}}};
    aurora::Scene scene{std::move(root)};
    aurora::Application app{std::move(scene), std::move(window_owner), opts};
    probe.window = app.window();

    // ---- 落盘页并导航（file://：真实文档加载路径，FINISHED 语义完整）----
    const std::string page_path = "/tmp/aurora_verify_webview_wkwebview_" + std::to_string(::getpid()) + ".html";
    {
        auto written = std::ofstream(page_path, std::ios::trunc);
        written << probe_page_html();
        if (!written) {
            AURORA_LOG_ERROR("verify", "Cannot write probe page to " + page_path);
            return 2;
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
                    probe.results.add(
                        caps.engine == aurora::webview::WebViewEngine::WkWebView &&
                            caps.availability == aurora::webview::WebViewAvailability::Available && caps.interactive &&
                            caps.snapshot,
                        "engine-selected",
                        std::string("engine=") +
                            (caps.engine == aurora::webview::WebViewEngine::WkWebView ? "WkWebView" : "not-WkWebView") +
                            " availability=" + std::to_string(static_cast<int>(caps.availability)));
                    if (probe.interactive->degraded() || probe.snapshot_view->degraded()) {
                        probe.results.add(
                            false, "views-not-degraded",
                            std::string("interactive.degraded=") + (probe.interactive->degraded() ? "true" : "false") +
                                " snapshot.degraded=" + (probe.snapshot_view->degraded() ? "true" : "false"));
                        enter_stage(Stage::Done);
                        return;
                    }
                    probe.results.add(true, "views-not-degraded", "both views registered with the host");
                    probe.interactive->load_url(page_url);
                    probe.snapshot_view->load_url(page_url);
                }
                if (probe.nav_loading) {
                    enter_stage(Stage::Navigate);
                    return;
                }
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
                                          " finished=" + (probe.nav_finished ? "1" : "0") +
                                          " title=" + (probe.title_seen ? "1" : "0"));
                    enter_stage(Stage::Done);
                }
                return;
            }
            case Stage::Script: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    probe.interactive->evaluate_script("window.__ready === true", [&probe](std::string v) {
                        probe.eval_ready_done = true;
                        probe.eval_ready_value = std::move(v);
                    });
                    probe.interactive->evaluate_script(
                        std::string("document.title === '") + AURORA_PROBE_DOC_TITLE + "'", [&probe](std::string v) {
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
                // → 结果回调），故轮询重发而非按帧一次性触发。成功（=true）或墙钟超时才收敛。
                const int waited = probe.stage_waited_ms();
                const bool due =
                    probe.reflect_last_ms < 0 || waited - probe.reflect_last_ms >= AURORA_PROBE_REFLECT_INTERVAL_MS;
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
                    probe.eval_reflect_done = false;  // 反射尚未落定：下一轮继续轮询
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
                    NSWindow *host = host_window(probe.window);
                    const std::vector<ChildInfo> children = query_children(host);
                    probe.results.add(children.size() >= 2, "nsview-child-views",
                                      "WKWebView child count=" + aurora_verify::format_uint(children.size()));
                    for (const ChildInfo &c : children) {
                        probe.results.info("nsview-child", child_text(c));
                    }

                    // 预期矩形：aurora 自己的换算（窗口逻辑盒 × scale + CSD 内缩）。
                    const std::optional<aurora::Rect> ib = probe.interactive->window_bounds();
                    const std::optional<aurora::Rect> sb = probe.snapshot_view->window_bounds();
                    if (!ib.has_value() || !sb.has_value()) {
                        probe.results.add(false, "nsview-interactive-visible",
                                          "window_bounds unavailable after layout");
                        enter_stage(Stage::Done);
                        return;
                    }
                    const aurora::webview::NativeViewRect expected_i =
                        aurora::webview::to_native_rect(*ib, scale, inset);
                    const aurora::webview::NativeViewRect expected_s =
                        aurora::webview::to_native_rect(*sb, scale, inset);
                    probe.expected_before = expected_i;
                    probe.have_expected_before = true;
                    probe.results.info("expected-interactive", expected_text(expected_i));
                    probe.results.info("expected-snapshot", expected_text(expected_s));

                    const ChildInfo *match_i = nullptr;
                    const ChildInfo *match_s = nullptr;
                    for (const ChildInfo &c : children) {
                        if (!match_i && rect_close(expected_i, c, 2)) {
                            match_i = &c;
                        } else if (!match_s && rect_close(expected_s, c, 2)) {
                            match_s = &c;
                        }
                    }
                    probe.results.add(match_i != nullptr && !match_i->hidden, "nsview-interactive-visible",
                                      match_i == nullptr ? "no child matches expected rect " + expected_text(expected_i)
                                                         : child_text(*match_i));
                    probe.results.add(match_s != nullptr && match_s->hidden, "nsview-snapshot-hidden",
                                      match_s == nullptr ? "no child matches expected rect " + expected_text(expected_s)
                                                         : child_text(*match_s));
                }
                enter_stage(Stage::Resize);
                return;
            }
            case Stage::Resize: {
                if (!probe.stage_entered) {
                    probe.stage_entered = true;
                    // 改交互视图的尺寸意图 → 标脏布局 → 下一帧重排 → 几何再下发。
                    probe.interactive->width(aurora::px(AURORA_PROBE_VIEW_W1)).height(aurora::px(AURORA_PROBE_VIEW_H1));
                }
                aurora::Surface &surface = probe.window->surface();
                const float scale = surface.scale_factor();
                const aurora::EdgeInsets inset = surface.content_inset();
                const std::optional<aurora::Rect> ib = probe.interactive->window_bounds();
                if (!ib.has_value()) {
                    if (probe.stage_waited_ms() > AURORA_PROBE_RESIZE_MS) {
                        probe.results.add(false, "nsview-geometry-follows-resize",
                                          "window_bounds unavailable after resize");
                        enter_stage(Stage::Snapshot);
                    }
                    return;
                }
                const aurora::webview::NativeViewRect expected_after =
                    aurora::webview::to_native_rect(*ib, scale, inset);
                // 布局重排是异步的：先等预期矩形离开旧值（window_bounds 反映新尺寸意图），
                // 再轮询观测几何落定——过早接受旧矩形会把「尚未重排」误判成「不随动」。
                if (probe.have_expected_before && expected_after == probe.expected_before) {
                    if (probe.stage_waited_ms() > AURORA_PROBE_RESIZE_MS) {
                        probe.results.add(false, "nsview-geometry-follows-resize",
                                          "layout never reflected the new size intent within wall-clock budget");
                        enter_stage(Stage::Snapshot);
                    }
                    return;
                }
                // 轮询到观测几何落定（帧循环节拍受 WebKit IPC 影响，固定短墙钟会误判）。
                const std::vector<ChildInfo> after = query_children(host_window(probe.window));
                const ChildInfo *match = nullptr;
                for (const ChildInfo &c : after) {
                    if (rect_close(expected_after, c, 2)) {
                        match = &c;
                        break;
                    }
                }
                if (match == nullptr) {
                    if (probe.stage_waited_ms() > AURORA_PROBE_RESIZE_MS) {
                        probe.results.add(false, "nsview-geometry-follows-resize",
                                          "no child matches resized expected rect " + expected_text(expected_after));
                        enter_stage(Stage::Snapshot);
                    }
                    return;
                }
                const bool moved =
                    probe.have_expected_before && std::abs(match->width - probe.expected_before.width) > 4;
                probe.results.add(moved, "nsview-geometry-follows-resize",
                                  child_text(*match) + " (was " + expected_text(probe.expected_before) + ")");
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
                        // 空图是引擎按契约兑现的「捕获未产出像素」，不是「没收到回调」。
                        probe.results.add(false, "snapshot-bitmap",
                                          std::string("no pixels captured (") + aurora_verify::format_int(img.width) +
                                              "x" + aurora_verify::format_int(img.height) +
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
    emit("summary: frames=" + aurora_verify::format_int(probe.frames) +
         ", failures=" + aurora_verify::format_int(failures));
    if (failures > 0) {
        emit("FAILURES PRESENT (exit = 5)");
        return 5;
    }
    emit("ALL PASS -- WKWebView engine + host wiring verified on the real AppKit stack");
    if (keep_seconds > 0) {
        emit("interactive: keeping the process alive for " + std::to_string(keep_seconds) +
             "s -- inspect the window manually (frame loop already stopped)");
        std::this_thread::sleep_for(std::chrono::seconds(keep_seconds));
    }
    return 0;
}