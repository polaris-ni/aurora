/// 测试类型: unit
/// 目标单元: include/aurora/webview/native_view_host.h（宿主侧几何换算与登记表）
/// 测试说明: 钉住三个「必须确定性」的量——① dp → 物理像素的换算（含 CSD 内缩与缩放取整）；
/// ② 每帧同步的下发去重；③ 可见性只在**翻转时**下发。前者是原生子视图唯一的位置来源，
/// 算错即永久错位；后两条是 `window_bounds()` 性能契约的守门（该入口明确禁止进入每帧路径，
/// 故同步必须可跳过、且不重复下发）。
///
/// 判据纪律：换算判据全部用**手算字面期望值**，不拿被测函数的输出当基准，也不复用其
/// 中间量；去重判据用「若干次同步后 sink 收到的矩形序列」这一可区分的量——只看返回计数
/// 会漏掉「计数对了但实际仍下发」的分叉。

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/webview/native_view_host.h"
#include "aurora/webview/web_view.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/node.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_native_view_host {

using aurora::testing::require_value;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto inset(float left, float top) -> EdgeInsets {
    return EdgeInsets{.left = left, .top = top, .right = 0.0F, .bottom = 0.0F};
}

auto rect(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 宿主桩窗口句柄：只作非空判据，引擎侧从不解引用。
/// @return 指向进程内静态存储的地址。
auto stub_window() -> void * {
    static int handle = 0;
    return &handle;
}

/// @brief 固定尺寸哑控件：布局返回构造给定的自然尺寸（经约束钳制），绘制无副作用。
///
/// 用途是在树里制造**已知**的偏移量——嵌套层里前一个兄弟占多大，后一个控件的窗口原点
/// 就偏移多少，从而让「宿主下发的是窗口坐标而非局部坐标」这条判据有可手算的期望值。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*r*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

/// @brief 记录型 sink：把宿主下发过的矩形序列与可见性翻转序列照单记下。
class RecordSink final : public webview::WebViewSink {
  public:
    auto set_native_rect(const webview::NativeViewRect &r) -> void override { rects.push_back(r); }
    auto set_native_visible(bool v) -> void override { visibility.push_back(v); }
    auto capture(std::function<void(Image)> done) -> void override {
        ++captures;
        if (done) {
            done(Image{});  // 本套件不判像素，只保证接口存在且回调必达
        }
    }
    auto detach() -> void override { detached = true; }

    // 内容通道在本套件不参与判据（几何/像素另有专测），按契约留空实现。
    auto navigate(const std::string & /*uri*/) -> void override {}
    auto navigate_to_string(const std::string & /*html*/, const std::string & /*base_uri*/) -> void override {}
    auto reload() -> void override {}
    auto stop() -> void override {}
    auto go_back() -> void override {}
    auto go_forward() -> void override {}
    auto set_zoom_factor(float /*zoom*/) -> void override {}
    auto set_javascript_enabled(bool /*enabled*/) -> void override {}
    auto evaluate_script(const std::string & /*js*/, std::function<void(std::string)> /*done*/) -> void override {}
    auto post_message(const std::string & /*json*/) -> void override {}
    auto set_callbacks(std::weak_ptr<webview::WebViewCallbacks> /*callbacks*/) -> void override {}

    std::vector<webview::NativeViewRect> rects;  ///< 收到过的矩形（按下发顺序）
    std::vector<bool> visibility;  ///< 可见性翻转序列（未翻转则不追加）
    int captures = 0;  ///< 被请求捕获的次数
    bool detached = false;  ///< 是否被解绑过
};

/// @brief 两模式皆可用的桩引擎：让控件的挂载走**真实登记路径**（而非降级路径）。
class CapableEngine final : public webview::NativeViewEngine {
  public:
    [[nodiscard]] auto id() const -> webview::WebViewEngine override { return webview::WebViewEngine::Null; }
    [[nodiscard]] auto probe() const -> webview::WebViewCapabilities override {
        return webview::WebViewCapabilities{.interactive = true,
                                            .snapshot = true,
                                            .engine = webview::WebViewEngine::Null,
                                            .availability = webview::WebViewAvailability::Available};
    }
    [[nodiscard]] auto create_view(void * /*parent_window*/) -> std::unique_ptr<webview::WebViewSink> override {
        return nullptr;  // 桩视图：宿主只登记几何，平台侧资源为零
    }
};

/// @brief 一棵「Row { 前导盒, Column { 前导盒, WebView } }」的已布局树（两级嵌套）。
///
/// 窗口原点因此落在 {20, 30}：Row 内前导盒宽 20，Column 内前导盒高 30。
struct NestedTree {
    std::shared_ptr<Row> root;
    std::shared_ptr<webview::WebView> wv;
};

/// @brief 搭两级嵌套树并完成一次布局（触发挂载）。
/// @param w 布局约束宽。
/// @param h 布局约束高。
/// @return 已布局的 fixture。
auto laid_out_nested_tree(float w, float h) -> NestedTree {
    NestedTree f;
    f.wv = std::make_shared<webview::WebView>();
    auto inner = std::make_shared<Column>();
    inner->add(Node{std::make_shared<FixedBox>(300.0F, 30.0F)});
    inner->add(Node{std::static_pointer_cast<Widget>(f.wv)});
    f.root = std::make_shared<Row>();
    f.root->add(Node{std::make_shared<FixedBox>(20.0F, 400.0F)});
    f.root->add(Node{inner});
    LayoutEngine::layout(*f.root, bounded(w, h));
    return f;
}

/// @brief 绑定宿主：给定缩放与 CSD 内缩（取值经回调现取，便于中途改档测去重）。
///
/// `scale` 按 const 引用捕获（调用点的命名局部量，多例在绑定后改档测去重，必须共享）；
/// `csd_inset` **按值接收并堆拷贝**后进 lambda 捕获——曾有版本按 const 引用接收，调用点
/// 传临时量（`inset(8, 4)`）时参数绑定到的临时量在语句结束即析构，lambda 之后读到的是
/// 悬垂引用（Linux g++ 下 Stack 复用恰好回零，判据静默变红）。按值 + 共享持有把
/// 「引用必须比宿主活得久」这条隐式契约从调用点拿走；确实需要在绑定后改内缩的用例
/// （去重那条）自行 bind_window，不走本助手。
/// @param host 待绑定宿主。
/// @param scale 缩放档位存储（回调按引用读它）。
/// @param csd_inset CSD 内缩值（拷贝进共享持有）。
auto bind_host(webview::NativeViewHost &host, const float &scale, EdgeInsets csd_inset) -> void {
    const auto stored = std::make_shared<EdgeInsets>(csd_inset);
    host.bind_window(
        stub_window(), [&scale]() -> float { return scale; }, [stored]() -> EdgeInsets { return *stored; });
}

}  // namespace

// ---- ① 换算纯函数：dp + scale + content_inset → 物理矩形 ----

AURORA_TEST_CASE(to_native_rect_is_identity_at_scale_one_without_inset) {
    const auto r = webview::to_native_rect(rect(12.0F, 34.0F, 100.0F, 50.0F), 1.0F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(r.x, 12);
    AURORA_TEST_CHECK_EQ(r.y, 34);
    AURORA_TEST_CHECK_EQ(r.width, 100);
    AURORA_TEST_CHECK_EQ(r.height, 50);
    AURORA_TEST_CHECK_TRUE(r.has_area());
}

AURORA_TEST_CASE(to_native_rect_scales_every_axis) {
    // 四档缩放逐一手算：1.0 / 1.25 / 1.5 / 2.0（1.5 档专门盯「截断 vs 取最近」）。
    const Rect d = rect(0.0F, 0.0F, 100.0F, 50.0F);
    const auto r1 = webview::to_native_rect(d, 1.0F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(r1.width, 100);
    AURORA_TEST_CHECK_EQ(r1.height, 50);

    const auto r125 = webview::to_native_rect(d, 1.25F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(r125.width, 125);
    AURORA_TEST_CHECK_EQ(r125.height, 63);  // 50 × 1.25 = 62.5 → 取最近 63（截断会得 62）

    const auto r15 = webview::to_native_rect(d, 1.5F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(r15.width, 150);
    AURORA_TEST_CHECK_EQ(r15.height, 75);

    const auto r2 = webview::to_native_rect(d, 2.0F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(r2.width, 200);
    AURORA_TEST_CHECK_EQ(r2.height, 100);
}

AURORA_TEST_CASE(to_native_rect_rounds_origin_to_nearest_not_truncate) {
    // 1.25 倍下 10.4dp 应为 13px（10.4 × 1.25 = 13.0）；换成 10.6dp 则是 13.25 → 13。
    // 截断会得到 13 与 13，看不出差别，故再取一条会分叉的：10.0dp @1.5 = 15 与 10.4dp @1.5 = 15.6 → 16。
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(10.4F, 0.0F, 1.0F, 1.0F), 1.25F, EdgeInsets{}).x, 13);
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(10.4F, 0.0F, 1.0F, 1.0F), 1.5F, EdgeInsets{}).x, 16);
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(10.0F, 0.0F, 1.0F, 1.0F), 1.5F, EdgeInsets{}).x, 15);
}

AURORA_TEST_CASE(to_native_rect_adds_inset_before_scaling_not_after) {
    // 内缩是逻辑 dp 量：先加再乘。顺序颠倒（先乘再加 8px）在 2 倍档会得到 24 而非 16。
    const EdgeInsets i = inset(8.0F, 8.0F);
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(0.0F, 0.0F, 10.0F, 10.0F), 1.0F, i).x, 8);
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(0.0F, 0.0F, 10.0F, 10.0F), 2.0F, i).x, 16);
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(0.0F, 0.0F, 10.0F, 10.0F), 2.0F, i).y, 16);
    // 内缩只平移原点，不参与宽高（宽高由控件自身占位决定）。
    AURORA_TEST_CHECK_EQ(webview::to_native_rect(rect(0.0F, 0.0F, 10.0F, 10.0F), 2.0F, i).width, 20);
}

AURORA_TEST_CASE(to_native_rect_offsets_inset_on_both_origin_axes_independently) {
    // left / top 各管一个轴：写反一次（left 加到 y）在这里就现形。
    const auto r = webview::to_native_rect(rect(1.0F, 2.0F, 10.0F, 10.0F), 1.0F, inset(30.0F, 40.0F));
    AURORA_TEST_CHECK_EQ(r.x, 31);
    AURORA_TEST_CHECK_EQ(r.y, 42);
}

AURORA_TEST_CASE(to_native_rect_clamps_negative_size_and_degenerate_scale) {
    const auto r = webview::to_native_rect(rect(5.0F, 5.0F, -20.0F, -5.0F), 1.0F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(r.width, 0);
    AURORA_TEST_CHECK_EQ(r.height, 0);
    AURORA_TEST_CHECK_FALSE(r.has_area());
    // scale <= 0 视为 1.0：后端未上报比例时不应把矩形算没（「不知道」不等于「没有」）。
    const auto rz = webview::to_native_rect(rect(3.0F, 4.0F, 10.0F, 10.0F), 0.0F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(rz.x, 3);
    AURORA_TEST_CHECK_EQ(rz.width, 10);
    const auto rn = webview::to_native_rect(rect(3.0F, 4.0F, 10.0F, 10.0F), -2.0F, EdgeInsets{});
    AURORA_TEST_CHECK_EQ(rn.width, 10);
    AURORA_TEST_CHECK_EQ(rn.x, 3);
}

// ---- ② 登记表：配对、幂等与前置条件 ----

AURORA_TEST_CASE(register_and_unregister_are_pairable_and_idempotent) {
    webview::NativeViewHost host;
    const float scale = 1.0F;
    bind_host(host, scale, EdgeInsets{});

    auto wv = std::make_shared<webview::WebView>();
    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    const std::size_t token = require_value(host.register_view(wv.get(), std::move(sink)));
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);

    host.unregister_view(token);
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);
    AURORA_TEST_CHECK(raw->detached);

    // 重复注销幂等（不崩、不误伤后续槽位）。
    host.unregister_view(token);
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);
}

AURORA_TEST_CASE(unregister_only_touches_the_named_slot) {
    // 幂等还有一层含义：注销一个 token 不得带走别家。取回原对象后再次注销，第二条仍在。
    webview::NativeViewHost host;
    const float scale = 1.0F;
    bind_host(host, scale, EdgeInsets{});

    auto wv_a = std::make_shared<webview::WebView>();
    auto wv_b = std::make_shared<webview::WebView>();
    auto sink_a = std::make_unique<RecordSink>();
    auto sink_b = std::make_unique<RecordSink>();
    RecordSink *raw_b = sink_b.get();
    const std::size_t token_a = require_value(host.register_view(wv_a.get(), std::move(sink_a)));
    const std::size_t token_b = require_value(host.register_view(wv_b.get(), std::move(sink_b)));
    AURORA_TEST_CHECK_EQ(host.view_count(), 2U);

    host.unregister_view(token_a);
    host.unregister_view(token_a);
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
    AURORA_TEST_CHECK_FALSE(raw_b->detached);  // 另一条完好无损

    host.unregister_view(token_b);
    AURORA_TEST_CHECK(raw_b->detached);
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);
}

AURORA_TEST_CASE(token_reuse_after_unregister_does_not_shift_siblings) {
    // 登记号是**稳定索引**（置空而非 erase）：注销后再登记会复用空槽，此时旧 token 必须
    // 指向新条目而不是把新条目当成自己的历史包袱清掉。
    webview::NativeViewHost host;
    const float scale = 1.0F;
    bind_host(host, scale, EdgeInsets{});

    auto wv_a = std::make_shared<webview::WebView>();
    auto wv_b = std::make_shared<webview::WebView>();
    const std::size_t token_a = require_value(host.register_view(wv_a.get(), nullptr));
    host.unregister_view(token_a);

    const std::size_t token_b = require_value(host.register_view(wv_b.get(), nullptr));
    AURORA_TEST_CHECK_EQ(token_b, token_a);  // 复用空槽（登记表不随挂卸单调增长）
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);

    host.unregister_view(token_b);
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);
}

AURORA_TEST_CASE(register_view_rejects_unbound_host_or_null_owner) {
    webview::NativeViewHost unbound;
    auto wv = std::make_shared<webview::WebView>();
    AURORA_TEST_CHECK_FALSE(unbound.register_view(wv.get(), nullptr).has_value());
    AURORA_TEST_CHECK_FALSE(unbound.bound());

    webview::NativeViewHost bound;
    const float scale = 1.0F;
    bind_host(bound, scale, EdgeInsets{});
    AURORA_TEST_CHECK_TRUE(bound.bound());
    AURORA_TEST_CHECK_FALSE(bound.register_view(nullptr, nullptr).has_value());
}

AURORA_TEST_CASE(ticket_exposes_host_only_while_host_lives) {
    // 回访票据是控件析构期注销的**唯一**活路：宿主在世时 lock 得到指针，析构后必须失效。
    std::weak_ptr<webview::NativeViewTicket> weak;
    {
        webview::NativeViewHost host;
        const float scale = 1.0F;
        bind_host(host, scale, EdgeInsets{});
        weak = host.ticket();
        AURORA_TEST_CHECK_TRUE(weak.lock() != nullptr);
        AURORA_TEST_CHECK_TRUE(weak.lock()->host == &host);
    }
    AURORA_TEST_CHECK_TRUE(weak.lock() == nullptr);  // 宿主已亡：不得再解引用其地址
}

// ---- ③ 每帧同步：窗口坐标、去重、门控与可见性 ----

AURORA_TEST_CASE(sync_geometry_dispatches_window_rect_at_nested_origin) {
    // 两级嵌套树：控件窗口原点 {20, 30}、尺寸 {320, 240}。缩放 2、CSD 内缩 {8, 4}
    // ⇒ 手算 x = (20+8)×2 = 56，y = (30+4)×2 = 68，w = 640，h = 480。
    // 若下发的是**局部**坐标（少加祖先偏移），x/y 会落到 16 / 8，本条即转红。
    NestedTree f = laid_out_nested_tree(600.0F, 600.0F);
    const Rect wb = require_value(f.wv->window_bounds());
    AURORA_TEST_CHECK_EQ(wb.origin.x, 20.0F);
    AURORA_TEST_CHECK_EQ(wb.origin.y, 30.0F);
    AURORA_TEST_CHECK_EQ(wb.size.width, 320.0F);
    AURORA_TEST_CHECK_EQ(wb.size.height, 240.0F);

    webview::NativeViewHost host;
    const float scale = 2.0F;
    bind_host(host, scale, inset(8.0F, 4.0F));

    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    (void)require_value(host.register_view(f.wv.get(), std::move(sink)));

    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_REQUIRE_EQ(raw->rects.size(), 1U);
    AURORA_TEST_CHECK_EQ(raw->rects.at(0).x, 56);
    AURORA_TEST_CHECK_EQ(raw->rects.at(0).y, 68);
    AURORA_TEST_CHECK_EQ(raw->rects.at(0).width, 640);
    AURORA_TEST_CHECK_EQ(raw->rects.at(0).height, 480);
    AURORA_TEST_CHECK_EQ(raw->visibility.size(), 1U);
    AURORA_TEST_CHECK_TRUE(raw->visibility.at(0));
}

AURORA_TEST_CASE(sync_geometry_skips_pure_paint_frames) {
    NestedTree f = laid_out_nested_tree(600.0F, 600.0F);
    webview::NativeViewHost host;
    const float scale = 1.0F;
    bind_host(host, scale, EdgeInsets{});

    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    (void)require_value(host.register_view(f.wv.get(), std::move(sink)));

    AURORA_TEST_CHECK_EQ(host.sync_geometry(false), 0U);  // 门控关闭：连 window_bounds() 都不查
    AURORA_TEST_CHECK_TRUE(raw->rects.empty());
    AURORA_TEST_CHECK_TRUE(raw->visibility.empty());
}

AURORA_TEST_CASE(sync_geometry_dedups_unchanged_geometry_and_redispatches_on_change) {
    NestedTree f = laid_out_nested_tree(600.0F, 600.0F);
    float scale = 2.0F;
    EdgeInsets csd;
    webview::NativeViewHost host;
    host.bind_window(stub_window(), [&scale]() -> float { return scale; }, [&csd]() -> EdgeInsets { return csd; });

    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    const std::size_t token = require_value(host.register_view(f.wv.get(), std::move(sink)));

    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_REQUIRE_EQ(raw->rects.size(), 1U);
    AURORA_TEST_CHECK_EQ(raw->rects.at(0).width, 640);

    // 几何未变：既不下发、也不计入下发数（判据看的是 sink 实际收到的序列，不只是返回计数）。
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);
    AURORA_TEST_CHECK_EQ(raw->rects.size(), 1U);

    // 缩放档位变了 ⇒ 再下发一次，且新矩形随之改变。
    scale = 1.0F;
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_REQUIRE_EQ(raw->rects.size(), 2U);
    AURORA_TEST_CHECK_EQ(raw->rects.at(1).width, 320);
    const std::optional<webview::NativeViewRect> last = host.last_dispatched_rect(token);
    AURORA_TEST_CHECK_EQ(require_value(last).width, 320);

    // CSD 内缩变了（同一缩放）⇒ 只有原点平移、宽高不变，也必须重新下发。
    csd = inset(8.0F, 8.0F);
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_REQUIRE_EQ(raw->rects.size(), 3U);
    AURORA_TEST_CHECK_EQ(raw->rects.at(2).x, 28);  // (20+8)×1
    AURORA_TEST_CHECK_EQ(raw->rects.at(2).y, 38);  // (30+8)×1
    AURORA_TEST_CHECK_EQ(raw->rects.at(2).width, 320);
}

AURORA_TEST_CASE(sync_geometry_reports_visibility_only_on_transitions) {
    // 零尺寸盒（布局归零）⇒ 不可见；恢复尺寸 ⇒ 可见。可见性只在**翻转**时下发，
    // 连续不可见的帧不得重复上报（否则平台侧每帧都在改窗口样式）。
    NestedTree f = laid_out_nested_tree(600.0F, 600.0F);
    float scale = 1.0F;
    webview::NativeViewHost host;
    bind_host(host, scale, EdgeInsets{});

    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    (void)require_value(host.register_view(f.wv.get(), std::move(sink)));

    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);  // 可见但不重复上报

    LayoutEngine::layout(*f.root, bounded(0.0F, 0.0F));  // 零尺寸盒
    AURORA_TEST_CHECK_TRUE(require_value(f.wv->window_bounds()).size.width <= 0.0F);
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);  // 不可见 ⇒ 不下发矩形

    LayoutEngine::layout(*f.root, bounded(600.0F, 600.0F));  // 恢复：矩形与首次一字不差
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);

    // 关键判据：矩形被去重吞掉时，**可见性恢复不得跟着一起丢**。缺了这一步，原生视图
    // 此后再也不出现，而下发计数与矩形序列全都“看起来正确”。
    const std::vector<bool> expected = {true, false, true};
    AURORA_TEST_CHECK_TRUE(raw->visibility == expected);
    AURORA_TEST_CHECK_EQ(raw->rects.size(), 1U);

    // 缩放随之改变 ⇒ 矩形重新下发；可见性已为 true，故不再重复上报。
    scale = 2.0F;
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_CHECK_EQ(raw->rects.size(), 2U);
    AURORA_TEST_CHECK_EQ(raw->visibility.size(), 3U);
}

AURORA_TEST_CASE(snapshot_mode_view_gets_geometry_but_never_native_visibility) {
    // `show_natively = false`（快照模式的登记形态）：几何照常下发——页面布局尺寸与捕获
    // 像素的尺寸都由这块矩形决定，少发一次页面就按错尺寸重排；可见性则**恒不授予**，
    // 像素只能经 `capture` 出帧。若门控写漏，原生视图会盖在 Painter 内容之上。
    NestedTree f = laid_out_nested_tree(600.0F, 600.0F);
    webview::NativeViewHost host;
    const float scale = 2.0F;
    bind_host(host, scale, EdgeInsets{});

    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    const std::size_t token = require_value(host.register_view(f.wv.get(), std::move(sink), false));

    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    AURORA_TEST_REQUIRE_EQ(raw->rects.size(), 1U);
    AURORA_TEST_CHECK_EQ(raw->rects.at(0).width, 640);
    AURORA_TEST_CHECK_TRUE(raw->visibility.empty());

    // 盒归零再恢复：矩形照常跟随，可见性仍一条不发。
    LayoutEngine::layout(*f.root, bounded(0.0F, 0.0F));
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);
    LayoutEngine::layout(*f.root, bounded(600.0F, 600.0F));
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);  // 矩形与首次一字不差 ⇒ 去重
    AURORA_TEST_CHECK_TRUE(raw->visibility.empty());
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
    AURORA_TEST_CHECK_TRUE(host.view_of(token) == raw);
}

AURORA_TEST_CASE(view_of_returns_the_registered_sink_only_while_registered) {
    webview::NativeViewHost host;
    const float scale = 1.0F;
    bind_host(host, scale, EdgeInsets{});

    auto wv = std::make_shared<webview::WebView>();
    auto sink = std::make_unique<RecordSink>();
    RecordSink *raw = sink.get();
    const std::size_t token = require_value(host.register_view(wv.get(), std::move(sink)));
    AURORA_TEST_CHECK_TRUE(host.view_of(token) == raw);

    host.unregister_view(token);
    AURORA_TEST_CHECK_TRUE(host.view_of(token) == nullptr);  // 注销后不得再交出悬垂视图
    AURORA_TEST_CHECK_TRUE(host.view_of(token + 97U) == nullptr);

    // 降级登记（无视图，仅占位以便同步空转）：如实返回空，而不是替调用方造一个。
    const std::size_t bare = require_value(host.register_view(wv.get(), nullptr));
    AURORA_TEST_CHECK_TRUE(host.view_of(bare) == nullptr);
    AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
}

AURORA_TEST_CASE(sync_geometry_survives_control_death_between_frames) {
    // 真实挂载路径下的拆树顺序：控件经 `on_mount` 自己登记，随树析构自己注销。
    // 宿主持有的是裸 `WebView*`，一旦注销漏掉，下一帧同步就是解引用已释放内存。
    webview::install_engine(std::make_shared<CapableEngine>());
    webview::NativeViewHost host;
    const float scale = 1.0F;
    bind_host(host, scale, EdgeInsets{});
    webview::NativeViewHost::set_current(&host);
    {
        const NestedTree f = laid_out_nested_tree(600.0F, 600.0F);
        AURORA_TEST_CHECK_FALSE(f.wv->degraded());
        AURORA_TEST_CHECK_EQ(host.view_count(), 1U);
        AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 1U);
    }  // 树与控件在此析构（无显式 unmount）
    AURORA_TEST_CHECK_EQ(host.view_count(), 0U);
    AURORA_TEST_CHECK_EQ(host.sync_geometry(true), 0U);
    webview::NativeViewHost::set_current(nullptr);
    webview::install_engine(nullptr);
}

}  // namespace aurora::test_cases::utest_native_view_host
