/// 测试类型: unit
/// 目标单元: include/aurora/widget/scroll.h
/// 测试说明: 覆盖 Scroll——构造默认值与自描述、内容小于视口不滚动、内容溢出时视口取父约束且内容宽被钳制、
/// scroll_by 方向与 step 乘子、偏移钳制、程序化 set_offset 的夹取与语义（越窗跳转仍取到正确内容带）、
/// 无子项退化、初始化列表取首项、step 序列化往返

#include <memory>
#include <string>

#include "aurora/app/scroll_storage.h"
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
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x,
                                             .y = bounds.origin.y + (band * static_cast<float>(i))},
                             .size = Size{.width = bounds.size.width, .height = band}},
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
    p.begin(100, 100);                             // 重新起帧
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

}  // namespace aurora::test_cases::utest_scroll
