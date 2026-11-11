#pragma once

// ============================================================
// E2E 平台语义通道 · macOS AX（tools/include/e2e/macos_ax_client.h）
// ------------------------------------------------------------
// 从**同进程客户端**读 AppKit 无障碍协议面，验证「aurora 语义树 → MacOSAxBridge →
// NSAccessibility 协议」整条链路确实到达平台。形态与 Windows UIA 客户端同侧（不是
// AT-SPI 的子进程形态）：`AXUIElementCreateApplication(getpid())` 取本进程的 AX 应用
// 元素 → 按 `kAXWindowsAttribute` 定位标题匹配的窗口 → 先序采集元素树。查询本身会
// 触发内容视图的 `accessibilityChildren`，即桥的惰性激活时机——与读屏上线时同一条路径。
//
// 产出统一语义形状（`AccessibilityRole` + name + 动作位 + 几何投影）：与内核
// `semantic_snapshot()`（harness.h）同词汇，用例侧断言代码跨平台复用。
//
// 两条实测口径（决定了实现形态，不是随手选择）：
// - **查询必须在后台线程**：AX 客户端调用同步等待服务端应答，而服务端就在主线程的
//   AppKit 事件处理里；主线程自问自答即僵死。故采集在 worker 线程跑，主线程按调用方
//   给的 `pump` 持续泵事件（与 AT-SPI 通道「等待期不泵事件 = 客户端永远等不到投影」同理，
//   只是这里泵的是自己的线程）。
// - **本腿是「已授权的加强腿」，不是默认证据**：实测未授权时——同进程
//   `AXUIElementCreateApplication(getpid())` 的调用**返回 success 但属性是退化的**
//   （`NSWindow.title` 明明是 `ScratchWindowTitle`，客户端侧 `kAXRoleAttribute` 一律回
//   `AXApplication`、`kAXTitleAttribute` 回进程名，逐元素的角色/名称/几何拿不到）；跨进程
//   则直接 `kAXErrorAPIDisabled (-25211)`。故 `ax_channel_available()` 以
//   `AXIsProcessTrusted()` 为准：**未授权即不可用**，用例据此跳过——绝不拿退化数据判绿。
//   macOS 通道的默认证据由 `aurora_verify_macos_ax` 探针在 AppKit 协议面（服务端本体）取证。
//
// 门控：仅 `AURORA_PLATFORM_MACOS` 下有实现，其它平台包含本头得到空壳
// （`ax_channel_available()` 恒 false），用例据此 `AURORA_TEST_SKIP`。链接：AX 符号随
// `-framework Cocoa` 的传递闭包可解（实测只链 Cocoa 即可），无需新增框架依赖。
// ============================================================

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef AURORA_PLATFORM_MACOS

#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>

namespace aurora::e2e {

/// @brief 平台投影的语义节点（统一词汇；AX role 映射后归并）。
struct AxSemanticNode {
    AccessibilityRole role = AccessibilityRole::Generic;  ///< AX role 映射后的统一角色
    std::string name;  ///< AX 名称（Title 优先，回落 Description——即桥投影的 accessibilityLabel）
    std::string identifier;  ///< AXIdentifier（桥写入的节点稳定键；诊断用）
    bool press = false;  ///< AXPress 动作可用（Invoke / Click / Toggle 三类动作在 AX 侧的统一出口）
    bool value_present = false;  ///< AXValue 属性在场（取值 / 文本 / 勾状态的值投影）
    bool rect_ok = false;  ///< AXPosition + AXSize 给出非空矩形
};

/// @brief Carbon `Boolean` → C++ `bool` 的显式收敛（CoreFoundation 的返回型是 `UInt8`，隐式转换
///        在本仓 lint 口径下要点名；集中一枚命名函数比逐点 `static_cast` 更好读）。
[[nodiscard]] constexpr auto ax_bool(Boolean value) noexcept -> bool { return value != 0; }

/// @brief 本进程是否可作为 AX 客户端（= 已持有辅助功能授权；未授权时属性是退化的，不可取证）。
[[nodiscard]] inline auto ax_channel_available() -> bool { return ax_bool(AXIsProcessTrusted()); }

/// @brief AX role 字符串 → 统一角色（与 `MacOSAxBridge` 的 `ax_role()` 反向表对齐；未映射者归
///        Generic）。
[[nodiscard]] inline auto ax_role_of(const std::string &role) -> AccessibilityRole {
    if (role == "AXButton") {
        return AccessibilityRole::Button;
    }
    if (role == "AXCheckBox") {
        return AccessibilityRole::Checkbox;
    }
    if (role == "AXSlider") {
        return AccessibilityRole::Slider;
    }
    if (role == "AXTextField" || role == "AXTextArea") {
        return AccessibilityRole::TextInput;
    }
    if (role == "AXStaticText") {
        return AccessibilityRole::Text;
    }
    if (role == "AXImage") {
        return AccessibilityRole::Image;
    }
    if (role == "AXList") {
        return AccessibilityRole::List;
    }
    if (role == "AXRow") {
        return AccessibilityRole::ListItem;
    }
    if (role == "AXProgressIndicator") {
        return AccessibilityRole::Progress;
    }
    if (role == "AXDialog") {
        return AccessibilityRole::Dialog;
    }
    return AccessibilityRole::Generic;
}

namespace detail {

/// @brief CFTypeRef 的按值持有（离开作用域即 `CFRelease`；本通道只在 C++ 侧编译，无 ARC 可用）。
class CfHolder {
  public:
    /// @brief 接管一次拷贝返回的引用（可为 nullptr）。
    /// @param value `AXUIElementCopyAttributeValue` 等函数的出参；所有权移交给本对象。
    explicit CfHolder(CFTypeRef value) : value_(value) {}
    CfHolder(const CfHolder &) = delete;
    auto operator=(const CfHolder &) -> CfHolder & = delete;
    CfHolder(CfHolder &&other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    auto operator=(CfHolder &&other) noexcept -> CfHolder & {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    ~CfHolder() { reset(); }

    /// @brief 放弃所有权（无副作用地交出引用，调用方随后自行释放）。
    /// @return 内部持有的 CFTypeRef；所有权移出后本对象不再释放。
    [[nodiscard]] auto release() noexcept -> CFTypeRef {
        const CFTypeRef value = value_;
        value_ = nullptr;
        return value;
    }

    /// @brief 只读访问持有值（不转移所有权）。
    [[nodiscard]] auto get() const noexcept -> CFTypeRef { return value_; }

    /// @brief 是否持有非空引用。
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

  private:
    auto reset() noexcept -> void {
        if (value_ != nullptr) {
            CFRelease(value_);
            value_ = nullptr;
        }
    }

    CFTypeRef value_ = nullptr;
};

/// @brief CFString → UTF-8 `std::string`（非 CFString / 转换失败得空串，不抛）。
[[nodiscard]] inline auto cf_string_to_std(CFStringRef text) -> std::string {
    if (text == nullptr || CFGetTypeID(text) != CFStringGetTypeID()) {
        return {};
    }
    // 先走零拷贝快路径，再退回按 UTF-8 取值到定长缓冲（Apple 的 CFStringGetCString 语义）。
    if (const char *fast = CFStringGetCStringPtr(text, kCFStringEncodingUTF8); fast != nullptr) {
        return std::string{fast};
    }
    const CFIndex length = CFStringGetLength(text);
    const CFIndex max_bytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    if (max_bytes <= 1) {
        return {};
    }
    std::vector<char> buffer(static_cast<std::size_t>(max_bytes));
    if (!ax_bool(CFStringGetCString(text, buffer.data(), max_bytes, kCFStringEncodingUTF8))) {
        return {};
    }
    return std::string{buffer.data()};
}

/// @brief 读取一个字符串属性（Title / Description / Identifier / Role）。
[[nodiscard]] inline auto ax_copy_string(AXUIElementRef element, CFStringRef attribute) -> std::string {
    if (element == nullptr) {
        return {};
    }
    CFTypeRef raw = nullptr;
    if (AXUIElementCopyAttributeValue(element, attribute, &raw) != kAXErrorSuccess) {
        return {};
    }
    const CfHolder holder{raw};
    return cf_string_to_std(static_cast<CFStringRef>(holder.get()));
}

/// @brief 元素是否声明了某个 AX 动作（`AXUIElementCopyActionNames` 数组比对）。
[[nodiscard]] inline auto ax_has_action(AXUIElementRef element, CFStringRef action) -> bool {
    if (element == nullptr) {
        return false;
    }
    CFArrayRef raw = nullptr;
    if (AXUIElementCopyActionNames(element, &raw) != kAXErrorSuccess) {
        return false;
    }
    const CfHolder holder{raw};
    if (CFGetTypeID(holder.get()) != CFArrayGetTypeID()) {
        return false;
    }
    const auto *const names = static_cast<CFArrayRef>(holder.get());
    for (CFIndex i = 0; i < CFArrayGetCount(names); ++i) {
        if (CFStringCompare(static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, i)), action, 0) ==
            kCFCompareEqualTo) {
            return true;
        }
    }
    return false;
}

/// @brief 元素的几何投影是否非空（AXPosition + AXSize 两枚 AXValue 合成）。
[[nodiscard]] inline auto ax_rect_nonempty(AXUIElementRef element) -> bool {
    if (element == nullptr) {
        return false;
    }
    CFTypeRef raw_position = nullptr;
    CFTypeRef raw_size = nullptr;
    if (AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &raw_position) != kAXErrorSuccess) {
        return false;
    }
    const CfHolder position_holder{raw_position};
    if (AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &raw_size) != kAXErrorSuccess) {
        return false;
    }
    const CfHolder size_holder{raw_size};
    CGPoint position = CGPointZero;
    CGSize size = CGSizeZero;
    // SDK 把 kAXValue*Type 声明成 const UInt32（CF_ENUM 在 C++ 下不是 AXValueType），需显式收敛。
    if (!ax_bool(AXValueGetValue(static_cast<AXValueRef>(position_holder.get()),
                                 static_cast<AXValueType>(kAXValueCGPointType), &position)) ||
        !ax_bool(AXValueGetValue(static_cast<AXValueRef>(size_holder.get()),
                                 static_cast<AXValueType>(kAXValueCGSizeType), &size))) {
        return false;
    }
    return size.width > 0.0 && size.height > 0.0;
}

/// @brief 元素的 `AXValue` 是否在场（不判类型：取值 / 文本 / 勾状态各自不同）。
[[nodiscard]] inline auto ax_has_value(AXUIElementRef element) -> bool {
    if (element == nullptr) {
        return false;
    }
    CFTypeRef raw = nullptr;
    if (AXUIElementCopyAttributeValue(element, kAXValueAttribute, &raw) != kAXErrorSuccess) {
        return false;
    }
    if (raw != nullptr) {
        CFRelease(raw);
    }
    return true;
}

/// @brief 树遍历的封顶（深度 / 总量），与 `uia_client.h` / `atspi_client.h` 同口径防失控。
inline constexpr int AURORA_AX_DEPTH_CAP = 32;
inline constexpr std::size_t AURORA_AX_NODE_CAP = 400;

/// @brief 先序采集一个元素及其子树（追加进 `out`，封顶后停止下钻）。
inline auto ax_collect_tree(AXUIElementRef element, int depth, std::vector<AxSemanticNode> &out) -> void {
    if (element == nullptr || depth > AURORA_AX_DEPTH_CAP || out.size() >= AURORA_AX_NODE_CAP) {
        return;
    }
    AxSemanticNode node;
    node.role = ax_role_of(ax_copy_string(element, kAXRoleAttribute));
    // 名称口径：Title 优先、回落 Description——桥把 Name 写进 accessibilityLabel，
    // 而 accessibilityLabel 在协议面上就是 AXDescription。
    node.name = ax_copy_string(element, kAXTitleAttribute);
    if (node.name.empty()) {
        node.name = ax_copy_string(element, kAXDescriptionAttribute);
    }
    node.identifier = ax_copy_string(element, kAXIdentifierAttribute);
    node.press = ax_has_action(element, kAXPressAction);
    node.value_present = ax_has_value(element);
    node.rect_ok = ax_rect_nonempty(element);
    // 容器（role 未映射且无名称）不入库：窗口分组 / 标题栏投影会淹没比对，与 UIA 侧
    // 只比 `FrameworkId=="Aurora"` 自有投影是同一目的的不同手段。
    if (node.role != AccessibilityRole::Generic || !node.name.empty()) {
        out.push_back(std::move(node));
    }

    CFTypeRef raw_children = nullptr;
    if (AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, &raw_children) != kAXErrorSuccess) {
        return;
    }
    const CfHolder children_holder{raw_children};
    if (CFGetTypeID(children_holder.get()) != CFArrayGetTypeID()) {
        return;
    }
    const auto *const children = static_cast<CFArrayRef>(children_holder.get());
    const CFIndex count = CFArrayGetCount(children);
    for (CFIndex i = 0; i < count && out.size() < AURORA_AX_NODE_CAP; ++i) {
        ax_collect_tree(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(children, i)), depth + 1, out);
    }
}

/// @brief 后台线程的执行体：轮询定位目标窗口，命中即先序采集，随后置完成标志。
/// @note 只写 `worker` 的私有字段；`nodes` 的写入全部先于 `done` 的 release 存储。
struct AxWorkerState {
    std::string title;  ///< 目标窗口标题（`WindowSpec::title`）
    int budget_ms = 15000;  ///< worker 侧总预算（含轮询等待桥激活）
    std::vector<AxSemanticNode> nodes;  ///< 采集结果（先序）
    bool frame_found = false;  ///< 是否找到标题匹配的窗口
    std::string last_error;  ///< 最近一次 AX 调用的错误名（诊断入错误消息，不静默）
    std::string observed;  ///< 实际看到的窗口标题列表（诊断入错误消息）
    std::atomic<bool> done{false};  ///< 完成标志（release/acquire 与 nodes 配对）
};

/// @brief `AXError` → 可读名（只列本通道实际会遇到的码；未知码回落十进制）。
[[nodiscard]] inline auto ax_error_name(AXError error) -> std::string {
    switch (error) {
        case kAXErrorSuccess:
            return "success";
        case kAXErrorFailure:
            return "failure";
        case kAXErrorIllegalArgument:
            return "illegal_argument";
        case kAXErrorInvalidUIElement:
            return "invalid_ui_element";
        case kAXErrorCannotComplete:
            return "cannot_complete";
        case kAXErrorAttributeUnsupported:
            return "attribute_unsupported";
        case kAXErrorActionUnsupported:
            return "action_unsupported";
        case kAXErrorNotImplemented:
            return "not_implemented";
        default:
            return "ax_error_" + std::to_string(static_cast<int>(error));
    }
}

inline auto ax_worker_run(AxWorkerState &state) -> void {
    const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(state.budget_ms);
    AXUIElementRef application = AXUIElementCreateApplication(::getpid());
    if (application == nullptr) {
        state.last_error = "element_create_failed";
        state.done.store(true, std::memory_order_release);
        return;
    }
    // 单条消息 2s 上限：主线程未在泵时也要让 worker 有界返回，而不是无限等待。
    AXUIElementSetMessagingTimeout(application, 2.0F);
    while (std::chrono::steady_clock::now() < stop) {
        // 出参按 `CFTypeRef *`（属性值类型擦除的原生签名）接，再按类型 ID 判形后向下转型——
        // 与 `ax_copy_string` / `ax_has_value` 同一路读法，不需要对指针本身做重解释。
        CFTypeRef raw_windows = nullptr;
        const AXError windows_error = AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &raw_windows);
        state.last_error = ax_error_name(windows_error);
        if (windows_error == kAXErrorSuccess) {
            const CfHolder windows_holder{raw_windows};
            if (CFGetTypeID(windows_holder.get()) == CFArrayGetTypeID()) {
                const auto *const windows = static_cast<CFArrayRef>(windows_holder.get());
                state.observed.clear();
                for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
                    const auto *const window = static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(windows, i));
                    const std::string window_title = ax_copy_string(window, kAXTitleAttribute);
                    if (i > 0) {
                        state.observed += ",";
                    }
                    state.observed += window_title.empty() ? "(untitled)" : window_title;
                    if (window_title == state.title) {
                        state.frame_found = true;
                        ax_collect_tree(window, 0, state.nodes);
                        break;
                    }
                }
            }
        }
        if (state.frame_found) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CFRelease(application);
    state.done.store(true, std::memory_order_release);
}

}  // namespace detail

/// @brief 经 AX 客户端读取指定窗口的语义投影（先序）。
///
/// @param frame_title 目标窗口标题（`WindowSpec::title`，客户端按窗口 AXTitle 定位）
/// @param out 先序采集结果（在既有内容之后追加）
/// @param pump 等待期回调（每轮轮询间调用；须推进窗口事件泵使桥能应答查询——查询正由它激活）
/// @param timeout_ms 主线程侧总等待上限（worker 自身另有同量级预算）
/// @note Thread: main-thread only（pump 须在主线程；采集在 worker 线程）
[[nodiscard]] inline auto ax_collect(const std::string &frame_title, std::vector<AxSemanticNode> &out,
                                     const std::function<void()> &pump, int timeout_ms) -> Result<void> {
    if (frame_title.empty()) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument, "ax_collect: empty frame title")};
    }

    detail::AxWorkerState state;
    state.title = frame_title;
    state.budget_ms = timeout_ms > 0 ? timeout_ms : 15000;
    std::thread worker([&state] { detail::ax_worker_run(state); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(state.budget_ms + 4000);
    while (!state.done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (pump) {
            pump();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 有界 join：worker 的每一条 AX 调用受 2s messaging timeout 约束，最坏情况仍能自行返回。
    worker.join();

    if (state.frame_found) {
        for (auto &node : state.nodes) {
            out.push_back(std::move(node));
        }
    }
    if (out.empty()) {
        // 诊断随错误走：最后一次的 AX 错误码与实际看到的窗口列表，是区分「没建窗」「标题不符」
        // 「主线程未在泵（cannot_complete / timeout）」三态的唯一线索。
        std::string detail = state.last_error;
        if (!state.observed.empty()) {
            detail += "; windows=[" + state.observed + "]";
        }
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported,
                       state.frame_found ? "ax_collect: frame found but projection is empty (" + detail + ")"
                                         : "ax_collect: target window not found on the AX protocol (" + detail + ")")};
    }
    return Result<void>{};
}

}  // namespace aurora::e2e

#else  // 非 macOS —— 空壳：用例据 ax_channel_available() 跳过

namespace aurora::e2e {

/// @brief 非目标平台恒不可用（平台能力事实，用例据此跳过）。
[[nodiscard]] inline auto ax_channel_available() -> bool { return false; }

}  // namespace aurora::e2e

#endif
