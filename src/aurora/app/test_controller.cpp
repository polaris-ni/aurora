// ============================================================================
// test_controller.cpp — 无头 widget 测试驱动实现（见 include/aurora/app/test_controller.h）。
//
// 帧序对齐 `Application::run`（specification/06-app-platform.md §3）：
//   tick 手势计时 → Animator::tick(dt) → Scheduler::tick(dt) → Window::present_root
// 与真实帧循环的两点差异（皆为「无窗口系统」所需）：
//   ① dt 取固定步长而非真实时钟（保证用例确定性）；
//   ② 不做跨线程回投排水的 drain_posted（无平台事件来源）。
// `pump_and_settle` 的收敛判据与 Application 的帧调度决策同源：present_root 的
// idle 跳帧 + Animator 无活跃动画。
// ============================================================================

#include "aurora/app/test_controller.h"

#ifdef AURORA_BACKEND_HEADLESS

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/app/scheduler.h"
#include "aurora/core/error_codes.gen.h"
#include "aurora/core/result.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/widget/widget.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

namespace aurora {

namespace {

/// @brief 文本类属性键（Text/Button/TextInput 等命名不统一，逐个比对）。
constexpr std::string_view AURORA_TEXT_PROP_KEYS[] = {"content", "text", "label", "value", "hint", "placeholder"};

/// @brief 树前序收集（Node 无父指针，自顶向下遍历 `Widget::child_nodes()`）。
auto collect_preorder(const Node &n, std::vector<Node> &out) -> void {
    if (!n) {
        return;
    }
    out.push_back(n);
    for (const Node &child : n.widget().child_nodes()) {
        collect_preorder(child, out);
    }
}

/// @brief 读单个属性的 JSON 值（缺失返回 null）。
[[nodiscard]] auto prop_of(const Widget &w, std::string_view key) -> Json {
    Json props = Json::object();
    w.serialize_props(props);
    const std::string k{key};
    if (props.contains(k)) {
        return props[k];  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
    }
    return Json{};
}

/// @brief JSON → 可读串（断言信息用；`dump()` 无缩进以保持确定性）。
[[nodiscard]] auto json_dump(const Json &j) -> std::string { return j.is_null() ? std::string{"<missing>"} : j.dump(); }

/// @brief 节点描述（断言信息里定位是哪个节点）。
[[nodiscard]] auto describe(const Node &n) -> std::string {
    if (!n) {
        return std::string{"<empty node>"};
    }
    std::string s{n.widget().type_name()};
    if (!n.id().empty()) {
        s += "#" + std::string{n.id()};
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct TestController::Impl {
    Node root;
    Config cfg;
    HeadlessSurface *surface = nullptr;  ///< 非拥有（`Window` 持有 ownership）
    std::unique_ptr<Window> window;
    Animator animator;
    Scheduler scheduler;

    Impl(Node r, Config c) : root(std::move(r)), cfg(std::move(c)) {
        // 尺寸回退：非法值（<=0）退到 800×600，避免整树被布局到 0×0。
        if (cfg.width <= 0) {
            cfg.width = 800;
        }
        if (cfg.height <= 0) {
            cfg.height = 600;
        }
        if (cfg.frame_seconds <= 0.0) {
            cfg.frame_seconds = 1.0 / 60.0;
        }

        auto surf = std::make_unique<HeadlessSurface>(
            cfg.png_path, Size{.width = static_cast<float>(cfg.width), .height = static_cast<float>(cfg.height)});
        // Surface 尺寸由首次 begin_frame 确立：present_root 首帧读取该尺寸布局整树。
        (void)surf->begin_frame(cfg.width, cfg.height);
        surface = surf.get();
        window = std::make_unique<Window>(std::move(surf));

        // 定时任务挂载点：与 Application::run 同——`Timer` 控件在 mount 时经 current() 注册。
        Scheduler::set_current(&scheduler);
        // 组件级动画挂载点：图表 grow-in 在 mount 时经 current() 注册（无则降级到终态）。
        Animator::set_current(&animator);
    }

    ~Impl() {
        // 若仍有其它 Scheduler / Animator 期待成为 current，此处置空以避免本对象析构后残留悬垂指针。
        Scheduler::set_current(nullptr);
        Animator::set_current(nullptr);
    }

    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    /// @brief 单帧：tick 三类驱动器 → present（脏驱动，可能整帧跳过）。
    [[nodiscard]] auto pump_one() -> Result<void> {
        root.widget().tick(std::chrono::steady_clock::now());
        animator.tick(cfg.frame_seconds);
        scheduler.tick(cfg.frame_seconds);
        const Result<bool> r = window->present_root(root);
        if (!r.ok()) {
            return Result<void>{r.error()};
        }
        // `Window::present_root` 只写 widget 自身几何，不写 Node（与 `render_to_png` 不同，
        // 后者显式 `root.set_bounds`）。这里补齐，使 Node 几何与管控端一致——finders 的
        // `Node::bounds()` 与「几何权威在 Node」的约定才成立。
        root.set_bounds(
            Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                 .size = Size{.width = static_cast<float>(cfg.width), .height = static_cast<float>(cfg.height)}});
        return Result<void>{};
    }

    /// @brief 交互后请求下一帧全量重绘（模拟真实平台「输入事件唤醒帧循环」）。
    ///
    /// 实测（utest_test_controller）：单纯的值变更（如 `simulate_text_input` 写入控件内部
    /// State）未必把脏传播到窗口级标记，下一帧会被判为 idle 跳过、像素停留在交互前。
    /// 真实平台每来一个输入事件都会 request_wake，故在此补同名语义。
    auto request_frame() const -> void { window->force_full_redraw(); }
};

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

TestController::TestController(Node root, const Config &cfg) : impl_(std::make_unique<Impl>(std::move(root), cfg)) {}

TestController::TestController(TestController &&other) noexcept = default;
auto TestController::operator=(TestController &&other) noexcept -> TestController & = default;
TestController::~TestController() = default;

// ---------------------------------------------------------------------------
// 帧驱动
// ---------------------------------------------------------------------------

auto TestController::pump(int frames) -> Result<void> {
    for (int i = 0; i < std::max(1, frames); ++i) {
        const Result<void> r = impl_->pump_one();
        if (!r.ok()) {
            return r;
        }
    }
    return Result<void>{};
}

auto TestController::pump_and_settle(int max_frames) -> int {
    const int limit = std::max(1, max_frames);
    for (int i = 0; i < limit; ++i) {
        if (!impl_->pump_one().ok()) {
            return i + 1;  // 出错帧仍计入，调用方据 present 结果另行断言
        }
        // 收敛判据（与 Application 的「活跃信号」同源）：本帧 idle 跳帧 + 无运行中动画。
        if (impl_->window->is_idle_frame() && !impl_->animator.has_active()) {
            return i + 1;
        }
    }
    return limit;
}

auto TestController::set_viewport(int width, int height) -> Result<void> {
    if (width <= 0 || height <= 0) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "set_viewport: viewport size must be positive (width 与 height 均须 > 0)")};
    }
    impl_->cfg.width = width;
    impl_->cfg.height = height;
    (void)impl_->surface->begin_frame(width, height);
    // 尺寸变化已由 present_root 检出；再强制一次全绘以确保下一帧完整重画（含脏区外像素）。
    impl_->window->force_full_redraw();
    return Result<void>{};
}

auto TestController::frame_count() const -> int { return impl_->surface->frame_count(); }

auto TestController::root_node() -> Node & { return impl_->root; }

auto TestController::root() -> Widget & { return impl_->root.widget(); }

auto TestController::window() -> Window & { return *impl_->window; }

auto TestController::animator() -> Animator & { return impl_->animator; }

auto TestController::scheduler() -> Scheduler & { return impl_->scheduler; }

// ---------------------------------------------------------------------------
// 查找
// ---------------------------------------------------------------------------

auto TestController::find_by_key(std::string_view id) const -> std::vector<Node> {
    std::vector<Node> all;
    collect_preorder(impl_->root, all);
    std::vector<Node> hits;
    for (const Node &n : all) {
        if (n.id() == id) {
            hits.push_back(n);
        }
    }
    return hits;
}

auto TestController::find_by_type(std::string_view type) const -> std::vector<Node> {
    std::vector<Node> all;
    collect_preorder(impl_->root, all);
    std::vector<Node> hits;
    for (const Node &n : all) {
        if (std::string_view{n.widget().type_name()} == type) {
            hits.push_back(n);
        }
    }
    return hits;
}

auto TestController::find_by_text(std::string_view text) const -> std::vector<Node> {
    std::vector<Node> all;
    collect_preorder(impl_->root, all);
    std::vector<Node> hits;
    for (const Node &n : all) {
        for (const std::string_view key : AURORA_TEXT_PROP_KEYS) {
            Json v = prop_of(n.widget(), key);
            if (v.is_string() && std::string_view{v.get<std::string>()} == text) {
                hits.push_back(n);
                break;  // 同一节点命中一个键即止（避免重复计入）
            }
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

auto TestController::tap(Widget &w) -> Result<void> {
    const Result<void> r = Inspector::simulate_click(w);
    if (r.ok()) {
        impl_->request_frame();
    }
    return r;
}

auto TestController::tap(const Node &n) -> Result<void> {
    if (!n) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "tap: target node is empty（查找失败时应检查 finder 语义与节点是否已在树中）")};
    }
    Node target = n;  // 拷贝即共享：仍是同一 widget 实例
    return tap(target.widget());
}

auto TestController::drag(Widget &w, const Point &delta) -> Result<void> {
    const Result<void> r = Inspector::simulate_drag(w, delta.x, delta.y);
    if (r.ok()) {
        impl_->request_frame();
    }
    return r;
}

auto TestController::drag(const Node &n, const Point &delta) -> Result<void> {
    if (!n) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "drag: target node is empty（查找失败时应检查 finder 语义与节点是否已在树中）")};
    }
    Node target = n;  // 拷贝即共享：仍是同一 widget 实例
    return drag(target.widget(), delta);
}

auto TestController::enter_text(Widget &w, std::string_view text) -> Result<void> {
    const Result<void> r = Inspector::simulate_text_input(w, text);
    if (r.ok()) {
        impl_->request_frame();
    }
    return r;
}

auto TestController::enter_text(const Node &n, std::string_view text) -> Result<void> {
    if (!n) {
        return Result<void>{
            make_error(ErrorCode::GeneralInvalidArgument,
                       "enter_text: target node is empty（查找失败时应检查 finder 语义与节点是否已在树中）")};
    }
    Node target = n;  // 拷贝即共享：仍是同一 widget 实例
    return enter_text(target.widget(), text);
}

// ---------------------------------------------------------------------------
// 断言
// ---------------------------------------------------------------------------

auto TestController::expect_visible(const Node &n) -> Result<void> {
    if (!n) {
        return Result<void>{make_error(ErrorCode::ValidationFailed,
                                       "expect_visible: target node is empty（先经 find_by_* 取到非空节点再断言）")};
    }
    const Json show = prop_of(n.widget(), "show");
    if (show.is_boolean() && !show.get<bool>()) {
        return Result<void>{make_error(
            ErrorCode::ValidationFailed,
            "expect_visible: " + describe(n) + " has show=false（隐藏节点不入绘制；断言前确认控件未被置为隐藏）")};
    }
    // 几何非空：参与过布局（Node 几何权威）或绘制（paint_bounds）二者取其一即可判定「可见」。
    const Size node_size = n.bounds().size;
    const Rect painted = n.widget().paint_bounds();
    if (node_size.width <= 0.0F && node_size.height <= 0.0F && painted.size.width <= 0.0F &&
        painted.size.height <= 0.0F) {
        return Result<void>{
            make_error(ErrorCode::ValidationFailed,
                       "expect_visible: " + describe(n) +
                           " has empty geometry（先 pump 至少一帧让整树完成挂载与布局，再断言可见性）")};
    }
    return Result<void>{};
}

auto TestController::expect_prop(const Widget &w, std::string_view key, const Json &expected) -> Result<void> {
    const Json actual = prop_of(w, key);
    if (actual != expected) {
        return Result<void>{make_error(ErrorCode::WidgetInvalidProp,
                                       "expect_prop: " + std::string{key} + " expected " + json_dump(expected) +
                                           " but got " + json_dump(actual) +
                                           "（属性名取自控件自描述；Reactive/Localized 属性读的是当前解析值）")};
    }
    return Result<void>{};
}

auto TestController::expect_prop(const Node &n, std::string_view key, const Json &expected) -> Result<void> {
    if (!n) {
        return Result<void>{make_error(ErrorCode::ValidationFailed,
                                       "expect_prop: target node is empty（先经 find_by_* 取到非空节点再断言）")};
    }
    return expect_prop(n.widget(), key, expected);
}

}  // namespace aurora

#endif  // AURORA_BACKEND_HEADLESS
