#pragma once

// ============================================================
// E2E 平台语义通道 · Linux AT-SPI（tools/include/e2e/atspi_client.h）
// ------------------------------------------------------------
// 经 AT-SPI 客户端子进程读取平台语义投影，验证「aurora 语义树 → AtspiBridge →
// D-Bus → AT-SPI」整条链路确实到达平台。复用 `aurora_verify_atspi` 探针已验证的
// python3-gi（Atspi）客户端模式：客户端枚举桌面树找 app "Aurora" 下指定 FRAME，
// 先序输出 `NODE|<role_name>|<name>` 行与 `DONE` 结束行；父进程在等待期持续泵
// 窗口事件（桥在主线程应答跨进程查询，等待期不泵事件 = 客户端永远等不到投影）。
//
// 产出统一语义形状（`AccessibilityRole` + name）：与内核 `semantic_snapshot()`
// （harness.h）同词汇，用例侧断言代码跨平台复用。
//
// 门控：仅 POSIX 非 macOS 下有实现，其它平台包含本头得到空壳（`atspi_client_available()`
// 恒 false），用例据此 `AURORA_TEST_SKIP`。运行期合法降级链（逐级 skip，不算失败）：
// 无 python3-gi 客户端 → 无 org.a11y.Bus（桥按设计永久降级，`accessibility_provider()`
// 为空）→ 超时未找到目标 FRAME。
// ============================================================

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "e2e/harness.h"

#if defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS)

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace aurora::e2e {

/// @brief 平台投影的语义节点（统一词汇；AT-SPI role_name 映射后归并）。
struct AtspiSemanticNode {
    AccessibilityRole role = AccessibilityRole::Generic;  ///< role_name 映射后的统一角色
    std::string name;  ///< AT-SPI name（桥投影的 Name 回退链产物）
};

/// @brief AT-SPI role_name → 统一角色（未映射者归 Generic）。
[[nodiscard]] inline auto atspi_role_of(const std::string &role_name) -> AccessibilityRole {
    if (role_name == "push button") {
        return AccessibilityRole::Button;
    }
    if (role_name == "check box") {
        return AccessibilityRole::Checkbox;
    }
    if (role_name == "slider") {
        return AccessibilityRole::Slider;
    }
    if (role_name == "entry") {
        return AccessibilityRole::TextInput;
    }
    if (role_name == "static") {
        return AccessibilityRole::Text;
    }
    return AccessibilityRole::Generic;
}

/// @brief 本环境是否有 AT-SPI python3-gi 客户端（子进程探测，不建窗、不触桥）。
[[nodiscard]] inline auto atspi_client_available() -> bool {
    // NOLINTBEGIN(bugprone-command-processor, concurrency-mt-unsafe): 本工具的职责就是派发
    // 子进程命令探测 python3-gi 可用性；一次性探测、非并发路径。
    return std::system(
               "python3 -c \"import gi; gi.require_version('Atspi', '2.0'); "
               "from gi.repository import Atspi\"") == 0;
    // NOLINTEND(bugprone-command-processor, concurrency-mt-unsafe)
}

/// @brief 客户端脚本：枚举桌面树找 app "Aurora" 下名为 FRAME_TITLE 的 FRAME，先序输出。
///        目标标题经 execlp argv[1] 传入（脚本内 `sys.argv[1]`），不拼进脚本文本。
[[nodiscard]] inline auto atspi_client_script() -> std::string {
    return R"PY(
import sys, time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

APP_NAME = 'Aurora'
FRAME_TITLE = sys.argv[1]


def safe(fn, default=''):
    try:
        return fn()
    except Exception:
        return default


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


Atspi.init()
deadline = time.time() + 20.0
frame = None
while time.time() < deadline and frame is None:
    desktop = Atspi.get_desktop(0)
    for i in range(int(safe(desktop.get_child_count, 0) or 0)):
        app = desktop.get_child_at_index(i)
        if app is None or safe(app.get_name) != APP_NAME:
            continue
        bag = []
        collect(app, bag)
        for x in bag:
            if int(safe(x.get_role, -1) or -1) == Atspi.Role.FRAME and safe(x.get_name) == FRAME_TITLE:
                frame = x
                break
        if frame is not None:
            break
    if frame is None:
        time.sleep(0.4)

if frame is None:
    print('DONE', flush=True)
    sys.exit(0)

nodes = []
collect(frame, nodes)
for x in nodes:
    print('NODE|%s|%s' % (safe(x.get_role_name, '?'), safe(x.get_name, '')), flush=True)
print('DONE', flush=True)
)PY";
}

/// @brief 经 AT-SPI 客户端子进程采集指定 FRAME 的语义投影（先序）。
///
/// @param frame_title 目标窗口标题（`WindowSpec::title`，客户端按 FRAME name 定位）
/// @param out 先序采集结果（在既有内容之后追加）
/// @param pump 等待期回调（每轮轮询间调用；须推进窗口事件泵使桥能应答跨进程查询）
/// @param timeout_ms 父进程侧总等待上限（客户端自身另有 20s 桌面搜索上限）
/// @note Thread: main-thread only（pump 须在主线程）
[[nodiscard]] inline auto atspi_collect(const std::string &frame_title, std::vector<AtspiSemanticNode> &out,
                                        const std::function<void()> &pump, int timeout_ms) -> Result<void> {
    if (!atspi_client_available()) {
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported, "atspi_collect: python3-gi/Atspi client unavailable")};
    }

    const std::string script_path = "/tmp/aurora_e2e_atspi_" + std::to_string(::getpid()) + ".py";
    const std::string output_path = "/tmp/aurora_e2e_atspi_" + std::to_string(::getpid()) + ".out";
    {
        std::ofstream script(script_path, std::ios::trunc);
        script << atspi_client_script();
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        return Result<void>{make_error(ErrorCode::GeneralUnknown, "atspi_collect: fork failed")};
    }
    if (pid == 0) {
        // 子进程：stdout/stderr 重定向到输出文件，exec 客户端；exec 失败即退出非 0。
        ::freopen(output_path.c_str(), "w", stdout);
        ::freopen(output_path.c_str(), "a", stderr);
        ::execlp("python3", "python3", script_path.c_str(), frame_title.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto read_output = [&output_path]() {
        std::ifstream file(output_path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    };
    bool child_exited = false;
    std::string output;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pump) {
            pump();
        }
        int status = 0;
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            child_exited = true;
            output = read_output();
            break;
        }
        output = read_output();
        if (output.find("DONE") != std::string::npos) {
            (void)::waitpid(pid, &status, 0);  // 收尸，不阻塞（客户端已在收尾）
            child_exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!child_exited) {
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
    }
    std::remove(script_path.c_str());
    std::remove(output_path.c_str());

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.starts_with("NODE|")) {
            continue;
        }
        const auto first = line.find('|');
        const auto second = line.find('|', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }
        AtspiSemanticNode node;
        node.role = atspi_role_of(line.substr(first + 1, second - first - 1));
        node.name = line.substr(second + 1);
        out.push_back(std::move(node));
    }

    if (out.empty()) {
        return Result<void>{make_error(
            ErrorCode::GeneralNotSupported,
            child_exited ? "atspi_collect: no nodes (frame not found on the a11y desktop or projection empty)"
                         : "atspi_collect: client timeout before projection was collected")};
    }
    return Result<void>{};
}

}  // namespace aurora::e2e

#else  // 非 POSIX / macOS —— 空壳：用例据 atspi_client_available() 跳过

namespace aurora::e2e {

/// @brief 非目标平台恒不可用（平台能力事实，用例据此跳过）。
[[nodiscard]] inline auto atspi_client_available() -> bool { return false; }

}  // namespace aurora::e2e

#endif
