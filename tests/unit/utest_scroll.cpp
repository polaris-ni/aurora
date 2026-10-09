/// 测试类型: unit
/// 目标单元: include/aurora/widget/scroll.h
/// 测试说明: 覆盖 Scroll——构造默认值与自描述、内容小于视口不滚动、内容溢出时视口取父约束且内容宽被钳制、
/// scroll_by 方向与 step 乘子、偏移钳制、程序化 set_offset 的夹取与语义（越窗跳转仍取到正确内容带）、
/// 无子项退化、初始化列表取首项、step 序列化往返、滚轮余量回传（嵌套滚动协调）、
/// snap/paging 收位短滑动（含 Center 对齐与半页回弹）、reduce-motion 直落端点、
/// scroll_to 的即时/动画/夹取语义、offset_signal 随各通道发布、snap 三属性自描述与序列化往返，
/// 以及 scroll_regression 段（计数类门槛 G-5 至 G-8，阈值取自 tools/check/perf_gates.json，
/// 仅在 AURORA_ENABLE_PROFILING=ON 时生效，否则注册为 skip）

#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/perf/counters.h"
#include "aurora/perf/scroll_bench.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/scroll.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "paths.h"

namespace aurora::test_cases::utest_scroll {
using aurora::testing::require_field;
using aurora::testing::require_value;

namespace {

/// 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

auto box(float w, float h) -> Node { return Node{std::make_shared<FixedBox>(w, h)}; }

/// @brief 绘制四条等高横向色带的固定盒子（上→下：红/绿/蓝/黄），用于观测滚动后取到的内容区域。
class BandBox final : public Widget {
  public:
    BandBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "BandBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
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

auto find_prop(const WidgetDescriptor &d, const char *name) -> const PropDescriptor * {
    for (const auto &p : d.properties) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

/// @brief 假想帧钟：单调递增，令自驱动的收位滑动逐帧可测（不依赖墙钟抖动）。
auto frame_clock() -> std::chrono::steady_clock::time_point & {
    static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{};
    return now;
}

/// @brief 推进 n 帧（每帧 16ms），驱动 snap 收位 / scroll_to 短滑动。
auto pump(Scroll &s, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        s.tick(frame_clock());
    }
}

/// @brief 等滑动走完：滑满时长 150ms + 余量。
auto settle(Scroll &s) -> void { pump(s, 16); }

/// @brief reduce-motion 守卫：作用域内开启，离开时复原进程级设置（单例，测试须自清）。
class ReduceMotionGuard final {
  public:
    ReduceMotionGuard() : saved_(current_accessibility_settings()) {
        AccessibilitySettings s = saved_;
        s.reduce_motion = true;
        set_accessibility_settings(s);
    }
    ~ReduceMotionGuard() { set_accessibility_settings(saved_); }
    ReduceMotionGuard(const ReduceMotionGuard &) = delete;
    auto operator=(const ReduceMotionGuard &) -> ReduceMotionGuard & = delete;
    ReduceMotionGuard(ReduceMotionGuard &&) = delete;
    auto operator=(ReduceMotionGuard &&) -> ReduceMotionGuard & = delete;

  private:
    AccessibilitySettings saved_;
};

/// @brief scroll_regression 的固定内容树：Scroll 套 40 行 300x40 哑控件（内容 1600dp、视口 200dp = 8 屏）。
///
/// 刻意用节点数已知的浅树：计数门槛要的是「整树重排 / 整帧重绘一旦发生就跳变明显」的形状，
/// 不是逼真视觉。40 个内容节点下，逐帧重排会把 `layout_nodes` 从个位推到 40 以上。
auto make_regression_tree() -> Node {
    std::vector<Node> rows;
    rows.reserve(40);
    for (int i = 0; i < 40; ++i) {
        rows.emplace_back(box(300.0F, 40.0F));
    }
    auto col = std::make_shared<Column>(ColumnProps{.children = std::move(rows)});
    ScrollProps props;
    props.child = Node{std::move(col)};
    return Node{std::make_shared<Scroll>(props)};
}

/// @brief 取 `tools/check/perf_gates.json` 中指定 id 的门槛阈值。
///
/// 该文件是「有哪些门槛、门槛多少」的唯一登记处，本段不重复声明数值；缺条目即致命失败，
/// 否则登记册被删空后断言会静默变成空转。
auto gate_threshold(const char *id) -> double {
    const std::string path = aurora::testing::paths::under_repo("tools/check/perf_gates.json");
    std::ifstream in(path, std::ios::binary);
    AURORA_TEST_REQUIRE_MSG(in.good(), "perf_gates.json must be readable: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto parsed = json::parse(ss.str());
    AURORA_TEST_REQUIRE_MSG(parsed.ok(), std::string{"perf_gates.json must parse: "} + path);
    const Json &cfg = parsed.value();
    AURORA_TEST_REQUIRE_MSG(cfg.contains("gates") && cfg.at("gates")->is_array(),
                            std::string{"perf_gates.json must carry a gates array: "} + path);
    const auto &gates = *cfg.at("gates");
    for (const auto &g : gates) {
        if (g.contains("id") && g.at("id")->as_or<std::string>("") == id) {
            AURORA_TEST_REQUIRE_MSG(g.contains("threshold"), std::string{"gate "} + id + " has no threshold");
            return g.at("threshold")->as_or<double>(0.0);
        }
    }
    AURORA_TEST_REQUIRE_MSG(false, std::string{"gate "} + id + " is not declared in perf_gates.json");
    return 0.0;
}

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
    auto &storage = ScrollStorage::instance();
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
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("k", 250.0F);  // 注册表内已有记录

    Scroll restored;
    restored.restore_key = "k";
    restored.add(box(100.0F, 400.0F));
    LayoutEngine::layout(restored, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(restored.offset_y(), 250.0F, 1e-4F);  // 键恢复

    Json props = Json::object();
    restored.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "offset"), 250.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "restore_key"), std::string{"k"});

    // 显式反序列化的 offset 优先于键恢复。
    Scroll explicit_scroll;
    explicit_scroll.restore_key = "k";
    explicit_scroll.add(box(100.0F, 400.0F));
    Json patch = Json::object();
    patch.set("offset", 30.0F);
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
    const PropDescriptor *step = find_prop(d, "step");
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
    Json props = Json::object();
    Scroll{}.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "step"), 16.0F, 1e-4F);

    Scroll src;
    src.step = 24.0F;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "step"), 24.0F, 1e-4F);

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

    SignalView<float> &offset = s.offset_signal();  // 懒创建：初值取当前偏移
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
    const PropDescriptor *ext = find_prop(d, "snap_extent");
    AURORA_TEST_REQUIRE_NOT_NULL(ext);
    AURORA_TEST_CHECK_EQ(std::string{ext->type}, "float");
    AURORA_TEST_CHECK_EQ(std::string{ext->default_value}, "0.0");
    const PropDescriptor *paging = find_prop(d, "snap_paging");
    AURORA_TEST_REQUIRE_NOT_NULL(paging);
    AURORA_TEST_CHECK_EQ(std::string{paging->type}, "bool");
    const PropDescriptor *align = find_prop(d, "snap_alignment");
    AURORA_TEST_REQUIRE_NOT_NULL(align);
    AURORA_TEST_CHECK_EQ(std::string{align->type}, "ScrollSnapAlignment");
    AURORA_TEST_CHECK_EQ(align->enum_values.size(), 3U);
    AURORA_TEST_CHECK_EQ(align->enum_values.front(), std::string{"Start"});
    AURORA_TEST_CHECK_EQ(align->enum_values.back(), std::string{"End"});

    Scroll src{ScrollProps{.child = box(300.0F, 800.0F),
                           .step = 1.0F,
                           .snap = ScrollSnap{.extent = 120.0F, .alignment = ScrollSnapAlignment::Center}}};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "snap_extent"), 120.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(require_field<bool>(props, "snap_paging"));
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "snap_alignment"), std::string{"Center"});

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

// ---- scroll_regression 段：计数类门槛 G-5 至 G-8 ----
//
// 分工：时间类门槛（G-1 至 G-4）随机器负载漂移，只作本地趋势对照、不进 CI；计数类在
// Headless 下**逐帧可复现**，因此锁定在这里。阈值不在本文件复写，一律经 `gate_threshold()`
// 从 `tools/check/perf_gates.json` 读取——登记册是唯一权威来源，删条目即红灯，杜绝
// 「门槛悄悄蒸发而断言还在假装通过」。
AURORA_TEST_CASE(scroll_regression_counter_gates) {
    // 埋点宏在 AURORA_ENABLE_PROFILING 关闭时展开为空操作，读数恒为 0——此时比对阈值会
    // 「假通过」，故显式 skip。CI 由 profiling-tracing-debug-on 作业（PROFILING=ON）实跑本段。
    if (!profiling_enabled()) {
        AURORA_TEST_SKIP("RenderCounters compiled out: needs -DAURORA_ENABLE_PROFILING=ON");
    }

    ScrollBenchHarness::Config cfg;
    cfg.frames = 60;
    cfg.warmup_frames = 5;
    cfg.settle_ms = 0.0;  // 静态树无首屏瞬态可等：关掉落定，帧数与计数完全确定
    cfg.name = "scroll_regression";
    const ScrollBenchHarness::Result r =
        ScrollBenchHarness::run(make_regression_tree(), Size{.width = 300.0F, .height = 200.0F}, cfg);

    // 先验伪再看数：读数不成立时门槛断言毫无意义，直接致命失败而非静默比对。
    AURORA_TEST_REQUIRE_TRUE(r.trustworthy());
    AURORA_TEST_REQUIRE_EQ(r.report.frame_count, std::size_t{60});
    // 再证明「这 60 帧真的在画」：若场景因故空转，四项计数会全部归零而门槛照样绿灯，
    // 那正是门槛类断言最隐蔽的失效形态。绘制原语计数不归零是本段的生效前提。
    const auto &peak = r.counters_max();
    AURORA_TEST_REQUIRE_GT(peak.paint_nodes, std::uint32_t{0});
    AURORA_TEST_REQUIRE_GT(peak.draw_calls, std::uint32_t{0});
    AURORA_TEST_REQUIRE_GT(peak.pixels_filled, std::uint64_t{0});

    const auto layout_max = static_cast<double>(peak.layout_nodes);
    const auto dl_records_max = static_cast<double>(peak.dl_records);
    const auto full_redraw_frames = static_cast<double>(r.full_redraw_frames());
    const auto dirty_rects_max = static_cast<double>(peak.dirty_rect_count);
    AURORA_TEST_PRINTF(
        "scroll_regression G-5..G-8 readings: layout_nodes_max=%.0f dl_records_max=%.0f "
        "full_redraw_frames=%.0f dirty_rect_count_max=%.0f | max=%s\n",
        layout_max, dl_records_max, full_redraw_frames, dirty_rects_max, peak.to_json().c_str());

    // G-5：滚动不得逐帧重排——整树 41 个节点，一旦逐帧重排读数就从 0 跳到 41。
    AURORA_TEST_CHECK_LE(layout_max, gate_threshold("G-5"));
    // G-6：DisplayList 应回放既有列表，而非逐帧把子树重录一遍。
    AURORA_TEST_CHECK_LE(dl_records_max, gate_threshold("G-6"));
    // G-7：60 个采样帧里退化为整帧重绘的帧数。
    AURORA_TEST_CHECK_LE(full_redraw_frames, gate_threshold("G-7"));
    // G-8：一帧的脏区应合并成少数几块，而不是每个可见行一块。
    AURORA_TEST_CHECK_LE(dirty_rects_max, gate_threshold("G-8"));
}

// ---- 内容命中链：内容须可点，且命中点须随滚动偏移换算 ----
//
// 回归背景：修复前 Scroll 的 on_layout 从不调 Node::set_bounds，内容子树停留在默认零盒，
// 且未覆写 on_hit_test_chain ⇒ 命中链只剩 Scroll 自身，内容里的可交互控件一个都点不到。
//
// 探针取点纪律：命中点取**目标行自身盒的中心**（行高固定 40dp，换算是确定的），
// 断言的是「盒内 ⇒ 命中该行」这一关系，不硬编码绝对坐标。

namespace {

constexpr float AURORA_G27_ROW_H = 40.0F;  ///< 内容行高（dp）
constexpr float AURORA_G27_VIEW_W = 300.0F;  ///< 视口宽（dp）
constexpr float AURORA_G27_VIEW_H = 200.0F;  ///< 视口高（dp）＝ 5 行
constexpr int AURORA_G27_ROWS = 20;  ///< 内容行数（内容总高 800dp > 视口，可滚）
constexpr float AURORA_G27_CONTENT_H = static_cast<float>(AURORA_G27_ROWS) * AURORA_G27_ROW_H;

auto g27_viewport() -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                .size = Size{.width = AURORA_G27_VIEW_W, .height = AURORA_G27_VIEW_H}};
}

/// @brief 命中观测台账：记录累计点击数与最近一次被点的行号。
struct HitLedger {
    int clicks = 0;
    int last_index = -1;
};

/// @brief 可点击的内容行（叶控件）：固定行高，点击于 Release 记入共享台账。
class HitRow final : public Widget {
  public:
    HitRow(int index, HitLedger &ledger) : index_(index), ledger_(&ledger) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "HitRow"; }
    [[nodiscard]] auto index() const -> int { return index_; }
    [[nodiscard]] auto ledger() -> HitLedger & { return *ledger_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = AURORA_G27_VIEW_W, .height = AURORA_G27_ROW_H});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    /// @brief 点击记账入口：基类在「先按下、再抬起」且未构成拖拽时调用（见 Widget::activate）。
    /// 记账挂在此处而非 on_pointer_event —— 后者受 `wants_click()` 门控，不可靠。
    auto activate() -> void override {
        ++ledger_->clicks;
        ledger_->last_index = index_;
    }

  private:
    int index_;
    HitLedger *ledger_;  ///< 非拥有：台账由 fixture 持有，生命周期长于本控件
};

/// @brief 记录派发期收到的 `local_position` / `position` 的探针行（事件本地化观测用）。
///
/// 与 `HitRow` 分工：`HitRow` 只记「有没有被点中」（走 `activate`，受 `wants_click()` 门控），
/// 本类改从 `on_pointer_event` 直接取**事件自带坐标**，用于钉住「滚动容器内控件收到的本地坐标
/// 是否正确」。它不需要 clickable 修饰——`on_pointer_event` 是控件收到事件的入口。
class LocalProbeRow final : public Widget {
  public:
    explicit LocalProbeRow(int index) : index_(index) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "LocalProbeRow"; }
    [[nodiscard]] auto index() const -> int { return index_; }
    [[nodiscard]] auto seen() const -> bool { return seen_; }
    [[nodiscard]] auto last_local() const -> Point { return local_; }
    [[nodiscard]] auto last_position() const -> Point { return position_; }

    auto on_pointer_event(MouseEvent &e) -> void override {
        seen_ = true;
        local_ = e.local_position;
        position_ = e.position;
        e.is_handled = true;  // 只记不消费：避免打断同批用例的其它派发断言
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = AURORA_G27_VIEW_W, .height = AURORA_G27_ROW_H});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*r*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    int index_;
    bool seen_ = false;
    Point local_{};
    Point position_{};
};

/// @brief 构造「Scroll 套 Column」固定内容树（行控件为 `LocalProbeRow`），供事件本地化判据使用。
struct LocalProbeFixture {
    std::shared_ptr<Scroll> scroll;
    std::shared_ptr<Column> content;
    std::vector<std::shared_ptr<LocalProbeRow>> rows;
};

/// @brief 搭一棵带探针行的可滚 Scroll，并按给定偏移就位。
/// @param offset 布局后设置的滚动偏移（dp）。
/// @return 已完成布局的 fixture（行实例在布局期由 Column 写入 Node bounds）。
auto make_local_probe_scrollable(float offset) -> LocalProbeFixture {
    LocalProbeFixture f;
    f.scroll = std::make_shared<Scroll>();
    f.content = std::make_shared<Column>();
    for (int i = 0; i < AURORA_G27_ROWS; ++i) {
        auto r = std::make_shared<LocalProbeRow>(i);
        // 必须挂 Clickable 修饰：`wants_click()` 缺省只看修饰链，不挂则基类不认它为命中目标，
        // `on_pointer_event` 永不触发（探针自身接线错误的经典陷阱，与 make_scrollable 同因）。
        r->modifier.set(Modifier{}.clickable([]() -> void {}));
        f.rows.push_back(r);
        f.content->add(Node{r});
    }
    f.scroll->add(Node{f.content});
    LayoutEngine::layout(*f.scroll, bounded(AURORA_G27_VIEW_W, AURORA_G27_VIEW_H));
    if (offset > 0.0F) {
        (void)f.scroll->set_offset(offset);
    }
    return f;
}

/// @brief 构造「Scroll 套Column」固定内容树，并保留各行实例供断言取用。
struct ScrollFixture {
    std::shared_ptr<Scroll> scroll;
    std::shared_ptr<Column> content;
    std::vector<std::shared_ptr<HitRow>> rows;
    std::vector<std::unique_ptr<HitLedger>> ledgers;  ///< 每个行一份台账（行实例可能先于台账销毁）
};

/// @brief 按给定偏移搭好并布局一棵可滚的 Scroll 树。
/// @param offset 布局后立即设置的滚动偏移（dp）。
/// @return 已完成布局的 fixture（rows/ledgers 保持行实例与台账的对应关系）。
auto make_scrollable(float offset) -> ScrollFixture {
    ScrollFixture f;
    f.scroll = std::make_shared<Scroll>();
    f.content = std::make_shared<Column>();
    for (int i = 0; i < AURORA_G27_ROWS; ++i) {
        f.ledgers.push_back(std::make_unique<HitLedger>());
        auto row = std::make_shared<HitRow>(i, *f.ledgers.back());
        // 挂 Clickable 修饰：`wants_click()` 默认只看修饰链，缺它则基类不识别点击、
        // activate() 永不触发（探针自身接线错误的经典陷阱）。
        row->modifier.set(Modifier{}.clickable([]() -> void {}));
        f.rows.push_back(row);
        f.content->add(Node{std::move(row)});
    }
    f.scroll->add(Node{f.content});
    LayoutEngine::layout(*f.scroll, bounded(AURORA_G27_VIEW_W, AURORA_G27_VIEW_H));
    if (offset > 0.0F) {
        (void)f.scroll->set_offset(offset);
    }
    return f;
}

/// @brief 在视口内点一次（Press + Release 成对）。
auto click_at(Widget &root, float x, float y) -> void {
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = Point{.x = x, .y = y};
    EventDispatcher::dispatch(root, press, nullptr);
    MouseEvent release;
    release.action = MouseAction::Release;
    release.button = MouseButton::Left;
    release.position = Point{.x = x, .y = y};
    EventDispatcher::dispatch(root, release, nullptr);
}

}  // namespace

AURORA_TEST_CASE(content_bounds_written_in_content_coordinates) {
    // 回归点：布局必须把内容盒写入 children_[0]；且是**内容坐标**（原点 0,0、不含滚动偏移）。
    // 吸顶判据（natural_y >= offset_y_）依赖此前提，写成视口坐标会破坏它。
    ScrollFixture f = make_scrollable(200.0F);
    const std::vector<Node> &kids = f.scroll->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    const Rect cb = kids.front().bounds();
    // 修复前此盒为默认 Rect{}（宽高皆 0）。
    AURORA_TEST_CHECK_NEAR(cb.size.width, AURORA_G27_VIEW_W, 1e-3F);
    AURORA_TEST_CHECK_NEAR(cb.size.height, AURORA_G27_CONTENT_H, 1e-3F);
    AURORA_TEST_CHECK_NEAR(cb.origin.x, 0.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(cb.origin.y, 0.0F, 1e-3F);
}

AURORA_TEST_CASE(scrolled_content_still_hits_visual_row) {
    // 核心验收：滚到 offset=200 后点击「视觉上那一行」必须命中它。
    // 若命中链不做 `local.y + offset_y_` 换算，此处会命中错位一个滚动量的行。
    ScrollFixture f = make_scrollable(200.0F);
    AURORA_TEST_REQUIRE(f.scroll->set_offset(200.0F) || f.scroll->offset_y() == 200.0F);
    AURORA_TEST_CHECK_NEAR(f.scroll->offset_y(), 200.0F, 1e-3F);

    const float visual_y = 60.0F;  ///< 视口内 y=60（第 2 行的中心带）
    // 视口 y=60 + 偏移 200 = 内容 y=260 ⇒ 行号 floor(260 / 40) = 6。
    const int expected = static_cast<int>((200.0F + visual_y) / AURORA_G27_ROW_H);
    AURORA_TEST_CHECK_EQ(expected, 6);
    AURORA_TEST_REQUIRE(expected < static_cast<int>(f.rows.size()));

    const auto chain = f.scroll->hit_test_chain(Point{.x = 150.0F, .y = visual_y}, g27_viewport(), BuildContext{});
    // 回归点：修复前此处链为空（内容零盒）或最深是 Scroll 自身。
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    AURORA_TEST_CHECK_EQ(std::string{chain.back().ptr->type_name()}, std::string{"HitRow"});
    // 链尾即目标行实例本身（不只是类型名相同）。
    AURORA_TEST_CHECK_EQ(chain.back().ptr, static_cast<Widget *>(f.rows[static_cast<std::size_t>(expected)].get()));

    // 条目回调触发：目标行台账 +1，其余行不动。
    const int before = f.rows[static_cast<std::size_t>(expected)]->ledger().clicks;
    click_at(*f.scroll, 150.0F, visual_y);
    AURORA_TEST_CHECK_EQ(f.rows[static_cast<std::size_t>(expected)]->ledger().clicks, before + 1);
    AURORA_TEST_CHECK_EQ(f.rows[static_cast<std::size_t>(expected)]->ledger().last_index, expected);
    // 容器不吞点击：整棵树只有一个台账被记（各行独立台账，其余必须保持 0）。
    int touched = 0;
    for (const auto &l : f.ledgers) {
        if (l->clicks > 0) {
            ++touched;
        }
    }
    AURORA_TEST_CHECK_EQ(touched, 1);
}

AURORA_TEST_CASE(content_outside_viewport_is_not_hittable) {
    // 被 offset 推出可视区的内容不应命中（可视区裁剪）。
    ScrollFixture f = make_scrollable(400.0F);
    AURORA_TEST_CHECK_NEAR(f.scroll->offset_y(), 400.0F, 1e-3F);

    // 视口顶（y=0.5）对应内容 y=400.5 ⇒ 行号 10；行 9（y=360..400）已在视口之上，不得命中。
    auto chain = f.scroll->hit_test_chain(Point{.x = 150.0F, .y = 0.5F}, g27_viewport(), BuildContext{});
    AURORA_TEST_REQUIRE_FALSE(chain.empty());
    for (const auto &n : chain) {
        auto *row = dynamic_cast<HitRow *>(n.ptr);
        if (row != nullptr) {
            AURORA_TEST_CHECK(row->index() >= 10);
        }
    }
    // 视口下边界之外（视口高 200，探测 y=250）：即便换算后落在某行范围内，
    // 该行也已不在可视区 —— 命中链不得包含任何行。裁剪来源见 on_hit_test_chain 的视口相交闸门。
    const auto below = f.scroll->hit_test_chain(Point{.x = 150.0F, .y = 250.0F}, g27_viewport(), BuildContext{});
    for (const auto &n : below) {
        AURORA_TEST_CHECK(dynamic_cast<HitRow *>(n.ptr) == nullptr);
    }
    // 视口上边界之外（探测 y=-50）：同上，链中不得有行。
    const auto above = f.scroll->hit_test_chain(Point{.x = 150.0F, .y = -50.0F}, g27_viewport(), BuildContext{});
    for (const auto &n : above) {
        AURORA_TEST_CHECK(dynamic_cast<HitRow *>(n.ptr) == nullptr);
    }
    // 被推出可视区的行（下标 0 到 9）一次都点不到。步进用整型索引再换算坐标，
    // 避免以 float 作循环计数器（浮点累加不可靠，且 clang-tidy 会报 FloatLoopCounter）。
    const int probe_steps = 25;  ///< 25 × 8dp = 200dp，恰覆盖整个视口高
    const float probe_step = 8.0F;  ///< 探测步长（dp）
    for (int step = 0; step < probe_steps; ++step) {
        const float y = static_cast<float>(step) * probe_step;
        for (const auto &n : f.scroll->hit_test_chain(Point{.x = 150.0F, .y = y}, g27_viewport(), BuildContext{})) {
            auto *row = dynamic_cast<HitRow *>(n.ptr);
            if (row != nullptr) {
                AURORA_TEST_CHECK(row->index() >= 10);
            }
        }
    }
}

AURORA_TEST_CASE(container_stays_on_chain_behind_content) {
    // 容器自身须仍在链上（滚轮/拖拽需要它），但排在内容之后（派发自链尾向链头）。
    ScrollFixture f = make_scrollable(0.0F);
    const auto chain = f.scroll->hit_test_chain(Point{.x = 150.0F, .y = 60.0F}, g27_viewport(), BuildContext{});
    AURORA_TEST_REQUIRE(chain.size() >= 2U);
    AURORA_TEST_CHECK_EQ(chain.front().ptr, static_cast<Widget *>(f.scroll.get()));
    AURORA_TEST_CHECK_EQ(std::string{chain.back().ptr->type_name()}, std::string{"HitRow"});
    // 滚轮仍能路由到容器（wants_scroll 为真、自身在链上）。
    AURORA_TEST_CHECK(f.scroll->wants_scroll());
}

AURORA_TEST_CASE(positive_control_column_root_also_clickable) {
    // 正对照：同一行直接挂 Column（Column 写视口坐标 bounds）时同一派发路径可点。
    // 用于排除「探针接线错误判成通过」——若此对照红，问题在探针而非 Scroll。
    auto ledger = std::make_unique<HitLedger>();
    auto col = std::make_shared<Column>();
    auto row = std::make_shared<HitRow>(3, *ledger);
    row->modifier.set(Modifier{}.clickable([]() -> void {}));
    col->add(Node{std::move(row)});
    LayoutEngine::layout(*col, bounded(AURORA_G27_VIEW_W, 400.0F));

    const std::vector<Node> &kids = col->child_nodes();
    AURORA_TEST_REQUIRE(!kids.empty());
    const Rect bb = kids.front().bounds();
    AURORA_TEST_REQUIRE(bb.size.height > 0.0F);

    click_at(*col, bb.origin.x + 150.0F, bb.origin.y + (bb.size.height * 0.5F));
    AURORA_TEST_CHECK_EQ(ledger->clicks, 1);
    AURORA_TEST_CHECK_EQ(ledger->last_index, 3);
}

// ---- VerticalViewSize 数据源（Scroll） ----
AURORA_TEST_CASE(vertical_view_size_reports_visible_fraction_of_content) {
    // 判据①（数据源）：视口 100 / 内容 400 ⇒ 25%；与 LazyList 同源。
    Scroll s{ScrollProps{.child = box(300.0F, 400.0F)}};
    LayoutEngine::layout(s, bounded(300.0F, 100.0F));
    const auto range = require_value(s.accessibility_scroll());
    AURORA_TEST_CHECK_NEAR(range.viewport, 100.0, 1e-3);
    AURORA_TEST_CHECK_NEAR(range.content, 400.0, 1e-3);
    AURORA_TEST_CHECK_NEAR(aurora::compute_vertical_view_size(range), 25.0, 1e-6);
}

AURORA_TEST_CASE(vertical_view_size_and_scroll_percent_consistent) {
    // 判据③（数据源）：滚到中段时 percent 增大、viewsize 恒定（可见比例不随位置变）。
    Scroll s{ScrollProps{.child = box(300.0F, 800.0F)}};
    LayoutEngine::layout(s, bounded(300.0F, 100.0F));
    const auto top = require_value(s.accessibility_scroll());
    const double top_view = aurora::compute_vertical_view_size(top);
    const double top_pct = (top.position - top.min) / (top.max - top.min) * 100.0;
    AURORA_TEST_CHECK_NEAR(top_pct, 0.0, 1e-6);
    s.set_offset(350.0F);
    const auto mid = require_value(s.accessibility_scroll());
    const double mid_pct = (mid.position - mid.min) / (mid.max - mid.min) * 100.0;
    const double mid_view = aurora::compute_vertical_view_size(mid);
    AURORA_TEST_CHECK(mid_pct > 1.0);
    AURORA_TEST_CHECK_NEAR(mid_view, top_view, 1e-6);
}

// ---- 控件 -> 窗口逻辑 dp 绝对盒的事后查询（Widget::window_bounds） ----
//
// 背景：`paint_bounds()` 的注释曾承诺「绝对（窗口逻辑 dp）盒」，但 Scroll 把内容录进离屏缓冲时
// 给子树传的是 `{0, -buffer_origin_y_}`，故滚动容器内后代的该读数是**缓冲坐标**；消费侧要换算
// 成窗口坐标需要 `buffer_origin_y_`，而它既无 getter 也不进 serialize_props ⇒ 结构上无法折算。
// `window_bounds()` 是公共面上该换算的唯一入口。
//
// 判据纪律：**预期值一律由测试自己独立复算**（视口窗口原点 + 内容 y − offset_y_），不得取实现
// 自己的输出当基准，否则判据会跟着实现一起漂、变异打不红。

AURORA_TEST_CASE(window_bounds_of_scrolled_descendant_equals_independently_recomputed_origin) {
    // 形态①：Scroll 内容后代在**非零偏移**后取窗口盒，逐位等于独立复算的真窗口位。
    // 视口放在 y=60 的父容器里，确保「视口原点」真的进入读数（视口原点为 0 的形态会把该项漏掉）。
    constexpr float g36_viewport_origin_y = 60.0F;
    auto outer = std::make_shared<Column>();
    outer->add(Node{std::make_shared<FixedBox>(AURORA_G27_VIEW_W, g36_viewport_origin_y)});
    ScrollFixture f = make_scrollable(200.0F);
    outer->add(Node{f.scroll});

    // 按外层尺寸重排（60dp 占位 + 200dp 视口），使视口原点确实落在 y=60。
    LayoutEngine::layout(*outer, bounded(AURORA_G27_VIEW_W, AURORA_G27_VIEW_H + g36_viewport_origin_y));
    (void)f.scroll->set_offset(200.0F);
    const float offset = f.scroll->offset_y();
    AURORA_TEST_REQUIRE(offset > 0.0F);

    // 逐行核对：真窗口位 = 视口窗口原点 + 行内容 y − offset_y_。
    int checked = 0;
    for (std::size_t i = 0; i < f.rows.size(); ++i) {
        const float content_y = static_cast<float>(i) * AURORA_G27_ROW_H;
        if (!f.rows[i]->window_bounds().has_value()) {
            continue;
        }
        ++checked;
        const Rect wb = require_value(f.rows[i]->window_bounds());
        const float expected = g36_viewport_origin_y + content_y - offset;
        AURORA_TEST_CHECK_NEAR(wb.origin.y, expected, 1e-3F);
    }
    AURORA_TEST_CHECK(checked > 0);

    // 关键区分：缓冲锚点 `buffer_origin_y_` **不得**参与折算。若实现误把它加上/减掉，
    // 上面的逐行核对会整体错位一个锚点量而转红。
    // ⚠️ 本用例**不绘制**：故 `paint_bounds()` 此处是零盒（该读数是绘制期写入、有缓存缺口，
    // 见其 `@warning`）。这恰好说明为什么公共面需要 `window_bounds()` 这条不依赖绘制的查询路径。
    AURORA_TEST_CHECK(f.rows[5]->paint_bounds().size.height <= 0.0F);
}

AURORA_TEST_CASE(paint_bounds_of_scrolled_descendant_stays_in_buffer_coordinates) {
    // 本次只改 `paint_bounds()` 的注释口径、不改其行为，故在此钉住现状：绘制后该读数给的是
    // **内容坐标**（等于行的内容 y），不是窗口坐标——这正是改口的理由，也是与 `window_bounds()`
    // 并存两条读数的原因。两条读数之差 = offset_y_ − 视口窗口原点。
    constexpr float g36_viewport_origin_y = 60.0F;
    auto outer = std::make_shared<Column>();
    outer->add(Node{std::make_shared<FixedBox>(AURORA_G27_VIEW_W, g36_viewport_origin_y)});
    ScrollFixture f = make_scrollable(200.0F);
    outer->add(Node{f.scroll});
    LayoutEngine::layout(*outer, bounded(AURORA_G27_VIEW_W, AURORA_G27_VIEW_H + g36_viewport_origin_y));
    (void)f.scroll->set_offset(200.0F);

    Painter p;
    p.begin(400, 400);
    outer->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 400.0F, .height = 400.0F}},
                 BuildContext{});

    const float offset = f.scroll->offset_y();
    const Rect mid = require_value(f.rows[5]->window_bounds());
    AURORA_TEST_REQUIRE(f.rows[5]->paint_bounds().size.height > 0.0F);
    // 缓冲口径：绘制读数 == 内容 y（不含视口原点、也不含滚动偏移）。
    AURORA_TEST_CHECK_NEAR(f.rows[5]->paint_bounds().origin.y, 5.0F * AURORA_G27_ROW_H, 1e-3F);
    // 与窗口盒的差恰为「offset − 视口原点」，两条读数不可混用。
    AURORA_TEST_CHECK_NEAR(f.rows[5]->paint_bounds().origin.y - mid.origin.y, offset - g36_viewport_origin_y, 1e-3F);
}

AURORA_TEST_CASE(window_bounds_matches_recomputed_origin_without_scrolling) {
    // 形态②：未滚动 / 不在滚动容器内两种形态，窗口盒与独立复算值**容差 0** 逐位相等。
    // 「容差 0」守住缺省路径零变化：任何多减/少减一份平移都会在这里现形。

    // 形态②-a：不在滚动容器内 —— 纯 Column 树，偏移恒 0。
    auto col = std::make_shared<Column>();
    std::vector<std::shared_ptr<HitRow>> rows;
    for (int i = 0; i < 4; ++i) {
        auto r = std::make_shared<HitRow>(i, *std::make_unique<HitLedger>());
        r->modifier.set(Modifier{}.clickable([]() -> void {}));
        rows.push_back(r);
        col->add(Node{r});
    }
    LayoutEngine::layout(*col, bounded(AURORA_G27_VIEW_W, 400.0F));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Rect wb = require_value(rows[i]->window_bounds());
        const float content_y = static_cast<float>(i) * AURORA_G27_ROW_H;
        AURORA_TEST_CHECK(wb.origin.y == content_y);  // 容差 0：逐位相等
    }

    // 形态②-b：在Scroll 内但**未滚动**（offset==0）⇒ 窗口位 == 视口原点 + 内容 y。
    ScrollFixture f = make_scrollable(0.0F);
    AURORA_TEST_CHECK_NEAR(f.scroll->offset_y(), 0.0F, 1e-4F);
    for (std::size_t i = 0; i < f.rows.size(); ++i) {
        const Rect wb = require_value(f.rows[i]->window_bounds());
        const float content_y = static_cast<float>(i) * AURORA_G27_ROW_H;
        AURORA_TEST_CHECK(wb.origin.y == content_y);
    }
}

AURORA_TEST_CASE(window_bounds_accounts_for_modifier_translation) {
    // 同源性回归：中间层Column 带 padding(10) 时，窗口盒须含该内容盒平移，
    // 否则「origin 与绘制仿射同源」这条不变量在事后查询侧就断了（退化成朴素布局原点）。
    auto col = std::make_shared<Column>();
    col->modifier.set(Modifier{}.padding(10.0F));
    auto target = std::make_shared<HitRow>(0, *std::make_unique<HitLedger>());
    target->modifier.set(Modifier{}.clickable([]() -> void {}));
    col->add(Node{target});

    auto root = std::make_shared<Column>();
    root->add(Node{std::make_shared<HitRow>(0, *std::make_unique<HitLedger>())});  // 40dp 占位行
    root->add(Node{col});
    LayoutEngine::layout(*root, bounded(AURORA_G27_VIEW_W, 200.0F));

    const Rect wb = require_value(target->window_bounds());
    // 独立复算：占位行高 40（col 盒原点 y=40）+ col 的 Modifier 内容平移 10 = 50。
    AURORA_TEST_CHECK_NEAR(wb.origin.y, 50.0F, 1e-3F);
}

AURORA_TEST_CASE(window_bounds_is_empty_for_hidden_and_unlaid_out_widget) {
    // 负守卫（口径定死，不得留未定义）：show==false 与从未布局两种情形均返回 nullopt，
    // 而**不是**零盒 —— 零盒无法与「盒恰在窗口原点」区分，会被「非空即采用」的几何判据
    // （如语义树的绘制盒回退）当成有效几何。
    FixedBox fresh(40.0F, 20.0F);
    AURORA_TEST_CHECK_FALSE(fresh.window_bounds().has_value());

    ScrollFixture f = make_scrollable(200.0F);
    AURORA_TEST_REQUIRE(f.rows.size() > 3U);
    f.rows[3]->show.set(false);
    AURORA_TEST_CHECK_FALSE(f.rows[3]->window_bounds().has_value());
    // 同一条树里的兄弟未受影响：判据不得因一个隐藏节点而整树失效。
    AURORA_TEST_CHECK(f.rows[4]->window_bounds().has_value());
}

// ---- 派发链 origin 修复：滚动容器内控件的事件本地化 ----
//
// 缺陷：`Scroll::on_hit_test_chain` 下传给内容子树的全局原点漏扣 `offset_y_`，而派发器按
// `local_position = position − origin` 本地化坐标 ⇒ 滚动容器内每个控件收到的本地坐标整体错位
// 一个滚动量（实测点行视觉中心收到 −180，正确值 20）。
//
// 判据纪律：预期值一律由测试**独立复算**，不得取实现输出当基准。

AURORA_TEST_CASE(scrolled_descendant_receives_correct_local_position) {
    // 判据①：滚到 offset=200 后点击内容 y=200 那��的视觉中心，控件收到的 local_position
    // 必须是「行内局部 y」= 行高的一半，而不是错位一个 offset_y_ 的值。
    constexpr float aurora_g27_offset = 200.0F;
    LocalProbeFixture f = make_local_probe_scrollable(aurora_g27_offset);
    AURORA_TEST_REQUIRE(f.scroll->offset_y() > 0.0F);

    // 选内容 y = offset 的行（滚后恰在视口顶），点它的视觉中心。
    const auto idx = static_cast<std::size_t>(aurora_g27_offset / AURORA_G27_ROW_H);
    AURORA_TEST_REQUIRE(idx < f.rows.size());
    // 独立复算视觉中心：视口原点 0 + 行内容 y − offset + 行高/2（视口本身是根，原点为 0）。
    const float row_center_window_y =
        (static_cast<float>(idx) * AURORA_G27_ROW_H) - f.scroll->offset_y() + (AURORA_G27_ROW_H * 0.5F);
    const float expected_local_y = AURORA_G27_ROW_H * 0.5F;

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = Point{.x = 10.0F, .y = row_center_window_y};
    EventDispatcher::dispatch(*f.scroll, press, nullptr);

    AURORA_TEST_REQUIRE(f.rows[idx]->seen());
    // 事件本身的全局位置须与派发点一致（确认探针没被坐标系问题带偏）。
    AURORA_TEST_CHECK_NEAR(f.rows[idx]->last_position().y, row_center_window_y, 1e-3F);
    // 核心断言：本地坐标 = 点在行盒内的偏移。
    AURORA_TEST_CHECK_NEAR(f.rows[idx]->last_local().y, expected_local_y, 1e-3F);
    // 反向钉住「不是错位值」：修复前此处为 expected − offset_y_。
    AURORA_TEST_CHECK(f.rows[idx]->last_local().y > expected_local_y - 1.0F);
}

AURORA_TEST_CASE(hit_node_origin_equals_independently_recomputed_window_position) {
    // 判据②：`HitNode.origin` 逐位等于独立复算的真窗口位（视口原点 + 内容 y − offset_y_）。
    // 这条同时钉住「origin 是窗口坐标」这一语义，供`local_position = position − origin` 本地化。
    constexpr float aurora_g27_offset = 120.0F;
    LocalProbeFixture f = make_local_probe_scrollable(aurora_g27_offset);
    AURORA_TEST_REQUIRE(f.scroll->offset_y() > 0.0F);

    // 命中一条滚后可见的行：视口局部点取该行中心。
    const auto idx = static_cast<std::size_t>(aurora_g27_offset / AURORA_G27_ROW_H);
    AURORA_TEST_REQUIRE(idx < f.rows.size());
    const float local_y =
        (static_cast<float>(idx) * AURORA_G27_ROW_H) - f.scroll->offset_y() + (AURORA_G27_ROW_H * 0.5F);
    const auto chain = f.scroll->hit_test_chain(Point{.x = 10.0F, .y = local_y}, g27_viewport(), BuildContext{});
    AURORA_TEST_REQUIRE(!chain.empty());

    const auto it = std::ranges::find_if(
        chain, [&f, idx](const HitNode &n) -> bool { return n.ptr == static_cast<Widget *>(f.rows[idx].get()); });
    AURORA_TEST_REQUIRE(it != chain.end());
    // 独立复算真窗口位：该行盒顶 = 内容 y − offset_y_（视口是根，原点 0）。
    const float expected_window_y = (static_cast<float>(idx) * AURORA_G27_ROW_H) - f.scroll->offset_y();
    AURORA_TEST_CHECK_NEAR(it->origin.y, expected_window_y, 1e-3F);
}

AURORA_TEST_CASE(hit_node_origin_agrees_with_window_bounds) {
    // 判据②的交叉验证：`HitNode.origin`（事件本地化基准）与 `window_bounds()`（事后查询）
    // 对同一控件、同一帧必须给出**同一个窗口位置**——这是与本次修复合流后的关键不变量：
    // 两者同源，消费侧无需再自算折算。
    constexpr float aurora_g27_offset = 120.0F;
    LocalProbeFixture f = make_local_probe_scrollable(aurora_g27_offset);
    const auto idx = static_cast<std::size_t>(aurora_g27_offset / AURORA_G27_ROW_H);
    AURORA_TEST_REQUIRE(idx < f.rows.size());
    const float local_y =
        (static_cast<float>(idx) * AURORA_G27_ROW_H) - f.scroll->offset_y() + (AURORA_G27_ROW_H * 0.5F);
    const auto chain = f.scroll->hit_test_chain(Point{.x = 10.0F, .y = local_y}, g27_viewport(), BuildContext{});
    const auto it = std::ranges::find_if(
        chain, [&f, idx](const HitNode &n) -> bool { return n.ptr == static_cast<Widget *>(f.rows[idx].get()); });
    AURORA_TEST_REQUIRE(it != chain.end());
    const Rect wb = require_value(f.rows[idx]->window_bounds());
    AURORA_TEST_CHECK(it->origin.y == wb.origin.y);  // 容差 0：逐位相等
}

}  // namespace aurora::test_cases::utest_scroll
