/// 测试类型: unit
/// 目标单元: include/aurora/webview/web_view.h（控件降级矩阵与属性往返）
/// 测试说明: 核心承诺是「引擎不可用时降级而非崩溃」。本文件把四种可用性裁决逐条钉成
/// **可区分的量**：每种降级路径都必须① `degraded()` 为真；② 上报**恰好一条**归因正确的
/// 诊断；③ 不抛异常。四种 `WebViewAvailability` 各自成一行——`UnsupportedPlatform` 与
/// `Disabled` / `MissingRuntime` 在诊断 slug 上必须分叉，否则「平台不支持」会被误读成
/// 「忘了装运行时」，用户按提示去装运行时也不解决问题。
///
/// 另覆盖：属性的序列化往返逐值一致、每挂载恰好一条告警（不是一次性）、宿主登记表随
/// 显式卸载与树拆除两条路径回落（后者是框架不代调 `on_unmount` 的唯一防线）。
///
/// 判据纪律：不拿「没崩」当通过——每条都断言具体 slug；且用 `Diagnostics::take()` 取出的
/// 真实条目为准，不用 `count()` 的差值（后者会把别处遗留的诊断算进来）；告警条数按
/// 「本次挂载新产生的全部诊断」计，故 `take()` 兼作清场。

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/types.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/webview/native_view_host.h"
#include "aurora/webview/web_view.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/node.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_web_view {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 记录型视图桩：把控件经内容通道下发的每一件事照单记下，并可脚本化捕获产物。
///
/// 记的是**调用序列**而非次数——「挂载时先装回调、再设缩放、再初始导航」这类顺序本身就是
/// 契约（顺序错了，页面会按错尺寸排版一次再重排）。
class StubSink final : public webview::WebViewSink {
  public:
    auto set_native_rect(const webview::NativeViewRect & /*rect*/) -> void override {}
    auto set_native_visible(bool /*visible*/) -> void override {}
    auto capture(std::function<void(Image)> done) -> void override {
        log.emplace_back("capture");
        if (done) {
            done(next_image);  // 引擎契约：无论成败回调必达，失败即空图
        }
    }
    auto detach() -> void override { log.emplace_back("detach"); }

    auto navigate(const std::string &uri) -> void override { log.emplace_back("navigate:" + uri); }
    auto navigate_to_string(const std::string &html, const std::string &base_uri) -> void override {
        log.emplace_back("navigate_to_string:" + html + "@" + base_uri);
    }
    auto reload() -> void override { log.emplace_back("reload"); }
    auto stop() -> void override { log.emplace_back("stop"); }
    auto go_back() -> void override { log.emplace_back("go_back"); }
    auto go_forward() -> void override { log.emplace_back("go_forward"); }
    auto set_zoom_factor(float zoom) -> void override { log.emplace_back("zoom:" + std::to_string(zoom)); }
    auto set_javascript_enabled(bool enabled) -> void override {
        log.emplace_back(std::string{"js:"} + (enabled ? "1" : "0"));
    }
    auto evaluate_script(const std::string &js, std::function<void(std::string)> done) -> void override {
        log.emplace_back("evaluate:" + js);
        if (done) {
            done(script_result);
        }
    }
    auto post_message(const std::string &json) -> void override { log.emplace_back("post:" + json); }
    auto set_callbacks(std::weak_ptr<webview::WebViewCallbacks> callbacks) -> void override {
        log.emplace_back("set_callbacks");
        callbacks_ = std::move(callbacks);
    }

    /// @brief 以引擎身份触发一次 Web→Native 消息（经弱引用判活）。
    /// @param json 页面投来的 JSON 文本。
    /// @return 控件仍在世且订阅了该事件为 true（否则回调被丢弃）。
    auto fire_web_message(const std::string &json) -> bool {
        const std::shared_ptr<webview::WebViewCallbacks> cbs = callbacks_.lock();
        if (cbs == nullptr || !cbs->on_web_message) {
            return false;
        }
        cbs->on_web_message(json);
        return true;
    }

    /// @brief 以引擎身份触发一次标题变化。
    /// @param title 新标题。
    /// @return 回调确实抵达为 true。
    auto fire_title_changed(const std::string &title) -> bool {
        const std::shared_ptr<webview::WebViewCallbacks> cbs = callbacks_.lock();
        if (cbs == nullptr || !cbs->on_title_changed) {
            return false;
        }
        cbs->on_title_changed(title);
        return true;
    }

    std::vector<std::string> log;  ///< 下发序列（"方法名:参数"）
    Image next_image;  ///< 下一次捕获交付的位图（空 pixels 即模拟失败）
    std::string script_result{"\"ok\""};  ///< `evaluate_script` 的回值

  private:
    std::weak_ptr<webview::WebViewCallbacks> callbacks_;
};

/// @brief 一块确定性小图：判据要的是「像素真的经引擎流进控件」，形态无所谓，2×2 足够。
/// @return 16 字节 RGBA8 的 2×2 位图。
auto stub_bitmap() -> Image {
    Image img;
    img.width = 2;
    img.height = 2;
    img.pixels.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4U, 77U);
    return img;
}

/// @brief 能力可配的桩引擎：让四种可用性裁决都能被确定性触发。
///
/// `availability` 必须可配——降级 slug 的归因就取自它；写死成 `Available` 就等于没测那条分支。
class StubEngine final : public webview::NativeViewEngine {
  public:
    StubEngine(bool interactive_caps, bool snapshot_caps, webview::WebViewAvailability availability_caps)
        : interactive_(interactive_caps), snapshot_(snapshot_caps), availability_(availability_caps) {}

    [[nodiscard]] auto id() const -> webview::WebViewEngine override { return webview::WebViewEngine::Null; }

    [[nodiscard]] auto probe() const -> webview::WebViewCapabilities override {
        return webview::WebViewCapabilities{.interactive = interactive_,
                                            .snapshot = snapshot_,
                                            .engine = webview::WebViewEngine::Null,
                                            .availability = availability_};
    }

    [[nodiscard]] auto create_view(void * /*parent_window*/) -> std::unique_ptr<webview::WebViewSink> override {
        auto sink = std::make_unique<StubSink>();
        last_sink = sink.get();  // 留给用例脚本化捕获产物（所有权仍在返回的 unique_ptr）
        return sink;
    }

    StubSink *last_sink = nullptr;  ///< 最近一次创建的视图（未创建过为空）

  private:
    bool interactive_;
    bool snapshot_;
    webview::WebViewAvailability availability_;
};

/// @brief 装配一个两模式皆可用的桩引擎。
/// @return 已安装的引擎共享指针（调用方无需持有，仅经 `install_engine` 生效）。
auto install_capable_engine() -> std::shared_ptr<StubEngine> {
    auto engine = std::make_shared<StubEngine>(true, true, webview::WebViewAvailability::Available);
    webview::install_engine(engine);
    return engine;
}

/// @brief 绑定一个桩宿主线程局部口（`native_window` 取一块静态存储的地址，只作非空判据）。
/// @param host 待绑定的宿主。
auto bind_stub_host(webview::NativeViewHost &host) -> void {
    static int stub_native_window = 0;
    host.bind_window(&stub_native_window, []() -> float { return 1.0F; }, []() -> EdgeInsets { return EdgeInsets{}; });
}

/// @brief 清空诊断收集器，返回本次挂载之后新产生的全部诊断。
/// @param mount 待执行的挂载动作（布局即触发挂载）。
auto collect_after(const std::function<void()> &mount) -> std::vector<Diagnostic> {
    Diagnostics::take();  // 先清场：收集器是进程级共享的
    mount();
    return Diagnostics::take();
}

/// @brief 诊断列表中是否含给定 slug。
/// @param items 待检查的诊断列表。
/// @param slug 冻结 slug。
/// @return 命中为 true。
auto has_code(const std::vector<Diagnostic> &items, const std::string &slug) -> bool {
    for (const Diagnostic &d : items) {
        if (d.code == slug) {
            return true;
        }
    }
    return false;
}

/// @brief 把控件放进容器并布局一次（触发挂载），返回根容器以延长生命周期。
/// @param wv 被测控件。
/// @return 已完成布局的根容器（持有 `wv` 的一份共享所有权）。
/// @brief 视图桩日志里以给定前缀开头的条目数。
/// @param sink 待检查的视图桩。
/// @param prefix 方法名前缀（如 "capture"）。
/// @return 命中条数。
auto count_calls(const StubSink &sink, const std::string &prefix) -> std::size_t {
    std::size_t n = 0;
    for (const std::string &entry : sink.log) {
        if (entry.size() >= prefix.size() && entry.compare(0, prefix.size(), prefix) == 0) {
            ++n;
        }
    }
    return n;
}

/// @brief 日志里是否出现给定条目（整串相等）。
/// @param sink 待检查的视图桩。
/// @param entry 期望条目。
/// @return 出现为 true。
auto logged(const StubSink &sink, const std::string &entry) -> bool {
    return std::find(sink.log.begin(), sink.log.end(), entry) != sink.log.end();
}

auto mount_in_tree(const std::shared_ptr<webview::WebView> &wv) -> std::shared_ptr<Column> {
    auto root = std::make_shared<Column>();
    root->add(Node{std::static_pointer_cast<Widget>(wv)});
    LayoutEngine::layout(*root, bounded(400.0F, 300.0F));
    return root;
}

}  // namespace

AURORA_TEST_CASE(no_engine_installed_degrades_as_runtime_missing) {
    webview::install_engine(nullptr);
    auto wv = std::make_shared<webview::WebView>();
    AURORA_TEST_CHECK(wv->capabilities().unavailable());

    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-runtime-missing"));
}

AURORA_TEST_CASE(null_engine_reports_disabled_availability) {
    webview::install_null_engine();
    const webview::WebViewCapabilities caps = webview::current_capabilities();
    AURORA_TEST_CHECK(caps.unavailable());
    AURORA_TEST_CHECK_EQ(static_cast<int>(caps.availability), static_cast<int>(webview::WebViewAvailability::Disabled));

    auto wv = std::make_shared<webview::WebView>();
    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-runtime-missing"));
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(missing_runtime_availability_degrades_as_runtime_missing) {
    // 宏已开启但运行期探测不到引擎：`create_view` 之外的一切都不该发生，且归因必须是运行时缺失。
    webview::install_engine(std::make_shared<StubEngine>(false, false, webview::WebViewAvailability::MissingRuntime));
    auto wv = std::make_shared<webview::WebView>();
    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-runtime-missing"));
    AURORA_TEST_CHECK_FALSE(has_code(items, "webview-unsupported-mode"));
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(unsupported_platform_availability_degrades_as_unsupported_mode) {
    // 平台 / 后端本身不提供（如 Wayland 的交互模式）：归因必须是「模式不支持」而非「运行时缺失」。
    // 这条与上一条 slug 必须**不同**——两者的修复动作完全相反（装运行时 vs 换模式 / 换后端）。
    webview::install_engine(
        std::make_shared<StubEngine>(false, false, webview::WebViewAvailability::UnsupportedPlatform));
    auto wv = std::make_shared<webview::WebView>();
    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-unsupported-mode"));
    AURORA_TEST_CHECK_FALSE(has_code(items, "webview-runtime-missing"));
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(available_engine_without_host_degrades_with_host_unbound) {
    (void)install_capable_engine();
    // 线程局部宿主口未安装：`NativeViewHost::current()` 为空。
    webview::NativeViewHost::set_current(nullptr);
    auto wv = std::make_shared<webview::WebView>();
    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-host-unbound"));
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(interactive_mode_unsupported_degrades_with_unsupported_mode) {
    webview::install_engine(std::make_shared<StubEngine>(false, true, webview::WebViewAvailability::Available));
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(webview::WebViewProps{.mode = webview::WebViewMode::Interactive});
    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-unsupported-mode"));

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(snapshot_mode_unsupported_degrades_with_snapshot_unsupported) {
    webview::install_engine(std::make_shared<StubEngine>(true, false, webview::WebViewAvailability::Available));
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(webview::WebViewProps{.mode = webview::WebViewMode::Snapshot});
    const std::vector<Diagnostic> items = collect_after([&wv]() { mount_in_tree(wv); });
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK(has_code(items, "webview-snapshot-unsupported"));

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(available_engine_and_bound_host_registers_with_host) {
    (void)install_capable_engine();
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>();
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_CHECK(!wv->degraded());
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);

    // 显式卸载：登记必须回落（与登记成对）。
    wv->unmount();
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);

    // 重复卸载幂等（不得把别的条目带走）。
    wv->unmount();
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(snapshot_mode_registers_view_and_blits_captured_pixels) {
    // 快照模式的像素只有一个来源：引擎视图的 `capture`。这条把「控件 → 宿主 → 视图」的
    // 回访链整个钉住——`snapshot_bitmap()` 此前没有任何写入方，恒为空图。
    const std::shared_ptr<StubEngine> engine = install_capable_engine();
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(webview::WebViewProps{.mode = webview::WebViewMode::Snapshot});
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_REQUIRE_TRUE(!wv->degraded());
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
    AURORA_TEST_REQUIRE_TRUE(engine->last_sink != nullptr);
    AURORA_TEST_CHECK_TRUE(wv->snapshot_bitmap().pixels.empty());  // 未捕获前确实是空图

    engine->last_sink->next_image = stub_bitmap();
    std::vector<std::size_t> seen;
    wv->request_snapshot([&seen](const Image &img) -> void { seen.push_back(img.pixels.size()); });

    AURORA_TEST_CHECK_EQ(count_calls(*engine->last_sink, "capture"), 1U);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen.at(0), 16U);  // 2×2 RGBA8
    AURORA_TEST_CHECK_EQ(wv->snapshot_bitmap().pixels.size(), 16U);
    AURORA_TEST_CHECK_EQ(wv->snapshot_bitmap().width, 2);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(failed_capture_keeps_the_last_good_bitmap) {
    // 捕获失败（引擎回空图）不得把已有的好像素清掉：快照模式本来就是「一次拷贝一张静态图」，
    // 失败时留着上一帧才是可用行为，清空则让控件在一次瞬时故障后变成空白。
    const std::shared_ptr<StubEngine> engine = install_capable_engine();
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(webview::WebViewProps{.mode = webview::WebViewMode::Snapshot});
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_REQUIRE_TRUE(engine->last_sink != nullptr);

    engine->last_sink->next_image = stub_bitmap();
    wv->request_snapshot({});
    AURORA_TEST_REQUIRE_EQ(wv->snapshot_bitmap().pixels.size(), 16U);

    engine->last_sink->next_image = Image{};  // 模拟捕获失败
    std::vector<std::size_t> seen;
    wv->request_snapshot([&seen](const Image &img) -> void { seen.push_back(img.pixels.size()); });
    AURORA_TEST_CHECK_EQ(count_calls(*engine->last_sink, "capture"), 2U);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen.at(0), 16U);  // 回调拿到的仍是上一帧好图
    AURORA_TEST_CHECK_EQ(wv->snapshot_bitmap().pixels.size(), 16U);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(snapshot_request_without_view_reports_and_resolves_empty) {
    // 能力说得出、视图却没登记上（宿主未绑定 ⇒ 整段降级）：必须回一次诊断**且**让回调落地。
    // 「没收到回调」与「收到空图」是两种状态，只有后者可被调用方判读，故回调不得悬着。
    install_capable_engine();
    webview::NativeViewHost::set_current(nullptr);
    auto wv = std::make_shared<webview::WebView>(webview::WebViewProps{.mode = webview::WebViewMode::Snapshot});
    Diagnostics::take();
    mount_in_tree(wv);
    const std::vector<Diagnostic> mounted = Diagnostics::take();
    AURORA_TEST_CHECK(wv->degraded());
    AURORA_TEST_CHECK(has_code(mounted, "webview-host-unbound"));

    bool resolved = false;
    std::size_t pixels = 1;
    wv->request_snapshot([&](const Image &img) -> void {
        resolved = true;
        pixels = img.pixels.size();
    });
    AURORA_TEST_CHECK_TRUE(resolved);
    AURORA_TEST_CHECK_EQ(pixels, 0U);
    const std::vector<Diagnostic> snapshot_dg = Diagnostics::take();
    AURORA_TEST_CHECK(has_code(snapshot_dg, "webview-host-unbound"));

    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(tree_teardown_unregisters_without_explicit_unmount) {
    // 框架在拆树时**不代调**子件 `on_unmount`（`Container::on_unmount` 的口径：摘除时刻容器
    // 无从判断子件是否仍活在容器之外），而登记表条目持有裸 `WebView*`。故控件析构必须自行
    // 注销——否则宿主下一次几何同步就解引用已释放内存。
    (void)install_capable_engine();
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    std::shared_ptr<Column> root;
    {
        auto wv = std::make_shared<webview::WebView>();
        root = mount_in_tree(wv);
        AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
        wv.reset();  // 只剩树持有
    }
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);  // 控件仍在世（树持有），登记未动
    root.reset();  // 拆树 ⇒ 控件随之析构，无显式 unmount
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(host_destroyed_before_control_does_not_crash_on_teardown) {
    // 反向拆毁顺序（先拆窗口、后拆树）：控件析构时的回访必须判活，不得解引用已亡宿主。
    (void)install_capable_engine();
    auto wv = std::make_shared<webview::WebView>();
    std::shared_ptr<Column> root;
    {
        webview::NativeViewHost host;
        bind_stub_host(host);
        webview::NativeViewHost::set_current(&host);
        root = mount_in_tree(wv);
        AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
        webview::NativeViewHost::set_current(nullptr);
    }  // 宿主在此析构，登记表与票据一同失效
    AURORA_TEST_CHECK(!wv->degraded());
    root.reset();  // 控件析构：经票据判活后应安静地什么都不做
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(degradation_warning_is_emitted_once_per_mount) {
    // 「同一次挂载只报一条」而**不是**「一辈子只报一条」：重挂（换宿主、转场复用同一实例）
    // 必须重新告一次，否则第二次降级全程静默。这条判据钉住 `degraded_reported_` 随挂载复位。
    webview::install_engine(nullptr);
    auto wv = std::make_shared<webview::WebView>();

    Diagnostics::take();
    wv->mount(BuildContext{});
    const std::vector<Diagnostic> first = Diagnostics::take();
    AURORA_TEST_CHECK_EQ(first.size(), 1U);
    AURORA_TEST_CHECK(has_code(first, "webview-runtime-missing"));

    wv->unmount();
    wv->mount(BuildContext{});
    const std::vector<Diagnostic> second = Diagnostics::take();
    AURORA_TEST_CHECK_EQ(second.size(), 1U);
    AURORA_TEST_CHECK(has_code(second, "webview-runtime-missing"));
}

AURORA_TEST_CASE(mount_installs_gate_props_and_initial_navigation_in_order) {
    // 挂载期把属性推给视图的**顺序**本身是契约：先装回调（否则初始导航的事件丢在无人听的
    // 时刻）、再设缩放与脚本开关（页面排版依赖缩放）、最后才发起初始导航。
    const std::shared_ptr<StubEngine> engine =
        std::make_shared<StubEngine>(true, true, webview::WebViewAvailability::Available);
    webview::install_engine(engine);
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(webview::WebViewProps{.url = "https://example.invalid/a",
                                                                       .mode = webview::WebViewMode::Interactive,
                                                                       .zoom_factor = 1.5F,
                                                                       .javascript_enabled = false});
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_REQUIRE_TRUE(!wv->degraded());
    AURORA_TEST_REQUIRE_TRUE(engine->last_sink != nullptr);

    const std::vector<std::string> expected = {
        "set_callbacks",
        "zoom:1.500000",
        "js:0",
        "navigate:https://example.invalid/a",
    };
    AURORA_TEST_CHECK_TRUE(engine->last_sink->log == expected);
    // `url` 非空 ⇒ 内联 HTML 一律不被导航（属性优先级是声明出来的，不是碰巧）。
    AURORA_TEST_CHECK_FALSE(logged(*engine->last_sink, "navigate_to_string:"));

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(mount_navigates_inline_html_only_when_url_is_empty) {
    const std::shared_ptr<StubEngine> engine =
        std::make_shared<StubEngine>(true, true, webview::WebViewAvailability::Available);
    webview::install_engine(engine);
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>(
        webview::WebViewProps{.html = "<p>hi</p>", .base_uri = "https://example.invalid/"});
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_REQUIRE_TRUE(engine->last_sink != nullptr);
    AURORA_TEST_CHECK_TRUE(logged(*engine->last_sink, "navigate_to_string:<p>hi</p>@https://example.invalid/"));
    AURORA_TEST_CHECK_EQ(count_calls(*engine->last_sink, "navigate:"), 0U);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(commands_forward_to_the_registered_view) {
    const std::shared_ptr<StubEngine> engine =
        std::make_shared<StubEngine>(true, true, webview::WebViewAvailability::Available);
    webview::install_engine(engine);
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>();
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_REQUIRE_TRUE(engine->last_sink != nullptr);
    engine->last_sink->log.clear();  // 只看挂载之后的命令面

    wv->load_url("https://example.invalid/b");
    wv->reload();
    wv->stop();
    wv->go_back();
    wv->go_forward();
    wv->post_json("{\"k\":1}");
    std::string got_script;
    wv->evaluate_script("1+1", [&got_script](const std::string &r) -> void { got_script = r; });

    const std::vector<std::string> expected = {
        "navigate:https://example.invalid/b",
        "reload",
        "stop",
        "go_back",
        "go_forward",
        "post:{\"k\":1}",
        "evaluate:1+1",
    };
    AURORA_TEST_CHECK_TRUE(engine->last_sink->log == expected);
    AURORA_TEST_CHECK_EQ(got_script, std::string{"\"ok\""});
    // 命令同时留意图在属性上：缺引擎时序列化仍读得到（判据 5）。
    AURORA_TEST_CHECK_EQ(wv->url, std::string{"https://example.invalid/b"});

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(commands_without_a_view_only_record_intent) {
    // 降级路径（无宿主 ⇒ 无登记视图）：命令不得崩、不得抛，且属性意图仍完整可序列化。
    webview::install_engine(nullptr);
    auto wv = std::make_shared<webview::WebView>();
    Diagnostics::take();
    wv->load_url("https://example.invalid/c");
    wv->load_html("<b>x</b>", "https://example.invalid/");
    wv->reload();
    wv->stop();
    wv->go_back();
    wv->go_forward();
    wv->post_json("{}");
    AURORA_TEST_CHECK(wv->url == "https://example.invalid/c");
    AURORA_TEST_CHECK(wv->html == "<b>x</b>");

    // `evaluate_script` 无视图时**必须**让回调落地（空串即既有的失败编码），
    // 不回才是让调用方的续延永久悬空。
    bool resolved = false;
    std::string result{"sentinel"};
    wv->evaluate_script("2+2", [&](const std::string &r) -> void {
        resolved = true;
        result = r;
    });
    AURORA_TEST_CHECK_TRUE(resolved);
    AURORA_TEST_CHECK_TRUE(result.empty());
}

AURORA_TEST_CASE(engine_events_reach_the_controls_subscribed_callbacks) {
    const std::shared_ptr<StubEngine> engine =
        std::make_shared<StubEngine>(true, true, webview::WebViewAvailability::Available);
    webview::install_engine(engine);
    webview::NativeViewHost host;
    bind_stub_host(host);
    webview::NativeViewHost::set_current(&host);

    auto wv = std::make_shared<webview::WebView>();
    const std::shared_ptr<Column> root = mount_in_tree(wv);
    AURORA_TEST_REQUIRE_TRUE(engine->last_sink != nullptr);

    std::vector<std::string> messages;
    std::vector<std::string> titles;
    wv->set_on_web_message([&messages](std::string json) -> void { messages.push_back(std::move(json)); });
    wv->set_on_title_changed([&titles](std::string title) -> void { titles.push_back(std::move(title)); });

    // 挂载后再设回调也必须立即生效：引擎持的是弱引用，读的是聚合里的最新值。
    AURORA_TEST_CHECK_TRUE(engine->last_sink->fire_web_message("{\"v\":1}"));
    AURORA_TEST_CHECK_TRUE(engine->last_sink->fire_title_changed("T1"));
    AURORA_TEST_REQUIRE_EQ(messages.size(), 1U);
    AURORA_TEST_CHECK(messages.at(0) == "{\"v\":1}");
    AURORA_TEST_REQUIRE_EQ(titles.size(), 1U);
    AURORA_TEST_CHECK(titles.at(0) == "T1");

    // 撤销订阅后事件被安全丢弃（引擎侧逐个判空），不得崩也不得投递给空函数。
    wv->set_on_web_message({});
    AURORA_TEST_CHECK_FALSE(engine->last_sink->fire_web_message("{}"));
    AURORA_TEST_CHECK_EQ(messages.size(), 1U);

    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

AURORA_TEST_CASE(callback_gate_drops_events_once_the_owner_is_gone) {
    // 视图侧的判活契约（真实引擎必依赖它）：事件令牌注册在引擎对象上、回收发生在 detach，
    // 这中间排队的回调可能在控件已析构后抵达。聚合归控件强持、视图只弱持 ⇒ 到期即空。
    auto sink = std::make_unique<StubSink>();
    StubSink *raw = sink.get();
    auto cbs = std::make_shared<webview::WebViewCallbacks>();
    int seen = 0;
    cbs->on_web_message = [&seen](std::string /*json*/) -> void { ++seen; };
    raw->set_callbacks(cbs);

    AURORA_TEST_CHECK_TRUE(raw->fire_web_message("a"));
    AURORA_TEST_CHECK_EQ(seen, 1);

    cbs.reset();  // 控件析构：强引用最后一份释放，视图手里的弱引用随即失效
    AURORA_TEST_CHECK_FALSE(raw->fire_web_message("b"));
    AURORA_TEST_CHECK_EQ(seen, 1);  // 一条都不许多投递
}

AURORA_TEST_CASE(props_round_trip_through_json) {
    webview::WebView src;
    src.url = "https://example.invalid/page";
    src.html = "<p>hi</p>";
    src.base_uri = "https://example.invalid/";
    src.mode = webview::WebViewMode::Snapshot;
    src.zoom_factor = 1.5F;
    src.javascript_enabled = false;
    src.background = Color{10, 20, 30, 40};

    Json props = Json::object();
    src.serialize_props(props);

    webview::WebView dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK(dst.url == src.url);
    AURORA_TEST_CHECK(dst.html == src.html);
    AURORA_TEST_CHECK(dst.base_uri == src.base_uri);
    AURORA_TEST_CHECK_EQ(static_cast<int>(dst.mode), static_cast<int>(webview::WebViewMode::Snapshot));
    AURORA_TEST_CHECK(dst.zoom_factor == src.zoom_factor);
    AURORA_TEST_CHECK_FALSE(dst.javascript_enabled);
    AURORA_TEST_CHECK_TRUE(dst.background == src.background);
}

AURORA_TEST_CASE(deserialize_ignores_absent_and_mistyped_keys) {
    // 键缺失或类型不符都要保留现值（不清零、不抛），这是 `deserialize_props` 的既有契约。
    webview::WebView wv;
    wv.url = "https://example.invalid/keep";
    wv.zoom_factor = 2.0F;

    Json props = Json::object();
    props.set("url", Json{"https://example.invalid/new"});
    props.set("zoom_factor", Json{"not-a-number"});  // 类型不符
    props.set("mode", Json{"Bogus"});  // 枚举值非法
    wv.deserialize_props(props);

    AURORA_TEST_CHECK(wv.url == "https://example.invalid/new");
    AURORA_TEST_CHECK(wv.zoom_factor == 2.0F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(wv.mode), static_cast<int>(webview::WebViewMode::Interactive));
}

AURORA_TEST_CASE(defaults_are_interactive_and_script_enabled) {
    webview::WebView w;
    AURORA_TEST_CHECK_EQ(static_cast<int>(w.mode), static_cast<int>(webview::WebViewMode::Interactive));
    AURORA_TEST_CHECK(w.javascript_enabled);
    AURORA_TEST_CHECK(w.url.empty());
    AURORA_TEST_CHECK(w.zoom_factor == 1.0F);
    AURORA_TEST_CHECK(std::string{w.type_name()} == "WebView");
    AURORA_TEST_CHECK_FALSE(w.wants_focus());  // 焦点契约：不进本库 Tab 焦点链
}

AURORA_TEST_CASE(describe_static_exposes_mode_enumeration) {
    const WidgetDescriptor d = webview::WebView::describe_static();
    AURORA_TEST_CHECK(d.name == "WebView");
    AURORA_TEST_CHECK(d.children_policy == "none");
    bool found = false;
    for (const PropDescriptor &p : d.properties) {
        if (p.name == "mode") {
            found = true;
            AURORA_TEST_CHECK_EQ(p.enum_values.size(), 2U);
            AURORA_TEST_CHECK_EQ(p.default_value, std::string{"Interactive"});
        }
    }
    AURORA_TEST_CHECK(found);
}

}  // namespace aurora::test_cases::utest_web_view
