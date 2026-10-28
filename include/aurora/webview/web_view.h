#pragma once

/// @brief WebView 控件：把外部 Web 内容接进自绘 UI（交互 / 快照双模）。
/// @file web_view.h
///
/// 双模的选择由使用方按场景显式声明（不做自动推断）：
/// - `Interactive`：内容由 OS 原生视图承载，交互 / 输入法 / 无障碍全部原生质量，代价是
///   **不参与 Painter 合成**（恒在最顶，祖先的裁剪 / 透明度 / 变换对它无效，且它上面
///   画不了任何自绘内容）。
/// - `Snapshot`：内容经一次快照变成位图进 Painter 帧流，可被裁剪、叠字、可与
///   `HeadlessSurface` 的 golden 与逻辑快照逐像素比对，代价是无交互。
///
/// 两种模式在引擎不可用时**统一降级**为占位矩形：参与布局、画出占位外观、上报一次
/// 诊断，绝不抛异常、绝不阻塞树渲染（对齐 `media/audio.h` 的静默模式语义）。
///
/// 用法：
/// @code
/// au::webview::WebView{ au::webview::WebViewProps{ .url = "...", .mode = ... } }
/// @endcode
///
/// @note Thread: main-thread only
/// @note Side-effects: 交互模式下创建 / 移动原生子窗口；快照模式下发起异步快照
/// @note Rebuildable: yes, via from_json
///

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/webview/native_view_host.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora::webview {

/// @brief WebView 承载模式（双模；由使用方显式声明，框架不自动推断）。
enum class WebViewMode : std::uint8_t {
    Interactive,  ///< 交互模式：原生子视图承载，交互原生质量，不参与 Painter 合成
    Snapshot,  ///< 快照模式：内容快照进 Painter 帧流，可裁剪可比对，无交互
};

/// @brief WebView 属性（聚合；字段即控件公有字段，继承式双模 API）。
///
/// 全部字段带合理默认值且声明处可见，序列化往返按 `serialize_props` / `deserialize_props`。
struct WebViewProps {
    std::string url;  ///< 初始导航目标（空串表示暂不加载）；为非空时忽略 `html`
    std::string html;  ///< 内联 HTML（`url` 非空时被忽略）
    std::string base_uri;  ///< 内联 HTML 的基 URI（相对资源解析基准）
    WebViewMode mode = WebViewMode::Interactive;  ///< 承载模式
    float zoom_factor = 1.0F;  ///< 页面缩放系数（1.0 为原始尺寸）
    bool javascript_enabled = true;  ///< 是否允许页面执行脚本
    Color background{245, 245, 245, 255};  ///< 背景色（占位与快照模式的填底色）
};

/// @brief Web 内容 widget（specification/04-widget.md §3.6）。
///
/// 采用**继承式双模 API**（specification/04-widget.md §2.5）：`WebViewProps` 字段即本控件
/// 公有字段，可直接访问或以配置块构造。
///
/// 焦点契约：`WebView` **不进 `FocusManager` 焦点链**——交互模式的键盘焦点由原生视图
/// 向 OS 承担，Tab 序进出 Web 内容属引擎行为，首版不承诺 Aurora ↔ Web 的 Tab 遍历贯通。
/// 无障碍契约：交互模式下 Web 内部的无障碍树由引擎自带的 UIA / AT-SPI 提供，本库语义树
/// 不穿透，`WebView` 在语义树中是一个叶子节点。
class WebView : public Widget, public WebViewProps {
  public:
    /// @brief 纯展示 + 外部焦点：不进本库 Tab 焦点链（见类注释的焦点契约）。
    /// @return 恒 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return false; }

    WebView() = default;
    /// @brief 析构：把自己从宿主登记表摘掉。
    ///
    /// 框架在拆树时**不代调**子件 `on_unmount`（见 `Container::on_unmount` 的口径：摘除时刻
    /// 容器无从判断这只子件是否仍在容器之外被持有），故「控件先亡」这一最常见顺序不会走
    /// `on_unmount`。而登记表条目持有的是裸 `WebView*`，宿主的每帧几何同步要解引用它——
    /// 不在此处注销，下一次同步即悬垂访问。注销本身经 `NativeViewTicket` 判活，宿主先亡时
    /// 只是什么都不做。
    ~WebView() override;
    /// @brief 以 URL 构造（其余字段取默认）。
    /// @param url 初始导航目标。
    explicit WebView(std::string url) : WebViewProps{.url = std::move(url)} {}
    /// @brief 配置块构造：整份属性直接作为本控件字段。
    /// @param props WebView 属性，移动进基类子对象。
    WebView(WebViewProps props) : WebViewProps(std::move(props)) {}

    /// @brief 禁复制/移动：widget 经 `shared_ptr` 持有，拷贝会产生两份同源控件。
    WebView(const WebView &) = delete;
    /// @brief 禁移动构造（理由同拷贝构造）。
    WebView(WebView &&) = delete;
    /// @brief 禁复制赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const WebView &) -> WebView & = delete;
    /// @brief 禁移动赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(WebView &&) -> WebView & = delete;

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"WebView"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "WebView"; }

    /// @brief 当前生效能力快照（引擎未安装时为全 false 的 `Disabled` 快照）。
    /// @return 本进程此刻的能力；调用方据此判降级，不做编译期分支。
    [[nodiscard]] auto capabilities() const -> WebViewCapabilities;

    /// @brief 是否已降级为占位（能力不足或宿主未绑定）。
    /// @return 处于降级态为 true。
    [[nodiscard]] auto degraded() const -> bool { return degraded_; }

    // ---- 命令（无引擎时为空操作，仅保留意图；不因缺引擎而报错）----

    /// @brief 导航到指定 URI（无引擎时只记录意图，`url` 仍可被序列化读出）。
    /// @param uri 目标 URI。
    auto load_url(std::string uri) -> void;
    /// @brief 以内联 HTML 替换内容（无引擎时只记录意图）。
    /// @param html_text HTML 文本（形参加 `_text` 后缀以避免与成员 `html` 同名遮蔽）。
    /// @param uri_base 相对资源的基 URI（可空）。
    auto load_html(std::string html_text, std::string uri_base = {}) -> void;
    /// @brief 重新加载当前文档。
    auto reload() -> void;
    /// @brief 停止当前导航。
    auto stop() -> void;
    /// @brief 后退一页（无历史时为空操作）。
    auto go_back() -> void;
    /// @brief 前进一页（无历史时为空操作）。
    auto go_forward() -> void;
    /// @brief 向页面投递一条 JSON 消息（无引擎时为空操作）。
    /// @param json 待投递的 JSON 文本。
    auto post_json(const std::string &json) -> void;

    /// @brief 在页面中执行脚本（异步；无视图时立即以空串回调，即「结果失败」的既有编码）。
    /// @param js 待执行的脚本源码。
    /// @param done 结果回调（参数为返回值 JSON 文本；失败时为空串）。
    auto evaluate_script(const std::string &js, std::function<void(std::string)> done = {}) -> void;

    /// @brief 请求一次内容快照（快照模式的主入口；无引擎时回调空图并上报一次诊断）。
    /// @param done 快照完成回调（参数为位图；失败或未就绪时为空图）。
    auto request_snapshot(std::function<void(const Image &)> done = {}) -> void;

    /// @brief 当前快照位图（快照模式下最近一次成功快照的结果）。
    /// @return 位图常引用；从未成功快照时为空图。
    [[nodiscard]] auto snapshot_bitmap() const -> const Image & { return snapshot_; }

    // ---- 回调接线 ----
    //
    // 四个 setter 写的是同一份 `callbacks_`（控件自持 `shared_ptr`，视图只持 `weak_ptr`）：
    // 挂载前设置同样有效，因为写的是数据而非向引擎注册的订阅；引擎回调到达时经 `lock()`
    // 判活后读最新值，故挂载后再 `set_on_*` 也立即生效，不需要重挂。

    /// @brief 设置 Web → Native 消息回调。
    /// @param cb 回调（传空撤销）。
    auto set_on_web_message(WebViewMessageCallback cb) -> void { callbacks_->on_web_message = std::move(cb); }
    /// @brief 设置导航状态回调。
    /// @param cb 回调（传空撤销）。
    auto set_on_navigation(WebViewNavigationCallback cb) -> void { callbacks_->on_navigation = std::move(cb); }
    /// @brief 设置标题变化回调。
    /// @param cb 回调（传空撤销）。
    auto set_on_title_changed(WebViewTitleCallback cb) -> void { callbacks_->on_title_changed = std::move(cb); }
    /// @brief 设置引擎进程异常回调。
    /// @param cb 回调（传空撤销）。
    auto set_on_process_failed(WebViewProcessFailedCallback cb) -> void {
        callbacks_->on_process_failed = std::move(cb);
    }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "WebView" 的描述表，子节点策略 "none"。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例级自描述：转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：本控件无响应式属性。
    auto collect_signals(StateSink & /*out*/) -> void override {}

    /// @brief 序列化自有属性（先由基类写通用属性）。
    /// @param props 输出目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从 JSON 重建自有属性（键缺失或类型不符则保留现值）。
    /// @param props 序列化属性对象。
    auto deserialize_props(const Json &props) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;
    auto on_mount(const BuildContext &ctx) -> void override;
    auto on_unmount(const BuildContext &ctx) -> void override;

  private:
    /// @brief 本控件登记在宿主侧的引擎视图（快照像素来源；未登记或降级时为空）。
    ///
    /// 只经宿主票据回访取得：视图所有权在宿主，控件持裸指针仅在本帧内用。
    /// @return 引擎视图指针（内容通道与像素来源），或 nullptr（未登记 / 降级登记）。
    [[nodiscard]] auto registered_sink() -> WebViewSink *;

    /// @brief 从宿主登记表摘掉自己（`on_unmount` 与析构共用；未登记或宿主已亡时为空操作）。
    ///
    /// 回访宿主只经 `host_ticket_.lock()` 取得指针——析构时刻宿主可能已经先走一步，
    /// 票据失效即什么都不做，绝不解引用任何缓存的宿主地址。
    auto detach_from_host() -> void;

    /// @brief 进入降级态并上报一次诊断（幂等：同一次挂载只报一条）。
    /// @param code 冻结 slug（驱动 severity / category 表注入）。
    /// @param detail 人类可读的原因片段。
    auto enter_degraded(std::string_view code, std::string_view detail) -> void;

    /// @brief 占位外观：填底色 + 描边框（确定性，可进 golden）。
    /// @param p 绘制器。
    /// @param bounds 绘制矩形。
    auto paint_placeholder(Painter &p, const Rect &bounds) -> void;

    /// 四个入向回调的聚合，控件自持强引用（构造即存在，故 setter 挂载前后同样有效）。
    /// 视图侧只拿 `weak_ptr`：异步回调到达时 `lock()` 判活，控件已析构即静默丢弃。
    std::shared_ptr<WebViewCallbacks> callbacks_ = std::make_shared<WebViewCallbacks>();

    std::optional<std::size_t> host_token_;  ///< 宿主登记号（未登记为空）
    /// 挂载时所在宿主的回访票据（弱引用）。刻意不存裸宿主指针：卸载与析构都发生在
    /// 「宿主可能已经先走」的时刻（拆窗口与拆树的顺序不由本控件决定），判活是前置条件。
    std::weak_ptr<NativeViewTicket> host_ticket_;
    Image snapshot_;  ///< 最近一次成功的快照位图（快照模式）
    bool degraded_ = false;  ///< 是否处于降级态
    bool degraded_reported_ = false;  ///< 降级诊断是否已上报过（防重复）
};

}  // namespace aurora::webview
