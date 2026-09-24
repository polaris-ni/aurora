#pragma once

// ============================================================
// E2E 平台语义通道 · Windows UIA（tools/include/e2e/uia_client.h）
// ------------------------------------------------------------
// 从**同进程客户端**读 UIA 投影，验证「aurora 语义树 → Win32 无障碍桥 → UIA」整条
// 链路确实到达平台。与 `aurora_verify_win32_ua` 探针同一路径（CoCreateInstance
// CUIAutomation8 → ElementFromHandle（内部即 WM_GETOBJECT，触发桥惰性激活）→
// 控件视图遍历器先序采集）；探针已证明 ObjectFromLresult 走不通（同进程拿不到
// 跨进程所需的 Lresult 语义），本头沿用客户端直连形态。COM 调用形式与探针逐字
// 同形（MinGW 头以 #define 提供 UIA_*PropertyId/PatternId 常量、无 MSVC 的
// UIA_PROPERTY_ID/UIA_PATTERN_ID 类型别名，属性入参一律 int）。
//
// 产出统一语义形状（`AccessibilityRole` + name + pattern 位 + 几何投影）：与内核
// `semantic_snapshot()`（harness.h）同词汇，用例侧断言代码跨平台复用。
//
// 门控：仅 `AURORA_PLATFORM_WINDOWS` 下有实现，其它平台包含本头得到空壳
// （`uia_channel_available()` 恒 false），用例据此 `AURORA_TEST_SKIP`——通道缺失是
// 平台能力事实，不是构建失败。链接：`oleacc`（runner 在 WIN32 下统一链接，
// 见 cmake/AuroraTests.cmake；与探针 `aurora_verify_win32_ua` 同一依赖口径）。
// ============================================================

#include <string>
#include <utility>
#include <vector>

#include "e2e/harness.h"

#ifdef AURORA_PLATFORM_WINDOWS

#include <cstddef>
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#ifndef NOMINMAX  // NOLINT(readability-identifier-naming)
#define NOMINMAX
#endif
#include <ole2.h>
#include <uiautomation.h>
#include <windows.h>
// clang-format on

namespace aurora::e2e {

/// @brief UIA ControlType → 统一角色（通道投影词汇；未映射者归 Generic）。
[[nodiscard]] inline auto uia_role_of(int control_type) -> AccessibilityRole {
    switch (control_type) {
        case UIA_ButtonControlTypeId:
            return AccessibilityRole::Button;
        case UIA_CheckBoxControlTypeId:
            return AccessibilityRole::Checkbox;
        case UIA_EditControlTypeId:
            return AccessibilityRole::TextInput;
        case UIA_TextControlTypeId:
            return AccessibilityRole::Text;
        case UIA_SliderControlTypeId:
            return AccessibilityRole::Slider;
        default:
            return AccessibilityRole::Generic;
    }
}

/// @brief 平台投影的语义节点（统一词汇；pattern 以 bool 位呈现，未覆盖的归 Generic）。
struct UiaSemanticNode {
    AccessibilityRole role = AccessibilityRole::Generic;  ///< ControlType 映射后的统一角色
    std::string name;  ///< UIA Name（桥投影的 Name 回退链产物）
    std::string framework_id;  ///< UIA FrameworkId（"Aurora" = 库自有投影，区别于系统非客户区）
    bool invoke = false;  ///< IInvokeProvider 可用
    bool toggle = false;  ///< IToggleProvider 可用
    bool value = false;  ///< IValueProvider 可用
    bool range_value = false;  ///< IRangeValueProvider 可用
    bool rect_ok = false;  ///< BoundingRectangle 投影成功且非空
};

namespace detail {

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access): Win32 UIA/COM 边界——VARIANT 属性只能读联合体成员
/// @brief 读元素的 BSTR 属性（与读屏同款入口）；失败返回空串。
[[nodiscard]] inline auto uia_element_string(IUIAutomationElement &element, int property) -> std::string {
    VARIANT value{};
    VariantInit(&value);
    std::string out;
    if (SUCCEEDED(element.GetCurrentPropertyValueEx(property, TRUE, &value)) && value.vt == VT_BSTR &&
        value.bstrVal != nullptr) {
        const auto len = WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string buffer(static_cast<std::size_t>(len) - 1U, '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, buffer.data(), len, nullptr, nullptr);
            out = std::move(buffer);
        }
    }
    VariantClear(&value);
    return out;
}

/// @brief 读元素的整型属性；失败返回 0。
[[nodiscard]] inline auto uia_element_int(IUIAutomationElement &element, int property) -> int {
    VARIANT value{};
    VariantInit(&value);
    int out = 0;
    if (SUCCEEDED(element.GetCurrentPropertyValueEx(property, TRUE, &value)) &&
        (value.vt == VT_I4 || value.vt == VT_I8)) {
        out = static_cast<int>(value.lVal);
    }
    VariantClear(&value);
    return out;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

/// @brief 元素几何是否投影：`UIA_BoundingRectanglePropertyId` 返回非空矩形
///        （VT_ARRAY|VT_R8 的 left/top/width/height 四元组；与探针逐字同形）。
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access): 矩形属性只能读联合体 parray 成员
[[nodiscard]] inline auto uia_rect_ok(IUIAutomationElement &element) -> bool {
    VARIANT value{};
    VariantInit(&value);
    if (FAILED(element.GetCurrentPropertyValueEx(UIA_BoundingRectanglePropertyId, TRUE, &value))) {
        VariantClear(&value);
        return false;
    }
    bool ok = false;
    if (value.vt == (VT_ARRAY | VT_R8) && value.parray != nullptr) {
        long lower = 0;
        long upper = 0;
        if (SUCCEEDED(SafeArrayGetLBound(value.parray, 1, &lower)) &&
            SUCCEEDED(SafeArrayGetUBound(value.parray, 1, &upper)) && (upper - lower + 1) >= 4) {
            ok = true;  // 至少 4 个 double（left/top/width/height）⇒ 矩形已投影
        }
    }
    VariantClear(&value);
    return ok;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

/// @brief 元素是否已支持某 pattern（控制模式可用性即桥投影的动作面）。
[[nodiscard]] inline auto uia_has_pattern(IUIAutomationElement &element, int pattern) -> bool {
    IUnknown *unknown = nullptr;
    const HRESULT hr = element.GetCurrentPattern(pattern, &unknown);
    if (SUCCEEDED(hr) && unknown != nullptr) {
        unknown->Release();
        return true;
    }
    return false;
}

/// @brief 控件视图先序采集（深度与总量封顶；逐层释放子元素，失败即止）。
inline auto uia_walk(IUIAutomationTreeWalker &walker, IUIAutomationElement &element, int depth,
                     std::vector<UiaSemanticNode> &out) -> void {
    constexpr int uia_walk_max_depth = 16;
    constexpr std::size_t uia_walk_max_nodes = 256;
    if (out.size() >= uia_walk_max_nodes || depth > uia_walk_max_depth) {
        return;
    }
    UiaSemanticNode node;
    node.role = uia_role_of(uia_element_int(element, UIA_ControlTypePropertyId));
    node.name = uia_element_string(element, UIA_NamePropertyId);
    node.framework_id = uia_element_string(element, UIA_FrameworkIdPropertyId);
    node.invoke = uia_has_pattern(element, UIA_InvokePatternId);
    node.toggle = uia_has_pattern(element, UIA_TogglePatternId);
    node.value = uia_has_pattern(element, UIA_ValuePatternId);
    node.range_value = uia_has_pattern(element, UIA_RangeValuePatternId);
    node.rect_ok = uia_rect_ok(element);
    out.push_back(std::move(node));

    IUIAutomationElement *child = nullptr;
    if (FAILED(walker.GetFirstChildElement(&element, &child)) || child == nullptr) {
        return;
    }
    while (child != nullptr) {
        uia_walk(walker, *child, depth + 1, out);
        IUIAutomationElement *next = nullptr;
        if (FAILED(walker.GetNextSiblingElement(child, &next))) {
            next = nullptr;
        }
        child->Release();
        child = next;
    }
}

}  // namespace detail

/// @brief 本构建 / 本环境是否可用 UIA 客户端（CoCreateInstance 探测，不建窗）。
[[nodiscard]] inline auto uia_channel_available() -> bool {
    IUIAutomation *automation = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                        reinterpret_cast<void **>(&automation));  // NOLINT(*-reinterpret-cast)
    if (SUCCEEDED(hr) && automation != nullptr) {
        automation->Release();
        return true;
    }
    return false;
}

/// @brief 经 UIA 客户端采集目标窗口的语义投影（先序；含系统非客户区，调用方按
///        `framework_id == "Aurora"` 过滤自有投影）。
///
/// COM 初始化沿用探针口径：STA 优先，已以其它模式初始化（S_FALSE / RPC_E_CHANGED_MODE）
/// 视为可用；UIA 客户端在任一 apartment 下均可工作。每次调用独立初始化 / 反初始化，
/// 不依赖调用方 COM 状态。
/// @param hwnd 目标窗口（须为已创建的宿主 HWND；空句柄返回 `GeneralInvalidArgument`）
/// @param out 先序采集结果（在既有内容之后追加）
[[nodiscard]] inline auto uia_collect(HWND hwnd, std::vector<UiaSemanticNode> &out) -> Result<void> {
    if (hwnd == nullptr) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument, "uia_collect: hwnd is null")};
    }
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 UIA/COM 边界——CoCreateInstance 出参即 void**
    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || automation == nullptr) {
        // CUIAutomation8（IUIAutomation3）不可得时回退基础版：行为面覆盖本通道所需子集。
        hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                              reinterpret_cast<void **>(&automation));
    }
    if (FAILED(hr) || automation == nullptr) {
        CoUninitialize();
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "uia_collect: IUIAutomation unavailable")};
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(static_cast<UIA_HWND>(hwnd), &root);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr) || root == nullptr) {
        automation->Release();
        CoUninitialize();
        return Result<void>{make_error(ErrorCode::GeneralNotSupported,
                                       "uia_collect: ElementFromHandle failed (no UIA element for HWND)")};
    }

    // 控件视图条件 + 自建遍历器：与读屏同口径（探针 control_view_walker 同形）。
    IUIAutomationCondition *condition = nullptr;
    IUIAutomationTreeWalker *walker = nullptr;
    if (FAILED(automation->get_ControlViewCondition(&condition)) || condition == nullptr) {
        root->Release();
        automation->Release();
        CoUninitialize();
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported, "uia_collect: ControlViewCondition unavailable")};
    }
    hr = automation->CreateTreeWalker(condition, &walker);
    condition->Release();  // walker 已持条件引用
    if (FAILED(hr) || walker == nullptr) {
        root->Release();
        automation->Release();
        CoUninitialize();
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "uia_collect: control-view walker unavailable")};
    }

    detail::uia_walk(*walker, *root, 0, out);

    walker->Release();
    root->Release();
    automation->Release();
    CoUninitialize();

    if (out.empty()) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "uia_collect: empty control view")};
    }
    return Result<void>{};
}

}  // namespace aurora::e2e

#else  // !AURORA_PLATFORM_WINDOWS —— 空壳：用例据 uia_channel_available() 跳过

namespace aurora::e2e {

/// @brief 非 Windows 平台恒不可用（平台能力事实，用例据此跳过）。
[[nodiscard]] inline auto uia_channel_available() -> bool { return false; }

}  // namespace aurora::e2e

#endif
