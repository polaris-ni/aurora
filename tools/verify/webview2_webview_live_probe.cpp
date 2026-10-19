// WebView2 WebView —— Windows 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖对象：`src/aurora/webview/engine_webview2.cpp`（Microsoft Edge WebView2，运行期
// LoadLibrary 绑定 + 异步创建环境与控制器）经 `NativeViewHost` 宿主接线的**端到端**行为。
//
// 为什么必须真机：引擎的全部实质都在与真实 WebView2 运行时 / Win32 窗口管理器的接缝上——
//   * 环境与控制器的创建都是异步的（完成回调落在 UI 线程），「就绪之前到达的几何 / 可见性
//     下发与内容命令不丢」只能在真实运行时上验；
//   * 页面加载 / 导航三态 / 标题 / 脚本求值 / 双向消息通道依赖真实 WebView2 渲染进程；
//   * 几何同步的终点是 **Win32 子窗口的真实矩形与可见性**，只能由 Win32 读回；
//   * COM/STA 线程模型与本库消息泵的吻合只能由「异步回调真的抵达」证明。
// 单测覆盖的（本探针不重复）：`utest_web_view`（控件层降级 / 挂载 / 回调门 / 序列化）与
// `utest_native_view_host`（几何换算纯函数 / 去重 / 可见性翻转，桩引擎）。
//
// 探针原理（单一真实窗口，两个 WebView 同窗）：
//   1) 经 `Application` 建真实窗口——WebView 需要 `WindowHost` 的原生子视图书架在场，
//      故必须走 Application（裸 `create_native_window` 不装宿主，控件会走 host-unbound 降级）；
//   2) 场景根为 Column{ 交互模式 WebView, 快照模式 WebView }：交互视图拿原生可见性，
//      快照视图只作像素来源（宿主恒不授予原生可见）；
//   3) 内联 HTML 页（落盘后以 file:// 导航）自带：确定标题、确定背景色、
//      DOMContentLoaded 时经 shim 向 Native 投一条消息、onmessage 反射 global；
//   4) 帧循环由 `Application::run` 驱动（on_frame 里跑状态机）——异步证据（导航态 / 标题 /
//      消息 / 求值结果 / 快照像素）只在循环推进后才抵达，这本身就是「COM 回调已并入消息泵」
//      的接线证明；
//   5) 观测侧用 **独立 Win32 查询**读窗口真相：宿主 HWND 下的子窗口、几何（换算到宿主客户区
//      坐标）、可见性，与 aurora 自己的 `to_native_rect(window_bounds())` 预期值闭环比对；
//   6) 运行期改交互视图的尺寸意图（width / height）→ 布局重排 → 几何再下发，再次闭环比对；
//   7) 收尾：进程正常退出即 teardown 无恙。
//
// 构建：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release
//         -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//         -DAURORA_ENABLE_WEBVIEW=ON -DAURORA_ENABLE_WEBVIEW_WEBVIEW2=ON
//   cmake --build build-verify --target aurora_verify_webview_webview2
//
// 运行（需 WebView2 Evergreen 运行时 + `WebView2Loader.dll` 可被 `LoadLibrary` 找到）：
//   build-verify\aurora_verify_webview_webview2.exe
//   build-verify\aurora_verify_webview_webview2.exe --interactive[=秒]
//
// ⚠ `WebView2Loader.dll` 是**应用侧可再分发件**，Evergreen 运行时**不随附**它（系统装的是
//   浏览器本体，装在 `%ProgramFiles(x86)%\Microsoft\EdgeWebView\Application\<版本>\`）。引擎
//   按设计零链接、运行期 `LoadLibraryA("WebView2Loader.dll")`，故运行前必须让它可被找到——
//   把一份 `WebView2Loader.dll` 放到本 exe 同目录，或把其所在目录加进 `PATH`。找不到时引擎
//   自报 `MissingRuntime`、控件降级为占位，本探针会在 `engine-selected` 判据上如实报 fail。
//
// ⚠ 渲染拓扑：本探针在 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` 未被显式设定时置
//   `--in-process-gpu`（见 main 的「环境前置」）。原因是虚拟 / 沙箱会话下 WebView2 的 GPU 进程
//   常常起不来（浏览器日志 `GPU process exited unexpectedly` 反复若干次后
//   `FATAL ... GPU process isn't usable. Goodbye.`），viz 合成器随之不产出帧，`CapturePreview`
//   虽返回 S_OK 却**永不回调** ⇒ `snapshot-bitmap` 会超时。该塑形不改引擎代码路径，只固定渲染
//   拓扑；如需按 GPU 进程隔离的原样验收，显式设定该变量（如空串或其它参数）即可覆盖。
//
// 自动段验收项（逐项打 `RES|pass|…` / `RES|fail|…` 行，末行汇总）：
//   engine-selected              —— 懒安装挑中的引擎是 WebView2 且 Available
//   views-not-degraded           —— 两个视图均未走降级（建视图 + 登记成功）
//   nav-loading-then-finished    —— 交互视图导航态 Loading → Finished（无 Failed），标题一致
//   eval-ready-and-title         —— evaluate_script 两次布尔往返均 true
//   page-to-native-message       —— 页面经 shim 投出的 JSON 抵达 on_web_message
//   native-to-page-message       —— post_json 经 shim 抵达页面 onmessage
//   win32-child-windows          —— 宿主 HWND 下出现两个子窗口（两视图各一）
//   win32-interactive-visible    —— 交互视图子窗可见且几何等于预期矩形
//   win32-snapshot-hidden        —— 快照视图子窗不可见（宿主不授予原生可见）
//   win32-geometry-follows-resize—— 改尺寸意图后子窗几何随新预期矩形更新
//   snapshot-bitmap              —— 快照位图非空、尺寸正确、背景色为主、有内容像素
//
// 退出码：
//   0  全部验收项通过 —— 引擎与宿主接线在真机成立
//   2  环境不可用（建窗失败或回退无头）
//   3  探针无法触及目标（引擎不是 WebView2 / 视图意外降级 / 视图未建出）
//   5  部分验收项不符 —— 见逐行 fail 表
//
// 注意：
//   * 窗口以 NoActivate 建（不抢焦点）；退出即销毁，不留桌面残留。
//   * 观测侧只读 Win32 事实，不触碰 aurora 内部状态；几何一律换算到宿主客户区坐标再比对
//     （`put_Bounds` 的坐标系就是父客户区）。
// ============================================================================

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_webview_webview2 can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif
#ifndef AURORA_BACKEND_WIN32
#error "aurora_verify_webview_webview2 requires AURORA_BACKEND_WIN32"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

// windows.h 必须后于全部 aurora 头（其 `interface` / `min` / `max` 宏会污染 aurora 侧声明；
// 口径同 win32_csd_live_probe 与 engine_webview2.cpp）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
// clang-format off
#include <windows.h>
// clang-format on

namespace {

constexpr auto AURORA_PROBE_TITLE = "aurora-verify-webview-webview2";
constexpr auto AURORA_PROBE_DOC_TITLE = "Aurora WebView probe";

/// WebView2 传给浏览器进程的附加命令行（经此环境变量读取；见运行前置说明）。
constexpr auto AURORA_PROBE_BROWSER_ARGS_ENV = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
/// GPU 收进本进程：本机 GPU 进程不可用时 viz 合成器不产出帧，快照捕获永不回调。
constexpr auto AURORA_PROBE_BROWSER_ARGS_VALUE = "--in-process-gpu";

/// 页面背景色（快照像素判据的确定值）：#1F4E79。
constexpr std::uint8_t AURORA_PROBE_BG_R = 31;
constexpr std::uint8_t AURORA_PROBE_BG_G = 78;
constexpr std::uint8_t AURORA_PROBE_BG_B = 121;

/// 交互视图初始 / 改档后的尺寸意图（dp）：改档驱动几何再下发。
constexpr float AURORA_PROBE_VIEW_W0 = 300.0F;
constexpr float AURORA_PROBE_VIEW_H0 = 220.0F;
constexpr float AURORA_PROBE_VIEW_W1 = 380.0F;
constexpr float AURORA_PROBE_VIEW_H1 = 260.0F;

/// 各阶段墙钟上限（毫秒）。帧循环是忙转 ⇒ **帧预算对异步等待无意义**（WebView2 环境 /
/// 控制器创建、IPC、渲染都以墙钟计），故一律以墙钟判，避免按帧早早误判超时。
constexpr int AURORA_PROBE_BOOT_MS = 30000;  ///< 挂载 + 能力落定 + 导航起
constexpr int AURORA_PROBE_NAV_MS = 40000;  ///< 导航 + 标题（WebView2 IPC）
constexpr int AURORA_PROBE_SCRIPT_MS = 20000;  ///< 两次 evaluate 往返
constexpr int AURORA_PROBE_MESSAGE_MS = 20000;  ///< 两侧消息通道
constexpr int AURORA_PROBE_RESIZE_MS = 300;  ///< 改档后重排 + 再同步（纯本地布局，短墙钟即可）
constexpr int AURORA_PROBE_SNAPSHOT_MS = 30000;  ///< 快照异步捕获
/// 页侧反射求值的重试间隔（ms）：postMessage → 页 onmessage → 反射是异步的，须轮询。
constexpr int AURORA_PROBE_REFLECT_INTERVAL_MS = 120;
/// 几何比对容差（物理像素）。
constexpr int AURORA_PROBE_GEOM_TOL = 2;

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 宽字符 → UTF-8（页 URL / 窗口类名显示用）。
[[nodiscard]] auto to_utf8(const std::wstring &w) -> std::string {
    if (w.empty()) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

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
<div id="tag">aurora-webview2-probe</div>
<script>
  window.__ready = false;
  window.__last_native = null;
  window.aurora.onmessage = function (d) { window.__last_native = JSON.stringify(d); };
  document.addEventListener('DOMContentLoaded', function () {
    window.__ready = true;
    window.aurora.postMessage({ hello: 'from-page', seq: 1, tag: 'aurora-webview2-probe' });
  });
</script>
</body>
</html>
)HTML";
}

/// @brief 一个 Win32 子窗口的观测事实（几何为**宿主客户区**相对，可见性读自 `IsWindowVisible`）。
struct ChildInfo {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    int depth = 0;  ///< 相对宿主的层数（1 = 直接子窗）
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool visible = false;
    std::string cls;
};

/// @brief `EnumChildWindows` 的收集上下文（回调无法捕获，经 LPARAM 传）。
struct EnumCtx {
    HWND host = nullptr;
    std::vector<ChildInfo> *out = nullptr;
};

auto CALLBACK enum_child_cb(HWND child, LPARAM lparam) -> BOOL {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    auto *ctx = reinterpret_cast<EnumCtx *>(lparam);
    ChildInfo ci;
    ci.hwnd = child;
    ci.parent = ::GetParent(child);
    int depth = 1;
    for (HWND p = ci.parent; p != nullptr && p != ctx->host; p = ::GetParent(p)) {
        ++depth;
    }
    ci.depth = depth;

    RECT wr{};
    if (::GetWindowRect(child, &wr) != 0) {
        // 一律换算到**宿主客户区**（`put_Bounds` 的坐标系）；WebView2 的子窗可能嵌套，
        // 故不能只在直接父窗里算。
        POINT pts[2] = {{.x = wr.left, .y = wr.top}, {.x = wr.right, .y = wr.bottom}};
        ::MapWindowPoints(nullptr, ctx->host, pts, 2);  // hWndFrom = nullptr ⇒ 入参视为屏幕坐标
        ci.x = static_cast<int>(pts[0].x);
        ci.y = static_cast<int>(pts[0].y);
        ci.width = static_cast<int>(wr.right - wr.left);
        ci.height = static_cast<int>(wr.bottom - wr.top);
    }
    ci.visible = ::IsWindowVisible(child) != 0;

    wchar_t buf[256] = {0};
    if (::GetClassNameW(child, buf, 256) > 0) {
        ci.cls = to_utf8(buf);
    }
    ctx->out->push_back(ci);
    return TRUE;  // 继续枚举（含后代）
}

/// @brief 列宿主窗下的全部子窗（`EnumChildWindows` 深度优先返回，几何与可见性逐个补齐）。
[[nodiscard]] auto query_children(HWND host) -> std::vector<ChildInfo> {
    std::vector<ChildInfo> out;
    EnumCtx ctx{.host = host, .out = &out};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    ::EnumChildWindows(host, &enum_child_cb, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

/// @brief 两个物理矩形是否在容差内相等（预期 vs Win32 实测）。
[[nodiscard]] auto rect_close(const aurora::webview::NativeViewRect &expected, const ChildInfo &actual, int tol)
    -> bool {
    return std::abs(expected.x - actual.x) <= tol && std::abs(expected.y - actual.y) <= tol &&
           std::abs(expected.width - actual.width) <= tol && std::abs(expected.height - actual.height) <= tol;
}

[[nodiscard]] auto child_text(const ChildInfo &c) -> std::string {
    return "hwnd=" + aurora_verify::format_handle(static_cast<const void *>(c.hwnd)) +
           " depth=" + aurora_verify::format_int(c.depth) + " cls=" + c.cls + " geom=(" +
           aurora_verify::format_int(c.x) + "," + aurora_verify::format_int(c.y) + " " +
           aurora_verify::format_int(c.width) + "x" + aurora_verify::format_int(c.height) +
           ") visible=" + (c.visible ? "yes" : "no");
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
    Geometry,  ///< Win32 子窗口证据 + 预期矩形闭环比对
    Resize,  ///< 改尺寸意图 → 几何再下发 → 再次闭环比对
    Snapshot,  ///< 快照请求 → 像素判据
    Done,  ///< 收尾
};

/// @brief 探针主体状态（帧回调与 main 共享；全部主线程内访问）。
struct Probe {
    Results results;

    // ---- 异步证据（回调置位；状态机轮询）----
    // 成员按对齐分组排列：8 字节对齐成员在前、4 字节成员居中、1 字节标志收尾。
    // bool 与多字节成员交错会各自引入填充（clang-analyzer performance.Padding），
    // 集中收尾后仅剩对齐收尾的零星填充。
    std::string nav_failed_detail;
    std::string title_value;
    std::string page_message;
    std::string eval_ready_value;
    std::string eval_title_value;
    std::string eval_reflect_value;
    aurora::Image snapshot_bitmap;

    // ---- 控件与窗口 ----
    std::shared_ptr<aurora::webview::WebView> interactive;
    std::shared_ptr<aurora::webview::WebView> snapshot_view;
    aurora::Window *window = nullptr;
    std::chrono::steady_clock::time_point stage_start = std::chrono::steady_clock::now();
    std::vector<ChildInfo> children_before;
    aurora::webview::NativeViewRect expected_before{};

    int frames = 0;
    int reflect_last_ms = -1;  ///< 上次发起反射求值的阶段相对毫秒（轮询节流用；-1=尚未发起）
    Stage stage = Stage::Boot;

    // ---- 1 字节标志集中收尾（见上文对齐分组说明）----
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
           json.find("aurora-webview2-probe") != std::string::npos;
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码 / terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto spec = aurora::cli::CommandSpec{
        .name = "aurora_verify_webview_webview2",
        .about = "WebView2 WebView live probe (Windows)",
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

    // ---- 环境前置（对位 Linux 探针的环境塑形口径：那边清 WAYLAND_DISPLAY 强制走 X11）----
    // 本机若 GPU 进程不可用（虚拟 / 沙箱会话常见，chromium 日志落 `GPU process isn't usable.
    // Goodbye.`），viz 合成器不产出帧，`CapturePreview` 永不回调 ⇒ 快照项无从验收。把 GPU 收进
    // 本进程即可让捕获路径可用；它**不改引擎代码路径**（几何 / 可见性 / 事件 / 捕获接口逐一对位），
    // 只把运行环境的渲染拓扑固定成确定可用的一种。调用方已显式设定者尊重其设定。
    if (::GetEnvironmentVariableA(AURORA_PROBE_BROWSER_ARGS_ENV, nullptr, 0) == 0) {
        // 用 `_putenv_s` 而非 `SetEnvironmentVariableA`：后者只改 Win32 环境块，而读取方若走
        // CRT 的 `getenv`，读的是进程启动时快照的 `_environ` —— 进程内后设的变量它看不见。
        // `_putenv_s` 两处一并更新，故进程内塑形也能抵达读取方。
        ::_putenv_s(AURORA_PROBE_BROWSER_ARGS_ENV, AURORA_PROBE_BROWSER_ARGS_VALUE);
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
        AURORA_LOG_ERROR("verify", "Surface fell back to Headless (no Win32 backend for this session)");
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

    aurora::Node root =
        aurora::Node{aurora::Column{aurora::Node{probe.interactive}, aurora::Node{probe.snapshot_view}}};
    aurora::Scene scene{std::move(root)};
    aurora::Application app{std::move(scene), std::move(window_owner), opts};
    probe.window = app.window();

    // ---- 落盘页并导航（file://：真实文档加载路径，FINISHED 语义完整）----
    wchar_t tmp_dir[MAX_PATH] = {0};
    if (::GetTempPathW(MAX_PATH, tmp_dir) == 0) {
        AURORA_LOG_ERROR("verify", "GetTempPathW failed");
        return 2;
    }
    const std::wstring page_wpath = std::wstring(tmp_dir) + L"aurora_verify_webview_webview2_" +
                                    std::to_wstring(::GetCurrentProcessId()) + L".html";
    {
        std::ofstream written(page_wpath.c_str(), std::ios::trunc);
        written << probe_page_html();
        if (!written) {
            AURORA_LOG_ERROR("verify", "Cannot write probe page to " + to_utf8(page_wpath));
            return 4;
        }
    }
    // file:// URL：反斜杠转正斜杠，前缀 file:///。
    std::string page_path_utf8 = to_utf8(page_wpath);
    std::replace(page_path_utf8.begin(), page_path_utf8.end(), '\\', '/');
    const std::string page_url = "file:///" + page_path_utf8;

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
                        caps.engine == aurora::webview::WebViewEngine::WebView2 &&
                            caps.availability == aurora::webview::WebViewAvailability::Available && caps.interactive &&
                            caps.snapshot,
                        "engine-selected",
                        std::string("engine=") +
                            (caps.engine == aurora::webview::WebViewEngine::WebView2 ? "WebView2" : "not-WebView2") +
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
                    // 首帧挂载即发起初始导航（on_mount 的序：set_callbacks → zoom → js → navigate）。
                    probe.interactive->load_url(page_url);
                    probe.snapshot_view->load_url(page_url);
                }
                if (probe.nav_loading) {
                    enter_stage(Stage::Navigate);
                    return;
                }
                // 墙钟上限：WebView2 环境 + 控制器冷启动要数百毫秒到数秒，忙转帧数不反映墙钟。
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
                // → 结果回调），故**轮询**重发而非按帧一次性触发：帧率不受限时「第 20 帧」只有毫秒级，
                // 早于页侧更新即读到空值。成功（=true）或墙钟超时才收敛。
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
                    // 反射尚未落定（读到 false / 空）：清标志，下一轮继续轮询。
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
                    // 宿主 HWND：`Win32Surface::native_handle()` 契约 = 返回宿主 HWND。
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
                    HWND host = static_cast<HWND>(probe.window->surface().native_handle());
                    probe.children_before = query_children(host);
                    probe.results.add(probe.children_before.size() >= 2, "win32-child-windows",
                                      "host child count=" + aurora_verify::format_uint(probe.children_before.size()));
                    for (const ChildInfo &c : probe.children_before) {
                        probe.results.info("win32-child", child_text(c));
                    }

                    // 预期矩形：aurora 自己的换算（窗口逻辑盒 × scale + CSD 内缩）。
                    const std::optional<aurora::Rect> ib = probe.interactive->window_bounds();
                    const std::optional<aurora::Rect> sb = probe.snapshot_view->window_bounds();
                    if (!ib.has_value() || !sb.has_value()) {
                        probe.results.add(false, "win32-interactive-visible", "window_bounds unavailable after layout");
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
                    for (const ChildInfo &c : probe.children_before) {
                        if (match_i == nullptr && rect_close(expected_i, c, AURORA_PROBE_GEOM_TOL)) {
                            match_i = &c;
                        } else if (match_s == nullptr && rect_close(expected_s, c, AURORA_PROBE_GEOM_TOL)) {
                            match_s = &c;
                        }
                    }
                    probe.results.add(match_i != nullptr && match_i->visible, "win32-interactive-visible",
                                      match_i == nullptr ? "no child matches expected rect " + expected_text(expected_i)
                                                         : child_text(*match_i));
                    probe.results.add(match_s != nullptr && !match_s->visible, "win32-snapshot-hidden",
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
                if (probe.stage_waited_ms() < AURORA_PROBE_RESIZE_MS) {
                    return;  // 等墙钟让重排与几何同步落定（帧忙转，帧数不代表墙钟）
                }
                aurora::Surface &surface = probe.window->surface();
                const float scale = surface.scale_factor();
                const aurora::EdgeInsets inset = surface.content_inset();
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
                HWND host = static_cast<HWND>(probe.window->surface().native_handle());
                const std::vector<ChildInfo> after = query_children(host);
                const std::optional<aurora::Rect> ib = probe.interactive->window_bounds();
                if (!ib.has_value()) {
                    probe.results.add(false, "win32-geometry-follows-resize", "window_bounds unavailable after resize");
                    enter_stage(Stage::Done);
                    return;
                }
                const aurora::webview::NativeViewRect expected_after =
                    aurora::webview::to_native_rect(*ib, scale, inset);
                probe.results.info("expected-interactive-after", expected_text(expected_after));
                const ChildInfo *match = nullptr;
                for (const ChildInfo &c : after) {
                    if (rect_close(expected_after, c, AURORA_PROBE_GEOM_TOL)) {
                        match = &c;
                        break;
                    }
                }
                const bool moved = match != nullptr && probe.have_expected_before &&
                                   std::abs(match->width - probe.expected_before.width) > 4;
                probe.results.add(match != nullptr && moved, "win32-geometry-follows-resize",
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
                        // 要求失败也必须回调空图）。最常见成因是合成器不产帧：GPU 进程不可用
                        // 的会话里 `CapturePreview` 返回 S_OK 却永不回调。给出复核手段，免得
                        // 把环境限制误读成引擎缺陷。
                        probe.results.add(false, "snapshot-bitmap",
                                          std::string("no pixels captured (") + aurora_verify::format_int(img.width) +
                                              "x" + aurora_verify::format_int(img.height) +
                                              "); compositor may produce no frames — retry with "
                                              "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--in-process-gpu");
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
    ::DeleteFileW(page_wpath.c_str());

    // ---- 汇总 ----
    probe.results.dump();
    const int failures = probe.results.failures();
    emit("summary: frames=" + aurora_verify::format_int(probe.frames) +
         ", failures=" + aurora_verify::format_int(failures));
    if (failures > 0) {
        emit("FAILURES PRESENT (exit = 5)");
        return 5;
    }
    emit("ALL PASS -- WebView2 engine + host wiring verified on the real Windows stack");
    if (keep_seconds > 0) {
        emit("interactive: keeping the process alive for " + std::to_string(keep_seconds) +
             "s -- inspect the window manually (frame loop already stopped)");
        std::this_thread::sleep_for(std::chrono::seconds(keep_seconds));
    }
    return 0;
}
