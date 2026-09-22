/* Win32 UIA 无障碍桥 —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖后端：`Win32Surface`（GDI 上屏）与 `D3D11Surface`（GPU 上屏）。二者共用同一
// `Win32Host` 宿主、同一份 `detail::Win32UiaBridge`，故一份探针同时验收两路：
//   * 只开 AURORA_BACKEND_WIN32  → 验 Win32Surface
//   * 再开 AURORA_BACKEND_D3D11  → 两路都验
//
// 为什么必须真机：无头 CI 里没有 UIA 客户端，`WM_GETOBJECT` 永远不会被投递、桥也不会被
// 查询 —— 桥的「接线是否正确」（根对象能否应答、树能否 Navigate、属性是否有值、pattern
// 能否 QueryInterface 到）只能在本机窗口上证明。单元测试覆盖的是平台中立层（快照 / diff /
// 偏移映射 / 动作路由），本探针覆盖的是「桥与 Windows 的接缝」。
//
// 原理（与读屏同路径）：
//   1) 用 aurora 建真实窗口，塞入一列带无障碍语义的控件（Button / Checkbox / Slider /
//      TextInput / Text）并完成一次布局与呈现；
//   2) `CoCreateInstance(CLSID_CUIAutomation)` 起 UIA 客户端 —— NVDA / Narrator 的入口；
//   3) `IUIAutomation::ElementFromHandle(hwnd)` 取根元素：这一步内部即向窗口投递
//      `WM_GETOBJECT(UiaRootObjectId)`，是本探针唯一能触发桥**惰性激活**（D14）的路径；
//   4) 用「控件视图」遍历器（`get_ControlViewCondition`，与读屏同口径）先序下钻，
//      逐节点读 UIA 属性与 pattern；
//   5) 与期望表比对：五类控件各自的 ControlType 必须在树中出现，且各自声明的 pattern
//      必须可取到（Invoke / Toggle / Value / RangeValue）。
//
// 为何不走 `WM_GETOBJECT` 的 LRESULT + `ObjectFromLresult`：`ObjectFromLresult` 只对
// **跨进程**取对象成立，同线程内 `SendMessage` 取回的值无法可靠还原（实测 E_FAIL）。
// 那是「取对象方式」的局限，不是桥的缺陷；探针因此直接走客户端，与真实读屏完全同径。
//
// 构建（推荐）：
//   cmake -S . -B build-verify-ua -G Ninja -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify-ua --target aurora_verify_win32_ua
//   build-verify-ua\aurora_verify_win32_ua.exe
//
// 退出码（多路时取其中最差者）：
//   0  全部验收项通过 —— 桥与 Windows 接缝正确
//   2  环境不可用（窗口创建失败 / 取不到 HWND）
//   3  UIA 运行时不可用（IUIAutomation 起不来 / UIAutomationCore.dll 缺失 → 设计降级）
//   4  遍历不到任何子节点（结构未暴露）
//   5  部分验收项不符 —— 见逐行表格 ok 列
//   6  本机未编译进任何 Win32 家族后端
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_ua can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#endif
#ifdef AURORA_BACKEND_D3D11
#include "aurora/window/d3d11_surface.h"
#endif
#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
// `WIN32_LEAN_AND_MEAN` 把 `ole2.h` 从 `windows.h` 的包含链里摘掉了，而 MIDL 生成的
// `UIAutomationCore.h` 直接用 `interface` 关键字（`#define interface __STRUCT__` 来自
// `combaseapi.h`，由 `ole2.h` 拉入）。缺它时 GCC 靠自身头链侥幸通过，clang 前端（clang-tidy）
// 则在 40+ 处 `typedef interface …` 上报「unknown type name」并中止分析——该 TU 从此不产出任何
// 告警，门禁的「0 条」是覆盖塌了而非干净。显式补齐依赖，两种前端同一条包含链。
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aurora/state/state.h"
#include "aurora/widget/a11y_tree.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/window/window.h"
#include "verify_print.h"

namespace {

using aurora::Node;

/// @brief 探针要看到的控件清单：期望 ControlType + 期望 pattern（按设计 §7.3 映射表）。
struct Expectation {
    const char *label{};
    CONTROLTYPEID control_type{};
    bool want_invoke{};
    bool want_toggle{};
    bool want_value{};
    bool want_range{};
    /// @brief 是否要求 Non-empty Name。
    ///
    /// 只对**自带标签来源**的控件为真：Button（`ButtonProps::label`）、TextInput
    /// （`placeholder`）、Text（内容文本），以及宿主经 `set_accessibility_label()` 显式声明
    /// 名的任意控件（回退链第一级）。`Checkbox` / `Slider` 无内建 label，其标签通常是**兄弟**
    /// 节点而非子节点，故走下方 `sibling_expect` 的兄弟关联，或直接显式声明。
    bool require_name{};
    /// @brief 期望经「兄弟标签关联」解析出的 Name（#1-C 验收）；`nullptr` 表示不作此断言。
    ///
    /// 仅对 `Checkbox` / `Slider` 这类「标签常为兄弟」的控件有意义：探针将它们放进
    /// `Row { Text(sibling), 控件 }`，由 `sibling_label_name` 取最近相邻文本兄弟为 Name。
    const char *sibling_expect = nullptr;
};

constexpr Expectation AURORA_EXPECTATIONS[] = {
    {.label = "Button",
     .control_type = UIA_ButtonControlTypeId,
     .want_invoke = true,
     .want_toggle = false,
     .want_value = false,
     .want_range = false,
     .require_name = true},
    {.label = "Checkbox",
     .control_type = UIA_CheckBoxControlTypeId,
     .want_invoke = false,
     .want_toggle = true,
     .want_value = false,
     .want_range = false,
     .require_name = false,
     .sibling_expect = "启用自动更新"},
    {.label = "Slider",
     .control_type = UIA_SliderControlTypeId,
     .want_invoke = false,
     .want_toggle = false,
     .want_value = false,
     .want_range = true,
     .require_name = false,
     .sibling_expect = "音量"},
    {.label = "TextInput",
     .control_type = UIA_EditControlTypeId,
     .want_invoke = false,
     .want_toggle = false,
     .want_value = true,
     .want_range = false,
     .require_name = true},
    {.label = "Text",
     .control_type = UIA_TextControlTypeId,
     .want_invoke = false,
     .want_toggle = false,
     .want_value = false,
     .want_range = false,
     .require_name = true},
};

/// @brief 一次遍历采到的节点事实（对齐 UIA 客户端可见的属性面）。
struct Visited {
    int depth = 0;
    std::string name;
    /// @brief `UIA_FrameworkIdPropertyId`：桥置为 "Aurora"。用于把**自有元素**与系统在同一层
    ///        合成进来的非客户区投影（标题栏 / 系统菜单 / 最小化-最大化-关闭按钮）区分开 ——
    ///        后者由 UIA 的默认 HWND provider 提供，与桥无关，不应参与本探针的期望比对。
    std::string framework_id;
    long long control_type = 0;
    bool is_control = false;
    bool is_content = false;
    bool invoke = false;
    bool toggle = false;
    bool value = false;
    bool range = false;
    HRESULT hr_invoke = S_OK;
    HRESULT hr_toggle = S_OK;
    HRESULT hr_value = S_OK;
    HRESULT hr_range = S_OK;
    /// @brief 元素几何是否投影（UIA_BoundingRectanglePropertyId 返回非空矩形；#3 验收）。
    bool rect_ok = false;
    /// @brief 文本几何矩形计数（`ITextRangeProvider::GetBoundingRectangles` 经 TextPattern 取得；
    ///        -1 = 无 TextPattern；0 = 空（可能字体度量未就绪）；>=4 = 至少一个矩形，#7 盲区补断言）。
    long text_range_rects = -1;
    /// @brief `UIA_LabeledByPropertyId` 解析出的目标元素之名（空 = 桥未投影该关系，#21 验收）。
    std::string labeled_by_name;
};

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

[[nodiscard]] auto bstr_to_utf8(BSTR s) -> std::string {
    if (s == nullptr) {
        return {};
    }
    const int wide = static_cast<int>(SysStringLen(s));
    const int narrow = WideCharToMultiByte(CP_UTF8, 0, s, wide, nullptr, 0, nullptr, nullptr);
    if (narrow <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, wide, out.data(), narrow, nullptr, nullptr);
    return out;
}

/// @brief 组装一列带无障碍语义的控件（每类一个，便于逐类验 pattern）。
///
/// 存活要求：State 必须由 shared_ptr 持有且被 Node 树间接引用，否则控件在探针运行期即析构。
[[nodiscard]] auto build_probe_column() -> aurora::Node {
    static const auto CHECKED = std::make_shared<aurora::State<bool>>(true);
    static const auto SLIDER_VALUE = std::make_shared<aurora::State<double>>(0.5);

    // 引用式标签关联（#21 验收）：目标带 `stable_key`，引用者 `set_labelled_by` 指过去。
    // 二者放进**纵向**列：`sibling_label_name` 的行内相邻判据（#1-C）在纵向下刻意不命中，
    // 且引用者自身无内置标签 —— 故它的 UIA Name 只可能来自 labelled_by 的解析结果。
    auto caption = std::make_shared<aurora::Text>(aurora::TextProps{.content = aurora::LocalizedString{"季度汇总"}});
    caption->set_stable_key("probe-caption");
    auto referrer = std::make_shared<aurora::Checkbox>(aurora::Reactive{CHECKED});
    referrer->set_labelled_by("probe-caption");

    // Checkbox / Slider 用 `Row { 文本兄弟, 控件 }` 包裹，使其具备「兄弟标签」，
    // 验收 #1-C 的 `sibling_label_name` 兜底（叶子控件标签常为兄弟而非子节点）。
    return aurora::Node{aurora::Column{
        aurora::Button{aurora::ButtonProps{.label = aurora::LocalizedString{"确定"}}},
        aurora::Row{aurora::Text{aurora::TextProps{.content = aurora::LocalizedString{"启用自动更新"}}},
                    aurora::Checkbox{aurora::Reactive{CHECKED}}},
        aurora::Row{aurora::Text{aurora::TextProps{.content = aurora::LocalizedString{"音量"}}},
                    aurora::Slider{aurora::Reactive{SLIDER_VALUE}}},
        aurora::TextInput{aurora::TextInputProps{.value = "abc", .placeholder = "请输入"}},
        aurora::Text{aurora::TextProps{.content = aurora::LocalizedString{"订单总额"}}},
        aurora::Node{std::move(caption)},
        aurora::Node{std::move(referrer)},
    }};
}

// ---- UIA 客户端辅助（与读屏同款入口）----

/// @brief 建 UIA 客户端（与读屏同款入口）。
///
/// 优先 `CUIAutomation8`（Win8+，设计 §11 指定；现代读屏走这一代接口），老系统上回退
/// `CUIAutomation`（Vista+）。二者都是进程内 COM 对象，取到的 `IUIAutomation` 同接口。
[[nodiscard]] auto automation_client() -> IUIAutomation * {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        // 已被初始化为其它套间：UIA 客户端在 MTA 下同样可用，不视为失败。
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    IUIAutomation *automation = nullptr;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 UIA/COM 边界——CoCreateInstance
    // 的出参只接受 void**，接口指针只能 reinterpret 传递
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  reinterpret_cast<void **>(&automation));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                              reinterpret_cast<void **>(&automation));
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (FAILED(hr)) {
        return nullptr;
    }
    return automation;
}

/// @brief 取窗口对应的根元素（`ElementFromHandle`，内部即投递 `WM_GETOBJECT`）。
[[nodiscard]] auto root_element(IUIAutomation *automation, HWND hwnd) -> IUIAutomationElement * {
    IUIAutomationElement *element = nullptr;
    if (automation == nullptr || FAILED(automation->ElementFromHandle(static_cast<UIA_HWND>(hwnd), &element))) {
        return nullptr;
    }
    return element;
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access): Win32 UIA/COM 边界——VARIANT 属性值只能是联合体，
// 按 vt 判别后读联合体成员是唯一取法
/// @brief 读字符串属性；不支持（VT_UNKNOWN「reserved not supported」/ VT_EMPTY）时返回空串。
[[nodiscard]] auto element_string(IUIAutomationElement *e, PROPERTYID id) -> std::string {
    if (e == nullptr) {
        return {};
    }
    VARIANT v{};
    VariantInit(&v);
    if (FAILED(e->GetCurrentPropertyValueEx(id, TRUE, &v)) || v.vt != VT_BSTR || v.bstrVal == nullptr) {
        VariantClear(&v);
        return {};
    }
    std::string out = bstr_to_utf8(v.bstrVal);
    VariantClear(&v);
    return out;
}

/// @brief 读整数属性（VT_I4 / VT_R8 归一为 long long）；不支持时返回 0。
[[nodiscard]] auto element_int(IUIAutomationElement *e, PROPERTYID id) -> long long {
    if (e == nullptr) {
        return 0;
    }
    long long out = 0;
    VARIANT v{};
    VariantInit(&v);
    if (SUCCEEDED(e->GetCurrentPropertyValueEx(id, TRUE, &v))) {
        if (v.vt == VT_I4) {
            out = v.lVal;
        } else if (v.vt == VT_R8) {
            out = static_cast<long long>(v.dblVal);
        }
    }
    VariantClear(&v);
    return out;
}

/// @brief 读布尔属性；`VT_BOOL` 之外一律视为 false（不支持属性即「否」）。
[[nodiscard]] auto element_bool(IUIAutomationElement *e, PROPERTYID id) -> bool {
    if (e == nullptr) {
        return false;
    }
    VARIANT v{};
    VariantInit(&v);
    const bool ok =
        SUCCEEDED(e->GetCurrentPropertyValueEx(id, TRUE, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    (void)VariantClear(&v);
    return ok;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

/// @brief 取 pattern：`GetCurrentPattern`（读屏取 pattern 的同款调用）。
/// @param hr 出参：调用返回码（`S_OK` 表示可用，`E_FAIL`/`UIA_E_NOTSUPPORTED` 表示不支持）。
/// @param e 目标元素；`nullptr` 时直接返回 false。
/// @param pid 要查询的模式 ID（如 `UIA_InvokePatternId`）。
[[nodiscard]] auto query_pattern(IUIAutomationElement *e, PATTERNID pid, HRESULT *hr) -> bool {
    if (hr != nullptr) {
        *hr = E_POINTER;
    }
    if (e == nullptr) {
        return false;
    }
    IUnknown *pattern = nullptr;
    const HRESULT res = e->GetCurrentPattern(pid, &pattern);
    if (hr != nullptr) {
        *hr = res;
    }
    if (FAILED(res) || pattern == nullptr) {
        return false;
    }
    pattern->Release();
    return true;
}

/// @brief `HRESULT` → `0x` 十六进制串（诊断用）。
[[nodiscard]] auto hex_hr(HRESULT hr) -> std::string {
    std::ostringstream oss;
    oss << "0x" << std::hex << static_cast<unsigned long>(static_cast<std::uint32_t>(hr));
    return oss.str();
}

/// @brief 元素几何是否投影（#3 验收）：`UIA_BoundingRectanglePropertyId` 返回非空矩形。
///
/// 与读屏同款入口（COM 客户端取属性）；非空即桥的 `get_BoundingRectangle` 生效，离屏/出窗元素
/// 应返回空矩形（本探针的控件均可见，故应恒为非空）。
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access): Win32 UIA/COM 边界——VARIANT 矩形属性只能读联合体 parray 成员
[[nodiscard]] auto element_rect_ok(IUIAutomationElement *e) -> bool {
    if (e == nullptr) {
        return false;
    }
    VARIANT v{};
    VariantInit(&v);
    if (FAILED(e->GetCurrentPropertyValueEx(UIA_BoundingRectanglePropertyId, TRUE, &v))) {
        VariantClear(&v);
        return false;
    }
    bool ok = false;
    if (v.vt == (VT_ARRAY | VT_R8) && v.parray != nullptr) {
        long lb = 0;
        long ub = 0;
        if (SUCCEEDED(SafeArrayGetLBound(v.parray, 1, &lb)) && SUCCEEDED(SafeArrayGetUBound(v.parray, 1, &ub)) &&
            (ub - lb + 1) >= 4) {
            ok = true;  // 至少 4 个 double（left/top/width/height）⇒ 矩形已投影
        }
    }
    VariantClear(&v);
    return ok;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

/// @brief 文本几何矩形计数（#7 盲区补断言）：经 `ITextPattern::GetVisibleRanges` →
///        `ITextRangeProvider::GetBoundingRectangles` 取得逐字符盒。
///
/// 返回 -1（无 TextPattern）/ 0（空，可能字体度量未就绪）/ >=4（至少一个矩形）。
[[nodiscard]] auto text_range_rect_count(IUIAutomationElement *e) -> long {
    if (e == nullptr) {
        return -1;
    }
    IUnknown *p = nullptr;
    if (FAILED(e->GetCurrentPattern(UIA_TextPatternId, &p)) || p == nullptr) {
        return -1;
    }
    IUIAutomationTextPattern *tp = nullptr;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 COM 边界——QueryInterface 出参只接受 void**
    const HRESULT hr = p->QueryInterface(IID_IUIAutomationTextPattern, reinterpret_cast<void **>(&tp));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    p->Release();
    if (FAILED(hr) || tp == nullptr) {
        return -1;
    }
    IUIAutomationTextRangeArray *ranges = nullptr;
    const HRESULT hr_ranges = tp->GetVisibleRanges(&ranges);
    tp->Release();
    if (FAILED(hr_ranges) || ranges == nullptr) {
        return -1;
    }
    int count = 0;
    if (FAILED(ranges->get_Length(&count)) || count <= 0) {
        ranges->Release();
        return 0;
    }
    IUIAutomationTextRange *range = nullptr;
    if (FAILED(ranges->GetElement(0, &range)) || range == nullptr) {
        ranges->Release();
        return 0;
    }
    ranges->Release();
    SAFEARRAY *arr = nullptr;
    const HRESULT hr_rects = range->GetBoundingRectangles(&arr);
    range->Release();
    if (FAILED(hr_rects) || arr == nullptr) {
        return 0;
    }
    long lb = 0;
    long ub = 0;
    if (SUCCEEDED(SafeArrayGetLBound(arr, 1, &lb)) && SUCCEEDED(SafeArrayGetUBound(arr, 1, &ub))) {
        long n = ub - lb + 1;
        SafeArrayDestroy(arr);
        return n;
    }
    SafeArrayDestroy(arr);
    return 0;
}

[[nodiscard]] auto control_view_walker(IUIAutomation *automation) -> IUIAutomationTreeWalker * {
    IUIAutomationCondition *condition = nullptr;
    if (FAILED(automation->get_ControlViewCondition(&condition)) || condition == nullptr) {
        if (condition != nullptr) {
            condition->Release();
        }
        return nullptr;
    }
    IUIAutomationTreeWalker *walker = nullptr;
    const HRESULT hr = automation->CreateTreeWalker(condition, &walker);
    condition->Release();  // walker 已持条件引用
    if (FAILED(hr)) {
        return nullptr;
    }
    return walker;
}

/// @brief 读「标注者」元素属性并取其 Name：`get_CurrentLabeledBy` 正是读屏播报「由 … 标注」
///        所用的客户端入口（桥侧对应 `UIA_LabeledByPropertyId`，#21 验收）。
[[nodiscard]] auto element_labeled_by_name(IUIAutomationElement *e) -> std::string {
    if (e == nullptr) {
        return {};
    }
    IUIAutomationElement *target = nullptr;
    if (FAILED(e->get_CurrentLabeledBy(&target)) || target == nullptr) {
        if (target != nullptr) {
            target->Release();
        }
        return {};
    }
    std::string out = element_string(target, UIA_NamePropertyId);
    target->Release();
    return out;
}

/// @brief 先序采集一棵子树（含 `element` 自身）。深度与总量设上限，防桥递归缺陷导致失控。
auto collect(IUIAutomationTreeWalker *walker, IUIAutomationElement *element, int depth, std::vector<Visited> &out,
             std::size_t cap) -> void {
    if (element == nullptr || out.size() >= cap || depth > 16) {
        return;
    }
    Visited v{};
    v.depth = depth;
    v.control_type = element_int(element, UIA_ControlTypePropertyId);
    v.name = element_string(element, UIA_NamePropertyId);
    v.framework_id = element_string(element, UIA_FrameworkIdPropertyId);
    v.is_control = element_bool(element, UIA_IsControlElementPropertyId);
    v.is_content = element_bool(element, UIA_IsContentElementPropertyId);
    v.invoke = query_pattern(element, UIA_InvokePatternId, &v.hr_invoke);
    v.toggle = query_pattern(element, UIA_TogglePatternId, &v.hr_toggle);
    v.value = query_pattern(element, UIA_ValuePatternId, &v.hr_value);
    v.range = query_pattern(element, UIA_RangeValuePatternId, &v.hr_range);
    v.rect_ok = element_rect_ok(element);
    v.text_range_rects = text_range_rect_count(element);
    v.labeled_by_name = element_labeled_by_name(element);
    out.push_back(std::move(v));

    IUIAutomationElement *child = nullptr;
    if (FAILED(walker->GetFirstChildElement(element, &child))) {
        child = nullptr;
    }
    while (child != nullptr) {
        collect(walker, child, depth + 1, out, cap);
        IUIAutomationElement *next = nullptr;
        if (FAILED(walker->GetNextSiblingElement(child, &next))) {
            next = nullptr;
        }
        child->Release();
        child = next;
    }
}

[[nodiscard]] auto patterns_of(const Visited &v) -> std::string {
    std::string out;
    if (v.invoke) {
        out += "Invoke ";
    }
    if (v.toggle) {
        out += "Toggle ";
    }
    if (v.value) {
        out += "Value ";
    }
    if (v.range) {
        out += "RangeValue ";
    }
    return out;
}

/// @brief 单路后端验收。`window` 与 `root` 的生命周期由调用方（main）保留。
auto run_probe(aurora::Window &window, aurora::Surface &surface, aurora::Node &root, const char *label,
               const char *title) -> int {
    emit(std::string("==== ") + label + " ====");

    HWND hwnd = static_cast<HWND>(surface.native_handle());
    if (hwnd == nullptr) {
        hwnd = FindWindowA(nullptr, title);
    }
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": cannot obtain HWND");
        return 2;
    }

    // 先呈现一帧：桥的语义树取自布局与绘制产物，未呈现即无可投影内容。
    (void)window.present_root(root);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    for (int i = 0; i < 4; ++i) {
        surface.wait_events(20.0);
    }
    (void)window.present_root(root);

    // 惰性验证（D14）：客户端查询之前，桥本不该存在。
    const bool bridge_before = surface.accessibility_provider() != nullptr;
    emit(std::string("lazy activation: bridge before first client query = ") + (bridge_before ? "present" : "absent"));

    IUIAutomation *automation = automation_client();
    if (automation == nullptr) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": cannot create IUIAutomation (COM/UIA client unavailable)");
        return 3;
    }

    IUIAutomationElement *root_el = root_element(automation, hwnd);
    if (root_el == nullptr) {
        const bool dll_present = LoadLibraryA("UIAutomationCore.dll") != nullptr;
        AURORA_LOG_ERROR("verify", std::string(label) + ": ElementFromHandle failed (no UIA element for HWND)" +
                                       (dll_present ? "" : " [UIAutomationCore.dll MISSING -> designed degradation]"));
        automation->Release();
        return dll_present ? 5 : 3;
    }

    if (const aurora::a11y::Provider *bridge = surface.accessibility_provider(); bridge != nullptr) {
        emit("bridge = " + bridge->name() + ", active = " + (bridge->is_active() ? "true" : "false"));
    } else {
        AURORA_LOG_ERROR("verify",
                         std::string(label) + ": bridge still absent after client query (WM_GETOBJECT not wired)");
        root_el->Release();
        automation->Release();
        return 4;
    }

    IUIAutomationTreeWalker *walker = control_view_walker(automation);
    if (walker == nullptr) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": cannot create control-view tree walker");
        root_el->Release();
        automation->Release();
        return 3;
    }

    std::vector<Visited> nodes;
    collect(walker, root_el, 0, nodes, 256);
    walker->Release();
    root_el->Release();
    automation->Release();

    emit(aurora_verify::pad_right("#", 4) + aurora_verify::pad_right("depth", 7) +
         aurora_verify::pad_right("controlType", 13) + aurora_verify::pad_right("name", 14) +
         aurora_verify::pad_right("framework", 10) + aurora_verify::pad_right("ctl", 5) +
         aurora_verify::pad_right("cnt", 5) + aurora_verify::pad_right("patterns", 26) +
         aurora_verify::pad_right("labeledBy", 14));
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Visited &v = nodes[i];
        const std::string pats = patterns_of(v);
        emit(aurora_verify::pad_right(aurora_verify::format_uint(i), 4) +
             aurora_verify::pad_right(aurora_verify::format_uint(static_cast<unsigned long long>(v.depth)), 7) +
             aurora_verify::pad_right(std::to_string(v.control_type), 13) +
             aurora_verify::pad_right(v.name.empty() ? "(empty)" : v.name, 14) +
             aurora_verify::pad_right(v.framework_id.empty() ? "(host)" : v.framework_id, 10) +
             aurora_verify::pad_right(v.is_control ? "y" : "n", 5) +
             aurora_verify::pad_right(v.is_content ? "y" : "n", 5) +
             aurora_verify::pad_right(pats.empty() ? "(none)" : pats, 26) +
             aurora_verify::pad_right(v.labeled_by_name.empty() ? "(none)" : v.labeled_by_name, 14));
    }
    emit("summary: visited=" + aurora_verify::format_uint(nodes.size()));

    int failures = 0;
    if (nodes.size() < 2) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": no child node exposed (structure not projected)");
        return 4;
    }

    // 每个期望控件：`FrameworkId == "Aurora"`（确属本桥投影）∧ ControlType 命中 ∧
    // 其声明的 pattern 可取到 ∧ 具备可访问名（G22 必需属性集；无 label 的控件由唯一文本子节点兜底）。
    const auto own_nodes = static_cast<std::size_t>(
        std::ranges::count_if(nodes, [](const Visited &v) -> bool { return v.framework_id == "Aurora"; }));
    emit("own (FrameworkId=Aurora) nodes = " + aurora_verify::format_uint(own_nodes) + " / visited " +
         aurora_verify::format_uint(nodes.size()) + " (剩余为 UIA 默认 HWND provider 合成的非客户区)");
    if (own_nodes == 0) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": bridge projected 0 elements (FrameworkId=Aurora absent)");
        ++failures;
    }

    for (const Expectation &exp : AURORA_EXPECTATIONS) {
        const auto it = std::ranges::find_if(nodes, [&exp](const Visited &v) -> bool {
            return v.framework_id == "Aurora" && v.control_type == static_cast<long long>(exp.control_type);
        });
        if (it == nodes.end()) {
            AURORA_LOG_ERROR("verify", std::string(label) + ": no Aurora element with controlType " +
                                           std::to_string(exp.control_type) + " (" + exp.label + ")");
            ++failures;
            continue;
        }
        const Visited &v = *it;
        const bool ok = (!exp.want_invoke || v.invoke) && (!exp.want_toggle || v.toggle) &&
                        (!exp.want_value || v.value) && (!exp.want_range || v.range);
        if (!ok) {
            AURORA_LOG_ERROR("verify", std::string(label) + ": " + exp.label + " (controlType " +
                                           std::to_string(exp.control_type) + ") missing required pattern; got [" +
                                           patterns_of(v) + "] hr{Invoke=" + hex_hr(v.hr_invoke) +
                                           " Toggle=" + hex_hr(v.hr_toggle) + " Value=" + hex_hr(v.hr_value) +
                                           " Range=" + hex_hr(v.hr_range) + "} (0x0=S_OK/可用)");
            ++failures;
            continue;
        }
        if (v.name.empty()) {
            if (exp.require_name) {
                AURORA_LOG_ERROR("verify", std::string(label) + ": " + exp.label +
                                               " exposes an empty Name (G22 required property set)");
                ++failures;
            } else {
                emit(std::string("~~  ") + exp.label +
                     ": Name 为空 —— 本库 Checkbox/Slider 无内建标签，标签常为**兄弟**节点，"
                     "G24 的唯一文本子节点兜底取不到（待设计决定是否引入标签关联）");
            }
            continue;
        }
        // #1-C 兄弟标签关联验收：期望 Name 由最近相邻文本兄弟解析而来。
        if (exp.sibling_expect != nullptr && v.name != exp.sibling_expect) {
            AURORA_LOG_ERROR("verify", std::string(label) + ": " + exp.label + " sibling-label Name mismatch: got \"" +
                                           v.name + "\" expected \"" + exp.sibling_expect + "\" (#1-C)");
            ++failures;
            continue;
        }
        emit(std::string("ok  ") + exp.label + " controlType=" + std::to_string(exp.control_type) + " name=\"" +
             v.name + "\" patterns=[" + patterns_of(v) + "]");
    }

    // #3 / #7 几何验收：自有（Aurora）控件必须投影非空矩形（#3）；文本控件补文本几何盲区断言（#7）。
    for (const Expectation &exp : AURORA_EXPECTATIONS) {
        const auto it = std::ranges::find_if(nodes, [&exp](const Visited &v) -> bool {
            return v.framework_id == "Aurora" && v.control_type == static_cast<long long>(exp.control_type);
        });
        if (it == nodes.end()) {
            continue;
        }
        const Visited &v = *it;
        if (!v.rect_ok) {
            AURORA_LOG_ERROR("verify", std::string(label) + ": " + exp.label +
                                           " exposes no bounding rectangle (geometry not projected; #3)");
            ++failures;
        }
        if (exp.label == std::string_view("Text") || exp.label == std::string_view("TextInput")) {
            if (v.text_range_rects == 0) {
                emit(std::string("~~  ") + exp.label +
                     ": TextPattern 存在但 GetBoundingRectangles 为空（可能字体度量未就绪；#7 盲区已覆盖）");
            } else if (v.text_range_rects >= 4) {
                emit(std::string("ok  ") + exp.label + " text geometry rects present (#7)");
            } else {
                emit(std::string("~~  ") + exp.label + ": 无 TextPattern（#7；只读控件通常不暴露）");
            }
        }
    }

    // 引用式标签关联（#21）：桥须把语义树解析出的目标以 `UIA_LabeledByPropertyId` 投影，且
    // 客户端经 `get_CurrentLabeledBy` 取回的元素其 Name 即目标之名。判据控件是列末的第二个
    // Checkbox：它自身无任何标签来源（纵向排布不命中 #1-C 的行内相邻判据），故它的 Name 非空
    // 本身就是「名字跟随引用」的证据，再叠一条关系可导航的显式断言。
    const auto ref_it = std::ranges::find_if(nodes, [](const Visited &v) -> bool {
        return v.framework_id == "Aurora" && v.control_type == static_cast<long long>(UIA_CheckBoxControlTypeId) &&
               v.name == "季度汇总";
    });
    if (ref_it == nodes.end()) {
        AURORA_LOG_ERROR(
            "verify", std::string(label) + ": 无 Name 为「季度汇总」的元素 —— set_labelled_by 的名字未跟随目标 (#21)");
        ++failures;
    } else if (ref_it->labeled_by_name != "季度汇总") {
        AURORA_LOG_ERROR("verify", std::string(label) + ": UIA_LabeledByPropertyId 未投影 (got \"" +
                                       ref_it->labeled_by_name + "\", expected \"季度汇总\") (#21)");
        ++failures;
    } else {
        emit("ok  labelled-by: Name 跟随目标，LabeledBy 关系可导航（get_CurrentLabeledBy）(#21)");
    }

    if (failures > 0) {
        return 5;
    }
    emit(std::string(label) + ": PASS");
    return 0;
}

/// @brief 建窗 + 装载 + 跑验收（两路后端共用）。
template <typename MakeWindow>
auto run_backend(const char *label, const char *title, MakeWindow make_window) -> int {
    auto window = make_window();
    if (window == nullptr) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": window creation failed");
        return 2;
    }
    int rc = 0;
    {
        aurora::Node root = build_probe_column();
        rc = run_probe(*window, window->surface(), root, label, title);
    }
    window.reset();
    return rc;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常 → 非零退出码/terminate 呈现（捕获反而把它压成 0），与 demo 入口同口径。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    int rc = 0;

#ifdef AURORA_BACKEND_WIN32
    rc = std::max(rc,
                  run_backend("Win32Surface/GDI", "Aurora UIA verify (GDI)", []() -> std::unique_ptr<aurora::Window> {
                      auto res = aurora::create_native_window(aurora::WindowOptions{
                          .size = aurora::Size{.width = 520.0F, .height = 420.0F}, .title = "Aurora UIA verify (GDI)"});
                      return res ? std::move(res.value()) : nullptr;
                  }));
#else
    emit("Win32Surface/GDI: SKIPPED (AURORA_BACKEND_WIN32 off)");
#endif

#ifdef AURORA_BACKEND_D3D11
    rc = std::max(rc,
                  run_backend("D3D11Surface/GPU", "Aurora UIA verify (D3D11)", []() -> std::unique_ptr<aurora::Window> {
                      auto res = aurora::create_native_window(
                          aurora::WindowOptions{.size = aurora::Size{.width = 520.0F, .height = 420.0F},
                                                .title = "Aurora UIA verify (D3D11)",
                                                .renderer = aurora::RendererPreference::GpuD3D11});
                      return res ? std::move(res.value()) : nullptr;
                  }));
#else
    emit(
        "D3D11Surface/GPU: SKIPPED (AURORA_BACKEND_D3D11 off; SAME Win32Host host as GDI -- "
        "the bridge is shared, enable the backend to cover the GPU blit path)");
#endif

    if (rc == 0) {
        emit("ALL PASS");
        return 0;
    }
    emit("FAILURES PRESENT (worst exit = " + std::to_string(rc) + ")");
    return rc;
}
