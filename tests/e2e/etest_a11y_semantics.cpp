// 测试类型: e2e
// 目标单元: tools/include/e2e/harness.h
// 测试说明: 真实后端上对代表性控件做语义树断言（角色/名称/几何/状态/可执行动作），
//           并与像素断言并列独立取证；平台通道用例验证「语义树 → 平台桥 → 平台投影」
//           整条链路确实到达平台（Windows 走 UIA 客户端，Linux 走 AT-SPI 客户端，
//           macOS 通道未实现为已知缺口）。像素读回不可用按期望集口径记账。

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "e2e/atspi_client.h"
#include "e2e/harness.h"
#include "e2e/uia_client.h"
#include "e2e_expect.h"
#include "framework/aurora_test.h"

namespace au = aurora;

namespace aurora::test_cases::etest_a11y_semantics {
namespace {

// 统一期望表：进程内快照与平台通道投影共用同一张表（同源语义树，断言同词汇）。
// 名称回退链已由单测覆盖；此处锁的是「真实后端上桥投影与快照一致」这一端到端事实。
constexpr std::string_view AURORA_A11Y_BUTTON_NAME = "确定";
constexpr std::string_view AURORA_A11Y_CHECKBOX_LABEL = "启用自动更新";
constexpr std::string_view AURORA_A11Y_SLIDER_LABEL = "音量";
constexpr std::string_view AURORA_A11Y_TEXT_NAME = "订单总额";
constexpr std::string_view AURORA_A11Y_FRAME_TITLE = "aurora_e2e_a11y";

/// @brief 语义断言目标控件列：Button / Row{Text, Checkbox} / Row{Text, Slider} /
///        TextInput / Text——覆盖 Invoke、Toggle、Value 三类动作面与两条名称回退链
///        （兄弟标签、取值）。
[[nodiscard]] auto build_a11y_column() -> au::Node {
    const auto checked = std::make_shared<au::State<bool>>(true);
    const auto volume = std::make_shared<au::State<double>>(25.0);

    au::Node slider_node{au::Slider{au::Reactive{volume}}};
    // 值域未在构造参数声明，显式收敛到 0..100 便于两端断言 range 投影。
    static_cast<au::Slider &>(slider_node.widget())  // NOLINT(*-pro-type-static-cast-downcast)
        .set_range(0.0, 100.0)
        .set_step(1.0);

    au::Node input_node{au::TextInput{au::TextInputProps{.value = "abc", .placeholder = "请输入"}}};

    return au::Node{au::Column{
        au::Button{au::ButtonProps{.label = au::LocalizedString{AURORA_A11Y_BUTTON_NAME}}},
        au::Row{au::Text{au::TextProps{.content = au::LocalizedString{AURORA_A11Y_CHECKBOX_LABEL}}},
                au::Checkbox{au::Reactive{checked}}},
        au::Row{au::Text{au::TextProps{.content = au::LocalizedString{AURORA_A11Y_SLIDER_LABEL}}},
                std::move(slider_node)},
        std::move(input_node),
        au::Text{au::TextProps{.content = au::LocalizedString{AURORA_A11Y_TEXT_NAME}}},
    }};
}

/// @brief 会话的通用装配：指定后端建窗（NoActivate：a11y 平台桥按真实窗口接线，隐藏
///        窗口的平台投影未经证明；可见但不夺焦点是确定性档位）、挂树、出帧、收敛。
///        `root` 由调用方持有且须活过整个会话（桥与快照都按活控件树投影）。
[[nodiscard]] auto assemble_a11y_session(e2e::Backend backend, au::Node &root) -> e2e::Session {
    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = 320;
    spec.height = 240;
    spec.title = std::string{AURORA_A11Y_FRAME_TITLE};
    spec.visibility = au::WindowVisibility::NoActivate;

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

/// @brief 像素通道独立取证：帧可读回（不可用按期望集口径记账）且画面有内容。
auto verify_pixel_channel(const e2e::Session &session, e2e::Backend backend) -> void {
    const auto frame = session.read_pixels();
    if (!frame.ok()) {
        // 与语义通道并列记账：读回不可用不推翻语义断言，也不被语义断言掩盖。
        e2e::account_unavailable(backend, frame.error().message);
    }
    AURORA_TEST_CHECK_TRUE(frame.value().width > 0 && frame.value().height > 0);
    AURORA_TEST_CHECK_TRUE(frame.value().pixels.size() == static_cast<std::size_t>(frame.value().width) *
                                                              static_cast<std::size_t>(frame.value().height) * 4U);
    // 画面非单色（控件树确有内容渲染）：全帧单色意味着控件树未被绘制。
    const auto &pixels = frame.value().pixels;
    bool uniform = true;
    for (std::size_t i = 4; i < pixels.size(); i += 4) {
        if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] || pixels[i + 2] != pixels[2]) {
            uniform = false;
            break;
        }
    }
    AURORA_TEST_CHECK_FALSE(uniform);
}

/// @brief 进程内语义断言：统一期望表逐项命中（角色 / 名称 / 动作 / 几何 / 状态 / 值域）。
auto verify_semantic_snapshot(const e2e::Session &session) -> void {
    const auto snapshot = session.semantic_snapshot();
    AURORA_TEST_REQUIRE_TRUE(snapshot.ok());
    AURORA_TEST_CHECK_FALSE(snapshot.value().flat.empty());

    // Button：显式 label → Name；动作面 Invoke | Click | Focus；几何非空。
    const auto *button = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Button, AURORA_A11Y_BUTTON_NAME);
    AURORA_TEST_REQUIRE_TRUE(button != nullptr);
    AURORA_TEST_CHECK_TRUE(button->node.has_action(au::AccessibilityAction::Invoke));
    AURORA_TEST_CHECK_TRUE(button->node.has_action(au::AccessibilityAction::Click));
    AURORA_TEST_CHECK_TRUE(button->node.has_action(au::AccessibilityAction::Focus));
    AURORA_TEST_CHECK_TRUE(button->node.bounds.size.width > 0.0F);
    AURORA_TEST_CHECK_TRUE(button->node.state.focusable);

    // Checkbox：兄弟标签回退 → Name；动作面 Toggle；状态位 checked=true（State 初值）。
    const auto *checkbox =
        e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Checkbox, AURORA_A11Y_CHECKBOX_LABEL);
    AURORA_TEST_REQUIRE_TRUE(checkbox != nullptr);
    AURORA_TEST_CHECK_TRUE(checkbox->node.has_action(au::AccessibilityAction::Toggle));
    AURORA_TEST_CHECK_TRUE(checkbox->node.state.checked);
    AURORA_TEST_CHECK_TRUE(checkbox->node.bounds.size.width > 0.0F);

    // Slider：兄弟标签回退 → Name；动作面 Value；值域投影 0..100、值 25。
    const auto *slider = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Slider, AURORA_A11Y_SLIDER_LABEL);
    AURORA_TEST_REQUIRE_TRUE(slider != nullptr);
    AURORA_TEST_CHECK_TRUE(slider->node.has_action(au::AccessibilityAction::Value));
    AURORA_TEST_REQUIRE_TRUE(slider->node.range.has_value());
    const auto &slider_range = slider->node.range.value();  // NOLINT(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_TRUE(slider_range.min == 0.0);
    AURORA_TEST_CHECK_TRUE(slider_range.max == 100.0);
    AURORA_TEST_CHECK_TRUE(slider_range.value == 25.0);

    // TextInput：名称经回退链产出可朗读文本（文案/占位/取值的具体落点已由单测锁定，
    // 此处只锁端到端事实：投影非空且动作面 Value 到位）。
    const auto *input = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::TextInput);
    AURORA_TEST_REQUIRE_TRUE(input != nullptr);
    AURORA_TEST_CHECK_FALSE(input->node.name.empty());
    AURORA_TEST_CHECK_TRUE(input->node.has_action(au::AccessibilityAction::Value));

    // Text：文案直映 → Name；几何非空。
    const auto *text = e2e::find_semantic_node(snapshot.value(), AccessibilityRole::Text, AURORA_A11Y_TEXT_NAME);
    AURORA_TEST_REQUIRE_TRUE(text != nullptr);
    AURORA_TEST_CHECK_TRUE(text->node.bounds.size.width > 0.0F);
}

/// @brief 矩阵取值表：全部真实窗口后端（未编译后端留在表内，是「期望集外 → 跳过」
///        的常态样本；`Headless` 不在表内——语义断言面向真实窗口上屏链路）。
[[nodiscard]] auto a11y_backend_values() -> std::vector<e2e::Backend> {
    return {e2e::Backend::Win32, e2e::Backend::D3D11,   e2e::Backend::Glfw,
            e2e::Backend::X11,   e2e::Backend::Wayland, e2e::Backend::Wgpu};
}

/// @brief 取值名生成器：直接用后端短名，使报告里一眼看出失败的是哪个后端。
[[nodiscard]] auto a11y_backend_name(const e2e::Backend &backend) -> std::string { return e2e::backend_name(backend); }

class A11yBackends : public testing::TestWithParam<e2e::Backend> {};

}  // namespace

// 用例一（全后端）：进程内语义快照断言与像素断言并列独立记账。
AURORA_TEST_P(A11yBackends, semantic_snapshot_matches_widgets) {
    const e2e::Backend backend = param();
    if (!e2e::backend_compiled(backend)) {
        AURORA_TEST_SKIP("backend not compiled into this build");
    }
    au::Node root = build_a11y_column();
    e2e::Session session = assemble_a11y_session(backend, root);
    verify_pixel_channel(session, backend);
    verify_semantic_snapshot(session);
}

// 用例二（按平台选通道）：平台桥投影必须与进程内快照同词汇到达平台。
// Windows：UIA 客户端（win32 / d3d11 共用 Win32Host 桥）；Linux：AT-SPI 客户端
// （x11 / wayland 共用 AtspiBridge，无会话总线为合法永久降级）；macOS：通道未实现。
AURORA_TEST_P(A11yBackends, platform_bridge_projects_semantics) {
    const e2e::Backend backend = param();
    if (!e2e::backend_compiled(backend)) {
        AURORA_TEST_SKIP("backend not compiled into this build");
    }
#ifdef AURORA_PLATFORM_WINDOWS
    if (backend != e2e::Backend::Win32 && backend != e2e::Backend::D3D11) {
        AURORA_TEST_SKIP("该后端未接 Win32Host 无障碍桥（UIA 通道覆盖面仅 win32/d3d11）");
    }
    if (!e2e::uia_channel_available()) {
        AURORA_TEST_SKIP("UIA client unavailable on this environment");
    }
    au::Node root = build_a11y_column();
    e2e::Session session = assemble_a11y_session(backend, root);
    // 与探针同款预热：出帧后泵事件并再出一帧，确保宿主消息循环与桥接线就绪再发起查询。
    for (int i = 0; i < 4; ++i) {
        session.surface().wait_events(20.0);
    }
    (void)session.present(root);

    void *host = session.surface().native_handle();
    AURORA_TEST_REQUIRE_TRUE(host != nullptr);
    std::vector<e2e::UiaSemanticNode> nodes;
    const auto collected = e2e::uia_collect(static_cast<HWND>(host), nodes);
    AURORA_TEST_REQUIRE_TRUE(collected.ok());
    // 诊断笔记：UIA 实际返回了什么（默认 HWND 供应商 vs 桥投影），失败时可定位层级。
    {
        std::string seen;
        for (const auto &node : nodes) {
            seen += node.framework_id.empty() ? "?" : node.framework_id;
            seen += std::string{"("} + e2e::semantic_role_name(node.role) + ") ";
        }
        if (auto *ctx = testing::current_context(); ctx != nullptr) {
            ctx->add_note("uia_nodes=" + std::to_string(nodes.size()) + " " + seen);
        }
    }
    // 客户端查询触发桥惰性激活（D14）：投影抵达平台后，桥必须已就位。
    AURORA_TEST_CHECK_TRUE(session.surface().accessibility_provider() != nullptr);

    // 只比对库自有投影（FrameworkId == "Aurora"），排除系统非客户区元素。
    std::vector<e2e::UiaSemanticNode> own;
    for (auto &node : nodes) {
        if (node.framework_id == "Aurora") {
            own.push_back(std::move(node));
        }
    }
    AURORA_TEST_REQUIRE_TRUE(!own.empty());

    auto find_uia = [&own](AccessibilityRole role, std::string_view name) -> const e2e::UiaSemanticNode * {
        for (const auto &node : own) {
            if (node.role == role && (name.empty() || node.name == name)) {
                return &node;
            }
        }
        return nullptr;
    };

    const auto *button = find_uia(AccessibilityRole::Button, AURORA_A11Y_BUTTON_NAME);
    AURORA_TEST_REQUIRE_TRUE(button != nullptr);
    AURORA_TEST_CHECK_TRUE(button->invoke);
    AURORA_TEST_CHECK_TRUE(button->rect_ok);

    const auto *checkbox = find_uia(AccessibilityRole::Checkbox, AURORA_A11Y_CHECKBOX_LABEL);
    AURORA_TEST_REQUIRE_TRUE(checkbox != nullptr);
    AURORA_TEST_CHECK_TRUE(checkbox->toggle);
    AURORA_TEST_CHECK_TRUE(checkbox->rect_ok);

    const auto *slider = find_uia(AccessibilityRole::Slider, AURORA_A11Y_SLIDER_LABEL);
    AURORA_TEST_REQUIRE_TRUE(slider != nullptr);
    AURORA_TEST_CHECK_TRUE(slider->range_value);
    AURORA_TEST_CHECK_TRUE(slider->rect_ok);

    const auto *input = find_uia(AccessibilityRole::TextInput, "");
    AURORA_TEST_REQUIRE_TRUE(input != nullptr);
    AURORA_TEST_CHECK_TRUE(input->value);
    AURORA_TEST_CHECK_FALSE(input->name.empty());

    const auto *text = find_uia(AccessibilityRole::Text, AURORA_A11Y_TEXT_NAME);
    AURORA_TEST_REQUIRE_TRUE(text != nullptr);
    AURORA_TEST_CHECK_TRUE(text->rect_ok);
#elif defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS)
    if (backend != e2e::Backend::X11 && backend != e2e::Backend::Wayland) {
        AURORA_TEST_SKIP("该后端未接 AtspiBridge（AT-SPI 通道覆盖面仅 x11/wayland）");
    }
    if (!e2e::atspi_client_available()) {
        AURORA_TEST_SKIP("python3-gi/Atspi client unavailable on this environment");
    }
    e2e::Session session = assemble_a11y_session(backend);

    // 库按设计永久降级：无 org.a11y.Bus 时桥缺席 → 合法 skip（环境能力，非缺陷）。
    if (session.surface().accessibility_provider() == nullptr) {
        AURORA_TEST_SKIP("org.a11y.Bus unreachable; bridge degraded per design");
    }

    std::vector<e2e::AtspiSemanticNode> nodes;
    const auto collected = e2e::atspi_collect(
        std::string{AURORA_A11Y_FRAME_TITLE}, nodes, [&session] { session.surface().wait_events(20.0); }, 45000);
    AURORA_TEST_REQUIRE_TRUE(collected.ok());

    auto find_atspi = [&nodes](AccessibilityRole role, std::string_view name) -> const e2e::AtspiSemanticNode * {
        for (const auto &node : nodes) {
            if (node.role == role && (name.empty() || node.name == name)) {
                return &node;
            }
        }
        return nullptr;
    };

    AURORA_TEST_REQUIRE_TRUE(find_atspi(AccessibilityRole::Button, AURORA_A11Y_BUTTON_NAME) != nullptr);
    AURORA_TEST_REQUIRE_TRUE(find_atspi(AccessibilityRole::Checkbox, AURORA_A11Y_CHECKBOX_LABEL) != nullptr);
    AURORA_TEST_REQUIRE_TRUE(find_atspi(AccessibilityRole::Slider, AURORA_A11Y_SLIDER_LABEL) != nullptr);
    const auto *input = find_atspi(AccessibilityRole::TextInput, "");
    AURORA_TEST_REQUIRE_TRUE(input != nullptr);
    AURORA_TEST_CHECK_FALSE(input->name.empty());
    AURORA_TEST_REQUIRE_TRUE(find_atspi(AccessibilityRole::Text, AURORA_A11Y_TEXT_NAME) != nullptr);
#else
    static_cast<void>(backend);
    AURORA_TEST_SKIP("本平台的语义通道未实现（macOS AX 为已知缺口，见 specification/08-tooling.md §8.2）");
#endif
}

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(a11y_backends, A11yBackends, a11y_backend_values(), a11y_backend_name);

}  // namespace aurora::test_cases::etest_a11y_semantics
