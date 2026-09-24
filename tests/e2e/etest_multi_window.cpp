/// 测试类型: e2e
/// 目标单元: tools/include/e2e/harness.h
/// 测试说明: 多窗口与窗口生命周期经真实窗口往返。五个用例：
///
///   1. `two_windows_render_route_focus_independently` —— 两窗并存：各自渲染读回（帧尺寸 /
///      中心色独立命中）、经 `Surface::set_event_handler` 各自接线独立 `FocusManager` 后
///      合成窗口事件互不串台、应用内焦点互不干扰（与 utest_multi_window 的抽象层断言互补，
///      这里只断言真实窗口下的可观测行为）。
///   2. `lifecycle_close_rebuild_and_raii` —— 关闭一窗不影响另一窗继续渲染读回；内核 RAII
///      兜底经异常路径验证（中途 throw 后 Session 析构关窗）；关闭后同规格重建与连续开关
///      循环成功（资源不泄漏的可移植运行时证据；OS 层枚举窗口数不可移植，不做）。
///   3. `resize_relayout_converges` —— 程序化 `Surface::set_size` 后整树重排重绘收敛，
///      连续多轮 resize 每轮收敛且帧缓冲与逻辑尺寸内部一致（`framebuffer_size` ==
///      逻辑 × `scale_factor()`，DPI 无关口径，不绑绝对物理像素）；X11/Wayland 未 override
///      `set_size`，skip 桩。
///   4. `partial_dirty_preserves_previous_pixels` —— `present_root` 脏区语义：数据变化但
///      无脏登记时 idle 跳帧（`frame_count` 不增）；手动 `mark_dirty` 局部矩形后仅裁剪区
///      重绘、裁剪外保留上帧像素（树状态已变但读回仍是旧色，整屏刷底色实现会画出新色，
///      两者可区分）；`has_pending_dirty()` 与收敛判据配合。
///   5. `visibility_modes_coexist` —— `NoActivate` 与 `Hidden` 档多窗并存：各窗渲染读回
///      正常、帧计数独立。NoActivate 的 OS 前台行为已在内核探针验证（Windows 前台锁定
///      策略下无人值守断言不可移植），此处只断言并存可用与渲染正确。
///
/// 后端不可用时按 `AURORA_E2E_EXPECT` 记账（语义同 etest_smoke_render：期望集内判失败、
/// 期望集外跳过）。像素采样点按帧尺寸/逻辑尺寸比例映射（DPI 缩放环境下仍取纯色区域内部）。
///
/// 已知口径：多会话交错推进时，`Animator::set_current` 是进程级登记且 `ensure_drivers`
/// 仅首次生效——mount 之后才注册的运行期动画可能落到其他会话的驱动器。本套件用例均为
/// 静态场景（无运行期动画注册），交互动画的推进语义已由 etest_interaction_flow 覆盖。

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "e2e/harness.h"
#include "e2e_expect.h"
#include "framework/aurora_test.h"

namespace au = aurora;
namespace e2e = aurora::e2e;

namespace aurora::test_cases::etest_multi_window {

namespace {

// ---- 场景配色（编译期常量，纯色区域内部采样，无 glyph / AA 边）----

constexpr au::Color AURORA_WIN_RED = au::Color{220, 80, 80, 255};
constexpr au::Color AURORA_WIN_BLUE = au::Color{80, 110, 220, 255};
constexpr au::Color AURORA_WIN_GREEN = au::Color{90, 180, 100, 255};
constexpr au::Color AURORA_WIN_YELLOW = au::Color{225, 200, 80, 255};
constexpr au::Color AURORA_PROBE_STALE = au::Color{255, 120, 120, 255};  // 探针初色（帧 1）
constexpr au::Color AURORA_PROBE_FRESH = au::Color{120, 200, 120, 255};  // 探针新色（重绘后）
constexpr au::Color AURORA_INPUT_FOCUSED = au::Color{255, 248, 220, 255};

/// @brief 后端矩阵取值表：全部真实窗口后端（语义与 etest_smoke_render 一致，Headless 不在表内）。
[[nodiscard]] auto multiwin_backend_values() -> std::vector<e2e::Backend> {
    return {e2e::Backend::Win32, e2e::Backend::D3D11,   e2e::Backend::Glfw,
            e2e::Backend::X11,   e2e::Backend::Wayland, e2e::Backend::Wgpu};
}

/// @brief 取值名生成器：直接用后端短名，报告里一眼定位失败后端。
[[nodiscard]] auto multiwin_backend_name(const e2e::Backend &backend) -> std::string {
    return e2e::backend_name(backend);
}

/// @brief 后端矩阵 fixture（类名不得与其他 etest 文件重名：注册表按类名串合并）。
class MultiWindowBackends : public testing::TestWithParam<e2e::Backend> {};

/// @brief 建窗；失败按期望集记账（本函数不返回）。
[[nodiscard]] auto open_checked(const e2e::WindowSpec &spec) -> e2e::Session {
    auto session = e2e::open(spec);
    if (!session.ok()) {
        e2e::account_unavailable(spec.backend, session.reason());
    }
    return session;
}

/// @brief 读回一帧；读回能力缺失与建窗失败同口径记账。
[[nodiscard]] auto read_checked(const e2e::Session &session) -> e2e::Frame {
    auto read = session.read_pixels();
    if (!read) {
        e2e::account_unavailable(session.backend(), read.error().message);
    }
    return std::move(read.value());  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
}

/// @brief 两颜色各通道差是否都在容差内。
[[nodiscard]] auto color_near(const au::Color &actual, const au::Color &expected, int tol) -> bool {
    const int dr = actual.r - expected.r;
    const int dg = actual.g - expected.g;
    const int db = actual.b - expected.b;
    const int da = actual.a - expected.a;
    return dr >= -tol && dr <= tol && dg >= -tol && dg <= tol && db >= -tol && db <= tol && da >= -tol && da <= tol;
}

/// @brief 像素断言：逻辑坐标按帧/逻辑尺寸比例映射后采样，失败消息携带实测与期望 RGBA。
///
/// 逻辑尺寸显式传参（区别于 etest_interaction_flow 的 spec 绑定形态）：resize 用例中
/// 窗口逻辑尺寸在用例内变化，须以采样当时的逻辑尺寸为映射基准。容差默认 8：所有采样点
/// 均取纯色区域内部（无 AA 边），容差只吸收跨后端上屏取整噪声。
auto expect_pixel(const e2e::Frame &frame, float logical_w, float logical_h, float logical_x, float logical_y,
                  const au::Color &expected, const std::string &what, int tol = 8) -> void {
    const auto map = [](float logical, int frame_extent, int logical_extent) -> int {
        return static_cast<int>(
            std::lround(logical * static_cast<double>(frame_extent) / static_cast<double>(logical_extent)));
    };
    const au::Color actual = e2e::pixel_at(frame, map(logical_x, frame.width, static_cast<int>(logical_w)),
                                           map(logical_y, frame.height, static_cast<int>(logical_h)));
    const std::string detail =
        what + ": sample(" + std::to_string(map(logical_x, frame.width, static_cast<int>(logical_w))) + "," +
        std::to_string(map(logical_y, frame.height, static_cast<int>(logical_h))) + ") actual=(" +
        std::to_string(actual.r) + "," + std::to_string(actual.g) + "," + std::to_string(actual.b) + "," +
        std::to_string(actual.a) + ") expected=(" + std::to_string(expected.r) + "," + std::to_string(expected.g) +
        "," + std::to_string(expected.b) + "," + std::to_string(expected.a) + ")";
    AURORA_TEST_CHECK_MSG(color_near(actual, expected, tol), detail);
}

/// @brief 帧缓冲与逻辑尺寸的**等比率**断言（DPI 无关口径）。
///
/// 不假设「物理 = 逻辑 × scale」：GLFW 软件路径的读回帧是逻辑尺寸（240×160），Win32
/// 家族是物理尺寸（× scale）——乘法关系不是跨后端不变量，等比率才是（缩放不得破坏
/// 纵横比）。比率本身逐窗动态，消息携带 scale_factor 供诊断（SubTask 0.4：同进程首个
/// Win32 窗 scale 恒 1.0、第二个起为系统真实缩放，断言必须动态读，不得绑绝对物理像素）。
auto expect_frame_ratio(const e2e::Session &session, const std::string &what) -> void {
    const e2e::Frame frame = read_checked(session);
    const au::Size sz = session.window().size();
    AURORA_TEST_REQUIRE_MSG(sz.width > 0.0F && sz.height > 0.0F, what + ": 窗口逻辑尺寸非正");
    const double sx = static_cast<double>(frame.width) / static_cast<double>(sz.width);
    const double sy = static_cast<double>(frame.height) / static_cast<double>(sz.height);
    AURORA_TEST_CHECK_MSG(std::fabs(sx - sy) <= 0.02,
                          what + ": 帧与逻辑尺寸不等比 (帧 " + std::to_string(frame.width) + "x" +
                              std::to_string(frame.height) + ", 逻辑 " + std::to_string(static_cast<int>(sz.width)) +
                              "x" + std::to_string(static_cast<int>(sz.height)) +
                              ", scale=" + std::to_string(session.surface().scale_factor()) + ")");
}

/// @brief 建窗规格（tag 进标题便于真机上区分窗口）。
[[nodiscard]] auto make_spec(e2e::Backend backend, int width, int height, std::string_view tag,
                             au::WindowVisibility visibility) -> e2e::WindowSpec {
    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = width;
    spec.height = height;
    spec.title = std::string{"etest_multi_window:"} + std::string{tag} + ":" + e2e::backend_name(backend);
    spec.visibility = visibility;
    return spec;
}

/// @brief 单色块（场景最小单元）。
[[nodiscard]] auto color_block(float width, float height, au::Color color) -> au::Node {
    auto block = std::make_shared<au::Row>();
    block->modifier.set(au::Modifier{}.size(width, height).background(color));
    return au::Node{block};
}

/// @brief 全窗纯色场景（窗口独立渲染的最小判据：中心色命中 + 帧尺寸）。
struct SolidScene {
    au::Node root;

    static auto build(float width, float height, au::Color color) -> SolidScene {
        return SolidScene{.root = color_block(width, height, color)};
    }
};

/// @brief 双用途场景（用例 1）：主色块 + 钉尺寸输入框同树挂载——输入框必须随窗口一起
/// present 布局，`paint_bounds()` 才有值（独立于窗口场景的另一棵树不会被挂载）。
struct TwinScene {
    std::shared_ptr<au::TextInput> input = std::make_shared<au::TextInput>();
    au::Node root;

    static auto build(float width, float height, au::Color color) -> TwinScene {
        TwinScene scene;
        scene.input->modifier.set(au::Modifier{}.size(120.0F, 32.0F));
        scene.input->set_focused_background(AURORA_INPUT_FOCUSED);
        scene.root = au::Node{au::Column{color_block(width, height - 40.0F, color), au::Node{scene.input}}};
        return scene;
    }
};

/// @brief 窗口事件路由接线：计数器 + 独立 FocusManager，对齐 demo_common.h run_demo 纪律
/// （鼠标/键盘/文本派发必须携带 FocusManager）。
[[nodiscard]] auto make_route(au::Node &root, au::FocusManager &fm, int &counter) -> std::function<void(au::Event &)> {
    fm.set_root(&root.widget());
    return [&root, &fm, &counter](au::Event &e) -> void {
        ++counter;
        auto &wd = root.widget();
        if (auto *me = dynamic_cast<au::MouseEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *me, &fm);
        } else if (auto *ke = dynamic_cast<au::KeyEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *ke, fm);
        } else if (auto *se = dynamic_cast<au::ScrollEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *se);
        } else if (auto *te = dynamic_cast<au::TextInputEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *te, fm);
        }
    };
}

/// @brief 中心点（矩形几何 helper）。
[[nodiscard]] auto center_of(const au::Rect &r) -> au::Point {
    return au::Point{.x = r.origin.x + (r.size.width * 0.5F), .y = r.origin.y + (r.size.height * 0.5F)};
}

/// @brief 合成一次完整点击（Press + Release）并经路由派发（窗口坐标语义）。
auto click_via_route(const std::function<void(au::Event &)> &route, au::Point position) -> void {
    au::MouseEvent press;
    press.action = au::MouseAction::Press;
    press.button = au::MouseButton::Left;
    press.position = position;
    route(press);
    au::MouseEvent release = press;
    release.action = au::MouseAction::Release;
    route(release);
}

/// @brief 纯绘制探针控件：paint 输出只取决于公共成员 `fill`，成员变更**不触发任何脏登记**
/// （不调 on_subtree_dirty / mark_needs_layout）——用例 4 据此构造「树状态已变但无脏」的
/// 状态，验证窗口的 idle 跳帧与部分脏区语义。
class PaintProbe final : public au::LeafWidget {
  public:
    au::Color fill = AURORA_PROBE_STALE;

    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "PaintProbe"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "PaintProbe", .children_policy = "none"};
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = c.max.height});
    }

    void on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) override {
        p.fill_rect(bounds, fill);
    }
};

/// @brief 探针场景（用例 4）：根控件即 PaintProbe，占满整窗。
struct ProbeScene {
    std::shared_ptr<PaintProbe> probe = std::make_shared<PaintProbe>();
    au::Node root{probe};
};

}  // namespace

// ============================================================
// 用例 1：两窗并存——独立渲染、独立事件路由、应用内焦点互不干扰
// ============================================================

AURORA_TEST_P(MultiWindowBackends, two_windows_render_route_focus_independently) {
    const e2e::Backend backend = param();

    // 两窗不同尺寸 + 不同颜色：帧与像素不可能混淆。输入框随场景树挂载（点击获焦用）。
    const e2e::WindowSpec spec_a = make_spec(backend, 240, 160, "twinA", au::WindowVisibility::Hidden);
    const e2e::WindowSpec spec_b = make_spec(backend, 200, 140, "twinB", au::WindowVisibility::Hidden);
    auto session_a = open_checked(spec_a);
    auto session_b = open_checked(spec_b);
    TwinScene scene_a = TwinScene::build(240.0F, 160.0F, AURORA_WIN_RED);
    TwinScene scene_b = TwinScene::build(200.0F, 140.0F, AURORA_WIN_BLUE);

    AURORA_TEST_REQUIRE(static_cast<bool>(session_a.present(scene_a.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session_b.present(scene_b.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session_a.pump_until_settled()));
    AURORA_TEST_REQUIRE(static_cast<bool>(session_b.pump_until_settled()));

    // ---- 独立渲染：帧等比率 + 色块区中心色各自命中 ----
    expect_frame_ratio(session_a, "窗口A 帧等比");
    expect_frame_ratio(session_b, "窗口B 帧等比");
    expect_pixel(read_checked(session_a), 240.0F, 160.0F, 120.0F, 60.0F, AURORA_WIN_RED, "窗口A 中心色");
    expect_pixel(read_checked(session_b), 200.0F, 140.0F, 100.0F, 50.0F, AURORA_WIN_BLUE, "窗口B 中心色");

    // ---- 独立事件路由：每窗各自 handler + 计数器，事件互不串台 ----
    au::FocusManager fm_a;
    au::FocusManager fm_b;
    int events_a = 0;
    int events_b = 0;
    auto route_a = make_route(scene_a.root, fm_a, events_a);
    auto route_b = make_route(scene_b.root, fm_b, events_b);
    session_a.surface().set_event_handler(route_a);
    session_b.surface().set_event_handler(route_b);

    // 向 A 注入一次完整点击（2 个事件）→ 只有 A 的计数器前进。
    click_via_route(route_a, center_of(scene_a.input->paint_bounds()));
    AURORA_TEST_CHECK_MSG(events_a == 2, "窗口A 应收到 2 个事件，实际 " + std::to_string(events_a));
    AURORA_TEST_CHECK_MSG(events_b == 0, "窗口B 不应收到事件，实际 " + std::to_string(events_b));
    AURORA_TEST_CHECK_TRUE(scene_a.input->is_focused());

    // 向 B 注入 → B 计数前进，A 不变；两窗焦点互不干扰（各自 FocusManager）。
    click_via_route(route_b, center_of(scene_b.input->paint_bounds()));
    AURORA_TEST_CHECK_MSG(events_a == 2, "窗口A 计数不应被 B 的注入改变，实际 " + std::to_string(events_a));
    AURORA_TEST_CHECK_MSG(events_b == 2, "窗口B 应收到 2 个事件，实际 " + std::to_string(events_b));
    AURORA_TEST_CHECK_TRUE(scene_a.input->is_focused());
    AURORA_TEST_CHECK_TRUE(scene_b.input->is_focused());
    AURORA_TEST_CHECK_TRUE(fm_a.focused() == scene_a.input.get());
    AURORA_TEST_CHECK_TRUE(fm_b.focused() == scene_b.input.get());

    // 像素通道：B 获焦后其聚焦背景生效（采样输入框右侧无文本区）。
    AURORA_TEST_REQUIRE(static_cast<bool>(session_b.pump(2)));
    const au::Rect &inp_b = scene_b.input->paint_bounds();
    expect_pixel(read_checked(session_b), 200.0F, 140.0F, inp_b.origin.x + inp_b.size.width - 8.0F,
                 inp_b.origin.y + (inp_b.size.height * 0.5F), AURORA_INPUT_FOCUSED, "B 窗聚焦背景");
}

// ============================================================
// 用例 2：生命周期——关一窗余窗可用、RAII 异常兜底、重建无残留
// ============================================================

AURORA_TEST_P(MultiWindowBackends, lifecycle_close_rebuild_and_raii) {
    const e2e::Backend backend = param();
    const e2e::WindowSpec spec_a = make_spec(backend, 220, 150, "lifeA", au::WindowVisibility::Hidden);
    const e2e::WindowSpec spec_b = make_spec(backend, 200, 140, "lifeB", au::WindowVisibility::Hidden);

    // ---- 两窗并存后显式关闭其一，另一窗继续渲染读回 ----
    e2e::Session keep;
    {
        auto closing = open_checked(spec_a);
        auto kept = open_checked(spec_b);
        SolidScene scene_closing = SolidScene::build(220.0F, 150.0F, AURORA_WIN_RED);
        SolidScene scene_kept = SolidScene::build(200.0F, 140.0F, AURORA_WIN_BLUE);
        AURORA_TEST_REQUIRE(static_cast<bool>(closing.present(scene_closing.root)));
        AURORA_TEST_REQUIRE(static_cast<bool>(kept.present(scene_kept.root)));
        expect_pixel(read_checked(closing), 220.0F, 150.0F, 110.0F, 75.0F, AURORA_WIN_RED, "关闭前窗口A 中心色");
        keep = std::move(kept);  // 移出子作用域；closing 在作用域结束即析构关闭
    }
    AURORA_TEST_REQUIRE(static_cast<bool>(keep.pump(2)));
    expect_pixel(read_checked(keep), 200.0F, 140.0F, 100.0F, 70.0F, AURORA_WIN_BLUE, "A 关闭后 B 仍可读回");

    // ---- RAII 兜底：中途故意失败（异常路径）不留窗口 ----
    bool threw = false;
    try {
        auto doomed = open_checked(spec_a);
        SolidScene scene_doomed = SolidScene::build(220.0F, 150.0F, AURORA_WIN_RED);
        AURORA_TEST_REQUIRE(static_cast<bool>(doomed.present(scene_doomed.root)));
        throw std::runtime_error("deliberate mid-test failure");
    } catch (const std::runtime_error &) {
        threw = true;  // 栈展开时 doomed 析构 → 窗口关闭
    }
    AURORA_TEST_CHECK_TRUE(threw);

    // ---- 无幽灵窗的可移植运行时证据：异常路径后同规格重建成功；连续开关循环无衰减 ----
    for (int round = 0; round < 3; ++round) {
        auto rebuilt = open_checked(spec_a);
        SolidScene scene_rebuilt = SolidScene::build(220.0F, 150.0F, AURORA_WIN_GREEN);
        AURORA_TEST_REQUIRE_MSG(static_cast<bool>(rebuilt.present(scene_rebuilt.root)),
                                "第 " + std::to_string(round) + " 轮重建后 present 失败");
        expect_pixel(read_checked(rebuilt), 220.0F, 150.0F, 110.0F, 75.0F, AURORA_WIN_GREEN,
                     "第 " + std::to_string(round) + " 轮重建渲染");
    }
}

// ============================================================
// 用例 3：程序化 resize——整树重排重绘收敛、连续 resize、DPI 无关口径
// ============================================================

AURORA_TEST_P(MultiWindowBackends, resize_relayout_converges) {
    const e2e::Backend backend = param();

    // `Surface::set_size` 实现面与传导链（实测）：
    //   - X11/Wayland：未 override `set_size`（虚默认空实现）→ skip 桩；
    //   - 非 Windows 宿主的 wgpu：宿主无 `set_size` override → skip 桩；
    //   - GLFW：override 存在（glfwSetWindowSize），但 surface 逻辑尺寸缓存仅在 begin_frame
    //     更新，而 set_size 后窗口无脏登记 → present_root 判 idle 跳帧 → begin_frame 不执行
    //     → 尺寸永不更新（idle 死锁），程序化 resize 不传导到帧缓冲。库层缺口，记录在案
    //     （specification/08-tooling.md §8.2），待独立修复后放开。
#if defined(AURORA_BACKEND_X11) || defined(AURORA_BACKEND_WAYLAND)
    if (backend == e2e::Backend::X11 || backend == e2e::Backend::Wayland) {
        AURORA_TEST_SKIP("set_size not implemented on this backend (no override)");
    }
#endif
    if (backend == e2e::Backend::Glfw) {
        AURORA_TEST_SKIP(
            "set_size does not propagate: surface size cache updates only in begin_frame, "
            "which idle frames skip (library-level gap, recorded)");
    }
#ifndef _WIN32
    if (backend == e2e::Backend::Wgpu) {
        AURORA_TEST_SKIP("wgpu host on this platform has no set_size override");
    }
#endif

    // 固定尺寸双块（已验证形态）：初始 240×160 恰好满屏（红左蓝右）；放大后右/下露出
    // 底色，缩小后块被裁——「重排 + 新帧呈现」可观测。窗口档位 NoActivate（可见但不激活，
    // 不打扰桌面）。
    const e2e::WindowSpec spec = make_spec(backend, 240, 160, "resize", au::WindowVisibility::NoActivate);
    auto session = open_checked(spec);
    au::Node root{au::Row{color_block(120.0F, 160.0F, AURORA_WIN_RED), color_block(120.0F, 160.0F, AURORA_WIN_BLUE)}};

    // ---- 初始尺寸：240×160，块区恰好满屏 ----
    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump_until_settled()));
    const au::Size initial_logical = session.window().size();
    const e2e::Frame initial_frame = read_checked(session);
    expect_frame_ratio(session, "resize 前帧等比");
    expect_pixel(initial_frame, initial_logical.width, initial_logical.height, 60.0F, 80.0F, AURORA_WIN_RED,
                 "resize 前左块");
    expect_pixel(initial_frame, initial_logical.width, initial_logical.height, 180.0F, 80.0F, AURORA_WIN_BLUE,
                 "resize 前右块");

    // ---- 放大：320×240（重排可观测：帧随动 + 块区 dp 采样仍正确）----
    session.surface().set_size(au::Size{.width = 320.0F, .height = 240.0F});
    AURORA_TEST_REQUIRE_MSG(static_cast<bool>(session.pump_until_settled()), "放大 resize 未收敛");
    const e2e::Frame enlarged_frame = read_checked(session);
    AURORA_TEST_CHECK_MSG(enlarged_frame.width != initial_frame.width || enlarged_frame.height != initial_frame.height,
                          "放大 resize 后帧尺寸应随动 (前 " + std::to_string(initial_frame.width) + "x" +
                              std::to_string(initial_frame.height) + ", 后 " + std::to_string(enlarged_frame.width) +
                              "x" + std::to_string(enlarged_frame.height) + ")");
    expect_frame_ratio(session, "放大后帧等比");
    const au::Size enlarged_logical = session.window().size();
    expect_pixel(enlarged_frame, enlarged_logical.width, enlarged_logical.height, 60.0F, 80.0F, AURORA_WIN_RED,
                 "放大后左块");
    expect_pixel(enlarged_frame, enlarged_logical.width, enlarged_logical.height, 180.0F, 80.0F, AURORA_WIN_BLUE,
                 "放大后右块");

    // ---- 连续 resize 收敛（含缩小）：每轮收敛、帧随动、等比 ----
    const std::vector<au::Size> rounds = {au::Size{.width = 200.0F, .height = 120.0F},
                                          au::Size{.width = 300.0F, .height = 180.0F},
                                          au::Size{.width = 160.0F, .height = 100.0F}};
    e2e::Frame prev_frame = enlarged_frame;
    for (std::size_t i = 0; i < rounds.size(); ++i) {
        session.surface().set_size(rounds[i]);  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_REQUIRE_MSG(static_cast<bool>(session.pump_until_settled()),
                                "第 " + std::to_string(i) + " 轮 resize 未收敛");
        const e2e::Frame round_frame = read_checked(session);
        AURORA_TEST_CHECK_MSG(round_frame.width != prev_frame.width || round_frame.height != prev_frame.height,
                              "第 " + std::to_string(i) + " 轮 resize 后帧尺寸应随动 (前 " +
                                  std::to_string(prev_frame.width) + "x" + std::to_string(prev_frame.height) + ", 后 " +
                                  std::to_string(round_frame.width) + "x" + std::to_string(round_frame.height) + ")");
        expect_frame_ratio(session, "第 " + std::to_string(i) + " 轮 resize 帧等比");
        prev_frame = round_frame;
    }

    // ---- 末轮像素：缩小后红块仍覆盖窗口左上（160×100 ⊂ 红块 120×160 的前段？否——
    // 红块 [0,120)×[0,160)，窗口 [0,160)×[0,100)：红块内采样 (60,50)dp 命中 ----
    const au::Size final_logical = session.window().size();
    expect_pixel(prev_frame, final_logical.width, final_logical.height, 60.0F, 50.0F, AURORA_WIN_RED, "缩小后红块区");
}

// ============================================================
// 用例 4：脏区语义——idle 跳帧、部分脏区保留上帧像素、has_pending_dirty 配合
// ============================================================

AURORA_TEST_P(MultiWindowBackends, partial_dirty_preserves_previous_pixels) {
    const e2e::Backend backend = param();
    const e2e::WindowSpec spec = make_spec(backend, 200, 160, "dirty", au::WindowVisibility::Hidden);
    auto session = open_checked(spec);
    ProbeScene scene;

    // ---- 帧 1：全屏旧色 ----
    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(scene.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump_until_settled()));
    expect_pixel(read_checked(session), 200.0F, 160.0F, 100.0F, 80.0F, AURORA_PROBE_STALE, "帧 1 探针色");
    const int frames_after_first = session.frame_count();

    // ---- 数据变化但无脏登记 → idle 跳帧，帧计数不增 ----
    scene.probe->fill = AURORA_PROBE_FRESH;
    AURORA_TEST_CHECK_FALSE(session.has_pending_dirty());
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(1)));
    AURORA_TEST_CHECK_MSG(session.is_idle_frame(), "无脏登记时应 idle 跳帧");
    AURORA_TEST_CHECK_MSG(session.frame_count() == frames_after_first,
                          "idle 跳帧不应产生新呈现帧 (before=" + std::to_string(frames_after_first) +
                              ", after=" + std::to_string(session.frame_count()) + ")");

    // ---- 只标脏右半：仅裁剪区重绘（新色），裁剪外保留上帧像素（旧色）----
    // 探针 fill 已是新色：若实现整屏刷底色/整屏重绘，左半也会被画成新色；正确实现下
    // 裁剪外沿用上帧（window.h partial-clip 路径：跳过 begin_frame 保留上帧缓冲）。
    const float half_w = static_cast<float>(spec.width) * 0.5F;
    session.window().mark_dirty(au::Rect{.origin = au::Point{.x = half_w, .y = 0.0F},
                                         .size = au::Size{.width = half_w, .height = static_cast<float>(spec.height)}});
    AURORA_TEST_CHECK_TRUE(session.has_pending_dirty());
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(1)));
    AURORA_TEST_CHECK_FALSE(session.is_idle_frame());
    expect_pixel(read_checked(session), 200.0F, 160.0F, 50.0F, 80.0F, AURORA_PROBE_STALE, "脏区外保留上帧像素");
    expect_pixel(read_checked(session), 200.0F, 160.0F, 150.0F, 80.0F, AURORA_PROBE_FRESH, "脏区内重绘为新内容");

    // ---- 再标脏左半：这次左半真重绘，新色生效 ----
    session.window().mark_dirty(au::Rect{.origin = au::Point{.x = 0.0F, .y = 0.0F},
                                         .size = au::Size{.width = half_w, .height = static_cast<float>(spec.height)}});
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(1)));
    expect_pixel(read_checked(session), 200.0F, 160.0F, 50.0F, 80.0F, AURORA_PROBE_FRESH, "补标脏后左半重绘");
}

// ============================================================
// 用例 5：可见性档位——NoActivate 与 Hidden 多窗并存，渲染读回正常
// ============================================================

AURORA_TEST_P(MultiWindowBackends, visibility_modes_coexist) {
    const e2e::Backend backend = param();

    // 四窗并存：NoActivate × 2 + Hidden × 2，尺寸颜色各异。
    const e2e::WindowSpec spec_na_a = make_spec(backend, 220, 140, "naA", au::WindowVisibility::NoActivate);
    const e2e::WindowSpec spec_na_b = make_spec(backend, 200, 130, "naB", au::WindowVisibility::NoActivate);
    const e2e::WindowSpec spec_hid_a = make_spec(backend, 180, 120, "hidA", au::WindowVisibility::Hidden);
    const e2e::WindowSpec spec_hid_b = make_spec(backend, 160, 110, "hidB", au::WindowVisibility::Hidden);
    auto na_a = open_checked(spec_na_a);
    auto na_b = open_checked(spec_na_b);
    auto hid_a = open_checked(spec_hid_a);
    auto hid_b = open_checked(spec_hid_b);
    SolidScene scene_na_a = SolidScene::build(220.0F, 140.0F, AURORA_WIN_RED);
    SolidScene scene_na_b = SolidScene::build(200.0F, 130.0F, AURORA_WIN_BLUE);
    SolidScene scene_hid_a = SolidScene::build(180.0F, 120.0F, AURORA_WIN_GREEN);
    SolidScene scene_hid_b = SolidScene::build(160.0F, 110.0F, AURORA_WIN_YELLOW);

    AURORA_TEST_REQUIRE(static_cast<bool>(na_a.present(scene_na_a.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(na_b.present(scene_na_b.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(hid_a.present(scene_hid_a.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(hid_b.present(scene_hid_b.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(na_a.pump_until_settled()));
    AURORA_TEST_REQUIRE(static_cast<bool>(na_b.pump_until_settled()));
    AURORA_TEST_REQUIRE(static_cast<bool>(hid_a.pump_until_settled()));
    AURORA_TEST_REQUIRE(static_cast<bool>(hid_b.pump_until_settled()));

    // 各窗中心色独立命中 + 帧计数独立推进。
    expect_pixel(read_checked(na_a), 220.0F, 140.0F, 110.0F, 70.0F, AURORA_WIN_RED, "NoActivate A 中心色");
    expect_pixel(read_checked(na_b), 200.0F, 130.0F, 100.0F, 65.0F, AURORA_WIN_BLUE, "NoActivate B 中心色");
    expect_pixel(read_checked(hid_a), 180.0F, 120.0F, 90.0F, 60.0F, AURORA_WIN_GREEN, "Hidden A 中心色");
    expect_pixel(read_checked(hid_b), 160.0F, 110.0F, 80.0F, 55.0F, AURORA_WIN_YELLOW, "Hidden B 中心色");

    const int frames_na_a = na_a.frame_count();
    const int frames_na_b = na_b.frame_count();
    AURORA_TEST_REQUIRE(static_cast<bool>(na_a.pump(1)));
    AURORA_TEST_CHECK_MSG(na_a.frame_count() >= frames_na_a,
                          "NoActivate A 帧计数应推进或持平 (before=" + std::to_string(frames_na_a) +
                              ", after=" + std::to_string(na_a.frame_count()) + ")");
    AURORA_TEST_CHECK_MSG(na_b.frame_count() == frames_na_b,
                          "NoActivate B 帧计数不应被 A 的推进改变 (before=" + std::to_string(frames_na_b) +
                              ", after=" + std::to_string(na_b.frame_count()) + ")");
}

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(multiwin_backends, MultiWindowBackends, multiwin_backend_values(),
                                    multiwin_backend_name);

}  // namespace aurora::test_cases::etest_multi_window
