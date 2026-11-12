/// 测试类型: unit
/// 目标单元: include/aurora/window/window.h
/// 测试说明: 覆盖 Window 纯逻辑路径——WindowOptions/HeadlessOptions 默认值不变量、
/// 标题/尺寸/装饰内边距与帧生命周期向 Surface 的转发、程序化窗口控制、
/// 脏追踪开关语义、HUD 叠加层槽位与「空闲期 HUD-only 刷新帧」调度、
/// run 帧数上限与空 Surface 工厂拒绝；
/// 另以计数型 RhiFrameSink 桩锁定「系统重绘请求在 GPU 帧路径下重渲染而非裸 present」，
/// 并锁定无障碍开关换档经根注入送达子树（含换档帧不得被 idle 跳过、不入键位不得出帧）

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "aurora/core/accessibility.h"
#include "aurora/core/duration.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/spacer.h"
#include "aurora/widget/text.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window {

using aurora::testing::require_value;

namespace {

/// @brief 记录型 Surface 桩：纯内存实现，记录 Window 转发的调用与参数。
class RecordingSurface final : public Surface {
  public:
    // 测试替身的记录成员需被测试体直接读写，刻意 public，不改私有。
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    Size size_val{.width = 320.0F, .height = 240.0F};
    EdgeInsets inset_val{};
    std::string last_title;
    bool close_requested = false;
    bool close_called = false;
    bool minimize_called = false;
    bool toggle_maximize_called = false;
    int fullscreen_last = -1;  // -1 未调用 / 1 true / 0 false
    bool begin_move_called = false;
    std::optional<WindowResizeEdge> resize_last;
    std::optional<TitleBarStyle> style_last;
    std::shared_ptr<Image> icon_last;
    int begin_w = 0;
    int begin_h = 0;
    int present_count = 0;
    int pump_count = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto begin_frame(int width, int height) -> Result<bool> override {
        begin_w = width;
        begin_h = height;
        // 与真实软件后端一致地真正启动离屏缓冲：`present_root` 的绘制/合成会落进这个 Painter，
        // 桩若只记参数不 begin，缓冲恒 0×0 而让所有绘制静默失效（测试看不出差异但毫无覆盖力）。
        painter_.begin(width, height);
        return Result<bool>{true};
    }
    auto painter() -> Painter & override { return painter_; }
    auto present() -> Result<bool> override {
        ++present_count;
        return Result<bool>{true};
    }
    [[nodiscard]] auto size() const -> Size override { return size_val; }
    [[nodiscard]] auto should_close() const -> bool override { return close_requested; }
    auto poll_platform_events() -> void override { ++pump_count; }
    auto set_title(const std::string &title) -> void override { last_title = title; }
    [[nodiscard]] auto content_inset() const -> EdgeInsets override { return inset_val; }
    auto close() -> void override { close_called = true; }
    auto minimize() -> void override { minimize_called = true; }
    auto toggle_maximize() -> void override { toggle_maximize_called = true; }
    auto set_fullscreen(bool on) -> void override { fullscreen_last = on ? 1 : 0; }
    auto begin_window_move() -> void override { begin_move_called = true; }
    auto begin_window_resize(WindowResizeEdge edge) -> void override { resize_last = edge; }
    auto set_title_bar_style(const TitleBarStyle &style) -> void override { style_last = style; }
    auto set_title_bar_icon(const std::shared_ptr<Image> &icon) -> void override { icon_last = icon; }

  private:
    Painter painter_;
};

/// @brief 计数型命令面桩：证明帧 DL（含系统重绘帧）确实回放到了 GPU 侧而非软件侧。
class CountingRhi final : public rhi::RhiBackend {
  public:
    int submits = 0;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes) 测试替身记录成员

    [[nodiscard]] auto name() const -> std::string_view override { return "gpu-stub"; }
    auto submit(const DrawCmd & /*cmd*/, const rhi::CmdData & /*data*/) -> void override { ++submits; }
};

/// @brief 计数型 GPU 帧调度桩：`begin_frame` 可切换成败，以覆盖「GPU 生效」与「永久回退」两分支。
class CountingSink final : public rhi::RhiFrameSink {
  public:
    int begin_calls = 0;  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    int end_calls = 0;
    bool begin_ok = true;  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    [[nodiscard]] auto name() const -> std::string_view override { return "gpu-stub"; }
    [[nodiscard]] auto backend() -> rhi::RhiBackend & override { return rhi_; }
    [[nodiscard]] auto begin_frame(int /*device_width*/, int /*device_height*/, float /*scale*/) -> bool override {
        ++begin_calls;
        return begin_ok;
    }
    auto end_frame() -> void override { ++end_calls; }

    /// @brief 已消费命令数（GPU 侧真正收到帧内容的证据）。
    [[nodiscard]] auto rhi_submits() const -> int { return rhi_.submits; }

  private:
    CountingRhi rhi_;
};

/// @brief 带 GPU 帧挂点的 Surface 桩：软件路径仍留真实 Painter 缓冲，供 GPU 失效回退时回放。
class GpuStubSurface final : public Surface {
  public:
    // 测试替身的记录成员需被测试体直接读写，刻意 public，不改私有。
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    int present_count = 0;
    int painter_begin_calls = 0;
    CountingSink sink;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override {
        ++painter_begin_calls;
        painter_.begin(width, height);
        return Result<bool>{true};
    }
    [[nodiscard]] auto painter() -> Painter & override { return painter_; }
    [[nodiscard]] auto present() -> Result<bool> override {
        ++present_count;
        return Result<bool>{true};
    }
    [[nodiscard]] auto size() const -> Size override { return Size{.width = 320.0F, .height = 240.0F}; }
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override { return &sink; }

    /// @brief 触发系统重绘请求：真实后端在 WM_PAINT / Wayland configure 同址调用本回调。
    auto fire_present_request() -> void {
        if (present_request_) {
            present_request_();
        }
    }

  private:
    Painter painter_;
};

/// @brief 会真正产出绘制命令的根：`Spacer` 自身无绘制，帧 DL 会为空，无法区分命令去向。
class FilledSpacer final : public Spacer {
  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, Color{0x11U, 0x22U, 0x33U, 0xFFU});
    }
};

/// @brief 布局期抄下「注入到本窗根上下文的主题」的叶桩。
///
/// 无障碍开关的生效路径是「进程级设置 → Window 物化点 → root_env_ 注入 → 子树读取」，物化点在
/// Window 私有面里取不到，故「开关置位 → 生效值变」只能由子树侧的读数证明。桩同时留下
/// `BuildContext` 副本（其 `env` 指向地址恒定的 root_env_，窗口存活期内可再喂给控件读件），
/// 用于比对控件回落点解出的生效值与主题令牌是否同源。
class ThemeProbeLeaf final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "ThemeProbe"; }

    /// @brief 最近一次布局所见主题的快照拷贝。
    /// @return 已布局过为其指针；否则 nullptr（调用方须判空）。
    [[nodiscard]] auto seen() const -> const Theme * { return seen_.has_value() ? &*seen_ : nullptr; }

    /// @brief 最近一次布局拿到的上下文副本。
    /// @return 已布局过为有值；否则为空。
    [[nodiscard]] auto ctx() const -> const std::optional<BuildContext> & { return ctx_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        seen_ = inherit_theme(ctx);
        ctx_ = ctx;
        return c.constrain(Size{.width = 64.0F, .height = 64.0F});
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    std::optional<Theme> seen_;
    std::optional<BuildContext> ctx_;
};

/// @brief 进程级无障碍设置的 RAII 复原守卫：本文件其余用例默认全关，改动必须还原。
class ScopedSettings final {
  public:
    ScopedSettings() : saved_{current_accessibility_settings()} {}
    ~ScopedSettings() { set_accessibility_settings(saved_); }

    ScopedSettings(const ScopedSettings &) = delete;
    auto operator=(const ScopedSettings &) -> ScopedSettings & = delete;
    ScopedSettings(ScopedSettings &&) = delete;
    auto operator=(ScopedSettings &&) -> ScopedSettings & = delete;

  private:
    AccessibilitySettings saved_;
};

}  // namespace

AURORA_TEST_CASE(window_options_defaults) {
    // 跨后端共享选项的默认值不变量（AI 消费者据此可零配置开窗）。
    const WindowOptions opts;
    AURORA_TEST_CHECK_NEAR(opts.size.width, 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.size.height, 600.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(opts.title, std::string{"Aurora"});
    AURORA_TEST_CHECK_EQ(opts.max_frames, -1);
    AURORA_TEST_CHECK_EQ(opts.max_fps, 60);
    AURORA_TEST_CHECK_TRUE(opts.power_saving);
    AURORA_TEST_CHECK_EQ(opts.renderer, RendererPreference::Auto);
    // 高级样式默认：普通可缩放窗口 + 自动装饰策略。
    AURORA_TEST_CHECK_FALSE(opts.style.always_on_top);
    AURORA_TEST_CHECK_FALSE(opts.style.frameless);
    AURORA_TEST_CHECK_FALSE(opts.style.transparent);
    AURORA_TEST_CHECK_TRUE(opts.style.resizable);
    AURORA_TEST_CHECK_EQ(opts.style.decoration, DecorationPolicy::Auto);
    AURORA_TEST_CHECK_NEAR(opts.style.min_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.style.max_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.style.title_bar.height, 36.0F, 1e-4F);
    // Headless 专属选项：继承通用默认，png_path 默认留内存。
    const HeadlessOptions headless;
    AURORA_TEST_CHECK_TRUE(headless.png_path.empty());
    AURORA_TEST_CHECK_EQ(headless.renderer, RendererPreference::Auto);
    AURORA_TEST_CHECK_NEAR(headless.size.width, 800.0F, 1e-4F);
}

AURORA_TEST_CASE(window_title_defaults_and_forwards_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface const &surf = *stub;
    Window w{std::move(stub)};
    AURORA_TEST_CHECK_EQ(w.title(), std::string{"Aurora"});
    w.set_title("Hello");
    AURORA_TEST_CHECK_EQ(w.title(), std::string{"Hello"});
    // set_title 同步下发到后端（Win32 经 SetWindowText 生效）。
    AURORA_TEST_CHECK_EQ(surf.last_title, std::string{"Hello"});
}

AURORA_TEST_CASE(window_size_and_content_inset_delegate_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window const w{std::move(stub)};
    const Size sz = w.size();
    AURORA_TEST_CHECK_NEAR(sz.width, 320.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sz.height, 240.0F, 1e-4F);
    // 默认无装饰：安全区内边距全零。
    const EdgeInsets default_inset = w.content_inset();
    AURORA_TEST_CHECK_NEAR(default_inset.horizontal(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(default_inset.vertical(), 0.0F, 1e-4F);
    // 后端报告 CSD 装饰时逐字段镜像（应用据此下沉根布局）。
    surf.inset_val = EdgeInsets{.left = 2.0F, .top = 36.0F, .right = 2.0F, .bottom = 0.0F};
    const EdgeInsets inset = w.content_inset();
    AURORA_TEST_CHECK_NEAR(inset.top, 36.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.left, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(inset.bottom, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(window_frame_lifecycle_delegates_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface const &surf = *stub;
    Window const w{std::move(stub)};
    // begin_frame 以窗口逻辑尺寸（取整）启动后端帧。
    const auto bf = w.begin_frame();
    AURORA_TEST_CHECK_TRUE(bf.ok());
    AURORA_TEST_CHECK_TRUE(bf.value());
    AURORA_TEST_CHECK_EQ(surf.begin_w, 320);
    AURORA_TEST_CHECK_EQ(surf.begin_h, 240);
    // present 提交当前帧（无头后端即计数 +1）。
    const auto pr = w.present();
    AURORA_TEST_CHECK_TRUE(pr.ok());
    AURORA_TEST_CHECK_TRUE(pr.value());
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);
}

AURORA_TEST_CASE(window_control_actions_forward_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window const w{std::move(stub)};
    w.minimize();
    w.toggle_maximize();
    w.set_fullscreen(true);
    w.begin_window_move();
    w.begin_window_resize(WindowResizeEdge::BottomRight);
    AURORA_TEST_CHECK_TRUE(surf.minimize_called);
    AURORA_TEST_CHECK_TRUE(surf.toggle_maximize_called);
    AURORA_TEST_CHECK_EQ(surf.fullscreen_last, 1);
    AURORA_TEST_CHECK_TRUE(surf.begin_move_called);
    AURORA_TEST_REQUIRE_TRUE(surf.resize_last.has_value());
    AURORA_TEST_CHECK_EQ(require_value(surf.resize_last), WindowResizeEdge::BottomRight);
    w.set_fullscreen(false);
    AURORA_TEST_CHECK_EQ(surf.fullscreen_last, 0);
    // 关闭流：close() 转发，后端置关闭请求后 should_close() 为真。
    AURORA_TEST_CHECK_FALSE(w.should_close());
    w.close();
    AURORA_TEST_CHECK_TRUE(surf.close_called);
    surf.close_requested = true;
    AURORA_TEST_CHECK_TRUE(w.should_close());
}

AURORA_TEST_CASE(window_title_bar_style_and_icon_forward_to_surface) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface &surf = *stub;
    Window const w{std::move(stub)};
    TitleBarStyle style;
    style.height = 42.0F;
    style.button_layout = TitleBarButtonLayout::Windows;
    w.set_title_bar_style(style);
    AURORA_TEST_REQUIRE_TRUE(surf.style_last.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(surf.style_last.value().height, 42.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(surf.style_last.value().button_layout, TitleBarButtonLayout::Windows);
    // NOLINTEND(bugprone-unchecked-optional-access)
    // 图标槽：空指针也走透传路径（清除图标语义）。
    w.set_title_bar_icon(nullptr);
    AURORA_TEST_CHECK_TRUE(surf.icon_last == nullptr);
}

AURORA_TEST_CASE(window_dirty_tracking_toggle_and_pending_dirty) {
    auto stub = std::make_unique<RecordingSurface>();
    Window w{std::move(stub)};
    // 默认开启脏追踪；首帧/布局脏使下一帧必绘。
    AURORA_TEST_CHECK_TRUE(w.dirty_tracking_enabled());
    AURORA_TEST_CHECK_TRUE(w.has_pending_dirty());
    AURORA_TEST_CHECK_TRUE(w.dirty_tracker().is_empty());
    const Rect dirty_rect{.origin = Point{.x = 10.0F, .y = 10.0F}, .size = Size{.width = 40.0F, .height = 30.0F}};
    w.mark_dirty(dirty_rect);
    AURORA_TEST_CHECK_FALSE(w.dirty_tracker().is_empty());
    w.force_full_redraw();
    AURORA_TEST_CHECK_TRUE(w.dirty_tracker().is_full());
    // 关闭后视为永远有脏（每帧全绘，仅受帧预算节流）。
    w.enable_dirty_tracking(false);
    AURORA_TEST_CHECK_FALSE(w.dirty_tracking_enabled());
    AURORA_TEST_CHECK_TRUE(w.has_pending_dirty());
    // 重新开启：清空脏集合并重置首帧标志（下一帧强制全绘）。
    w.enable_dirty_tracking(true);
    AURORA_TEST_CHECK_TRUE(w.dirty_tracking_enabled());
    AURORA_TEST_CHECK_TRUE(w.dirty_tracker().is_empty());
    AURORA_TEST_CHECK_TRUE(w.has_pending_dirty());
}

AURORA_TEST_CASE(window_overlay_slot_roundtrip) {
    auto stub = std::make_unique<RecordingSurface>();
    Window w{std::move(stub)};
    AURORA_TEST_CHECK_TRUE(w.overlay() == nullptr);
    const auto overlay = std::make_shared<Spacer>();
    w.set_overlay(overlay);
    AURORA_TEST_CHECK_TRUE(w.overlay() == overlay);
    // nullptr 关闭叠加层。
    w.set_overlay(nullptr);
    AURORA_TEST_CHECK_TRUE(w.overlay() == nullptr);
}

AURORA_TEST_CASE(window_idle_hud_refresh_presents_without_repainting_tree) {
    auto stub = std::make_unique<RecordingSurface>();
    RecordingSurface const &surf = *stub;
    Window w{std::move(stub)};
    Node page = FilledSpacer{};
    // 叠加层用裸 Spacer：本用例只验证「空闲期是否为叠加层出帧」的调度与上屏，
    // 叠加层画了什么无关（HUD 内容刷新已有 itest_perf_overlay_refresh 覆盖）。
    w.set_overlay(std::make_shared<Spacer>());

    // 刚注入、尚未渲染过 → 立即到期，否则「空闲中 set_overlay」会永不显示。
    AURORA_TEST_CHECK_TRUE(w.hud_refresh_pending());
    AURORA_TEST_CHECK_NEAR(w.hud_refresh_due_ms(), 0.0, 1e-9);

    // 首帧：完整渲染 + 合成 HUD。
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_FALSE(w.is_idle_frame());
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);
    // 刚渲染 → 叠加层未到期，下一帧应整帧跳过（不上屏）。
    AURORA_TEST_CHECK_FALSE(w.hud_refresh_pending());
    AURORA_TEST_CHECK_GT(w.hud_refresh_due_ms(), 0.0);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_TRUE(w.is_idle_frame());
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);

    // 睡过一个刷新周期：整树仍无脏，但叠加层到期 ⇒ 必须仅为此上屏一次
    // （否则帧循环深睡下去，HUD 永久停在最后一帧的读数上）。
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(Window::AURORA_HUD_REFRESH_MS) + 60));
    AURORA_TEST_CHECK_TRUE(w.hud_refresh_pending());
    AURORA_TEST_CHECK_NEAR(w.hud_refresh_due_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    // 关键不变量：仍是 idle 帧（没有渲染树 ⇒ 不计入 FPS / 帧时间），但确实上屏了一次。
    AURORA_TEST_CHECK_TRUE(w.is_idle_frame());
    AURORA_TEST_CHECK_EQ(surf.present_count, 2);
    // 上屏后回到未到期态，下一帧重新整帧跳过。
    AURORA_TEST_CHECK_FALSE(w.hud_refresh_pending());
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_EQ(surf.present_count, 2);

    // 空闲中注入叠加层：同样应触发一次 HUD-only 上屏（不是等下一次树脏）。
    w.set_overlay(std::make_shared<Spacer>());
    AURORA_TEST_CHECK_NEAR(w.hud_refresh_due_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_EQ(surf.present_count, 3);

    // 卸下叠加层：不再为 HUD 唤醒（`<0` = 无此唤醒源，帧调度不参与最小截止时间比较）。
    w.set_overlay(nullptr);
    AURORA_TEST_CHECK_FALSE(w.hud_refresh_pending());
    AURORA_TEST_CHECK_NEAR(w.hud_refresh_due_ms(), -1.0, 1e-9);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_EQ(surf.present_count, 3);
}

AURORA_TEST_CASE(window_gpu_path_does_not_take_hud_only_shortcut) {
    // GPU 帧路径下软件缓冲只有底色（控件像素在 GPU 侧）：裸 present 会闪一屏空白，
    // 故 HUD 到期也必须走完整重渲染（帧 DL 录制 + replay），不得抄软件路径的近道。
    auto stub = std::make_unique<GpuStubSurface>();
    GpuStubSurface const &surf = *stub;
    Window w{std::move(stub)};
    Node page = FilledSpacer{};
    w.set_overlay(std::make_shared<Spacer>());

    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_EQ(surf.sink.begin_calls, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(Window::AURORA_HUD_REFRESH_MS) + 60));
    AURORA_TEST_CHECK_TRUE(w.hud_refresh_pending());
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    // 完整重渲染：GPU 帧调度再次 begin/end，且本帧不算 idle。
    AURORA_TEST_CHECK_FALSE(w.is_idle_frame());
    AURORA_TEST_CHECK_EQ(surf.sink.begin_calls, 2);
    AURORA_TEST_CHECK_EQ(surf.sink.end_calls, 2);
    AURORA_TEST_CHECK_EQ(surf.present_count, 2);
}

AURORA_TEST_CASE(create_window_rejects_null_surface) {
    // 文档化契约：空 Surface 注入返回 Error（不崩溃），错误归属调用方。
    const auto result = create_window(std::unique_ptr<Surface>{});
    AURORA_TEST_CHECK_FALSE(result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::PlatformUnavailable);
}

AURORA_TEST_CASE(system_redraw_with_gpu_sink_re_renders_instead_of_bare_present) {
    auto stub = std::make_unique<GpuStubSurface>();
    GpuStubSurface &surf = *stub;
    Window w{std::move(stub)};
    Node page = FilledSpacer{};

    // 首帧走 GPU 通道：录帧 DL → sink.begin_frame → replay → sink.end_frame → present。
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    CountingSink const &sink = surf.sink;
    AURORA_TEST_CHECK_EQ(sink.begin_calls, 1);
    AURORA_TEST_CHECK_EQ(sink.end_calls, 1);
    AURORA_TEST_CHECK_GT(sink.rhi_submits(), 0);  // 命令确实进了 GPU 消费面
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);

    // 同根、无脏、尺寸未变 → idle 跳帧（整帧跳过，不上屏）。
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_TRUE(w.is_idle_frame());
    AURORA_TEST_CHECK_EQ(sink.begin_calls, 1);
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);

    // WM_PAINT 语义（尺寸未变且无脏）：GPU 模式下必须重渲染整帧。裸 present 上屏的是
    // 软件缓冲，而 GPU 模式 Painter 缓冲只有底色、从无控件像素 —— 即白闪缺陷。
    surf.fire_present_request();
    AURORA_TEST_CHECK_EQ(sink.begin_calls, 2);
    AURORA_TEST_CHECK_EQ(sink.end_calls, 2);
    AURORA_TEST_CHECK_EQ(surf.present_count, 2);
    AURORA_TEST_CHECK_FALSE(w.is_idle_frame());
}

AURORA_TEST_CASE(system_redraw_after_gpu_fallback_keeps_bare_present) {
    auto stub = std::make_unique<GpuStubSurface>();
    GpuStubSurface &surf = *stub;
    surf.sink.begin_ok = false;  // 首帧即判定 GPU 失效 → Window 永久回退软件路径
    Window w{std::move(stub)};
    Node page = FilledSpacer{};

    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_EQ(surf.sink.begin_calls, 1);
    AURORA_TEST_CHECK_EQ(surf.sink.end_calls, 0);  // begin 失败 → 无 end_frame
    AURORA_TEST_CHECK_EQ(surf.present_count, 1);

    // 回退后软件帧缓冲持有真实控件像素，兜底全量 blit 即正确：不重渲染、不再触碰 GPU。
    const int presents = surf.present_count;
    const int begins = surf.painter_begin_calls;
    surf.fire_present_request();
    AURORA_TEST_CHECK_EQ(surf.present_count, presents + 1);
    AURORA_TEST_CHECK_EQ(surf.painter_begin_calls, begins);
    AURORA_TEST_CHECK_EQ(surf.sink.begin_calls, 1);
    AURORA_TEST_CHECK_EQ(surf.sink.end_calls, 0);
}

AURORA_TEST_CASE(accessibility_switches_reach_subtree_through_root_injection) {
    const ScopedSettings guard;
    auto stub = std::make_unique<RecordingSurface>();
    Window w{std::move(stub)};
    auto probe = std::make_shared<ThemeProbeLeaf>();
    Node page{probe};

    set_accessibility_settings(AccessibilitySettings{});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_REQUIRE(probe->seen() != nullptr);
    const Theme neutral = *probe->seen();
    AURORA_TEST_REQUIRE(probe->ctx().has_value());
    // 基线自身必须是「不缩放 / 不减动效 / 不高对比」的快照，否则后面的成对比较没有基准。
    AURORA_TEST_CHECK_GT(neutral.type_scale.body_medium.size_pt, 0.0F);
    AURORA_TEST_CHECK_GT(neutral.spacing.medium, 0.0F);
    AURORA_TEST_CHECK_GT(neutral.shapes.small, 0.0F);
    AURORA_TEST_CHECK_FALSE(neutral.motion.medium == Duration{});  // 基线非零档，否则②的短路无从证明

    // ① 字号缩放三档：字阶正文档与两枚**非文本**度量族同倍增长。
    //    口径出处是 Apple `@ScaledMetric` / Compose `sp` 的那条共识——度量不跟着长就是溢出硬失败，
    //    属可达性而非观感；且只允许在物化点乘这一次，故这里用「同帧倍数」而非绝对磅值断言
    //    （绝对磅值随进程探测到的风格档变，跨机器不成立）。
    //    次序取 2 → 3 → 1 而非 1 → 2 → 3：换档检测比的是**相邻两帧**的输入，从 1.0 起步的第一步
    //    与基线同值、本就不该出一帧（那是④的判据），放在最后一步恰好既覆盖 1.0 档又把基准复原给②。
    for (const float scale : {2.0F, 3.0F, 1.0F}) {
        set_accessibility_settings(AccessibilitySettings{.font_scale = scale});
        AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
        // 换档帧不得被 idle 跳过：跳过的话子树停在旧读数，「开关置位」在真实帧循环里等于没生效。
        AURORA_TEST_CHECK_FALSE(w.is_idle_frame());
        const Theme &seen = *probe->seen();
        AURORA_TEST_CHECK_NEAR(seen.type_scale.body_medium.size_pt, neutral.type_scale.body_medium.size_pt * scale,
                               1e-3F);
        AURORA_TEST_CHECK_NEAR(seen.spacing.medium, neutral.spacing.medium * scale, 1e-3F);
        AURORA_TEST_CHECK_NEAR(seen.shapes.small, neutral.shapes.small * scale, 1e-3F);
        // 两条通道缺陷的直接反证：图表读的是 `inherit_theme(ctx).type_scale.body_medium`，
        // `Text` 未表态字号的回落点是同一枚令牌——同一 ctx 下两者必须给出同一个数。
        AURORA_TEST_CHECK_NEAR(Text{std::string{"x"}}.font_size(require_value(probe->ctx())),
                               seen.type_scale.body_medium.size_pt, 1e-4F);
    }

    // ② 减动效：时长档整族取零档（短路门不是倍率），且折叠不串档——字阶仍回到基线。
    set_accessibility_settings(AccessibilitySettings{.reduce_motion = true});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_FALSE(w.is_idle_frame());
    const Theme &reduced = *probe->seen();
    AURORA_TEST_CHECK(reduced.motion.none == Duration{});
    AURORA_TEST_CHECK(reduced.motion.extra_small == Duration{});
    AURORA_TEST_CHECK(reduced.motion.small == Duration{});
    AURORA_TEST_CHECK(reduced.motion.medium == Duration{});
    AURORA_TEST_CHECK(reduced.motion.large == Duration{});
    AURORA_TEST_CHECK(reduced.motion.extra_large == Duration{});
    AURORA_TEST_CHECK_NEAR(reduced.type_scale.body_medium.size_pt, neutral.type_scale.body_medium.size_pt, 1e-4F);
    // 撤开关即复原到逐字段全等的快照：折算必须是输入的纯函数（否则「同值覆写不重排」判据不成立）。
    set_accessibility_settings(AccessibilitySettings{});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK(*probe->seen() == neutral);

    // ③ 高对比：本档锁的是**通道**——该位参与解算键，置位即出一帧并重新注入快照。
    //    生效值本身的成对断言在无障碍单测里（那里人为造一枚不达线的登记行）：内置色列本就达线，
    //    「折叠后必须改变」按风格档与本机探测结果而定，跨机器不成立，写在这里只会成为空转断言。
    set_accessibility_settings(AccessibilitySettings{.high_contrast = true});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_FALSE(w.is_idle_frame());
    set_accessibility_settings(AccessibilitySettings{});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK(*probe->seen() == neutral);

    // ④ 不入键的位不构成换档：读屏在线不改任何主题取值，也不该为此出一帧。
    set_accessibility_settings(AccessibilitySettings{.screen_reader_active = true});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(w.present_root(page)));
    AURORA_TEST_CHECK_TRUE(w.is_idle_frame());
    AURORA_TEST_CHECK(*probe->seen() == neutral);
}

}  // namespace aurora::test_cases::utest_window
