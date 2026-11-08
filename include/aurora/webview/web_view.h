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
/// au::webview::WebView(au::webview::WebViewProps().set_url("...").set_mode(WebViewMode::Snapshot))
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
#include <string_view>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/json.h"
#include "aurora/core/types.h"
#include "aurora/webview/native_view_host.h"
#include "aurora/widget/props.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora::webview {

/// @brief WebView 承载模式（双模；由使用方显式声明，框架不自动推断）。
enum class WebViewMode : std::uint8_t {
    Interactive,  ///< 交互模式：原生子视图承载，交互原生质量，不参与 Painter 合成
    Snapshot,  ///< 快照模式：内容快照进 Painter 帧流，可裁剪可比对，无交互
};

/// @brief 承载模式 → JSON 字符串（线上形态沿用枚举成员名的大写拼法，与迁移前逐字节一致）。
/// @param v 承载模式枚举值。
/// @return `"Interactive"` 或 `"Snapshot"`。
[[nodiscard]] inline auto mode_to_json(WebViewMode v) -> Json {
    return v == WebViewMode::Snapshot ? "Snapshot" : "Interactive";
}

/// @brief JSON → 承载模式（非字符串或未知值回退交互档，与迁移前的宽容解析同一档）。
/// @param j 模式字符串。
/// @return 匹配的模式；未知名返回 `WebViewMode::Interactive`。
[[nodiscard]] inline auto mode_from_json(const Json &j) -> WebViewMode {
    if (j.is_string() && j.as_or<std::string>("") == "Snapshot") {
        return WebViewMode::Snapshot;
    }
    return WebViewMode::Interactive;
}

}  // namespace aurora::webview

namespace aurora {

/// @brief WebView 承载模式的属性编解码登记（枚举名走宽容通路：退默认档、照常表态、不上报）。
template <>
struct PropCodec<webview::WebViewMode>
    : detail::EnumPropCodec<webview::WebViewMode, &webview::mode_to_json, &webview::mode_from_json> {
    /// @brief schema 的 C++ 类型名。
    /// @return `WebViewMode`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "WebViewMode"; }

    /// @brief 合法值表（发射 schema 的 `enum` 字段；拼写沿用枚举成员名）。
    /// @return `{"Interactive", "Snapshot"}`。
    [[nodiscard]] static auto enum_names() -> std::vector<std::string_view> { return {"Interactive", "Snapshot"}; }
};

}  // namespace aurora

namespace aurora::webview {

/// @brief WebView 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 三态语义与迁移前一致，但「未设」不再靠 `std::optional` 外泄表达：
/// - **可空行**（全部七行）：未表态时取该行回落点（与迁移前字段声明处的默认值逐值相同），表态后不再跟随；
///   撤表态走公共面上唯一的 `clear(row)`；
/// - **常量档**（七行全是）：回落点都是库内常量提供者，没有一行随作用域换肤，故读件一律无参。
///
/// 档位口径：只有 `mode` 与 `background` 挂 `.paint()`——前者决定 `on_paint` 走快照 blit 还是填底色，
/// 后者是这两条路径共用的底色。其余五行（`url` / `html` / `base_uri` / `zoom_factor` /
/// `javascript_enabled`）只被 `on_mount` 一次性推给原生视图，`on_layout` 与 `on_paint` 都不读它们，
/// 故不挂任何档位：写入不标脏，与迁移前「直写公有字段、从不标脏」的可观察行为一致。
///
/// @note Thread: main-thread only
/// @note Side-effects: 挂档位的两行经宿主链接位标脏；未挂载时是纯 DTO
class WebViewProps : public AbstractProps<WebViewProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：初始导航目标。只被 `on_mount` 消费 ⇒ 不挂档位；回落点是空串。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto url_row() {
        return prop_row<&WebViewProps::url_>("url")
            .fallback(&WebViewProps::url_default)
            .note("Initial navigation target (empty = no initial load)");
    }

    /// @brief 可空值件：内联 HTML（`url` 非空时被忽略）。不挂档位；回落点是空串。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto html_row() {
        return prop_row<&WebViewProps::html_>("html")
            .fallback(&WebViewProps::html_default)
            .note("Inline HTML (ignored when url is non-empty)");
    }

    /// @brief 可空值件：内联 HTML 的基 URI。不挂档位；回落点是空串。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto base_uri_row() {
        return prop_row<&WebViewProps::base_uri_>("base_uri")
            .fallback(&WebViewProps::base_uri_default)
            .note("Base URI for resolving relative resources of inline HTML");
    }

    /// @brief 可空值件：承载模式。决定 `on_paint` 的分支 ⇒ 绘制档；回落点是常量交互档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto mode_row() {
        return prop_row<&WebViewProps::mode_>("mode")
            .paint()
            .fallback(&WebViewProps::mode_default)
            .note("Interactive = native child view (no Painter compositing); Snapshot = blit into the Painter frame");
    }

    /// @brief 可空值件：页面缩放系数。只被 `on_mount` 推给视图 ⇒ 不挂档位；下界 0.25、上界 5。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto zoom_factor_row() {
        return prop_row<&WebViewProps::zoom_factor_>("zoom_factor")
            .bounds("0.25", "5")
            .fallback(&WebViewProps::zoom_factor_default)
            .note("Page zoom factor (1 = natural size)");
    }

    /// @brief 可空值件：是否允许页面执行脚本。只被 `on_mount` 推给视图 ⇒ 不挂档位；回落点为允许。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto javascript_enabled_row() {
        return prop_row<&WebViewProps::javascript_enabled_>("javascript_enabled")
            .fallback(&WebViewProps::javascript_enabled_default)
            .note("Whether the page may execute scripts");
    }

    /// @brief 可空值件：背景色（占位与快照模式的填底色）。绘制读它 ⇒ 绘制档；回落点是常量浅灰。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto background_row() {
        return prop_row<&WebViewProps::background_>("background")
            .paint()
            .fallback(&WebViewProps::background_default)
            .note("Backdrop color for the placeholder and the snapshot mode");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(url_row(), html_row(), base_uri_row(), mode_row(), zoom_factor_row(),
                              javascript_enabled_row(), background_row());
    }

    // ---- 读面：每行恰一件，七行皆常量档 ⇒ 一律无参 ----

    /// @brief 当前生效的初始导航目标。
    /// @return 目标 URI 的副本；未表态时为空串。
    [[nodiscard]] auto url() const -> std::string { return stated(url_row()); }

    /// @brief 当前生效的内联 HTML。
    /// @return HTML 文本的副本；未表态时为空串。
    [[nodiscard]] auto html() const -> std::string { return stated(html_row()); }

    /// @brief 当前生效的基 URI。
    /// @return 基 URI 的副本；未表态时为空串。
    [[nodiscard]] auto base_uri() const -> std::string { return stated(base_uri_row()); }

    /// @brief 当前生效的承载模式。
    /// @return 生效模式；未表态时为交互档。
    [[nodiscard]] auto mode() const -> WebViewMode { return stated(mode_row()); }

    /// @brief 当前生效的页面缩放系数。
    /// @return 生效系数；未表态时为 1.0。
    [[nodiscard]] auto zoom_factor() const -> float { return stated(zoom_factor_row()); }

    /// @brief 当前生效的脚本开关。
    /// @return 允许页面执行脚本为 true；未表态时亦为 true。
    [[nodiscard]] auto javascript_enabled() const -> bool { return stated(javascript_enabled_row()); }

    /// @brief 当前生效的背景色。
    /// @return 生效底色；未表态时为浅灰 `{245,245,245,255}`。
    [[nodiscard]] auto background() const -> Color { return stated(background_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入初始导航目标（表态即脱离空串基线）。
    /// @param value 目标 URI；空串表示暂不加载。
    /// @return 引用本属性对象，便于链式续写。
    auto set_url(const std::string &value) -> WebViewProps & { return assign(url_row(), value); }

    /// @brief 装入内联 HTML。
    /// @param value HTML 文本；`url` 非空时被忽略。
    /// @return 引用本属性对象，便于链式续写。
    auto set_html(const std::string &value) -> WebViewProps & { return assign(html_row(), value); }

    /// @brief 装入基 URI。
    /// @param value 相对资源的解析基准（可空）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_base_uri(const std::string &value) -> WebViewProps & { return assign(base_uri_row(), value); }

    /// @brief 装入承载模式；表态即脱离交互档基线。
    /// @param value 新模式。
    /// @return 引用本属性对象，便于链式续写。
    auto set_mode(WebViewMode value) -> WebViewProps & { return assign(mode_row(), value); }

    /// @brief 装入页面缩放系数。
    /// @param value 新系数；写件不夹取（与迁移前直写公有字段同形），0.25–5 这界只约束反序列化与 schema 面。
    /// @return 引用本属性对象，便于链式续写。
    auto set_zoom_factor(float value) -> WebViewProps & { return assign(zoom_factor_row(), value); }

    /// @brief 装入脚本开关。
    /// @param value 是否允许页面执行脚本。
    /// @return 引用本属性对象，便于链式续写。
    auto set_javascript_enabled(bool value) -> WebViewProps & { return assign(javascript_enabled_row(), value); }

    /// @brief 装入背景色；表态即脱离浅灰基线。
    /// @param value 新底色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_background(Color value) -> WebViewProps & { return assign(background_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 导航目标回落基线。
    /// @return 空串（暂不加载）。
    [[nodiscard]] static auto url_default() -> std::string { return {}; }

    /// @brief 内联 HTML 回落基线。
    /// @return 空串（无内联内容）。
    [[nodiscard]] static auto html_default() -> std::string { return {}; }

    /// @brief 基 URI 回落基线。
    /// @return 空串（按文档自身基准解析）。
    [[nodiscard]] static auto base_uri_default() -> std::string { return {}; }

    /// @brief 承载模式回落基线。
    /// @return `WebViewMode::Interactive`（默认交互档）。
    [[nodiscard]] static auto mode_default() -> WebViewMode { return WebViewMode::Interactive; }

    /// @brief 缩放系数回落基线。
    /// @return 1.0F（原始尺寸）。
    [[nodiscard]] static auto zoom_factor_default() -> float { return 1.0F; }

    /// @brief 脚本开关回落基线。
    /// @return true（允许执行脚本）。
    [[nodiscard]] static auto javascript_enabled_default() -> bool { return true; }

    /// @brief 背景色回落基线。
    /// @return `{245,245,245,255}`（浅灰不透明）。
    [[nodiscard]] static auto background_default() -> Color { return Color{245, 245, 245, 255}; }

  private:
    std::optional<std::string> url_;  ///< 导航目标表态位
    std::optional<std::string> html_;  ///< 内联 HTML 表态位
    std::optional<std::string> base_uri_;  ///< 基 URI 表态位
    std::optional<WebViewMode> mode_;  ///< 承载模式表态位
    std::optional<float> zoom_factor_;  ///< 页面缩放系数表态位
    std::optional<bool> javascript_enabled_;  ///< 脚本开关表态位
    std::optional<Color> background_;  ///< 背景色表态位

    friend class aurora::AbstractProps<WebViewProps>;
};

/// @brief Web 内容 widget（specification/04-widget.md §3.6）。
///
/// 采用**继承式双模 API**（specification/04-widget.md §2.5）的属性表形态：本控件公有继承
/// `WebViewProps`，故读写件都直接可用（`wv.set_zoom_factor(1.5F)` 写、`wv.zoom_factor()` 读），
/// 也可整份配置块经构造传入（`au::webview::WebView(au::webview::WebViewProps().set_url("..."))`）。
/// 属性字段已是私有的，`wv.url = "..."` 与 `WebViewProps{ .url = "..." }` 都不再合法。
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

    /// @brief 默认构造：七行全未表态（生效值即各行回落点），并接线宿主。
    WebView() { bind(*this); }
    /// @brief 析构：把自己从宿主登记表摘掉。
    ///
    /// 框架在拆树时**不代调**子件 `on_unmount`（见 `Container::on_unmount` 的口径：摘除时刻
    /// 容器无从判断这只子件是否仍在容器之外被持有），故「控件先亡」这一最常见顺序不会走
    /// `on_unmount`。而登记表条目持有的是裸 `WebView*`，宿主的每帧几何同步要解引用它——
    /// 不在此处注销，下一次同步即悬垂访问。注销本身经 `NativeViewTicket` 判活，宿主先亡时
    /// 只是什么都不做。
    ~WebView() override;
    /// @brief 以 URL 构造（其余行不表态，生效值即各行回落点），并接线宿主。
    /// @param url 初始导航目标。
    explicit WebView(std::string url) {
        set_url(std::move(url));
        bind(*this);
    }
    /// @brief 配置块构造：整份属性接管后接线宿主。
    /// @param props WebView 属性，移动进基类子对象。
    WebView(WebViewProps props) : WebViewProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `WebViewProps`：七行皆未表态，生效值即各行回落点。
    [[nodiscard]] static auto defaults() -> WebViewProps { return {}; }

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

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "WebView" 的描述表：`properties` 按 `rows()` 点名顺序，无事件，子策略 none。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例级自描述：转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：本表七行皆为值形态，没有可登记的来源。
    /// @param out 信号采集出口；本控件不向其写入任何信号。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 序列化自有属性：先由基类写通用属性，再写权威表里已表态的行。
    /// @param props 输出目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从 JSON 重建自有属性：表内键逐行装入，键缺失保留现值，非法值撤表态。
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
    /// @param code 冻结 slug（驱动 severity / domain / kind 表注入）。
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
