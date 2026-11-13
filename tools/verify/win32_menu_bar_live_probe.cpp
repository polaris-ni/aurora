// tools/verify/win32_menu_bar_live_probe.cpp — Win32 原生菜单栏真机验收探针（非 CTest）。
//
// 覆盖：`NativeMenuBar` 经 Win32 桥（src/aurora/window/detail/win32_menu_bar.cpp）落到真实 OS 菜单栏的
// 整条接线——`CreateMenu` / `CreatePopupMenu` / `AppendMenu` / `SetMenu` 建树并挂到窗口、`WM_COMMAND`
// 经 `Win32MenuBarBridge::on_command` 按命令 id 反查模型 id 回投点击、重复 install 替换旧树、uninstall
// 摘下菜单。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * 单测只能在**假后端**上验折算计划、安装-替换-卸载契约与点击路由（utest_menu_bar_translate.cpp），
//     它无法证明 `CreateMenu` / `SetMenu` 真的在真实窗口上建出了 `HMENU`、层级条数对齐；
//   * 真实 Win32 菜单对象的层级（顶层项数 / 子菜单项数）只有读 `GetMenu` / `GetMenuItemCount` 才证得了；
//   * 点击回投走的是 `WM_COMMAND` 派发路径，单测直接调门面绕过了它——本探针用桥的 `on_command` 入口
//     （消息泵正是转发到这里）逐 leaf 回投，验证「稳定 id 正确回到路由回调、空 id 不回投」。
//
// 取证口径（关键：证据链**独立于**被测桥的私有记账）：
//   探针直接读 `GetMenu(hwnd)` / `GetMenuItemCount` / `GetSubMenu` 核对原生菜单层级，不读桥内部
//   的 `id_map_`——否则「桥记了账但 OS 没建树」这类缺陷会被自证。
//
// 构建（方式 ① 推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_menu_bar
//   build-verify\aurora_verify_win32_menu_bar.exe
//
// 运行：命令行直接跑即可。探针自建一个**隐藏窗口**（不展示、不进消息循环）——它的存在只为了给
//   `SetMenu` 一个真实 `HWND`；菜单项点击由探针**直接调 `bridge.on_command(cmd)`** 确定性回投，
//   不走真实 `WM_COMMAND` 派发（那需要消息循环且时序不可控）。
//
// 退出码：
//   0  自动段全部通过
//   1  自动段断言失败
//   2  环境不可用（能力位为假 / 窗口创建失败）
//   7  人工段存在未确认项（本探针无人工段，沿用同目录口径保留该码）

#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_menu_bar can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled for aurora_verify_win32_menu_bar"
#endif

// 与库内同口径：先在**任何**平台头之前定好这两个宏，避免 <windows.h> 的 min/max 宏污染标准库用法。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
// clang-format on

#include <iostream>
#include <string>
#include <vector>

#include "aurora/app/menu.h"  // MenuModel
#include "aurora/core/log.h"
#include "aurora/window/detail/menu_bar_translate.h"  // flatten_menu_models / FlatMenuItem / AURORA_MENU_BAR_NO_PARENT
#include "aurora/window/detail/win32_menu_bar.h"  // Win32MenuBarBridge / AURORA_WIN32_MENU_COMMAND_BASE
#include "aurora/window/platform.h"  // Platform / SurfaceKind / PlatformCapabilities
#include "verify_args.h"
#include "verify_print.h"

namespace {

auto failures() -> int & {
    static int count = 0;
    return count;
}
auto env_unavailable() -> bool & {
    static bool flag = false;
    return flag;
}

/// @brief 输出一行探针结论到 stdout（无日志前缀、不过级别过滤）。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        ++failures();
    }
}

auto env_fail(const std::string &label, const std::string &hint) -> void {
    emit(std::string("[ENV ] ") + label);
    emit("Hint: " + hint);
    env_unavailable() = true;
}

/// @brief 收集前序遍历里的终端项（非分隔符、无孩子的项）模型 id，顺序即命令 id 派发顺序。
[[nodiscard]] auto terminal_ids(const std::vector<aurora::menu_bar_translate::FlatMenuItem> &flat)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        if (flat[i].separator) {
            continue;
        }
        const bool has_children = (i + 1U < flat.size()) && flat[i + 1U].depth == flat[i].depth + 1U;
        if (!has_children) {
            out.push_back(flat[i].id);
        }
    }
    return out;
}

/// @brief 递归核对原生菜单层级与扁平项表一致（顶层 + 各子菜单）。
/// @param hmenu 待核对的原生菜单（顶层菜单栏或某子菜单）。
/// @param flat 扁平项表。
/// @param parent_index 本菜单内所有项的父项下标；顶层菜单栏传 `AURORA_MENU_BAR_NO_PARENT`（无父）。
/// @param ok 累计一致性判据（引用回写）。
///
/// 证据独立于桥私有记账：直接读 Win32 的 `GetMenuItemCount` / `GetSubMenu`，按 `parent_index`
/// 收集本菜单的孩子（前序 ⇒ 与 native 顺序一致），有孩子的孩子再下钻核对子菜单。
auto verify_menu_children(HMENU hmenu, const std::vector<aurora::menu_bar_translate::FlatMenuItem> &flat,
                          std::size_t parent_index, bool &ok) -> void {
    const int raw = GetMenuItemCount(hmenu);
    if (raw < 0) {
        ok = false;
        return;
    }
    const auto count = static_cast<std::size_t>(raw);
    // 本菜单的孩子 = parent_index 指向本父项的扁平项（前序，故与 native 顺序一致）。
    std::vector<std::size_t> kids;
    kids.reserve(count > 0U ? count : 1U);
    for (std::size_t i = 0; i < flat.size(); ++i) {
        if (flat[i].parent_index == parent_index) {
            kids.push_back(i);
        }
    }
    if (kids.size() != count) {
        ok = false;
        emit("  hierarchy mismatch: native count " + aurora_verify::format_uint(count) + " != model " +
             aurora_verify::format_uint(kids.size()) + " (parent flat[" + aurora_verify::format_uint(parent_index) +
             "])");
        return;
    }
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t ci = kids[k];
        // 孩子是否还有孩子：前序下紧随其后且深度 +1 即本项的弹出子菜单。
        const bool child_has_children = (ci + 1U < flat.size()) && flat[ci + 1U].depth == flat[ci].depth + 1U;
        if (!child_has_children) {
            continue;
        }
        const HMENU sub = GetSubMenu(hmenu, static_cast<int>(k));
        if (sub == nullptr) {
            ok = false;
            emit("  missing submenu at flat[" + aurora_verify::format_uint(ci) + "]");
        } else {
            verify_menu_children(sub, flat, ci, ok);
        }
    }
}

// 窗口类名（本探针私有）。
constexpr const wchar_t *AURORA_MENU_BAR_PROBE_CLASS = L"AuroraVerifyMenuBar";

/// @brief 最小窗口过程：本探针不跑消息循环，全部消息交默认处理即可。
auto CALLBACK probe_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/// @brief 自动段：全部无人值守判据。
/// @return 退出码：0 通过；1 断言失败；2 环境不可用。
auto run_auto_stage() -> int {
    emit("---- auto segment ----");

    // 1. 能力位核对。为假说明本腿没接桥，后续判据都会因「unsupported」失败——同一件事的重复报错。
    const aurora::Platform snapshot = aurora::platform();
    const bool supported = snapshot.capabilities().native_menu_bar;
    check(supported, "platform capability: native_menu_bar is true for this surface");
    if (!supported) {
        env_fail("no native menu bar for the detected surface",
                 "Build with AURORA_BACKEND_WIN32=ON (or D3D11) and re-run.");
        return 2;
    }

    // 2. 建一个隐藏窗口给 SetMenu 当 HWND（不展示、不进消息循环）。
    WNDCLASSW wc{};
    wc.lpfnWndProc = &probe_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = AURORA_MENU_BAR_PROBE_CLASS;
    if (RegisterClassW(&wc) == 0) {
        env_fail("cannot register the probe window class", "Re-run in a normal desktop session.");
        return 2;
    }
    const HWND hwnd =
        CreateWindowExW(0, AURORA_MENU_BAR_PROBE_CLASS, L"AuroraMenuBarProbe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                        CW_USEDEFAULT, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        env_fail("cannot create the probe window", "Re-run in a normal desktop session.");
        return 2;
    }

    // 3. 构造桥并装一份典型菜单树：File(>New, 分隔符, Open(>Recent(>doc))) + Edit(>Undo)。
    aurora::detail::Win32MenuBarBridge bridge{hwnd};

    std::vector<std::string> clicks;
    auto on_click = [&clicks](std::string id) -> void { clicks.push_back(std::move(id)); };

    aurora::MenuModel file;
    file.id = "file";
    file.label = "File";
    aurora::MenuModel nw;
    nw.id = "file.new";
    nw.label = "New";
    nw.shortcut_text = "Ctrl+N";
    aurora::MenuModel sep;
    sep.separator = true;
    aurora::MenuModel open;
    open.id = "file.open";
    open.label = "Open";
    open.checkable = true;
    open.checked = true;
    aurora::MenuModel recent;
    recent.id = "file.open.recent";
    recent.label = "Open Recent";
    aurora::MenuModel doc;
    doc.id = "file.open.recent.1";
    doc.label = "doc1.txt";
    recent.children.push_back(std::move(doc));
    open.children.push_back(std::move(recent));
    file.children.push_back(std::move(nw));
    file.children.push_back(std::move(sep));
    file.children.push_back(std::move(open));

    aurora::MenuModel edit;
    edit.id = "edit";
    edit.label = "Edit";
    aurora::MenuModel undo;
    undo.id = "edit.undo";
    undo.label = "Undo";
    undo.enabled = false;
    edit.children.push_back(std::move(undo));

    // 一个空 id 的终端项：建出但点击不回投（留 WARN）。
    aurora::MenuModel anonymous;
    anonymous.label = "NoId";
    aurora::MenuModel sub_of_anon;
    sub_of_anon.label = "NoIdChild";  // 同样空 id 的子项，整棵空 id 子树都应建出
    anonymous.children.push_back(std::move(sub_of_anon));

    std::vector<aurora::MenuModel> models;
    models.push_back(std::move(file));
    models.push_back(std::move(edit));
    models.push_back(std::move(anonymous));

    const auto flat = aurora::menu_bar_translate::flatten_menu_models(models);
    const auto res = bridge.install(flat, on_click);
    check(res.ok(), std::string("install: Ok (got ") + (res.ok() ? std::string{"Ok"} : res.error().code) + ")");
    check(bridge.is_installed(), "install: is_installed() == true after install");

    // 4. 真实 Win32 菜单层级核对（证据独立于桥私有记账）。
    const HMENU hmenubar = GetMenu(hwnd);
    bool hierarchy_ok = (hmenubar != nullptr);
    check(hmenubar != nullptr, "OS menu: GetMenu(hwnd) returns the menu bar HMENU");
    if (hmenubar != nullptr) {
        verify_menu_children(hmenubar, flat, aurora::menu_bar_translate::AURORA_MENU_BAR_NO_PARENT,
                             hierarchy_ok);  // 顶层菜单栏由所有 depth-0 项构成
        check(hierarchy_ok, "OS menu: native hierarchy matches the flattened model (top + submenus)");
        // 顶层项数应为 3（File / Edit / NoId）。
        check(static_cast<std::size_t>(GetMenuItemCount(hmenubar)) == std::size_t{3},
              "OS menu: top-level count == 3 (File, Edit, NoId)");
    }

    // 5. 点击回投：逐终端项调 on_command，验证稳定 id 正确回到路由回调。
    const auto terminals = terminal_ids(flat);  // 顺序即命令 id 派发顺序
    clicks.clear();
    for (std::size_t i = 0; i < terminals.size(); ++i) {
        const bool consumed = bridge.on_command(aurora::detail::AURORA_WIN32_MENU_COMMAND_BASE + static_cast<UINT>(i));
        if (!consumed && !terminals[i].empty()) {
            check(false, "on_command consumed terminal '" + terminals[i] + "'");
        }
    }
    // 空 id 项（"NoId" 与 "NoIdChild"）不应出现在回投记录里；非空终端应全部命中。
    const bool all_nonempty_routed = std::ranges::all_of(terminals, [&clicks](const std::string &id) {
        if (id.empty()) {
            return true;  // 空 id 不在 clicks 里（不回投）
        }
        return std::ranges::find(clicks, id) != clicks.end();
    });
    const bool no_empty_routed = std::ranges::none_of(clicks, [](const std::string &id) { return id.empty(); });
    check(all_nonempty_routed, "click routing: every non-empty terminal id is routed back to the callback");
    check(no_empty_routed, "click routing: empty-id terminals are NOT routed (WARN only)");
    // 非空终端应恰好是 terminals 中非空者。
    std::size_t nonempty = 0;
    for (const auto &id : terminals) {
        if (!id.empty()) {
            ++nonempty;
        }
    }
    check(clicks.size() == nonempty,
          "click routing: routed count == non-empty terminal count (" + aurora_verify::format_uint(nonempty) + ")");

    // 6. 非本桥命令 id 不被消费（其余 WM_COMMAND 应交 DefWindowProc）。
    check(!bridge.on_command(0x0001), "on_command(foreign id) is not consumed (returns false)");
    check(!bridge.on_command(0xFFFF), "on_command(out-of-range id) is not consumed (returns false)");

    // 7. 重复 install 替换旧树（不叠加）：再装一次，菜单仍挂、is_installed 仍真、层级仍一致。
    const auto res2 = bridge.install(flat, on_click);
    check(res2.ok() && bridge.is_installed(), "re-install: replaces the tree, is_installed() stays true");
    const HMENU hmenubar2 = GetMenu(hwnd);
    bool hierarchy_ok2 = (hmenubar2 != nullptr);
    if (hmenubar2 != nullptr) {
        verify_menu_children(hmenubar2, flat, aurora::menu_bar_translate::AURORA_MENU_BAR_NO_PARENT, hierarchy_ok2);
    }
    check(hierarchy_ok2, "re-install: native hierarchy still matches after replacement");

    // 8. uninstall 摘下菜单。
    bridge.uninstall();
    check(!bridge.is_installed(), "uninstall: is_installed() == false");
    check(GetMenu(hwnd) == nullptr, "uninstall: GetMenu(hwnd) is null (menu detached)");

    DestroyWindow(hwnd);
    UnregisterClassW(AURORA_MENU_BAR_PROBE_CLASS, GetModuleHandleW(nullptr));
    return failures() > 0 ? 1 : 0;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与同目录探针入口同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 native menu bar live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    aurora::init_console();  // Windows 控制台切 UTF-8

    emit("== Aurora Win32 native menu bar live probe ==");
    emit("auto segment: capability bit / real HMENU build / hierarchy / click routing / replace / uninstall");

    const int auto_code = run_auto_stage();
    if (auto_code != 0) {
        return auto_code;
    }
    emit("PASS: Win32 native menu bar acceptance passed (auto stage)");
    return 0;
}
