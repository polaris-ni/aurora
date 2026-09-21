/// 测试类型: unit
/// 目标单元: include/aurora/widget/scroll.h
/// 测试说明: 覆盖 Scroll——构造默认值与自描述、内容小于视口不滚动、内容溢出时视口取父约束且内容宽被钳制、
/// scroll_by 方向与 step 乘子、偏移钳制、程序化 set_offset 的夹取与语义（越窗跳转仍取到正确内容带）、
/// 无子项退化、初始化列表取首项、step 序列化往返、滚轮余量回传（嵌套滚动协调）、
/// snap/paging 收位短滑动（含 Center 对齐与半页回弹）、reduce-motion 直落端点、
/// scroll_to 的即时/动画/夹取语义、offset_signal 随各通道发布、snap 三属性自描述与序列化往返

#include <chrono>
#include <memory>
#include <string>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/scroll.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scroll {

namespace {

/// 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

auto box(float w, float h) -> Node { return Node{std::make_shared<FixedBox>(w, h)}; }

/// @brief 绘制四条等高横向色带的固定盒子（上→下：红/绿/蓝/黄），用于观测滚动后取到的内容区域。
class BandBox final : public Widget {
  public:
    BandBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "BandBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter& p, const Rect& bounds, const BuildContext& /*ctx*/) -> void override {
        const float band = bounds.size.height / 4.0F;
        const Color colors[4] = {Color{255, 0, 0, 255}, Color{0, 255, 0, 255}, Color{0, 0, 255, 255},
                                 Color{255, 255, 0, 255}};
        for (int i = 0; i < 4; ++i) {
            p.fill_rect(
                Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + (band * static_cast<float>(i))},
                     .size = Size{.width = bounds.size.width, .height = band}},
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): 条带序号循环取值，上界由条带数约束
                colors[i]);
        }
    }

  private:
    float w_;
    float h_;
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto find_prop(const WidgetDescriptor& d, const char* name) -> const PropDescriptor* {
    for (const auto& p : d.properties) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

/// @brief 假想帧钟：单调递增，令自驱动的收位滑动逐帧可测（不依赖墙钟抖动）。
auto frame_clock() -> std::chrono::steady_clock::time_point& {
    static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{};
    return now;
}

/// @brief 推进 n 帧（每帧 16ms），驱动 snap 收位 / scroll_to 短滑动。
auto pump(Scroll& s, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        s.tick(frame_clock());
    }
}

/// @brief 等滑动走完：滑满时长 150ms + 余量。
auto settle(Scroll& s) -> void { pump(s, 16); }

/// @brief reduce-motion 守卫：作用域内开启，离开时复原进程级设置（单例，测试须自清）。
class ReduceMotionGuard final {
  public:
    ReduceMotionGuard() : saved_(current_accessibility_settings()) {
        AccessibilitySettings s = saved_;
        s.reduce_motion = true;
        set_accessibility_settings(s);
    }
    ~ReduceMotionGuard() { set_accessibility_settings(saved_); }
    ReduceMotionGuard(const ReduceMotionGuard&) = delete;
    auto operator=(const ReduceMotionGuard&) -> ReduceMotionGuard& = delete;
    ReduceMotionGuard(ReduceMotionGuard&&) = delete;
    auto operator=(ReduceMotionGuard&&) -> ReduceMotionGuard& = delete;

  private:
    AccessibilitySettings saved_;
};

}  // namespace

AURORA_TEST_CASE(set_offset_clamps_and_reports_change) {
    Scroll s;
    s.add(box(100.0F, 400.0F));
    LayoutEngine::layout(s, bounded(100.0F, 100.0F));

    AURORA_TEST_CHECK_TRUE(s.set_offset(120.0F));
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 120.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(s.set_offset(120.0F));  // 同值：无变化

    s.set_offset(-50.0F);  // 负值夹到 0
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
    s.set_offset(99999.0F);  // 超出内容：夹到 max = 400 - 100
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 300.0F, 1e-4F);

    // 未布局过（内容/视口未定）：夹到 0 且不报告变化——调用方（restore_key 恢复）须等首次可滚动布局。
    Scroll fresh_scroll;
    fresh_scroll.add(box(100.0F, 400.0F));
    AURORA_TEST_CHECK_FALSE(fresh_scroll.set_offset(150.0F));
    AURORA_TEST_CHECK_NEAR(fresh_scroll.offset_y(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(set_offset_jump_composites_correct_content_band) {
    // 越窗大跳不依赖 content_valid_ 失效：on_paint 的 !in_buffer → reanchor 分支重录后合成到正确内容区域。
    Scroll s;
    s.add(Node{std::make_shared<BandBox>(100.0F, 400.0F)});
    LayoutEngine::layout(s, bounded(100.0F, 100.0F));

    constexpr BuildContext ctx;
    const Rect view{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 100.0F}};

    Painter p;
    p.begin(100, 100);
    s.paint(p, view, ctx);
    const Color top = p.get_pixel(50, 50);  // 内容 y≈50 → 第 1 条带（红）
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.r), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.g), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(top.b), 0);

    AURORA_TEST_CHECK_TRUE(s.set_offset(300.0F));  // 跳到缓冲窗口之外（第 4 条带）
    p.begin(100, 100);  // 重新起帧
    s.paint(p, view, ctx);
    const Color jumped = p.get_pixel(50, 50);  // 内容 y≈350 → 第 4 条带（黄）
    AURORA_TEST_CHECK_EQ(static_cast<int>(jumped.r), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(jumped.g), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(jumped.b), 0);
}

AURORA_TEST_CASE(restore_key_restores_offset_on_first_scrollable_layout) {
    auto& storage = ScrollStorage::instance();
    storage.clear_all();

    // 第一次「会话」：滚动后位置写入注册表。
    {
        Scroll s;
        s.restore_key = "demo.feed";
        s.add(box(100.0F, 400.0F));
        LayoutEngine::layout(s, bounded(100.0F, 100.0F));
        AURORA_TEST_CHECK_TRUE(s.set_offset(200.0F));
        AURORA_TEST_CHECK_NEAR(storage.read("demo.feed").value_or(-1.0F), 200.0F, 1e-4F);
    }

    // 重建同键控件（新实例）：首次可滚动布局即恢复，无需外部介入。
    {
        Scroll s;
        s.restore_key = "demo.feed";
        s.add(box(100.0F, 400.0F));
        AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
        LayoutEngine::layout(s, bounded(100.0F, 100.0F));
        AURORA_TEST_CHECK_NEAR(s.offset_y(), 200.0F, 1e-4F);
    }

    // 无键：不参与恢复（偏移保持 0，注册表也不被写入）。
    {
        Scroll s;
        s.add(box(100.0F, 400.0F));
        LayoutEngine::layout(s, bounded(100.0F, 100.0F));
        s.set_offset(90.0F);
        AURORA_TEST_CHECK_NEAR(s.offset_y(), 90.0F, 1e-4F);
        AURORA_TEST_CHECK_NEAR(storage.read("demo.feed").value_or(-1.0F), 200.0F, 1e-4F);
    }
    storage.clear_all();
}

AURORA_TEST_CASE(serialized_offset_round_trips_and_beats_restore_key) {
    auto& storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("k", 250.0F);  // 注册表内已有记录

    Scroll restored;
    restored.restore_key = "k";
    restored.add(box(100.0F, 400.0F));
    LayoutEngine::layout(restored, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(restored.offset_y(), 250.0F, 1e-4F);  // 键恢复

    Json props;
    restored.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["offset"].get<float>(), 250.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(props["restore_key"].get<std::string>(), std::string{"k"});

    // 显式反序列化的 offset 优先于键恢复。
    Scroll explicit_scroll;
    explicit_scroll.restore_key = "k";
    explicit_scroll.add(box(100.0F, 400.0F));
    Json patch;
    patch["offset"] = 30.0F;
    explicit_scroll.deserialize_props(patch);
    LayoutEngine::layout(explicit_scroll, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(explicit_scroll.offset_y(), 30.0F, 1e-4F);
    storage.clear_all();
}

AURORA_TEST_CASE(default_scroll_invariants) {
    Scroll s;
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Scroll");
    AURORA_TEST_CHECK_NEAR(s.step, 16.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.overscan, 1.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
    // Scroll 自管离屏内容缓冲，禁用框架 DL 缓存。
    AURORA_TEST_CHECK_FALSE(s.can_cache_display_list());

    const auto d = Scroll::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Scroll");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
    const PropDescriptor* step = find_prop(d, "step");
    AURORA_TEST_REQUIRE_NOT_NULL(step);
    AURORA_TEST_CHECK_EQ(std::string{step->type}, "float");
    AURORA_TEST_CHECK_EQ(std::string{step->default_value}, "16.0");
    AURORA_TEST_CHECK_EQ(std::string{s.describe().name}, "Scroll");
}

AURORA_TEST_CASE(content_smaller_than_viewport_never_scrolls) {
    // 内容 100 < 视口 200：尺寸取视口，任意方向滚动都停在 0。
    Scroll s{ScrollProps{.child = box(300.0F, 100.0F)}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 200.0F, 1e-4F);
    s.scroll_by(-50.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
    s.scroll_by(50.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(overflowing_content_takes_viewport_and_clamps_width) {
    // 内容自然宽 1000 被内容约束（min width = 视口宽）钳到 300，高度 800 不受视口限制。
    Scroll s{ScrollProps{.child = box(1000.0F, 800.0F)}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 200.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_by_direction_and_step_multiplier) {
    // delta_y 为负（向下滚动）时 offset 增大 |delta|*step；正值（向上滚动）减小。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 10.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));
    s.scroll_by(-30.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 300.0F, 1e-4F);
    s.scroll_by(5.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 250.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_offset_clamps_to_content_range) {
    // 可滚范围 = 内容高 800 - 视口高 200 = 600，双向越界均被钳制。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));
    s.scroll_by(-1000.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 600.0F, 1e-4F);
    s.scroll_by(1000.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(empty_scroll_layouts_to_viewport) {
    Scroll s;
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 200.0F, 1e-4F);
    s.scroll_by(-10.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
    // 无子项时布局可缓存（子项检查短路为 true）。
    AURORA_TEST_CHECK_TRUE(s.can_cache_layout());
}

AURORA_TEST_CASE(initializer_list_takes_first_child_only) {
    Scroll s{box(100.0F, 10.0F), box(200.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(s.child_nodes().size(), 1U);
    LayoutEngine::layout(s, bounded(300.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(step_serialization_roundtrip) {
    Json props;
    Scroll{}.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["step"].get<float>(), 16.0F, 1e-4F);

    Scroll src;
    src.step = 24.0F;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["step"].get<float>(), 24.0F, 1e-4F);

    // 反序列化出的 step 参与滚动计算：step=24 时滚一单位位移 24px。
    Scroll dst{ScrollProps{.child = box(300.0F, 800.0F)}};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.step, 24.0F, 1e-4F);
    LayoutEngine::layout(dst, bounded(300.0F, 200.0F));
    dst.scroll_by(-1.0F);
    AURORA_TEST_CHECK_NEAR(dst.offset_y(), 24.0F, 1e-4F);
}

AURORA_TEST_CASE(wheel_margin_bubbles_up_when_clamped_at_edges) {
    // 嵌套滚动协调契约：端点被夹掉的量以 remaining_y 回传（保留符号），由派发器交给更浅层祖先。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));  // 可滚范围 [0, 600]

    ScrollEvent at_top;
    at_top.delta_y = 50.0F;  // 已在顶部向上滚：全量退为余量
    s.on_scroll(at_top);
    AURORA_TEST_CHECK_TRUE(at_top.is_handled);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(at_top.remaining_y, 50.0F, 1e-4F);

    ScrollEvent overrun;
    overrun.delta_y = -1000.0F;  // 向下滚过头：吃掉 600，余 -400
    s.on_scroll(overrun);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 600.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(overrun.remaining_y, -400.0F, 1e-4F);

    ScrollEvent consumed;
    consumed.delta_y = 100.0F;  // 中途可全量消化：无余量上冒
    s.on_scroll(consumed);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 500.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(consumed.remaining_y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_extent_glides_to_nearest_boundary_after_wheel) {
    // extent=200 / step=1：滚到 120 后不瞬间跳变，而是 150ms 短滑动收位到 200。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F, .snap = ScrollSnap{.extent = 200.0F}}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));

    s.scroll_by(-120.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 120.0F, 1e-4F);  // 跟手位：收位前仍为原始夹取值
    AURORA_TEST_CHECK_TRUE(s.is_gliding());

    pump(s, 1);  // easeOutCubic 首帧：仍严格处于 (120, 200) 之间
    AURORA_TEST_CHECK_TRUE(s.offset_y() > 120.0F && s.offset_y() < 200.0F);

    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 200.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(s.is_gliding());

    // 已过中线则向前吸附：400 恰为对齐点（而非退回 200）。
    s.set_offset(350.0F);
    AURORA_TEST_CHECK_FALSE(s.is_gliding());  // 程序化跳转作废滑动，且不经 snap 路径
    s.scroll_by(-50.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 400.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(s.is_gliding());  // 已在对齐点：无需收位
    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 400.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_center_alignment_targets_entry_centre) {
    // Center：条目 k 中心贴视口中心 → offset = k·ext + ext/2 − viewport/2 = 120 + 60 − 100 = 80。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F),
                         .step = 1.0F,
                         .snap = ScrollSnap{.extent = 120.0F, .alignment = ScrollSnapAlignment::Center}}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));
    s.scroll_by(-50.0F);
    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 80.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_paging_snaps_back_when_less_than_half_page) {
    // 分页：周期取视口高 300（内容 800 → 可滚 [0, 500]）。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F, .snap = ScrollSnap::page()}};
    LayoutEngine::layout(s, bounded(300.0F, 300.0F));

    s.scroll_by(-200.0F);  // 越过半页（150）→ 进下一页
    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 300.0F, 1e-4F);

    s.scroll_by(-100.0F);  // 仅 400，未过 300/600 的中线 → 回弹本页
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 400.0F, 1e-4F);
    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 300.0F, 1e-4F);

    // 末端不足一页时以可达性优先：夹到 max_off 而非强求整页。
    s.scroll_by(-500.0F);
    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 500.0F, 1e-4F);
}

AURORA_TEST_CASE(reduce_motion_snaps_directly_without_intermediate_frames) {
    ReduceMotionGuard guard;
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F, .snap = ScrollSnap{.extent = 200.0F}}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));

    s.scroll_by(-120.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 200.0F, 1e-4F);  // 直落端点：状态与走完一致
    AURORA_TEST_CHECK_FALSE(s.is_gliding());  // 且不产生中间帧

    s.scroll_to(0.0F);  // scroll_to 的 animate=true 同样短路
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(s.is_gliding());
}

AURORA_TEST_CASE(scroll_to_supports_instant_animated_and_clamped) {
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));  // 可滚 [0, 600]

    AURORA_TEST_CHECK_TRUE(s.scroll_to(500.0F, false));  // 即时：无滑动、当场就位
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 500.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(s.is_gliding());
    AURORA_TEST_CHECK_FALSE(s.scroll_to(500.0F, false));  // 同值：不动

    AURORA_TEST_CHECK_TRUE(s.scroll_to(99999.0F, false));  // 越界夹到末端
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 600.0F, 1e-4F);

    AURORA_TEST_CHECK_TRUE(s.scroll_to(300.0F));  // 缺省 animate：先起滑动，位置待逐帧推进
    AURORA_TEST_CHECK_TRUE(s.is_gliding());
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 600.0F, 1e-4F);
    settle(s);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 300.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(s.is_gliding());
}

AURORA_TEST_CASE(offset_signal_publishes_every_scroll_channel) {
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F), .step = 1.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 200.0F));

    SignalView<float>& offset = s.offset_signal();  // 懒创建：初值取当前偏移
    AURORA_TEST_CHECK_NEAR(offset.get(), 0.0F, 1e-4F);

    s.set_offset(80.0F);
    AURORA_TEST_CHECK_NEAR(offset.get(), 80.0F, 1e-4F);
    s.scroll_by(-40.0F);
    AURORA_TEST_CHECK_NEAR(offset.get(), 120.0F, 1e-4F);

    s.scroll_to(0.0F);  // 滑动期逐帧发布（滚动驱动动画靠中间帧才有视差）
    pump(s, 1);
    AURORA_TEST_CHECK_TRUE(offset.get() < 120.0F && offset.get() > 0.0F);
    settle(s);
    AURORA_TEST_CHECK_NEAR(offset.get(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_properties_describe_and_round_trip) {
    const auto d = Scroll::describe_static();
    const PropDescriptor* ext = find_prop(d, "snap_extent");
    AURORA_TEST_REQUIRE_NOT_NULL(ext);
    AURORA_TEST_CHECK_EQ(std::string{ext->type}, "float");
    AURORA_TEST_CHECK_EQ(std::string{ext->default_value}, "0.0");
    const PropDescriptor* paging = find_prop(d, "snap_paging");
    AURORA_TEST_REQUIRE_NOT_NULL(paging);
    AURORA_TEST_CHECK_EQ(std::string{paging->type}, "bool");
    const PropDescriptor* align = find_prop(d, "snap_alignment");
    AURORA_TEST_REQUIRE_NOT_NULL(align);
    AURORA_TEST_CHECK_EQ(std::string{align->type}, "ScrollSnapAlignment");
    AURORA_TEST_CHECK_EQ(align->enum_values.size(), 3U);
    AURORA_TEST_CHECK_EQ(align->enum_values.front(), std::string{"Start"});
    AURORA_TEST_CHECK_EQ(align->enum_values.back(), std::string{"End"});

    Scroll src{ScrollProps{.child = box(300.0F, 800.0F),
                           .step = 1.0F,
                           .snap = ScrollSnap{.extent = 120.0F, .alignment = ScrollSnapAlignment::Center}}};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["snap_extent"].get<float>(), 120.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(props["snap_paging"].get<bool>());
    AURORA_TEST_CHECK_EQ(props["snap_alignment"].get<std::string>(), std::string{"Center"});

    // 反序列化只还原属性、不还原子树：目标实例自带同尺寸内容，才能检验 snap 是否真生效。
    Scroll dst{ScrollProps{.child = box(300.0F, 800.0F)}};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.snap.extent, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(dst.snap.paging);
    AURORA_TEST_CHECK_EQ(static_cast<int>(dst.snap.alignment), static_cast<int>(ScrollSnapAlignment::Center));

    // 往返后的 snap 真实参与收位（Center 目标 80，同 snap_center_alignment 口径）。
    LayoutEngine::layout(dst, bounded(300.0F, 200.0F));
    dst.scroll_by(-50.0F);
    AURORA_TEST_CHECK_NEAR(dst.offset_y(), 50.0F, 1e-4F);  // step 亦随序列化恢复为 1
    settle(dst);
    AURORA_TEST_CHECK_NEAR(dst.offset_y(), 80.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_scroll
