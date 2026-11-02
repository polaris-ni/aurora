/* Linux 系统双击速度真机验收探针（非 CTest）
// ============================================================================
// 为什么必须真机：`src/aurora/event/click_config.cpp` 的 Linux 分支把 GLib
// GSettings 读做成**运行时 dlopen / dlsym**（libgio-2.0 / libgobject-2.0），
// 构建期不碰任何桌面依赖，CI 与各开发机无需装 GLib 开发包即可编译。于是
// 「本机有没有 GLib」「有没有会话总线」「有没有装 mouse schema」这些差别只有真机
// 能分辨；无 GLib 的机器上该代码路径必然回退到库级常量，无从确认「跟随系统」是否真生效。
// 本探针就是这条路径在真实桌面上的机器级物证 + 交叉见证。
//
// 关于「是否真跟随了系统」：公共 API 只暴露 `os_double_click_ms()`（一个标量），
// 不暴露「当前是从哪个来源取的」。故本探针自己 **独立** 再经 dlopen 读一遍
// `org.gnome.desktop.peripherals.mouse → double-click` 作为见证值，与库返回值比对：
// 库若被改坏成「恒返回常量」，见证值仍读回真实系统值，二者不一致即 FAIL——这正是
// 无头 CI 抓不到、却决定「连击手感是否跟随系统」的那条回归。另以 `gsettings` CLI
// 作第二见证（仅 WARN 提示，不判失败）。
//
// 自动段逐项验收（不需要人工看屏幕）：
//   ① 环境核对：libgio-2.0.so.0 / libgobject-2.0.so.0 是否可 dlopen、gsettings CLI 是否
//      在 PATH、DBUS_SESSION_BUS_ADDRESS 是否设置——逐项打印，只作诊断，不作判据。
//   ② 范围合理性：os_double_click_ms() ∈ [1, 10000]，否则 FAIL。
//   ③ 跟随系统 / 优雅回退二选一断言：
//        - 见证可读（GLib 在、schema 在、值 > 0）：断言 库返回值 == 见证值（证明真跟随系统）；
//        - 见证不可读（缺 GLib 或 schema 未装）：断言 库返回值 == AURORA_DEFAULT_CLICK_WINDOW_MS
//          （证明缺环境时优雅回退，而非静默失真）。
//   ④ 第二见证交叉核对：gsettings CLI 可读且与库/见证值不一致时 WARN（信息提示，不判失败）。
// 人工段（--interactive）：无——本探针全自动化即可得出结论（系统值不依赖屏幕观察）。
//
// 退出码：
//   0  自动段全部通过（WARN 不影响退出码）
//   1  自动段存在 FAIL（范围越界 / 未跟随系统 / 未优雅回退）
//
// 构建（目标由 cmake/AuroraVerify.cmake 注册；需 -DAURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BUILD_VERIFY_TOOLS=ON -DAURORA_BACKEND_X11=ON
//   cmake --build build-verify --target aurora_verify_linux_double_click
//   ./build-verify/aurora_verify_linux_double_click                 # 自动段
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
#error "aurora_verify_linux_double_click can only be built on desktop Linux"
#endif

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "aurora/event/event.h"
#include "verify_args.h"

namespace {

/// 失败计数：函数局部静态 + 访问器收敛（探针退出码的依据），避免命名空间级可变全局量。
auto failures() -> int & {
    static int count = 0;
    return count;
}

/// 警告计数：同上。
auto warnings() -> int & {
    static int count = 0;
    return count;
}

/// @brief 写一行探针输出到 stdout（走 raw 通道：无前缀、不过级别过滤）。
/// @param text 待输出的一行文本。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记一项自动判据结论。
/// @param ok 判据是否成立。
/// @param label 结论文案（英文，ASCII）。
auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        ++failures();
    }
}

/// @brief 记一条不判失败的提示（环境差异由真机决定，人工复核）。
/// @param label 提示文案。
auto warn(const std::string &label) -> void {
    emit("[WARN] " + label);
    ++warnings();
}

// GLib GSettings 符号 ABI（取自 GLib 2.x，长期稳定，与 click_config.cpp 同源）：
//   GSettings *g_settings_new(const gchar *schema_id);
//   gint      g_settings_get_int(GSettings *settings, const gchar *key);
//   void      g_object_unref(gpointer object);
using FnGSettingsNew = void *(*)(const char *);
using FnGSettingsGetInt = int (*)(void *, const char *);
using FnGObjectUnref = void (*)(void *);

/// @brief 库能否被 `dlopen` 打开（只看加载，不解析符号）。
/// @param soname 目标库的 soname。
/// @return 打开成功为真。
[[nodiscard]] auto probe_library(const char *soname) -> bool {
    void *handle = ::dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) {
        return false;
    }
    ::dlclose(handle);
    return true;
}

/// @brief 外壳命令是否在 PATH 里（`command -v` 避开别名与 shell 内建干扰）。
/// @param name 命令名。
/// @return 命令可解析为真。
[[nodiscard]] auto probe_command(const char *name) -> bool {
    const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
    // NOLINTNEXTLINE(cert-env33, bugprone-command-processor)
    return std::system(command.c_str()) == 0;
}

/// @brief 独立见证：再用 dlopen 读一遍系统双击速度，作为库返回值的对照。
/// @return GLib 可读到系统值（>0）时返回该值；缺 GLib / 无 schema / 值非法时返回 nullopt
///         （此时库应回退到 `AURORA_DEFAULT_CLICK_WINDOW_MS`）。
/// @note 与 `src/aurora/event/click_config.cpp::linux_double_click_ms()` 同机制、独立调用栈，
///       专门用于抓「库被改坏成恒返回常量」这类回归——若库坏了，本函数仍读回真实值，比对即 FAIL。
[[nodiscard]] auto witness_double_click_ms() -> std::optional<int> {
    void *gio = ::dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (gio == nullptr) {
        gio = ::dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
    }
    void *gobj = ::dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (gobj == nullptr) {
        gobj = ::dlopen("libgobject-2.0.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (gio == nullptr || gobj == nullptr) {
        if (gio != nullptr) {
            ::dlclose(gio);
        }
        if (gobj != nullptr) {
            ::dlclose(gobj);
        }
        return std::nullopt;  // 缺 GLib：库必回退常量
    }

    FnGSettingsNew settings_new = nullptr;
    FnGSettingsGetInt settings_get_int = nullptr;
    FnGObjectUnref object_unref = nullptr;
    bool ok = true;
    const auto bind = [&ok](const char *name, void *h, auto &dst) {
        void *p = ::dlsym(h, name);
        if (p == nullptr) {
            ok = false;
            return;
        }
        // dlsym 只返回 void*，目标是函数指针别名：逐字节 memcpy 落值是刻意避开 reinterpret_cast
        // （对象指针→函数指针在 POSIX/dlopen ABI 下合法但不被标准允许）。
        // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast,bugprone-multi-level-implicit-pointer-conversion)
        std::memcpy(&dst, &p, sizeof(dst));
    };
    bind("g_settings_new", gio, settings_new);
    bind("g_settings_get_int", gio, settings_get_int);
    bind("g_object_unref", gobj, object_unref);
    if (!ok) {
        ::dlclose(gio);
        ::dlclose(gobj);
        return std::nullopt;
    }

    void *settings = settings_new("org.gnome.desktop.peripherals.mouse");
    if (settings == nullptr) {
        ::dlclose(gio);
        ::dlclose(gobj);
        return std::nullopt;  // schema 未装：库同样回退常量
    }
    const int ms = settings_get_int(settings, "double-click");
    object_unref(settings);
    ::dlclose(gio);
    ::dlclose(gobj);
    if (ms <= 0) {
        return std::nullopt;
    }
    return ms;
}

/// @brief 第二见证：gsettings CLI 读到的双击速度（仅作信息提示的对照）。
/// @return CLI 可读到整值时返回该值；否则 nullopt。
[[nodiscard]] auto witness_double_click_cli() -> std::optional<int> {
    if (!probe_command("gsettings")) {
        return std::nullopt;
    }
    // 命令串是常量拼接，只做 PATH 查询与只读取值，不含外部输入。
    const std::string command = "gsettings get org.gnome.desktop.peripherals.mouse double-click 2>/dev/null";
    // NOLINTNEXTLINE(cert-env33, bugprone-command-processor)
    std::string raw;
    {
        // NOLINTNEXTLINE(readability-isolate-declaration)
        FILE *pipe = ::popen(command.c_str(), "r");
        if (pipe == nullptr) {
            return std::nullopt;
        }
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            raw.append(buffer);
        }
        // NOLINTNEXTLINE(readability-isolate-declaration)
        int status = ::pclose(pipe);
        static_cast<void>(status);
    }
    // gsettings 输出形如 "400" 或 "uint32 400"：取最后一个连续整数段。
    int parsed = 0;
    bool found = false;
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] >= '0' && raw[i] <= '9') {
            std::size_t j = i;
            long value = 0;
            while (j < raw.size() && raw[j] >= '0' && raw[j] <= '9') {
                value = value * 10 + (raw[j] - '0');
                ++j;
            }
            parsed = static_cast<int>(value);
            found = true;
            i = j;
        } else {
            ++i;
        }
    }
    if (!found || parsed <= 0) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针、examples/ 各 demo 入口同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Linux system double-click speed live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    static_cast<void>(interactive);  // 本探针全自动化，--interactive 仅保持旗标兼容性

    emit("== Aurora Linux system double-click speed live probe ==");
    emit("verifies aurora::os_double_click_ms() follows the system setting (or falls back cleanly)");

    // ---- ① 环境核对 ----
    emit("");
    emit("[ENV] GLib / gsettings availability (the library reads the system value through these at runtime):");
    const bool has_gio = probe_library("libgio-2.0.so.0") || probe_library("libgio-2.0.so");
    const bool has_gobject = probe_library("libgobject-2.0.so.0") || probe_library("libgobject-2.0.so");
    emit(std::string("       libgio-2.0            : ") + (has_gio ? "loadable" : "absent"));
    emit(std::string("       libgobject-2.0        : ") + (has_gobject ? "loadable" : "absent"));
    emit(std::string("       gsettings CLI         : ") + (probe_command("gsettings") ? "on PATH" : "absent"));
    const char *bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    emit(std::string("       DBUS_SESSION_BUS_ADDRESS: ") + ((bus_address != nullptr) ? bus_address : "(unset)"));

    // ---- ② 范围合理性 ----
    emit("");
    const std::uint32_t value = aurora::os_double_click_ms();
    emit("os_double_click_ms() = " + std::to_string(value) + " ms");
    check(value >= 1U && value <= 10000U,
          "returned value is within sane bounds [1, 10000] ms (got " + std::to_string(value) + ")");

    // ---- ③ 跟随系统 / 优雅回退二选一断言 ----
    emit("");
    const std::optional<int> witness = witness_double_click_ms();
    if (witness.has_value()) {
        emit("[WITNESS] GSettings double-click read independently = " + std::to_string(*witness) + " ms");
        check(value == static_cast<std::uint32_t>(*witness),
              "os_double_click_ms() follows the system value read from GLib GSettings (" + std::to_string(value) +
                  " == " + std::to_string(*witness) + ")");
    } else {
        emit(
            "[WITNESS] GLib GSettings not readable on this machine (missing libgio / schema / session) -> "
            "expecting clean fallback to the library constant");
        check(value == aurora::AURORA_DEFAULT_CLICK_WINDOW_MS,
              "os_double_click_ms() falls back to AURORA_DEFAULT_CLICK_WINDOW_MS when GSettings is unreadable (" +
                  std::to_string(value) + " == " + std::to_string(aurora::AURORA_DEFAULT_CLICK_WINDOW_MS) + ")");
    }

    // ---- ④ 第二见证交叉核对（仅 WARN） ----
    const std::optional<int> cli_witness = witness_double_click_cli();
    if (cli_witness.has_value()) {
        emit("[CLI] gsettings CLI reports double-click = " + std::to_string(*cli_witness) + " ms");
        if (witness.has_value() && *cli_witness != *witness) {
            warn("gsettings CLI (" + std::to_string(*cli_witness) + ") disagrees with the GLib GSettings read (" +
                 std::to_string(*witness) +
                 "); both are independent of the library, investigate the desktop environment");
        } else if (!witness.has_value() && *cli_witness != static_cast<int>(aurora::AURORA_DEFAULT_CLICK_WINDOW_MS)) {
            warn("gsettings CLI sees " + std::to_string(*cli_witness) +
                 " ms but the library could not read GSettings (fell back to the constant) — possible "
                 "dlopen/session gap worth checking on this machine");
        }
    }

    emit("");
    if (failures() > 0) {
        emit("result: " + std::to_string(failures()) + " FAILURE(S), " + std::to_string(warnings()) +
             " warning(s) (exit 1)");
        return 1;
    }
    emit("result: ALL PASS, " + std::to_string(warnings()) + " warning(s) (exit 0)");
    return 0;
}
