/* macOS OS 级输入注入真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ **同进程 AppKit 事件队列注入**驱动的完整输入管线：`[NSApp postEvent:]` →
//      `nextEventMatchingMask:` → `sendEvent:` → 内容视图 `mouseDown:` / `keyDown:` →
//      `MacOSSurface` 翻译为 `aurora::Event` → 派发 / 命中测试 / 手势状态机 → 控件状态翻转
//      并反映到新帧像素。形状与 Win32 的 `post_click`（PostMessage 定向投递）同侧：投递侧
//      绕过 OS 输入流，但窗口过程之后的整段接线单独取证得到；
//   ✅ 指针通道：点击语义盒中心 → Checkbox 翻转 → 再点翻转回来（往返两次，非单向巧合）；
//   ✅ 键盘通道：点击 TextInput 获焦 → 合成 `keyDown` 的 `characters:` 上屏（`on_changed`
//      捕获到的实值即控件状态）；
//   ✅ 区域像素复核：注入前后按语义盒（dp × scale）比对，变化必须落在目标区域内；
//   ✅ 注入前的一枚**中性 priming 点击**（落在无 widget 的角落）本身也是判据：它不得改变勾选
//      状态。这一枚铺垫不是摆设——`NSView` 默认 `acceptsFirstMouse:` 为 NO，窗口未取得激活态时
//      AppKit 把落在本进程的第一次 `mouseDown:` 用作「点击获焦」而不转发给视图（实测无铺垫时
//      十轮里两三次首点丢失，失败集合恒为「首点未翻 + 次点翻到了 + 终态非未选中」＝少一次转发
//      而非管线断裂；加铺垫后连跑十轮 20/20 全绿）。顺带记下两条实测前提：**key 态与前台态都
//      不是前提**（本探针全程 `app_active=0 window_key=0 visible=1`，队列路径按事件自带的
//      `windowNumber:` 派发，与 key 态无关），故这些状态只打印供判读、不立为判据；
//   ✅ CGEvent 加强腿的**自我申报**：未授权时 `e2e::cg_click` / `e2e::cg_text` 带原因回
//      不可用，且裸 `CGEventPostToPid` 实测一个事件都不送达（状态纹丝不动）——这条判据
//      把「静默无效」的形态固定下来，防止日后有人拿它当证据。
//
//   ❌ 不证明真实 HID 输入流（硬件鼠标 / 键盘 → WindowServer → 本进程）：那需要辅助功能
//      授权，实测 SIP 环境下无法脚本授予（`tccutil` 只能重置不能写入），故已授权时本探针才
//      追加 CGEvent 全局注入的判据，未授权时如实记 SKIP；
//   ❌ 不证明拖拽 / 滚轮 / 多点触控与 IME 组合输入：本 fixture 只有指针点击与单字符键盘，
//      IME 与滚轮面另见 `macos_surface_live_probe` 与人工用例。
//
// 构建：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_os_input
//   ./build-macos/aurora_verify_macos_os_input
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（无法创建窗口：无 WindowServer / 无图形会话）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_os_input 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/state/state.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text_input.h"

#include "e2e/harness.h"
#include "e2e/os_input.h"

#include "verify_print.h"

namespace {

// 探针记账：与 macos_ax_live_probe 同款（进程私有、匿名命名空间、就地豁免非 const 全局）。
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
/// 命名取 `AURORA_` + 全大写：文件作用域常量在门禁里只认这个形态（`GlobalConstantCase`）。
constexpr const char *AURORA_FRAME_TITLE = "aurora_verify_macos_os_input";

// AppKit 的 accessibility* 取值口可返 nil，形参显式收 _Nullable，否则撞空性检查。
// `UTF8String` 本身也是 `_Nullable` 的返回值：先落地成局部量再判空，重复发消息的写法会让
// 空性分析器看不到「同一枚指针已判过非空」（NullablePassedToNonnull）。
auto ns_to_std(NSString *_Nullable text) -> std::string {
    if (text == nil) {
        return {};
    }
    const char *_Nullable bytes = [text UTF8String];
    return bytes != nullptr ? std::string{bytes} : std::string{};
}

/// @brief 采集到的协议面元素（只为拿矩形；角色投影本身由 `aurora_verify_macos_ax` 取证）。
struct ProjectedElement {
    std::string role;
    std::string name;
    NSRect frame = NSZeroRect;
};

/// @brief 先序遍历内容视图的无障碍子树（封顶防失控，与 `aurora_verify_macos_ax` 同口径）。
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
    if (!item.role.empty()) {
        out.push_back(std::move(item));
    }
    // ObjC 快速枚举的循环变量由运行时逐个赋值，语法上无法声明时初始化（`id child = nil` 反而
    // 会被随即覆盖），故该检查在此逐点豁免。
    for (id child in [node accessibilityChildren]) {  // NOLINT(cppcoreguidelines-init-variables)
        walk(child, depth + 1, out);
    }
}

/// @brief 只按 AppKit 角色找首个元素（本探针只吃几何，名称面归 `aurora_verify_macos_ax`）。
auto find_by_role(const std::vector<ProjectedElement> &elements, NSString *role) -> const ProjectedElement * {
    const std::string want = ns_to_std(role);
    for (const auto &element : elements) {
        if (element.role == want) {
            return &element;
        }
    }
    return nullptr;
}

/// @brief 屏幕点（Cocoa 左下原点）→ 窗口基坐标点（`NSEvent.locationInWindow` 的口径）。
///
/// 取 2×2 矩形折算中心，避开四舍五入把点掉到盒外。
[[nodiscard]] auto window_point_for(NSWindow *window, NSRect screen_box) -> CGPoint {
    const NSRect base = [window convertRectFromScreen:screen_box];
    return CGPointMake(NSMidX(base), NSMidY(base));
}

/// @brief 经本进程 AppKit 事件队列投递一次左键点击（按下 + 抬起）。
///
/// `postEvent:atStart:` 把事件放进 `NSApp` 队列，由 `poll_platform_events` 的
/// `nextEventMatchingMask:` 取出后 `sendEvent:` 派发——与真实点击进队列后同一条路。
/// `atStart:NO` 追加到队尾：先前泵出的窗口态事件先处理完，命中测试才对着当前布局。
auto post_click(NSWindow *window, CGPoint point) -> bool {
    const NSTimeInterval base = [NSApp currentEvent] != nil ? [[NSApp currentEvent] timestamp] : 0.0;
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:point
                                  modifierFlags:0
                                      timestamp:base + 0.01
                                   windowNumber:[window windowNumber]
                                        context:nil
                                    eventNumber:1
                                     clickCount:1
                                       pressure:1.0];
    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:point
                                modifierFlags:0
                                    timestamp:base + 0.02
                                 windowNumber:[window windowNumber]
                                      context:nil
                                  eventNumber:2
                                   clickCount:1
                                     pressure:0.0];
    if (down == nil || up == nil) {
        return false;
    }
    [NSApp postEvent:down atStart:NO];
    [NSApp postEvent:up atStart:NO];
    return true;
}

/// @brief 经本进程 AppKit 事件队列投递一个字符键（按下；`characters:` 即字符通道载荷）。
auto post_key(NSWindow *window, CGPoint point, NSString *character) -> bool {
    const NSTimeInterval stamp = [NSApp currentEvent] != nil ? [[NSApp currentEvent] timestamp] + 0.5 : 0.5;
    NSEvent *down = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                     location:point
                                modifierFlags:0
                                    timestamp:stamp
                                 windowNumber:[window windowNumber]
                                      context:nil
                                   characters:character
                  charactersIgnoringModifiers:character
                                    isARepeat:NO
                                      keyCode:0U];
    if (down == nil) {
        return false;
    }
    [NSApp postEvent:down atStart:NO];
    return true;
}

/// @brief 语义盒（dp，窗口本地）→ 物理像素盒（× scale，外扩 1px 后夹到帧内）。
struct PixelBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

[[nodiscard]] auto pixel_box(const aurora::Rect &bounds_dp, float scale, int width, int height) -> PixelBox {
    PixelBox box;
    box.x0 = std::max(0, static_cast<int>(std::floor(bounds_dp.origin.x * scale)) - 1);
    box.y0 = std::max(0, static_cast<int>(std::floor(bounds_dp.origin.y * scale)) - 1);
    box.x1 =
        std::min(width, static_cast<int>(std::ceil((bounds_dp.origin.x * scale) + (bounds_dp.size.width * scale))) + 1);
    box.y1 = std::min(height,
                      static_cast<int>(std::ceil((bounds_dp.origin.y * scale) + (bounds_dp.size.height * scale))) + 1);
    return box;
}

/// @brief 两帧在给定像素盒内是否有差异。
[[nodiscard]] auto region_differs(const aurora::e2e::Frame &before, const aurora::e2e::Frame &after,
                                  const PixelBox &box) -> bool {
    if (before.width != after.width || before.height != after.height) {
        return true;
    }
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            if (aurora::e2e::pixel_at(before, x, y) != aurora::e2e::pixel_at(after, x, y)) {
                return true;
            }
        }
    }
    return false;
}

/// @brief 泵若干轮平台事件（注入的事件要在这里被取出并派发）。
auto pump(aurora::e2e::Session &session, int rounds) -> void {
    for (int i = 0; i < rounds; ++i) {
        session.surface().wait_events(20.0);
    }
}

/// @brief 读回当前 Checkbox 的语义状态（翻转判据的唯一来源）。
[[nodiscard]] auto checkbox_state(const aurora::e2e::Session &session) -> bool {
    const auto snapshot = session.semantic_snapshot();
    if (!snapshot.ok()) {
        return false;
    }
    const auto *node = aurora::e2e::find_semantic_node(snapshot.value(), aurora::AccessibilityRole::Checkbox);
    return node != nullptr && node->node.state.checked;
}

/// @brief 读回指定角色的语义盒（未命中给空盒）。
[[nodiscard]] auto semantic_bounds(const aurora::e2e::Session &session, aurora::AccessibilityRole role)
    -> aurora::Rect {
    const auto snapshot = session.semantic_snapshot();
    if (!snapshot.ok()) {
        return aurora::Rect{};
    }
    const auto *node = aurora::e2e::find_semantic_node(snapshot.value(), role);
    return node != nullptr ? node->node.bounds : aurora::Rect{};
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与既有探针同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    @autoreleasepool {
        // ---- 建窗与挂载：与 etest_os_input 同一装配（Normal 档：OS 输入不打到隐藏窗口）----
        aurora::e2e::WindowSpec spec;
        spec.backend = aurora::e2e::Backend::MacOS;
        spec.width = 320;
        spec.height = 200;
        spec.title = AURORA_FRAME_TITLE;
        spec.visibility = aurora::WindowVisibility::Normal;
        aurora::e2e::Session session = aurora::e2e::open(spec);
        if (!session.ok()) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: ", session.reason(), "\n");
            return 2;
        }

        // 与 etest_os_input 的注入目标同款 UI：TextInput 在上（键盘目标）、Checkbox 在下
        // （指针目标），键盘值经 `on_changed` 捕获后回读——判据只看外部可读的实值。
        const auto checked = aurora::State<bool>{false};
        auto typed = std::make_shared<std::string>();
        aurora::Node input_node{aurora::TextInput{aurora::TextInputProps{.value = "", .placeholder = "Type here"}}};
        static_cast<aurora::TextInput &>(input_node.widget())  // NOLINT(*-pro-type-static-cast-downcast)
            .set_on_changed([captured = typed](const std::string &v) { *captured = v; });
        aurora::Node root{aurora::Column{std::move(input_node), aurora::Checkbox{checked}}};

        const auto presented = session.present(root);
        check(presented.ok(), "present fixture", presented ? "" : presented.error().message);
        const auto settled = session.pump_until_settled();
        check(settled.ok(), "pump_until_settled", settled ? "" : settled.error().message);
        const auto desktop = aurora::e2e::interactive_desktop();
        check(desktop.interactive, "window server face available", desktop.reason);

        auto *window = (__bridge NSWindow *)session.surface().native_handle();
        if (window == nil) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: surface exposes no NSWindow\n");
            return 2;
        }
        const float scale = session.surface().scale_factor();

        // ---- 目标矩形：走 AppKit 协议面（读屏/客户端用的同一份几何）----
        std::vector<ProjectedElement> elements;
        walk((id<NSAccessibility>)window.contentView, 0, elements);
        const auto *box_element = find_by_role(elements, NSAccessibilityCheckBoxRole);
        const auto *field_element = find_by_role(elements, NSAccessibilityTextFieldRole);
        check(box_element != nullptr, "checkbox rect projected on the protocol face",
              box_element != nullptr ? ns_to_std(NSStringFromRect(box_element->frame)) : "missing");
        check(field_element != nullptr, "text field rect projected on the protocol face",
              field_element != nullptr ? ns_to_std(NSStringFromRect(field_element->frame)) : "missing");
        if (box_element == nullptr || field_element == nullptr) {
            AURORA_LOG_RAW("verify", "----------------------------------------\n",
                           "summary: ", aurora_verify::format_int(g_checks - g_failures), "/",
                           aurora_verify::format_int(g_checks), " checks passed\n");
            return g_failures == 0 ? 0 : 1;
        }
        const CGPoint checkbox_point = window_point_for(window, box_element->frame);
        const CGPoint field_point = window_point_for(window, field_element->frame);

        // ---- 注入前的铺垫：真人式的「先点一次窗口」----
        // 实测形态：不带铺垫直接首点，同一份代码偶发「首点被吃掉」（十轮里两三次），失败集合恒为
        // 「首点未翻 + 次点翻到了 + 终态非未选中」——少了一次转发，而不是派发链断了。原因是
        // `NSView` 默认 `acceptsFirstMouse:` 为 NO：窗口尚未取得激活态时，AppKit 把落在本进程的
        // 第一次 `mouseDown:` 用作「点击获焦」而不转发给视图。真人对此无感（点第一下本来就只是
        // 激活窗口），判据必须把这一枚 OS 语义前提显式化。
        // 注意 key 态**不是**前提：实测本探针里 `[window isKeyWindow]` 到 5 秒有界等待结束仍为
        // NO（经命令行启动的二进制不是前台应用），而两条腿照样全绿——队列路径的派发看的是事件
        // 自带的 `windowNumber:`，与 key 态无关。故此处只记账激活状态供判读，不立为判据。
        AURORA_LOG_RAW("verify", "state app_active=", [NSApp isActive] == YES ? "1" : "0",
                       " window_key=", [window isKeyWindow] == YES ? "1" : "0",
                       " visible=", [window isVisible] == YES ? "1" : "0", "\n");
        // 铺垫点取内容视图左下角：该处无 widget（fixture 只有顶部两枚控件），命中它只改激活态。
        check(post_click(window, CGPointMake(2.0, 2.0)), "focus primer click posted");
        pump(session, 60);
        (void)session.pump_until_settled();
        check(!checkbox_state(session), "focus primer leaves the checkbox untouched");

        // ---- 判据一：AppKit 队列点击 → Checkbox 翻转 + 区域内像素变化 ----
        const aurora::Rect box_dp = semantic_bounds(session, aurora::AccessibilityRole::Checkbox);
        const auto before = session.read_pixels();
        check(before.ok(), "read_pixels before", before ? "" : before.error().message);
        const auto before_frame = before.ok() ? before.value() : aurora::e2e::Frame{};
        const auto box_px = pixel_box(box_dp, scale, before_frame.width, before_frame.height);
        check(!checkbox_state(session), "checkbox starts unchecked");

        const bool posted = post_click(window, checkbox_point);
        check(posted, "appkit queue click posted", "window point " + ns_to_std(NSStringFromPoint(checkbox_point)));
        pump(session, 60);
        (void)session.pump_until_settled();
        check(checkbox_state(session), "click toggles checkbox on");
        const auto after = session.read_pixels();
        check(after.ok(), "read_pixels after", after ? "" : after.error().message);
        const auto after_frame = after.ok() ? after.value() : aurora::e2e::Frame{};
        check(region_differs(before_frame, after_frame, box_px), "pixels change inside the checkbox box");

        // ---- 判据二：再点一次翻回去（往返，不是单向巧合）----
        check(post_click(window, checkbox_point), "second click posted");
        pump(session, 60);
        (void)session.pump_until_settled();
        check(!checkbox_state(session), "second click toggles checkbox back off");

        // ---- 判据三：键盘通道——点击 TextInput 获焦后合成 keyDown 上屏 ----
        check(post_click(window, field_point), "text field click posted");
        pump(session, 60);
        (void)session.pump_until_settled();
        check(post_key(window, field_point, @"a"), "keyDown posted");
        pump(session, 60);
        (void)session.pump_until_settled();
        check(*typed == "a", "character reaches the text input", "typed=\"" + *typed + "\"");

        // ---- 判据四：CGEvent 加强腿（已授权则真注入，未授权则申报不可用并复核静默形态）----
        if (aurora::e2e::cg_input_channel_available()) {
            session.surface().raise();
            session.surface().focus_window();
            pump(session, 20);
            const double height = aurora::e2e::cg_display_height();
            const aurora::Point global{
                .x = static_cast<float>(box_element->frame.origin.x + (box_element->frame.size.width / 2.0)),
                .y = static_cast<float>(height -
                                        (box_element->frame.origin.y + (box_element->frame.size.height / 2.0)))};
            const auto clicked = aurora::e2e::cg_click(global);
            check(clicked.ok(), "cg_click accepted", clicked ? "" : clicked.error().message);
            pump(session, 120);
            (void)session.pump_until_settled();
            check(checkbox_state(session), "cg_click toggles checkbox through the real input stream");
        } else {
            const auto refused = aurora::e2e::cg_click(aurora::Point{.x = 10.0F, .y = 10.0F});
            check(!refused.ok() && refused.error().code_enum == aurora::ErrorCode::GeneralNotSupported,
                  "cg channel self-reports unavailable without the grant", refused ? "" : refused.error().message);
            // 复核「静默无效」这一实测形态：裸投递不报错，但一个事件都不送达（状态不变）。
            const CGPoint raw = CGPointMake(box_element->frame.origin.x + (box_element->frame.size.width / 2.0),
                                            aurora::e2e::cg_display_height() -
                                                (box_element->frame.origin.y + (box_element->frame.size.height / 2.0)));
            CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
            CGEventRef down = CGEventCreateMouseEvent(source, kCGEventLeftMouseDown, raw, kCGMouseButtonLeft);
            CGEventRef up = CGEventCreateMouseEvent(source, kCGEventLeftMouseUp, raw, kCGMouseButtonLeft);
            CGEventPostToPid(::getpid(), down);
            CGEventPostToPid(::getpid(), up);
            CFRelease(down);
            CFRelease(up);
            CFRelease(source);
            pump(session, 120);
            (void)session.pump_until_settled();
            check(!checkbox_state(session), "untrusted CGEventPostToPid delivers nothing at all");
            AURORA_LOG_RAW("verify", "SKIP  authorized CGEvent leg (process is not Accessibility-trusted; "
                                     "SIP blocks scripted grants, see the manual-test record)\n");
        }

        AURORA_LOG_RAW("verify", "----------------------------------------\n",
                       "summary: ", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        return g_failures == 0 ? 0 : 1;
    }
}
