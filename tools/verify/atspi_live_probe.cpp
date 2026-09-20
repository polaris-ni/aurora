/* AT-SPI2 无障碍桥 —— Linux 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖后端：`X11Surface` 与 `WaylandSurface` 共用的 `detail::AtspiBridge`（libdbus
// dlopen 接线）＋ `AtspiModel` 折算层。二者对 AT-SPI 的接缝同一份桥，只是宿主 fd 泵
// （`wait_events` 集成）各自实现，故 `--x11` / `--wayland` 各跑一路。
//
// 为什么必须真机：无头 CI 里没有 AT-SPI 客户端 —— `org.a11y.Bus` 取不到、注册表
// `Socket.Embed` 无人应答、libatspi 永远读不到我们的线格式。**桥与 AT-SPI2 栈的接缝**
// （握手、Cache.GetItems 行格式、角色/状态序号、Component/Text/Action 方法面）只能对
// **真实 at-spi2-core（registryd + libatspi）**证明。无头单测（`utest_atspi_protocol`）
// 覆盖的是折算层的表与算术；本探针覆盖的是「线上应答是否被上游客户端接受」。
//
// 原理（与读屏同路径，跨进程）：
//   1) 用 aurora 建**真实窗口**（X11 或 Wayland），塞入 Button / TextInput / Text 三个
//      带无障碍语义的控件，`present_root` 注根 ⇒ 桥首帧建链并 eager Embed；
//   2) 起 **python3 + gi/Atspi**（libatspi 的官方绑定，Accerciser 同款入口）为子进程
//      客户端：枚举桌面树 → 找到本应用（app Name "Aurora" + FRAME Name = 窗口标题）→
//      逐节点断言角色/名称/状态/接口/几何/文本/动作；
//   3) 客户端每一次跨进程调用都会阻塞在**本进程的 D-Bus 应答**上 —— 父进程同时在
//      `wait_events` 帧循环里泵桥（与真实应用的空闲泵同构），这本身就是接线证明；
//   4) `DoAction("press")` 经桥路由回 `Button::on_click`，父进程以进程内标志位核对
//      「读屏反向操作真的点到了控件」（跨半程闭环）。
//
// 构建：
//   cmake -S . -B build-verify-a11y -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_X11=ON -DAURORA_BACKEND_WAYLAND=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify-a11y --target aurora_verify_atspi
//
// 运行（WSLg / 桌面会话；需 python3-gi + gir1.2-atspi-2.0）：
//   发行版缺 atspi typelib 时可解包 deb 到本地目录并指给子进程环境：
//     export GI_TYPELIB_PATH=<解包>/…/girepository-1.0[:<fd 解包>/…/girepository-1.0]
//     export PYTHONPATH=<解包>/python3/dist-packages
//   ./build-verify-a11y/aurora_verify_atspi              # 默认：Wayland 会话优先（同工厂）
//   ./build-verify-a11y/aurora_verify_atspi --x11        # 强制 X11/XWayland 一路
//   ./build-verify-a11y/aurora_verify_atspi --wayland    # 要求原生 Wayland
//   ./build-verify-a11y/aurora_verify_atspi --interactive[=秒]  # 自动段后驻留供 Accerciser 浏览
//
// 自动段验收项（客户端侧逐项打 `RES|pass|<id>|<detail>`，父进程汇总）：
//   app-found / frame-found            —— Embed 入桌面树、FRAME Name=标题
//   button-found（role=push button 且 Name=确定）
//   button-state-{ENABLED,SENSITIVE,FOCUSABLE}（SHOWING/VISIBLE 只报 INFO，属已知申报项）
//   button-action-press / button-do-action     —— Action 接口 "press" 及应答
//   press-handler-side-effect          —— 父进程侧 on_click 真的被触发（跨半程闭环）
//   button-extents                     —— Component.GetExtents(SCREEN) 非空盒
//   entry-found / entry-text / entry-char-count  —— role=entry + Text 接口（码点口径）
//   static-found                       —— role=static 且 Name=订单总额（Text→STATIC 折算）
//
// 人工段（--interactive）：自动段结束后窗口驻留（默认 30s），期间用 Accerciser /
//   `python3 -c` 手工下钻核对树形、名称、几何与「操作」页签；父进程持续泵桥。
//
// 退出码：
//   0  全部验收项通过 —— 桥与 AT-SPI2 栈接缝正确
//   2  环境不可用（无 DISPLAY/WAYLAND_DISPLAY / 无 python3-gi Atspi / 窗口回退 Headless）
//   3  桥未建立（org.a11y.Bus 不可达或 Embed 失败 ⇒ 库按设计永久降级；或设了 NO_AT_BRIDGE）
//   4  客户端无有效结果（子进程崩溃/超时/未产出任何断言行）
//   5  部分验收项不符 —— 见逐行 fail 表
//
// 注意：本探针**不**移动用户指针、不改窗口管理器状态；退出前销毁窗口即断开 a11y 连接。
//   无事件通道（Object:/Cache: 信号、LiveRegion 播报）尚属已申报的推迟项 —— 故客户端的
//   树枚举靠轮询而非事件推送，与「注册表可见性」断言不冲突。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_atspi can only be built on Linux/Unix (non-Apple)"
#endif
#if !defined(AURORA_BACKEND_X11) && !defined(AURORA_BACKEND_WAYLAND)
#error "AURORA_BACKEND_X11 or AURORA_BACKEND_WAYLAND must be enabled"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aurora/i18n/localized_string.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/window/window.h"
#include "aurora/window/surface.h"
#include "verify_print.h"

namespace {

constexpr const char *kAppName = "Aurora";        ///< 桥 env.app_name（宿主接线固定值）
constexpr const char *kFrameTitle = "Aurora AT-SPI verify";  ///< FRAME Name（窗口标题）

bool g_press_executed = false;  ///< DoAction 跨半程闭环证据（on_click 在 UI 线程同步置位）

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 客户端脚本（libatspi 官方 GI 绑定，与 Accerciser 同栈）。逐断言打 RES 行。
const char *kClientScript = R"PY(
import sys, time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

app_name, frame_title = sys.argv[1], sys.argv[2]

def res(status, ident, detail=''):
    print('RES|%s|%s|%s' % (status, ident, str(detail).replace('\n', ' ')), flush=True)

def info(ident, detail=''):
    print('INFO|%s|%s' % (ident, str(detail).replace('\n', ' ')), flush=True)

def safe(fn, default):
    try:
        return fn()
    except Exception:
        return default

Atspi.init()

def collect(node, out, cap=400):
    if node is None or len(out) >= cap:
        return
    out.append(node)
    n = int(safe(node.get_child_count, 0) or 0)
    for i in range(min(n, 64)):
        try:
            collect(node.get_child_at_index(i), out, cap)
        except Exception:
            pass

deadline = time.time() + 20.0
app = None
frame = None
while time.time() < deadline:
    desktop = Atspi.get_desktop(0)
    for i in range(int(safe(desktop.get_child_count, 0) or 0)):
        a = desktop.get_child_at_index(i)
        if a is None or safe(a.get_name, '') != app_name:
            continue
        bag = []
        collect(a, bag)
        for x in bag:
            if safe(x.get_role, -1) == Atspi.Role.FRAME and safe(x.get_name, '') == frame_title:
                app, frame = a, x
                break
        if app is not None:
            break
    if app is not None:
        break
    time.sleep(0.4)

if app is None:
    res('fail', 'app-found',
        'no app "%s" exposing frame "%s" on the desktop within 20s' % (app_name, frame_title))
    print('DONE', flush=True)
    sys.exit(0)
res('pass', 'app-found', app_name)
res('pass', 'frame-found', frame_title)

nodes = []
collect(app, nodes)
info('tree-size', len(nodes))
for x in nodes:
    print('TREE|%s|%s' % (safe(x.get_role_name, '?'), safe(x.get_name, '')), flush=True)

def find_one(role, name=None):
    for x in nodes:
        if safe(x.get_role, -1) == role and (name is None or safe(x.get_name, '') == name):
            return x
    return None

btn = find_one(Atspi.Role.PUSH_BUTTON, '\u786e\u5b9a')
res('pass' if btn is not None else 'fail', 'button-found', 'role=push button name=\u786e\u5b9a')
if btn is not None:
    ss = safe(btn.get_state_set, None)

    def has(label):
        st = getattr(Atspi.StateType, label)
        return bool(ss is not None and safe(lambda: ss.contains(st), False))

    for label in ('ENABLED', 'SENSITIVE', 'FOCUSABLE'):
        res('pass' if has(label) else 'fail', 'button-state-' + label)
    info('button-state-showing', int(has('SHOWING')))
    info('button-state-visible', int(has('VISIBLE')))
    na = int(safe(lambda: Atspi.Action.get_n_actions(btn), 0) or 0)
    names = [safe(lambda i=i: Atspi.Action.get_action_name(btn, i), '') for i in range(na)]
    res('pass' if 'press' in names else 'fail', 'button-action-press', 'actions=%s' % names)
    if 'press' in names:
        ok = safe(lambda: Atspi.Action.do_action(btn, names.index('press')), False)
        res('pass' if ok else 'fail', 'button-do-action', 'returned=%s' % ok)
    else:
        res('skip', 'button-do-action', 'no press action')
    ext = safe(lambda: Atspi.Component.get_extents(btn, Atspi.CoordType.SCREEN), None)
    if ext is None:
        res('fail', 'button-extents', 'Component.GetExtents unavailable')
    else:
        ok = ext.width > 0 and ext.height > 0
        res('pass' if ok else 'fail', 'button-extents',
            '(%d,%d %dx%d)' % (ext.x, ext.y, ext.width, ext.height))

ent = find_one(Atspi.Role.ENTRY)
res('pass' if ent is not None else 'fail', 'entry-found', 'role=entry')
if ent is not None:
    t = safe(lambda: Atspi.Text.get_text(ent, 0, -1), None)
    res('pass' if t == 'abc' else 'fail', 'entry-text', 'get_text=%r' % t)
    n = int(safe(lambda: Atspi.Text.get_character_count(ent), -1) or -1)
    res('pass' if n == 3 else 'fail', 'entry-char-count', 'count=%d' % n)

st = find_one(Atspi.Role.STATIC, '\u8ba2\u5355\u603b\u989d')
res('pass' if st is not None else 'fail', 'static-found',
    'role=static name=\u8ba2\u5355\u603b\u989d')

print('DONE', flush=True)
)PY";

/// @brief 一列带无障碍语义的控件（与 Win32 UIA 探针同族的三件套，判据见头注）。
[[nodiscard]] auto build_probe_column() -> aurora::Node {
    auto button = aurora::Button{aurora::ButtonProps{.label = aurora::LocalizedString{"确定"}}};
    button.set_on_click([] { g_press_executed = true; });
    return aurora::Node{aurora::Column{
        std::move(button),
        aurora::TextInput{aurora::TextInputProps{.value = "abc", .placeholder = "请输入"}},
        aurora::Text{aurora::TextProps{.content = aurora::LocalizedString{"订单总额"}}},
    }};
}

/// @brief python3 + gi/Atspi 可用性（阻塞调用即可：此步不触本进程桥）。
[[nodiscard]] auto python_atspi_available() -> bool {
    return std::system("python3 -c \"import gi; gi.require_version('Atspi','2.0'); "
                       "from gi.repository import Atspi\" >/dev/null 2>&1") == 0;
}

[[nodiscard]] auto write_text_file(const std::string &path, const std::string &text) -> bool {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
    f.close();
    return static_cast<bool>(f);
}

[[nodiscard]] auto read_text_file(const std::string &path) -> std::string {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

/// @brief 起 python 客户端（stdout/stderr → out_path），返回 pid（0 = fork 失败，父内 -1 无）。
[[nodiscard]] auto spawn_client(const std::string &script_path, const std::string &out_path) -> pid_t {
    const pid_t pid = fork();
    if (pid == 0) {
        const int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        execlp("python3", "python3", script_path.c_str(), kAppName, kFrameTitle,
               static_cast<char *>(nullptr));
        _exit(127);
    }
    return pid;
}

struct ResLine {
    std::string status;  ///< pass / fail / skip
    std::string id;
    std::string detail;
};

/// @brief 拆出 RES / TREE / INFO 三类行（RES 参与判定；其余原样转录）。
auto parse_client_output(const std::string &text, std::vector<ResLine> &results, std::vector<std::string> &notes,
                         bool &done) -> void {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "DONE") {
            done = true;
            continue;
        }
        if (line.rfind("RES|", 0) == 0) {
            std::istringstream rs(line);
            ResLine r;
            std::string head;
            std::getline(rs, head, '|');  // RES
            std::getline(rs, r.status, '|');
            std::getline(rs, r.id, '|');
            std::getline(rs, r.detail);  // 其余全部（detail 不再含 '|' 由客户端保证）
            results.push_back(std::move(r));
        } else if (line.rfind("TREE|", 0) == 0 || line.rfind("INFO|", 0) == 0) {
            notes.push_back(line);
        } else if (!line.empty()) {
            notes.push_back("OUT|" + line);  // 客户端 traceback 等
        }
    }
}

auto nap_pump(aurora::Window &window, int frames, double ms) -> void {
    for (int i = 0; i < frames; ++i) {
        window.surface().wait_events(ms);
    }
}

}  // namespace

auto main(int argc, char **argv) -> int {
    bool want_x11 = false;
    bool want_wayland = false;
    int keep_seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--x11") {
            want_x11 = true;
        } else if (a == "--wayland") {
            want_wayland = true;
        } else if (a.rfind("--interactive", 0) == 0) {
            const auto eq = a.find('=');
            keep_seconds = eq == std::string::npos ? 30 : std::atoi(a.c_str() + eq + 1);
            if (keep_seconds < 1) {
                keep_seconds = 1;
            }
        }
    }
    if (want_x11) {
        ::unsetenv("WAYLAND_DISPLAY");  // 工厂运行期选择：清掉 Wayland 环境即回 X11/XWayland 路
    } else if (want_wayland && ::getenv("WAYLAND_DISPLAY") == nullptr) {
        AURORA_LOG_ERROR("verify", "--wayland requested but WAYLAND_DISPLAY is unset");
        return 2;
    }
    if (::getenv("WAYLAND_DISPLAY") == nullptr && ::getenv("DISPLAY") == nullptr) {
        AURORA_LOG_ERROR("verify", "No WAYLAND_DISPLAY / DISPLAY -- AT-SPI live probe needs a desktop session");
        return 2;
    }
    if (!python_atspi_available()) {
        AURORA_LOG_ERROR("verify",
                         "python3 with gi + Atspi (gir1.2-atspi-2.0) unavailable; client-side acceptance impossible");
        return 2;
    }

    // ---- 1) 建真实窗口 + 注根（桥首帧建链并 eager Embed）----
    auto made = aurora::create_native_window(aurora::WindowOptions{
        .size = aurora::Size{.width = 480.0F, .height = 320.0F}, .title = kFrameTitle});
    if (!made) {
        AURORA_LOG_ERROR("verify", "create_native_window failed: " + made.error().message);
        return 2;
    }
    auto window = std::move(made.value());
    if (window->surface().native_handle() == nullptr) {
        AURORA_LOG_ERROR("verify", "Surface fell back to Headless (no real display for the chosen backend)");
        return 2;
    }
    emit(std::string("backend = ") + (want_x11 ? "X11 (forced)" : "factory-selected (Wayland-first)"));

    aurora::Node root = build_probe_column();
    (void)window->present_root(root);
    nap_pump(*window, 6, 20.0);  // 让 ConfigureNotify 落原点、桥完成握手与首帧快照

    // ---- 2) 桥必须在位（null = org.a11y.Bus 不可达 / Embed 失败 ⇒ 库按设计永久降级）----
    const aurora::a11y::Provider *bridge = window->surface().accessibility_provider();
    if (bridge == nullptr) {
        AURORA_LOG_ERROR("verify",
                         "AtspiBridge absent after root injection -- a11y bus unreachable or embed failed "
                         "(designed permanent degradation; NO_AT_BRIDGE also lands here)");
        return 3;
    }
    emit("bridge = " + std::string{bridge->name()} +
         ", active = " + (bridge->is_active() ? "true" : "false"));

    // ---- 3) 起跨进程 libatspi 客户端；父进程持续泵桥（客户端每个调用都等我们应答）----
    const std::string tag = std::to_string(static_cast<long>(::getpid()));
    const std::string script_path = "/tmp/aurora_verify_atspi_client_" + tag + ".py";
    const std::string out_path = "/tmp/aurora_verify_atspi_out_" + tag + ".txt";
    if (!write_text_file(script_path, kClientScript)) {
        AURORA_LOG_ERROR("verify", "Cannot write client script to " + script_path);
        return 4;
    }
    const pid_t child = spawn_client(script_path, out_path);
    if (child <= 0) {
        AURORA_LOG_ERROR("verify", "fork/exec of python3 client failed");
        return 4;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    int child_status = -1;
    bool timed_out = false;
    while (true) {
        window->surface().wait_events(20.0);  // 泵桥：方法调用 → 应答（与真实空闲帧同构）
        const pid_t r = ::waitpid(child, &child_status, WNOHANG);
        if (r == child) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &child_status, 0);
            timed_out = true;
            break;
        }
    }

    // ---- 4) 汇总判定 ----
    std::vector<ResLine> results;
    std::vector<std::string> notes;
    bool done = false;
    parse_client_output(read_text_file(out_path), results, notes, done);
    for (const std::string &note : notes) {
        emit(note);
    }
    emit(aurora_verify::pad_right("status", 8) + aurora_verify::pad_right("check", 26) + "detail");
    int failures = 0;
    for (const ResLine &r : results) {
        if (r.status == "fail") {
            ++failures;
        }
        emit(aurora_verify::pad_right(r.status, 8) + aurora_verify::pad_right(r.id, 26) + r.detail);
    }
    // 跨半程闭环：DoAction 应答过后，进程内 on_click 必须真的被触发。
    const bool do_action_ran =
        std::any_of(results.begin(), results.end(), [](const ResLine &r) { return r.id == "button-do-action" && r.status == "pass"; });
    if (do_action_ran) {
        const bool fired = g_press_executed;
        emit(aurora_verify::pad_right(fired ? "pass" : "fail", 8) +
             aurora_verify::pad_right("press-handler-side-effect", 26) +
             "on_click executed in-app (bridge routed action back to widget)");
        if (!fired) {
            ++failures;
        }
    }
    emit("summary: checks=" + aurora_verify::format_uint(results.size()) +
         ", failures=" + aurora_verify::format_int(failures) +
         (timed_out ? " [client TIMEOUT]" : done ? "" : " [client truncated (no DONE)]"));

    std::remove(script_path.c_str());
    std::remove(out_path.c_str());

    int rc = 0;
    if (timed_out || results.empty() || !done) {
        rc = 4;  // 客户端本身失能（崩溃/超时/无输出）——不能判桥通过，也不逐条归咎
    } else if (failures > 0) {
        rc = 5;
    }

    // ---- 5) 人工段：驻留泵桥，供 Accerciser / 手工脚本下钻 ----
    if (keep_seconds > 0) {
        emit("interactive: pumping for " + std::to_string(keep_seconds) +
             "s -- inspect with Accerciser (app \"" + kAppName + "\", frame \"" + kFrameTitle + "\")");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(keep_seconds);
        while (std::chrono::steady_clock::now() < end) {
            window->surface().wait_events(50.0);
        }
    }

    if (rc == 0) {
        emit("ALL PASS");
    } else {
        emit("FAILURES PRESENT (exit = " + std::to_string(rc) + ")");
    }
    return rc;
}
