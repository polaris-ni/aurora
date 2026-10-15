#pragma once

/// @brief WebView 宿主侧：原生子视图书架 + 能力裁决 + 几何换算。
/// @file native_view_host.h
///
/// 本头是「自绘 UI 承载外部原生视图」的**宿主侧**抽象，与具体 Web 引擎无关：
/// 引擎能力由 `NativeViewEngine` 提供（平台实现各自一个），几何同步与生命周期由
/// `NativeViewHost` 统一管理，二者经 `NativeViewSink` 这一个窄接口对接；页面内容操作
/// （导航 / 脚本 / 消息）不经宿主，走派生的 `WebViewSink` 由控件直达视图。
///
/// 为什么几何必须由宿主统一管理而不是控件自己算：`BuildContext` 只有
/// `env` / `scale_factor` / `size` / `host_id`，**没有任何窗口指针**；而把控件 dp 矩形
/// 换算成宿主物理像素需要 `Surface::scale_factor()` 与 `Surface::content_inset()`
/// （CSD 装饰预留的安全区），这两项只有宿主拿得到。控件各自去找窗口会复制出 N 份
/// 换算实现，且没法保证与 `Window` 的绘制坐标同域。
///
/// 线程模型：全部入口 main-thread only。宿主访问口是线程局部槽（形态同
/// `event/focus.h` 的 `current_focus_manager()` / `set_current_focus_manager()`），
/// 由宿主在**帧首、树重建之前**安装，帧尾恢复——控件登记发生在挂载期，故安装口
/// 必须早于挂载，否则控件拿不到宿主。
///
/// 典型用法（宿主侧）：
/// @code
/// au::webview::NativeViewHost host;
/// au::webview::NativeViewHost::set_current(&host);   // 帧首安装
/// // ... 布局 / 绘制 ...
/// host.sync_geometry(geometry_changed);              // 布局落定后、present 之前
/// au::webview::NativeViewHost::set_current(nullptr); // 帧尾恢复
/// @endcode
///

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/types.h"

namespace aurora::webview {

/// @brief Web 引擎种类（枚举位按平台实现逐步填充；`Null` 为无引擎的确定性桩）。
enum class WebViewEngine : std::uint8_t {
    Null,  ///< 无引擎（确定性桩）：恒不可用，供无头 / WASM / CI 走降级路径
    WebView2,  ///< Windows：Microsoft Edge WebView2（系统运行时）
    WebKitGtk,  ///< Linux：WebKitGTK（系统运行时）
    WkWebView,  ///< macOS：WKWebView（系统框架）
};

/// @brief 引擎可用性裁决结果（驱动降级语义，不驱动控制流分支）。
enum class WebViewAvailability : std::uint8_t {
    Available,  ///< 可用
    MissingRuntime,  ///< 已编译本平台实现，但运行期探测不到引擎运行时
    UnsupportedPlatform,  ///< 当前平台 / 后端不提供该能力（如 Wayland 的交互模式）
    Disabled,  ///< 编译期未开启（对应 `AURORA_ENABLE_WEBVIEW*` 开关为 OFF）
};

/// @brief 引擎能力快照：回答「本进程此刻能做什么」，而非「目标平台是什么」。
struct WebViewCapabilities {
    bool interactive = false;  ///< 交互模式（原生子视图）是否可用
    bool snapshot = false;  ///< 快照模式（blit 进 Painter 帧流）是否可用
    WebViewEngine engine = WebViewEngine::Null;  ///< 实际生效的引擎
    WebViewAvailability availability = WebViewAvailability::Disabled;  ///< 可用性裁决

    /// @brief 是否完全不可用（两种模式皆不可）。
    /// @return `interactive` 与 `snapshot` 皆为 false 时为 true。
    [[nodiscard]] auto unavailable() const -> bool { return !interactive && !snapshot; }
};

/// @brief 宿主窗口坐标系下的**物理像素**矩形（原生子视图的下发单位）。
///
/// 与 `Rect`（逻辑 dp）的根本区别：原生视图按设备像素定位，故下发前必须完成
/// dp → 物理换算；本结构即换算产物，也是去重判据的比较单位。
struct NativeViewRect {
    int x = 0;  ///< 左边界（物理像素）
    int y = 0;  ///< 上边界（物理像素）
    int width = 0;  ///< 宽度（物理像素，< 0 视为 0）
    int height = 0;  ///< 高度（物理像素，< 0 视为 0）

    /// @brief 矩形是否有面积（宽高皆为正）。
    /// @return 宽高皆 > 0 时为 true。
    [[nodiscard]] auto has_area() const -> bool { return width > 0 && height > 0; }
    /// @brief 逐字段相等（去重判据的比较基准）。
    /// @return 四字段逐一相等时为 true。
    [[nodiscard]] auto operator==(const NativeViewRect &other) const -> bool = default;
};

/// @brief 把窗口逻辑 dp 矩形换算为宿主物理像素矩形（**纯函数**，几何同步的唯一换算落点）。
///
/// 换算 = 先叠加 CSD 安全区内缩（`content_inset`），再乘设备像素比（`scale_factor`）。
/// 顺序不可颠倒：`content_inset` 是逻辑 dp 量，先乘比例会把内缩量也放大一次。
///
/// 取整取最近值（`std::lround`）而非截断：截断在 1.5 倍缩放下会让相邻控件系统性
/// 偏移半个像素，且与「下发的矩形必须覆盖控件实际占区」这一诉求相反。
///
/// @param window_dips 控件在**窗口逻辑坐标**下的矩形（经 `Widget::window_bounds()` 取得）。
/// @param scale_factor 设备像素比（`dpi / 96`，经 `Surface::scale_factor()` 取得；<= 0 时按 1.0 计）。
/// @param content_inset CSD 装饰预留的安全区内边距（逻辑 dp；经 `Surface::content_inset()` 取得）。
/// @return 宿主物理像素矩形；宽高负值被夹为 0。
/// @note Thread: main-thread only
/// @note Side-effects: none
[[nodiscard]] auto to_native_rect(const Rect &window_dips, float scale_factor, const EdgeInsets &content_inset)
    -> NativeViewRect;

class NativeViewHost;

/// @brief 宿主回访票据：把「宿主指针」与「宿主是否还在」绑成一个可判活的整体。
///
/// 存在的理由是拆毁顺序有两种而控件只可能活在其中一种里：登记表条目持有的是裸
/// `WebView*`，控件析构时必须回访宿主注销自己；而「先拆窗口、后拆控件树」这一反向顺序下，
/// 回访时刻宿主自身已经析构，裸指针即悬垂。票据由宿主持有 `shared_ptr`、控件侧只持
/// `weak_ptr`，故**唯一取到宿主指针的途径就是 `lock()`**——宿主随控制块一同失效，取不到
/// 就不再回访。与 `Node::~Node` 切断语义树投影用的是同一类防护。
struct NativeViewTicket {
    NativeViewHost *host = nullptr;  ///< 宿主指针（仅当票据未失效时有效，不得跨 lock() 缓存）
};

/// @brief 导航状态（`WebViewCallbacks::on_navigation` 的第二参数）。
///
/// 定义落在宿主头而非控件头，是因为它是**引擎契约的一部分**（三个平台实现都要上报这三态），
/// 而控件头包含宿主头、反向包含会成环。控件侧同名转发由 `web_view.h` 的 using 承担。
enum class WebViewNavigationState : std::uint8_t {
    Loading,  ///< 导航已发起、尚未完成
    Finished,  ///< 导航成功完成
    Failed,  ///< 导航失败
};

class WebView;

/// @brief Web → Native 消息回调（载荷为页面经 `window.aurora.postMessage` 投出的 JSON 文本）。
using WebViewMessageCallback = std::function<void(std::string json)>;

/// @brief 导航状态回调（uri 为本次导航目标；state 为终态或加载中）。
using WebViewNavigationCallback = std::function<void(std::string uri, WebViewNavigationState state)>;

/// @brief 标题变化回调。
using WebViewTitleCallback = std::function<void(std::string title)>;

/// @brief 引擎进程崩溃 / 挂起回调（`detail` 为引擎自报的原因文本）。
using WebViewProcessFailedCallback = std::function<void(std::string detail)>;

/// @brief 控件侧的四个入向回调（引擎 → 控件的唯一通道）。
///
/// 为什么是「一份可被 `weak_ptr` 观察的数据」而不是四个 `std::function` 直接塞进视图：
/// 引擎的事件回调是**异步**的，投递时刻控件可能已经不在（宿主先亡时视图还活着、控件已析构，
/// 或反之）。让控件持 `shared_ptr`、视图只持 `weak_ptr`，回调到达时 `lock()` 判活即天然的
/// 守卫——不需要「不判活不解引用」这种靠自觉的契约，写出来就是对的。
///
/// 各成员可空（只订阅自己关心的事件）；引擎侧调用前须逐个判空。
struct WebViewCallbacks {
    WebViewNavigationCallback on_navigation;  ///< 导航三态（发起 / 完成 / 失败）
    WebViewTitleCallback on_title_changed;  ///< 文档标题变化
    WebViewMessageCallback on_web_message;  ///< 页面经 shim 投来的 JSON
    WebViewProcessFailedCallback on_process_failed;  ///< 引擎渲染进程异常
};

/// @brief 原生视图的宿主侧接收接口（窄接口；由各平台引擎实现）。
///
/// 宿主只经本接口下发「在哪、是否可见」，其余一切（加载、脚本、输入）都不经宿主——
/// 这条边界的意义是把「与窗口几何相关的部分」收在宿主一处，平台实现不必各自
/// 重新实现坐标换算与去重。控件侧要驱动页面内容走派生的 `WebViewSink`（同源一实例，
/// 宿主按本基类持有，不需要向下转型）。
///
/// @note Thread: main-thread only（回调落在 UI 线程）
class NativeViewSink {
  public:
    /// @brief 虚析构（经基类指针销毁派生实现的唯一入口）。
    virtual ~NativeViewSink() = default;

    NativeViewSink() = default;

    /// @brief 禁复制/移动：多态接口经基类指针持有派生实现，拷贝会对象切片。
    NativeViewSink(const NativeViewSink &) = delete;
    /// @brief 禁移动构造（理由同拷贝构造）。
    NativeViewSink(NativeViewSink &&) = delete;
    /// @brief 禁复制赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const NativeViewSink &) -> NativeViewSink & = delete;
    /// @brief 禁移动赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(NativeViewSink &&) -> NativeViewSink & = delete;

    /// @brief 下发物理矩形（宿主每帧同步时调用；与上次相同则不会被调用）。
    /// @param rect 宿主物理像素矩形（已含 CSD 内缩与 scale 换算）。
    virtual auto set_native_rect(const NativeViewRect &rect) -> void = 0;

    /// @brief 下发可见性（控件脱离窗口布局或零面积时为 false）。
    /// @param visible 是否应可见。
    virtual auto set_native_visible(bool visible) -> void = 0;

    /// @brief 请求一次内容快照（异步；像素就绪后 `done` 落在 UI 线程）。
    ///
    /// 快照模式（`WebViewMode::Snapshot`）的唯一像素来源。三平台引擎的捕获入口都是异步的
    /// （WebView2 `CapturePreview` / WebKitGTK `get_snapshot`），且回调天然回到创建视图的
    /// UI 线程，故与本库单线程 UI 模型直接契合，不需要跨线程回投。
    ///
    /// 实现契约：**只可视口有像素**（引擎侧的固有限制，提案已据实写明）；捕获失败或尚未
    /// 就绪时 `done` 必须仍被调用且携空图——调用方据 `pixels.empty()` 判失败，不得靠
    /// 「没收到回调」推断结果。
    /// @param done 像素就绪回调（参数为 RGBA8 位图）。
    virtual auto capture(std::function<void(Image)> done) -> void = 0;

    /// @brief 销毁前的解绑（宿主摘除登记表条目时调用；实现须在此释放平台资源）。
    virtual auto detach() -> void = 0;
};

/// @brief 原生 Web 视图的控件侧接口：几何之外另有内容通道。
///
/// 与 `NativeViewSink` 拆开的理由是两侧契约不同：宿主侧的窄接口是「只关窗口几何」，它必须
/// 与引擎无关（几何同步与去重全在宿主一处）；本接口则全是页面语义，宿主一行都不碰。合并
/// 成一份的话，「宿主不知道页面存在」这条边界就没了，而引擎实现也会被迫为一个用不到的
/// 面负责。
///
/// 实现契约（三平台一致，故可直接做成虚函数面）：
/// - 所有方法**异步不阻塞**；需要结果的（`evaluate_script`）经回调落在 UI 线程。
/// - 未导航 / 无历史时 `reload` / `stop` / `go_back` / `go_forward` 是空操作而非错误。
/// - `post_message` 在页面尚未建立消息通道时**丢弃并可被引擎侧观测**，不得排队泄漏。
class WebViewSink : public NativeViewSink {
  public:
    /// @brief 虚析构（宿主按 `NativeViewSink` 基类指针销毁本派生实现）。
    ~WebViewSink() override = default;

    WebViewSink() = default;

    /// @brief 禁复制/移动：多态接口经基类指针持有派生实现，拷贝会对象切片。
    WebViewSink(const WebViewSink &) = delete;
    /// @brief 禁移动构造（理由同拷贝构造）。
    WebViewSink(WebViewSink &&) = delete;
    /// @brief 禁复制赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const WebViewSink &) -> WebViewSink & = delete;
    /// @brief 禁移动赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(WebViewSink &&) -> WebViewSink & = delete;

    /// @brief 导航到指定 URI。
    /// @param uri 目标 URI。
    virtual auto navigate(const std::string &uri) -> void = 0;

    /// @brief 以内联 HTML 替换内容。
    /// @param html HTML 文本。
    /// @param base_uri 相对资源的基 URI（可空）。
    virtual auto navigate_to_string(const std::string &html, const std::string &base_uri) -> void = 0;

    /// @brief 重新加载当前文档。
    virtual auto reload() -> void = 0;

    /// @brief 停止当前导航。
    virtual auto stop() -> void = 0;

    /// @brief 后退一页（无历史时空操作）。
    virtual auto go_back() -> void = 0;

    /// @brief 前进一页（无历史时空操作）。
    virtual auto go_forward() -> void = 0;

    /// @brief 设置页面缩放系数（对应 `WebViewProps::zoom_factor`）。
    /// @param zoom 缩放系数（1.0 为原始尺寸）。
    virtual auto set_zoom_factor(float zoom) -> void = 0;

    /// @brief 设置是否允许页面执行脚本（对应 `WebViewProps::javascript_enabled`）。
    /// @param enabled 允许为 true。
    virtual auto set_javascript_enabled(bool enabled) -> void = 0;

    /// @brief 在页面中执行脚本（异步；结果经 `done` 落在 UI 线程）。
    /// @param js 待执行的脚本源码。
    /// @param done 结果回调（参数为返回值 JSON 文本；失败时为空串）。
    virtual auto evaluate_script(const std::string &js, std::function<void(std::string)> done) -> void = 0;

    /// @brief 向页面投递一条 JSON 消息（对应 `window.aurora` 侧的接收）。
    /// @param json 待投递的 JSON 文本。
    virtual auto post_message(const std::string &json) -> void = 0;

    /// @brief 安装入向回调（控件在登记时调一次；传空撤销）。
    ///
    /// 只收 `weak_ptr`：见 `WebViewCallbacks` 的理由——异步回调到达时控件可能已析构，
    /// 判活必须是结构性的，不能靠注释提醒。
    /// @param callbacks 回调聚合的弱引用。
    virtual auto set_callbacks(std::weak_ptr<WebViewCallbacks> callbacks) -> void = 0;
};

/// @brief 宿主需要并入事件等待的 fd 条目（语义同既有 AT-SPI 桥的 `WatchFd`）。
///
/// 定义位置刻意在 `NativeViewEngine` 之前：它的 `poll_watches()` 要按值返回本类型的容器。
struct WatchFd {
    int fd = -1;  ///< 待监听的文件描述符（-1 表示无效条目，宿主跳过）
    bool readable = true;  ///< 是否关心可读事件
    bool writable = false;  ///< 是否关心可写事件
};

/// @brief Web 引擎的进程级抽象（平台实现各自一个，经 `install_engine` 注入）。
///
/// 能力探测**只做一次**并缓存（`probe()` 结果即缓存值），禁止每帧 `dlopen` /
/// `LoadLibrary`。
class NativeViewEngine {
  public:
    /// @brief 虚析构。
    virtual ~NativeViewEngine() = default;

    NativeViewEngine() = default;

    /// @brief 禁复制/移动：进程级引擎经 `shared_ptr` 单例管理，拷贝会产生第二份加载句柄。
    NativeViewEngine(const NativeViewEngine &) = delete;
    /// @brief 禁移动构造（理由同拷贝构造）。
    NativeViewEngine(NativeViewEngine &&) = delete;
    /// @brief 禁复制赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const NativeViewEngine &) -> NativeViewEngine & = delete;
    /// @brief 禁移动赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(NativeViewEngine &&) -> NativeViewEngine & = delete;

    /// @brief 引擎种类标识。
    /// @return 本实现的引擎枚举值。
    [[nodiscard]] virtual auto id() const -> WebViewEngine = 0;

    /// @brief 能力探测（缓存式：实现须自行缓存首次结果，本函数可被调用多次）。
    /// @return 本进程此刻的能力快照。
    [[nodiscard]] virtual auto probe() const -> WebViewCapabilities = 0;

    /// @brief 创建一个原生视图实例并挂到给定父窗口下。
    ///
    /// 两种承载模式都需要一个视图：交互模式由它承载交互，快照模式由它提供**像素来源**
    /// （引擎侧的捕获入口挂在实例上，无实例即无像素）。区别只在可见性——快照模式的视图
    /// 由宿主按 `register_view` 的 `show_natively = false` 保持隐藏，像素只经 `capture` 出。
    ///
    /// 实现契约：新建视图缺省**不可见**（可见性一律由宿主经 `set_native_visible` 授予，
    /// 控件侧不得自显），否则宿主的首帧同步无从知道它已经露出。
    /// @param parent_window 宿主窗口的原生句柄（`HWND` / X11 `Window` / `GtkWidget`，
    ///        由各实现自行解释；为 nullptr 时实现应返回 nullptr 而非崩溃）。
    /// @return 新视图的宿主侧句柄；本平台不支持或创建失败时返回 nullptr。
    [[nodiscard]] virtual auto create_view(void *parent_window) -> std::unique_ptr<WebViewSink> = 0;

    /// @brief 需要并入宿主事件等待的 fd 集合（Linux/WebKitGTK 覆写；其余平台取默认空）。
    ///
    /// WebKitGTK 需要 GTK 自己的循环推进，其 fd 与超时必须并入宿主 `poll(2)`；
    /// 该并入面由宿主经 `NativeViewHost::poll_watches()` 暴露给 `Surface::wait_events`，
    /// 语义与既有 AT-SPI 桥的 `AtspiBridge::poll_watches()` 完全同构。
    /// @return 待监听的 fd 条目列表；默认实现返回空（Windows / macOS 引擎无需并入）。
    [[nodiscard]] virtual auto poll_watches() const -> std::vector<WatchFd> { return {}; }

    /// @brief 推进一次引擎侧事件循环（与 `poll_watches()` 成对：fd 就绪后调用）。
    /// @note 默认实现为空操作（Windows / macOS 引擎由 OS 消息泵驱动，无需宿主推进）。
    virtual auto pump_events() -> void {}
};

/// @brief 安装进程级引擎（可重复调用；后装覆盖先装）。
/// @param engine 引擎实现（传 nullptr 撤销安装，能力回落到「无引擎」）。
/// @note Thread: main-thread only
auto install_engine(std::shared_ptr<NativeViewEngine> engine) -> void;

/// @brief 当前生效引擎（未安装时为 nullptr）。
/// @return 引擎共享指针（可空）。
[[nodiscard]] auto installed_engine() -> std::shared_ptr<NativeViewEngine>;

/// @brief 安装 Null 引擎（确定性桩：任何模式都不可用，供无头 / WASM / CI 走降级路径）。
///
/// 与「不安装任何引擎」的差别只在能力来源可被显式读出（`engine == Null` 而非「无引擎」），
/// 便于测试断言降级路径确实走了引擎裁决而非漏装。
/// @note Thread: main-thread only
auto install_null_engine() -> void;

/// @brief 进程级能力裁决（引擎未安装时返回 `Disabled` 快照）。
/// @return 当前能力快照；调用方据此决定降级形态，不做编译期分支。
[[nodiscard]] auto current_capabilities() -> WebViewCapabilities;

/// @brief WebView 宿主：一张「原生子视图登记表」+ 每帧几何同步。
///
/// 由 `WindowHost` 持有一份；控件在挂载期经线程局部访问口登记自己、卸载期注销。
/// 登记与注销必须成对（同一纪律于 `Animator::remove`）。
class NativeViewHost {
  public:
    /// @brief 默认构造（无窗口句柄；`bind_window()` 后才有下发能力）。
    ///
    /// 定义在实现文件而非类内 `= default`：`entries_` 是 `vector<unique_ptr<Entry>>`，而 `Entry`
    /// 只在本头前向声明。类内默认定义会在**每个包含本头的 TU** 里实例化 vector 的析构，
    /// 进而对不完整类型求 `sizeof` —— 典型的 pimpl 陷阱。把构造函数体也放进实现文件后，
    /// 所有触碰 `Entry` 的实例化都收束到那一个 TU。
    NativeViewHost();
    NativeViewHost(const NativeViewHost &) = delete;
    auto operator=(const NativeViewHost &) -> NativeViewHost & = delete;
    NativeViewHost(NativeViewHost &&) = delete;
    auto operator=(NativeViewHost &&) -> NativeViewHost & = delete;
    /// @brief 析构：解绑全部登记条目（控件若先于宿主销毁，须已自行注销）。
    ~NativeViewHost();

    /// @brief 线程局部访问口：当前帧生效的宿主（未安装时为 nullptr）。
    /// @return 宿主指针（可空）。
    [[nodiscard]] static auto current() noexcept -> NativeViewHost *;
    /// @brief 安装 / 恢复线程局部宿主槽（帧首安装、帧尾置空）。
    /// @param host 宿主指针；nullptr 表示恢复（帧尾）。
    static auto set_current(NativeViewHost *host) noexcept -> void;

    /// @brief 绑定宿主窗口的原生句柄与换算参数（由 `WindowHost` 在接线期调用一次）。
    /// @param native_window 宿主窗口原生句柄（各平台自行解释）。
    /// @param scale_factor_provider 设备像素比取值的回调（帧内现取，避免缓存过期）。
    /// @param content_inset_provider CSD 安全区内边距取值的回调（同上）。
    auto bind_window(void *native_window, std::function<float()> scale_factor_provider,
                     std::function<EdgeInsets()> content_inset_provider) -> void;

    /// @brief 是否已绑定宿主窗口。
    /// @return `native_window` 非空为 true。
    [[nodiscard]] auto bound() const -> bool { return native_window_ != nullptr; }

    /// @brief 宿主窗口的原生句柄（交给引擎 `create_view` 作父窗口；未绑定时为 nullptr）。
    ///
    /// 返回类型为 `void *` 而非各平台的 `HWND` / `Window` / `GtkWidget`，是为了让公共头零平台污染
    /// （`check_platform_macros` 的口径）；类型解释责任在引擎实现侧，宿主全程只做非空与传递。
    [[nodiscard]] auto native_window() const -> void * { return native_window_; }

    /// @brief 登记后可回访本宿主的票据（控件侧应持 `weak_ptr`，回访前 `lock()` 判活）。
    ///
    /// 用途见 `NativeViewTicket`：控件析构时刻宿主未必还活着，裸 `this` 回访是悬垂访问。
    /// @return 指向本宿主的票据（与本宿主同生共死的控制块）。
    [[nodiscard]] auto ticket() const -> std::shared_ptr<NativeViewTicket>;

    /// @brief 登记一个原生子视图（控件挂载期调用一次）。
    /// @param owner 归属控件（每帧经其 `window_bounds()` 取窗口逻辑矩形；必须非空）。
    /// @param view 引擎视图实例（可为空——降级路径下无视图，仅登记占位以便同步空转）。
    /// @param show_natively 是否允许宿主把该视图置为原生可见。交互模式传 true；快照模式传
    ///        false——视图仍收几何下发（页面布局尺寸由它决定、捕获像素的尺寸也由它决定），
    ///        但**永不**收到 `set_native_visible(true)`，像素只经 `capture` 出帧。
    /// @return 登记号（供 `unregister_view` 注销）；未绑定宿主窗口或 owner 为空时返回 `std::nullopt`。
    [[nodiscard]] auto register_view(WebView *owner, std::unique_ptr<WebViewSink> view, bool show_natively = true)
        -> std::optional<std::size_t>;

    /// @brief 注销一个登记条目（控件卸载期调用；重复注销幂等）。
    /// @param token `register_view` 返回的登记号。
    auto unregister_view(std::size_t token) -> void;

    /// @brief 登记条目的引擎视图（控件取自己像素来源的唯一入口）。
    ///
    /// 视图所有权在宿主（`detach()` 由宿主在摘除时调一次），控件只借指针用。经宿主回访而非
    /// 控件自持的第二个理由：宿主先亡时票据失效，本入口根本到不了（见 `NativeViewTicket`）。
    /// @param token 登记号。
    /// @return 视图裸指针；条目不存在、条目无视图（降级登记）时为 nullptr。
    [[nodiscard]] auto view_of(std::size_t token) -> WebViewSink *;

    /// @brief 当前登记条目数（测试与诊断用）。
    /// @return 登记表中非空条目数。
    [[nodiscard]] auto view_count() const -> std::size_t;

    /// @brief 每帧几何同步：遍历登记表，重算物理矩形并**仅在变化时**下发。
    ///
    /// 触发门控（`geometry_may_have_changed`）：`Widget::window_bounds()` 是查询时现算的
    /// O(树深 × 每层子节点数) 上溯，其契约明确禁止进入每帧路径；故宿主只在「本帧发生过
    /// 布局 / 窗口几何变化 / 缩放变化」时传入 true，纯绘制帧直接跳过。
    ///
    /// 两条判据**各自独立**：矩形只在变化时下发（去重），可见性只在翻转时下发（边沿触发）。
    /// 二者不得合并——控件「隐去后再现」时矩形往往一字未改，若可见性恢复挂在矩形变化的
    /// 分支里就被去重吞掉，原生视图此后永久不可见，而下发计数与矩形序列全都看着正确。
    ///
    /// @param geometry_may_have_changed 本帧几何是否可能已变化（false 时跳过同步）。
    /// @return 本次实际下发了新矩形的条目数（去重后）。
    /// @note Thread: main-thread only
    /// @note Side-effects: 经 `NativeViewSink` 基类面改动平台视图的几何与可见性
    auto sync_geometry(bool geometry_may_have_changed) -> std::size_t;

    /// @brief 需要并入宿主事件等待的 fd 集合（Linux/WebKitGTK 侧非空；其余平台为空）。
    /// @return 待监听的 fd 条目列表。
    [[nodiscard]] auto poll_watches() const -> std::vector<WatchFd>;

    /// @brief 推进引擎侧事件泵（Linux/WebKitGTK 需要；其余平台为空操作）。
    ///
    /// 与 `poll_watches()` 成对：前者告诉宿主「等哪些 fd」，后者在 fd 就绪后推进一次
    /// 引擎循环。既有 AT-SPI 桥同此二段式。
    auto pump_engine_events() -> void;

    /// @brief 上次同步时下发的矩形（测试断言用；未同步过为 `std::nullopt`）。
    /// @param token 登记号。
    /// @return 上次下发的物理矩形；该条目不存在或未下发过时为空。
    [[nodiscard]] auto last_dispatched_rect(std::size_t token) const -> std::optional<NativeViewRect>;

  private:
    struct Entry;

    auto entry_by_token(std::size_t token) -> Entry *;
    [[nodiscard]] auto entry_by_token(std::size_t token) const -> const Entry *;

    void *native_window_ = nullptr;
    std::function<float()> scale_factor_provider_;
    std::function<EdgeInsets()> content_inset_provider_;
    std::vector<std::unique_ptr<Entry>> entries_;
    /// 自持的存活票据（析构到成员时释放，控件侧 `weak_ptr` 随即失效）。
    /// 声明位置必须在 `entries_` 之后：成员逆序销毁时它最后失效，而条目本身不引用票据。
    std::shared_ptr<NativeViewTicket> ticket_;
};

}  // namespace aurora::webview
