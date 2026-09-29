#include "aurora/window/detail/win32_ua.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// clang-format off
#include <oleauto.h>
// clang-format on

#include "aurora/core/diagnostics.h"
#include "aurora/core/version.h"
#include "aurora/event/dispatcher.h"
#include "aurora/state/async.h"

// 本 TU 是 Windows UI Automation 的 COM 桥接层：值的构造与解析必须直接操作 SDK 的
// VARIANT union、BSTR(OLECHAR*) 与 HWND/HMONITOR 句柄，这正是下列 Core Guidelines
// 检查要禁止的写法，但在 ABI 边界上无法按建议改写（换成 std::variant 会与
// UIAutomationCore 的接口契约脱钩）。故在本命名空间内整块具名抑制，范围仅限四类：
//   - cppcoreguidelines-pro-type-union-access：VARIANT 各分量的读写
//   - cppcoreguidelines-pro-type-reinterpret-cast：BSTR / 句柄与整型互转
//   - cppcoreguidelines-pro-type-static-cast-downcast：QueryInterface 后的接口下行
//   - cppcoreguidelines-pro-type-const-cast：COM 方法多为 const 形参，桥接层需去掉 const 才能调用
//   - cppcoreguidelines-pro-bounds-constant-array-index：SAFEARRAY 元素取值
//   - cppcoreguidelines-special-member-functions / cppcoreguidelines-virtual-class-destructor：
//     各 provider 是 COM 引用计数对象，只能经 QueryInterface/Release 取得与释放（IUnknown
//     契约禁止按值拷贝/移动），且析构遵循 COM 惯例为 protected virtual —— 五法则与
//     「公开非虚析构」在此均不适用。
// NOLINTBEGIN(*-pro-bounds-*, *-pro-type-*, *-special-member-functions, *-virtual-class-destructor)

namespace aurora::detail {

// UIA 布尔出/入参：MSVC SDK 的 UIAutomationCore.h 用 BOOL，MinGW-w64 的 IDL 映射用 WINBOOL；
// 两者均为 int，别名后 override 签名在两侧工具链同时精确匹配。
#ifdef __MINGW32__
using UiaBool = WINBOOL;
#else
using UiaBool = BOOL;
#endif

namespace {

// ---- VARIANT / BSTR / SAFEARRAY 辅助（桥边界的 COM 值构造）----

[[nodiscard]] auto bstr_from_utf8(const std::string &utf8) -> BSTR {
    const std::u16string wide = a11y::utf8_to_utf16(utf8);
    return SysAllocStringLen(reinterpret_cast<const OLECHAR *>(wide.data()), static_cast<UINT>(wide.size()));
}

auto variant_init_bool(VARIANT &v, bool b) -> void {
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

auto variant_init_i4(VARIANT &v, long x) -> void {
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = x;
}

auto variant_init_double(VARIANT &v, double x) -> void {
    VariantInit(&v);
    v.vt = VT_R8;
    v.dblVal = x;
}

auto variant_init_bstr(VARIANT &v, const std::string &utf8) -> void {
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = bstr_from_utf8(utf8);  // 调用方负责 VariantClear
}

/// @brief 对象引用型属性值（`UIA_LabeledByPropertyId` 等）：VARIANT 自己持一份 COM 引用。
/// @note `bridge_->provider_for()` 交还的是**缓存借用**（不增计数），故此处必须 AddRef：
///       出参的那一份归 UIA 客户端，由其 `VariantClear` 释放。
/// @note 元素引用属性必须用 **`VT_UNKNOWN`** 承载 `IRawElementProviderSimple*`（不是 `VT_DISPATCH`）：
///       实测同一条 `UIA_LabeledByPropertyId` 在 `VT_DISPATCH` 下客户端 `get_CurrentLabeledBy`
///       只拿回空元素（UIA 按 `IUnknown` 问回 provider，不走 Dispatch 路径），改 `VT_UNKNOWN` 后
///       关系可导航 —— 判据见 `tools/verify/win32_ua_live_probe.cpp` 的 #21 验收项。
auto variant_init_provider(VARIANT &v, IRawElementProviderSimple *p) -> void {
    VariantInit(&v);
    v.vt = VT_UNKNOWN;
    v.punkVal = p;
    if (p != nullptr) {
        p->AddRef();
    }
}

/// @brief 属性「不支持」的保留 VARIANT：必须是 UIA 保留值而非 VT_EMPTY。
auto variant_not_supported(VARIANT &v) -> void {
    VariantInit(&v);
    IUnknown *reserved = nullptr;
    if (UiaApi::instance().get_reserved_not_supported != nullptr &&
        SUCCEEDED(UiaApi::instance().get_reserved_not_supported(&reserved)) && reserved != nullptr) {
        v.vt = VT_UNKNOWN;
        v.punkVal = reserved;  // 所有权转交 VARIANT（调用方 VariantClear）
        return;
    }
    v.vt = VT_EMPTY;  // 动态加载失败时的兜底
}

/// @brief 把「主人线程动作」的三态结果映射成 UIA 的 HRESULT。
///
/// `nullopt`（元素已被摘出快照 / 回投超时）→ `UIA_E_ELEMENTNOTAVAILABLE`，与旧「widget 取不到」
/// 同判；`false`（控件不认这个动作）→ `UIA_E_NOTSUPPORTED`；`true` → `S_OK`。
/// 超时不走异常、不打诊断的取舍见 `wait_on_main` 上方注释。
auto hr_of(const std::optional<bool> &performed) -> HRESULT {
    if (!performed.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    return *performed ? S_OK : UIA_E_NOTSUPPORTED;
}

/// @brief 主人线程一次**读**的三态结果。
///
/// 为何不能只用 `std::optional`：UIA 把「元素已不存在」（`UIA_E_ELEMENTNOTAVAILABLE`，客户端
/// 应丢弃该元素）与「控件没有这项语义」（`UIA_E_NOTSUPPORTED`，客户端只是不显示该属性）当成两件
/// 事，而回投把两者都表达成「没拿到值」。默认构造即 `Unavailable`，所以超时/降级不必另写分支。
template <typename T>
struct MainRead {
    enum class State : std::uint8_t { Unavailable, Unsupported, Value };
    State state = State::Unavailable;
    T value{};

    [[nodiscard]] static auto value_of(T v) -> MainRead<T> {
        return MainRead<T>{.state = State::Value, .value = std::move(v)};
    }
    [[nodiscard]] static auto unsupported() -> MainRead<T> { return MainRead<T>{.state = State::Unsupported}; }
};

/// @brief 三态读结果 → HRESULT（不动出参）。
template <typename T>
auto hr_of(const MainRead<T> &r) -> HRESULT {
    if (r.state == MainRead<T>::State::Value) {
        return S_OK;
    }
    return r.state == MainRead<T>::State::Unsupported ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
}

/// @brief 三态读结果 → 写 UIA 出参 + HRESULT。
template <typename T>
auto store_read(const MainRead<T> &r, T *ret) -> HRESULT {
    if (r.state == MainRead<T>::State::Value) {
        *ret = r.value;
        return S_OK;
    }
    return r.state == MainRead<T>::State::Unsupported ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
}

/// @brief 在主人线程对本节点的真实 widget 施加 `fn`，结果**按值**带回调用线程。
///
/// 三个 provider 类（节点 / 文本区间 / 文本图案）共用这一份回投实现：`bridge == nullptr`、
/// 「节点已摘出快照」与「回投超时」都归一成 `R{}`，由调用方按 `hr_of` / `store_read` 的口径
/// 映射成 UIA 错误码。`fn` 的返回值不得含指针或引用 —— 只有副本跨得来。
template <typename R>
[[nodiscard]] auto read_on_main(Win32UiaBridge *bridge, std::uint64_t id, std::function<R(Widget &)> fn,
                                unsigned long timeout_ms) -> R {
    if (bridge == nullptr) {
        return R{};
    }
    return bridge->eval_widget_on_main<R>(id, std::move(fn), timeout_ms);
}

[[nodiscard]] auto runtime_id_array(HWND hwnd, std::uint64_t id) -> SAFEARRAY * {
    // 形态（设计 §7.2）：[UiaAppendRuntimeId, hwnd_lo, hwnd_hi, id_lo, id_hi] —— 窗口内唯一且跨进程稳定。
    const auto hw = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
    int values[5] = {UiaAppendRuntimeId, static_cast<int>(hw & 0xFFFFFFFFULL),
                     static_cast<int>((hw >> 32U) & 0xFFFFFFFFULL), static_cast<int>(id & 0xFFFFFFFFULL),
                     static_cast<int>((id >> 32U) & 0xFFFFFFFFULL)};
    SAFEARRAY *arr = SafeArrayCreateVector(VT_I4, 0, 5);
    if (arr == nullptr) {
        return nullptr;
    }
    for (LONG i = 0; i < 5; ++i) {
        SafeArrayPutElement(arr, &i, &values[i]);
    }
    return arr;
}

/// @brief role → UIA ControlType（设计 §7.3 映射表）。
[[nodiscard]] auto control_type_of(AccessibilityRole role) -> CONTROLTYPEID {
    switch (role) {
        case AccessibilityRole::Button:
            return UIA_ButtonControlTypeId;
        case AccessibilityRole::Text:
            return UIA_TextControlTypeId;
        case AccessibilityRole::TextInput:
            return UIA_EditControlTypeId;
        case AccessibilityRole::Checkbox:
        case AccessibilityRole::Switch:
            return UIA_CheckBoxControlTypeId;
        case AccessibilityRole::Slider:
            return UIA_SliderControlTypeId;
        case AccessibilityRole::Image:
            return UIA_ImageControlTypeId;
        case AccessibilityRole::List:
            return UIA_ListControlTypeId;
        case AccessibilityRole::ListItem:
            return UIA_ListItemControlTypeId;
        case AccessibilityRole::Header:
            return UIA_HeaderControlTypeId;
        case AccessibilityRole::Progress:
            return UIA_ProgressBarControlTypeId;
        case AccessibilityRole::Dialog:
            return UIA_WindowControlTypeId;
        case AccessibilityRole::Generic:
        default:
            return UIA_PaneControlTypeId;
    }
}

/// @brief UIA 文本单位枚举 → 共享 `a11y::TextUnit`。
[[nodiscard]] auto to_shared_unit(enum TextUnit unit) -> a11y::TextUnit {
    switch (unit) {
        case TextUnit_Character:
            return a11y::TextUnit::Character;
        case TextUnit_Word:
            return a11y::TextUnit::Word;
        case TextUnit_Line:
            return a11y::TextUnit::Line;
        case TextUnit_Document:
            return a11y::TextUnit::Document;
        default:
            return a11y::TextUnit::Character;
    }
}

/// @brief `IRawElementProviderSimple*` → `IRawElementProviderFragment*`（经 QueryInterface，
///        返回时已 +1，所有权归调用方）。
[[nodiscard]] auto as_fragment(IRawElementProviderSimple *simple) -> IRawElementProviderFragment * {
    if (simple == nullptr) {
        return nullptr;
    }
    IRawElementProviderFragment *frag = nullptr;
    const HRESULT hr = simple->QueryInterface(IID_IRawElementProviderFragment, reinterpret_cast<void **>(&frag));
    return SUCCEEDED(hr) ? frag : nullptr;
}

/// @brief 当前线程是否已初始化 COM 公寓：未初始化时 UIA 回调线程不确定。
[[nodiscard]] auto ensure_sta_apartment() -> bool {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        // S_FALSE：本线程此前已 CoInitialize（Aurora 主线程常见）——无需配对 CoUninitialize。
        return true;
    }
    return hr == RPC_E_CHANGED_MODE;  // 已以 MTA 初始化：UIA 在 MTA 下亦可工作，仅失去 STA 保证
}

}  // namespace

// ============================================================================
// UiaApi：UIAutomationCore.dll 动态加载
// ============================================================================

auto UiaApi::instance() -> const UiaApi & {
    static const UiaApi UIA_API = [] {
        UiaApi a;
        a.loaded = false;
        HMODULE dll = LoadLibraryA("UIAutomationCore.dll");
        if (dll == nullptr) {
            return a;
        }
        // GetProcAddress 恒返 FARPROC（无参函数指针），向带参签名 reinterpret_cast 是
        // Win32 动态加载的标准写法——GCC 的 -Wcast-function-type 对此属误报，本地压制。
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        a.return_raw_element_provider = reinterpret_cast<decltype(a.return_raw_element_provider)>(
            GetProcAddress(dll, "UiaReturnRawElementProvider"));
        a.raise_automation_event =
            reinterpret_cast<decltype(a.raise_automation_event)>(GetProcAddress(dll, "UiaRaiseAutomationEvent"));
        a.raise_property_changed = reinterpret_cast<decltype(a.raise_property_changed)>(
            GetProcAddress(dll, "UiaRaiseAutomationPropertyChangedEvent"));
        a.raise_structure_changed =
            reinterpret_cast<decltype(a.raise_structure_changed)>(GetProcAddress(dll, "UiaRaiseStructureChangedEvent"));
        a.disconnect_provider =
            reinterpret_cast<decltype(a.disconnect_provider)>(GetProcAddress(dll, "UiaDisconnectProvider"));
        a.host_provider_from_hwnd =
            reinterpret_cast<decltype(a.host_provider_from_hwnd)>(GetProcAddress(dll, "UiaHostProviderFromHwnd"));
        a.get_reserved_not_supported = reinterpret_cast<decltype(a.get_reserved_not_supported)>(
            GetProcAddress(dll, "UiaGetReservedNotSupportedValue"));
        // 可选：较老 Windows 上不存在，缺失时播报回退 LiveRegionChanged。
        a.raise_notification_event =
            reinterpret_cast<decltype(a.raise_notification_event)>(GetProcAddress(dll, "UiaRaiseNotificationEvent"));
        a.loaded = a.return_raw_element_provider != nullptr && a.raise_automation_event != nullptr &&
                   a.raise_property_changed != nullptr && a.raise_structure_changed != nullptr &&
                   a.disconnect_provider != nullptr && a.host_provider_from_hwnd != nullptr &&
                   a.get_reserved_not_supported != nullptr;
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic pop
#endif
        return a;
    }();
    return UIA_API;
}

// ============================================================================
// UiaNodeProvider：单个语义节点的 provider（fragment + 全 pattern）
// ============================================================================

/// @brief 语义节点 provider：实现 fragment 全方法与各 pattern。
///
/// 跨线程纪律：只持**桥裸指针 + 节点 id**，不持 `Widget*`。UIA 把每次回调投递在
/// `UIAutomationCore` 的 COM/RPC 线程上，而 widget 树 / `snap_` / `providers_` 属主人线程，故
/// 属性与几何读的是 `node()` 返回的快照**副本**，动作与活树读写一律经 `read_on_main` 回投。
/// 未命中（元素已摘出快照）或回投超时即返回 `UIA_E_ELEMENTNOTAVAILABLE`（§4.1 安全不变量 / R1）。
class UiaNodeProvider : public IRawElementProviderSimple,
                        public IRawElementProviderFragment,
                        public IInvokeProvider,
                        public IToggleProvider,
                        public IValueProvider,
                        public IRangeValueProvider,
                        public IScrollProvider,
                        public IScrollItemProvider {
  public:
    UiaNodeProvider(Win32UiaBridge *bridge, std::uint64_t id) : bridge_(bridge), id_(id) {}

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IRawElementProviderSimple) {
            *ppv = static_cast<IRawElementProviderSimple *>(this);
        } else if (riid == IID_IRawElementProviderFragment) {
            *ppv = static_cast<IRawElementProviderFragment *>(this);
        } else if (riid == IID_IInvokeProvider) {
            *ppv = static_cast<IInvokeProvider *>(this);
        } else if (riid == IID_IToggleProvider) {
            *ppv = static_cast<IToggleProvider *>(this);
        } else if (riid == IID_IValueProvider) {
            *ppv = static_cast<IValueProvider *>(this);
        } else if (riid == IID_IRangeValueProvider) {
            *ppv = static_cast<IRangeValueProvider *>(this);
        } else if (riid == IID_IScrollProvider) {
            *ppv = static_cast<IScrollProvider *>(this);
        } else if (riid == IID_IScrollItemProvider) {
            *ppv = static_cast<IScrollItemProvider *>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            delete this;
        }
        return r;
    }
    /// @brief 引用计数探针（回收判据）：COM 没有只读计数口，只能临时借一份再还回去窥。
    /// @return 除本次临时借还之外的既有持有者数。桥在 `providers_` 里恒持一份 ⇒ 下界为 1，
    ///         故探针期间计数不可能归零。
    /// @note 这里直拨 `ref_` 而不调 `AddRef()/Release()`：后者含「归零即 `delete this`」，
    ///       让静态分析无法证明借还之后本对象仍活着（clang-analyzer-cplusplus.NewDelete）。
    ///       两者对 `ref_` 的效果同为 +1/-1（均为 `Interlocked` 系列），语义不变。
    [[nodiscard]] auto holder_count() -> ULONG {
        const ULONG after_add = InterlockedIncrement(&ref_);
        (void)InterlockedDecrement(&ref_);
        return after_add - 1U;
    }

    // ---- IRawElementProviderSimple ----
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(enum ProviderOptions *ret) override {
        if (ret == nullptr) {
            return E_POINTER;
        }
        *ret = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern_id, IUnknown **ret) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property_id, VARIANT *ret) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple **ret) override {
        if (ret == nullptr) {
            return E_POINTER;
        }
        *ret = nullptr;  // 非根 fragment：宿主由根提供
        return S_OK;
    }

    // ---- IRawElementProviderFragment（6 个方法全实现）----
    HRESULT STDMETHODCALLTYPE Navigate(enum NavigateDirection direction, IRawElementProviderFragment **ret) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY **ret) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(struct UiaRect *ret) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY **ret) override {
        if (ret == nullptr) {
            return E_POINTER;
        }
        *ret = nullptr;  // 无嵌入根
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot **ret) override;

    // ---- IInvokeProvider ----
    HRESULT STDMETHODCALLTYPE Invoke() override;

    // ---- IToggleProvider ----
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(enum ToggleState *ret) override;

    // ---- IValueProvider ----
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR val) override;
    HRESULT STDMETHODCALLTYPE get_Value(BSTR *ret) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(UiaBool *ret) override;

    // ---- IRangeValueProvider ----
    //
    // 注意：`get_IsReadOnly(UiaBool*)` 在 IValueProvider 与 IRangeValueProvider 中**签名完全相同**，
    // 重复声明会构成重复成员函数（编译错误）——一份声明同时满足两个基接口的覆盖，故此处不重复写。
    HRESULT STDMETHODCALLTYPE SetValue(double val) override;
    HRESULT STDMETHODCALLTYPE get_Value(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_Maximum(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_Minimum(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_LargeChange(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_SmallChange(double *ret) override;

    // ---- IScrollProvider ----
    HRESULT STDMETHODCALLTYPE Scroll(enum ScrollAmount horizontal, enum ScrollAmount vertical) override;
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontal_percent, double vertical_percent) override;
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double *ret) override;
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(UiaBool *ret) override;
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(UiaBool *ret) override;

    // ---- IScrollItemProvider ----
    HRESULT STDMETHODCALLTYPE ScrollIntoView() override;

    [[nodiscard]] auto id() const -> std::uint64_t { return id_; }

  protected:
    // COM 基接口（IUnknown…）在 MinGW SDK 下析构器非 virtual，故此处不可写 `override`。
    virtual ~UiaNodeProvider() = default;

    /// @brief 取当前节点的快照**副本**（内部回投主人线程）；`nullopt` = 控件已销毁 / 超时。
    [[nodiscard]] auto node() const -> std::optional<a11y::NodeSnapshot>;

    /// @brief 在主人线程对本节点的真实 widget 施加 `fn`，结果**按值**带回。
    ///
    /// 这里刻意**没有** `widget()` 可用：活 `Widget*` 一旦离开主人线程就是本次缺陷的本体
    /// ——快照会被整体换掉、widget 会被摘除。一切读与动都经本口 hop 过去。
    /// `fn` 的返回值不应含指针/引用（只有副本回得来）；`R{}` 即「不可得」判据。
    template <typename R>
    [[nodiscard]] auto on_main(std::function<R(Widget &)> fn, unsigned long timeout_ms) const -> R {
        return read_on_main<R>(bridge_, id_, std::move(fn), timeout_ms);
    }

    /// @brief 动作统一入口：主人线程解析 widget 后调 `perform_accessibility_action`。
    /// @return `true` = 控件已执行；`false` = 控件不认该动作；`nullopt` = 元素不可得 / 超时。
    /// 入参一律按值携带：`AccessibilityActionRequest::text` 是 `string_view`，绝不引用调用方
    /// 栈上的串（超时后那次执行会晚于栈帧消失）。
    [[nodiscard]] auto perform_on_main(AccessibilityAction action, double number, std::string text) const
        -> std::optional<bool>;

    Win32UiaBridge *bridge_ = nullptr;
    std::uint64_t id_ = 0;
    ULONG ref_ = 1;
};

// ============================================================================
// UiaRootProvider：根 provider（fragment root + advise events）
// ============================================================================

class UiaRootProvider : public UiaNodeProvider,
                        public IRawElementProviderFragmentRoot,
                        public IRawElementProviderAdviseEvents {
  public:
    UiaRootProvider(Win32UiaBridge *bridge, std::uint64_t id) : UiaNodeProvider(bridge, id) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IRawElementProviderFragmentRoot) {
            *ppv = static_cast<IRawElementProviderFragmentRoot *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IRawElementProviderAdviseEvents) {
            *ppv = static_cast<IRawElementProviderAdviseEvents *>(this);
            AddRef();
            return S_OK;
        }
        return UiaNodeProvider::QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return UiaNodeProvider::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return UiaNodeProvider::Release(); }

    // 根：宿主 provider 由 hwnd 派生——免费获得窗口级属性与 IsOffscreen 判定。
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple **ret) override;

    // ---- IRawElementProviderFragmentRoot ----
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment **ret) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment **ret) override;

    // ---- IRawElementProviderAdviseEvents ----
    HRESULT STDMETHODCALLTYPE AdviseEventAdded(EVENTID /*event_id*/, SAFEARRAY * /*props*/) override {
        if (bridge_ != nullptr) {
            bridge_->note_listener_added();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AdviseEventRemoved(EVENTID /*event_id*/, SAFEARRAY * /*props*/) override {
        if (bridge_ != nullptr) {
            bridge_->note_listener_removed();
        }
        return S_OK;
    }

  protected:
    ~UiaRootProvider() override = default;
};

// ============================================================================
// UiaTextRangeProvider / UiaTextProvider
// ============================================================================

/// @brief 文本区间 provider：内部以 **UTF-8 字节偏移**保存端点，UIA 边界用 `UtfOffsetMap` 换算。
///
/// 线程模型与 `UiaNodeProvider` 同纪律：只持**桥裸指针 + 节点 id**，对 widget 的每一次
/// 读写都经 `read_on_main` 回投主人线程、只取按值副本回来；`start_`/`end_` 是本 provider 的私有
/// 状态，留在 UIA 调用线程上改，不涉及共享数据。
class UiaTextRangeProvider : public ITextRangeProvider {
  public:
    UiaTextRangeProvider(Win32UiaBridge *bridge, std::uint64_t id, std::size_t start, std::size_t end)
        : bridge_(bridge), id_(id), start_(start), end_(end) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITextRangeProvider) {
            *ppv = static_cast<ITextRangeProvider *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            delete this;
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider **ret) override;
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider *range, UiaBool *ret) override;
    HRESULT STDMETHODCALLTYPE CompareEndpoints(enum TextPatternRangeEndpoint endpoint, ITextRangeProvider *target,
                                               enum TextPatternRangeEndpoint target_endpoint, int *ret) override;
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(enum TextUnit unit) override;
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID /*attribute_id*/, VARIANT /*value*/,
                                            UiaBool /*not_supported*/, ITextRangeProvider ** /*ret*/) override {
        return E_NOTIMPL;  // NVDA 不依赖（设计 §7.4 允许的合法降级）
    }
    HRESULT STDMETHODCALLTYPE FindText(BSTR /*text*/, UiaBool /*backward*/, UiaBool /*ignore_case*/,
                                       ITextRangeProvider ** /*ret*/) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID attribute_id, VARIANT *ret) override;
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY **ret) override;
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple **ret) override;
    HRESULT STDMETHODCALLTYPE GetText(int max_length, BSTR *ret) override;
    HRESULT STDMETHODCALLTYPE Move(enum TextUnit unit, int count, int *ret) override;
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(enum TextPatternRangeEndpoint endpoint, enum TextUnit unit, int count,
                                                 int *ret) override;
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(enum TextPatternRangeEndpoint endpoint, ITextRangeProvider *target,
                                                  enum TextPatternRangeEndpoint target_endpoint) override;
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(UiaBool align_to_top) override;
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY **ret) override;

    [[nodiscard]] auto start() const -> std::size_t { return start_; }
    [[nodiscard]] auto end() const -> std::size_t { return end_; }

  protected:
    virtual ~UiaTextRangeProvider() = default;

    /// @brief 当前文本（回投主人线程取 `Widget::accessibility_text()` 的**副本**）。
    /// `nullopt` = 控件已销毁 / 回投超时 → 调用方回 `UIA_E_ELEMENTNOTAVAILABLE`。
    [[nodiscard]] auto current_text() const -> std::optional<std::string>;
    /// @brief 把端点夹紧到 `text` 的长度（入参由调用方取回，避免每步都回投一次）。
    auto clamp_to_text(const std::string &text) -> void;

    Win32UiaBridge *bridge_ = nullptr;
    std::uint64_t id_ = 0;
    std::size_t start_ = 0;
    std::size_t end_ = 0;
    ULONG ref_ = 1;
};

/// @brief TextPattern provider（每文本节点一个；`ITextProvider` 六方法全实现）。
///
/// 线程模型同 `UiaTextRangeProvider`：不持 `Widget*`，对 widget 的读写一律经 `read_on_main`
/// 回投主人线程，跨线程只传副本。
class UiaTextProvider : public ITextProvider {
  public:
    UiaTextProvider(Win32UiaBridge *bridge, std::uint64_t id) : bridge_(bridge), id_(id) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITextProvider) {
            *ppv = static_cast<ITextProvider *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            delete this;
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY **ret) override;
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY **ret) override;
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple * /*child*/, ITextRangeProvider **ret) override {
        if (ret != nullptr) {
            *ret = nullptr;
        }
        return E_NOTIMPL;  // Aurora 无嵌入对象，规范允许如此
    }
    HRESULT STDMETHODCALLTYPE RangeFromPoint(struct UiaPoint point, ITextRangeProvider **ret) override;
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider **ret) override;
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(enum SupportedTextSelection *ret) override {
        if (ret == nullptr) {
            return E_POINTER;
        }
        *ret = SupportedTextSelection_Single;
        return S_OK;
    }

  protected:
    virtual ~UiaTextProvider() = default;

    Win32UiaBridge *bridge_ = nullptr;
    std::uint64_t id_ = 0;
    ULONG ref_ = 1;
};

// ============================================================================
// UiaNodeProvider：实现
// ============================================================================

auto UiaNodeProvider::node() const -> std::optional<a11y::NodeSnapshot> {
    if (bridge_ == nullptr) {
        return std::nullopt;
    }
    // 拉取式同步点与跨线程纪律都在 `snapshot_copy` 内：回投主人线程重建，
    // 再把该节点**按值**带回。此后本 provider 只读自己这份副本 —— 主线程随后怎么换快照、
    // 怎么摘 widget，都不会让这里的读数悬垂（旧实现返回指向 `snap_` 的裸指针，正是缺陷本体）。
    return bridge_->snapshot_copy(id_);
}

auto UiaNodeProvider::perform_on_main(AccessibilityAction action, double number, std::string text) const
    -> std::optional<bool> {
    return on_main<std::optional<bool>>(
        [action, number, text = std::move(text)](Widget &w) -> std::optional<bool> {
            const AccessibilityActionRequest req{.action = action, .number = number, .text = text};
            return w.perform_accessibility_action(req);
        },
        AURORA_UI_ACTION_TIMEOUT_MS);
}

auto UiaNodeProvider::GetPatternProvider(PATTERNID pattern_id, IUnknown **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    const auto n = node();
    if (!n.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const AccessibilityNode &an = n->node;
    switch (pattern_id) {
        case UIA_InvokePatternId:
            if (an.has_action(AccessibilityAction::Invoke) || an.has_action(AccessibilityAction::Click)) {
                *ret = static_cast<IInvokeProvider *>(this);
                AddRef();
            }
            break;
        case UIA_TogglePatternId:
            if (an.state.checkable) {
                *ret = static_cast<IToggleProvider *>(this);
                AddRef();
            }
            break;
        case UIA_ValuePatternId:
            if (an.role == AccessibilityRole::TextInput || an.has_action(AccessibilityAction::Value)) {
                *ret = static_cast<IValueProvider *>(this);
                AddRef();
            }
            break;
        case UIA_RangeValuePatternId:
            if (an.range.has_value()) {
                *ret = static_cast<IRangeValueProvider *>(this);
                AddRef();
            }
            break;
        case UIA_ScrollPatternId:
            if (an.has_action(AccessibilityAction::ScrollUp) || an.has_action(AccessibilityAction::ScrollDown)) {
                *ret = static_cast<IScrollProvider *>(this);
                AddRef();
            }
            break;
        case UIA_ScrollItemPatternId:
            if (bridge_ != nullptr && bridge_->has_scrollable_ancestor(id_)) {
                *ret = static_cast<IScrollItemProvider *>(this);
                AddRef();
            }
            break;
        case UIA_TextPatternId:
            if (an.role == AccessibilityRole::TextInput || an.role == AccessibilityRole::Text) {
                // 文本 provider 无需跨调用保同一性：按需构造、引用计数自管。
                auto *text = new UiaTextProvider(bridge_, id_);
                *ret = static_cast<ITextProvider *>(text);  // 初始引用计数 1 交予 UIA
            }
            break;
        default:
            break;
    }
    return S_OK;
}

auto UiaNodeProvider::GetPropertyValue(PROPERTYID property_id, VARIANT *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value()) {
        variant_not_supported(*ret);
        return S_OK;  // 元素已销毁：不支持而非失败
    }
    const AccessibilityNode &an = n->node;
    switch (property_id) {
        case UIA_ControlTypePropertyId:
            variant_init_i4(*ret, static_cast<long>(control_type_of(an.role)));
            return S_OK;
        case UIA_NamePropertyId:
            variant_init_bstr(*ret, an.name);
            return S_OK;
        case UIA_LabeledByPropertyId:
            // 引用式标签关联（对标 ARIA `aria-labelledby`）：`labelled_by_id` 由语义树解析得出，
            // 非 0 即代表「名字确实跟随该目标」（空名目标不投影，见 `apply_labelled_by_relations`）。
            if (an.labelled_by_id != 0) {
                if (auto *target = bridge_->provider_for(an.labelled_by_id); target != nullptr) {
                    variant_init_provider(*ret, target);
                    return S_OK;
                }
            }
            break;
        case UIA_HelpTextPropertyId:
            variant_init_bstr(*ret, an.hint);
            return S_OK;
        case UIA_AutomationIdPropertyId:
            variant_init_bstr(*ret, std::to_string(an.id));
            return S_OK;
        case UIA_FrameworkIdPropertyId:
            variant_init_bstr(*ret, "Aurora");
            return S_OK;
        case UIA_ProviderDescriptionPropertyId:
            variant_init_bstr(*ret, std::string{"Aurora "} + AURORA_VERSION_STRING);
            return S_OK;
        case UIA_IsControlElementPropertyId:
            variant_init_bool(*ret, an.is_control);
            return S_OK;
        case UIA_IsContentElementPropertyId:
            variant_init_bool(*ret, an.is_content);
            return S_OK;
        case UIA_IsEnabledPropertyId:
            variant_init_bool(*ret, !an.state.disabled);
            return S_OK;
        case UIA_IsOffscreenPropertyId:
            variant_init_bool(*ret, an.state.offscreen);
            return S_OK;
        case UIA_IsKeyboardFocusablePropertyId:
            variant_init_bool(*ret, an.state.focusable);
            return S_OK;
        case UIA_HasKeyboardFocusPropertyId:
            variant_init_bool(*ret, an.state.focused);
            return S_OK;
        case UIA_IsPasswordPropertyId:
            variant_init_bool(*ret, an.state.password);
            return S_OK;
        case UIA_IsDialogPropertyId:
            variant_init_bool(*ret, an.role == AccessibilityRole::Dialog);
            return S_OK;
        case UIA_LiveSettingPropertyId:
            variant_init_i4(*ret, 1);  // LiveSetting::Polite（播报语义）
            return S_OK;
        case UIA_NativeWindowHandlePropertyId:
            variant_init_i4(*ret, 0);  // 非根：无窗口句柄（根在 UiaRootProvider 覆写）
            return S_OK;
        case UIA_ValueValuePropertyId:
            if (an.role == AccessibilityRole::TextInput || !an.value.empty()) {
                variant_init_bstr(*ret, an.value);
                return S_OK;
            }
            break;
        case UIA_ToggleToggleStatePropertyId:
            if (an.state.checkable) {
                variant_init_i4(*ret, an.state.checked ? ToggleState_On : ToggleState_Off);
                return S_OK;
            }
            break;
        case UIA_RangeValueValuePropertyId:
            if (an.range.has_value()) {
                variant_init_double(*ret, an.range->value);
                return S_OK;
            }
            break;
        case UIA_RangeValueMinimumPropertyId:
            if (an.range.has_value()) {
                variant_init_double(*ret, an.range->min);
                return S_OK;
            }
            break;
        case UIA_RangeValueMaximumPropertyId:
            if (an.range.has_value()) {
                variant_init_double(*ret, an.range->max);
                return S_OK;
            }
            break;
        case UIA_RangeValueSmallChangePropertyId:
        case UIA_RangeValueLargeChangePropertyId:
            if (an.range.has_value() && an.range->step > 0.0) {
                variant_init_double(*ret, an.range->step);
                return S_OK;
            }
            break;
        default:
            break;
    }
    variant_not_supported(*ret);
    return S_OK;
}

auto UiaNodeProvider::Navigate(enum NavigateDirection direction, IRawElementProviderFragment **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    // 方向语义一次翻成 NavigationKind（RTL 与 children_of 的解析都留在主人线程做）。
    NavigationKind kind{};
    switch (direction) {
        case NavigateDirection_Parent:
            kind = NavigationKind::Parent;
            break;
        case NavigateDirection_FirstChild:
            kind = NavigationKind::FirstChild;
            break;
        case NavigateDirection_LastChild:
            kind = NavigationKind::LastChild;
            break;
        case NavigateDirection_NextSibling:
            kind = NavigationKind::NextSibling;
            break;
        case NavigateDirection_PreviousSibling:
            kind = NavigationKind::PreviousSibling;
            break;
        default:
            return S_OK;  // 未知方向：无目标，非失败
    }
    if (bridge_ == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const auto target = bridge_->navigate_target(id_, kind);
    if (!target.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;  // 本节点已被摘出快照（与旧实现同判）
    }
    if (*target == 0) {
        return S_OK;  // 该方向没有邻居（根无父，由宿主窗口 provider 承载）
    }
    // 缓存借用 → 交还前补一次引用（口径与旧 `as_fragment` + `AddRef` 一致）。
    IRawElementProviderFragment *frag = as_fragment(bridge_->provider_for(*target));
    if (frag != nullptr) {
        frag->AddRef();
    }
    *ret = frag;
    return S_OK;
}

auto UiaNodeProvider::GetRuntimeId(SAFEARRAY **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = runtime_id_array(bridge_ != nullptr ? bridge_->hwnd() : nullptr, id_);
    return (*ret != nullptr) ? S_OK : E_OUTOFMEMORY;
}

auto UiaNodeProvider::get_BoundingRectangle(struct UiaRect *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value() || bridge_ == nullptr) {
        *ret = UiaRect{.left = 0, .top = 0, .width = 0, .height = 0};
        return S_OK;
    }
    // #3：离屏元素返回空矩形（与 IsOffscreen 属性一致），避免读屏取到屏幕外元素的几何。
    if (n->node.state.offscreen) {
        *ret = UiaRect{.left = 0, .top = 0, .width = 0, .height = 0};
        return S_OK;
    }
    // #3：部分出窗的元素裁剪到窗口可见盒（根几何，窗口本地 DIP），保留可见部分。
    Rect b = n->node.bounds;
    const Rect vis = bridge_->visible_box();
    if (vis.size.width > 0.0F && vis.size.height > 0.0F) {
        const double l = std::max(static_cast<double>(b.origin.x), static_cast<double>(vis.origin.x));
        const double t = std::max(static_cast<double>(b.origin.y), static_cast<double>(vis.origin.y));
        const double r = std::min(static_cast<double>(b.origin.x) + b.size.width,
                                  static_cast<double>(vis.origin.x) + vis.size.width);
        const double bo = std::min(static_cast<double>(b.origin.y) + b.size.height,
                                   static_cast<double>(vis.origin.y) + vis.size.height);
        if (r <= l || bo <= t) {
            *ret = UiaRect{.left = 0, .top = 0, .width = 0, .height = 0};
            return S_OK;
        }
        b = Rect{.origin = Point{.x = static_cast<float>(l), .y = static_cast<float>(t)},
                 .size = Size{.width = static_cast<float>(r - l), .height = static_cast<float>(bo - t)}};
    }
    *ret = bridge_->to_physical(b);
    return S_OK;
}

auto UiaNodeProvider::SetFocus() -> HRESULT { return hr_of(perform_on_main(AccessibilityAction::Focus, 0.0, {})); }

auto UiaNodeProvider::get_FragmentRoot(IRawElementProviderFragmentRoot **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    if (bridge_ == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    IRawElementProviderSimple *root = bridge_->root_provider();
    if (root == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    return root->QueryInterface(IID_IRawElementProviderFragmentRoot, reinterpret_cast<void **>(ret));
}

auto UiaNodeProvider::Invoke() -> HRESULT { return hr_of(perform_on_main(AccessibilityAction::Invoke, 0.0, {})); }

auto UiaNodeProvider::Toggle() -> HRESULT { return hr_of(perform_on_main(AccessibilityAction::Toggle, 0.0, {})); }

auto UiaNodeProvider::get_ToggleState(enum ToggleState *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = n->node.state.checked ? ToggleState_On : ToggleState_Off;
    return S_OK;
}

auto UiaNodeProvider::SetValue(LPCWSTR val) -> HRESULT {
    if (val == nullptr) {
        return E_POINTER;
    }
    // `req.text` 是 string_view，串本身必须在执行那一刻仍存活 —— 回投协议里由
    // `perform_on_main` 按值搬进闭包承担（超时后晚到的执行也读得到，读的是闭包而非栈帧）。
    const std::string utf8 = a11y::utf16_to_utf8(std::u16string_view{reinterpret_cast<const char16_t *>(val)});
    return hr_of(perform_on_main(AccessibilityAction::Value, 0.0, utf8));
}

auto UiaNodeProvider::get_Value(BSTR *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = bstr_from_utf8(n->node.value);
    return (*ret != nullptr) ? S_OK : E_OUTOFMEMORY;
}

auto UiaNodeProvider::get_IsReadOnly(UiaBool *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = n->node.state.read_only ? TRUE : FALSE;
    return S_OK;
}

auto UiaNodeProvider::SetValue(double val) -> HRESULT {
    return hr_of(perform_on_main(AccessibilityAction::Value, val, {}));
}

auto UiaNodeProvider::get_Value(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value() || !n->node.range.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = n->node.range->value;
    return S_OK;
}

auto UiaNodeProvider::get_Maximum(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value() || !n->node.range.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = n->node.range->max;
    return S_OK;
}

auto UiaNodeProvider::get_Minimum(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value() || !n->node.range.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = n->node.range->min;
    return S_OK;
}

auto UiaNodeProvider::get_LargeChange(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto n = node();
    if (!n.has_value() || !n->node.range.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = n->node.range->step > 0.0 ? n->node.range->step : 0.0;
    return S_OK;
}

auto UiaNodeProvider::get_SmallChange(double *ret) -> HRESULT { return get_LargeChange(ret); }

auto UiaNodeProvider::Scroll(enum ScrollAmount horizontal, enum ScrollAmount vertical) -> HRESULT {
    // 只处理垂直/水平四个方向量（`NoAmount` 忽略）；一屏即 Large*，其余按小步。
    std::vector<AccessibilityAction> actions;
    if (vertical == ScrollAmount_LargeIncrement || vertical == ScrollAmount_SmallIncrement) {
        actions.push_back(AccessibilityAction::ScrollDown);
    } else if (vertical == ScrollAmount_LargeDecrement || vertical == ScrollAmount_SmallDecrement) {
        actions.push_back(AccessibilityAction::ScrollUp);
    }
    if (horizontal == ScrollAmount_LargeIncrement || horizontal == ScrollAmount_SmallIncrement) {
        actions.push_back(AccessibilityAction::ScrollRight);
    } else if (horizontal == ScrollAmount_LargeDecrement || horizontal == ScrollAmount_SmallDecrement) {
        actions.push_back(AccessibilityAction::ScrollLeft);
    }
    if (actions.empty()) {
        return S_OK;
    }
    // 四个方向量在**同一次**主人线程 hop 内顺序执行：拆成多次回投会让「一次滚动」横跨两个
    // 快照，且任一动作被拒即整体判不支持（与旧 `ok = perform && ok` 逐位一致）。
    return hr_of(on_main<std::optional<bool>>(
        [actions](Widget &w) -> std::optional<bool> {
            bool ok = true;
            for (const AccessibilityAction a : actions) {
                const AccessibilityActionRequest req{.action = a};
                ok = w.perform_accessibility_action(req) && ok;
            }
            return ok;
        },
        AURORA_UI_ACTION_TIMEOUT_MS));
}

auto UiaNodeProvider::SetScrollPercent(double horizontal_percent, double vertical_percent) -> HRESULT {
    // 读滚动跨度与写偏移合进同一次主人线程求值（旧实现先取 widget，再两次经它读写）。
    // 水平量忽略（Aurora 当前无横向滚动语义位）；-1 为 UIA 的「保持不变」约定。
    return hr_of(on_main<MainRead<bool>>(
        [horizontal_percent, vertical_percent](Widget &w) -> MainRead<bool> {
            const auto scroll = w.accessibility_scroll();
            if (!scroll.has_value()) {
                return MainRead<bool>::unsupported();
            }
            const double span = scroll->max - scroll->min;
            if (span <= 0.0 || (vertical_percent < 0.0 && horizontal_percent < 0.0)) {
                return MainRead<bool>::value_of(true);  // 无跨度 / 「保持不变」：no-op 即成功
            }
            const double target = scroll->min + ((std::clamp(vertical_percent, 0.0, 100.0) / 100.0) * span);
            w.accessibility_scroll_to(target);
            return MainRead<bool>::value_of(true);
        },
        AURORA_UI_ACTION_TIMEOUT_MS));
}

auto UiaNodeProvider::get_HorizontalScrollPercent(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = -1.0;  // UIA 约定：不可滚动返回 -1
    return S_OK;
}

auto UiaNodeProvider::get_VerticalScrollPercent(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    return store_read(on_main<MainRead<double>>(
                          [](Widget &w) -> MainRead<double> {
                              const auto scroll = w.accessibility_scroll();
                              if (!scroll.has_value()) {
                                  return MainRead<double>::unsupported();
                              }
                              const double span = scroll->max - scroll->min;
                              return MainRead<double>::value_of(
                                  span <= 0.0 ? 0.0 : ((scroll->position - scroll->min) / span) * 100.0);
                          },
                          AURORA_UI_READ_TIMEOUT_MS),
                      ret);
}

auto UiaNodeProvider::get_HorizontalViewSize(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = 100.0;  // 无横向滚动：视口即全部内容
    return S_OK;
}

auto UiaNodeProvider::get_VerticalViewSize(double *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    return store_read(
        on_main<MainRead<double>>(
            [](Widget &w) -> MainRead<double> {
                const auto scroll = w.accessibility_scroll();
                if (!scroll.has_value()) {
                    return MainRead<double>::unsupported();
                }
                const double total = scroll->max;  // 可滚动量（内容 − 视口）
                return MainRead<double>::value_of(
                    total <= 0.0 ? 100.0 : std::clamp((scroll->max / (scroll->max + 1.0)) * 100.0, 0.0, 100.0));
            },
            AURORA_UI_READ_TIMEOUT_MS),
        ret);
}

auto UiaNodeProvider::get_HorizontallyScrollable(UiaBool *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = FALSE;
    return S_OK;
}

auto UiaNodeProvider::get_VerticallyScrollable(UiaBool *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    return store_read(on_main<MainRead<UiaBool>>(
                          [](Widget &w) -> MainRead<UiaBool> {
                              const auto scroll = w.accessibility_scroll();
                              const bool can = scroll.has_value() && scroll->max > scroll->min;
                              return MainRead<UiaBool>::value_of(can ? TRUE : FALSE);
                          },
                          AURORA_UI_READ_TIMEOUT_MS),
                      ret);
}

auto UiaNodeProvider::ScrollIntoView() -> HRESULT {
    return hr_of(perform_on_main(AccessibilityAction::ScrollIntoView, 0.0, {}));
}

// ============================================================================
// UiaRootProvider：实现
// ============================================================================

auto UiaRootProvider::get_HostRawElementProvider(IRawElementProviderSimple **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    if (bridge_ == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    IRawElementProviderSimple *host = bridge_->host_provider();
    if (host == nullptr) {
        return S_OK;
    }
    host->AddRef();
    *ret = host;
    return S_OK;
}

auto UiaRootProvider::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    if (bridge_ == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    // 物理屏幕像素 → 窗口本地 DIP（反换算），再走与派发器同口径的命中测试。
    const std::uint64_t id = bridge_->hit_test_id(bridge_->to_local(x, y));
    if (id == 0) {
        return S_OK;
    }
    IRawElementProviderSimple *p = bridge_->provider_for(id);
    if (p == nullptr) {
        return S_OK;
    }
    const HRESULT hr = p->QueryInterface(IID_IRawElementProviderFragment, reinterpret_cast<void **>(ret));
    return hr;
}

auto UiaRootProvider::GetFocus(IRawElementProviderFragment **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    if (bridge_ == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const std::uint64_t id = bridge_->focused_id();
    if (id == 0) {
        return S_OK;
    }
    IRawElementProviderSimple *p = bridge_->provider_for(id);
    if (p == nullptr) {
        return S_OK;
    }
    return p->QueryInterface(IID_IRawElementProviderFragment, reinterpret_cast<void **>(ret));
}

// ============================================================================
// UiaTextRangeProvider：实现
// ============================================================================

auto UiaTextRangeProvider::current_text() const -> std::optional<std::string> {
    return read_on_main<std::optional<std::string>>(
        bridge_, id_,
        [](Widget &w) -> std::optional<std::string> {
            return std::string{w.accessibility_text()};  // 副本：出不了主人线程的裸 widget 不外流
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto UiaTextRangeProvider::clamp_to_text(const std::string &text) -> void {
    start_ = std::min(start_, text.size());
    end_ = std::clamp(end_, start_, text.size());
}

auto UiaTextRangeProvider::Clone(ITextRangeProvider **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = new UiaTextRangeProvider(bridge_, id_, start_, end_);
    return S_OK;
}

auto UiaTextRangeProvider::Compare(ITextRangeProvider *range, UiaBool *ret) -> HRESULT {
    if (ret == nullptr || range == nullptr) {
        return E_POINTER;
    }
    // 本项目以 -fno-rtti 构建；本 provider 只产出 UiaTextRangeProvider，故静态下转即可
    // （跨实现的 range 不属本 provider 的契约范围）。
    const auto *other = static_cast<const UiaTextRangeProvider *>(range);
    *ret = (other->start_ == start_ && other->end_ == end_ && other->id_ == id_) ? TRUE : FALSE;
    return S_OK;
}

auto UiaTextRangeProvider::CompareEndpoints(enum TextPatternRangeEndpoint endpoint, ITextRangeProvider *target,
                                            enum TextPatternRangeEndpoint target_endpoint, int *ret) -> HRESULT {
    if (ret == nullptr || target == nullptr) {
        return E_POINTER;
    }
    const auto *other = static_cast<const UiaTextRangeProvider *>(target);
    const long self = endpoint == TextPatternRangeEndpoint_Start ? static_cast<long>(start_) : static_cast<long>(end_);
    const long other_v = target_endpoint == TextPatternRangeEndpoint_Start ? static_cast<long>(other->start_)
                                                                           : static_cast<long>(other->end_);
    *ret = static_cast<int>(self - other_v);
    return S_OK;
}

auto UiaTextRangeProvider::ExpandToEnclosingUnit(enum TextUnit unit) -> HRESULT {
    const auto text = current_text();
    if (!text.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    clamp_to_text(*text);
    const auto [s, e] = a11y::expand_to_unit(*text, start_, to_shared_unit(unit));
    start_ = s;
    end_ = e;
    return S_OK;
}

auto UiaTextRangeProvider::GetAttributeValue(TEXTATTRIBUTEID attribute_id, VARIANT *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    // 只答只读属性（设计 §7.4 的有限集），其余返回 UIA 保留「不支持」值。
    if (attribute_id == UIA_IsReadOnlyAttributeId) {
        const auto ro = read_on_main<std::optional<bool>>(
            bridge_, id_, [](Widget &w) -> std::optional<bool> { return w.accessibility_state().read_only; },
            AURORA_UI_READ_TIMEOUT_MS);
        if (!ro.has_value()) {
            return UIA_E_ELEMENTNOTAVAILABLE;  // 控件已销毁 / 超时
        }
        variant_init_bool(*ret, *ro);
        return S_OK;
    }
    variant_not_supported(*ret);
    return S_OK;
}

auto UiaTextRangeProvider::GetBoundingRectangles(SAFEARRAY **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    const auto text = current_text();
    if (!text.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    clamp_to_text(*text);
    if (start_ >= end_) {
        *ret = SafeArrayCreateVector(VT_R8, 0, 0);  // 空区间：空数组
        return S_OK;
    }
    // 逐码点取字符盒（无字体度量时 `char_bounds` 返回 nullopt ⇒ 该字符跳过）。整趟遍历收进
    // **一次**回投：跨线程只带窗口本地 DIP 盒的副本回来。
    const auto boxes = read_on_main<std::optional<std::vector<Rect>>>(
        bridge_, id_,
        [s = start_, e = end_, text = *text](Widget &w) -> std::optional<std::vector<Rect>> {
            std::vector<Rect> out;
            for (std::size_t i = s; i < e;) {
                const auto box = w.accessibility_char_bounds(i);
                const auto [unused_cp, len] = a11y::detail::decode_cp(text, i);
                (void)unused_cp;
                const std::size_t adv = (len == 0) ? 1 : len;
                if (box.has_value()) {
                    out.push_back(*box);
                }
                i += adv;
            }
            return out;
        },
        AURORA_UI_READ_TIMEOUT_MS);
    if (!boxes.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    // DIP → 物理屏幕像素：`to_physical` 只读 Win32 几何 API，在 UIA 调用线程换算即可。
    std::vector<double> rects;
    rects.reserve(boxes->size() * 4U);
    for (const Rect &box : *boxes) {
        const UiaRect r = bridge_->to_physical(box);
        rects.push_back(static_cast<double>(r.left));
        rects.push_back(static_cast<double>(r.top));
        rects.push_back(static_cast<double>(r.width));
        rects.push_back(static_cast<double>(r.height));
    }
    SAFEARRAY *arr = SafeArrayCreateVector(VT_R8, 0, static_cast<ULONG>(rects.size()));
    if (arr == nullptr) {
        return E_OUTOFMEMORY;
    }
    // 循环上界先落到 LONG 常量：把 static_cast 直接写在条件里会被
    // modernize-use-integer-sign-comparison 判为有符号/无符号混比（size_t → LONG）。
    const LONG rect_count = static_cast<LONG>(rects.size());
    for (LONG i = 0; i < rect_count; ++i) {
        SafeArrayPutElement(arr, &i, &rects[static_cast<std::size_t>(i)]);
    }
    *ret = arr;
    return S_OK;
}

auto UiaTextRangeProvider::GetEnclosingElement(IRawElementProviderSimple **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    if (bridge_ == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    IRawElementProviderSimple *p = bridge_->provider_for(id_);
    if (p == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    p->AddRef();
    *ret = p;
    return S_OK;
}

auto UiaTextRangeProvider::GetText(int max_length, BSTR *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    const auto text = current_text();
    if (!text.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    clamp_to_text(*text);
    std::string slice = text->substr(start_, end_ - start_);
    if (max_length >= 0 && slice.size() > static_cast<std::size_t>(max_length)) {
        slice.resize(static_cast<std::size_t>(max_length));
    }
    *ret = bstr_from_utf8(slice);
    return (*ret != nullptr) ? S_OK : E_OUTOFMEMORY;
}

auto UiaTextRangeProvider::Move(enum TextUnit unit, int count, int *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = 0;
    if (count == 0) {
        return S_OK;
    }
    const auto text_copy = current_text();
    if (!text_copy.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const std::string &text = *text_copy;
    clamp_to_text(text);
    const std::size_t len = end_ - start_;
    int moved = 0;
    const int step = count > 0 ? 1 : -1;
    for (int i = 0; i < std::abs(count); ++i) {
        const auto kind = to_shared_unit(unit);
        // 两分支互斥且已全覆盖，故直接以 const 初始化求值：先声明后赋值会同时触发
        // cppcoreguidelines-init-variables（未初始化）与 deadcode.DeadStores（初始化值从未被读取）。
        const std::size_t next = [&]() -> std::size_t {
            if (kind == a11y::TextUnit::Character) {
                return (step > 0) ? a11y::UtfOffsetMap{text}.advance_utf8(end_, 1)  // 端点前进：整体右移
                                  : a11y::UtfOffsetMap{text}.advance_utf8(start_, -1);
            }
            const auto [s, e] = a11y::expand_to_unit(text, start_, kind);
            return (step > 0) ? e : (s == 0 ? 0 : a11y::expand_to_unit(text, s - 1, kind).first);
        }();
        if (next == start_) {
            break;  // 已到边界
        }
        start_ = next;
        end_ = std::min(start_ + len, text.size());
        moved += step;
    }
    *ret = moved;
    return S_OK;
}

auto UiaTextRangeProvider::MoveEndpointByUnit(enum TextPatternRangeEndpoint endpoint, enum TextUnit unit, int count,
                                              int *ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = 0;
    const auto text_copy = current_text();
    if (!text_copy.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const std::string &text = *text_copy;
    clamp_to_text(text);
    a11y::UtfOffsetMap map{text};
    int moved = 0;
    const int step = count > 0 ? 1 : -1;
    for (int i = 0; i < std::abs(count); ++i) {
        const bool is_start = endpoint == TextPatternRangeEndpoint_Start;
        std::size_t &edge_ref = is_start ? start_ : end_;
        // 同 MoveEndpointByUnit：分支互斥，直接以 const 初始化，避免未初始化告警与「初始化值未被读取」。
        const std::size_t next = [&]() -> std::size_t {
            if (unit == TextUnit_Character) {
                // UTF-16 半代理索引由 `advance_utf16` 向下夹紧到码点起点。
                return map.advance_utf16(map.to_utf16(edge_ref), step);
            }
            const auto [s, e] = a11y::expand_to_unit(text, edge_ref, to_shared_unit(unit));
            return (step > 0) ? e : (s == 0 ? 0 : a11y::expand_to_unit(text, s - 1, to_shared_unit(unit)).first);
        }();
        if (next == edge_ref) {
            break;
        }
        edge_ref = next;
        moved += step;
    }
    end_ = std::max(end_, start_);
    *ret = moved;
    return S_OK;
}

auto UiaTextRangeProvider::MoveEndpointByRange(enum TextPatternRangeEndpoint endpoint, ITextRangeProvider *target,
                                               enum TextPatternRangeEndpoint target_endpoint) -> HRESULT {
    if (target == nullptr) {
        return E_POINTER;
    }
    const auto *other = static_cast<const UiaTextRangeProvider *>(target);
    const std::size_t value = target_endpoint == TextPatternRangeEndpoint_Start ? other->start_ : other->end_;
    if (endpoint == TextPatternRangeEndpoint_Start) {
        start_ = value;
        end_ = std::max(end_, start_);
    } else {
        end_ = value;
        start_ = std::min(start_, end_);
    }
    const auto text = current_text();
    if (!text.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    clamp_to_text(*text);
    return S_OK;
}

auto UiaTextRangeProvider::Select() -> HRESULT {
    // 夹紧与落选在一次回投里做完：端点是本地状态，文本长度只在主人线程可知。
    return hr_of(read_on_main<std::optional<bool>>(
        bridge_, id_,
        [s = start_, e = end_](Widget &w) -> std::optional<bool> {
            const std::string text{w.accessibility_text()};
            const std::size_t start = std::min(s, text.size());
            const std::size_t end = std::clamp(e, start, text.size());
            w.accessibility_set_selection(start, end);
            return true;
        },
        AURORA_UI_ACTION_TIMEOUT_MS));
}

auto UiaTextRangeProvider::ScrollIntoView(UiaBool align_to_top) -> HRESULT {
    const auto performed = read_on_main<std::optional<bool>>(
        bridge_, id_,
        [](Widget &w) -> std::optional<bool> {
            const AccessibilityActionRequest req{.action = AccessibilityAction::ScrollIntoView};
            return w.perform_accessibility_action(req);
        },
        AURORA_UI_ACTION_TIMEOUT_MS);
    if (!performed.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    // false = 无滚动祖先可承接：降级 no-op 而非报错（NVDA 会频繁调用）。
    (void)align_to_top;
    return S_OK;
}

auto UiaTextRangeProvider::GetChildren(SAFEARRAY **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    // Aurora 无嵌入对象：返回空数组（而非 E_NOTIMPL）。
    *ret = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
    return (*ret != nullptr) ? S_OK : E_OUTOFMEMORY;
}

// ============================================================================
// UiaTextProvider：实现
// ============================================================================

auto UiaTextProvider::GetSelection(SAFEARRAY **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    // 选区与文本长度都只在主人线程可知：一次回投带回夹紧后的端点副本。
    const auto span = read_on_main<std::optional<std::pair<std::size_t, std::size_t>>>(
        bridge_, id_,
        [](Widget &w) -> std::optional<std::pair<std::size_t, std::size_t>> {
            const std::string text{w.accessibility_text()};
            std::size_t s = 0;
            std::size_t e = 0;
            if (const auto sel = w.accessibility_selection(); sel.has_value()) {
                s = std::min(sel->start, text.size());
                e = std::clamp(sel->end, s, text.size());
            }
            return std::make_pair(s, e);
        },
        AURORA_UI_READ_TIMEOUT_MS);
    if (!span.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    SAFEARRAY *arr = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
    if (arr == nullptr) {
        return E_OUTOFMEMORY;
    }
    auto *range = static_cast<ITextRangeProvider *>(new UiaTextRangeProvider(bridge_, id_, span->first, span->second));
    LONG index = 0;
    SafeArrayPutElement(arr, &index, static_cast<void *>(range));
    *ret = arr;
    return S_OK;
}

auto UiaTextProvider::GetVisibleRanges(SAFEARRAY **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    // 退化为单 range（全文）：与 `DocumentRange` 同（设计 §7.4 允许的降级）。
    ITextRangeProvider *range = nullptr;
    const HRESULT hr = get_DocumentRange(&range);
    if (FAILED(hr)) {
        return hr;
    }
    if (range == nullptr) {
        return E_FAIL;
    }
    SAFEARRAY *arr = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
    if (arr == nullptr) {
        range->Release();  // 数组没建成，这次 new 出来的引用得自己收
        return E_OUTOFMEMORY;
    }
    LONG index = 0;
    SafeArrayPutElement(arr, &index, static_cast<void *>(range));  // 数组接管该引用
    *ret = arr;
    return S_OK;
}

auto UiaTextProvider::RangeFromPoint(struct UiaPoint point, ITextRangeProvider **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    // 物理 → 本地 DIP 只读 Win32 几何（任意线程可调）；随后的「按最近字符反查」整趟收进
    // 一次回投，跨线程只带一个字节偏移回来。
    const Point local = (bridge_ != nullptr) ? bridge_->to_local(point.x, point.y) : Point{};
    const auto offset = read_on_main<std::optional<std::size_t>>(
        bridge_, id_,
        [local](Widget &w) -> std::optional<std::size_t> {
            const std::string text{w.accessibility_text()};
            if (text.empty()) {
                return 0;
            }
            std::size_t best = text.size();  // 绘制盒退化 / 字符盒全不可得：保持旧实现的「落到文末」
            const Rect box = w.paint_bounds();
            if (box.size.width > 0.0F && box.size.height > 0.0F) {
                float best_dist = std::numeric_limits<float>::max();
                for (std::size_t i = 0; i < text.size();) {
                    const auto cb = w.accessibility_char_bounds(i);
                    const auto [unused_cp, len] = a11y::detail::decode_cp(text, i);
                    (void)unused_cp;
                    if (cb.has_value()) {
                        const float dx = (cb->origin.x + (cb->size.width * 0.5F)) - local.x;
                        const float dy = (cb->origin.y + (cb->size.height * 0.5F)) - local.y;
                        const float d = std::abs(dx) + std::abs(dy);
                        if (d < best_dist) {
                            best_dist = d;
                            best = i;
                        }
                    }
                    i += (len == 0) ? 1 : len;
                }
            }
            return best;
        },
        AURORA_UI_READ_TIMEOUT_MS);
    if (!offset.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = new UiaTextRangeProvider(bridge_, id_, *offset, *offset);
    return S_OK;
}

auto UiaTextProvider::get_DocumentRange(ITextRangeProvider **ret) -> HRESULT {
    if (ret == nullptr) {
        return E_POINTER;
    }
    *ret = nullptr;
    const auto size = read_on_main<std::optional<std::size_t>>(
        bridge_, id_, [](Widget &w) -> std::optional<std::size_t> { return w.accessibility_text().size(); },
        AURORA_UI_READ_TIMEOUT_MS);
    if (!size.has_value()) {
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    *ret = new UiaTextRangeProvider(bridge_, id_, 0, *size);
    return S_OK;
}

// ============================================================================
// Win32UiaBridge：实现
// ============================================================================

Win32UiaBridge::Win32UiaBridge(HWND hwnd) : hwnd_(hwnd) {}

// ============================================================================
// 跨线程回投
// ============================================================================
// UIA 把 provider 的每一次回调都投递在 UIAutomationCore 的 COM/RPC 线程上，而 widget 树、
// `snap_`、`providers_` 只属于主人线程。故本桥对 provider 暴露的每一个访问口都经
// `eval_on_main` _hop_ 过去、只把**按值副本**带回调用线程。降级策略见头文件
// `eval_on_main` / `wait_on_main` 的文档：限时等待，超时返回零值，由调用方映射为
// `UIA_E_ELEMENTNOTAVAILABLE`。
//
// 超时路径刻意**不**打诊断：`Diagnostics` 的收集器是主人线程专属（其头注明「单线程 UI，
// 无需加锁」），而超时必须先在 RPC 线程返回——在 RPC 线程写诊断等于用一个新的数据 race
// 去换一个日志。可观测性改由客户端看到的 `UIA_E_ELEMENTNOTAVAILABLE` 承担。

auto Win32UiaBridge::on_owner_thread() const -> bool { return std::this_thread::get_id() == owner_thread_; }

namespace {

/// @brief 就地执行 `work`，异常不得外越 COM 边界（转成 false 让调用方按零值降级）。
[[nodiscard]] auto run_guarded(const std::function<void()> &work) -> bool {
    try {
        work();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

auto Win32UiaBridge::wait_on_main(std::function<void()> work, const std::shared_ptr<std::atomic<bool>> &abandoned,
                                  unsigned long timeout_ms) -> bool {
    // 就地路径①：调用线程就是主人线程。宿主接线（`WM_GETOBJECT`、帧循环、事件派发）与
    // `UiaDisconnectProvider` 的同步重入都落在这里 —— 若入队，主人线程会等自己排空队列，
    // 必然耗满超时，故直接跑。
    if (on_owner_thread()) {
        return run_guarded(work);
    }
    std::function<void(std::function<void()>)> poster;
    {
        std::scoped_lock lock(detail::main_poster_mutex());
        poster = detail::main_poster();
    }
    // 就地路径②：进程级回投器未安装（无头 / 单测 / 事件循环未起）——与 `post_to_main` 的
    // 无 poster 语义一致（裁定③）。
    if (!poster) {
        return run_guarded(work);
    }
    auto done = std::make_shared<std::promise<bool>>();
    auto ready = done->get_future();
    auto alive = alive_;
    const auto owner = owner_thread_;  // 按值捕获可拷贝的线程 id（`std::thread` 本体不可拷贝）
    poster([work = std::move(work), done, abandoned, alive, owner]() -> void {
        bool ran = false;
        // 队列项执行前的三道闸，任一不过即不碰树（详见头文件文档）。判主线程放在最前：
        // 它是唯一与 `this` 无关的判据（只比已按值捕获的 thread id）。
        if (std::this_thread::get_id() == owner && alive->load(std::memory_order_acquire) &&
            !abandoned->load(std::memory_order_acquire)) {
            ran = run_guarded(work);
        }
        try {
            done->set_value(ran);
        } catch (const std::future_error &) {
            // 值已设过 ⇒ 同一队列项被排了两次水（无合法路径）。不回抛：抛出会打断主人线程
            // 的帧循环，而调用方拿到的永远是第一次那一份答案，问题得留在诊断里而非炸在帧里。
            Diagnostics::warn("Win32UiaBridge: accessibility main-thread task signalled twice",
                              "Win32UiaBridge::wait_on_main", "a11y-task-drained-twice");
        }
    });
    if (ready.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        return false;  // 主线程未在预算内排空：调用方降级，晚到的项被放弃闸拦住
    }
    return ready.get();
}

auto Win32UiaBridge::widget_on_main(std::uint64_t id) -> Widget * {
    // 只在 `eval_*_on_main` 的闭包内到达（主人线程）：先消费脏位（拉取式同步点），
    // 再把快照里的裸 widget 交出去 —— 该指针的有效期止于本次回投闭包，不得离开主人线程。
    sync_if_dirty();
    const auto *n = find_node(id);
    return (n != nullptr) ? const_cast<Widget *>(n->widget) : nullptr;
}

auto Win32UiaBridge::snapshot_copy(std::uint64_t id) -> std::optional<a11y::NodeSnapshot> {
    return eval_on_main<std::optional<a11y::NodeSnapshot>>(
        [this, id]() -> std::optional<a11y::NodeSnapshot> {
            sync_if_dirty();
            const auto *n = find_node(id);
            if (n == nullptr) {
                return std::nullopt;
            }
            // 按值跨线程：调用方持有副本，之后的重建都不会影响它。
            auto copy = *n;
            // 活指针随副本出境就是跨线程回投要堵的本体缺陷，故一律抹掉：碰活 widget 只许走
            // `eval_widget_on_main`（在那里 `widget_on_main` 于主人线程解析，指针不出线程）。
            copy.widget = nullptr;
            return copy;
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::navigate_target(std::uint64_t id, NavigationKind kind) -> std::optional<std::uint64_t> {
    // `nullopt` = 本节点已不在快照（或超时）→ 调用方回 `UIA_E_ELEMENTNOTAVAILABLE`；
    // 值 `0` = 该方向无目标 → 调用方回 `S_OK` + 空出参。两态合在一次回投里取回，
    // 免得树遍历的热路上每步跳两次线程。
    return eval_on_main<std::optional<std::uint64_t>>(
        [this, id, kind]() -> std::optional<std::uint64_t> {
            sync_if_dirty();
            const auto *n = find_node(id);
            if (n == nullptr) {
                return std::nullopt;
            }
            const auto children_of = [this](std::uint64_t parent) -> const std::vector<std::uint64_t> * {
                const auto it = snap_.children_of.find(parent);
                return (it == snap_.children_of.end()) ? nullptr : &it->second;
            };
            // #5：RTL 下逻辑首/末与「下一个=视觉左、上一个=视觉右」，与旧实现逐位一致。
            const bool rtl = rtl_.load(std::memory_order_relaxed);
            switch (kind) {
                case NavigationKind::Parent:
                    return n->parent_id;  // 0 = 根，父级由宿主窗口 provider 承载
                case NavigationKind::FirstChild:
                case NavigationKind::LastChild: {
                    const auto *kids = children_of(id);
                    if (kids == nullptr || kids->empty()) {
                        return 0;
                    }
                    const bool first = (kind == NavigationKind::FirstChild) != rtl;
                    return first ? kids->front() : kids->back();
                }
                case NavigationKind::NextSibling:
                case NavigationKind::PreviousSibling: {
                    const auto *sibs = children_of(n->parent_id);
                    if (sibs == nullptr) {
                        return 0;
                    }
                    const auto self = std::ranges::find(*sibs, id);
                    if (self == sibs->end()) {
                        return 0;
                    }
                    const bool forward = (kind == NavigationKind::NextSibling) != rtl;
                    if (forward) {
                        return (std::next(self) == sibs->end()) ? 0U : *std::next(self);
                    }
                    return (self == sibs->begin()) ? 0U : *std::prev(self);
                }
            }
            return 0;
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::has_scrollable_ancestor(std::uint64_t id) -> bool {
    return eval_on_main<bool>(
        [this, id]() -> bool {
            sync_if_dirty();
            return scrollable_ancestor_on_main(id);
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::visible_box() -> Rect {
    return eval_on_main<Rect>(
        [this]() -> Rect {
            sync_if_dirty();
            if (snap_.flat.empty()) {
                return Rect{};
            }
            return snap_.flat.front().node.bounds;  // #3：根节点几何即客户区（窗口本地 DIP）
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::hit_test_id(Point local_dip) -> std::uint64_t {
    // 活树遍历（`EventDispatcher::hit_test`）必须在主人线程做，与绘制/命中同源（既有派发不变量）。
    return eval_on_main<std::uint64_t>(
        [this, local_dip]() -> std::uint64_t {
            sync_if_dirty();
            if (root_ == nullptr) {
                return 0;
            }
            Widget *hit = EventDispatcher::hit_test(*root_, local_dip);
            return (hit != nullptr) ? hit->runtime_id() : 0;
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::focused_id() -> std::uint64_t {
    return eval_on_main<std::uint64_t>(
        [this]() -> std::uint64_t {
            sync_if_dirty();
            for (const auto &n : snap_.flat) {
                if (n.node.state.focused) {
                    return n.id;
                }
            }
            return 0;
        },
        AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::provider_for(std::uint64_t id) -> IRawElementProviderSimple * {
    // `providers_` 的惰性插入与 `recycle_removed()` 的擦除都改树，故创建过程整体回投；
    // 交还的仍是**缓存借用**（不增计数），调用方按需 AddRef（与旧口径一致）。
    return eval_on_main<IRawElementProviderSimple *>(
        [this, id]() -> IRawElementProviderSimple * { return provider_on_main(id); }, AURORA_UI_READ_TIMEOUT_MS);
}

auto Win32UiaBridge::root_provider() -> IRawElementProviderSimple * {
    return eval_on_main<IRawElementProviderSimple *>(
        [this]() -> IRawElementProviderSimple * { return root_provider_on_main(); }, AURORA_UI_READ_TIMEOUT_MS);
}

// 告警面是「分析器无法证明不抛」的三类边界：纯虚 `uninstall_hook()`、函数指针
// `UiaApi::disconnect_provider` 与 COM `Release()`。实现侧只做 `vector<Provider *>` 的 erase、
// POD 赋值与引用计数递减，无分配、无抛出路径。析构期 try/catch 只会把拆链失败静默吞掉，
// 与仓内既有 3 处析构关停豁免同口径，故此处显式取舍而非包一层 catch。
// NOLINTNEXTLINE(bugprone-exception-escape)
Win32UiaBridge::~Win32UiaBridge() {
    // 第一件事必须是落存活闸：此刻起任何仍在队列里的回投闭包都不再解引用本桥。
    // 拆链与断连都在主人线程，与闭包执行同线程，故这道闸的写/读不会交错。
    alive_->store(false, std::memory_order_release);
    // 走 `disconnect_all()`：它同时负责**从进程级注册表注销**（`unregister_provider`）。
    // 缺了这一步，注册表会留下指向已析构桥的悬垂指针 —— 之后任何一条无障碍事件广播都会
    // 在该指针上调用 `is_active()`（use-after-free）。
    disconnect_all();
    if (host_provider_ != nullptr) {
        host_provider_->Release();
        host_provider_ = nullptr;
    }
}

auto Win32UiaBridge::activate() -> void {
    if (active_) {
        return;
    }
    if (!ensure_sta_apartment()) {
        Diagnostics::warn("Win32UiaBridge: COM apartment unavailable, accessibility bridge disabled",
                          "Win32UiaBridge::activate", "a11y-no-com-apartment");
        return;
    }
    if (!UiaApi::instance().loaded) {
        if (!warned_) {
            warned_ = true;
            Diagnostics::warn("Win32UiaBridge: UIAutomationCore.dll unavailable, accessibility bridge disabled",
                              "Win32UiaBridge::activate", "a11y-uia-unavailable");
        }
        return;
    }
    if (UiaApi::instance().host_provider_from_hwnd != nullptr && hwnd_ != nullptr && host_provider_ == nullptr) {
        IRawElementProviderSimple *host = nullptr;
        if (SUCCEEDED(UiaApi::instance().host_provider_from_hwnd(hwnd_, &host)) && host != nullptr) {
            host_provider_ = host;  // 持有引用（根 provider 派生的窗口级属性来源）
        }
    }
    active_ = true;
    dirty_ = true;
    a11y::register_provider(*this);
    // 激活即回填 `screen_reader_active`（heuristic 语义见 specification/01-core.md §7.2）：保留其余字段。
    auto settings = current_accessibility_settings();
    settings.screen_reader_active = true;
    set_accessibility_settings(settings);
}

auto Win32UiaBridge::deactivate() -> void { disconnect_all(); }

auto Win32UiaBridge::is_active() const -> bool { return active_; }

auto Win32UiaBridge::name() const -> std::string { return "win32-uia"; }

auto Win32UiaBridge::set_root(Widget *root) -> void {
    if (root_ == root) {
        return;
    }
    // 换根重建（#8）：收到非空新根即解除「拆除门闩」——典型场景是旧根经
    // `on_widget_destroying` 销毁广播后，宿主注入重建后的新根。若 root 为 nullptr（去根），
    // 不动门闩（交由 `disconnect_all` / `on_widget_destroying` 管理）。
    if (root != nullptr && tearing_down_) {
        tearing_down_ = false;
    }
    root_ = root;
    dirty_ = true;
}

auto Win32UiaBridge::reset_teardown() -> void {
    // #8：受控复位（仅换根重建前调用；见头文件文档）。
    tearing_down_ = false;
}

auto Win32UiaBridge::mark_dirty() -> void { dirty_ = true; }

auto Win32UiaBridge::on_widget_destroying(const Widget *w) -> void {
    if (w == nullptr || w != root_) {
        return;  // 只关心「销毁的正是缓存的根」；子节点由下次重投影自然淘汰
    }
    // 根正在销毁（`~Node` 期间 `Widget` 仍存活）：桥缓存的 `root_` 返回后即悬垂。
    // 先切根/清快照/清脏 —— 断连过程中 UIA 的同步重入只会得到「元素不可用」，绝不重建。
    root_ = nullptr;
    dirty_ = false;
    snap_ = {};
    id_by_widget_.clear();
    release_platform_providers();
    // 刻意**不**清 `active_`、不断注册表：窗口仍活着，宿主可注入新根后继续投影。
}

auto Win32UiaBridge::sync_if_dirty() -> void {
    if (tearing_down_ || !dirty_) {
        return;
    }
    rebuild();
}

auto Win32UiaBridge::on_event(const AccessibilityEvent & /*e*/) -> void {
    // 拉取式模型：事件只置脏（已由 `mark_dirty()` 完成），重建推迟到平台查询。
    // 焦点事件除外——读屏对焦点变化敏感，置脏后在下次查询即派发（见 `rebuild` 的 focus 项）。
}

auto Win32UiaBridge::on_announcement(const std::string &text, const Widget * /*target*/) -> void {
    if (!active_ || text.empty()) {
        return;
    }
    const UiaApi &api = UiaApi::instance();
    if (!api.loaded) {
        return;
    }
    IRawElementProviderSimple *root = root_provider();
    if (root == nullptr) {
        return;
    }
    const BSTR bstr = bstr_from_utf8(text);
    if (api.raise_notification_event != nullptr) {
        // 现代播报主通道（旧系统缺失时回退 LiveRegion，见下）。
        const HRESULT hr = api.raise_notification_event(root, NotificationKind_Other,
                                                        NotificationProcessing_ImportantMostRecent, bstr, nullptr);
        if (SUCCEEDED(hr)) {
            SysFreeString(bstr);
            return;
        }
    }
    SysFreeString(bstr);
    api.raise_automation_event(root, UIA_LiveRegionChangedEventId);
}

auto Win32UiaBridge::handle_get_object(WPARAM wp, LPARAM lp) -> std::optional<LRESULT> {
    // 只应答 UIA 根请求（`UiaRootObjectId` = -25）；其余 OBJID 交 `DefWindowProc`（MSAA 兜底，非目标）。
    constexpr LONG uia_root_object_id = -25;
    // 比较在 DWORD 域进行：`UiaRootObjectId` 取值为 -25，转成同一无符号域后逐位等价，
    // 同时避免有符号/无符号混比。lp 为其低 32 位。
    if (static_cast<DWORD>(lp) != static_cast<DWORD>(uia_root_object_id)) {
        return std::nullopt;
    }
    if (!active_) {
        activate();  // 惰性激活
    }
    if (!active_) {
        return std::nullopt;  // 降级：UIA 不可用 → 交系统兜底
    }
    const UiaApi &api = UiaApi::instance();
    if (api.return_raw_element_provider == nullptr) {
        return std::nullopt;
    }
    IRawElementProviderSimple *root = root_provider();
    if (root == nullptr) {
        return std::nullopt;
    }
    return api.return_raw_element_provider(hwnd_, wp, lp, root);
}

auto Win32UiaBridge::disconnect_all() -> void {
    // 门闩必须**先**立：`UiaDisconnectProvider` 会同步重入本 provider 取属性（UIA 需为被
    // 丢弃的侦听者补发属性变更事件），而窗口销毁后于宿主拆 UI 树是常规顺序，此刻 `root_`
    // 已悬垂。先清根/快照/脏，令重入路径只得到「元素不可用」，绝不重建。
    tearing_down_ = true;
    root_ = nullptr;
    dirty_ = false;
    snap_ = {};
    id_by_widget_.clear();

    if (active_) {
        active_ = false;
        // 必须注销注册表：否则进程级广播会在已析构的桥上调用 `is_active()`（UAF）。
        a11y::unregister_provider(*this);
        auto settings = current_accessibility_settings();
        settings.screen_reader_active = false;
        set_accessibility_settings(settings);
    }
    release_platform_providers();
}

auto Win32UiaBridge::release_platform_providers() -> void {
    const UiaApi &api = UiaApi::instance();
    if (api.disconnect_provider != nullptr) {
        for (auto &entry : providers_) {
            if (entry.second != nullptr) {
                api.disconnect_provider(entry.second);
            }
        }
        if (root_provider_ != nullptr) {
            api.disconnect_provider(root_provider_);
        }
    }
    // 释放桥持有的引用（UIA 侧若仍持有，对象靠自身引用计数存活）。
    for (auto &entry : providers_) {
        if (entry.second != nullptr) {
            entry.second->Release();
        }
    }
    providers_.clear();
    if (root_provider_ != nullptr) {
        root_provider_->Release();
        root_provider_ = nullptr;
    }
}

auto Win32UiaBridge::scale_factor() const -> float {
    if (hwnd_ == nullptr) {
        return 1.0F;
    }
    const HDC dc = GetDC(hwnd_);
    if (dc == nullptr) {
        return 1.0F;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(hwnd_, dc);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;  // 与 Win32Host::scale_factor 同口径
}

auto Win32UiaBridge::origin() const -> Point {
    if (hwnd_ == nullptr) {
        return Point{};
    }
    RECT r{};
    if (GetWindowRect(hwnd_, &r) == 0) {
        return Point{};
    }
    return Point{.x = static_cast<float>(r.left), .y = static_cast<float>(r.top)};
}

auto Win32UiaBridge::to_physical(const Rect &dip) const -> UiaRect {
    const float s = scale_factor();
    const Point o = origin();
    return UiaRect{.left = static_cast<double>(o.x + (dip.origin.x * s)),
                   .top = static_cast<double>(o.y + (dip.origin.y * s)),
                   .width = static_cast<double>(dip.size.width * s),
                   .height = static_cast<double>(dip.size.height * s)};
}

auto Win32UiaBridge::to_local(double phys_x, double phys_y) const -> Point {
    const float s = scale_factor();
    const Point o = origin();
    const float inv = (s > 0.0F) ? (1.0F / s) : 1.0F;
    return Point{.x = (static_cast<float>(phys_x) - o.x) * inv, .y = (static_cast<float>(phys_y) - o.y) * inv};
}

auto Win32UiaBridge::find_node(std::uint64_t id) const -> const a11y::NodeSnapshot * { return snap_.find(id); }

auto Win32UiaBridge::id_of(const Widget *w) const -> std::uint64_t {
    if (w == nullptr) {
        return 0;
    }
    const auto it = id_by_widget_.find(w);
    return (it == id_by_widget_.end()) ? 0 : it->second;
}

auto Win32UiaBridge::root_provider_on_main() -> IRawElementProviderSimple * {
    if (root_ == nullptr) {
        return nullptr;
    }
    sync_if_dirty();
    if (snap_.flat.empty()) {
        return nullptr;
    }
    const std::uint64_t root_id = snap_.flat.front().id;
    if (root_provider_ == nullptr || root_provider_->id() != root_id) {
        if (root_provider_ != nullptr) {
            root_provider_->Release();
        }
        root_provider_ = new UiaRootProvider(this, root_id);  // 引用计数 1（桥持有）
    }
    return root_provider_;
}

auto Win32UiaBridge::host_provider() -> IRawElementProviderSimple * { return host_provider_; }

auto Win32UiaBridge::provider_object(std::uint64_t id) -> UiaNodeProvider * {
    const auto it = providers_.find(id);
    return (it == providers_.end()) ? nullptr : it->second;
}

auto Win32UiaBridge::provider_on_main(std::uint64_t id) -> IRawElementProviderSimple * {
    if (id == 0) {
        return nullptr;
    }
    sync_if_dirty();  // 根 id 取自快照首元素：先消费脏位，别拿旧快照判定「谁是根」
    if (auto *existing = provider_object(id); existing != nullptr) {
        return existing;
    }
    const std::uint64_t root_id = snap_.flat.empty() ? 0 : snap_.flat.front().id;
    if (id == root_id) {
        return root_provider_on_main();  // 根唯一：避免 Navigate 到根时出现第二份根 provider
    }
    auto *p = new UiaNodeProvider(this, id);
    p->AddRef();  // 桥缓存持一份（保对象同一性：Qt/Chromium 实践）
    providers_[id] = p;
    return p;
}

auto Win32UiaBridge::scrollable_ancestor_on_main(std::uint64_t id) const -> bool {
    // 沿 parent 链上溯：任一祖先声明了滚动量即可被滚入视口（IScrollItemProvider）。
    const auto *n = snap_.find(id);
    while (n != nullptr && n->parent_id != 0) {
        const auto *parent = snap_.find(n->parent_id);
        if (parent == nullptr) {
            return false;
        }
        if (parent->node.has_action(AccessibilityAction::ScrollUp) ||
            parent->node.has_action(AccessibilityAction::ScrollDown)) {
            return true;
        }
        n = parent;
    }
    return false;
}

auto Win32UiaBridge::rebuild() -> void {
    dirty_ = false;
    if (tearing_down_ || root_ == nullptr || !active_) {
        return;
    }
    a11y::TreeSnapshot next = a11y::build_tree_snapshot(*root_, Rect{.origin = Point{}, .size = root_->size()});
    const bool first_projection = snap_.flat.empty();
    const a11y::TreeDiff diff = a11y::diff_snapshots(snap_, next);
    if (first_projection) {
        // 首个快照无「前值」可比：整树判为新增会对客户端造成无意义的事件风暴，直接跳过派发。
        snap_ = std::move(next);
        id_by_widget_.clear();
        for (const auto &n : snap_.flat) {
            if (n.widget != nullptr) {
                id_by_widget_[n.widget] = n.id;
            }
        }
        return;
    }

    // ---- 结构事件（单增删用 ChildAdded/ChildRemoved + runtimeId，批量才 Invalidated）----
    for (const std::uint64_t id : diff.added) {
        const auto *n = next.find(id);
        queue_structure_changed(n != nullptr ? n->parent_id : 0, true, id);
    }
    for (const std::uint64_t id : diff.removed) {
        const auto *old = snap_.find(id);
        queue_structure_changed(old != nullptr ? old->parent_id : 0, false, id);
    }
    const std::size_t structural = diff.added.size() + diff.removed.size();
    if (structural > 16 || !diff.moved.empty()) {  // 批量/换序：整段失效更省客户端开销
        for (const std::uint64_t id : diff.moved) {
            const auto *n = next.find(id);
            queue_structure_changed(n != nullptr ? n->parent_id : 0, true, 0);  // child_id = 0 ⇒ Invalidated
        }
    }

    // ---- 焦点事件 ----
    if (diff.focused_id.has_value()) {
        queue_focus_changed(*diff.focused_id);
    }

    // ---- 属性事件（逐字段；同节点同属性由 emit 阶段去重）----
    for (const auto &[id, field] : diff.updated) {
        const auto *o = snap_.find(id);
        const auto *n = next.find(id);
        if (o == nullptr || n == nullptr) {
            continue;
        }
        switch (field) {
            case a11y::FieldChange::Name: {
                VARIANT a{};
                VARIANT b{};
                variant_init_bstr(a, o->node.name);
                variant_init_bstr(b, n->node.name);
                queue_property_changed(id, UIA_NamePropertyId, a, b);
                break;
            }
            case a11y::FieldChange::Value: {
                VARIANT a{};
                VARIANT b{};
                variant_init_bstr(a, o->node.value);
                variant_init_bstr(b, n->node.value);
                queue_property_changed(id, UIA_ValueValuePropertyId, a, b);
                break;
            }
            case a11y::FieldChange::Hint: {
                VARIANT a{};
                VARIANT b{};
                variant_init_bstr(a, o->node.hint);
                variant_init_bstr(b, n->node.hint);
                queue_property_changed(id, UIA_HelpTextPropertyId, a, b);
                break;
            }
            case a11y::FieldChange::Range: {
                if (o->node.range.has_value() && n->node.range.has_value()) {
                    VARIANT a{};
                    VARIANT b{};
                    variant_init_double(a, o->node.range->value);
                    variant_init_double(b, n->node.range->value);
                    queue_property_changed(id, UIA_RangeValueValuePropertyId, a, b);
                }
                break;
            }
            case a11y::FieldChange::State: {
                if (o->node.state.checked != n->node.state.checked) {
                    VARIANT a{};
                    VARIANT b{};
                    variant_init_i4(a, o->node.state.checked ? ToggleState_On : ToggleState_Off);
                    variant_init_i4(b, n->node.state.checked ? ToggleState_On : ToggleState_Off);
                    queue_property_changed(id, UIA_ToggleToggleStatePropertyId, a, b);
                } else if (o->node.state.focused != n->node.state.focused) {
                    VARIANT a{};
                    VARIANT b{};
                    variant_init_bool(a, o->node.state.focused);
                    variant_init_bool(b, n->node.state.focused);
                    queue_property_changed(id, UIA_HasKeyboardFocusPropertyId, a, b);
                }
                break;
            }
            case a11y::FieldChange::Bounds:
            case a11y::FieldChange::Actions:
            default:
                break;  // 几何/动作变化由客户端按需重取，不发属性事件（避免事件风暴）
        }
    }

    // 快照切换 + 索引重建（控件销毁后映射未命中即「元素不可用」）。
    snap_ = std::move(next);
    id_by_widget_.clear();
    for (const auto &n : snap_.flat) {
        if (n.widget != nullptr) {
            id_by_widget_[n.widget] = n.id;
        }
    }
    recycle_removed(diff.removed);
    emit_pending();
}

auto Win32UiaBridge::recycle_removed(const std::vector<std::uint64_t> &removed) -> void {
    const UiaApi &api = UiaApi::instance();
    for (const std::uint64_t id : removed) {
        auto it = providers_.find(id);
        if (it == providers_.end() || it->second == nullptr) {
            continue;
        }
        UiaNodeProvider *p = it->second;
        // 仅当**无 UIA 侧引用**（只有桥自己持有）时立即回收；
        // 仍有外部引用者延后到 deactivate 全量断连，保 R1 的悬垂防线。
        if (p->holder_count() > 1U) {  // 除桥外仍有持有者（UIA 侧缓存）⇒ 延后到 deactivate
            continue;
        }
        if (api.disconnect_provider != nullptr) {
            api.disconnect_provider(p);
        }
        p->Release();  // 释放桥持有的那一份
        providers_.erase(it);
    }
}

auto Win32UiaBridge::queue_property_changed(std::uint64_t id, PROPERTYID prop, const VARIANT &old_v,
                                            const VARIANT &new_v) -> void {
    PendingEvent e{};
    e.kind = PendingEvent::Kind::Property;
    e.id = id;
    e.property = prop;
    e.old_value = old_v;
    e.new_value = new_v;
    pending_.push_back(e);
}

auto Win32UiaBridge::queue_structure_changed(std::uint64_t parent_id, bool child_added, std::uint64_t child_id)
    -> void {
    PendingEvent e{};
    e.kind = PendingEvent::Kind::Structure;
    e.id = parent_id;
    e.child_id = child_id;
    e.child_added = child_added;
    pending_.push_back(e);
}

auto Win32UiaBridge::queue_focus_changed(std::uint64_t id) -> void {
    PendingEvent e{};
    e.kind = PendingEvent::Kind::Focus;
    e.id = id;
    pending_.push_back(e);
}

auto Win32UiaBridge::queue_announcement(const std::string &text) -> void {
    PendingEvent e{};
    e.kind = PendingEvent::Kind::Announcement;
    e.text = text;
    pending_.push_back(e);
}

auto Win32UiaBridge::emit_pending() -> void {
    if (pending_.empty()) {
        return;
    }
    // 先按「同一节点同一属性只保留最后一条」去重；无监听者时整批丢弃并释放 VARIANT。
    if (!has_listeners()) {
        for (auto &e : pending_) {
            VariantClear(&e.old_value);
            VariantClear(&e.new_value);
        }
        pending_.clear();
        return;
    }
    // 再排序批量发出：结构 → 焦点 → 属性 → 播报。
    std::ranges::stable_sort(pending_, [](const PendingEvent &a, const PendingEvent &b) -> bool {
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    const UiaApi &api = UiaApi::instance();
    std::vector<PendingEvent> batch = std::move(pending_);
    pending_.clear();
    for (auto it = batch.begin(); it != batch.end();) {
        if (it->kind != PendingEvent::Kind::Property) {
            ++it;
            continue;
        }
        // 同 (id, property) 只保留最后一条：中间值对客户端无意义。
        auto later = std::ranges::find_if(std::next(it), batch.end(), [&](const PendingEvent &c) -> bool {
            return c.kind == PendingEvent::Kind::Property && c.id == it->id && c.property == it->property;
        });
        if (later != batch.end()) {
            VariantClear(&it->old_value);
            VariantClear(&it->new_value);
            it = batch.erase(it);
            continue;
        }
        ++it;
    }
    for (PendingEvent &e : batch) {
        IRawElementProviderSimple *target = (e.id != 0) ? provider_for(e.id) : root_provider();
        if (target == nullptr && e.kind != PendingEvent::Kind::Announcement) {
            VariantClear(&e.old_value);
            VariantClear(&e.new_value);
            continue;  // 节点已销毁：丢弃
        }
        switch (e.kind) {
            case PendingEvent::Kind::Structure: {
                if (api.raise_structure_changed != nullptr && target != nullptr) {
                    int rid[5] = {UiaAppendRuntimeId, 0, 0, 0, 0};
                    int rid_len = 1;
                    if (e.child_id != 0) {
                        const std::uint64_t id = e.child_id;
                        rid[1] = static_cast<int>(id & 0xFFFFFFFFULL);
                        rid[2] = static_cast<int>((id >> 32U) & 0xFFFFFFFFULL);
                        rid_len = 3;
                    }
                    api.raise_structure_changed(target,
                                                e.child_id == 0 ? StructureChangeType_ChildrenInvalidated
                                                                : (e.child_added ? StructureChangeType_ChildAdded
                                                                                 : StructureChangeType_ChildRemoved),
                                                rid, rid_len);
                }
                break;
            }
            case PendingEvent::Kind::Focus:
                if (api.raise_automation_event != nullptr && target != nullptr) {
                    api.raise_automation_event(target, UIA_AutomationFocusChangedEventId);
                }
                break;
            case PendingEvent::Kind::Property:
                if (api.raise_property_changed != nullptr && target != nullptr) {
                    api.raise_property_changed(target, e.property, e.old_value, e.new_value);
                }
                VariantClear(&e.old_value);
                VariantClear(&e.new_value);
                break;
            case PendingEvent::Kind::Announcement:
                on_announcement(e.text, nullptr);
                break;
        }
    }
    for (PendingEvent &e : batch) {
        VariantClear(&e.old_value);
        VariantClear(&e.new_value);
    }
}

}  // namespace aurora::detail

// NOLINTEND(*-pro-bounds-*, *-pro-type-*, *-special-member-functions, *-virtual-class-destructor)

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
