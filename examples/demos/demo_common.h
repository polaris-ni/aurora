// =============================================================================
// Aurora 组件 Demo 共享基础设施 (examples/demos/demo_common.h)
// -----------------------------------------------------------------------------
// 各组件 demo 通过 #include "demo_common.h" 复用：
//   * namespace pal  —— 统一调色板与排版常量
//   * gap(float)     —— 固定高度间距
//   * Card / BrandBadge / GradientTitle —— 与 showcase 一致的本地复用控件
//   * run_demo(...)  —— 统一窗口启动器（Win32 + 事件派发；非 Windows 回退 PNG）
//
// 该头为 header-only，每个 demo 为独立可执行文件，故跨 TU 的 inline/static
// 定义不会触发 ODR 问题。
// =============================================================================

#pragma once

#include <aurora/aurora.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/render/font_engine.h"
#include "aurora/window/window.h"

#ifdef AURORA_BUILD_INSPECTOR_SERVER
// 远程检视 opt-in（随链接注入的 feature 宏裁切）：AURORA_BUILD_INSPECTOR_SERVER=ON 时
// demo 目标链接 aurora_inspector_server，run_demo 可经环境变量选择启动 InspectorServer。
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>

#include "aurora/inspector/inspector_server.h"
#endif

namespace au = aurora;

// ---------------------------------------------------------------------------
// 调色板与排版常量
// ---------------------------------------------------------------------------
namespace pal {
constexpr au::Color AURORA_BG{245, 247, 250};
constexpr au::Color AURORA_SURFACE{255, 255, 255};
constexpr au::Color AURORA_BORDER{222, 226, 232};
constexpr au::Color AURORA_PRIMARY{37, 99, 235};
constexpr au::Color AURORA_PRIMARY_SOFT{224, 231, 250};
constexpr au::Color AURORA_ACCENT{236, 72, 153};
constexpr au::Color AURORA_TEXT{17, 24, 39};
constexpr au::Color AURORA_MUTED{107, 114, 128};
constexpr au::Color AURORA_OK{22, 163, 74};
constexpr au::Color AURORA_WARN{217, 119, 6};
constexpr au::Color AURORA_DANGER{220, 38, 38};
constexpr float AURORA_RADIUS = 14.0F;
constexpr float AURORA_GUTTER = 16.0F;
}  // namespace pal

// 固定高度间距（Spacer 只吸收剩余空间，这里用空 Text 加 height 修饰实现）
[[maybe_unused]] static auto gap(float h) -> au::Node {
    au::Text t{" "};
    t.modifier.set(au::Modifier{}.height(h));
    return t;
}

// ---------------------------------------------------------------------------
// 卡片容器：继承 Column（获得 on_layout），用修饰画背景/边框/内边距
// ---------------------------------------------------------------------------
class Card : public au::Column {
  public:
    explicit Card(au::Node child, au::Color fill = pal::AURORA_SURFACE)
        : Column{ColumnProps{.children = {std::move(child)}}} {
        decorate(fill);
    }

    /// @brief 便捷构造：扁平罗列子项（Card{ a, b, c }），免写 Node{} 包裹。
    Card(std::initializer_list<au::Node> kids, au::Color fill = pal::AURORA_SURFACE) : Column{kids} { decorate(fill); }

    [[nodiscard]] auto type_name() const -> const char * override { return "Card"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override { return describe_static(); }

  private:
    void decorate(au::Color fill) {
        this->modifier.set(au::Modifier{}.padding(14.0F).background(fill).border(1.0F, pal::AURORA_BORDER));
    }
};

// ---------------------------------------------------------------------------
// 品牌徽章（纯绘制叶控件）
// ---------------------------------------------------------------------------
class BrandBadge : public au::LeafWidget {
  public:
    BrandBadge(std::string label, au::Color color) : label_(std::move(label)), color_(color) {}

    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "BrandBadge"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "BrandBadge", .children_policy = "none"};
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        const float w = std::min<float>(c.max.width, 220.0F);
        return c.constrain(au::Size{.width = w, .height = 52.0F});
    }

    void on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) override {
        p.fill_rect(bounds, color_);
        const float th = aurora::render::FontEngine::measure_height(au::Font{.size_pt = 20.0F});
        const float tx = bounds.origin.x + 16.0F;
        const float ty = bounds.origin.y + ((bounds.size.height - th) * 0.5F);
        p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = ty},
                             .size = au::Size{.width = bounds.size.width - 32.0F, .height = th}},
                    label_, au::Font{.size_pt = 20.0F}, au::Color{255, 255, 255});
    }

  private:
    std::string label_;
    au::Color color_;
};

// ---------------------------------------------------------------------------
// 渐变标题（纯绘制叶控件）
// ---------------------------------------------------------------------------
class GradientTitle : public au::LeafWidget {
  public:
    explicit GradientTitle(std::string t, float size = 34.0F) : text_(std::move(t)), size_(size) {
        // 纯展示标题：自定义叶控件默认参与 Tab 序（Widget::wants_focus() 基类返回 true），故宿主侧显式让位。
        set_focusable(false);
    }

    void collect_signals(std::vector<au::SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "GradientTitle"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "GradientTitle", .children_policy = "none"};
    }

  protected:
    // 文字真实行高：必须走 FontEngine（FreeType 字形度量），与 on_paint 里 draw_text 用的是同一套字形，
    // 否则盒高（渐变背景高度）会和实际渲染出的字形高度对不上。
    [[nodiscard]] auto text_height() const -> float {
        return aurora::render::FontEngine::measure_height(au::Font{.size_pt = size_});
    }

    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        // 标题横幅语义：宽度撑满容器可用宽度，高度恒等于文字真实行高（绝不撑满高度）。
        //   - 高度 == 文字真实行高（text_height()）：on_paint 里渐变背景用 bounds.size.height、
        //     文字用 text_height() 居中绘制，二者必须同源才严格同高。
        //   - 宽度：窗口/Column 下 c.max.width 有限 → 撑满整行；Row 等场景下无限 → 回退 520。
        // 注意：绝不能用 c.max.is_finite() 判定宽度——它检查整个 Size 的宽+高，
        // 而 Column 传给子节点的 max.height 本就是 infinity，会误判为无限而回退上限。
        const float th = text_height();
        const float w = c.max.width != std::numeric_limits<float>::infinity() ? c.max.width : 520.0F;
        return c.constrain(au::Size{.width = w, .height = th});
    }

    void on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) override {
        constexpr int bands = 24;
        const float bw = bounds.size.width / bands;
        for (int i = 0; i < bands; ++i) {
            const float t = static_cast<float>(i) / (bands - 1);
            const au::Color c{static_cast<uint8_t>(37 + ((236 - 37) * t)), static_cast<uint8_t>(99 + ((72 - 99) * t)),
                              static_cast<uint8_t>(235 + ((153 - 235) * t))};
            p.fill_rect(
                au::Rect{.origin = au::Point{.x = bounds.origin.x + (static_cast<float>(i) * bw), .y = bounds.origin.y},
                         .size = au::Size{.width = bw + 1.0F, .height = bounds.size.height}},
                c);
        }
        const float th = text_height();
        const float tx = bounds.origin.x + 6.0F;
        const float ty = bounds.origin.y + ((bounds.size.height - th) * 0.5F);
        p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = ty},
                             .size = au::Size{.width = bounds.size.width - 12.0F, .height = th}},
                    text_, au::Font{.size_pt = size_}, au::Color{255, 255, 255});
    }

  private:
    std::string text_;
    float size_;
};

// ---------------------------------------------------------------------------
// 远程检视 opt-in（同一口径供「不经 run_demo」的载体复用）
// ---------------------------------------------------------------------------
#ifdef AURORA_BUILD_INSPECTOR_SERVER
/// @brief 设了 `AURORA_INSPECTOR_PORT` 即在回环端口起 InspectorServer，未设/起不来返回 nullptr。
/// @param get_root   取当前 UI 树根（HTTP 工作线程内调用，实现方须自证安全——稳定根最安全）。
/// @param get_surface 可选，取后端 Surface（`/api/debug/*` 与截图端点需要）。
/// @param get_windows 可选，枚举窗口 id（多窗口载体用，注册后 `/api/windows` 与 `?window=` 可用）。
/// @param get_window_root 可选，按窗口 id 取该窗树根（无效 id 返回空 Node）。
/// @note 端口解析与客户端侧同口径：值须全串十进制 1..65535，脏值回落默认 6280（BUILD_OPTIONS.md §5）。
inline auto start_demo_inspector(std::function<au::Node()> get_root,
                                 std::function<au::Surface *()> get_surface = nullptr,
                                 std::function<std::vector<std::uint32_t>()> get_windows = nullptr,
                                 std::function<au::Node(std::uint32_t)> get_window_root = nullptr)
    -> std::unique_ptr<au::InspectorServer> {
    const char *port_env = std::getenv("AURORA_INSPECTOR_PORT");
    if (port_env == nullptr || *port_env == '\0') {
        return nullptr;  // 未 opt-in：demo 默认安静
    }
    std::uint16_t port = 6280;  // 与 InspectorServer::start() 默认值一致
    char *end = nullptr;
    const long long parsed = std::strtoll(port_env, &end, 10);
    if (end != nullptr && *end == '\0' && parsed >= 1 && parsed <= 65535) {
        port = static_cast<std::uint16_t>(parsed);
    }
    auto server = std::make_unique<au::InspectorServer>(std::move(get_root));
    if (get_surface) {
        server->set_surface_getter(std::move(get_surface));
    }
    if (get_windows) {
        server->set_window_ids_getter(std::move(get_windows));
    }
    if (get_window_root) {
        server->set_window_tree_getter(std::move(get_window_root));
    }
    if (!server->start(port)) {
        AURORA_LOG_ERROR("demo", "[demo] inspector server failed to start on port ", port);
        return nullptr;
    }
    AURORA_LOG_INFO("demo", "[demo] inspector server listening on 127.0.0.1:", port);
    return server;
}
#endif

// ---------------------------------------------------------------------------
// 统一窗口启动器：构建 UI 树并打开真实平台窗口（事件经 EventDispatcher 派发）。
// 后端由 create_native_window 按平台自动选择（Win32/X11/Cocoa/GLFW）；无真实后端或窗口
// 创建失败则回退无头 PNG 渲染，保证各 demo 均可编译可验证。
// 返回 0 表示正常结束。
// ---------------------------------------------------------------------------
inline auto run_demo(au::Node root, const std::string &title, float w, float h) -> int {
    au::enable_dpi_awareness();  // 必须在 AllocConsole/建窗前，否则 DPI 感知失败（scale=1.0，高分屏发虚）
    au::init_console();  // 最早设置 UTF-8 控制台代码页，避免中文乱码

    au::FocusManager fm;
    fm.set_root(&root.widget());

    au::WindowOptions wopts;
    wopts.size = au::Size{.width = w, .height = h};
    wopts.title = title;

    auto win_res = au::create_native_window(wopts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[run_demo] window creation failed: ", win_res.error().message,
                         ", falling back to headless render");
        std::error_code ec;
        std::filesystem::create_directories("build", ec);
        au::Scene scene{root};
        auto r = scene.render_to_png(("build/" + title + ".png").c_str(), static_cast<int>(w), static_cast<int>(h));
        if (r) {
            AURORA_LOG_INFO("demo", "[run_demo] rendered build/", title, ".png");
        } else {
            AURORA_LOG_ERROR("demo", "[run_demo] headless render failed: ", r.error().message);
        }
        return r ? 0 : -1;
    }

    auto win = std::move(win_res.value());
#ifdef AURORA_BUILD_INSPECTOR_SERVER
    // 远程检视 opt-in：设置 AURORA_INSPECTOR_PORT 环境变量即随 demo 启动 InspectorServer，
    // 供 aurora_e2e_client / aurora_mcp 经回环 REST 驱动本窗口；未设置则不启动（demo 默认安静）。
    // 端口解析与回调接线统一走 `start_demo_inspector`（自建 Application 的载体同源复用）。
    auto inspector = start_demo_inspector([&root]() -> au::Node { return au::Node{root}; },
                                          [&win]() -> au::Surface * { return &win->surface(); });
#endif
    win->surface().set_event_handler([&](au::Event &e) -> void {
        auto &wd = root.widget();
        // 鼠标派发必须携带 FocusManager：否则点击 Text/TextInput 时 request_focus() 静默 no-op，
        // 焦点始终为空，后续键盘事件（Ctrl+C 复制 / Ctrl+A 全选等）永远到不了控件。
        if (auto *me = dynamic_cast<au::MouseEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *me, &fm);
        } else if (auto *ke = dynamic_cast<au::KeyEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *ke, fm);
        } else if (auto *se = dynamic_cast<au::ScrollEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *se);
        } else if (auto *te = dynamic_cast<au::TextInputEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *te, fm);
        } else if (auto *ce = dynamic_cast<au::TextCompositionEvent *>(&e)) {
            // IME 组合事件（CJK）：缺这一支则中文输入法的 preedit 与上屏全被丢弃，
            // 控件里既不下划线预编辑串也不落字（键被输入法吃掉，界面无任何反应）。
            au::EventDispatcher::dispatch(wd, *ce, fm);
        }
    });

    AURORA_LOG_INFO("demo", "[run_demo] window shown: ", title, "(close window to exit)");
    // 事件驱动帧循环：经 Window::run 在帧末 wait_events 阻塞，
    // 静态 demo idle 时 CPU 趋近 0（旧忙轮询恒占满一个核）；有脏区时按 60Fps 预算节流。
    // demo 无 Animator/Scheduler（需要动画的 demo 用 Application，不走 run_demo），
    // 故决策只看 has_pending_dirty；输入事件随时打断等待，交互延迟不变。
    win->run([&]() -> void {
        const auto t0 = std::chrono::steady_clock::now();
        // 每帧先驱动根的子树计时：长按阈值、甩动惯性、播放时钟等一切挂在 `Widget::tick` 上的
        // 每帧逻辑都依赖这一跳（`Application` 路径由 `tick_all` 做同样的事；`Window::run` 不持有
        // 根控件，故只能在帧回调里补）。缺这一跳则 demo 里的动画类控件恒停在首帧。
        root.widget().tick(t0);
        // 注意：不可在此手调 begin_frame——present_root 内部按需 begin，
        // 部分脏区帧会刻意跳过 begin 以保留上帧像素；外层多调一次会把缓冲刷成底色，
        // 造成脏区外全白（如拖选文字时白屏）。
        (void)win->present_root(root);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // `anim_active` 取「本帧实际发生了渲染」：自驱动控件（视频/甩动）在 tick 里标脏 → 下一帧
        // 仍需按 60Fps 节拍续跑；真正空闲的静态 demo 仍走无限深睡，idle CPU 不因此上升。
        win->set_next_wait(au::compute_wait_timeout(win->has_pending_dirty(), /*anim_active=*/!win->is_idle_frame(),
                                                    /*next_deadline_ms=*/-1.0, /*frame_budget_ms=*/1000.0 / 60.0,
                                                    elapsed_ms, win->surface().paces_frames()));
    });
    return 0;
}
