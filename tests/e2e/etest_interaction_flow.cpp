/// 测试类型: e2e
/// 目标单元: tools/include/e2e/harness.h
/// 测试说明: 交互流（指针 / 键盘 / 文本输入）经真实窗口往返。三个用例：
///
///   1. `interaction_round_trip` —— 点击 / 拖拽 / 滚动 / 文本输入四类交互经内核注入
///      （`Inspector::simulate_*` 目标式语义，走真实命中测试 + 冒泡派发）往返：每类交互
///      均断言**控件状态 + 渲染像素双通道**，按序独立推进、独立记账，失败时携带实测值
///      （状态值 / 采样点 RGBA / 期望 RGBA），可分别诊断。
///   2. `focus_and_keyboard_routing_via_window_dispatch` —— 窗口事件路由面：经
///      `Surface::set_event_handler` 接线 `FocusManager`（对齐 demo_common.h `run_demo`
///      「鼠标派发必须携带 FocusManager」纪律），合成平台事件（窗口坐标）走
///      命中测试 → 焦点 → 键盘路由的完整链路，与用例 1 的目标式注入互补。
///   3. `animation_converges_after_interaction` —— 交互触发的动画（点击回调启动 Scroll
///      收位滑动）：注入后立即处于滑动中，`pump_until_settled` 收敛后偏移与像素达到稳定态。
///
/// 后端不可用时按 `AURORA_E2E_EXPECT` 记账（语义同 etest_smoke_render：期望集内判失败、
/// 期望集外跳过）。像素采样点按帧尺寸/逻辑尺寸比例映射（DPI 缩放环境下仍取纯色区域内部，
/// 不依赖逐位口径），无 glyph 采样。

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "e2e/harness.h"
#include "e2e_expect.h"
#include "framework/aurora_test.h"

namespace au = aurora;
namespace e2e = aurora::e2e;

namespace aurora::test_cases::etest_interaction_flow {

namespace {

// ---- 场景配色（编译期常量，纯色区域内部采样，无 glyph / AA 边）----

constexpr au::Color AURORA_SCENE_BG = au::Color{255, 255, 255, 255};
constexpr au::Color AURORA_BLOCK_0 = au::Color{255, 120, 120, 255};
constexpr au::Color AURORA_BLOCK_1 = au::Color{120, 200, 120, 255};
constexpr au::Color AURORA_BLOCK_2 = au::Color{120, 120, 255, 255};
constexpr au::Color AURORA_SWITCH_OFF = au::Color{180, 180, 180, 255};
constexpr au::Color AURORA_SWITCH_ON = au::Color{30, 140, 255, 255};
constexpr au::Color AURORA_SLIDER_OFF = au::Color{210, 210, 210, 255};
constexpr au::Color AURORA_SLIDER_ON = au::Color{30, 140, 255, 255};
constexpr au::Color AURORA_KNOB = au::Color{255, 255, 255, 255};
constexpr au::Color AURORA_INPUT_FOCUSED = au::Color{255, 248, 220, 255};

/// @brief 后端矩阵取值表：全部真实窗口后端（语义与 etest_smoke_render 一致，Headless 不在表内）。
[[nodiscard]] auto flow_backend_values() -> std::vector<e2e::Backend> {
    return {e2e::Backend::Win32, e2e::Backend::D3D11,   e2e::Backend::Glfw,
            e2e::Backend::X11,   e2e::Backend::Wayland, e2e::Backend::Wgpu};
}

/// @brief 取值名生成器：直接用后端短名，报告里一眼定位失败后端。
[[nodiscard]] auto flow_backend_name(const e2e::Backend &backend) -> std::string { return e2e::backend_name(backend); }

/// @brief 后端矩阵 fixture。
class InteractionBackends : public testing::TestWithParam<e2e::Backend> {};

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
/// 容差默认 8：所有采样点均取纯色区域内部（无 AA 边），容差只吸收跨后端上屏取整噪声。
auto expect_pixel(const e2e::Frame &frame, const e2e::WindowSpec &spec, float logical_x, float logical_y,
                  const au::Color &expected, const std::string &what, int tol = 8) -> void {
    const auto map = [](float logical, int frame_extent, int logical_extent) -> int {
        return static_cast<int>(
            std::lround(logical * static_cast<double>(frame_extent) / static_cast<double>(logical_extent)));
    };
    const au::Color actual =
        e2e::pixel_at(frame, map(logical_x, frame.width, spec.width), map(logical_y, frame.height, spec.height));
    const std::string detail = what + ": 采样(" + std::to_string(map(logical_x, frame.width, spec.width)) + "," +
                               std::to_string(map(logical_y, frame.height, spec.height)) + ") 实际=(" +
                               std::to_string(actual.r) + "," + std::to_string(actual.g) + "," +
                               std::to_string(actual.b) + "," + std::to_string(actual.a) + ") 期望=(" +
                               std::to_string(expected.r) + "," + std::to_string(expected.g) + "," +
                               std::to_string(expected.b) + "," + std::to_string(expected.a) + ")";
    AURORA_TEST_CHECK_MSG(color_near(actual, expected, tol), detail);
}

/// @brief 纯色块（滚动内容的最小单元）。
[[nodiscard]] auto color_block(float width, float height, au::Color color) -> au::Node {
    auto block = std::make_shared<au::Row>();
    block->modifier.set(au::Modifier{}.size(width, height).background(color));
    return au::Node{block};
}

/// @brief 三块竖排的滚动内容（每块 80dp 高，总内容高 240dp）。
[[nodiscard]] auto build_scroll_content() -> au::Node {
    return au::Node{au::Column{color_block(320.0F, 80.0F, AURORA_BLOCK_0), color_block(320.0F, 80.0F, AURORA_BLOCK_1),
                               color_block(320.0F, 80.0F, AURORA_BLOCK_2)}};
}

/// @brief 四类交互共用场景（用例 1）：按钮（点击回调翻转开关并计数）+ 开关 + 滑杆 + 文本框 + 滚动容器。
struct FlowScene {
    au::Node root;
    au::Button *button = nullptr;
    au::Switch *toggle = nullptr;
    au::Slider *slider = nullptr;
    au::Scroll *scroller = nullptr;
    au::TextInput *input = nullptr;
    std::shared_ptr<int> clicks;  ///< shared_ptr 持有：场景按值移动后回调捕获仍有效
};

/// @brief 构建用例 1 场景：320×200（行 A 36 + 行 B 28 + 行 C 136）。
[[nodiscard]] auto build_flow_scene() -> FlowScene {
    auto btn = std::make_shared<au::Button>("tap");
    auto sw = std::make_shared<au::Switch>();
    // 显式钉住 44×24：行交叉轴默认拉伸会把开关撑高（滑块直径随之变大，盖住采样点）。
    sw->modifier.set(au::Modifier{}.size(44.0F, 24.0F));
    sw->set_inactive_color(AURORA_SWITCH_OFF).set_active_color(AURORA_SWITCH_ON).set_thumb_color(AURORA_KNOB);
    auto input = std::make_shared<au::TextInput>();
    input->set_focused_background(AURORA_INPUT_FOCUSED);
    input->modifier.set(au::Modifier{}.size(120.0F, 32.0F));
    auto slider = std::make_shared<au::Slider>();
    slider->set_range(0.0, 100.0)
        .set_active_color(AURORA_SLIDER_ON)
        .set_inactive_color(AURORA_SLIDER_OFF)
        .set_thumb_color(AURORA_KNOB);
    slider->modifier.set(au::Modifier{}.size(240.0F, 24.0F));
    auto scroller = std::make_shared<au::Scroll>(au::ScrollProps{.child = build_scroll_content()});

    auto clicks = std::make_shared<int>(0);
    btn->on_click = [sw, clicks]() {
        ++(*clicks);
        sw->set_value(!sw->value());
    };

    au::Node row_a{au::Row{au::Node{btn}, au::Node{sw}, au::Node{input}}};
    row_a.widget().modifier.set(au::Modifier{}.size(320.0F, 36.0F).background(AURORA_SCENE_BG));
    au::Node row_b{au::Row{au::Node{slider}}};
    row_b.widget().modifier.set(au::Modifier{}.size(320.0F, 28.0F).background(AURORA_SCENE_BG));
    au::Node row_c{au::Row{au::Node{scroller}}};
    row_c.widget().modifier.set(au::Modifier{}.size(320.0F, 136.0F).background(AURORA_SCENE_BG));
    au::Node root{au::Column{row_a, row_b, row_c}};

    return FlowScene{.root = std::move(root),
                     .button = btn.get(),
                     .toggle = sw.get(),
                     .slider = slider.get(),
                     .scroller = scroller.get(),
                     .input = input.get(),
                     .clicks = std::move(clicks)};
}

/// @brief 焦点路由场景（用例 2）：单文本框，预置文本 "ab"。
struct FocusScene {
    au::Node root;
    au::TextInput *input = nullptr;
};

/// @brief 构建用例 2 场景：320×80 单行。
[[nodiscard]] auto build_focus_scene() -> FocusScene {
    auto input = std::make_shared<au::TextInput>();
    input->set_value("ab").set_focused_background(AURORA_INPUT_FOCUSED);
    input->modifier.set(au::Modifier{}.size(160.0F, 32.0F));
    au::Node row{au::Row{au::Node{input}}};
    row.widget().modifier.set(au::Modifier{}.size(320.0F, 36.0F).background(AURORA_SCENE_BG));
    au::Node root{au::Column{row}};
    return FocusScene{.root = std::move(root), .input = input.get()};
}

/// @brief 动画收敛场景（用例 3）：按钮回调以动画滚动到内容底部。
struct GlideScene {
    au::Node root;
    au::Button *jump = nullptr;
    au::Scroll *scroller = nullptr;
};

/// @brief 构建用例 3 场景：320×176（行 A 36 + 行 C 140）。
[[nodiscard]] auto build_glide_scene() -> GlideScene {
    auto btn = std::make_shared<au::Button>("jump");
    auto scroller = std::make_shared<au::Scroll>(au::ScrollProps{.child = build_scroll_content()});
    btn->on_click = [scroller]() {
        (void)scroller->scroll_to(10000.0F, true);  // 目标越界：内部夹取到内容底部
    };
    au::Node row_a{au::Row{au::Node{btn}}};
    row_a.widget().modifier.set(au::Modifier{}.size(320.0F, 36.0F).background(AURORA_SCENE_BG));
    au::Node row_c{au::Row{au::Node{scroller}}};
    row_c.widget().modifier.set(au::Modifier{}.size(320.0F, 140.0F).background(AURORA_SCENE_BG));
    au::Node root{au::Column{row_a, row_c}};
    return GlideScene{.root = std::move(root), .jump = btn.get(), .scroller = scroller.get()};
}

/// @brief 控件中心的窗口坐标（逻辑 dp；合成事件与采样共用此口径）。
[[nodiscard]] auto center_of(const au::Rect &bounds) -> au::Point {
    return au::Point{.x = bounds.origin.x + (bounds.size.width * 0.5F),
                     .y = bounds.origin.y + (bounds.size.height * 0.5F)};
}

}  // namespace

// ============================================================
// 用例 1：四类交互经真实窗口往返（状态 + 像素双通道）
// ============================================================

AURORA_TEST_P(InteractionBackends, interaction_round_trip) {
    const e2e::Backend backend = param();

    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = 320;
    spec.height = 200;
    spec.title = std::string{"etest_interaction_flow:round_trip:"} + e2e::backend_name(backend);
    spec.visibility = au::WindowVisibility::Hidden;

    auto session = open_checked(spec);
    FlowScene scene = build_flow_scene();
    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(scene.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump_until_settled()));

    const au::Rect &tg = scene.toggle->paint_bounds();
    const au::Rect &sl = scene.slider->paint_bounds();
    const au::Rect &scb = scene.scroller->paint_bounds();
    const au::Rect &inp = scene.input->paint_bounds();

    // ---- 交互前基准帧：各采样点取纯色内部，初始态即校验（同时防采样点选偏）----
    const e2e::Frame baseline = read_checked(session);
    expect_pixel(baseline, spec, tg.origin.x + 12.0F, tg.origin.y + (tg.size.height * 0.5F), AURORA_KNOB,
                 "开关初始滑块(关态左端)");
    expect_pixel(baseline, spec, sl.origin.x + (sl.size.width * 0.5F), sl.origin.y + (sl.size.height * 0.5F),
                 AURORA_SLIDER_OFF, "滑杆初始轨道(值 0 无填充)");
    expect_pixel(baseline, spec, scb.origin.x + (scb.size.width * 0.5F), scb.origin.y + (scb.size.height * 0.5F),
                 AURORA_BLOCK_0, "滚动视口初始内容(块 0)");
    AURORA_TEST_CHECK_MSG(scene.input->value().empty(), "文本框初始值为空");

    // ---- 点击：Button 回调翻转 Switch（状态通道：计数 + 开关值；像素通道：滑块移位 + 轨道色）----
    AURORA_TEST_REQUIRE(static_cast<bool>(session.tap(*scene.button)));
    AURORA_TEST_CHECK_EQ(*scene.clicks, 1);
    AURORA_TEST_CHECK_TRUE(scene.toggle->value());
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(2)));
    expect_pixel(read_checked(session), spec, tg.origin.x + 12.0F, tg.origin.y + (tg.size.height * 0.5F),
                 AURORA_SWITCH_ON, "点击后开关轨道(开态色，滑块已右移)");

    // ---- 拖拽：Slider 值随指针位移（状态通道：值解析式；像素通道：填充段扩张）----
    const float slider_w = sl.size.width;
    const float dx = 0.3F * slider_w;
    AURORA_TEST_REQUIRE(static_cast<bool>(session.drag(*scene.slider, au::Point{.x = dx, .y = 0.0F})));
    // Slider 值映射：local_x → (local_x - 4dp 内缩) / 轨道宽；按下点即控件中心。
    const double expected_value = 100.0 * ((0.5F * slider_w + dx - 4.0F) / (slider_w - 8.0F));
    AURORA_TEST_CHECK_NEAR(scene.slider->value(), expected_value, 1e-3);
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(2)));
    const e2e::Frame after_drag = read_checked(session);
    expect_pixel(after_drag, spec, sl.origin.x + (sl.size.width * 0.5F), sl.origin.y + (sl.size.height * 0.5F),
                 AURORA_SLIDER_ON, "拖拽后填充段覆盖轨道中点");
    expect_pixel(after_drag, spec, sl.origin.x + sl.size.width - 12.0F, sl.origin.y + (sl.size.height * 0.5F),
                 AURORA_SLIDER_OFF, "拖拽后填充段未越过轨道尾部");

    // ---- 滚动：滚轮注入到底（状态通道：偏移 = 内容高 - 视口高；像素通道：视口内容换块）----
    // ScrollEvent 的 delta_y 正方向为向上滚动（offset 减小），注入负值即向下滚到内容底。
    const float viewport_h = scb.size.height;
    AURORA_TEST_REQUIRE(static_cast<bool>(session.scroll(*scene.scroller, 0.0F, -1000.0F)));
    AURORA_TEST_CHECK_NEAR(scene.scroller->offset_y(), 240.0F - viewport_h, 0.5F);
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(2)));
    expect_pixel(read_checked(session), spec, scb.origin.x + (scb.size.width * 0.5F),
                 scb.origin.y + (scb.size.height * 0.5F), AURORA_BLOCK_2, "滚动到底后视口内容(块 2)");

    // ---- 文本输入（最后执行：聚焦不被后续指针交互清除）----
    AURORA_TEST_REQUIRE(static_cast<bool>(session.enter_text(*scene.input, "hi")));
    AURORA_TEST_CHECK_MSG(scene.input->value() == "hi", "文本输入落到目标控件 (value=\"" + scene.input->value() +
                                                            "\", 期望 \"hi\")");
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(2)));
    expect_pixel(read_checked(session), spec, inp.origin.x + inp.size.width - 8.0F,
                 inp.origin.y + (inp.size.height * 0.5F), AURORA_INPUT_FOCUSED, "文本输入后聚焦背景(右侧无文本区)");
}

// ============================================================
// 用例 2：焦点与键盘路由（窗口事件路由面 + FocusManager 纪律）
// ============================================================

AURORA_TEST_P(InteractionBackends, focus_and_keyboard_routing_via_window_dispatch) {
    const e2e::Backend backend = param();

    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = 320;
    spec.height = 80;
    spec.title = std::string{"etest_interaction_flow:focus_route:"} + e2e::backend_name(backend);
    spec.visibility = au::WindowVisibility::Hidden;

    auto session = open_checked(spec);
    FocusScene scene = build_focus_scene();

    // 对齐 demo_common.h run_demo 纪律：鼠标/键盘/文本派发必须携带 FocusManager，
    // 否则点击输入框时 request_focus() 静默 no-op，后续键盘事件到不了控件。
    au::FocusManager fm;
    fm.set_root(&scene.root.widget());
    auto route = [&root = scene.root, &fm](au::Event &e) -> void {
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
    session.surface().set_event_handler(route);

    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(scene.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump_until_settled()));

    // ---- 初始：无焦点 ----
    AURORA_TEST_CHECK_FALSE(scene.input->is_focused());

    // ---- 点击（窗口坐标合成事件）→ 焦点落到输入框 ----
    const au::Rect &inp = scene.input->paint_bounds();
    const au::Point center = center_of(inp);
    au::MouseEvent press;
    press.action = au::MouseAction::Press;
    press.button = au::MouseButton::Left;
    press.position = center;
    route(press);
    au::MouseEvent release = press;
    release.action = au::MouseAction::Release;
    route(release);
    AURORA_TEST_CHECK_TRUE(scene.input->is_focused());
    AURORA_TEST_CHECK_TRUE(fm.focused() == scene.input);

    // 像素通道：聚焦背景生效（采样输入框右侧无文本区）。
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump(2)));
    expect_pixel(read_checked(session), spec, inp.origin.x + inp.size.width - 8.0F,
                 inp.origin.y + (inp.size.height * 0.5F), AURORA_INPUT_FOCUSED, "点击聚焦后背景切换");

    // ---- 键盘路由：Backspace 经焦点管理器到达输入框（"ab" → "a"）----
    au::KeyEvent back;
    back.key = static_cast<int>(au::KeyCode::Backspace);
    back.action = au::KeyAction::Down;
    route(back);
    AURORA_TEST_CHECK_MSG(scene.input->value() == "a", "退格键经焦点路由到达目标控件 (value=\"" +
                                                            scene.input->value() + "\", 期望 \"a\")");

    // ---- 文本路由：TextInputEvent 追加到焦点控件（"a" → "acd"）----
    au::TextInputEvent ti;
    ti.text = "cd";
    route(ti);
    AURORA_TEST_CHECK_MSG(scene.input->value() == "acd", "文本片段追加到焦点控件 (value=\"" + scene.input->value() +
                                                             "\", 期望 \"acd\")");
}

// ============================================================
// 用例 3：交互触发的动画经 pump_until_settled 收敛
// ============================================================

AURORA_TEST_P(InteractionBackends, animation_converges_after_interaction) {
    const e2e::Backend backend = param();

    e2e::WindowSpec spec;
    spec.backend = backend;
    spec.width = 320;
    spec.height = 176;
    spec.title = std::string{"etest_interaction_flow:glide:"} + e2e::backend_name(backend);
    spec.visibility = au::WindowVisibility::Hidden;

    auto session = open_checked(spec);
    GlideScene scene = build_glide_scene();
    AURORA_TEST_REQUIRE(static_cast<bool>(session.present(scene.root)));
    AURORA_TEST_REQUIRE(static_cast<bool>(session.pump_until_settled()));
    AURORA_TEST_CHECK_FALSE(scene.scroller->is_gliding());

    const au::Rect &scb = scene.scroller->paint_bounds();
    const float viewport_h = scb.size.height;
    const float target_offset = 240.0F - viewport_h;

    // ---- 点击触发收位滑动：注入后立即处于滑动中（交互确实启动了动画）----
    AURORA_TEST_REQUIRE(static_cast<bool>(session.tap(*scene.jump)));
    AURORA_TEST_CHECK_TRUE(scene.scroller->is_gliding());

    // ---- 收敛：滑动结束后偏移到位、像素达到稳定态 ----
    // 收位滑动按真实时钟推进（dt 取 steady_clock 实测间隔），而泵帧无节流——同一帧预算对应的
    // 墙钟时长随机器性能浮动（轻量后端单帧 < 0.15ms 时 600 帧墙钟不足 0.15s 滑动时长），
    // 故分轮推进直至滑动结束，总帧数仍有上界。
    auto settled = session.pump_until_settled(600);
    for (int round = 0; round < 10 && !settled; ++round) {
        settled = session.pump_until_settled(600);
    }
    AURORA_TEST_REQUIRE_MSG(static_cast<bool>(settled), "收位滑动未在帧预算内收敛");
    AURORA_TEST_CHECK_FALSE(scene.scroller->is_gliding());
    AURORA_TEST_CHECK_NEAR(scene.scroller->offset_y(), target_offset, 0.5F);
    expect_pixel(read_checked(session), spec, scb.origin.x + (scb.size.width * 0.5F),
                 scb.origin.y + (scb.size.height * 0.5F), AURORA_BLOCK_2, "动画收敛后视口内容(块 2)");
}

AURORA_INSTANTIATE_TEST_SUITE_P_GEN(flow_backends, InteractionBackends, flow_backend_values(), flow_backend_name);

}  // namespace aurora::test_cases::etest_interaction_flow
