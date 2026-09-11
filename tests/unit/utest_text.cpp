/// 测试类型: unit
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: 覆盖 Text——默认属性不变量、链式 setter 落盘、非正字号降级与 validate_props、
/// 布局尺寸来自文本度量（折行填满、行高倍数、max_lines 截断、紧约束钳制）、序列化往返与 describe

#include <string>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_text {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 多词长句：在 80~100px 约束宽度下必然折成多行。
constexpr const char* AURORA_LONG_WORDS = "aaa bbb ccc ddd eee fff ggg hhh iii jjj";

}  // namespace

AURORA_TEST_CASE(default_text_state_and_type_name) {
    const Text t;
    AURORA_TEST_CHECK_EQ(std::string{t.type_name()}, "Text");

    Json props;
    t.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["content"].get<std::string>(), "");
    AURORA_TEST_CHECK_NEAR(props["font_size"].get<float>(), 14.0F, 1e-4F);  // 默认 14pt
    AURORA_TEST_CHECK_EQ(props["max_lines"].get<int>(), 0);  // 0 = 不限
    AURORA_TEST_CHECK_EQ(props["soft_wrap"].get<bool>(), true);
    AURORA_TEST_CHECK_NEAR(props["line_height"].get<float>(), 1.0F, 1e-4F);
    // 默认文字色 = 黑 {0,0,0,255}。
    AURORA_TEST_CHECK_EQ(props["color"][0].get<int>(), 0);
    AURORA_TEST_CHECK_EQ(props["color"][3].get<int>(), 255);
}

AURORA_TEST_CASE(chain_setters_update_serialized_props) {
    Text t("hi");
    t.set_content("hello")
        .font_size(18.0F)
        .color(Color(10, 20, 30, 40))
        .bold()
        .set_align(TextAlign::Center)
        .set_max_lines(3)
        .set_soft_wrap(false)
        .set_line_height(1.5F);

    Json props;
    t.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["content"].get<std::string>(), "hello");
    AURORA_TEST_CHECK_NEAR(props["font_size"].get<float>(), 18.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(props["color"][0].get<int>(), 10);
    AURORA_TEST_CHECK_EQ(props["color"][3].get<int>(), 40);
    AURORA_TEST_CHECK_EQ(props["max_lines"].get<int>(), 3);
    AURORA_TEST_CHECK_EQ(props["soft_wrap"].get<bool>(), false);
    AURORA_TEST_CHECK_NEAR(props["line_height"].get<float>(), 1.5F, 1e-4F);
}

AURORA_TEST_CASE(font_size_degrade_and_validate_props) {
    // font_size() 的降级路径：非正字号回落 14pt（需求 #21），降级后属性合法。
    Text zero;
    zero.font_size(0.0F);
    Text neg;
    neg.font_size(-2.5F);
    for (const Text* t : {&zero, &neg}) {
        Json props;
        t->serialize_props(props);
        AURORA_TEST_CHECK_NEAR(props["font_size"].get<float>(), 14.0F, 1e-4F);
        AURORA_TEST_CHECK_TRUE(t->validate_props().ok());
    }

    // 配置块构造可绕过降级路径，让 validate_props 真正拦截非法值。
    Text bad{TextProps{.font = Font{.size_pt = 0.0F}}};
    AURORA_TEST_CHECK_FALSE(bad.validate_props().ok());
    Text ok{TextProps{.font = Font{.size_pt = 14.0F}}};
    AURORA_TEST_CHECK_TRUE(ok.validate_props().ok());
}

AURORA_TEST_CASE(layout_width_tracks_text_metrics) {
    const Font f{};  // 默认 sans-serif 14pt，与 Text 默认一致
    Text short_t("x");
    LayoutEngine::layout(short_t, bounded(500.0F, 200.0F));
    // 未折行时宽度 = 文本度量 + 2px 常数；高度 = 单行行高 + 2px。
    AURORA_TEST_CHECK_NEAR(short_t.size().width, render::FontEngine::measure_width("x", f) + 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(short_t.size().height, render::FontEngine::measure_height(f) + 2.0F, 1e-4F);

    Text longer("xxxxxxx");
    LayoutEngine::layout(longer, bounded(500.0F, 200.0F));
    AURORA_TEST_CHECK_TRUE(longer.size().width > short_t.size().width);  // 更长文本更宽
    // 单行高度只依赖字号：内容不同同高。
    AURORA_TEST_CHECK_NEAR(longer.size().height, short_t.size().height, 1e-4F);
}

AURORA_TEST_CASE(soft_wrap_fills_width_on_overflow) {
    Text wrap(AURORA_LONG_WORDS);
    LayoutEngine::layout(wrap, bounded(100.0F, 5000.0F));
    // 固有宽度超限且 soft_wrap=true → 填满约束宽度。
    AURORA_TEST_CHECK_NEAR(wrap.size().width, 100.0F, 1e-4F);

    Text nowrap(AURORA_LONG_WORDS);
    nowrap.set_soft_wrap(false);
    LayoutEngine::layout(nowrap, bounded(100.0F, 5000.0F));
    // 不折行 → 单行高度明显低于折行结果。
    AURORA_TEST_CHECK_TRUE(nowrap.size().height < wrap.size().height);
}

AURORA_TEST_CASE(max_lines_limits_wrapped_height) {
    Text all(AURORA_LONG_WORDS);
    LayoutEngine::layout(all, bounded(80.0F, 5000.0F));

    Text one(AURORA_LONG_WORDS);
    one.set_max_lines(1);
    LayoutEngine::layout(one, bounded(80.0F, 5000.0F));

    AURORA_TEST_CHECK_TRUE(one.size().height < all.size().height);  // 截到 1 行更矮
}

AURORA_TEST_CASE(line_height_multiplier_scales_height) {
    Text base("hello");
    LayoutEngine::layout(base, bounded(500.0F, 2000.0F));
    Text tall("hello");
    tall.set_line_height(2.0F);
    LayoutEngine::layout(tall, bounded(500.0F, 2000.0F));
    // h = 行数 * 行高 * 倍数 + 2 → 2 倍行高时 tall = 2*base - 2（精确成立）。
    AURORA_TEST_CHECK_TRUE(tall.size().height > base.size().height);
    AURORA_TEST_CHECK_NEAR(tall.size().height, (2.0F * base.size().height) - 2.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_clamps_into_tight_constraints) {
    Text t(AURORA_LONG_WORDS);
    LayoutEngine::layout(t, bounded(30.0F, 20.0F));
    AURORA_TEST_CHECK_NEAR(t.size().width, 30.0F, 1e-4F);  // 宽度钳到约束
    AURORA_TEST_CHECK_TRUE(t.size().height > 0.0F);
    AURORA_TEST_CHECK_TRUE(t.size().height <= 20.0F);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Text src("source");
    src.font_size(16.0F)
        .color(Color(1, 2, 3, 4))
        .bold()
        .set_align(TextAlign::Center)
        .set_max_lines(2)
        .set_overflow(TextOverflow::Ellipsis)
        .set_soft_wrap(false)
        .set_line_height(1.25F)
        .set_decoration(TextDecoration::Underline)
        .set_background_color(Color(9, 8, 7, 6));

    Json props;
    src.serialize_props(props);

    Text dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.font.size_pt, 16.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(dst.text_align == TextAlign::Center);
    AURORA_TEST_CHECK_EQ(dst.max_lines, 2);
    AURORA_TEST_CHECK_FALSE(dst.soft_wrap);
    AURORA_TEST_CHECK_NEAR(dst.line_height, 1.25F, 1e-4F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(dst.font.weight), 700);  // bold() 经 JSON 往返

    // 再序列化比对枚举/颜色字段（不硬编码枚举拼写）。
    Json out;
    dst.serialize_props(out);
    AURORA_TEST_CHECK_EQ(out["content"].get<std::string>(), "source");
    AURORA_TEST_CHECK_EQ(out["color"][2].get<int>(), 3);
    AURORA_TEST_CHECK_EQ(out["background_color"][0].get<int>(), 9);
    AURORA_TEST_CHECK_EQ(out["overflow"].get<std::string>(), props["overflow"].get<std::string>());
    // decoration 按位组合序列化为字符串数组（props_io.h text_decoration_to_json），非字符串。
    AURORA_TEST_CHECK_TRUE(out["decoration"] == props["decoration"]);
}

AURORA_TEST_CASE(describe_metadata_signals_and_resolved_text) {
    const auto d = Text::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Text");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_content = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "content") {
            has_content = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_content);

    // 自有信号：仅 content 一个。
    Text t("hi");
    std::vector<aurora::SignalViewBase*> signals;
    t.collect_signals(signals);
    AURORA_TEST_CHECK_EQ(signals.size(), 1U);

    // resolved_text 在默认字符串表下原样返回。
    const BuildContext ctx;
    AURORA_TEST_CHECK_EQ(t.resolved_text(ctx), "hi");
}

AURORA_TEST_CASE(text_direction_serialize_and_unset_omitted) {
    // A2：显式 direction 序列化为 "RTL"/"LTR"；未设置（继承环境）时不输出键。
    Text rtl("مرحبا");
    rtl.set_direction(TextDirection::RTL);
    Json props;
    rtl.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["direction"].get<std::string>(), std::string{"RTL"});

    Text back;
    back.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(back.direction.has_value());
    AURORA_TEST_CHECK_TRUE(*back.direction == TextDirection::RTL);

    Text inherit("inherit");
    Json plain;
    inherit.serialize_props(plain);
    AURORA_TEST_CHECK_TRUE(!plain.contains("direction"));  // 继承语义不落盘
}

AURORA_TEST_CASE(text_start_end_alignment_follows_direction) {
    // A2：End 对齐——LTR 靠右、RTL 靠左（Start 对称）。
    // 用折行文本的可视行验证：End 对齐时短行贴齐端侧，方向翻转后贴齐另一侧。
    const std::string words = "MMMM MMMM MMMM MMMM";
    const Font f{};
    const float natural = render::FontEngine::measure_width(words, f);
    // 取约束 = 固有宽的 60%：必然折行，第二行是短行（对齐偏移可见）。
    const float wrap_w = natural * 0.6F;

    auto leftmost_ink_of_second_line = [&](bool rtl) -> float {
        Text t(words);
        t.set_align(TextAlign::End);
        if (rtl) {
            t.set_direction(TextDirection::RTL);
        }
        LayoutEngine::layout(t, bounded(wrap_w, 500.0F));
        AURORA_TEST_REQUIRE_TRUE(t.size().height > render::FontEngine::measure_height(f));
        Painter p;
        p.begin(static_cast<int>(wrap_w) + 8, static_cast<int>(t.size().height) + 4);
        const BuildContext ctx;
        t.paint(p, Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = t.size()}, ctx);
        const float line_h = render::FontEngine::measure_height(f);
        const int y0 = static_cast<int>(line_h);  // 第二行（0-based 行 1）中部
        for (int x = 0; x < static_cast<int>(wrap_w) + 8; ++x) {
            if (p.get_pixel(x, y0 + static_cast<int>(line_h / 2.0F)) != Color{0, 0, 0, 0}) {
                return static_cast<float>(x);
            }
        }
        return -1.0F;
    };

    const float ltr_left = leftmost_ink_of_second_line(false);
    const float rtl_left = leftmost_ink_of_second_line(true);
    AURORA_TEST_CHECK_TRUE(ltr_left > 0.0F);
    AURORA_TEST_CHECK_TRUE(rtl_left > 0.0F);
    // End 对齐：LTR 短行贴右缘（左侧留白大），RTL 短行贴左缘（左侧几乎无留白）。
    AURORA_TEST_CHECK_TRUE(ltr_left > rtl_left + 10.0F);
    AURORA_TEST_CHECK_TRUE(rtl_left <= 4.0F);
}

}  // namespace aurora::test_cases::utest_text
