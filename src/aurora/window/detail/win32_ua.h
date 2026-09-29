#pragma once

// Win32 UIA 无障碍桥：**内部头**（与 `win32_cursor.h` / `win32_capture.h` 同列于 src/）。
//
// 为何不进公共头：实现要 `<windows.h>` / `<uiautomationcore.h>` 与 HWND，而
// `Win32Host` 刻意 pimpl 隔离、公共头零平台污染；本文件仅供 `win32_host.cpp` /
// `win32_surface.cpp` / `d3d11_surface.cpp` 引入。
//
// 平台宏来源必须在守卫**之前**引入：`AURORA_PLATFORM_WINDOWS` 由本头导出，若依赖调用方
// 先包含则「首 include 即本头」的 TU（如 win32_ua.cpp 自身）会把整文件判为假 → 空 TU、
// 链接期 undefined symbol。
#include "aurora/core/platform.h"

// 门控与 `win32_cursor.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
// MSVC SDK 的 UIAutomationCore.h 不自带前置包含：`interface` 宏（objbase/combaseapi）与
// VARIANT/SAFEARRAY（oleauto）在 WIN32_LEAN_AND_MEAN 下缺失，须先引伞头 <ole2.h>；
// MinGW 版头已自行包含 ole2.h，重复包含有守卫，两侧均安全。
#include <ole2.h>
// 引**总头** <uiautomation.h>（= core + client 的正确包含顺序），而非分别引
// <uiautomationcore.h> + <uiautomationclient.h>：
//  1. 常量（UIA_*ControlTypeId / UIA_*PropertyId / UIA_*EventId …）在 MinGW SDK 下
//     只存在于 client 段，单引 core 拿不到；
//  2. 单引 client 会因 TreeTraversalOptions / ConnectionRecoveryBehaviorOptions /
//     CoalesceEventsOptions 未前置声明而编译失败（总头内部顺序才正确）。
#include <uiautomation.h>
#include <uiautomationcoreapi.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aurora/core/a11y_provider.h"
#include "aurora/core/a11y_text.h"
#include "aurora/widget/a11y_diff.h"
#include "aurora/widget/a11y_tree.h"

namespace aurora::detail {

// 主线程回投超时预算（ms）—— 跨线程回投的降级门限。
// 为何是这个量级：读屏客户端（NVDA 等）自身对 provider 调用有秒级超时，超了会整条
// 无障碍链路报错并拖死前台。预算必须显著小于客户端超时，又要大于一次正常帧排空的
// 延迟。读路径远多于动作，取更小的预算；动作是用户意图，多等一档。
// @brief 属性 / 几何 / 导航等读路径的回投预算。
inline constexpr unsigned long AURORA_UI_READ_TIMEOUT_MS = 250;
// @brief Invoke / Toggle / SetValue / Scroll 等动作路径的回投预算。
inline constexpr unsigned long AURORA_UI_ACTION_TIMEOUT_MS = 500;

/// @brief `Navigate` 的方向意图（主人线程解析出的目标节点 id，0 = 无）。
enum class NavigationKind : std::uint8_t { Parent, FirstChild, LastChild, NextSibling, PreviousSibling };

class Win32UiaBridge;
// 豁免 cppcoreguidelines-virtual-class-destructor：以下两型是 UIA provider 接口实现（IUnknown 系，
// 类体定义见 win32_ua.cpp），protected + virtual 析构是 COM 官方接口形态的刻意镜像——外部不得经
// 接口指针直接 delete，生命周期由 AddRef/Release 引用计数驱动（末次 Release 自毁），
// 检查建议的「公有化析构/去 virtual」会破坏 COM 契约。
// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor)
class UiaNodeProvider;
// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor)
class UiaTextRangeProvider;

/// @brief 动态加载的 UIA 扁平 API（无链接期依赖，缺库降级 no-op）。
///
/// `UIAutomationCore.dll` 只在桥首次激活时 `LoadLibraryA`；任一**核心**函数缺失即整桥降级
/// （`Diagnostics::warn` 一次），`UiaRaiseNotificationEvent` 是**可选**项——较老系统上不存在，
/// 缺失时播报回退 `UIA_LiveRegionChangedEventId`。
struct UiaApi {
    bool loaded = false;
    LRESULT(WINAPI *return_raw_element_provider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *) = nullptr;
    HRESULT(WINAPI *raise_automation_event)(IRawElementProviderSimple *, EVENTID) = nullptr;
    HRESULT(WINAPI *raise_property_changed)(IRawElementProviderSimple *, PROPERTYID, VARIANT, VARIANT) = nullptr;
    HRESULT(WINAPI *raise_structure_changed)(IRawElementProviderSimple *, enum StructureChangeType, int *,
                                             int) = nullptr;
    HRESULT(WINAPI *disconnect_provider)(IRawElementProviderSimple *) = nullptr;
    HRESULT(WINAPI *host_provider_from_hwnd)(HWND, IRawElementProviderSimple **) = nullptr;
    HRESULT(WINAPI *get_reserved_not_supported)(IUnknown **) = nullptr;
    HRESULT(WINAPI *raise_notification_event)
    (IRawElementProviderSimple *, enum NotificationKind, enum NotificationProcessing, BSTR, BSTR) = nullptr;

    /// @brief 进程级加载（幂等；失败时 `loaded == false`）。
    [[nodiscard]] static auto instance() -> const UiaApi &;
};

/// @brief Win32 UIA 桥：快照 / diff / provider 缓存 / 事件映射 / 坐标换算。
///
/// 所有权：由 `Win32Host::Impl` 持有；`Win32Surface` 与 `D3D11Surface` 的
/// `accessibility_provider()` 都返回同一实例，避免两份 id→Widget* 映射分裂。
///
/// 同步模型：事件只置 dirty；平台查询（Navigate / 属性拉取）到达时
/// `sync_if_dirty()` 才重建快照 + diff + **批量**发事件。
///
/// 线程模型：UIA provider 的**每一个**回调都跑在 `UIAutomationCore.dll` 的 COM/RPC
/// 线程上（见 `ensure_sta_apartment()`：以 `RPC_E_CHANGED_MODE` 为可接受即已知并依赖了这一点），
/// 而 widget 树、`snap_`、`providers_` 只属于**主人线程**（= 构造本桥、并安装 `main_poster`
/// 帧循环的那条线程）。故本类把访问面劈成两半：
///  - `sync_if_dirty` / `rebuild` / `find_node` / `mark_dirty` / 事件队列等**裸口**只许主人线程调；
///  - provider 侧一律走「回投 + 按值副本」口（`eval_on_main` 及其派生方法），调用线程拿到的
///    永远是副本而非指针，跨线程悬垂读由构造排除。
/// 主人线程自己调用回投口时就地执行、零等待，因此宿主接线（`WM_GETOBJECT`、帧循环、
/// `UiaDisconnectProvider` 的同步重入）行为与代价均不变。
class Win32UiaBridge final : public a11y::Provider {
  public:
    explicit Win32UiaBridge(HWND hwnd);
    ~Win32UiaBridge() override;

    Win32UiaBridge(const Win32UiaBridge &) = delete;
    auto operator=(const Win32UiaBridge &) -> Win32UiaBridge & = delete;
    Win32UiaBridge(Win32UiaBridge &&) = delete;
    auto operator=(Win32UiaBridge &&) -> Win32UiaBridge & = delete;

    // ---- a11y::Provider ----
    auto activate() -> void override;
    auto deactivate() -> void override;
    /// @brief 消费脏位：必要时重建快照 + diff + 批量发事件。
    /// @warning **主人线程专用**：`rebuild()` 要走活 widget 树并整体换掉 `snap_`。
    ///          provider 侧一律经 `snapshot_copy` / `eval_widget_on_main` 间接到达，不得直调。
    auto sync_if_dirty() -> void override;
    auto mark_dirty() -> void override;
    [[nodiscard]] auto is_active() const -> bool override;
    [[nodiscard]] auto name() const -> std::string override;
    auto set_root(Widget *root) -> void override;
    auto on_event(const AccessibilityEvent &e) -> void override;
    auto on_announcement(const std::string &text, const Widget *target) -> void override;
    auto on_widget_destroying(const Widget *w) -> void override;

    // ---- 宿主接线（Win32Host 的 WM_GETOBJECT 分支调用）----
    /// @brief 应答 `WM_GETOBJECT`：`lParam == UiaRootObjectId` 时返回根 provider 的 LRESULT。
    /// @return 已处理返回其 LRESULT；非 UIA 请求返回 `std::nullopt`（交由 DefWindowProc）。
    [[nodiscard]] auto handle_get_object(WPARAM wp, LPARAM lp) -> std::optional<LRESULT>;
    /// @brief 窗口销毁前调用：全量 `UiaDisconnectProvider` 防 UIA 侧悬垂访问（R1）。
    /// 等价于 `deactivate()`（幂等、可重入）；窗口关闭与桥析构都走此口。
    auto disconnect_all() -> void;
    /// @brief 切断根与缓存并把平台侧对象全部断连/释放（不清 `active_`，不断注册表）。
    /// 用于「根控件已销毁但窗口仍在」：桥之后可接受新根继续投影。
    auto release_platform_providers() -> void;

    /// @brief 解除「拆除门闩」（#8：`tearing_down_` 受控复位）。
    ///
    /// 仅用于**换根重建**场景：窗口 UI 树整体重建、旧根销毁广播已发生、即将经 `set_root`
    /// 注入新根之前调用。复位后桥恢复可投影状态。
    /// @warning 不得在窗口已销毁 / `disconnect_all()` 已调用的状态下调用——那会令「已销毁窗口复活」。
    ///          正常窗口生命周期无需调用本方法；`set_root` 在收到非空新根时会自动复位本门闩。
    auto reset_teardown() -> void;

    // ---- RTL（#5：应用侧经 Window 推送，桥内只做遍历/几何适配）----
    /// @brief 设置 RTL 标志（由 `Window::set_accessibility_rtl` 转发）。
    auto set_rtl(bool rtl) -> void override { rtl_.store(rtl, std::memory_order_relaxed); }
    /// @brief 当前是否 RTL（影响 Navigate 遍历顺序，使读屏按从右到左阅读顺序遍历）。
    /// RPC 线程在 `navigate_target` 之外也会读它，故用原子量。
    [[nodiscard]] auto is_rtl() const -> bool { return rtl_.load(std::memory_order_relaxed); }
    /// @brief 窗口可见盒（根节点几何，窗口本地 DIP）：裁剪离屏几何用（#3）。
    /// **任意线程可调**：内部回投主人线程取副本。
    [[nodiscard]] auto visible_box() -> Rect;

    // ---- 跨线程访问面（provider 回调所在的 RPC 线程用）----
    /// @brief 把 `fn` 送到主人线程执行，并把结果**按值**带回调用线程。
    ///
    /// 三条路径，语义一致地都返回 `R`：
    ///  1. 调用线程即主人线程（宿主接线、帧循环、`UiaDisconnectProvider` 的同步重入）
    ///     → 就地执行，零等待、零入队开销；
    ///  2. 进程级 `main_poster` 未安装（无头 / 单测 / 事件循环未起）→ 就地执行
    ///     （与 `post_to_main` 的无回投器回退同语义，裁定③）；
    ///  3. 其余（= 真的跨线程）→ 入队 + 限时等待 `timeout_ms`。
    ///
    /// 超时或 `fn` 抛异常即降级：返回零值 `R{}`，同时把「已放弃」标志置起——晚到的队列项
    /// 看到标志就不再执行 `fn`。这条很重要：客户端已经收到「元素不可用」，事后补做一次
    /// Invoke/SetValue 会做出用户以为没发生的事。
    /// @warning 传给 `fn` 的结果类型应优先选 `std::optional<...>`：超时与「节点已不在快照」
    ///          都返回 `R{}`，调用方据此统一降级为 `UIA_E_ELEMENTNOTAVAILABLE`。
    template <typename R>
    [[nodiscard]] auto eval_on_main(std::function<R()> fn, unsigned long timeout_ms) -> R {
        auto slot = std::make_shared<R>();
        auto abandoned = std::make_shared<std::atomic<bool>>(false);
        const bool ran = wait_on_main(
            [fn = std::move(fn), slot, abandoned]() -> void {
                if (abandoned->load(std::memory_order_acquire)) {
                    return;  // 调用方已超时返回：不补做
                }
                *slot = fn();
            },
            abandoned, timeout_ms);
        if (!ran) {
            abandoned->store(true, std::memory_order_release);
            return R{};
        }
        return std::move(*slot);
    }

    /// @brief `eval_on_main` 的取节点便捷形：在主人线程把 `id` 解析成活 `Widget` 后施加 `fn`。
    ///
    /// 解析口径与旧 `UiaNodeProvider::widget()` 一致（先消费脏位再查映射）；解析不到
    /// （节点已被摘出快照 / 桥已拆除）与超时同为 `R{}`。`fn` 只在主人线程被调用，
    /// 其返回值必须是不含 `Widget*`/指针的成员——只有副本能跨线程。
    template <typename R>
    [[nodiscard]] auto eval_widget_on_main(std::uint64_t id, std::function<R(Widget &)> fn, unsigned long timeout_ms)
        -> R {
        return eval_on_main<R>(
            [this, id, fn = std::move(fn)]() -> R {
                Widget *w = widget_on_main(id);
                if (w == nullptr) {
                    return R{};
                }
                return fn(*w);
            },
            timeout_ms);
    }

    // ---- 供 provider 查询（以下全部经上面两口回投，返回按值副本）----
    [[nodiscard]] auto hwnd() const -> HWND { return hwnd_; }
    /// @brief 缩放与窗口原点：只读 Win32 几何 API（`GetDC`/`GetDeviceCaps`/`GetWindowRect`），
    /// 句柄级 API 本身跨线程安全，故 provider 可在 RPC 线程直接调（不回投）。
    [[nodiscard]] auto scale_factor() const -> float;
    [[nodiscard]] auto origin() const -> Point;
    /// @brief 窗口本地 DIP → 物理屏幕像素（`物理 = bounds × scale + position`）。任意线程可调。
    [[nodiscard]] auto to_physical(const Rect &dip) const -> UiaRect;
    /// @brief 物理屏幕像素 → 窗口本地 DIP（`ElementProviderFromPoint` 反换算）。任意线程可调。
    [[nodiscard]] auto to_local(double phys_x, double phys_y) const -> Point;

    [[nodiscard]] auto root_widget() const -> Widget * { return root_; }
    /// @brief 窗口宿主 provider（`UiaHostProviderFromHwnd` 结果）。
    /// `activate()` 于主人线程一次性赋值，此后只读；返回缓存借用（不增计数）。
    [[nodiscard]] auto host_provider() -> IRawElementProviderSimple *;
    /// @brief 根 provider（回投主人线程取/建，返回缓存借用）。
    [[nodiscard]] auto root_provider() -> IRawElementProviderSimple *;
    /// @brief 节点 provider（**保对象同一性**：Qt/Chromium 实践；回投创建，返回缓存借用）。
    [[nodiscard]] auto provider_for(std::uint64_t id) -> IRawElementProviderSimple *;
    /// @brief 取某节点的快照**副本**：未命中/超时 → `nullopt`。provider 的属性读全走这里。
    /// @note 副本的 `widget` 活指针被置空 —— 出境的指针就是本缺陷的本体，碰活 widget 只许走
    ///       `eval_widget_on_main`。
    [[nodiscard]] auto snapshot_copy(std::uint64_t id) -> std::optional<a11y::NodeSnapshot>;
    /// @brief 按方向取导航目标节点 id（RTL 反转与 children_of 都在主人线程算完）。
    /// @return `nullopt` = 本节点已不在快照（→ `UIA_E_ELEMENTNOTAVAILABLE`）；值 0 = 该方向无目标；
    ///         其余 = 目标节点 id。用 `nullopt`/0 两态区分，是为了让一次回投就带回旧实现里
    ///         「先查节点、再查邻居」两步的全部信息（Navigate 是树遍历的热路）。
    [[nodiscard]] auto navigate_target(std::uint64_t id, NavigationKind kind) -> std::optional<std::uint64_t>;
    /// @brief 是否存在「可滚动祖先」（`IScrollItemProvider` 暴露判据；回投）。
    [[nodiscard]] auto has_scrollable_ancestor(std::uint64_t id) -> bool;
    /// @brief 命中测试（与派发器同口径）→ 节点 id（回投，内部会活树遍历）。
    [[nodiscard]] auto hit_test_id(Point local_dip) -> std::uint64_t;
    /// @brief 当前聚焦节点 id（0 = 无；回投）。
    [[nodiscard]] auto focused_id() -> std::uint64_t;

    // ---- 主人线程专用裸口 ----
    /// @brief 按 id 查当前快照节点（**不**回投、返回指向 `snap_` 的裸指针）。
    /// 仅供 `rebuild()` / `emit_pending()` / `widget_on_main()` 等主人线程路径使用。
    [[nodiscard]] auto find_node(std::uint64_t id) const -> const a11y::NodeSnapshot *;
    /// @brief 当前快照（同上：主人线程专用引用，provider 侧不得直接读）。
    [[nodiscard]] auto snapshot() const -> const a11y::TreeSnapshot & { return snap_; }
    [[nodiscard]] auto id_of(const Widget *w) const -> std::uint64_t;

    // ---- 事件（供 provider / 内部使用）----
    auto queue_property_changed(std::uint64_t id, PROPERTYID prop, const VARIANT &old_v, const VARIANT &new_v) -> void;
    auto queue_structure_changed(std::uint64_t parent_id, bool child_added, std::uint64_t child_id) -> void;
    auto queue_focus_changed(std::uint64_t id) -> void;
    auto queue_announcement(const std::string &text) -> void;
    /// @brief 是否有客户端在监听（无监听者时整批丢弃）。
    /// 计数由 RPC 线程上的连接点增删、由主人线程的派发读，故用原子量。
    [[nodiscard]] auto has_listeners() const -> bool { return listener_count_.load(std::memory_order_relaxed) > 0; }
    auto note_listener_added() -> void { listener_count_.fetch_add(1, std::memory_order_relaxed); }
    auto note_listener_removed() -> void {
        // 与原「> 0 才减」逐位等价；CAS 是为「RPC 线程增删 + 主人线程读」并存准备的。
        int expected = listener_count_.load(std::memory_order_relaxed);
        while (expected > 0 &&
               !listener_count_.compare_exchange_weak(expected, expected - 1, std::memory_order_relaxed)) {
            // 空体：CAS 失败时 expected 已被刷新为现值，直接重试。
        }
    }

  private:
    // ---- 回投内核 ----
    /// @brief 把 `work` 送到主人线程执行并**限时**等待其完成。
    ///
    /// 就地执行的两条合法路径（都不入队、不等待）：调用线程本就是主人线程（宿主接线、帧循环、
    /// `UiaDisconnectProvider` 的同步重入），或进程级 `main_poster` 未安装（无头 / 单测）。
    /// 其余情形一律入队；队列项真正执行前还要再过三道闸：桥未被析构、调用方未超时放弃、
    /// 当前线程确是主人线程（`Window::run` 退出后 poster 会把任务**就地**在投递线程上跑，
    /// 见 `window.h` 的 `queue->running` 分支——那正是本口要防的形态，故拒绝执行）。
    /// @return true = `work` 已在主人线程跑完且未抛异常；false = 超时 / 抛异常 / 被任一道闸拒绝。
    /// @note 入队之后本口只碰局部的 promise/future，不再解引用 `this`，因此**桥可以在等待方仍在途
    ///       时被析构**（窗口拆除与 RPC 在途是两条时间线）；存活闸保证队列项同样不碰树。
    [[nodiscard]] auto wait_on_main(std::function<void()> work, const std::shared_ptr<std::atomic<bool>> &abandoned,
                                    unsigned long timeout_ms) -> bool;
    /// @brief 主人线程专用：消费脏位后按 id 解析活 widget（先 `sync_if_dirty` 再查映射）。
    /// @warning 只在 `eval_on_main` / `eval_widget_on_main` 的闭包内调用；返回的裸指针不得离开主人线程。
    [[nodiscard]] auto widget_on_main(std::uint64_t id) -> Widget *;
    /// @brief 主人线程专用：根 provider 的取/建（`root_provider()` 的内层）。
    [[nodiscard]] auto root_provider_on_main() -> IRawElementProviderSimple *;
    /// @brief 主人线程专用：节点 provider 的取/建（`provider_for()` 的内层，会写 `providers_`）。
    [[nodiscard]] auto provider_on_main(std::uint64_t id) -> IRawElementProviderSimple *;
    /// @brief 主人线程专用：沿快照父链找可滚动祖先（`has_scrollable_ancestor()` 的内层）。
    [[nodiscard]] auto scrollable_ancestor_on_main(std::uint64_t id) const -> bool;
    /// @brief 当前线程是否就是主人线程（就地执行判据，兼挡 `UiaDisconnectProvider` 的重入死等）。
    [[nodiscard]] auto on_owner_thread() const -> bool;

    auto rebuild() -> void;
    auto emit_pending() -> void;
    auto recycle_removed(const std::vector<std::uint64_t> &removed) -> void;
    [[nodiscard]] auto provider_object(std::uint64_t id) -> UiaNodeProvider *;

    struct PendingEvent {
        enum class Kind : std::uint8_t { Structure, Focus, Property, Announcement } kind = Kind::Property;
        std::uint64_t id = 0;
        std::uint64_t child_id = 0;
        bool child_added = true;
        PROPERTYID property = 0;
        VARIANT old_value{};
        VARIANT new_value{};
        std::string text;
    };

    HWND hwnd_ = nullptr;
    Widget *root_ = nullptr;
    bool active_ = false;
    bool dirty_ = true;
    bool warned_ = false;
    /// @brief 拆除门闩（单向，仅由 `disconnect_all()` 立起；`reset_teardown()` / `set_root` 可复位）。
    ///
    /// 为何必需：`UiaDisconnectProvider` 会**同步重入**本 provider 取属性值（UIA 需为被
    /// 丢弃的侦听者补发属性变更事件）。而窗口销毁是**后于**宿主拆 UI 树的常规顺序，此刻
    /// `root_` 已悬垂 —— 若重入路径仍走拉取式重建，就会解引用已释放内存（实机 SIGSEGV，
    /// 栈：`UiaDisconnectProvider → GetPropertyValue → node() → rebuild() → build_accessibility_node`）。
    /// 立闩后重入只读旧快照、永不重建。
    bool tearing_down_ = false;
    /// @brief RTL 标志（#5）：影响 Navigate 遍历顺序；由应用侧 `Window::set_accessibility_rtl` 推送。
    /// RPC 线程会读（Navigate 的翻转判据），故原子。
    std::atomic<bool> rtl_{false};
    std::atomic<int> listener_count_{0};
    /// @brief 主人线程 = 构造本桥的线程（`Win32Host` 建窗时于 UI 线程 `make_unique`，与
    /// `Window::run` 的帧循环同线程）。`wait_on_main` 据此判定「就地执行 vs 入队等待」。
    std::thread::id owner_thread_ = std::this_thread::get_id();
    /// @brief 桥存活闸（`~Win32UiaBridge` 的第一件事就是把它置 false）。
    ///
    /// 为何需要：回投闭包必然捕获 `this`（要碰 `snap_`/`root_`），而窗口拆除与「已入队的
    /// 队列项」是两条独立时间线。共享标志本身活在自己的 `shared_ptr` 上，读它永远安全；
    /// 它为 false 时闭包绝不解引用 `this`。
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    a11y::TreeSnapshot snap_;
    std::unordered_map<const Widget *, std::uint64_t> id_by_widget_;
    std::unordered_map<std::uint64_t, UiaNodeProvider *> providers_;
    IRawElementProviderSimple *host_provider_ = nullptr;  ///< `UiaHostProviderFromHwnd`
    UiaNodeProvider *root_provider_ = nullptr;
    std::vector<PendingEvent> pending_;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
