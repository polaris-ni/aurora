// 目标源单元：window/win32_surface.h + window/win32_window.h（Win32/GDI 后端，仅 AURORA_PLATFORM_WINDOWS 编译运行）
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <memory>

#include "aurora/core/platform.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/render/painter.h"
#include "aurora/state/reactive.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora/window/win32_surface.h"
#include "aurora/window/window.h"

#include "test_harness.h"
#ifdef AURORA_BACKEND_WIN32
#include <windows.h>
#endif // AURORA_BACKEND_WIN32

namespace aurora::tests::sec_win32_black_screen {

#ifdef AURORA_BACKEND_WIN32
namespace {

/// @brief 最小根控件：present_root 可对其 layout+paint，用于驱动 Win32 同步重渲染路径。
class RootStub : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "RootStub"; }
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor { return WidgetDescriptor{ .name = "RootStub" }; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = 100.0f, .height = 30.0f });
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, Color{ 200, 200, 200, 255 });
    }
};

} // namespace

static void run() {
    const auto surf = std::make_shared<Win32Surface>(640, 360, "black");

    // 契约 1：类背景擦除刷非空 → 最大化/缩放时系统擦除区域为浅色，不再纯黑。
    AURORA_TEST_CHECK(Win32Surface::background_brush() != nullptr);

    // 契约 2：窗口句柄有效，且 WM_PAINT 被处理（立即 present 当前缓冲）而不崩溃/死循环。
    auto *h = static_cast<HWND>(surf->hwnd());
    AURORA_TEST_CHECK(h != nullptr);
    // 直接同步发送 WM_PAINT，验证 wnd_proc 分支存在且能安全呈现。
    (void)SendMessageA(h, WM_PAINT, 0, 0);
    AURORA_TEST_CHECK(true); // 抵达此处说明 WM_PAINT 处理未崩溃

    // 契约 3：最大化时 WM_SIZE 同步触发一次重渲染（消除放大区域白闪）。
    // 构造带真实根控件的 Window（其构造会把 present-request 接为 present_root），
    // 先把缓冲以初始尺寸渲染一帧，再真正最大化窗口并泵消息，断言 present 次数增加且
    // 离屏缓冲已按新（更大）客户区尺寸重分配——证明 DWM 合成前缓冲已为新尺寸内容。
    {
        auto white_surf = std::make_unique<Win32Surface>(640, 360, "white");
        Win32Surface *ws = white_surf.get();
        auto win = Window{ std::move(white_surf) };
        const auto stub = std::make_shared<RootStub>();
        Node root = std::static_pointer_cast<Widget>(stub);
        const auto first = win.present_root(root); // 设置 m_cached_root 并渲染首帧
        AURORA_TEST_CHECK(first.ok());
        const int before = ws->present_count();
        const int before_w = ws->painter().width();
        // 真正最大化：触发 OS 发送 WM_SIZE(SIZE_MAXIMIZED)，wnd_proc 当下调用 present-request。
        ShowWindow(static_cast<HWND>(ws->hwnd()), SW_MAXIMIZE);
        win.pump_events();
        const int after = ws->present_count();
        AURORA_TEST_CHECK(after > before);                   // WM_SIZE 同步触发了重渲染
        AURORA_TEST_CHECK(ws->painter().width() > before_w); // 离屏缓冲已按新尺寸重分配（无白闪）
    }
}

#else

static void run() { AURORA_TEST_PRINTF("skip: AURORA_BACKEND_WIN32 not compiled into this build"); }

#endif
} // namespace aurora::tests::sec_win32_black_screen

namespace aurora::tests::sec_win32_button_click {
#ifdef AURORA_BACKEND_WIN32

namespace {
[[maybe_unused]] auto gap(float h) -> Node {
    Text t{ " " };
    t.modifier.set(Modifier{}.height(h));
    return Node{ std::move(t) };
}
class Card : public Column {
  public:
    explicit Card(Node child) : Column{ ColumnProps{ .children = { std::move(child) } } } {
        this->modifier.set(Modifier{}.padding(14.0f).background(Color{ 255, 255, 255 }));
    }
};
} // namespace

static void run() {
    BuildContext ctx;

    auto counter = std::make_shared<State<int>>(0);
    auto count_text = std::make_shared<State<LocalizedString>>(LocalizedString{ "count = 0" });
    const auto apply = [counter, count_text](int next) -> void {
        counter->set(next);
        count_text->set(LocalizedString{ "count = " + std::to_string(next) });
    };

    Button b_plus{ ButtonProps{ .label = LocalizedString{ "+1" } } };
    b_plus.on_click = [counter, apply]() -> void { apply(counter->get() + 1); };

    Node root = Column{ Text{ LocalizedString{ "点击 +1 按钮改变计数" } },
                        Text{ TextProps{ .content = Reactive{ count_text } } }, Row{ Node{ std::move(b_plus) } } };
    Node card = Card{ std::move(root) };

    WindowOptions opts;
    opts.size = Size{ .width = 520.0f, .height = 380.0f };
    opts.title = "Win32ClickE2E";

    auto win_res = create_window(Win32Options{ opts });
    if (!win_res) {
        AURORA_LOG_ERROR("test", "[win32_button_click_test] window creation failed: ", win_res.error().message);
        return; // 非致命：无显示环境跳过
    }
    auto win = std::move(win_res.value());

    int events_on_button = 0;
    win->surface().set_event_handler([&](Event &e) -> void {
        auto &wd = card.widget();
        if (auto *me = dynamic_cast<MouseEvent *>(&e)) {
            const auto chain = wd.hit_test_chain(me->position, Rect{ .origin = Point{}, .size = wd.size() }, ctx);
            for (const auto &hn : chain) {
                Widget const *w = hn.get();
                if (w && std::string(w->type_name()) == "Button") {
                    events_on_button++;
                    break;
                }
            }
            EventDispatcher::dispatch(wd, *me);
        }
    });

    // 渲染一帧：mount + layout + paint，设置各 widget 的 Node 几何（与 run_demo 一致）
    FocusManager fm;
    fm.set_root(&card.widget());
    static_cast<void>(win->present_root(card));

    const float scale = win->surface().scale_factor();
    const Size logical = win->surface().size();
    AURORA_LOG_INFO("test", "[win32_button_click_test] logical size = ", logical.width, " x ", logical.height,
                    ", scale = ", scale);

    HWND hwnd = FindWindowA("AuroraWin32Surface", opts.title.c_str());
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("test", "[win32_button_click_test] window handle not found");
        return; /* 无显示环境：优雅跳过，不计失败 */
    }

    // 找落在 +1 按钮中心的一个 dp 坐标
    Point target{ .x = 0.0f, .y = 0.0f };
    bool found = false;
    for (int ix = 0; static_cast<float>(ix) < logical.width; ix += 2) {
        const auto x = static_cast<float>(ix);
        for (int iy = 0; static_cast<float>(iy) < logical.height; iy += 2) {
            const auto y = static_cast<float>(iy);
            auto chain = card.widget().hit_test_chain(Point{ .x = x, .y = y }, card.bounds(), ctx);
            for (auto &hn : chain) {
                Widget *w = hn.get();
                if ((w != nullptr) && std::string(w->type_name()) == "Button") {
                    target = Point{ .x = x, .y = y };
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        AURORA_LOG_ERROR("test", "[win32_button_click_test] +1 button hit point not found");
        AURORA_TEST_REQUIRE(false);
    }
    AURORA_LOG_INFO("test", "[win32_button_click_test] +1 button dp center = (", target.x, ",", target.y, ")");

    // 完全还原 run_demo 主循环：pump_events → present_root（begin_frame 由 present_root 内部按需调用，
    // 外层手调会破坏部分脏区帧「裁剪外沿用上帧像素」不变量），跑若干帧让 bounds 就绪
    auto loop_once = [&]() -> void {
        win->pump_events();
        (void)win->present_root(card);
    };
    for (int f = 0; f < 3; ++f) {
        loop_once();
    }

    // 通过真实 Win32 消息链路投递一次点击：物理像素 = dp × scale
    const int px = static_cast<int>(std::lround(target.x * scale));
    const int py = static_cast<int>(std::lround(target.y * scale));
    PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px, py));
    PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(px, py));

    // 再跑两帧：让 wnd_proc 处理消息（触发 on_click）并 present 重绘
    for (int f = 0; f < 2; ++f) {
        loop_once();
    }

    AURORA_LOG_INFO("test", "[win32_button_click_test] events_on_button = ", events_on_button);
    AURORA_LOG_INFO("test", "[win32_button_click_test] counter after ONE real Win32 click = ", counter->get());

    // 关键检查：真实窗口单击 +1 必须触发 on_click（counter == 1）。
    // 注意：Release 构建下 assert 为空操作，这里用显式失败返回确保测试有效。
    if (counter->get() != 1) {
        AURORA_LOG_ERROR("test",
                         "[win32_button_click_test] FAIL: real window click on +1 did not trigger on_click (counter=",
                         counter->get(), ")");
        AURORA_TEST_REQUIRE(false);
    }
    (void)events_on_button;
    AURORA_LOG_INFO(
        "test", "WIN32 BUTTON CLICK E2E: OK (real window click triggered on_click, counter = ", counter->get(), ")");
}

#else

void run() { AURORA_TEST_PRINTF("skip: AURORA_BACKEND_WIN32 not compiled into this build"); }

#endif
} // namespace aurora::tests::sec_win32_button_click

namespace aurora::tests::sec_count_display_col {
#ifdef AURORA_BACKEND_WIN32
namespace au = aurora;

namespace {
auto gap(float h) -> Node {
    Text t{ " " };
    t.modifier.set(Modifier{}.height(h));
    return Node{ std::move(t) };
}
} // namespace

static void run() {
    BuildContext ctx;

    auto counter = std::make_shared<State<int>>(0);
    auto count_text = std::make_shared<State<LocalizedString>>(LocalizedString{ "count = 0" });
    const auto apply = [counter, count_text](int next) -> void {
        counter->set(next);
        count_text->set(LocalizedString{ "count = " + std::to_string(next) });
    };

    Button b_plus{ ButtonProps{ .label = LocalizedString{ "+1" } } };
    b_plus.on_click = [counter, apply]() -> void { apply(counter->get() + 1); };

    // 原始布局：计数 Text 在 Column 中（与按钮 Row 并列）
    Node root = Column{
        gap(12),
        Text{ TextProps{ .content = Reactive{ count_text } } },
        Row{ Node{ std::move(b_plus) } },
    };

    WindowOptions opts;
    opts.size = Size{ .width = 520.0f, .height = 380.0f };
    opts.title = "CountDisplayCol";

    auto win_res = create_window(Win32Options{ opts });
    if (!win_res) {
        AURORA_LOG_ERROR("test", "[count_display_col_test] window creation failed: ", win_res.error().message);
        return; /* 无显示环境：优雅跳过，不计失败 */
    }
    auto win = std::move(win_res.value());

    win->surface().set_event_handler([&](Event &e) -> void {
        auto &wd = root.widget();
        if (auto *me = dynamic_cast<MouseEvent *>(&e)) {
            EventDispatcher::dispatch(wd, *me);
        }
    });

    FocusManager fm;
    fm.set_root(&root.widget());
    static_cast<void>(win->present_root(root));

    const float scale = win->surface().scale_factor();
    const Size logical = win->surface().size();

    HWND hwnd = FindWindowA("AuroraWin32Surface", opts.title.c_str());
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("test", "[count_display_col_test] window handle not found");
        return; /* 无显示环境：优雅跳过，不计失败 */
    }

    // 找到 +1 按钮在 dp 坐标下的命中中心点
    Point target{ .x = 0.0f, .y = 0.0f };
    bool found = false;
    for (int ix = 0; static_cast<float>(ix) < logical.width; ix += 2) {
        const auto x = static_cast<float>(ix);
        for (int iy = 0; static_cast<float>(iy) < logical.height; iy += 2) {
            const auto y = static_cast<float>(iy);
            auto chain = root.widget().hit_test_chain(Point{ .x = x, .y = y }, root.bounds(), ctx);
            for (auto &hn : chain) {
                Widget *w = hn.get();
                if ((w != nullptr) && std::string(w->type_name()) == "Button") {
                    target = Point{ .x = x, .y = y };
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        AURORA_LOG_ERROR("test", "[count_display_col_test] +1 button hit point not found");
        AURORA_TEST_REQUIRE(false);
    }

    auto loop_once = [&]() -> void {
        win->pump_events();
        (void)win->present_root(root); // begin_frame 由 present_root 内部按需调用，外层不得手调
    };
    for (int f = 0; f < 3; ++f) {
        loop_once();
    }

    // 点击前：显示文本应为 "count = 0"
    std::string disp_before;
    root.widget().for_each_child([&](const Widget &w) -> void {
        if (const auto *t = dynamic_cast<const Text *>(&w)) {
            if (t->display_text().starts_with("count = ")) {
                disp_before = t->display_text();
            }
        }
    });

    AURORA_LOG_INFO("test", "[count_display_col_test] display text before click = \"", disp_before, "\"");

    // 真实 Win32 点击 +1
    const int px = static_cast<int>(std::lround(target.x * scale));
    const int py = static_cast<int>(std::lround(target.y * scale));
    PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px, py));
    PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(px, py));

    for (int f = 0; f < 2; ++f) {
        loop_once();
    }

    // 点击后：底层 State 与显示文本都应更新
    if (counter->get() != 1) {
        AURORA_LOG_ERROR(
            "test", "[count_display_col_test] FAIL: click did not trigger on_click (counter=", counter->get(), ")");
        AURORA_TEST_REQUIRE(false);
    }

    std::string disp_after;
    root.widget().for_each_child([&](const Widget &w) -> void {
        if (const auto *t = dynamic_cast<const Text *>(&w)) {
            if (t->display_text().starts_with("count = ")) {
                disp_after = t->display_text();
            }
        }
    });
    AURORA_LOG_INFO("test", "[count_display_col_test] display text after click = \"", disp_after, "\"");

    if (disp_after != "count = 1") {
        AURORA_LOG_ERROR("test", "[count_display_col_test] FAIL: display text not updated after click (=", disp_after,
                         ")");
        AURORA_TEST_REQUIRE(false);
    }

    AURORA_LOG_INFO("test", "COUNT DISPLAY (Text in Column): OK (counter=1, display text=\"count = 1\")");
}

#else

static void run() { AURORA_TEST_PRINTF("skip: AURORA_BACKEND_WIN32 not compiled into this build"); }

#endif
} // namespace aurora::tests::sec_count_display_col

AURORA_TEST() {
    aurora::tests::sec_win32_black_screen::run();
    aurora::tests::sec_win32_button_click::run();
    aurora::tests::sec_count_display_col::run();
}
