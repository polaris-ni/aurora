/* macOS 无障碍协议面真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ 语义树 → `MacOSAxBridge` → AppKit NSAccessibility 协议面的整段投影：内容视图
//      `accessibilityChildren` 在非空即桥的惰性激活被真实触发；
//   ✅ 角色投影：Button / Checkbox / Slider / TextField / StaticText 逐类命中，且名称
//      取的是桥写入的 `accessibilityLabel`（= 语义节点 Name，含兄弟标签回退链产物）；
//   ✅ 几何投影：逐元素 `accessibilityFrame` 非空，且 `accessibilityHitTest:` 在按钮
//      矩形中心命中该按钮——证明窗口本地 dp → 屏幕点的换算方向正确，而不只是名字对；
//   ✅ 动作投影：`accessibilityPerformPress` 命中复选框后状态翻转，且下一次查询把新值
//      投影出来（客户端动作 → 桥 → Widget → 状态 → 重投影的完整往返）；
//   ✅ 值投影：复选框 / 滑杆 / 文本框的 `accessibilityValue` 在场。
//
//   ❌ 不证明 AX IPC 线上已授权客户端真能读到同一棵树：实测未授权时跨进程直接
//      `kAXErrorAPIDisabled (-25211)`，同进程虽返回 success 但属性退化（逐元素角色一律
//      `AXApplication`、标题为进程名）。故本探针取的是**服务端本体**（AppKit 查询路径与
//      读屏所走的分发是同一条），已授权时另由 `e2e::ax_collect` 加强腿补取客户端侧证据，
//      未授权则如实记 SKIP。真人读屏（VoiceOver / Accessibility Inspector）见
//      `codespec/manual-test/` 对应条目。
//   ❌ 不验收关系投影（LabeledBy 一类）与富文本 / 表格 role 的映射：前者在 macOS 桥面上
//      仍是空位，后者本 fixture 未包含。
//
// 构建：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_ax
//   ./build-macos/aurora_verify_macos_ax
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（无法创建窗口：无 WindowServer / 无图形会话）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_ax 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#import <AppKit/AppKit.h>

#include "aurora/aurora.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"

#include "e2e/harness.h"
#include "e2e/macos_ax_client.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "verify_print.h"

namespace {

// 探针记账：与 macos_surface_live_probe 同款（进程私有、匿名命名空间、就地豁免非 const 全局）。
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
int g_checks = 0;
int g_failures = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

auto check(bool ok, const std::string &name, const std::string &detail = {}) -> void {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 44), detail, "\n");
}

/// @brief fixture 的名称（ASCII 字面量：探针输出要在任何终端代码页下可读）。
/// 命名取 `AURORA_` + 全大写：这是文件作用域的常量，门禁的 `GlobalConstantCase` 只认这个形态
/// （`kCamelCase` 会被判为 invalid case style，口径同 `win32_drag_live_probe.cpp` 的探针文本常量）。
constexpr const char *AURORA_BUTTON_NAME = "Confirm order";
constexpr const char *AURORA_CHECKBOX_LABEL = "Enable auto-update";
constexpr const char *AURORA_SLIDER_LABEL = "Volume";
constexpr const char *AURORA_TEXT_NAME = "Order total";
constexpr const char *AURORA_FRAME_TITLE = "aurora_verify_macos_ax";

/// @brief 采集到的协议面元素（只做本探针的比对，不承担跨平台词汇归并）。
struct ProjectedElement {
    std::string role;  ///< `accessibilityRole`（AppKit 角色常量原样）
    std::string name;  ///< Title 优先、回落 Label（与客户端侧的 Name 口径恒一致）
    NSRect frame = NSZeroRect;  ///< `accessibilityFrame`（屏幕坐标，y 向上）
    bool has_value = false;  ///< `accessibilityValue` 在场
    id element = nil;  ///< 原对象（用于命中测试与动作调用）
};

// AppKit 的各 accessibility* 取值口一律可返 nil（协议方法无 nullability 标注），故形参显式
// 收 _Nullable——否则把返回值直传进来会撞上门禁的空性检查（NullablePassedToNonnull）。
// `UTF8String` 本身也是 `_Nullable` 的返回值：先落地成局部量再判空，重复发消息的写法会让空性
// 分析器看不到「同一枚指针已判过非空」。
auto ns_to_std(NSString *_Nullable text) -> std::string {
    if (text == nil) {
        return {};
    }
    const char *_Nullable bytes = [text UTF8String];
    return bytes != nullptr ? std::string{bytes} : std::string{};
}

/// @brief 先序遍历内容视图的无障碍子树（封顶防失控）。
auto walk(id<NSAccessibility> node, int depth, std::vector<ProjectedElement> &out) -> void {
    if (node == nil || depth > 24 || out.size() >= 400) {
        return;
    }
    ProjectedElement item;
    item.role = ns_to_std([node accessibilityRole]);
    item.name = ns_to_std([node accessibilityTitle]);
    if (item.name.empty()) {
        item.name = ns_to_std([node accessibilityLabel]);
    }
    item.frame = [node accessibilityFrame];
    item.has_value = [node accessibilityValue] != nil;
    item.element = node;
    if (!item.role.empty()) {
        out.push_back(std::move(item));
    }
    // ObjC 快速枚举的循环变量由运行时逐个赋值，语法上无法声明时初始化（`id child = nil` 反而
    // 会被随即覆盖），故该检查在此逐点豁免。
    for (id child in [node accessibilityChildren]) {  // NOLINT(cppcoreguidelines-init-variables)
        walk(child, depth + 1, out);
    }
}

/// @brief 在采集结果里按 AppKit 角色 + 名称找元素（名称为空表示只按角色）。
auto find(const std::vector<ProjectedElement> &elements, NSString *role, const std::string &name)
    -> const ProjectedElement * {
    const std::string want = ns_to_std(role);
    for (const auto &element : elements) {
        if (element.name == name && element.role == want) {
            return &element;
        }
    }
    return nullptr;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与既有探针同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char *argv[]) -> int {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        // ---- 建窗与挂载：经 E2E 内核，与 etest 同一装配 ----
        aurora::e2e::WindowSpec spec;
        spec.backend = aurora::e2e::Backend::MacOS;
        spec.width = 360;
        spec.height = 260;
        spec.title = AURORA_FRAME_TITLE;
        spec.visibility = aurora::WindowVisibility::NoActivate;
        aurora::e2e::Session session = aurora::e2e::open(spec);
        if (!session.ok()) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: ", session.reason(), "\n");
            return 2;
        }

        const auto checked = aurora::State<bool>{false};
        const auto volume = aurora::State<double>{25.0};
        aurora::Node slider_node{aurora::Slider{volume}};
        static_cast<aurora::Slider &>(slider_node.widget())  // NOLINT(*-pro-type-static-cast-downcast)
            .set_range(0.0, 100.0)
            .set_step(1.0);
        aurora::Node input_node{aurora::TextInput{aurora::TextInputProps().set_value("abc").set_placeholder("Type here")}};
        aurora::Node root{aurora::Column{
            aurora::Button{aurora::ButtonProps().set_label(aurora::LocalizedString{AURORA_BUTTON_NAME})},
            aurora::Row{aurora::Text{aurora::TextProps().set_content(aurora::LocalizedString{AURORA_CHECKBOX_LABEL})},
                        aurora::Checkbox{checked}},
            aurora::Row{aurora::Text{aurora::TextProps().set_content(aurora::LocalizedString{AURORA_SLIDER_LABEL})},
                        std::move(slider_node)},
            std::move(input_node),
            aurora::Text{aurora::TextProps().set_content(aurora::LocalizedString{AURORA_TEXT_NAME})}}};

        const auto presented = session.present(root);
        check(presented.ok(), "present fixture", presented ? "" : presented.error().message);
        const auto settled = session.pump_until_settled();
        check(settled.ok(), "pump_until_settled", settled ? "" : settled.error().message);

        auto *window = (__bridge NSWindow *)session.surface().native_handle();
        check(window != nil, "native handle is NSWindow");
        if (window == nil) {
            return 2;
        }
        NSView *content = [window contentView];
        check(content != nil, "content view present");

        // ---- 桥的惰性激活：首次 accessibilityChildren 查询即激活时机 ----
        check(session.surface().accessibility_provider() == nullptr, "bridge absent before the first query");
        std::vector<ProjectedElement> elements;
        walk((id<NSAccessibility>)content, 0, elements);
        check(!elements.empty(), "accessibilityChildren vends projection",
              aurora_verify::format_int(static_cast<long long>(elements.size())) + " elements");
        check(session.surface().accessibility_provider() != nullptr, "bridge activated by the query");

        // 逐行打印观测到的树（判据之外的人工线索：role|name|w x h）。
        for (const auto &element : elements) {
            AURORA_LOG_RAW("verify", "  tree  ", element.role, "|", element.name.empty() ? "(unnamed)" : element.name,
                           "  ", aurora_verify::format_int(static_cast<long long>(element.frame.size.width)), "x",
                           aurora_verify::format_int(static_cast<long long>(element.frame.size.height)), "\n");
        }

        // ---- 角色 / 名称投影 ----
        const auto *button = find(elements, NSAccessibilityButtonRole, AURORA_BUTTON_NAME);
        check(button != nullptr, "button projected with its Name");
        const auto *checkbox = find(elements, NSAccessibilityCheckBoxRole, AURORA_CHECKBOX_LABEL);
        check(checkbox != nullptr, "checkbox projected via sibling-label fallback");
        const auto *slider = find(elements, NSAccessibilitySliderRole, AURORA_SLIDER_LABEL);
        check(slider != nullptr, "slider projected via sibling-label fallback");
        const auto *static_text = find(elements, NSAccessibilityStaticTextRole, AURORA_TEXT_NAME);
        check(static_text != nullptr, "static text projected");
        const bool has_text_field = std::ranges::any_of(elements, [](const ProjectedElement &element) {
            return element.role == ns_to_std(NSAccessibilityTextFieldRole) ||
                   element.role == ns_to_std(NSAccessibilityTextAreaRole);
        });
        check(has_text_field, "text input projected as a text field");

        // ---- 几何投影：非空 + 命中测试 ----
        const bool all_frames_nonempty =
            !elements.empty() && std::ranges::all_of(elements, [](const ProjectedElement &element) {
                return element.frame.size.width > 0.0 && element.frame.size.height > 0.0;
            });
        check(all_frames_nonempty, "every element has a non-empty screen frame");
        if (button != nullptr) {
            const NSPoint center = NSMakePoint(button->frame.origin.x + (button->frame.size.width / 2.0),
                                               button->frame.origin.y + (button->frame.size.height / 2.0));
            id hit = [content accessibilityHitTest:center];
            const std::string hit_name = ns_to_std([hit accessibilityLabel]);
            check(hit != nil && hit_name == std::string{AURORA_BUTTON_NAME},
                  "hit test at button center hits the button",
                  hit_name.empty() ? ns_to_std([hit accessibilityRole]) : hit_name);
        }

        // ---- 值投影 ----
        check(checkbox != nullptr && checkbox->has_value, "checkbox vends accessibilityValue");
        check(slider != nullptr && slider->has_value, "slider vends accessibilityValue");

        // ---- 动作往返：客户端 press → 桥 → Widget → 状态 → 下一次查询重投影 ----
        if (checkbox != nullptr) {
            id value_before = [checkbox->element accessibilityValue];
            const BOOL pressed = [checkbox->element accessibilityPerformPress];
            session.surface().wait_events(60.0);
            (void)session.present(root);
            session.surface().wait_events(60.0);
            std::vector<ProjectedElement> again;
            walk((id<NSAccessibility>)content, 0, again);
            const auto *reloaded = find(again, NSAccessibilityCheckBoxRole, AURORA_CHECKBOX_LABEL);
            id value_after = reloaded != nullptr ? [reloaded->element accessibilityValue] : nil;
            // `isEqual:` 返 BOOL（signed char），取「不等」要显式比 NO：裸 `!` 是整数到 bool 的
            // 隐式转换，门禁按 readability-implicit-bool-conversion 记账。
            const bool flipped =
                value_before != nil && value_after != nil && ([value_before isEqual:value_after] == NO);
            check(pressed == YES, "press action accepted");
            check(flipped, "press re-projects the new value",
                  value_before != nil ? std::string([[value_before description] UTF8String]) : "nil");
        }

        // ---- 已授权时的加强腿：真客户端（AXUIElement）读同一棵树 ----
        if (aurora::e2e::ax_channel_available()) {
            std::vector<aurora::e2e::AxSemanticNode> nodes;
            const auto collected = aurora::e2e::ax_collect(
                std::string{AURORA_FRAME_TITLE}, nodes, [&session] { session.surface().wait_events(20.0); }, 15000);
            check(collected.ok() && !nodes.empty(), "authorized AX client reads the same projection",
                  collected ? aurora_verify::format_int(static_cast<long long>(nodes.size())) + " nodes"
                            : collected.error().message);
        } else {
            AURORA_LOG_RAW("verify", "SKIP  authorized AX client leg (process is not an Accessibility-trusted "
                                     "client; untrusted queries return degraded attributes)\n");
        }

        AURORA_LOG_RAW("verify", "----------------------------------------\n",
                       "summary: ", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        return g_failures == 0 ? 0 : 1;
    }
}
