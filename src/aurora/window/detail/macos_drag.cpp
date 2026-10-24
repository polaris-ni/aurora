// macOS 系统拖放桥实现（契约与门控见同名头文件）。
//
// AppKit 拖放的会话形态（协议对象 + 委托方法，字段布局与规范同源，不在此重述规范文本）：
//   * 拖入（target）：内容视图登记 `registerForDraggedTypes:` 后实现 `NSDraggingDestination`
//     五方法。Enter / Updated 回 `NSDragOperation` 位（None = 不接受）；**accept 闸门是
//     `prepareForDragOperation` 的返回值**（AppKit 只在 YES 后调 `performDragOperation`）；
//     取数同步发生在 `performDragOperation` 内（读 `draggingPasteboard`，AppKit 读取 API
//     自身阻塞到数据就绪）。
//   * 拖出（source）：`beginDraggingSessionWithItems:event:source:` 起手（event 必须是宿主
//     暂存的按下 `NSEvent`），数据经 `NSPasteboardItem` 的 data provider 惰性送达
//     （`pasteboard:item:provideDataForType:`，AppKit 在目标索要数据时调用）；终态来自
//     `draggingSession:endedAtPoint:operation:`（operation 为 none ⇒ 用户取消）。
//
// 内存纪律：**MRC**（无 `-fobjc-arc`，与 `macos_surface.cpp` 同口径）。ObjC 会话对象
// （按下事件 / 粘贴板条目 / 拖拽条目 / 源对象）在会话创建时显式 retain、会话收摊时释放；
// 收摊若发生在 AppKit 委托回调内（`endedAtPoint:operation:` 由源对象转发而来），改走
// autorelease——会话对象的最终释放必须晚于 AppKit 本次事件派发收尾，直接 release 有
// 「框架仍在用即已析构」的风险（AppKit 对源对象的持有期文档未言明，取保守侧）。

#include "aurora/window/detail/macos_drag.h"

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

// aurora 头先于 AppKit 头（同款纪律：平台头关在单个 TU 内，包含顺序即语义）。
// clang-format off
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/event/detail/system_drag_backend.h"
#include "aurora/event/detail/system_drag_ingress.h"
#include "aurora/window/detail/system_drag_translate.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace aurora::detail {
namespace {

/// @brief 进程级发起槽（多窗口下由宿主在事件泵入口刷新）。
auto initiator_slot() -> MacOSDragBridge *& {
    static MacOSDragBridge *slot = nullptr;
    return slot;
}

/// @brief 拖出后端是否已注册（进程级一次性；事件层另按「先到者优先」去重）。
auto backend_registered() -> bool & {
    static bool registered = false;
    return registered;
}

/// @brief 把动作约束进「本要约可声明」的集合：`Move` 需源侧能确认删除，否则退 `Copy`。
///
/// 与 X11 / Wayland 桥的同名函数同口径：没有 `on_move_confirmed` 就向 OS 声明可移动，
/// 等于承诺「落放后删原数据」却没有删的路，数据丢失风险由调用方承担。
[[nodiscard]] auto constrain_action(const DragOffer &offer, DragAction want) -> DragAction {
    if (want == DragAction::Move && offer.on_move_confirmed == nullptr) {
        return DragAction::Copy;
    }
    return want;
}

/// @brief NSString → UTF-8 字节（桥的文本载荷入口形态；nil 与空串都得空字节）。
[[nodiscard]] auto utf8_bytes(NSString *text) -> std::vector<std::uint8_t> {
    if (text == nil) {
        return {};
    }
    const char *raw = [text UTF8String];
    if (raw == nullptr) {
        return {};
    }
    const auto *base = reinterpret_cast<const std::uint8_t *>(raw);  // NOLINT(*-pro-type-reinterpret-cast)
    const std::size_t n = std::strlen(raw);
    return std::vector<std::uint8_t>(base, base + n);
}

/// @brief std::vector<std::string> → NSArray<NSString *>（登记 / 广告 / 读取共用）。
[[nodiscard]] auto ns_string_array(const std::vector<std::string> &values) -> NSArray<NSString *> * {
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:values.size()];
    for (const std::string &value : values) {
        [out addObject:[NSString stringWithUTF8String:value.c_str()]];
    }
    return out;
}

/// @brief NSString → std::string（nil 得空串；UTI / 类型名的通道形态）。
[[nodiscard]] auto std_string(NSString *text) -> std::string {
    if (text == nil) {
        return {};
    }
    const char *raw = [text UTF8String];
    return raw == nullptr ? std::string{} : std::string{raw};
}

}  // namespace

// ---- 会话状态机（会话态与时序集中于此；ObjC 委托对象只做「还原 Impl* 后转发」）----
// 必须定义在 `aurora::detail` 而非匿名 namespace：ObjC 委托对象（目标视图类 / 拖拽源对象）
// 的方法体要命名本类型，塞进匿名 namespace 即「declaration ... in a namespace which does
// not enclose the class」（与 `WaylandDragBridge::Impl` 同款处置）。
struct MacOSDragBridge::Impl {
    void *view = nullptr;  ///< 内容视图（`NSView *` 的 void 形态；由公共面构造时落入）
    Hooks hooks{};
    MacOSDragBridge *owner = nullptr;
    bool installed = false;

    /// @brief 拖入侧会话态（本窗口是 target）。
    struct DragIn {
        bool active = false;  ///< 会话进行中（enter 已到、exit/drop 未收摊）
        bool entered = false;  ///< 已派发过 `Enter`
        bool dropped = false;  ///< perform 已到（同步取数中；防重入）
        std::vector<std::string> types;  ///< 源广告的**已登记**粘贴板类型（读取时按它取首个命中）
        std::vector<std::string> mimes;  ///< 折算后的规范 MIME 清单（顺序即优先级）
        double x = 0.0;  ///< 视图坐标（逻辑 pt；`isFlipped` 视图 y 向下，与客户区同向）
        double y = 0.0;
        DragAction os_action = DragAction::None;  ///< 源声明的 operation 掩码折算值（OS 建议）
        DragAction allowed = DragAction::None;  ///< 派发器写定的允许动作（accept 闸门读它）
    };

    /// @brief 拖出侧会话态（本窗口是 source）。
    struct DragOut {
        bool active = false;  ///< 会话进行中
        bool cancel_requested = false;  ///< `system_drag::cancel()` 的本地记录（AppKit 无程序化中止）
        bool provide_failed = false;  ///< 数据提供失败（广告了却给不出 ⇒ 会话按失败收口）
        DragOffer offer{};  ///< 控件要约（会话期内只读；整份拷入）
        std::vector<std::string> mimes;  ///< 已广告的规范 MIME（要约格式 ∩ 登记表）
        std::uint32_t mask = 0;  ///< 已向 AppKit 声明的 operation 掩码（源侧能力）
        void *press_event = nullptr;  ///< 暂存的按下 `NSEvent *`（owned；发起即协议必需实参）
        void *source = nullptr;  ///< `AuroraMacDragSource *`（owned；会话期存活）
        void *item = nullptr;  ///< `NSPasteboardItem *`（owned；data provider 载体）
        void *drag_item = nullptr;  ///< `NSDraggingItem *`（owned；帧矩形载体）
    };

    DragIn in;
    DragOut out;
    bool handled = false;  ///< 派发器写定的 `is_handled`（Drop 态的受理信号）

    // ---- 安装 / 卸载 ----
    auto install() -> bool;
    auto uninstall() -> void;

    // ---- 拖入 ----
    [[nodiscard]] auto on_dragging_entered(void *sender) -> std::uint32_t;
    [[nodiscard]] auto on_dragging_updated(void *sender) -> std::uint32_t;
    auto on_dragging_exited() -> void;
    [[nodiscard]] auto on_prepare_for_drag_operation(void *sender) -> bool;
    auto on_perform_drag_operation(void *sender) -> void;
    [[nodiscard]] auto dispatch_drag_in(SystemDropPhase phase, std::shared_ptr<const DragPayload> payload)
        -> DragAction;
    auto reset_drag_in() -> void;

    // ---- 拖出 ----
    auto begin_drag_out(const DragOffer &offer) -> bool;
    auto cancel_drag_out() -> void;
    auto teardown_drag_out(SystemDragOutcome outcome, DragAction action, bool from_session_callback) -> void;
    auto on_session_ended(std::uint32_t operation) -> void;
    auto provide_for_type(NSString *type, NSPasteboardItem *item) -> void;

    // ---- 帧循环看守 ----
    auto tick() -> void;
};

}  // namespace aurora::detail

// ---- ObjC 委托对象（协议实现集中在两个 @interface；方法体只做「还原 Impl* 后转发」）----
// ObjC 类**必须**定义在全局作用域（「Objective-C declarations may only appear in global
// scope」——放进 C++ 命名空间即编译错误，已实测），故此处先合上 `aurora::detail`；
// 方法体命名 `aurora::detail::MacOSDragBridge::Impl` 不受影响（该结构体已定义于上）。
// 两个类均**不持有** Impl（裸指针，桥的生命周期必须长于它们，见头文件契约）。

// 拖入目标视图：实现 `NSDraggingDestination` 五方法（全部转发到桥的 C++ 落点）、
// `isFlipped = YES`（视图坐标 y 向下，与 X11 / Win32 客户区同向）与 `acceptsFirstResponder`。
// 注记为 `//` 而非 `///`：门禁的文档注释扫描不认 ObjC 的 `@interface` 为可文档声明，
// 挂 `///` 会落 DOC-R3（「/// 块未紧邻可文档声明」）。
@interface AuroraSystemDragView : NSView {
    aurora::detail::MacOSDragBridge::Impl *_impl;
}
- (instancetype)initWithBridge:(aurora::detail::MacOSDragBridge::Impl *)impl;
@end

// 拖出源对象：`NSDraggingSource` 三方法（能力掩码 / 修饰键让渡 / 终态）+
// `NSPasteboardItemDataProvider` 一方法（目标索要数据时经控件要约应答）。同上用 `//`。
@interface AuroraMacDragSource : NSObject <NSDraggingSource, NSPasteboardItemDataProvider> {
    aurora::detail::MacOSDragBridge::Impl *_impl;
}
- (instancetype)initWithBridge:(aurora::detail::MacOSDragBridge::Impl *)impl;
@end

@implementation AuroraSystemDragView

- (instancetype)initWithBridge:(aurora::detail::MacOSDragBridge::Impl *)impl {
    self = [super initWithFrame:NSZeroRect];
    if (self != nil) {
        _impl = impl;
    }
    return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return static_cast<NSDragOperation>(_impl->on_dragging_entered(static_cast<void *>(sender)));
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return static_cast<NSDragOperation>(_impl->on_dragging_updated(static_cast<void *>(sender)));
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    _impl->on_dragging_exited();
}

// accept 闸门（头文件差异①）：AppKit 只在 YES 后调 `performDragOperation`。闸门读
// Enter / Updated 派发后写定的 `allowed`——此刻再派发一次会让控件收到语义重复的 Over。
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    return _impl->on_prepare_for_drag_operation(static_cast<void *>(sender)) ? YES : NO;
}

// 取数同步发生在桥内（头文件差异②）。返回值按 AppKit 惯例恒 YES：受理信号由 Drop 派发的
// `is_handled` 承载（源侧没有「目标回传执行动作」的协议事件，同 Wayland）。
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    _impl->on_perform_drag_operation(static_cast<void *>(sender));
    return YES;
}

// 周期性拖放更新：Enter / Updated 之外补 `draggingUpdated` 之外的节律由 AppKit 自管；
// 返回 YES 走 AppKit 默认周期（本桥无 tick 级读取泵，周期性回调不驱动任何状态机）。
- (BOOL)wantsPeriodicDraggingUpdates {
    return YES;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

@end

@implementation AuroraMacDragSource

- (instancetype)initWithBridge:(aurora::detail::MacOSDragBridge::Impl *)impl {
    self = [super init];
    if (self != nil) {
        _impl = impl;
    }
    return self;
}

// 只声明本会话能兑现的动作位（`begin_drag_out` 已按要约约束；无 Move 确认即不含 Move 位）。
- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    return static_cast<NSDragOperation>(_impl->out.mask);
}

// 修饰键让渡（头文件修饰态注记）：AppKit 不自行按修饰键改写 operation——修饰键 → 动作的
// 翻译全库只有 `system_drag::proposed_action_for` 一处，跨平台一致口径。
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession *)session {
    (void)session;
    return YES;
}

// 终态（头文件差异③）：operation 为 none ⇒ 取消；否则按位序折算（Move 的确认与回滚在
// 事件层 `deliver_session_end` 内部完成，桥只登记终态，同 Wayland 的 `dnd_finished`）。
- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    (void)session;
    (void)screenPoint;
    _impl->on_session_ended(static_cast<std::uint32_t>(operation));
}

// NSPasteboardItemDataProvider：AppKit 在目标索要某类型数据时调用（主线程；会话期单线程
// UI 纪律成立）。登记过的类型逐个到达，控件要约 `provide` 在此应答；失败登记
// `provide_failed`，会话终态按失败收口（不把失败伪装成格式不存在）。
- (void)pasteboard:(NSPasteboard *)pasteboard item:(NSPasteboardItem *)item provideDataForType:(NSPasteboardType)type {
    (void)pasteboard;
    _impl->provide_for_type(type, item);
}

@end

namespace aurora::detail {

// ---- 安装 / 卸载 ----

auto MacOSDragBridge::Impl::install() -> bool {
    if (installed || view == nullptr) {
        return false;  // 视图缺失（骨架期宿主未建视图）：如实失败，由宿主记日志降级
    }
    const std::vector<std::string> types = drag::macos_registered_pasteboard_types();
    if (types.empty()) {
        return false;  // 登记表为空即「无可消费格式」：不登记（不谎报已有通路）
    }
    [static_cast<NSView *>(view) registerForDraggedTypes:ns_string_array(types)];
    installed = true;
    MacOSDragBridge::register_backend();
    return true;
}

auto MacOSDragBridge::Impl::uninstall() -> void {
    installed = false;
    if (out.active) {
        // 在途拖出按 Cancelled 兑现（与另两桥同口径）。本地终态由事件层「首登记者赢」规则
        // 兜底：AppKit 会话自然结束时补登的终态不会改写调用方已看到的 Cancelled。
        teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None, false);
    }
    reset_drag_in();
    if (view != nullptr) {
        NSView *ns_view = static_cast<NSView *>(view);
        // `unregisterDraggedTypes` 的可用性按运行期探测（保守侧：老系统没有该 selector 时
        // 不注销——视图随窗口销毁，登记自然失效，不谎报已注销）。
        if ([ns_view respondsToSelector:@selector(unregisterDraggedTypes)]) {
            [ns_view unregisterDraggedTypes];
        }
    }
    if (initiator_slot() == owner) {
        initiator_slot() = nullptr;
    }
}

// ---- 拖入 ----

/// @brief Enter / Updated 共用的会话刷新与派发（两者只差阶段）。
[[nodiscard]] auto MacOSDragBridge::Impl::on_dragging_entered(void *sender) -> std::uint32_t {
    auto *info = static_cast<id<NSDraggingInfo>>(sender);  // NOLINT(*-pro-type-static-cast-down)
    if (info == nil) {
        return 0;
    }
    // 异常路径（exited 未达就来了新 enter）：先收摊上一个会话（同一指针不可能同时在两场拖放里）。
    reset_drag_in();
    // 源广告的粘贴板类型 → 规范 MIME（顺序即优先级；未登记类型丢弃，不猜、不兜底）。
    NSArray<NSPasteboardType> *types = [[info draggingPasteboard] types];
    std::vector<std::string> names;
    names.reserve(types.count);
    for (NSPasteboardType type in types) {
        names.push_back(std_string(type));
    }
    // 读取时要按「源真的广告过」取类型（同 Wayland `start_next_read` 只请求广告过的线上名）：
    // `mimes_from_*` 会丢顺序与重复，故自存一份**已登记且出现过**的类型全集。
    in.types.clear();
    for (const std::string &name : names) {
        if (drag::mime_from_macos_pasteboard_type(name).has_value()) {
            in.types.push_back(name);
        }
    }
    in.mimes = drag::mimes_from_macos_pasteboard_types(names);
    if (in.mimes.empty()) {
        // 无一个本库登记的格式：不建立会话、直接拒绝（回了非 None 的 operation 也无从取数）。
        reset_drag_in();
        return 0;
    }
    const NSPoint at = [info draggingLocation];
    in.x = static_cast<double>(at.x);
    in.y = static_cast<double>(at.y);
    // OS 建议动作：源声明的 operation 掩码（拖入侧 `default_action` 的唯一真值源）。
    in.os_action =
        drag::action_from_macos_drag_operation(static_cast<std::uint32_t>([info draggingSourceOperationMask]));
    in.active = true;
    in.entered = false;
    in.allowed = DragAction::None;
    handled = false;
    in.allowed = dispatch_drag_in(SystemDropPhase::Enter, nullptr);
    in.entered = true;
    return static_cast<std::uint32_t>(drag::macos_drag_operation_from(in.allowed));
}

[[nodiscard]] auto MacOSDragBridge::Impl::on_dragging_updated(void *sender) -> std::uint32_t {
    auto *info = static_cast<id<NSDraggingInfo>>(sender);  // NOLINT(*-pro-type-static-cast-down)
    if (!in.active || info == nil) {
        return 0;
    }
    const NSPoint at = [info draggingLocation];
    in.x = static_cast<double>(at.x);
    in.y = static_cast<double>(at.y);
    in.os_action =
        drag::action_from_macos_drag_operation(static_cast<std::uint32_t>([info draggingSourceOperationMask]));
    // 每个悬停采样都派发一次 Over（与 X11 / Wayland 同口径：契约即「按指针采样频率到达」，
    // 轻重由控件负责，桥不替它节流）。
    in.allowed = dispatch_drag_in(SystemDropPhase::Over, nullptr);
    return static_cast<std::uint32_t>(drag::macos_drag_operation_from(in.allowed));
}

auto MacOSDragBridge::Impl::on_dragging_exited() -> void {
    if (!in.entered) {
        reset_drag_in();  // 从未派发过 Enter：无需补 Leave（控件不知道有过拖入）
        return;
    }
    // 先派发再清态：`dispatch_drag_in` 要读会话态（mimes / 位置 / OS 建议）。
    // `Leave` 的载荷契约允许为空——不放置即结束，没有数据可交。
    const std::shared_ptr<const DragPayload> no_payload;
    (void)dispatch_drag_in(SystemDropPhase::Leave, no_payload);
    reset_drag_in();
}

[[nodiscard]] auto MacOSDragBridge::Impl::on_prepare_for_drag_operation(void *sender) -> bool {
    (void)sender;  // 闸门读会话态，不用 sender 内容
    if (!in.active) {
        return false;
    }
    // accept 闸门（头文件差异①）：Enter / Updated 派发后写定的 `allowed` 非 None 即接受。
    return in.allowed != DragAction::None;
}

auto MacOSDragBridge::Impl::on_perform_drag_operation(void *sender) -> void {
    auto *info = static_cast<id<NSDraggingInfo>>(sender);  // NOLINT(*-pro-type-static-cast-down)
    if (!in.active || info == nil) {
        return;
    }
    in.dropped = true;
    NSPasteboard *pb = [info draggingPasteboard];
    std::unordered_map<std::string, std::vector<std::uint8_t>> entries;
    for (const std::string &mime : in.mimes) {
        // 只读源**真的广告过**的类型（按登记序取首个命中；`in.types` 即 Enter 时记录的全集）。
        const std::string *chosen = nullptr;
        for (const std::string &type : drag::macos_pasteboard_types_from_mime(mime)) {
            if (std::find(in.types.begin(), in.types.end(), type) != in.types.end()) {
                chosen = &type;
                break;
            }
        }
        if (chosen == nullptr) {
            continue;  // 防御性：mime 由 types 折算而来，理论上不可达
        }
        NSString *ns_type = [NSString stringWithUTF8String:chosen->c_str()];
        if (mime == AURORA_MIME_TEXT_PLAIN) {
            // 文本：`stringForType:` 同步返回（nil = 源未给该类型，按空口径收下）。
            entries.emplace(mime, utf8_bytes([pb stringForType:ns_type]));
        } else if (mime == AURORA_MIME_URI_LIST) {
            // 文件清单：`public.file-url` → NSURL 数组 → 本地路径 → `uri_list::encode`
            // （载荷形态与另三平台逐字节一致：CRLF 结尾的 file:// 行）。
            NSArray<NSURL *> *urls =
                [pb readObjectsForClasses:@[ [NSURL class] ] options:@{
                    NSPasteboardURLReadingFileURLsOnlyKey : @YES
                }];
            std::vector<std::string> paths;
            for (NSURL *url in urls) {
                const std::string path = std_string([url path]);
                if (!path.empty()) {
                    paths.push_back(path);
                }
            }
            const std::string encoded = uri_list::encode(paths);
            entries.emplace(mime, std::vector<std::uint8_t>(encoded.begin(), encoded.end()));
        } else if (mime == AURORA_MIME_IMAGE_PNG) {
            // 位图：`dataForType:` 同步返回原始 PNG 字节（nil = 未给，按空口径收下）。
            NSData *data = [pb dataForType:ns_type];
            if (data != nil) {
                const auto *base = static_cast<const std::uint8_t *>([data bytes]);
                entries.emplace(mime, std::vector<std::uint8_t>(base, base + [data length]));
            } else {
                entries.emplace(mime, std::vector<std::uint8_t>{});
            }
        }
    }
    // 同步取数完成 ⇒ 派发 `Drop`（唯一保证带载荷的态）并收摊会话。
    auto payload = DragPayload::from_bytes(std::move(entries));
    (void)dispatch_drag_in(SystemDropPhase::Drop, std::move(payload));
    reset_drag_in();
}

[[nodiscard]] auto MacOSDragBridge::Impl::dispatch_drag_in(SystemDropPhase phase,
                                                           std::shared_ptr<const DragPayload> payload) -> DragAction {
    const float scale = hooks.scale_factor != nullptr ? hooks.scale_factor() : 1.0F;
    drag::DragInFrame frame;
    frame.mimes = in.mimes;
    frame.modifiers = hooks.modifiers != nullptr ? hooks.modifiers() : ModifierKey::None;
    frame.os_suggested = in.os_action;
    // 视图坐标（逻辑 pt）→ 客户区物理像素：与 Wayland 桥同口径（那边表面坐标同为逻辑单位，
    // 先乘 scale 把量纲归一，再交 `logical_from_client_px` 这唯一折算点）。
    frame.client_x = static_cast<int>(in.x * static_cast<double>(scale));
    frame.client_y = static_cast<int>(in.y * static_cast<double>(scale));
    frame.scale = scale;
    SystemDropEvent event = drag::make_drop_event(phase, frame);
    event.payload = std::move(payload);
    handled = false;
    return system_drag::ingress::dispatch(event, [this](SystemDropEvent &e) -> DragAction {
        if (hooks.emit != nullptr) {
            hooks.emit(e);  // 派发器在调用内写定 `e.allowed`（Enter / Over 两态）
        }
        handled = e.is_handled;  // Drop 态的受理信号：派发器不写 allowed，消费与否即受理与否
        return e.allowed;
    });
}

auto MacOSDragBridge::Impl::reset_drag_in() -> void { in = DragIn{}; }

// ---- 拖出 ----

auto MacOSDragBridge::Impl::begin_drag_out(const DragOffer &offer) -> bool {
    if (out.active || view == nullptr) {
        return false;
    }
    NSView *ns_view = static_cast<NSView *>(view);
    if ([ns_view window] == nil) {
        // AppKit 要求视图在窗口内才能起会话；骨架期宿主未建窗口 ⇒ 如实失败、不谎报。
        AURORA_LOG_WARN("system-drag", "macOS: content view has no window; drag not started.");
        return false;
    }
    // `beginDraggingSessionWithItems:event:source:` 的 event 是协议必需实参（AppKit 从它取
    // 按下位置与事件时序）；没有暂存的按下事件（程序化合成发起）宁可不发起，也不编一个。
    if (out.press_event == nullptr) {
        AURORA_LOG_WARN("system-drag", "macOS: no press event for beginDraggingSession; drag not started.");
        return false;
    }
    // 要约格式 → 可广告的 MIME 与粘贴板类型（顺序即优先级；无 provide 回调的格式不广告）。
    std::vector<std::string> mimes;
    std::vector<std::string> paste_types;
    for (const std::string &mime : offer.formats) {
        const std::vector<std::string> types = drag::macos_pasteboard_types_from_mime(mime);
        if (!types.empty() && offer.provide != nullptr) {
            mimes.push_back(mime);
            for (const std::string &type : types) {
                paste_types.push_back(type);  // 该 MIME 的全部已登记类型（同 Wayland 广告全部线上名）
            }
        }
    }
    if (mimes.empty()) {
        return false;  // 无一格式可广告：如实不发起（不谎报会话已建立）
    }
    // 只声明本会话能兑现的动作位：无 Move 确认回调 ⇒ 不向 AppKit 声明可移动
    // （与另三桥 `constrain_action` 同口径；`on_session_ended` 再兜一层约束）。
    out.mask = drag::macos_supported_drag_operations();
    if (offer.on_move_confirmed == nullptr) {
        out.mask &= ~static_cast<std::uint32_t>(drag::MacOSDragOperation::Move);
    }
    AuroraMacDragSource *source = [[AuroraMacDragSource alloc] initWithBridge:this];
    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setDataProvider:source forTypes:ns_string_array(paste_types)];
    NSDraggingItem *drag_item = [[NSDraggingItem alloc] initWithPasteboardWriter:item];
    // 影像帧：按下点周边 16pt 见方（AppKit 要求非空帧；contents nil = 用粘贴板项的默认影像，
    // `DragOffer::icon` 是装饰性提示，不支持影像的后端静默忽略、不改变会话语义）。
    const NSPoint at = [static_cast<NSEvent *>(out.press_event) locationInWindow];
    const NSPoint in_view = [ns_view convertPoint:at fromView:nil];
    [drag_item setDraggingFrame:NSMakeRect(in_view.x - 8.0, in_view.y - 8.0, 16.0, 16.0) contents:nil];
    NSDraggingSession *session = [ns_view beginDraggingSessionWithItems:@[ drag_item ]
                                                                  event:static_cast<NSEvent *>(out.press_event)
                                                                 source:source];
    // MRC：会话对象由本桥持有（AppKit 的持有期文档未言明，取保守侧），收摊时释放。
    out.source = source;
    out.item = item;
    out.drag_item = drag_item;
    if (session == nil) {
        AURORA_LOG_WARN("system-drag", "macOS: beginDraggingSessionWithItems returned nil; drag not started.");
        teardown_drag_out(SystemDragOutcome::Failed, DragAction::None, false);
        return false;
    }
    out.active = true;
    out.cancel_requested = false;
    out.provide_failed = false;
    // 要约整份拷入：`provide` 失败会走会话收口，不得在回调后读已被清掉的成员
    // （与 X11 桥 `on_selection_request` 同款纪律）。
    out.offer = offer;
    out.mimes = std::move(mimes);
    return true;
}

auto MacOSDragBridge::Impl::cancel_drag_out() -> void {
    if (!out.active) {
        return;
    }
    // AppKit 没有公开的「程序化中止拖拽会话」API（与 X11 的指针 grab 解除 / Wayland 的
    // source 销毁不同）：本地终态已由 `system_drag::cancel()` 就地登记（首登记者赢），
    // 此处如实记录取消意图并保留会话态——OS 会话自然结束时 `on_session_ended` 的补登撞上
    // 已排定终态，由 `complete_session` 的首登记者规则自然作废。此刻强收会让 AppKit 对
    // 仍在用的会话对象回调，不干这事。
    out.cancel_requested = true;
    AURORA_LOG_INFO("system-drag",
                    "macOS: cancel requested; AppKit exposes no programmatic drag abort, honoring it on session end.");
}

auto MacOSDragBridge::Impl::teardown_drag_out(SystemDragOutcome outcome, DragAction action, bool from_session_callback)
    -> void {
    if (!out.active) {
        return;
    }
    // 会话对象成对释放（MRC）。收摊发生在 AppKit 委托回调内时改走 autorelease：最终释放
    // 必须晚于 AppKit 本次事件派发收尾（理由见文件头）。两条路径不能并进三元表达式——
    // `autorelease` 返回 `instancetype` 而 `release` 返回 `void`，混用即类型不匹配（已实测）。
    if (out.press_event != nullptr) {
        NSEvent *event = static_cast<NSEvent *>(out.press_event);
        out.press_event = nullptr;
        if (from_session_callback) {
            [event autorelease];
        } else {
            [event release];
        }
    }
    if (out.drag_item != nullptr) {
        NSDraggingItem *drag_item = static_cast<NSDraggingItem *>(out.drag_item);
        out.drag_item = nullptr;
        if (from_session_callback) {
            [drag_item autorelease];
        } else {
            [drag_item release];
        }
    }
    if (out.item != nullptr) {
        NSPasteboardItem *paste_item = static_cast<NSPasteboardItem *>(out.item);
        out.item = nullptr;
        if (from_session_callback) {
            [paste_item autorelease];
        } else {
            [paste_item release];
        }
    }
    if (out.source != nullptr) {
        AuroraMacDragSource *source = static_cast<AuroraMacDragSource *>(out.source);
        out.source = nullptr;
        if (from_session_callback) {
            [source autorelease];
        } else {
            [source release];
        }
    }
    out.active = false;
    out.mimes.clear();
    out.mask = 0;
    out.provide_failed = false;
    out.cancel_requested = false;
    out.offer = DragOffer{};
    // 终态登记的**唯一**通道（事件层在帧循环排空内投递 `on_end`，含 Move 确认次序）。
    complete_drag_out_session(outcome, action);
}

auto MacOSDragBridge::Impl::on_session_ended(std::uint32_t operation) -> void {
    if (!out.active) {
        return;
    }
    if (out.provide_failed) {
        // 广告了却给不出：与另三桥同罪（会话按失败结束），不把失败伪装成格式不存在。
        AURORA_LOG_WARN("system-drag", "macOS: provide failed during drag; ending session as failed.");
        teardown_drag_out(SystemDragOutcome::Failed, DragAction::None, true);
        return;
    }
    if (operation == 0U) {  // `NSDragOperationNone`：用户取消（Esc / 松手回到源窗）
        teardown_drag_out(SystemDragOutcome::Cancelled, DragAction::None, true);
        return;
    }
    // 否则按 `NSDragOperation` 位序折算（Copy → Move → Link 优先序）。Move 的确认与回滚在
    // 事件层 `deliver_session_end` 内部完成——桥不自调 `on_move_confirmed`（同 Wayland）。
    const DragAction action = constrain_action(out.offer, drag::action_from_macos_drag_operation(operation));
    teardown_drag_out(SystemDragOutcome::Succeeded, action, true);
}

auto MacOSDragBridge::Impl::provide_for_type(NSString *type, NSPasteboardItem *item) -> void {
    if (!out.active || type == nil) {
        return;
    }
    const auto mime = drag::mime_from_macos_pasteboard_type(std_string(type));
    if (!mime.has_value()) {
        return;  // 未登记类型：不可能广告过，如实不给数据（不猜）
    }
    if (std::find(out.mimes.begin(), out.mimes.end(), *mime) == out.mimes.end()) {
        return;  // 未广告的 MIME：如实拒绝
    }
    if (out.offer.provide == nullptr) {
        out.provide_failed = true;
        return;
    }
    // 要约先拷一份：`provide` 失败会走会话收口（清 `out.offer`），不得在回调后读已清成员
    // （与 X11 桥 `on_selection_request` 同款纪律）。
    const DragOffer offer = out.offer;
    const Result<std::vector<std::uint8_t>> bytes = offer.provide(*mime);
    if (!bytes) {
        out.provide_failed = true;
        AURORA_LOG_WARN("system-drag", "macOS: provide failed for mime; session will end as failed.");
        return;
    }
    const std::vector<std::uint8_t> &data = bytes.value();
    NSData *ns_data = [NSData dataWithBytes:data.data() length:data.size()];
    [item setData:ns_data forType:type];
}

// ---- 帧循环看守 ----

auto MacOSDragBridge::Impl::tick() -> void {
    // 无在途读取可泵（取数同步发生在 perform 内）；保留只为表层帧循环调用点与另两桥同形。
}

}  // namespace aurora::detail

// ---- 桥的公共面（全部转发给状态机） ----

namespace aurora::detail {

MacOSDragBridge::MacOSDragBridge(void *view, Hooks hooks)
    : impl_(std::make_unique<Impl>()), view_(view), hooks_(std::move(hooks)) {
    impl_->view = view;
    impl_->hooks = hooks_;
    impl_->owner = this;
}

MacOSDragBridge::~MacOSDragBridge() { uninstall(); }

auto MacOSDragBridge::attach_view(void *view) -> void {
    view_ = view;
    impl_->view = view;
}

auto MacOSDragBridge::install() -> bool { return impl_->install(); }

auto MacOSDragBridge::uninstall() -> void { impl_->uninstall(); }

auto MacOSDragBridge::on_dragging_entered(void *sender) -> std::uint32_t { return impl_->on_dragging_entered(sender); }

auto MacOSDragBridge::on_dragging_updated(void *sender) -> std::uint32_t { return impl_->on_dragging_updated(sender); }

auto MacOSDragBridge::on_dragging_exited() -> void { impl_->on_dragging_exited(); }

auto MacOSDragBridge::on_prepare_for_drag_operation(void *sender) -> bool {
    return impl_->on_prepare_for_drag_operation(sender);
}

auto MacOSDragBridge::on_perform_drag_operation(void *sender) -> void { impl_->on_perform_drag_operation(sender); }

auto MacOSDragBridge::note_press_event(void *event) -> void {
    auto *ns_event = static_cast<NSEvent *>(event);
    if (ns_event == nil || impl_->out.active) {
        return;  // 会话期内不换暂存事件（发起用的正是当前这份；鼠标派发亦被表层屏蔽）
    }
    if (impl_->out.press_event != nullptr) {
        [static_cast<NSEvent *>(impl_->out.press_event) release];
    }
    impl_->out.press_event = ns_event;
    [ns_event retain];  // 暂存期持一次引用（MRC）
}

auto MacOSDragBridge::tick() -> void { impl_->tick(); }

auto MacOSDragBridge::drag_out_active() const -> bool { return impl_->out.active; }

auto MacOSDragBridge::drag_in_active() const -> bool { return impl_->in.active; }

auto MacOSDragBridge::note_initiator(MacOSDragBridge *bridge) -> void { initiator_slot() = bridge; }

auto MacOSDragBridge::initiator() -> MacOSDragBridge * { return initiator_slot(); }

auto MacOSDragBridge::begin_drag_out(const DragOffer &offer) -> bool { return impl_->begin_drag_out(offer); }

auto MacOSDragBridge::cancel_drag_out() -> void { impl_->cancel_drag_out(); }

auto MacOSDragBridge::register_backend() -> void {
    if (backend_registered()) {
        return;
    }
    backend_registered() = true;
    register_system_drag_out_backend(SystemDragOutBackend{
        .begin = [](const DragOffer &offer) -> bool {
            MacOSDragBridge *self = initiator();
            return self != nullptr && self->begin_drag_out(offer);
        },
        .cancel = []() -> void {
            MacOSDragBridge *self = initiator();
            if (self != nullptr) {
                self->cancel_drag_out();
            }
        },
    });
}

auto MacOSDragBridge::make_drag_destination_view(void *bridge) -> void * {
    auto *owner = static_cast<MacOSDragBridge *>(bridge);
    if (owner == nullptr) {
        return nil;
    }
    return [[AuroraSystemDragView alloc] initWithBridge:owner->impl_.get()];
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
