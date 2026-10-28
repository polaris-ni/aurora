/// 测试类型: unit
/// 目标单元: include/aurora/app/application.h
/// 测试说明: 覆盖 Application 无头构造的默认状态（无 Window、严格模式、窗口可见性/几何态
/// 响应式初值）、子系统句柄（scene/focus/scheduler/shortcuts）、快捷键优先派发（消费/落空/禁用）、
/// 空场景同步派发安全性与 App 流式构建器链式配置（不进入真实帧循环）、
/// 帧循环对系统拖放异步结果的排空接线（终结回调 / 在途读取只在 step_frame 内兑现）

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/mime_types.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_application {

using aurora::testing::require_value;

namespace {

auto make_scene() -> Scene { return Scene{Node{std::make_shared<Text>("hi")}}; }

/// 固定尺寸探针控件（端到端用例）：几何由期望尺寸直接决定，不依赖字体度量；
/// `modifier` 可挂 `Modifier::cursor(...)` 以驱动派发器的悬停光标解析。
class CursorProbe final : public Widget {
  public:
    explicit CursorProbe(Size want) : want_(want) {}

    auto type_name() const -> const char * override { return "CursorProbe"; }

    auto on_layout(const Constraints &c, [[maybe_unused]] const BuildContext &ctx) -> Size override {
        size_ = c.constrain(want_);
        return size_;
    }

    auto on_paint([[maybe_unused]] Painter &p, [[maybe_unused]] const Rect &bounds,
                  [[maybe_unused]] const BuildContext &ctx) -> void override {}

  private:
    Size want_;
};

}  // namespace

AURORA_TEST_CASE(headless_application_defaults) {
    Application app{make_scene(), 320, 240};

    // 无头构造：不持有后端 Window。
    AURORA_TEST_CHECK_NULL(app.window());
    // 严格模式默认 Off；窗口可见性/几何态响应式初值为 Visible / Normal。
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::Off);
    AURORA_TEST_CHECK_EQ(app.window_state().get(), WindowState::Visible);
    AURORA_TEST_CHECK_EQ(app.window_mode().get(), WindowMode::Normal);

    // 命令式窗口状态回调可注册（无 Window 时不会触发）。
    bool state_cb_called = false;
    bool mode_cb_called = false;
    app.set_on_window_state([&state_cb_called](WindowState) -> void { state_cb_called = true; });
    app.set_on_window_mode([&mode_cb_called](WindowMode) -> void { mode_cb_called = true; });
    AURORA_TEST_CHECK_FALSE(state_cb_called);
    AURORA_TEST_CHECK_FALSE(mode_cb_called);

    // set_on_frame / set_overlay 仅注册/转发，无头路径不触发。
    app.set_on_frame([]() -> void {});
    app.set_overlay(nullptr);
    AURORA_TEST_CHECK_NULL(app.window());
}

AURORA_TEST_CASE(application_exposes_subsystem_handles) {
    Application app{make_scene(), 100, 80};

    // 场景根为构造时注入的 widget。
    AURORA_TEST_CHECK_EQ(std::string{app.scene().root().type_name()}, "Text");
    // 空场景下焦点管理器无焦点。
    AURORA_TEST_CHECK_NULL(app.focus().focused());
    // 快捷键注册表初始为空。
    AURORA_TEST_CHECK_EQ(app.shortcuts().count(), 0U);
    // 子系统句柄可取引用且稳定。
    AURORA_TEST_CHECK_EQ(&app.scheduler(), &app.scheduler());
    AURORA_TEST_CHECK_EQ(&app.shortcuts(), &app.shortcuts());
}

AURORA_TEST_CASE(strict_mode_roundtrip) {
    Application app{make_scene(), 100, 80};
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::Off);

    app.set_strict_mode(StrictMode::On);
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::On);

    app.set_strict_mode(StrictMode::Off);
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::Off);
}

AURORA_TEST_CASE(dispatch_key_consumed_by_registered_shortcut) {
    Application app{make_scene(), 320, 240};
    int fired = 0;
    app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O}, [&fired]() -> void { ++fired; });

    KeyEvent e;
    e.key = static_cast<int>(KeyCode::O);
    e.action = KeyAction::Down;
    e.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_TRUE(app.dispatch_key(e));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(dispatch_key_falls_through_when_unmatched) {
    Application app{make_scene(), 320, 240};
    int fired = 0;
    app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O}, [&fired]() -> void { ++fired; });

    // 无修饰键的同键不匹配 → 快捷键不消费；空场景无人处理 → 返回 false。
    KeyEvent plain;
    plain.key = static_cast<int>(KeyCode::O);
    plain.action = KeyAction::Down;
    plain.modifiers = ModifierKey::None;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(plain));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 匹配组合但为抬起事件 → 不触发快捷键。
    KeyEvent up;
    up.key = static_cast<int>(KeyCode::O);
    up.action = KeyAction::Up;
    up.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(up));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 注册表外的键同样落空。
    KeyEvent other;
    other.key = static_cast<int>(KeyCode::S);
    other.action = KeyAction::Down;
    other.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(other));
    AURORA_TEST_CHECK_EQ(fired, 0);
}

AURORA_TEST_CASE(disabled_shortcut_not_consumed) {
    Application app{make_scene(), 320, 240};
    int fired = 0;
    const int id = app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::S}, [&fired]() -> void { ++fired; });

    app.shortcuts().set_enabled(id, false);
    KeyEvent e;
    e.key = static_cast<int>(KeyCode::S);
    e.action = KeyAction::Down;
    e.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(e));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 重新启用后恢复消费。
    app.shortcuts().set_enabled(id, true);
    AURORA_TEST_CHECK_TRUE(app.dispatch_key(e));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(plain_scene_dispatches_are_safe_noops) {
    Application app{make_scene(), 320, 240};

    // 空场景（纯 Text 叶根）上的同步派发不命中任何控件，应安全无害。
    AURORA_TEST_CHECK_NO_THROW(app.dispatch_click(5.0F, 5.0F));
    AURORA_TEST_CHECK_NO_THROW(app.dispatch_pointer(6.0F, 6.0F, MouseAction::Move));
    AURORA_TEST_CHECK_NO_THROW(app.tick());
    // TEST_TEMP_EXEMPT: 模拟拖入的假路径字符串，非真实临时目录。
    std::unordered_map<std::string, std::vector<std::uint8_t>> entries;
    const std::string uri_text = "file:///C:/docs/a.txt\r\n";
    entries.emplace(std::string(AURORA_MIME_URI_LIST), std::vector<std::uint8_t>(uri_text.begin(), uri_text.end()));
    SystemDropEvent drop;
    drop.phase = SystemDropPhase::Drop;
    drop.position = Point{.x = 1.0F, .y = 2.0F};
    drop.formats.emplace_back(AURORA_MIME_URI_LIST);
    drop.payload = DragPayload::from_bytes(std::move(entries));
    AURORA_TEST_CHECK_NO_THROW(app.dispatch_system_drop(drop));

    // 文本输入无人处理 → 返回 false。
    TextInputEvent text;
    text.text = "x";
    AURORA_TEST_CHECK_FALSE(app.dispatch_text(text));
}

AURORA_TEST_CASE(frame_loop_drains_system_drag_session_end) {
#ifdef AURORA_BACKEND_HEADLESS
    // 帧循环排空接线：系统拖出的异步结果（终结回调 / 在途读取）只在宿主帧内兑现——
    // 登记之后、帧推进之前零投递；帧内排空后当帧即投递且只投一次（宿主不排空 ⇒ 收不到，
    // 不谎报；与通知激活 / 热键命中同一时点语义）。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    WindowOptions o;
    o.title = "drag-drain";
    o.size = Size{.width = 120.0F, .height = 80.0F};
    o.max_frames = 2;  // 两帧：第一帧兑现终态、第二帧兑现读取
    o.power_saving = false;  // 无 OS 等待通道：忙轮询，帧数确定
    HeadlessOptions h;
    static_cast<WindowOptions &>(h) = o;
    auto window = create_window(h);
    AURORA_TEST_REQUIRE_TRUE(static_cast<bool>(window));

    int end_calls = 0;
    int frames_seen = 0;
    SystemDragOutcome got_outcome = SystemDragOutcome::Failed;
    DragAction got_action = DragAction::None;

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), std::vector<std::uint8_t>{'a', 'b'});
    const auto payload = DragPayload::from_bytes(std::move(data));
    int read_calls = 0;

    Application app{Scene{Node{std::make_shared<Text>("idle")}}, std::move(window.value()), o};
    // on_frame 在帧内排空（system_drag::pump_reads）之后调用：第一帧里终态应已兑现
    // （据此断言「帧内兑现」），同帧登记在途读取（据此断言「登记后不立即兑现」），
    // 交由第二帧排空。
    app.set_on_frame([&]() -> void {
        ++frames_seen;
        if (frames_seen != 1) {
            return;
        }
        AURORA_TEST_CHECK_EQ(end_calls, 1);
        payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN),
                            [&read_calls](const Result<std::vector<std::uint8_t>> &r) -> void {
                                ++read_calls;
                                AURORA_TEST_CHECK(r.ok());
                            });
        AURORA_TEST_CHECK_EQ(read_calls, 0);
    });

    DragOffer offer;
    offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> {
        return std::vector<std::uint8_t>{'x'};
    };
    AURORA_TEST_REQUIRE(system_drag::begin(offer, [&](SystemDragOutcome outcome, DragAction action) -> void {
                            ++end_calls;
                            got_outcome = outcome;
                            got_action = action;
                        }).ok());
    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Copy);
    AURORA_TEST_CHECK_EQ(end_calls, 0);  // 帧未推进：零投递

    app.run();  // 推进两帧

    AURORA_TEST_CHECK_EQ(frames_seen, 2);
    AURORA_TEST_CHECK_EQ(end_calls, 1);  // 只投一次
    AURORA_TEST_CHECK_EQ(got_outcome, SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_EQ(got_action, DragAction::Copy);
    AURORA_TEST_CHECK_EQ(read_calls, 1);  // 第二帧排空兑现
#else
    AURORA_TEST_SKIP("frame-loop drain probe needs the Headless backend");
#endif
}

AURORA_TEST_CASE(app_builder_chains_fluently) {
    // 流式构建器：各 setter 返回同一实例引用（可链式）。
    // 注：命名空间里自由函数 App() 会隐藏类名 App，类型语境用 auto 绕开。
    auto builder = aurora::App::make();
    auto &chained =
        builder.title("utest").size(320, 240).frames(1).on_frame([]() -> void {}).strict_mode(StrictMode::Off);
    AURORA_TEST_CHECK_EQ(&chained, &builder);

    // 文档形态的自由函数工厂 `au::App()` 可用。
    auto factory = aurora::App();
    AURORA_TEST_CHECK_NO_THROW(factory.title("factory"));

    // 显式 Node 构造与 view() 替换（仅配置，不进入帧循环）；elaborated 类型名绕开函数隐藏。
    class aurora::App with_view{Node{std::make_shared<Text>("root")}};
    AURORA_TEST_CHECK_NO_THROW(with_view.view(Node{std::make_shared<Text>("replaced")}));
}

AURORA_TEST_CASE(application_wires_hover_cursor_to_surface) {
#ifdef AURORA_BACKEND_HEADLESS
    // 光标形状完成判据的端到端验收：派发器解析出的悬停光标经 Application 接线落到 Surface。
    // 链路：app.dispatch(Move) → EventDispatcher::dispatch_mouse → update_hover → resolve_cursor
    //       → EventDispatcher::cursor_handler_ → Application::wire_cursor → Surface::set_cursor。
    // 用 HeadlessSurface 的记录 seam 断言（无系统光标亦可确定性验证）。
    auto root = std::make_shared<CursorProbe>(Size{.width = 200.0F, .height = 80.0F});
    root->layout(Constraints{}, BuildContext{});
    Scene scene{Node{root}};

    auto surface = std::make_unique<HeadlessSurface>("", Size{.width = 200.0F, .height = 80.0F});
    HeadlessSurface *probe = surface.get();
    Application app{std::move(scene), std::move(surface),
                    WindowOptions{.size = Size{.width = 200.0F, .height = 80.0F}}};
    AURORA_TEST_REQUIRE_TRUE(app.window() != nullptr);  // 无 Window 时 wire_cursor 不落到 Surface

    auto move_to = [&](float x, float y) -> void { app.dispatch_pointer(x, y, MouseAction::Move); };

    // 1) 无任何光标声明 → 首次解析为 Arrow 并下发（cursor_emitted_ 初值 false）。
    move_to(100.0F, 40.0F);
    AURORA_TEST_REQUIRE_TRUE(probe->last_cursor().has_value());
    AURORA_TEST_CHECK_EQ(require_value(probe->last_cursor()), CursorShape::Arrow);

    // 2) 同形状重复移动 → 派发器按 current_cursor_ 去重，不再下发。
    move_to(110.0F, 40.0F);
    AURORA_TEST_CHECK_EQ(probe->cursor_log().size(), std::size_t{1});

    // 3) 悬停链声明改为 Crosshair → 形状变化 → 再次下发（证明修饰链经 Application 生效）。
    root->set_modifier(Modifier{}.cursor(CursorShape::Crosshair));
    move_to(120.0F, 40.0F);
    AURORA_TEST_REQUIRE_EQ(probe->cursor_log().size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(require_value(probe->last_cursor()), CursorShape::Crosshair);

    // 4) 移出根矩形（链空）→ 回落 Arrow，再次变化即下发。
    move_to(500.0F, 500.0F);
    AURORA_TEST_REQUIRE_EQ(probe->cursor_log().size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(require_value(probe->last_cursor()), CursorShape::Arrow);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS is not enabled; HeadlessSurface is not compiled");
#endif
}

// ---- 活动窗口查询 `Application::active_window()` ----
// 语义：最近一次用户输入派发的目的窗口，**不等价于 OS 前台窗**（无头 / 失焦两档下可分叉）。

/// @brief 产出一枚无头辅助窗口（多实例安全，与 utest_multi_window 同款底座）。
auto make_aux_window(std::string title, int frames = -1) -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    static_cast<WindowOptions &>(opts) = WindowOptions{};
    opts.title = std::move(title);
    opts.size = Size{.width = 200.0F, .height = 150.0F};
    opts.max_frames = frames;
    auto res = create_window(opts);
    return res ? std::move(res.value()) : nullptr;
}

/// @brief 构造一枚按下态键盘事件（属记账范围的「用户输入意图类」事件）。
auto make_key_down() -> KeyEvent {
    KeyEvent e;
    e.key = static_cast<int>(KeyCode::O);
    e.action = KeyAction::Down;
    return e;
}

AURORA_TEST_CASE(active_window_falls_back_to_main_without_records) {
    // 判据 1：两窗构造后尚无记账 → 回落主窗，且不是无效号。
    Application app{make_scene(), 320, 240};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);
    AURORA_TEST_CHECK_EQ(app.active_window(), app.main_window());
    AURORA_TEST_CHECK_NE(app.active_window(), AURORA_INVALID_WINDOW_ID);
    AURORA_TEST_CHECK_NE(id_b, app.main_window());
}

AURORA_TEST_CASE(active_window_records_key_dispatch_source) {
    // 判据 2：宿主级键盘派发经 key_pre 闭包记账（早于快捷键匹配）→ 回该窗；
    //        再经应用级指针 Press → 回主窗。
    Application app{make_scene(), 320, 240};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    auto *host_b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(host_b);

    KeyEvent down = make_key_down();
    host_b->dispatch(down);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_b);

    app.dispatch_pointer(10.0F, 10.0F, MouseAction::Press);
    AURORA_TEST_CHECK_EQ(app.active_window(), app.main_window());
}

AURORA_TEST_CASE(active_window_programmatic_entry_matrix) {
    // 判据 3：应用级 6 条入口一律回 main；宿主级 5 条直调不改变读数（无挂点）。
    Application app{make_scene(), 320, 240};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    auto *host_b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(host_b);
    const WindowId id_main = app.main_window();

    // 应用级六条：全部指向 main_host_，按记账范围过滤后一律把读数搬到 main。
    const std::vector<std::function<void()>> app_entries = {
        [&]() -> void { app.dispatch_click(5.0F, 5.0F); },
        [&]() -> void { app.dispatch_pointer(5.0F, 5.0F, MouseAction::Press); },
        [&]() -> void {
            KeyEvent e = make_key_down();
            (void)app.dispatch_key(e);
        },
        [&]() -> void {
            TextInputEvent t;
            t.text = "x";
            (void)app.dispatch_text(t);
        },
        [&]() -> void {
            TouchEvent t;
            TouchPoint p;
            p.is_active = true;
            t.points.push_back(p);
            app.dispatch_touch(t);
        },
        [&]() -> void {
            SystemDropEvent d;
            app.dispatch_system_drop(d);
        },
    };
    // 宿主级五条：宿主无 Application 反指，够不到记账点 ⇒ 直调不改变读数。
    const std::vector<std::function<void()>> host_entries = {
        [&]() -> void { host_b->dispatch_pointer(5.0F, 5.0F, MouseAction::Press); },
        [&]() -> void {
            KeyEvent e = make_key_down();
            (void)host_b->dispatch_key(e);
        },
        [&]() -> void {
            TextInputEvent t;
            t.text = "x";
            (void)host_b->dispatch_text(t);
        },
        [&]() -> void {
            TouchEvent t;
            TouchPoint p;
            p.is_active = true;
            t.points.push_back(p);
            host_b->dispatch_touch(t);
        },
        [&]() -> void {
            SystemDropEvent d;
            host_b->dispatch_system_drop(d);
        },
    };

    // 逐条：先把读数钉在 B（宿主级键盘派发记账），再跑应用级入口 → 应搬回 main。
    for (const auto &entry : app_entries) {
        KeyEvent seed = make_key_down();
        host_b->dispatch(seed);
        AURORA_TEST_REQUIRE_EQ(app.active_window(), id_b);
        entry();
        AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
    }
    // 逐条：先把读数钉在 main，再跑宿主级直调 → 读数不变。
    for (const auto &entry : host_entries) {
        app.dispatch_pointer(5.0F, 5.0F, MouseAction::Press);
        AURORA_TEST_REQUIRE_EQ(app.active_window(), id_main);
        entry();
        AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
    }
}

AURORA_TEST_CASE(active_window_visible_inside_shortcut_action) {
    // 判据 4：记账早于匹配 —— 快捷键闭包内读到的是本次派发来源，两次派发得两个不同号。
    Application app{make_scene(), 320, 240};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    auto *host_b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(host_b);

    std::vector<WindowId> seen;
    app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O},
                        [&app, &seen]() -> void { seen.push_back(app.active_window()); });
    KeyEvent e;
    e.key = static_cast<int>(KeyCode::O);
    e.action = KeyAction::Down;
    e.modifiers = ModifierKey::Control;

    (void)app.dispatch_key(e);  // 应用级 → 来源 main
    host_b->dispatch(e);  // 宿主级 → 来源 B
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(seen[0], app.main_window());
    AURORA_TEST_CHECK_EQ(seen[1], id_b);
    AURORA_TEST_CHECK_NE(seen[0], seen[1]);
}

AURORA_TEST_CASE(active_window_ignores_pointer_move) {
    // 判据 5：指针 Move 不记账（悬停不得翻转活动窗）；Press 记账；宿主级 MouseEvent 直调不翻转。
    Application app{make_scene(), 320, 240};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    auto *host_b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(host_b);
    const WindowId id_main = app.main_window();

    KeyEvent seed = make_key_down();
    host_b->dispatch(seed);
    AURORA_TEST_REQUIRE_EQ(app.active_window(), id_b);

    app.dispatch_pointer(11.0F, 12.0F, MouseAction::Move);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_b);  // Move 不记账

    app.dispatch_pointer(11.0F, 12.0F, MouseAction::Press);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);  // Press 记账

    MouseEvent me;
    me.action = MouseAction::Press;
    host_b->dispatch(me);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);  // 宿主级无挂点，不翻转
}

AURORA_TEST_CASE(active_window_cleared_after_reap) {
    // 判据 6（回收清号）与判据 8（回调窗口期仍有存活宿主 → 不为无效号）。
    WindowOptions opts;
    opts.max_frames = 2;
    opts.power_saving = false;
    auto wa = make_aux_window("main");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene(), std::move(wa), opts};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    auto *host_b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(host_b);
    const WindowId id_main = app.main_window();

    WindowId in_callback = AURORA_INVALID_WINDOW_ID;
    app.set_on_window_closed([&](WindowId) -> void { in_callback = app.active_window(); });

    KeyEvent seed = make_key_down();
    host_b->dispatch(seed);
    AURORA_TEST_REQUIRE_EQ(app.active_window(), id_b);

    app.close_window(id_b);
    app.run();  // 帧末 reap_closed 回收（step_frame 为私有，故走 run 的帧预算）

    AURORA_TEST_CHECK_NE(app.active_window(), id_b);  // 被记账窗 reap 后不再回它
    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
    AURORA_TEST_CHECK_NE(in_callback, AURORA_INVALID_WINDOW_ID);
}

AURORA_TEST_CASE(active_window_keeps_last_surviving_host) {
    // 判据 7：被记账窗正是「全关保留」规则留下的主宿主 → 仍回该号（有效存活号）。
    WindowOptions opts;
    opts.max_frames = 2;
    opts.power_saving = false;
    auto wa = make_aux_window("main");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene(), std::move(wa), opts};
    auto wb = make_aux_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene());
    const WindowId id_main = app.main_window();

    app.dispatch_pointer(5.0F, 5.0F, MouseAction::Press);
    AURORA_TEST_REQUIRE_EQ(app.active_window(), id_main);

    // 两窗全关：「保留最后一个」规则留下主宿主 → 仍回该号。
    std::vector<WindowId> all;
    for (auto *h : app.windows()) {
        all.push_back(h->id());
    }
    for (const WindowId id : all) {
        app.close_window(id);
    }
    app.run();

    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
    AURORA_TEST_CHECK_NE(app.active_window(), id_b);
}

AURORA_TEST_CASE(active_window_equals_main_in_single_window) {
    // 判据 9（回归保护）：单窗下任意派发后恒等于 main_window()。
    Application app{make_scene(), 320, 240};
    const WindowId id_main = app.main_window();
    app.dispatch_pointer(5.0F, 5.0F, MouseAction::Press);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
    KeyEvent e = make_key_down();
    (void)app.dispatch_key(e);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
    app.dispatch_pointer(6.0F, 6.0F, MouseAction::Move);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_main);
}

}  // namespace aurora::test_cases::utest_application
