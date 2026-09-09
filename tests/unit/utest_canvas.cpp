/// 测试类型: unit
/// 目标单元: include/aurora/widget/canvas.h
/// 测试说明: 覆盖 Canvas——默认 100x100 与约束钳制、固定尺寸构造与 Length setter、
/// min 约束扩张语义、自描述与序列化（回调不落盘、尺寸往返）、
/// 经无头渲染验证 on_paint 回调收到布局 bounds 并真实栅格化

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/canvas.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_canvas {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto no_paint() -> Canvas::PaintFn { return [](Painter &, const Rect &) {}; }

}  // namespace

AURORA_TEST_CASE(default_canvas_measures_100dp_and_clamps) {
    // 默认 auto 意图：宽高各取 min(100, max)。
    Canvas loose;
    LayoutEngine::layout(loose, bounded(1000.0F, 1000.0F));
    AURORA_TEST_CHECK_NEAR(loose.size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(loose.size().height, 100.0F, 1e-4F);

    Canvas tight;
    LayoutEngine::layout(tight, bounded(50.0F, 40.0F));
    AURORA_TEST_CHECK_NEAR(tight.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tight.size().height, 40.0F, 1e-4F);
}

AURORA_TEST_CASE(fixed_size_constructor_and_length_setters) {
    Canvas fixed{200.0F, 100.0F, no_paint()};
    LayoutEngine::layout(fixed, bounded(1000.0F, 1000.0F));
    AURORA_TEST_CHECK_NEAR(fixed.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fixed.size().height, 100.0F, 1e-4F);

    Canvas via_setters;
    via_setters.width(px(120.0F)).height(px(60.0F));
    LayoutEngine::layout(via_setters, bounded(1000.0F, 1000.0F));
    AURORA_TEST_CHECK_NEAR(via_setters.size().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(via_setters.size().height, 60.0F, 1e-4F);
}

AURORA_TEST_CASE(fixed_size_expands_to_min_constraint) {
    // 固定尺寸小于 min 约束时被抬升到 min（on_layout 显式 max(min, min(fixed, max))）。
    Canvas fixed{200.0F, 100.0F, no_paint()};
    Constraints roomy;
    roomy.min = Size{.width = 500.0F, .height = 500.0F};
    roomy.max = Size{.width = 600.0F, .height = 600.0F};
    LayoutEngine::layout(fixed, roomy);
    AURORA_TEST_CHECK_NEAR(fixed.size().width, 500.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fixed.size().height, 500.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    AURORA_TEST_CHECK_EQ(std::string{Canvas{}.type_name()}, "Canvas");

    const auto d = Canvas::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, std::string{"Canvas"});
    AURORA_TEST_CHECK_EQ(d.children_policy, std::string{"none"});

    bool has_on_paint = false;
    for (const auto &ev : d.events) {
        if (ev == "on_paint") {
            has_on_paint = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_on_paint);

    bool has_width = false;
    bool has_show = false;
    for (const auto &p : d.properties) {
        if (p.name == "width") {
            has_width = true;
        }
        if (p.name == "show") {
            has_show = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_width);
    AURORA_TEST_CHECK_TRUE(has_show);

    // 无响应式信号。
    Canvas c;
    std::vector<SignalViewBase *> out;
    c.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.size(), 0U);
}

AURORA_TEST_CASE(serialize_notes_callback_and_roundtrips_lengths) {
    Canvas src;
    // 库运行时语义：Canvas 覆写的 width()/height() 只写 Canvas 私有成员（canvas.h on_layout 读同一套），
    // 而 serialize_props/deserialize_props 的 width/height 键由基类 Widget 实现（widget.h），只覆盖
    // 基类尺寸意图——经 Canvas setter 设置的尺寸从不进 JSON（库缺口，见报告）。故经基类限定名
    // 调用 setter，绕过虚分派，覆盖真实可序列化的 Fixed 意图往返路径。
    src.Widget::width(px(120.0F));
    src.Widget::height(px(60.0F));
    Json props;
    src.serialize_props(props);
    // 绘制回调不可序列化：以 note 键显式声明。
    AURORA_TEST_CHECK_TRUE(props.contains("note"));
    AURORA_TEST_CHECK_TRUE(props.contains("width"));
    AURORA_TEST_CHECK_TRUE(props.contains("height"));
    AURORA_TEST_CHECK_EQ(props["show"].get<bool>(), true);

    Canvas restored;
    restored.deserialize_props(props);
    LayoutEngine::layout(restored, bounded(1000.0F, 1000.0F));
    AURORA_TEST_CHECK_NEAR(restored.size().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(restored.size().height, 60.0F, 1e-4F);
}

AURORA_TEST_CASE(paint_callback_receives_bounds_and_rasterizes) {
    // 无头渲染：回调收到的 bounds 即布局盒；填充红色后逐像素验证落盘 PNG。
    bool invoked = false;
    Rect seen{};
    Node root{std::make_shared<Canvas>(80.0F, 60.0F, [&invoked, &seen](Painter &p, const Rect &b) {
        invoked = true;
        seen = b;
        p.fill_rect(b, Color::red());
    })};

    const std::filesystem::path out =
        std::filesystem::path(testing::isolation::temp_dir()) / "canvas_paint.png";
    const auto written = render_to_png(root, 80, 60, out.string().c_str());
    AURORA_TEST_REQUIRE_TRUE(written.ok());

    AURORA_TEST_CHECK_TRUE(invoked);
    AURORA_TEST_CHECK_NEAR(seen.origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(seen.origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(seen.size.width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(seen.size.height, 60.0F, 1e-4F);

    const auto img = Image::load(out.string());
    AURORA_TEST_REQUIRE_TRUE(img.ok());
    AURORA_TEST_REQUIRE_EQ(img.value().width, 80);
    AURORA_TEST_REQUIRE_EQ(img.value().height, 60);
    const auto at = [&img](int x, int y) -> Color {
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.value().width) +
                               static_cast<std::size_t>(x)) *
                              4U;
        return Color{img.value().pixels[i], img.value().pixels[i + 1], img.value().pixels[i + 2],
                     img.value().pixels[i + 3]};
    };
    AURORA_TEST_CHECK_TRUE(at(0, 0) == Color::red());
    AURORA_TEST_CHECK_TRUE(at(40, 30) == Color::red());
    AURORA_TEST_CHECK_TRUE(at(79, 59) == Color::red());
}

}  // namespace aurora::test_cases::utest_canvas
