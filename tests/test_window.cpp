// 目标源单元：window/window.h + src/aurora/window/window.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_window_dirty.cpp
//   - test_window_layout_cache.cpp
//   - test_window_dirty_boundary.cpp
//   - test_window_style.cpp
//   - test_window_state.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// window/window_state.h(WindowState/WindowMode 正交枚举与映射，sec_window_state 段)。

#include <cstdio>
#include <memory>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/state/state.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "aurora/window/window_state.h"
#include "test_harness.h"
#ifdef AURORA_PLATFORM_WINDOWS
#include "aurora/window/native_surfaces.h"

using aurora::Application;
using aurora::Effect;
using aurora::HeadlessSurface;
using aurora::Node;
using aurora::Scene;
using aurora::Size;
using aurora::Text;
using aurora::WindowMode;
using aurora::WindowOptions;
using aurora::WindowState;
#endif

namespace sec_window_dirty {
namespace au = aurora;

/// @brief 纯色填充页：用于 present_root 渲染断言。
struct SolidPage : au::Widget {
    au::Color bg_;
    explicit SolidPage(au::Color c) : bg_(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }
    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> Size override {
        return c.constrain(c.max);
    }
    auto on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, bg_);
    }
};

void run() {
    AURORA_TEST_PRINTF("=== test_window_dirty ===\n");

    // ---- 1. 脏追踪默认开启：首帧 present_root 后 is_idle_frame == false ----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 100.0F, .height = 80.0F};
        opts.title = "dirty_default";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        AURORA_TEST_CHECK(win.dirty_tracking_enabled());  // 默认开启
        AURORA_TEST_CHECK(win.has_pending_dirty());  // 首帧前必有脏

        auto page = Node{SolidPage{au::Color::red()}};
        auto r = win.present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(!win.is_idle_frame());  // 首帧必须渲染
    }

    // ---- 2. 连续两帧无变化：第二帧为 idle 跳过 ----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 80.0F, .height = 60.0F};
        opts.title = "dirty_idle";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        auto page = Node{SolidPage{au::Color::blue()}};

        // 第一帧：必须渲染
        auto r1 = win.present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r1));
        AURORA_TEST_CHECK(!win.is_idle_frame());

        // 第二帧：无脏区、尺寸未变 → idle 跳过
        auto r2 = win.present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r2));
        AURORA_TEST_CHECK(win.is_idle_frame());
    }

    // ---- 3. force_full_redraw 强制下一帧非 idle ----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 60.0F, .height = 40.0F};
        opts.title = "dirty_force";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        auto page = Node{SolidPage{au::Color::green()}};

        // 第一帧
        (void)win.present_root(page);
        AURORA_TEST_CHECK(!win.is_idle_frame());

        // 第二帧 idle
        (void)win.present_root(page);
        AURORA_TEST_CHECK(win.is_idle_frame());

        // 强制全绘后第三帧必须渲染
        win.force_full_redraw();
        AURORA_TEST_CHECK(win.has_pending_dirty());
        (void)win.present_root(page);
        AURORA_TEST_CHECK(!win.is_idle_frame());
    }

    // ---- 4. 关闭脏追踪：每帧都渲染（无 idle）----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 50.0F, .height = 50.0F};
        opts.title = "dirty_off";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        win.enable_dirty_tracking(false);
        AURORA_TEST_CHECK(!win.dirty_tracking_enabled());
        AURORA_TEST_CHECK(win.has_pending_dirty());  // 关闭脏追踪时始终视为有脏

        auto page = Node{SolidPage{au::Color::red()}};
        (void)win.present_root(page);
        // 关闭脏追踪后没有 idle 帧概念（is_idle_frame 仅在脏追踪开启时置 true）
        // 验证 present_root 正常返回
        (void)win.present_root(page);
        AURORA_TEST_CHECK(win.has_pending_dirty());  // 仍然始终有脏
    }

    // ---- 5. 重新启用脏追踪：首帧强制全绘 ----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 40.0F, .height = 30.0F};
        opts.title = "dirty_reenable";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        // 先关闭
        win.enable_dirty_tracking(false);
        auto page = Node{SolidPage{au::Color::blue()}};
        (void)win.present_root(page);

        // 重新开启
        win.enable_dirty_tracking(true);
        AURORA_TEST_CHECK(win.dirty_tracking_enabled());
        AURORA_TEST_CHECK(win.has_pending_dirty());  // 重开首帧有脏

        // 重开后第一帧必须渲染
        (void)win.present_root(page);
        AURORA_TEST_CHECK(!win.is_idle_frame());
    }

    // ---- 6. 根节点切换：第二帧非 idle（root_changed 触发全绘）----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 70.0F, .height = 50.0F};
        opts.title = "dirty_root_change";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        auto page1 = Node{SolidPage{au::Color::red()}};
        auto page2 = Node{SolidPage{au::Color::blue()}};

        (void)win.present_root(page1);
        AURORA_TEST_CHECK(!win.is_idle_frame());

        // 切换到新根
        (void)win.present_root(page2);
        AURORA_TEST_CHECK(!win.is_idle_frame());  // 根变化必须重绘
    }
}
}  // namespace sec_window_dirty

namespace sec_window_layout_cache {
namespace au = aurora;

/// @brief 复现 Path B 的最小控件：on_layout 记录运行次数，并感知外部 phase 状态（0=骨架,1=真实）。
/// 当 can_cache_layout() 返回 true（旧）时，强制重排会被缓存短路，on_layout 不再运行，
/// phase 切换永不被 on_layout 拾取；返回 false（修复）时则每次重排都重跑 on_layout。
struct PhaseLeaf : au::LeafWidget {
    bool m_cache_layout_ = true;
    int m_layout_calls_ = 0;  ///< on_layout 累计运行次数
    int m_phase_ = 0;  ///< 外部状态：0=骨架, 1=真实内容（外部变更，不走 mark_needs_layout）
    bool m_saw_phase1_ = false;  ///< on_layout 是否曾以 phase==1 运行（即拾取到了状态变更）

    [[nodiscard]] auto type_name() const -> const char * override { return "PhaseLeaf"; }
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }
    [[nodiscard]] auto can_cache_layout() const -> bool override { return m_cache_layout_; }

    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> Size override {
        ++m_layout_calls_;
        if (m_phase_ == 1) {
            m_saw_phase1_ = true;  // 仅在 on_layout 真正运行且已切到 phase=1 时置位
        }
        return c.constrain(c.max);
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        p.fill_rect(b, au::Color::green());
    }
};

/// @brief 单场景：首帧建树（phase 0）→ 外部切到 phase 1（未显式 mark_needs_layout）
///        → 连续 force_full_redraw()+present_root 强制每帧整树重排，返回控件供断言。
static auto run_scenario(bool cache_layout) -> std::shared_ptr<PhaseLeaf> {
    au::HeadlessOptions opts;
    opts.size = Size{.width = 120.0F, .height = 90.0F};
    auto res = au::create_window(opts);
    AURORA_TEST_CHECK(static_cast<bool>(res));
    auto &win = *res.value();

    auto leaf = std::make_shared<PhaseLeaf>();
    leaf->m_cache_layout_ = cache_layout;
    auto page = Node{leaf};

    // 首帧：整树重排，on_layout 运行（phase 0，m_layout_calls==1）
    (void)static_cast<bool>(win.present_root(page));

    // 外部状态变更（骨架→真实），未显式 mark_needs_layout —— 这正是 Path B 的危险模式
    leaf->m_phase_ = 1;

    // 强制每帧整树重排：验证布局缓存是否让 on_layout 跳过
    for (int i = 0; i < 5; ++i) {
        win.force_full_redraw();
        (void)static_cast<bool>(win.present_root(page));
    }
    return leaf;
}

void run() {
    AURORA_TEST_PRINTF("=== test_window_layout_cache ===\n");

    // ---- 旧行为（can_cache_layout 默认 true）：强制重排仍被布局缓存短路
    //      → on_layout 仅首帧运行、状态切换未被拾取 → 复现 Path B 冻结 ----
    const auto old_leaf = run_scenario(/*cache_layout=*/true);
    AURORA_TEST_CHECK(old_leaf->m_layout_calls_ == 1);  // on_layout 被缓存冻结，不再运行
    AURORA_TEST_CHECK(!old_leaf->m_saw_phase1_);  // 状态变更未被 on_layout 拾取（白屏根因）

    // ---- 修复（can_cache_layout==false）：缓存不在本控件命中
    //      → 强制重排每次重跑 on_layout，拾取 phase=1 → 不再冻结 ----
    const auto new_leaf = run_scenario(/*cache_layout=*/false);
    AURORA_TEST_CHECK(new_leaf->m_layout_calls_ > 1);  // 每次强制重排都重跑 on_layout
    AURORA_TEST_CHECK(new_leaf->m_saw_phase1_);  // 状态变更被 on_layout 拾取（修复）
}
}  // namespace sec_window_layout_cache

namespace sec_window_dirty_boundary {
namespace au = aurora;

/// @brief 纯色填充页：boundary 子节点用。
struct SolidPage : au::Widget {
    au::Color bg_;
    explicit SolidPage(au::Color c) : bg_(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }
    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> Size override {
        return c.constrain(c.max);
    }
    auto on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, bg_);
    }
};

/// @brief 模拟「同根内原地回收 boundary 子节点」的宿主：复现白屏 UAF 时序。
/// 非 boundary（WrapContent）。持有单个 boundary 子节点 inner。
/// 首帧 paint 时：先把 inner 标脏（登记到 m_dirty_boundaries）再原地销毁 inner，
/// 使 m_dirty_boundaries 持有悬垂/失效指针进入下一帧。
struct BombHost : au::Widget {
    std::shared_ptr<Widget> inner_;  ///< boundary 子节点（持有所有权）
    bool armed_ = true;  ///< 首帧 paint 触发「标脏 + 销毁」
    au::Color self_bg_ = au::Color::green();

    [[nodiscard]] auto type_name() const -> const char * override { return "BombHost"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "BombHost", .children_policy = "single"};
    }
    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}

    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> Size override {
        const Size s = c.constrain(c.max);
        if (inner_) {
            inner_->layout(c, ctx);
        }
        return s;
    }

    auto on_paint(au::Painter &p, const au::Rect &bounds, const au::BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, self_bg_);
        if (armed_ && inner_) {
            // 1) 标脏 → 触发 inner.on_dirty(true) → 登记 &inner 到 m_dirty_boundaries（此时仍存活）
            inner_->mark_needs_layout();
            // 2) 原地销毁 inner（模拟 lazy row 回收）→ m_dirty_boundaries 持有失效指针
            inner_.reset();
            armed_ = false;
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (inner_) {
            fn(*inner_);
        }
    }
};

void run() {
    AURORA_TEST_PRINTF("=== test_window_dirty_boundary ===\n");

    // ---- 复现：boundary 在帧间被原地销毁，下一帧局部重排不得解引用悬垂指针 ----
    {
        au::HeadlessOptions opts;
        opts.size = Size{.width = 120.0F, .height = 90.0F};
        opts.title = "dirty_boundary_uaf";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto &win = *res.value();

        auto host = std::make_shared<BombHost>();
        auto inner = std::make_shared<SolidPage>(au::Color::red());
        inner->width(au::px(40));  // 显式宽度 → is_relayout_boundary() == true
        host->inner_ = inner;
        auto page = Node{host};

        // 首帧：整树重排 + 接线 on_dirty；paint 中把 inner 标脏并原地销毁。
        AURORA_TEST_CHECK(static_cast<bool>(win.present_root(page)));
        AURORA_TEST_CHECK(!win.is_idle_frame());

        // 第二帧（同一根 Node）：do_layout 因 m_dirty_boundaries 非空而为真，
        // whole_tree_relayout 为假 → 进入局部重排分支。
        // 修复前：解引用已释放的 inner → UB / ASan heap-use-after-free。
        // 修复后：weak_ptr.lock() 失败 → 跳过，成功返回。
        // 先仅标脏区（不动 m_layout_dirty），否则会走 idle 跳过分支绕开局部重排。
        win.mark_dirty(
            au::Rect{.origin = au::Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 10.0F, .height = 10.0F}});
        const bool r2 = static_cast<bool>(win.present_root(page));
        AURORA_TEST_CHECK(r2);  // 不得崩溃
        AURORA_TEST_CHECK(!win.is_idle_frame());  // 第二帧确实进入了 layout（命中局部重排分支）
    }
}
}  // namespace sec_window_dirty_boundary

namespace sec_window_style {

using aurora::Size;
using aurora::WindowOptions;
using aurora::WindowStyleOptions;

void run() {
    // ---- 1. WindowStyleOptions 默认值 ----
    {
        WindowStyleOptions style;
        AURORA_TEST_CHECK(!style.always_on_top);
        AURORA_TEST_CHECK(!style.frameless);
        AURORA_TEST_CHECK(style.resizable);
        AURORA_TEST_CHECK(style.min_size.width == 0.0F);
        AURORA_TEST_CHECK(style.max_size.height == 0.0F);
    }

    // ---- 2. WindowOptions 集成 style 字段 ----
    {
        WindowOptions opts;
        opts.style.always_on_top = true;
        opts.style.frameless = true;
        opts.style.min_size = Size{.width = 200.0F, .height = 150.0F};
        AURORA_TEST_CHECK(opts.style.always_on_top);
        AURORA_TEST_CHECK(opts.style.frameless);
        AURORA_TEST_CHECK(opts.style.min_size.width == 200.0F);
    }

    // ---- 3. 聚合初始化（AI 友好声明式写法）----
    {
        constexpr WindowStyleOptions style{
            .always_on_top = true,
            .frameless = false,
            .resizable = false,
            .min_size = Size{.width = 320.0F, .height = 240.0F},
            .max_size = Size{.width = 1280.0F, .height = 720.0F},
        };
        AURORA_TEST_CHECK(style.always_on_top);
        AURORA_TEST_CHECK(!style.resizable);
        AURORA_TEST_CHECK(style.max_size.width == 1280.0F);
    }

#ifdef AURORA_PLATFORM_WINDOWS
    // ---- 4. Win32Surface 带样式构造：真实创建（置顶 + 固定尺寸）后立即退出 ----
    {
        WindowStyleOptions style;
        style.always_on_top = true;
        style.resizable = false;
        style.min_size = Size{.width = 100.0F, .height = 80.0F};
        style.max_size = Size{.width = 400.0F, .height = 300.0F};

        aurora::Win32Surface surf{200, 150, "StyleTest", style};
        // 创建成功即样式映射未崩溃；尺寸为逻辑 dp
        AURORA_TEST_CHECK(surf.size().width == 200.0F);
        AURORA_TEST_CHECK(surf.size().height == 150.0F);
        // 立即析构（不进入消息循环，不阻塞测试）
    }

    // ---- 5. 无边框窗口创建 ----
    {
        WindowStyleOptions style;
        style.frameless = true;
        aurora::Win32Surface surf{160, 120, "FramelessTest", style};
        AURORA_TEST_CHECK(surf.size().width == 160.0F);
    }

    // ---- 6. create_window(Win32Options) 传递样式 ----
    {
        aurora::Win32Options opts;
        opts.size = Size{.width = 240.0F, .height = 180.0F};
        opts.title = "FactoryStyleTest";
        opts.style.always_on_top = true;
        auto res = aurora::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        // 窗口即刻销毁（unique_ptr 作用域结束），不阻塞
    }
#endif
}
}  // namespace sec_window_style

namespace sec_window_state {

void run() {
    AURORA_TEST_PRINTF("=== test_window_state ===\n");

    // 1) 纯函数真值表（与后端无关的确定性映射，便于单测）。
    AURORA_TEST_CHECK(aurora::compute_window_state(false, true) == aurora::WindowState::Visible);
    AURORA_TEST_CHECK(aurora::compute_window_state(false, false) == aurora::WindowState::Occluded);
    AURORA_TEST_CHECK(aurora::compute_window_state(true, true) == aurora::WindowState::Hidden);
    AURORA_TEST_CHECK(aurora::compute_window_state(true, false) == aurora::WindowState::Hidden);

    AURORA_TEST_CHECK(aurora::compute_window_mode(false, false, false) == aurora::WindowMode::Normal);
    AURORA_TEST_CHECK(aurora::compute_window_mode(true, false, false) == aurora::WindowMode::Minimized);
    AURORA_TEST_CHECK(aurora::compute_window_mode(false, true, false) == aurora::WindowMode::Maximized);
    AURORA_TEST_CHECK(aurora::compute_window_mode(false, false, true) == aurora::WindowMode::FullScreen);
    // 正交性：最大化时可见性仍为 Visible（仅几何态改变）。
    AURORA_TEST_CHECK(aurora::compute_window_state(false, true) == aurora::WindowState::Visible);

    // 2) Application + Headless 后端确定性驱动。
    Scene scene{Node{Text{"root"}}};
    WindowOptions opts;
    opts.size = Size{.width = 240.0F, .height = 160.0F};
    auto win_res = au::create_window(au::HeadlessOptions{opts});
    Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
    AURORA_TEST_CHECK(app.window() != nullptr);

    auto &surf = dynamic_cast<HeadlessSurface &>(app.window()->surface());

    // 初始状态（构造时 handler 尚未注册，不回放；保持默认 Visible）。
    AURORA_TEST_CHECK(app.window_state().get() == WindowState::Visible);
    AURORA_TEST_CHECK(app.window_mode().get() == WindowMode::Normal);

    // 命令式回调聚合可见性状态。
    std::vector<WindowState> states;
    app.set_on_window_state([&](WindowState s) -> void { states.push_back(s); });

    surf.simulate_window_state(WindowState::Hidden);
    AURORA_TEST_CHECK(app.window_state().get() == WindowState::Hidden);
    AURORA_TEST_CHECK(!states.empty() && states.back() == WindowState::Hidden);

    surf.simulate_window_state(WindowState::Occluded);
    AURORA_TEST_CHECK(app.window_state().get() == WindowState::Occluded);
    AURORA_TEST_CHECK(states.back() == WindowState::Occluded);

    surf.simulate_window_state(WindowState::Visible);
    AURORA_TEST_CHECK(app.window_state().get() == WindowState::Visible);
    // 无变化不重复通知（State 先比较后 set）。
    surf.simulate_window_state(WindowState::Visible);
    AURORA_TEST_CHECK(states.size() == 3);  // Hidden, Occluded, Visible

    // 命令式回调聚合几何态（与可见性正交）。
    std::vector<WindowMode> modes;
    app.set_on_window_mode([&](WindowMode m) -> void { modes.push_back(m); });

    surf.simulate_window_mode(WindowMode::Maximized);
    AURORA_TEST_CHECK(app.window_mode().get() == WindowMode::Maximized);
    AURORA_TEST_CHECK(modes.back() == WindowMode::Maximized);

    surf.simulate_window_mode(WindowMode::Minimized);
    AURORA_TEST_CHECK(app.window_mode().get() == WindowMode::Minimized);

    surf.simulate_window_mode(WindowMode::Normal);
    AURORA_TEST_CHECK(app.window_mode().get() == WindowMode::Normal);
    AURORA_TEST_CHECK(modes.size() == 3);

    // 3) 响应式：Effect 读取 window_state 后，状态变化应使其重跑。
    int runs = 0;
    Effect eff([&]() -> void {
        (void)app.window_state().get();  // 读取即登记依赖
        ++runs;
    });
    eff.run();
    const int after_init = runs;  // 1
    surf.simulate_window_state(WindowState::Hidden);  // 触发 set → notify
    AURORA_TEST_CHECK(runs == after_init + 1);
}
}  // namespace sec_window_state

AURORA_TEST() {
    sec_window_dirty::run();
    sec_window_layout_cache::run();
    sec_window_dirty_boundary::run();
    sec_window_style::run();
    sec_window_state::run();
}
