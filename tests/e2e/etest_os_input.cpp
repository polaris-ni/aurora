// 测试类型: e2e
// 目标单元: tools/include/e2e/os_input.h
// 测试说明: 真机增强层——OS 级输入注入（Windows SendInput / PostMessage、X11 XTest）
//           投递真实输入后断言控件状态与渲染像素，证明「OS 输入 → 窗口过程 → Aurora
//           事件管线」整段接线（内核目标式注入只覆盖后半段）。
//
// 独占执行约束（与「RUN_SERIAL 已移除、并行模型 = 进程隔离 + 资源虚拟化」的关系）：
// SendInput 是全局注入、无法定向窗口，多窗口并存时必然串台——既有模型隔离的是资源，
// 全局输入是无法虚拟化的**共享设备**。独占性由「opt-in 门控（未置 AURORA_E2E_OS_INPUT
// 时为 skip 桩，默认 ctest 中不参与并行）+ 独立 ctest 调用按 stem 编排
// （ctest -L e2e -R etest_os_input）」实现，不使用 CTest RUN_SERIAL 属性（TEST-R7 禁止）。
//
// 窗口策略固定 Normal：OS 输入投递到完全隐藏的窗口在 Win32 上语义不成立（SendInput
// 投给光标所在窗口，隐藏窗口不在命中路径）。见 specification/08-tooling.md §8.2。

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "aurora/aurora.h"
#include "aurora/state/state.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text_input.h"
// X11Surface 取 native_display 需要完整类型；aurora.h 不含后端头，且本类仅在
// `AURORA_PLATFORM_LINUX && AURORA_BACKEND_X11` 下声明（头内自门控，pimpl 不含 Xlib，
// 无条件 include 安全）——后端关闭的配置下用例体走编译期 skip 桩，不触达该类型。
#include "aurora/window/x11_surface.h"
#include "e2e/harness.h"
#include "e2e/os_input.h"
#include "e2e_expect.h"
#include "framework/aurora_test.h"

namespace au = aurora;

namespace aurora::test_cases::etest_os_input {

// helper 仅在注入通道编译的目标平台上有定义意义（其余平台全部用例走 skip 桩，
// 未使用的匿名命名空间符号会触发 unused-function 告警）。X11 侧以 AURORA_BACKEND_X11
// 收窄：后端关闭的 Linux 配置下 xtest 用例是编译期 skip 桩，不引用任何 helper。
#if defined(AURORA_PLATFORM_WINDOWS) || \
    (defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_X11))
namespace {

constexpr std::string_view AURORA_OS_FRAME_TITLE = "aurora_e2e_os_input";

/// @brief 注入目标 UI：TextInput（键盘目标，上）+ Checkbox（指针目标，下）。
///        Checkbox 走共享 `State<bool>` 观察翻转；TextInput 经 `on_changed` 捕获上屏文本。
struct OsInputUi {
    std::shared_ptr<au::State<bool>> checked = std::make_shared<au::State<bool>>(false);
    std::shared_ptr<std::string> typed = std::make_shared<std::string>();
    au::Node root;
};

/// @brief 装配注入目标 UI（值回调先挂好，再组树）。
[[nodiscard]] auto build_os_input_ui() -> OsInputUi {
    OsInputUi ui;
    au::Node input{au::TextInput{au::TextInputProps{.value = ""}}};
    static_cast<au::TextInput &>(input.widget())  // NOLINT(*-pro-type-static-cast-downcast)
        .set_on_changed([captured = ui.typed](const std::string &v) { *captured = v; });
    ui.root = au::Node{au::Column{std::move(input), au::Checkbox{au::Reactive{ui.checked}}}};
    return ui;
}

/// @brief 会话装配：**Normal 档**（本层硬约束，见文件头注释）；挂树、出帧、收敛。
[[nodiscard]] auto assemble_os_session(e2e::Backend backend, au::Node &root) -> e2e::Session {
    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = 320;
    spec.height = 200;
    spec.title = std::string{AURORA_OS_FRAME_TITLE};
    spec.visibility = au::WindowVisibility::Normal;

    e2e::Session session = e2e::open(spec);
    if (!session.ok()) {
        e2e::account_unavailable(backend, session.reason());
    }
    const auto presented = session.present(root);
    AURORA_TEST_REQUIRE_TRUE(presented.ok());
    const auto settled = session.pump_until_settled();
    AURORA_TEST_REQUIRE_TRUE(settled.ok());
    return session;
}

/// @brief 两道策略 skip 门：opt-in 未置位 → skip 桩；非交互桌面 → no_interactive_desktop。
auto require_os_input_policy() -> void {
    if (!e2e::os_input_opt_in()) {
        AURORA_TEST_SKIP(std::string{"opt-in env var "} + e2e::AURORA_OS_INPUT_ENV_VAR +
                         " not set (skipped by policy; OS-level injection must run exclusively: "
                         "ctest -L e2e -R etest_os_input)");
    }
    const e2e::DesktopInteraction desktop = e2e::interactive_desktop();
    if (!desktop.interactive) {
        AURORA_TEST_SKIP("no_interactive_desktop: " + desktop.reason);
    }
}

/// @brief 语义盒（dp，窗口本地）中心 → 客户区物理像素点（× scale；与宿主 px/scale 互逆）。
[[nodiscard]] auto center_px(const au::Rect &bounds_dp, float scale) -> au::Point {
    return au::Point{.x = (bounds_dp.origin.x + (bounds_dp.size.width / 2.0F)) * scale,
                     .y = (bounds_dp.origin.y + (bounds_dp.size.height / 2.0F)) * scale};
}

/// @brief 帧内语义盒区域（dp 盒 × scale 放大到物理像素，外扩 1px 后夹取）是否有像素变化。
[[nodiscard]] auto region_differs(const e2e::Frame &before, const e2e::Frame &after, const au::Rect &bounds_dp,
                                  float scale) -> bool {
    if (before.width != after.width || before.height != after.height) {
        return true;  // 尺寸漂移本身即差异（调用方已另行断言尺寸的前提兜底）
    }
    const auto x0 = std::max(0, static_cast<int>(std::floor(bounds_dp.origin.x * scale)) - 1);
    const auto y0 = std::max(0, static_cast<int>(std::floor(bounds_dp.origin.y * scale)) - 1);
    const auto x1 = std::min(
        before.width, static_cast<int>(std::ceil((bounds_dp.origin.x * scale) + (bounds_dp.size.width * scale))) + 1);
    const auto y1 = std::min(
        before.height, static_cast<int>(std::ceil((bounds_dp.origin.y * scale) + (bounds_dp.size.height * scale))) + 1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (e2e::pixel_at(before, x, y) != e2e::pixel_at(after, x, y)) {
                return true;
            }
        }
    }
    return false;
}

/// @brief 注入后取新快照中的语义节点（同一角色重新查找；布局不受注入影响，按名复核）。
[[nodiscard]] auto snapshot_node(const e2e::Session &session, AccessibilityRole role)
    -> const au::a11y::NodeSnapshot * {
    const auto snapshot = session.semantic_snapshot();
    AURORA_TEST_REQUIRE_TRUE(snapshot.ok());
    return e2e::find_semantic_node(snapshot.value(), role);
}

}  // namespace
#endif  // helper 平台门控（Windows / UNIX 非 macOS 且 X11 后端）

// ⚠️ 三个 AURORA_TEST_CASE 须无条件可见（AGENTS.md §5 测试纪律：#if 只能写在用例体内），
// 平台/后端差异全部收进用例体的 skip 桩分支——整体包裹会让 macOS 等不满足门控的平台
// 注册表单边失踪（registry_integrity 红灯 + --run 筛空 exit 2）。

// 用例一：SendInput 全局注入（真实光标移动 + 点击）→ Checkbox 翻转 + 像素变化。
// 全链路：OS 输入流 → 命中本窗口 → 窗口过程 → Aurora 事件管线。d3d11/wgpu 共用
// Win32Host 输入管线，通道覆盖以 win32 为代表。
AURORA_TEST_CASE(sendinput_click_toggles_checkbox) {
#ifdef AURORA_PLATFORM_WINDOWS
    require_os_input_policy();
    if (!e2e::backend_compiled(e2e::Backend::Win32)) {
        AURORA_TEST_SKIP("backend 'win32' not compiled into this build");
    }
    OsInputUi ui = build_os_input_ui();
    e2e::Session session = assemble_os_session(e2e::Backend::Win32, ui.root);

    const auto snapshot = session.semantic_snapshot();
    AURORA_TEST_REQUIRE_TRUE(snapshot.ok());
    const auto *box = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Checkbox);
    AURORA_TEST_REQUIRE_TRUE(box != nullptr);
    const float scale = session.surface().scale_factor();
    const auto before = session.read_pixels();
    AURORA_TEST_REQUIRE_TRUE(before.ok());

    const auto clicked = e2e::sendinput_click(session.surface().native_handle(), center_px(box->node.bounds, scale));
    AURORA_TEST_REQUIRE_TRUE(clicked.ok());
    const auto settled = session.pump_until_settled();
    AURORA_TEST_REQUIRE_TRUE(settled.ok());

    const auto *after_box = snapshot_node(session, AccessibilityRole::Checkbox);
    AURORA_TEST_REQUIRE_TRUE(after_box != nullptr);
    AURORA_TEST_CHECK_TRUE(after_box->node.state.checked);  // 控件状态翻转：OS 输入确实到达事件管线
    const auto after = session.read_pixels();
    AURORA_TEST_REQUIRE_TRUE(after.ok());
    AURORA_TEST_CHECK_TRUE(region_differs(before.value(), after.value(), box->node.bounds, scale));
#else
    AURORA_TEST_SKIP("本平台无 SendInput 通道（Windows 专属；Linux 走 XTest，macOS 为已知缺口）");
#endif
}

// 用例二：PostMessage 定向投递 → 直达窗口过程（不经光标/焦点系统）：鼠标消息翻转
// Checkbox，WM_CHAR 上屏 TextInput。投递侧与真实输入的差异即本通道价值——管线接线可
// 单独取证。
AURORA_TEST_CASE(postmessage_reaches_window_procedure) {
#ifdef AURORA_PLATFORM_WINDOWS
    require_os_input_policy();
    if (!e2e::backend_compiled(e2e::Backend::Win32)) {
        AURORA_TEST_SKIP("backend 'win32' not compiled into this build");
    }
    OsInputUi ui = build_os_input_ui();
    e2e::Session session = assemble_os_session(e2e::Backend::Win32, ui.root);

    const auto snapshot = session.semantic_snapshot();
    AURORA_TEST_REQUIRE_TRUE(snapshot.ok());
    const auto *box = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Checkbox);
    AURORA_TEST_REQUIRE_TRUE(box != nullptr);
    const auto *input = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::TextInput);
    AURORA_TEST_REQUIRE_TRUE(input != nullptr);
    const float scale = session.surface().scale_factor();
    void *hwnd = session.surface().native_handle();
    AURORA_TEST_REQUIRE_TRUE(hwnd != nullptr);

    // 鼠标消息定向投递 → Checkbox 翻转。
    const auto clicked = e2e::post_click(hwnd, center_px(box->node.bounds, scale));
    AURORA_TEST_REQUIRE_TRUE(clicked.ok());
    const auto settled = session.pump_until_settled();
    AURORA_TEST_REQUIRE_TRUE(settled.ok());
    const auto *after_box = snapshot_node(session, AccessibilityRole::Checkbox);
    AURORA_TEST_REQUIRE_TRUE(after_box != nullptr);
    AURORA_TEST_CHECK_TRUE(after_box->node.state.checked);

    // 点击 TextInput 获焦（Press 经 FocusManager::request_focus）→ WM_CHAR 逐字符上屏。
    const auto focused = e2e::post_click(hwnd, center_px(input->node.bounds, scale));
    AURORA_TEST_REQUIRE_TRUE(focused.ok());
    AURORA_TEST_REQUIRE_TRUE(session.pump_until_settled().ok());
    const auto typed_text = e2e::post_text(hwnd, "a1");
    AURORA_TEST_REQUIRE_TRUE(typed_text.ok());
    AURORA_TEST_REQUIRE_TRUE(session.pump_until_settled().ok());
    AURORA_TEST_CHECK_TRUE(*ui.typed == "a1");  // 字符通道：WM_CHAR → TextInputEvent → 控件状态
#else
    AURORA_TEST_SKIP("本平台无 PostMessage 通道（Windows 专属；Linux 走 XTest，macOS 为已知缺口）");
#endif
}

// 用例三：XTest 经 X 服务器合成真实输入（指针移动 + 按键）→ 与真实鼠标/键盘完全同径。
// 焦点铺垫用 XSetInputFocus（无窗口管理器时点击不自动夺焦；等价 WM click-to-focus）。
// X11Surface 的 static_cast 是编译期构造，须与头内声明门控（AURORA_BACKEND_X11）同款
// 收窄——后端关闭的配置（如默认 Linux 构建）下类型不存在，走编译期 skip 桩。
AURORA_TEST_CASE(xtest_server_input_reaches_window) {
#if defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_X11)
    require_os_input_policy();
    if (!e2e::xtest_available()) {
        AURORA_TEST_SKIP("XTEST unavailable (libX11/libXtst dlopen failed or symbols missing)");
    }
    OsInputUi ui = build_os_input_ui();
    e2e::Session session = assemble_os_session(e2e::Backend::X11, ui.root);

    const auto snapshot = session.semantic_snapshot();
    AURORA_TEST_REQUIRE_TRUE(snapshot.ok());
    const auto *box = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Checkbox);
    AURORA_TEST_REQUIRE_TRUE(box != nullptr);
    const auto *input = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::TextInput);
    AURORA_TEST_REQUIRE_TRUE(input != nullptr);
    const float scale = session.surface().scale_factor();
    void *display =
        static_cast<au::X11Surface &>(session.surface()).native_display();  // NOLINT(*-pro-type-static-cast-downcast)
    void *xid = session.surface().native_handle();
    AURORA_TEST_REQUIRE_TRUE(display != nullptr);
    AURORA_TEST_REQUIRE_TRUE(xid != nullptr);

    // 指针通道：XTest 假点击 → Checkbox 翻转 + 像素变化。
    const auto before = session.read_pixels();
    AURORA_TEST_REQUIRE_TRUE(before.ok());
    const auto clicked = e2e::xtest_click(display, xid, center_px(box->node.bounds, scale));
    AURORA_TEST_REQUIRE_TRUE(clicked.ok());
    const auto settled = session.pump_until_settled();
    AURORA_TEST_REQUIRE_TRUE(settled.ok());
    const auto *after_box = snapshot_node(session, AccessibilityRole::Checkbox);
    AURORA_TEST_REQUIRE_TRUE(after_box != nullptr);
    AURORA_TEST_CHECK_TRUE(after_box->node.state.checked);
    const auto after = session.read_pixels();
    AURORA_TEST_REQUIRE_TRUE(after.ok());
    AURORA_TEST_CHECK_TRUE(region_differs(before.value(), after.value(), box->node.bounds, scale));

    // 键盘通道：焦点铺垫 → XTest 假键 'a' → TextInput 上屏。
    const auto focused = e2e::xtest_focus_window(display, xid);
    AURORA_TEST_REQUIRE_TRUE(focused.ok());
    AURORA_TEST_REQUIRE_TRUE(session.pump_until_settled().ok());
    const auto typed_text = e2e::xtest_text(display, "a");
    AURORA_TEST_REQUIRE_TRUE(typed_text.ok());
    AURORA_TEST_REQUIRE_TRUE(session.pump_until_settled().ok());
    AURORA_TEST_CHECK_TRUE(*ui.typed == "a");
#elif defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS)
    AURORA_TEST_SKIP("backend 'x11' not compiled into this build (AURORA_BACKEND_X11 off)");
#else
    AURORA_TEST_SKIP("本平台无 XTest 通道（Linux 专属；Windows 走 SendInput/PostMessage，macOS 为已知缺口）");
#endif
}

}  // namespace aurora::test_cases::etest_os_input
