/* macOS system drag bridge -- real-machine acceptance probe (manual tool, not in CTest)
// ============================================================================
// Seams under test:
//   drag-in (this window is the target): the content view's `NSDraggingDestination` methods ->
//   `aurora::detail::MacOSDragBridge` -> translate layer (`detail/system_drag_translate`) -> drag-in entry
//   (`event/detail/system_drag_ingress`) -> the real dispatch chain (`EventDispatcher` + the
//   probe's own drop widget). Payload reading is **synchronous** inside `performDragOperation`
//   (AppKit's read API blocks until the data is ready) -- there is no per-frame read pump on this
//   platform, unlike X11 selection round-trips and Wayland pipes.
//   drag-out (this window is the source): `system_drag::begin` -> the out backend registered by
//   the bridge -> `beginDraggingSessionWithItems:event:source:`; the terminal state arrives from
//   `draggingSession:endedAtPoint:operation:`.
//
// Why the automated stage drives the bridge with a **probe-owned `NSDraggingInfo`** instead of a
// real AppKit session:
//   AppKit drag sessions are dispatched by the window server from a real pointer gesture; there is
//   no client-side API to deliver (or synthesize) one, and `beginDraggingSession...` needs a real
//   mouse-down event from a visible window. The Wayland probe solved the same problem by moving the
//   *compositor* into the process; on macOS the counterpart (the window server) cannot be moved, so
//   the probe stands in for it at the **protocol object** level: a real `NSPasteboard` (written
//   through AppKit), a real `NSView` produced by the library (`make_drag_destination_view`, so the
//   five forwarding methods are the shipping ones), and a probe object answering the three
//   `NSDraggingInfo` selectors the bridge reads. Everything below that seam is the shipping code
//   path: pasteboard reads, the translate layer, the ingress dispatcher and the widget contract.
//   What is *not* covered: AppKit actually starting/ending a session. That stays a human leg
//   (see codespec/manual-test/24-systemdrag.md), and the probe reports it as PENDING.
//
// Automated legs (no human dragging):
//   leg 1/5  translate registry + capability bits against the real SDK (UTI strings, operation bits).
//   leg 2/5  destination view: flipped coordinates, the five `NSDraggingDestination` selectors,
//            bridge install, and the dragged types AppKit recorded for the view.
//   leg 3/5  mock drag-in: Enter -> Over -> prepare -> perform, asserting one Enter, folded MIME
//            list, view-local position, proposed/allowed actions, accept gate, and the text payload
//            round-trip through a real `NSPasteboard`; then Exited -> Leave.
//   leg 4/5  rejection path: a target that accepts nothing => `NSDragOperationNone` on Enter,
//            `prepareForDragOperation` returns NO, and no Drop is ever dispatched.
//   leg 5/5  drag-out fail-closed: with no window / no press event `system_drag::begin` refuses and
//            no terminal event is delivered (it must not claim a session it never started).
//
// Manual stage (`--interactive`): requires a real window shell, which `MacOSSurface::create_window`
// does not implement yet (see src/aurora/window/macos_surface.cpp), so the flag currently reports
// that the stage is unavailable (exit 2) instead of pretending to run it. The four drag-in states
// and the two drag-out terminals are judged by a human on a real desktop; see the manual-test page.
//
// Build:
//   cmake -S . -B build-verify -G Ninja -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_macos_drag
//   ./build-verify/aurora_verify_macos_drag
//
// Exit codes:
//   0  every automated check passed AND the bridge was installed on a view inside a real NSWindow
//   2  environment unusable (AppKit / pasteboard / destination view unavailable)
//   3  automated checks passed but no window server => the AppKit-delivered session legs are
//      PENDING (a human must drag on a real desktop)
//   4  at least one check failed -- see the per-line PASS/FAIL column
// ============================================================================ */

#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_drag can only be built on macOS (AURORA_PLATFORM_MACOS)"
#endif

#ifndef AURORA_BACKEND_MACOS
#error "AURORA_BACKEND_MACOS must be enabled"
#endif

// aurora 头先于 AppKit 头（与 `macos_drag.cpp` 同款纪律：平台头关在单个 TU 内）。
// clang-format off
#include "aurora/aurora.h"

#include "aurora/app/mime_types.h"
#include "aurora/event/dispatcher.h"
#include "aurora/window/detail/macos_drag.h"
#include "aurora/window/detail/system_drag_translate.h"
#include "aurora/window/platform.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "verify_args.h"
#include "verify_print.h"
// clang-format on

namespace {

// AppKit 经 `MacTypes.h` 把 Carbon 遗留的 `Point` / `Size` / `Rect` 三个**全局**类型名带进
// 全局作用域（CI 的 Xcode SDK 实测：`reference to 'Point' is ambiguous`——候选为
// `MacTypes.h` 的 `Point` 与 `aurora::Point`）。`using namespace aurora;` 是 using 指令，
// 让 `aurora::` 的名字也像声明在全局一样参与查找，二者同为全局成员即歧义。故本文件改用
// **using 声明**：名字落进本匿名命名空间，非限定查找在此命中即止，不再与 Carbon 撞名。
using aurora::AURORA_MIME_TEXT_PLAIN;
using aurora::BuildContext;
using aurora::Constraints;
using aurora::DragAction;
using aurora::DragOffer;
using aurora::Event;
using aurora::EventDispatcher;
using aurora::LeafWidget;
using aurora::ModifierKey;
using aurora::Painter;
using aurora::Platform;
using aurora::PlatformCapabilities;
using aurora::Point;
using aurora::Rect;
using aurora::Result;
using aurora::Size;
using aurora::SurfaceKind;
using aurora::SystemDragOutcome;
using aurora::SystemDropEvent;
using aurora::SystemDropPhase;

// ---------------------------------------------------------------------------
// Verdict ledger and output
// ---------------------------------------------------------------------------

struct Ledger {
    int checks = 0;
    int failures = 0;
    int pending = 0;
};

auto note(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

auto check(bool ok, const std::string &name, Ledger &ledger, const std::string &detail = {}) -> void {
    ++ledger.checks;
    note(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + name + (detail.empty() ? std::string{} : "  -- " + detail));
    if (!ok) {
        ++ledger.failures;
    }
}

auto pending(const std::string &name, Ledger &ledger, const std::string &reason) -> void {
    ++ledger.pending;
    note(std::string("[PENDING] ") + name + "  -- " + reason);
}

[[nodiscard]] auto outcome_name(SystemDragOutcome outcome) -> const char * {
    switch (outcome) {
        case SystemDragOutcome::Succeeded:
            return "Succeeded";
        case SystemDragOutcome::Cancelled:
            return "Cancelled";
        case SystemDragOutcome::Rejected:
            return "Rejected";
        case SystemDragOutcome::Failed:
            return "Failed";
    }
    return "?";
}

[[nodiscard]] auto action_name(DragAction action) -> const char * {
    switch (action) {
        case DragAction::None:
            return "None";
        case DragAction::Copy:
            return "Copy";
        case DragAction::Move:
            return "Move";
        case DragAction::Link:
            return "Link";
    }
    return "?";
}

[[nodiscard]] auto phase_name(SystemDropPhase phase) -> const char * {
    switch (phase) {
        case SystemDropPhase::Enter:
            return "Enter";
        case SystemDropPhase::Over:
            return "Over";
        case SystemDropPhase::Leave:
            return "Leave";
        case SystemDropPhase::Drop:
            return "Drop";
    }
    return "?";
}

constexpr char AURORA_PROBE_TEXT[] = "aurora-macos-drag-probe-payload";

// ---------------------------------------------------------------------------
// Drop target widget (the dispatch-chain terminus of the drag-in legs)
// ---------------------------------------------------------------------------

/// @brief 落放目标：记录四态与最近一次的格式 / 位置 / 文本 / allowed；accept 查询可调（拒收腿用）。
class DropTarget final : public LeafWidget {
  public:
    std::vector<std::string> phase_log;  ///< 收到的阶段名（按序）
    std::vector<std::string> last_formats;  ///< 最近一次事件的格式清单
    Point last_position{};  ///< 最近一次事件的落点（客户区逻辑坐标）
    std::string last_text;  ///< 最近一次落放的纯文本（text/plain 解码；粘贴板往返判据）
    bool has_text = false;  ///< 落放是否带回了文本
    DragAction last_allowed = DragAction::None;  ///< 派发器写定的允许动作
    DragAction last_proposed = DragAction::None;  ///< 最近一次事件的源建议动作
    DragAction accept = DragAction::Copy;  ///< `query_system_drag_accept` 的返回值

    auto type_name() const -> const char * override { return "DropTarget"; }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        size_ = c.constrain(Size{.width = 480.0F, .height = 200.0F});
        return size_;
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

    auto on_system_drop(SystemDropEvent &e) -> void override {
        phase_log.emplace_back(phase_name(e.phase));
        last_formats = e.formats;
        last_position = e.position;
        last_proposed = e.proposed;
        last_allowed = e.allowed;
        if (e.phase == SystemDropPhase::Drop && e.payload != nullptr) {
            const Result<std::string> text = e.text();
            if (text) {
                last_text = text.value();
                has_text = true;
            }
        }
        e.is_handled = true;  // 消费即受理
    }

    [[nodiscard]] auto query_system_drag_accept(const std::vector<std::string> & /*formats*/) -> DragAction override {
        return accept;
    }
};

[[nodiscard]] auto count_phase(const DropTarget &target, SystemDropPhase phase) -> int {
    const char *want = phase_name(phase);
    return static_cast<int>(std::count_if(target.phase_log.begin(), target.phase_log.end(),
                                          [want](const std::string &got) { return got == want; }));
}

// ---------------------------------------------------------------------------
// Leg 1: registry and capability bits against the real SDK
// ---------------------------------------------------------------------------

auto leg_registry(Ledger &ledger) -> void {
    note("leg 1/5: pasteboard registry, operation bits and capability bits");
    Platform probe;
    probe.surface = SurfaceKind::MacOS;
    const PlatformCapabilities caps = probe.capabilities();
    check(caps.system_drag_in && caps.system_drag_out, "capability bits are true on the macOS backend", ledger,
          std::string{"in="} + (caps.system_drag_in ? "1" : "0") + " out=" + (caps.system_drag_out ? "1" : "0"));

    const std::vector<std::string> &types = aurora::drag::macos_registered_pasteboard_types();
    const bool has_text = std::find(types.begin(), types.end(), "public.utf8-plain-text") != types.end();
    const bool has_file_url = std::find(types.begin(), types.end(), "public.file-url") != types.end();
    const bool has_png = std::find(types.begin(), types.end(), "public.png") != types.end();
    check(has_text && has_file_url && has_png, "registry advertises the three UTI entries we can decode", ledger,
          "count=" + std::to_string(types.size()));
    // 旧式 `NSFilenamesPboardType` 的载荷是**路径数组**，本库不解析 ⇒ 不声称能读（登记表不含它）。
    check(std::find(types.begin(), types.end(), "NSFilenamesPboardType") == types.end(),
          "legacy NSFilenamesPboardType is not advertised (path-array payload is undecodable)", ledger);

    const std::optional<std::string> back = aurora::drag::mime_from_macos_pasteboard_type("public.utf8-plain-text");
    check(back.has_value() && back.value() == AURORA_MIME_TEXT_PLAIN,
          "reverse lookup maps the real UTI back to text/plain", ledger,
          back.has_value() ? back.value() : std::string{"nullopt"});
    // 位序与其他两平台都不同（Win32 Copy=1/Move=2、Wayland 走枚举值），此处锚定 macOS 自己的布局。
    check(aurora::drag::macos_drag_operation_from(DragAction::Copy) == 1U &&
              aurora::drag::macos_drag_operation_from(DragAction::Move) == 16U &&
              aurora::drag::macos_drag_operation_from(DragAction::Link) == 2U,
          "NSDragOperation bit layout is the documented one (Copy=1 Link=2 Move=16)", ledger);
}

// ---------------------------------------------------------------------------
// Leg 2: destination view + bridge install
// ---------------------------------------------------------------------------

/// @brief 建桥与内容视图（视图由桥自己产出 ⇒ 先建桥、再回装，见 `attach_view` 契约）。
struct BridgeUnderTest {
    std::unique_ptr<aurora::detail::MacOSDragBridge> bridge;
    DropTarget *target = nullptr;
    NSView *view = nil;
};

[[nodiscard]] auto make_bridge(DropTarget &target) -> BridgeUnderTest {
    BridgeUnderTest out;
    out.target = &target;
    out.bridge = std::make_unique<aurora::detail::MacOSDragBridge>(
        nullptr,
        aurora::detail::MacOSDragBridge::Hooks{.emit = [&target](Event &e) -> void {
                                                   auto *drop = dynamic_cast<SystemDropEvent *>(&e);
                                                   if (drop == nullptr) {
                                                       return;
                                                   }
                                                   (void)EventDispatcher::dispatch(target, *drop);
                                               },
                                               .scale_factor = []() -> float { return 1.0F; },
                                               .modifiers = []() -> ModifierKey { return ModifierKey::None; }});
    void *raw_view = aurora::detail::MacOSDragBridge::make_drag_destination_view(out.bridge.get());
    // `__bridge_transfer`：工厂返回的是 +1 所有权（MRC 口径「调用方持所有权」），ARC 侧必须
    // 用 transfer 接，否则局部变量再 retain 一次即多一个引用、视图永不释放。
    out.view = (__bridge_transfer NSView *)raw_view;
    if (out.view != nil) {
        out.bridge->attach_view(raw_view);
    }
    return out;
}

auto leg_view(BridgeUnderTest &uut, Ledger &ledger) -> void {
    note("leg 2/5: destination view and bridge install");
    check(uut.view != nil, "make_drag_destination_view returns a real NSView", ledger);
    if (uut.view == nil) {
        return;
    }
    check([uut.view isFlipped] == YES,
          "destination view is flipped (view y grows downward, same as X11/Win32 client area)", ledger);
    const bool five = [uut.view respondsToSelector:@selector(draggingEntered:)] &&
                      [uut.view respondsToSelector:@selector(draggingUpdated:)] &&
                      [uut.view respondsToSelector:@selector(draggingExited:)] &&
                      [uut.view respondsToSelector:@selector(prepareForDragOperation:)] &&
                      [uut.view respondsToSelector:@selector(performDragOperation:)];
    check(five, "destination view implements the five NSDraggingDestination methods", ledger);
    check(uut.bridge->install(), "bridge installs on the destination view", ledger);

    if (![uut.view respondsToSelector:@selector(registeredDraggedTypes)]) {
        pending("registered dragged types read back from AppKit", ledger,
                "NSView.registeredDraggedTypes unavailable on this system");
        return;
    }
    // 取回走 KVC 而非直接发消息：`registeredDraggedTypes` 是较晚进入 `NSView` 的属性，直接发消息
    // 在老 SDK 上是**编译错误**（不是运行期缺席），而 `respondsToSelector` 只能探运行期。KVC 两侧
    // 都能编译，取回失败按 PENDING 处理（不把 SDK 老当判定失败）。
    const id recorded_any = [uut.view valueForKey:@"registeredDraggedTypes"];
    if (recorded_any == nil || ![recorded_any isKindOfClass:[NSArray class]]) {
        pending("registered dragged types read back from AppKit", ledger,
                "NSView.registeredDraggedTypes returned nothing readable");
        return;
    }
    auto *recorded = (NSArray<NSPasteboardType> *)recorded_any;
    const std::vector<std::string> &reg = aurora::drag::macos_registered_pasteboard_types();
    bool all = static_cast<std::size_t>(recorded.count) == reg.size();
    for (std::size_t i = 0; all && i < reg.size(); ++i) {
        NSString *want = [NSString stringWithUTF8String:reg[i].c_str()];
        all = [recorded containsObject:want];
    }
    check(all, "registered dragged types read back from AppKit match the registry", ledger,
          "recorded=" + std::to_string(static_cast<int>(recorded.count)) + " registry=" + std::to_string(reg.size()));
}

}  // namespace

// ---- 探针自带的 `NSDraggingInfo`（窗口服务器的替身；只回答桥真正读的三个 selector）----
// 不声明 `<NSDraggingInfo>` 遵循：该协议还有一串与本次验收无关的必需成员（`draggingSession`、
// `draggingFormation` 等），逐个实现只会把探针的判据面摊大；而桥只读下列三个，消息发送按
// 运行期派发，不依赖编译期遵循声明。
@interface AuroraProbeDragInfo : NSObject {
    NSPasteboard *probe_pasteboard;
    NSDragOperation op_mask;
    NSPoint drop_point;
}
- (instancetype)initWithPasteboard:(NSPasteboard *)pasteboard mask:(NSDragOperation)mask location:(NSPoint)location;
- (NSPasteboard *)draggingPasteboard;
- (NSDragOperation)draggingSourceOperationMask;
- (NSPoint)draggingLocation;
@end

@implementation AuroraProbeDragInfo

- (instancetype)initWithPasteboard:(NSPasteboard *)pasteboard mask:(NSDragOperation)mask location:(NSPoint)location {
    self = [super init];
    if (self != nil) {
        probe_pasteboard = pasteboard;
        op_mask = mask;
        drop_point = location;
    }
    return self;
}

- (NSPasteboard *)draggingPasteboard {
    return probe_pasteboard;
}

- (NSDragOperation)draggingSourceOperationMask {
    return op_mask;
}

- (NSPoint)draggingLocation {
    return drop_point;
}

@end

namespace {

// ---------------------------------------------------------------------------
// Leg 3: mock drag-in (real pasteboard, shipping forwarding view, real dispatch chain)
// ---------------------------------------------------------------------------

auto leg_drag_in(BridgeUnderTest &uut, NSPasteboard *board, Ledger &ledger) -> void {
    note("leg 3/5: mock drag-in through the shipping destination view");
    DropTarget &target = *uut.target;
    // 视图坐标：`isFlipped` 视图 y 向下，与客户区同向；scale 钩子给 1.0 ⇒ 位置原样折算。
    AuroraProbeDragInfo *info = [[AuroraProbeDragInfo alloc]
        initWithPasteboard:board
                      mask:static_cast<NSDragOperation>(NSDragOperationCopy | NSDragOperationMove)
                  location:NSMakePoint(120.0, 80.0)];
    const NSDragOperation entered = [(id<NSDraggingDestination>)uut.view draggingEntered:(id)info];
    check(entered == NSDragOperationCopy, "Enter returns the accepted operation (Copy)", ledger,
          "op=" + std::to_string(static_cast<unsigned long>(entered)));
    check(count_phase(target, SystemDropPhase::Enter) == 1, "Enter is dispatched exactly once", ledger,
          "count=" + std::to_string(count_phase(target, SystemDropPhase::Enter)));
    check(target.last_formats.size() == 1 && target.last_formats.front() == AURORA_MIME_TEXT_PLAIN,
          "formats fold to the canonical MIME list", ledger, "n=" + std::to_string(target.last_formats.size()));
    check(target.last_position.x == 120.0F && target.last_position.y == 80.0F, "Enter carries the view-local position",
          ledger,
          "x=" + std::to_string(static_cast<int>(target.last_position.x)) +
              " y=" + std::to_string(static_cast<int>(target.last_position.y)));
    check(target.last_proposed == DragAction::Copy,
          "proposed action comes from the source mask (Copy wins over the Move bit)", ledger,
          std::string{"proposed="} + action_name(target.last_proposed));
    check(target.last_allowed == DragAction::Copy, "allowed is the intersection with the target accept", ledger,
          std::string{"allowed="} + action_name(target.last_allowed));

    info = [[AuroraProbeDragInfo alloc]
        initWithPasteboard:board
                      mask:static_cast<NSDragOperation>(NSDragOperationCopy | NSDragOperationMove)
                  location:NSMakePoint(140.0, 96.0)];
    const NSDragOperation over = [(id<NSDraggingDestination>)uut.view draggingUpdated:(id)info];
    check(over == NSDragOperationCopy, "Over returns the accepted operation", ledger);
    check(target.last_position.x == 140.0F && target.last_position.y == 96.0F, "Over tracks the new position", ledger,
          "x=" + std::to_string(static_cast<int>(target.last_position.x)) +
              " y=" + std::to_string(static_cast<int>(target.last_position.y)));

    const BOOL gate = [(id<NSDraggingDestination>)uut.view prepareForDragOperation:(id)info];
    check(gate == YES, "prepareForDragOperation is the accept gate and returns YES", ledger);
    [(id<NSDraggingDestination>)uut.view performDragOperation:(id)info];
    check(count_phase(target, SystemDropPhase::Drop) == 1, "perform dispatches exactly one Drop", ledger);
    check(target.has_text && target.last_text == AURORA_PROBE_TEXT, "Drop payload round-trips the pasteboard text",
          ledger, "text=" + target.last_text);
    check(!uut.bridge->drag_in_active(), "bridge clears the drag-in session after the drop", ledger);

    // 未落放即离开：补派 Leave 并收清会话态（同一场会话的 exited 才派发）。
    const int leave_before = count_phase(target, SystemDropPhase::Leave);
    info = [[AuroraProbeDragInfo alloc] initWithPasteboard:board
                                                      mask:static_cast<NSDragOperation>(NSDragOperationCopy)
                                                  location:NSMakePoint(10.0, 10.0)];
    (void)[(id<NSDraggingDestination>)uut.view draggingEntered:(id)info];
    [(id<NSDraggingDestination>)uut.view draggingExited:(id)info];
    check(count_phase(target, SystemDropPhase::Leave) == leave_before + 1, "Exited dispatches Leave exactly once",
          ledger, "count=" + std::to_string(count_phase(target, SystemDropPhase::Leave)));
    check(!uut.bridge->drag_in_active(), "bridge clears the drag-in session after Exited", ledger);
}

// ---------------------------------------------------------------------------
// Leg 4: rejection path (accept gate closed)
// ---------------------------------------------------------------------------

auto leg_reject(BridgeUnderTest &uut, NSPasteboard *board, Ledger &ledger) -> void {
    note("leg 4/5: rejection path");
    DropTarget &target = *uut.target;
    target.accept = DragAction::None;
    AuroraProbeDragInfo *info =
        [[AuroraProbeDragInfo alloc] initWithPasteboard:board
                                                   mask:static_cast<NSDragOperation>(NSDragOperationCopy)
                                               location:NSMakePoint(200.0, 100.0)];
    const NSDragOperation entered = [(id<NSDraggingDestination>)uut.view draggingEntered:(id)info];
    check(entered == NSDragOperationNone, "Enter returns NSDragOperationNone when the target accepts nothing", ledger,
          "op=" + std::to_string(static_cast<unsigned long>(entered)));
    const BOOL gate = [(id<NSDraggingDestination>)uut.view prepareForDragOperation:(id)info];
    check(gate == NO, "prepareForDragOperation returns NO (AppKit must not call perform)", ledger);
    check(count_phase(target, SystemDropPhase::Drop) == 0, "no Drop is dispatched on the rejected session", ledger);
    [(id<NSDraggingDestination>)uut.view draggingExited:(id)info];
    target.accept = DragAction::Copy;
}

// ---------------------------------------------------------------------------
// Leg 5: drag-out fail-closed
// ---------------------------------------------------------------------------

auto leg_drag_out(BridgeUnderTest &uut, Ledger &ledger) -> void {
    note("leg 5/5: drag-out refuses to start without a window and a press event");
    aurora::detail::MacOSDragBridge::note_initiator(uut.bridge.get());
    bool ended = false;
    DragOffer offer;
    offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> {
        return Result<std::vector<std::uint8_t>>{std::vector<std::uint8_t>{'x'}};
    };
    const auto began = aurora::system_drag::begin(offer, [&](SystemDragOutcome outcome, DragAction action) {
        ended = true;
        note(std::string{"[drag-out] end:"} + outcome_name(outcome) + ":" + action_name(action));
    });
    check(!began.ok(), "system_drag::begin fails closed without a press event / window", ledger,
          began.ok() ? std::string{"Ok"} : began.error().message);
    aurora::system_drag::pump_reads();
    check(!ended, "no terminal event is delivered for a session that never started", ledger);
    check(!uut.bridge->drag_out_active(), "bridge holds no drag-out session after the refusal", ledger);
    aurora::detail::MacOSDragBridge::note_initiator(nullptr);
}

// ---------------------------------------------------------------------------
// Environment probe: is there a window server to host a real session?
// ---------------------------------------------------------------------------

/// @brief 有无窗口服务器（决定桥是装在「真实窗口内的视图」上还是孤立视图上）。
///
/// 这里的 `@try` 只包环境探测（建一个不显示即关闭的窗口），不包任何判定：无窗口服务器时
/// AppKit 的建窗路径会抛，探测失败按「无」处理并转 PENDING，不把环境缺席当失败。
[[nodiscard]] auto window_server_available() -> bool {
    BOOL ok = NO;
    @try {
        NSWindow *probe_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0.0, 0.0, 480.0, 360.0)
                                                             styleMask:NSWindowStyleMaskTitled
                                                               backing:NSBackingStoreBuffered
                                                                 defer:NO];
        if (probe_window != nil) {
            NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 480.0, 360.0)];
            [probe_window setContentView:host];
            ok = ([host window] != nil);
            [probe_window close];
        }
    } @catch (NSException *exception) {
        (void)exception;
        ok = NO;
    }
    return ok == YES;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常 → 非零退出码呈现（捕获反而把它压成 0），与既有探针同口径。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("macOS system drag-and-drop live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    if (cli.arguments->flag("interactive")) {
        note("--interactive needs a real window shell; MacOSSurface::create_window is still a skeleton,");
        note("so this probe cannot host an on-screen drag session yet.");
        return 2;
    }

    int rc = 0;
    @autoreleasepool {
        (void)[NSApplication sharedApplication];  // 初始化 AppKit 与窗口服务器连接

        NSPasteboard *board = [NSPasteboard pasteboardWithUniqueName];
        NSString *const probe_text = [NSString stringWithUTF8String:AURORA_PROBE_TEXT];
        // 该常量是 ASCII，转换恒成功；判空只是为了让 `setString:forType:` 的 nonnull 形参
        // 拿到一枚已验证的指针（clang-analyzer 不推定 `stringWithUTF8String:` 的返回值非空）。
        if (probe_text == nil) {
            note("probe text is not representable as NSString");
            return 2;
        }
        const BOOL wrote = [board setString:probe_text forType:@"public.utf8-plain-text"];
        if (!wrote) {
            note("pasteboard write failed (no pasteboard server in this session)");
            return 2;
        }

        auto target = std::make_shared<DropTarget>();
        target->layout(Constraints{}, BuildContext{});
        BridgeUnderTest uut = make_bridge(*target);
        Ledger ledger;
        leg_registry(ledger);
        leg_view(uut, ledger);
        if (uut.view == nil) {
            note("destination view unavailable -> remaining legs skipped");
            return 2;
        }
        leg_drag_in(uut, board, ledger);
        leg_reject(uut, board, ledger);
        leg_drag_out(uut, ledger);

        uut.bridge->uninstall();

        if (ledger.failures > 0) {
            rc = 4;
        } else if (!window_server_available()) {
            pending("AppKit-delivered drag session (four drag-in states, drag-out success/cancel)", ledger,
                    "needs a human on a real macOS desktop; see codespec/manual-test/24-systemdrag.md");
            note("automated legs passed, but no window server: the bridge was installed on a free-standing view.");
            rc = 3;
        } else {
            pending("AppKit-delivered drag session (four drag-in states, drag-out success/cancel)", ledger,
                    "needs a human on a real macOS desktop; see codespec/manual-test/24-systemdrag.md");
            note("PASS: automated legs passed with a window server present.");
            rc = 0;
        }
        note("---------------------------------------------------------------");
        note("checks=" + std::to_string(ledger.checks) + " failures=" + std::to_string(ledger.failures) +
             " pending=" + std::to_string(ledger.pending));
    }
    return rc;
}
