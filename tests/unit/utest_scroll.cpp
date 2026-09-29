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
    for (const auto *g = gates.begin(); g != gates.end(); ++g) {
        if (g->contains("id") && g->at("id")->as_or<std::string>("") == id) {
            AURORA_TEST_REQUIRE_MSG(g->contains("threshold"), std::string{"gate "} + id + " has no threshold");
            return g->at("threshold")->as_or<double>(0.0);
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

}  // namespace aurora::test_cases::utest_scroll
