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
//      「读屏反向操作真的点到了控件」（跨半程闭环）；
//   5) **事件段**：客户端注册 7 个精确事件类型监听（libatspi 的匹配是**全串相等**，
//      无前缀通配）后打印 `LISTENING` 并进入 `GLib.MainLoop`；父进程看到标志后按时间
//      表做声明式变更（改标题 / 改输入值 / 收缩再增长 ReorderableList / 播报），客户端
//      另在环内 +1.5s 主动 `grab_focus`。每条事件都是桥**推送**上总线、客户端被动收取
//      ——与静态查询段互补，证明「无查询也有事件」的推模式成立。
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
// 事件段验收项（同上 RES 行；全部由桥**主动推送**，客户端只被动收取）：
//   evt-focus / evt-state-focused      —— Event.Focus 与 state-changed:focused(on=1)，
//                                        源 = 客户端 grab_focus 抓到的 entry
//   evt-prop-value                     —— object:property-change:accessible-value（父进程 set_value）
//   evt-prop-name                      —— object:property-change:accessible-name @ frame
//                                        （父进程 Window::set_title ⇒ FRAME 合成节点直发）
//   evt-children-remove / evt-children-add —— object:children-changed（ReorderableList 先减后加；
//                                        同批还推送 Cache.Add/RemoveAccessible —— 二者不经
//                                        事件监听器投递，其行格式错误会以子树解析异常暴露）
//   evt-announcement                   —— object:announcement + detail1=POLITE(1)
//                                        （announce_accessibility 直发，不经 diff）
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
//   事件段仍在的推迟项（见 atspi_bridge.h 头注申报）：window:* 事件、text-changed /
//   text-caret-moved、Bounds/Range/Actions 类变化（无规范事件词汇，客户端靠重读）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_atspi can only be built on Linux/Unix (non-Apple)"
#endif
#if !defined(AURORA_BACKEND_X11) && !defined(AURORA_BACKEND_WAYLAND)
#error "AURORA_BACKEND_X11 or AURORA_BACKEND_WAYLAND must be enabled"
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/reorderable_list.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

constexpr auto AURORA_AT_SPI_APP_NAME = "Aurora";  ///< 桥 env.app_name（宿主接线固定值）
constexpr auto AURORA_AT_SPI_FRAME_TITLE = "Aurora AT-SPI verify";  ///< FRAME Name（窗口标题）

bool g_press_executed = false;  ///< DoAction 跨半程闭环证据（on_click 在 UI 线程同步置位）

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 客户端脚本（libatspi 官方 GI 绑定，与 Accerciser 同栈）。逐断言打 RES 行。
constexpr auto AURORA_AT_SPI_CLIENT_SCRIPT = R"PY(
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

# ---- 事件段：注册精确类型监听 → LISTENING → GLib 事件环（本进程零查询，事件全靠桥推）----
from gi.repository import GLib

events = []

def on_evt(e, *user_data):  # GI 可能附带 user_data 形参，一律吃掉
    try:
        src = e.source
        if src is None:
            return
        a = safe(src.get_application, None)
        if a is None or safe(a.get_name, '') != app_name:
            return  # 同总线其他应用的事件不入账
        events.append((getattr(e, 'type', '') or '', safe(src.get_role_name, '?'),
                       safe(src.get_name, ''), int(getattr(e, 'detail1', 0) or 0)))
    except Exception as ex:
        info('evt-cb-error', repr(ex))

# libatspi 匹配为全串相等（is_superset），故逐个注册带 minor 的完整类型；
# announcement 的 minor 为空 ⇒ 类型串无尾冒号（上游 handle_event 拼装规则）。
listeners = []
for t in ('focus:',
          'object:state-changed:focused',
          'object:property-change:accessible-value',
          'object:property-change:accessible-name',
          'object:children-changed:add',
          'object:children-changed:remove',
          'object:announcement'):
    l = Atspi.EventListener.new(on_evt)
    if safe(lambda: l.register(t), False):
        listeners.append(l)
    else:
        res('fail', 'evt-register', 'register(%s) rejected' % t)

print('LISTENING', flush=True)

loop = GLib.MainLoop()
# GI 里 GLib.Timeout 是不透明类型（无类方法），用模块级 GLib.timeout_add；回调返 None=假 ⇒ 一次性
GLib.timeout_add(25000, lambda: loop.quit())

def grab():
    if ent is not None:
        info('grab-focus', int(bool(safe(lambda: Atspi.Component.grab_focus(ent), False))))
    return False

GLib.timeout_add(1500, grab)
loop.run()

for ev in events:
    print('EVT|%s|%s|%s|%d' % ev, flush=True)
info('event-count', len(events))

def saw(typ, role=None, d1=None):
    return [x for x in events if x[0] == typ and (role is None or x[1] == role)
            and (d1 is None or x[3] == d1)]

if ent is None:
    res('skip', 'evt-focus', 'no entry to grab')
    res('skip', 'evt-state-focused', 'no entry')
else:
    res('pass' if saw('focus:', 'entry') else 'fail', 'evt-focus',
        'Event.Focus pushed after client grab_focus')
    res('pass' if saw('object:state-changed:focused', 'entry', 1) else 'fail',
        'evt-state-focused', 'minor=focused detail1=1')
res('pass' if saw('object:property-change:accessible-value', 'entry') else 'fail',
    'evt-prop-value', 'host set_value pushed')
res('pass' if saw('object:property-change:accessible-name', 'frame') else 'fail',
    'evt-prop-name', 'host set_title pushed on FRAME')
res('pass' if saw('object:children-changed:remove') else 'fail',
    'evt-children-remove', 'list shrink pushed')
res('pass' if saw('object:children-changed:add') else 'fail',
    'evt-children-add', 'list regrow pushed')
an = saw('object:announcement')
res('pass' if any(x[3] == 1 for x in an) else 'fail', 'evt-announcement',
    'detail1(POLITE)=1 sources=%s' % sorted({x[1] for x in an}))

print('DONE', flush=True)
)PY";

/// @brief 探针控件列 + 事件段变更所需的可变句柄（判据见头注「事件段」）。
struct ProbeWidgets {
    aurora::Node root;
    aurora::TextInput *entry = nullptr;  ///< set_value → property-change:accessible-value
    aurora::ReorderableList<std::string> *list = nullptr;  ///< 收缩/增长 → children-changed
    std::shared_ptr<aurora::State<std::vector<std::string>>> items;
};

/// @brief 一列带无障碍语义的控件（与 Win32 UIA 探针同族的三件套 + ReorderableList）。
[[nodiscard]] auto build_probe_column() -> ProbeWidgets {
    auto button = aurora::Button{aurora::ButtonProps{.label = aurora::LocalizedString{"确定"}}};
    button.set_on_click([] { g_press_executed = true; });
    auto items = std::make_shared<aurora::State<std::vector<std::string>>>(
        std::vector<std::string>{"订单一", "订单二", "订单三"});
    // Node 以 shared_ptr 持有 widget：先建节点取裸指针，再拷进 Column（同一实例）。
    aurora::Node entry_node{aurora::TextInput{aurora::TextInputProps{.value = "abc", .placeholder = "请输入"}}};
    aurora::Node list_node{aurora::ReorderableList<std::string>{
        items,
        [](const std::string &label, int) {
            return aurora::Node{aurora::Text{aurora::TextProps{.content = aurora::LocalizedString{label}}}};
        },
        4.0F}};
    ProbeWidgets probe;
    probe.entry = static_cast<aurora::TextInput *>(&entry_node.widget());
    probe.list = static_cast<aurora::ReorderableList<std::string> *>(&list_node.widget());
    probe.items = std::move(items);
    probe.root = aurora::Node{aurora::Column{
        std::move(button),
        entry_node,
        list_node,
        aurora::Text{aurora::TextProps{.content = aurora::LocalizedString{"订单总额"}}},
    }};
    return probe;
}

/// @brief python3 + gi/Atspi 可用性（阻塞调用即可：此步不触本进程桥）。
[[nodiscard]] auto python_atspi_available() -> bool {
    return std::system(
               "python3 -c \"import gi; gi.require_version('Atspi','2.0'); "
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
        execlp("python3", "python3", script_path.c_str(), AURORA_AT_SPI_APP_NAME, AURORA_AT_SPI_FRAME_TITLE,
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

/// @brief 拆出 RES / TREE / INFO / EVT 四类行（RES 参与判定；其余原样转录）。
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
        } else if (line.rfind("TREE|", 0) == 0 || line.rfind("INFO|", 0) == 0 || line.rfind("EVT|", 0) == 0) {
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
    // 命令行面即声明表：--x11 / --wayland 选宿主，--interactive[=秒] 进人工驻留段。
    const auto spec = aurora::cli::CommandSpec{
        .name = "aurora_verify_atspi",
        .about = "AT-SPI2 semantic tree live probe",
        .options = {aurora::cli::OptionSchema{.long_name = "x11",
                                              .kind = aurora::cli::ValueKind::Bool,
                                              .arity = aurora::cli::Arity::flag(),
                                              .help = "Force the X11/XWayland path (unset WAYLAND_DISPLAY)"},
                    aurora::cli::OptionSchema{.long_name = "wayland",
                                              .kind = aurora::cli::ValueKind::Bool,
                                              .arity = aurora::cli::Arity::flag(),
                                              .help = "Require the Wayland path (fail if WAYLAND_DISPLAY is unset)"},
                    aurora::cli::OptionSchema{.long_name = "interactive",
                                              .kind = aurora::cli::ValueKind::Int,
                                              .arity = aurora::cli::Arity::optional_one(),
                                              .help = "Stay alive for manual inspection (Accerciser / Orca)",
                                              .value_hint = "SECONDS",
                                              .default_text = "30",
                                              .minimum = 1}}};
    const auto cli = aurora_verify::parse_command_line(spec, argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool want_x11 = cli.arguments->flag("x11");
    const bool want_wayland = cli.arguments->flag("wayland");
    // 驻留只在**显式**给出 --interactive 时生效（默认值 30 是写给帮助看的，不代表要驻留）。
    int keep_seconds = 0;
    if (cli.arguments->explicitly_given("interactive")) {
        const auto seconds = cli.arguments->get<int>("interactive");
        keep_seconds = seconds.ok() ? seconds.value() : 30;
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
        .size = aurora::Size{.width = 480.0F, .height = 320.0F}, .title = AURORA_AT_SPI_FRAME_TITLE});
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

    ProbeWidgets probe = build_probe_column();
    aurora::Node &root = probe.root;
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
    emit("bridge = " + std::string{bridge->name()} + ", active = " + (bridge->is_active() ? "true" : "false"));

    // ---- 3) 起跨进程 libatspi 客户端；父进程持续泵桥（客户端每个调用都等我们应答）----
    const std::string tag = std::to_string(static_cast<long>(::getpid()));
    const std::string script_path = "/tmp/aurora_verify_atspi_client_" + tag + ".py";
    const std::string out_path = "/tmp/aurora_verify_atspi_out_" + tag + ".txt";
    if (!write_text_file(script_path, AURORA_AT_SPI_CLIENT_SCRIPT)) {
        AURORA_LOG_ERROR("verify", "Cannot write client script to " + script_path);
        return 4;
    }
    const pid_t child = spawn_client(script_path, out_path);
    if (child <= 0) {
        AURORA_LOG_ERROR("verify", "fork/exec of python3 client failed");
        return 4;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    int child_status = -1;
    bool timed_out = false;
    bool child_exited = false;
    auto pump_ms = [&](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            window->surface().wait_events(20.0);
        }
    };

    // A 段：等客户端打出 LISTENING（监听器就绪）；期间继续泵桥——静态段每个跨进程
    // 查询都阻塞在本进程应答上。
    bool listening = false;
    while (true) {
        window->surface().wait_events(20.0);
        if (read_text_file(out_path).find("LISTENING") != std::string::npos) {
            listening = true;
            break;
        }
        if (::waitpid(child, &child_status, WNOHANG) == child) {
            child_exited = true;  // 客户端早逝：交给 C 段与汇总判 rc=4，不做变更
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &child_status, 0);
            timed_out = true;
            break;
        }
    }

    // B 段：声明式变更时间表 —— 每条 AT-SPI 事件都由桥在这些调用后的泵帧内**主动推**
    // 上总线（推模式证明；客户端只在环内 +1.5s 自己 grab_focus 一次）。
    if (listening && !child_exited && !timed_out) {
        auto list_state = [&](const char *tag) {
            std::string s;
            for (const auto &it : probe.items->get()) {
                s += it + ',';
            }
            emit(std::string("dbg ") + tag + ": data=[" + s + "] rendered=" + std::to_string(probe.list->item_count()));
        };
        pump_ms(800);  // 让客户端进入 GLib 事件环并挂好 grab_focus 定时器
        window->set_title(std::string{AURORA_AT_SPI_FRAME_TITLE} + " v2");  // FRAME property-change:accessible-name
        pump_ms(500);
        probe.entry->set_value("abcd");  // entry property-change:accessible-value
        pump_ms(500);
        auto shrink = probe.items->get();
        shrink.pop_back();
        probe.items->set(shrink);
        probe.list->invalidate();
        // 探针无响应式驱动：仅 invalidate 排不上新一轮布局 ⇒ 重注根强制新帧，
        // rebuild_if_needed 在 on_layout 落位（结构移除……）
        (void)window->present_root(root);
        pump_ms(500);
        list_state("after-shrink");
        probe.entry->set_value("abcde");  // ……再由这次 dirty 携带 remove diff 出帧
        pump_ms(500);
        auto grow = probe.items->get();
        grow.push_back("订单三");
        probe.items->set(grow);
        probe.list->invalidate();
        (void)window->present_root(root);  // 结构新增：AddAccessible 行先于 children-changed:add
        pump_ms(500);
        list_state("after-regrow");
        probe.entry->set_value("abcdef");
        aurora::announce_accessibility("测试播报", probe.entry);  // 播报直发，不经 diff
    }

    // C 段：等客户端事件环到期（25s）并完成断言输出。
    while (!child_exited && !timed_out) {
        window->surface().wait_events(20.0);
        if (::waitpid(child, &child_status, WNOHANG) == child) {
            child_exited = true;
        } else if (std::chrono::steady_clock::now() > deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &child_status, 0);
            timed_out = true;
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
        std::ranges::any_of(results, [](const ResLine &r) { return r.id == "button-do-action" && r.status == "pass"; });
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
         (timed_out ? " [client TIMEOUT]"
          : done    ? ""
                    : " [client truncated (no DONE)]"));

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
        emit("interactive: pumping for " + std::to_string(keep_seconds) + "s -- inspect with Accerciser (app \"" +
             AURORA_AT_SPI_APP_NAME + "\", frame \"" + AURORA_AT_SPI_FRAME_TITLE + "\")");
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
