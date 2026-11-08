// webview/web_view.cpp — WebView 控件本体。
//
// 本 TU 只做三件事：① 按场景声明选择承载模式；② 把几何与生命周期交给宿主统一管理；
// ③ 在引擎不可用时降级为确定性占位。**不含任何平台分支**——平台差异全在 engine_*.cpp。

#include "aurora/webview/web_view.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "aurora/core/diagnostics.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"  // WidgetDescriptor（属性条目由权威表发射，本 TU 不再手写）

namespace aurora::webview {

auto WebView::capabilities() const -> WebViewCapabilities { return current_capabilities(); }

WebView::~WebView() { detach_from_host(); }

auto WebView::detach_from_host() -> void {
    if (!host_token_.has_value()) {
        return;  // 从未登记（含整段降级路径）
    }
    if (const std::shared_ptr<NativeViewTicket> ticket = host_ticket_.lock();
        ticket != nullptr && ticket->host != nullptr) {
        ticket->host->unregister_view(*host_token_);
    }
    host_token_.reset();
    host_ticket_.reset();
}

auto WebView::registered_sink() -> WebViewSink * {
    if (!host_token_.has_value()) {
        return nullptr;
    }
    const std::shared_ptr<NativeViewTicket> ticket = host_ticket_.lock();
    if (ticket == nullptr || ticket->host == nullptr) {
        return nullptr;  // 宿主先亡：视图随之销毁，本函数无从给出可解引用的对象
    }
    return ticket->host->view_of(*host_token_);
}

auto WebView::load_url(std::string uri) -> void {
    // 形参是本函数持有的副本，先落成局部（一次真移动），再由写件装入表态位。
    // string 行的写件与其余已迁型同形态——只收 `const std::string &`，装入即一次拷贝。
    const std::string target = std::move(uri);
    set_url(target);
    // 无视图时只保留意图：`url` 行仍可被序列化读出，不因缺引擎而报错（验收判据 5）。
    // 意图同时是挂载时的初始导航来源，故「先构造后挂载」与「挂载后调用」两条路都真加载。
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->navigate(target);
    }
}

auto WebView::load_html(std::string html_text, std::string uri_base) -> void {
    const std::string document = std::move(html_text);  // 同 `load_url`：局部一次真移动，写件一次拷贝
    const std::string base = std::move(uri_base);
    set_html(document);
    set_base_uri(base);
    // `url` 非空时按属性声明的优先级忽略内联 HTML（与挂载期初始导航同一口径）。
    if (WebViewSink *sink = registered_sink(); sink != nullptr && url().empty()) {
        sink->navigate_to_string(document, base);
    }
}

auto WebView::reload() -> void {
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->reload();
    }
}
auto WebView::stop() -> void {
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->stop();
    }
}
auto WebView::go_back() -> void {
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->go_back();
    }
}
auto WebView::go_forward() -> void {
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->go_forward();
    }
}
auto WebView::post_json(const std::string &json) -> void {
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->post_message(json);
    }
}

auto WebView::evaluate_script(const std::string &js, std::function<void(std::string)> done) -> void {
    if (WebViewSink *sink = registered_sink(); sink != nullptr) {
        sink->evaluate_script(js, std::move(done));
        return;
    }
    // 没有视图就绝不可能有结果。此处**必须**让回调落地：脚本结果的失败编码本就是空串
    // （见 `WebViewSink::evaluate_script` 的契约），回空串是如实报告，而不回才是让调用方
    // 的续延永久悬空。
    if (done) {
        done(std::string{});
    }
}

auto WebView::request_snapshot(std::function<void(const Image &)> done) -> void {
    if (!current_capabilities().snapshot) {
        Diagnostics::warn("webview snapshot requested but no engine provides it; degrading to empty", "WebView",
                          "webview-snapshot-unsupported");
        if (done) {
            done(Image{});
        }
        return;
    }
    WebViewSink *sink = registered_sink();
    if (sink == nullptr) {
        // 能力说得出、视图却没登记上：宿主未绑定或挂载尚未发生。同样回空图并留一次诊断，
        // 不得让调用方的回调悬着——「没收到回调」与「收到空图」是两种状态，只有后者可读。
        Diagnostics::warn("webview snapshot requested without a registered native view", "WebView",
                          "webview-host-unbound");
        if (done) {
            done(Image{});
        }
        return;
    }
    sink->capture([this, done = std::move(done)](const Image &bitmap) -> void {
        if (!bitmap.pixels.empty()) {
            snapshot_ = bitmap;  // 只有成功捕获才覆盖上一次的好图（失败不得把已有像素丢掉）
        }
        if (done) {
            done(snapshot_);
        }
    });
}

auto WebView::describe_static() -> WidgetDescriptor {
    auto descriptor = props_descriptor("WebView");
    descriptor.children_policy = "none";
    descriptor.examples = {"au::webview::WebView(au::webview::WebViewProps().set_url(\"https://example.com\"))"};
    return descriptor;
}

auto WebView::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props_serialize(props);
}

auto WebView::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    props_deserialize(props);
}

auto WebView::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    static constexpr float DEFAULT_WIDTH = 320.0F;
    static constexpr float DEFAULT_HEIGHT = 240.0F;
    const float w = width_.kind == LengthKind::Fixed ? std::max(c.min.width, std::min(width_.value, c.max.width))
                                                     : std::max(c.min.width, std::min(DEFAULT_WIDTH, c.max.width));
    const float h = height_.kind == LengthKind::Fixed ? std::max(c.min.height, std::min(height_.value, c.max.height))
                                                      : std::max(c.min.height, std::min(DEFAULT_HEIGHT, c.max.height));
    return c.constrain(Size{.width = w, .height = h});
}

auto WebView::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    if (degraded_) {
        paint_placeholder(p, bounds);
        return;
    }
    if (mode() == WebViewMode::Snapshot && !snapshot_.pixels.empty()) {
        p.draw_image(snapshot_, bounds);
        return;
    }
    // 交互模式：原生视图盖在本矩形之上，此处填底只为「原生视图未就位时不露脏区」，
    // 且保证该矩形的绘制结果是确定性的（可进逻辑快照）。
    p.draw_rect(bounds, background());
}

auto WebView::on_mount(const BuildContext &ctx) -> void {
    Widget::on_mount(ctx);
    degraded_ = false;
    degraded_reported_ = false;  // 告警的计数单位是「每次挂载一条」，跨挂载不得沿用旧值

    const WebViewCapabilities caps = current_capabilities();
    if (caps.unavailable()) {
        // 两种模式都不提供的后端：按可用性裁决归因，让 `UnsupportedPlatform`（平台/后端本身
        // 不给）与 `Disabled` / `MissingRuntime`（运行时缺席）在诊断面上可区分。
        const std::string_view code = caps.availability == WebViewAvailability::UnsupportedPlatform
                                          ? "webview-unsupported-mode"
                                          : "webview-runtime-missing";
        enter_degraded(code, "no web engine provides either mode");
        return;
    }
    NativeViewHost *host = NativeViewHost::current();
    if (host == nullptr || !host->bound()) {
        enter_degraded("webview-host-unbound", "no native window host bound for this frame");
        return;
    }
    if (mode() == WebViewMode::Interactive && !caps.interactive) {
        enter_degraded("webview-unsupported-mode", "interactive mode needs a native child surface");
        return;
    }
    if (mode() == WebViewMode::Snapshot && !caps.snapshot) {
        enter_degraded("webview-snapshot-unsupported", "snapshot mode unavailable");
        return;
    }

    // 两种模式都建视图并登记：交互模式由视图承载交互，快照模式由视图提供**像素来源**
    // （引擎的捕获入口挂在实例上，无实例即无像素）。二者的差别只在可见性授权——快照模式
    // 的视图恒不可见，像素只经 `capture` 出帧。
    std::unique_ptr<WebViewSink> view;
    if (const std::shared_ptr<NativeViewEngine> &engine = installed_engine(); engine != nullptr) {
        view = engine->create_view(host->native_window());
    }
    // 票据与登记号成对留存：析构期的注销只能经票据判活后回访宿主（见 `detach_from_host`）。
    host_ticket_ = host->ticket();
    host_token_ = host->register_view(this, std::move(view), mode() == WebViewMode::Interactive);

    WebViewSink *sink = registered_sink();
    if (sink == nullptr) {
        return;  // 引擎未能建出视图（或宿主拒绝登记）：意图留在属性上，挂载不报错
    }
    // 入向回调交弱引用：控件析构后引擎的异步回调 `lock()` 即空，天然判活。
    sink->set_callbacks(callbacks_);
    sink->set_zoom_factor(zoom_factor());
    sink->set_javascript_enabled(javascript_enabled());
    // 初始导航：`url` 优先（非空时按属性声明忽略内联 HTML），二者皆空则停在空白页。
    // 读件返回副本，故整段导航只取一次，避免逐条比较各复制一遍字符串。
    const std::string initial_url = url();
    if (!initial_url.empty()) {
        sink->navigate(initial_url);
    } else if (const std::string initial_html = html(); !initial_html.empty()) {
        sink->navigate_to_string(initial_html, base_uri());
    }
}

auto WebView::on_unmount(const BuildContext &ctx) -> void {
    detach_from_host();
    Widget::on_unmount(ctx);
}

auto WebView::enter_degraded(std::string_view code, std::string_view detail) -> void {
    degraded_ = true;
    if (degraded_reported_) {
        return;  // 幂等：同一次挂载只报一条
    }
    degraded_reported_ = true;
    Diagnostics::warn(std::string{"webview degraded: "}.append(detail), "WebView", code);
}

auto WebView::paint_placeholder(Painter &p, const Rect &bounds) -> void {
    // 确定性两笔：先填边框色整块，再以内缩 1dp 的底色覆盖，得到一圈 1dp 边框。
    // 只用 draw_rect 而不用描边接口，是为了让占位外观与后端抗锯齿实现无关（可进 golden）。
    p.draw_rect(bounds, Color{160, 160, 160, 255});
    if (bounds.size.width > 2.0F && bounds.size.height > 2.0F) {
        p.draw_rect(Rect{.origin = Point{.x = bounds.origin.x + 1.0F, .y = bounds.origin.y + 1.0F},
                         .size = Size{.width = bounds.size.width - 2.0F, .height = bounds.size.height - 2.0F}},
                    background());
    }
}

}  // namespace aurora::webview
