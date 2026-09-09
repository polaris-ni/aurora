/// 测试类型: unit
/// 目标单元: include/aurora/widget/chip.h
/// 测试说明: 覆盖 Chip——标签/头像/颜色/字号链式 setter 与序列化、删除回调的命中区域语义
/// （右半区左键按下触发、其余忽略）、布局尺寸由文本度量构成；附带同头 Badge 的计数、
/// 尺寸加成、序列化与 describe

#include <memory>
#include <string>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/chip.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_chip {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto press_at(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    e.local_position = Point{.x = x, .y = y};
    return e;
}

}  // namespace

AURORA_TEST_CASE(default_and_labeled_construction) {
    const Chip empty;
    AURORA_TEST_CHECK_EQ(std::string{empty.type_name()}, "Chip");
    AURORA_TEST_CHECK_EQ(empty.label(), "");
    AURORA_TEST_CHECK_EQ(empty.avatar(), "");

    const Chip tagged("Tag");
    AURORA_TEST_CHECK_EQ(tagged.label(), "Tag");
    Json props;
    tagged.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["label"].get<std::string>(), "Tag");
    AURORA_TEST_CHECK_FALSE(props.contains("avatar"));  // 空头像不落盘
    AURORA_TEST_CHECK_EQ(props["background"][0].get<int>(), 230);
    AURORA_TEST_CHECK_NEAR(props["font_size"].get<float>(), 13.0F, 1e-4F);
}

AURORA_TEST_CASE(chain_setters_update_serialized_props) {
    Chip c("base");
    c.set_label("renamed")
        .set_avatar("★")
        .set_background(Color(1, 2, 3, 4))
        .set_text_color(Color(5, 6, 7, 8))
        .set_delete_color(Color(9, 10, 11, 12))
        .set_font_size(15.0F)
        .set_corner_radius(6.0F);

    Json props;
    c.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["label"].get<std::string>(), "renamed");
    AURORA_TEST_CHECK_EQ(props["avatar"].get<std::string>(), "★");
    AURORA_TEST_CHECK_EQ(props["background"][3].get<int>(), 4);
    AURORA_TEST_CHECK_EQ(props["text_color"][0].get<int>(), 5);
    AURORA_TEST_CHECK_EQ(props["delete_color"][1].get<int>(), 10);
    AURORA_TEST_CHECK_NEAR(props["font_size"].get<float>(), 15.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["corner_radius"].get<float>(), 6.0F, 1e-4F);
}

AURORA_TEST_CASE(font_size_non_positive_degrades_to_13) {
    Chip c;
    c.set_font_size(0.0F);  // 非正字号回落默认 13pt

    Json props;
    c.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["font_size"].get<float>(), 13.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_sizes_from_text_metrics) {
    const Font f{.size_pt = 13.0F};  // 与 Chip 默认字号一致

    Chip plain("hi");
    LayoutEngine::layout(plain, bounded(1000.0F, 200.0F));
    const float expect_w = 20.0F + render::FontEngine::measure_width("hi", f);  // 10px 左右内边距
    AURORA_TEST_CHECK_NEAR(plain.size().width, expect_w, 1e-4F);
    AURORA_TEST_CHECK_NEAR(plain.size().height, render::FontEngine::measure_height(f) + 8.0F, 1e-4F);

    Chip with_avatar("hi");
    with_avatar.set_avatar("A");
    LayoutEngine::layout(with_avatar, bounded(1000.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(with_avatar.size().width, expect_w + render::FontEngine::measure_width("A", f) + 4.0F,
                           1e-4F);

    Chip deletable("hi");
    deletable.set_on_delete([]() -> void {});
    LayoutEngine::layout(deletable, bounded(1000.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(deletable.size().width, expect_w + render::FontEngine::measure_width(" ×", f), 1e-4F);
}

AURORA_TEST_CASE(delete_callback_fires_on_right_region_press) {
    Chip c("Tag");
    int fired = 0;
    c.set_on_delete([&fired]() -> void { ++fired; });
    LayoutEngine::layout(c, bounded(1000.0F, 200.0F));

    // 简化命中模型：右半区（x > 0.7 * 宽）左键按下即删除。
    MouseEvent hit = press_at(c.size().width * 0.85F, c.size().height * 0.5F);
    c.on_pointer_event(hit);
    AURORA_TEST_CHECK_EQ(fired, 1);
    AURORA_TEST_CHECK_TRUE(hit.is_handled);

    MouseEvent miss = press_at(c.size().width * 0.2F, c.size().height * 0.5F);
    c.on_pointer_event(miss);
    AURORA_TEST_CHECK_EQ(fired, 1);  // 左半区不触发
    AURORA_TEST_CHECK_FALSE(miss.is_handled);
}

AURORA_TEST_CASE(non_press_or_non_left_clicks_ignored) {
    Chip c("Tag");
    int fired = 0;
    c.set_on_delete([&fired]() -> void { ++fired; });
    LayoutEngine::layout(c, bounded(1000.0F, 200.0F));

    MouseEvent move = press_at(c.size().width * 0.9F, c.size().height * 0.5F);
    move.action = MouseAction::Move;
    c.on_pointer_event(move);
    AURORA_TEST_CHECK_EQ(fired, 0);  // 非按下不触发

    MouseEvent right = press_at(c.size().width * 0.9F, c.size().height * 0.5F);
    right.button = MouseButton::Right;
    c.on_pointer_event(right);
    AURORA_TEST_CHECK_EQ(fired, 0);  // 非左键不触发
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Chip src("seed");
    src.set_avatar("A")
        .set_background(Color(1, 2, 3, 4))
        .set_text_color(Color(5, 6, 7, 8))
        .set_delete_color(Color(9, 10, 11, 12))
        .set_font_size(14.0F)
        .set_corner_radius(3.0F);

    Json props;
    src.serialize_props(props);

    Chip dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.label(), "seed");
    AURORA_TEST_CHECK_EQ(dst.avatar(), "A");

    Json out;
    dst.serialize_props(out);
    AURORA_TEST_CHECK_EQ(out["background"][2].get<int>(), 3);
    AURORA_TEST_CHECK_EQ(out["text_color"][1].get<int>(), 6);
    AURORA_TEST_CHECK_EQ(out["delete_color"][3].get<int>(), 12);
    AURORA_TEST_CHECK_NEAR(out["font_size"].get<float>(), 14.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out["corner_radius"].get<float>(), 3.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Chip::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Chip");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_on_delete = false;
    for (const auto& e : d.events) {
        if (std::string{e} == "on_delete") {
            has_on_delete = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_on_delete);
}

AURORA_TEST_CASE(badge_layout_adds_half_height_when_counted) {
    auto child_of = []() -> Node {
        auto t = std::make_shared<Text>(".");
        t->width(aurora::Length::fixed(40.0F));
        t->height(aurora::Length::fixed(20.0F));
        return Node{t};
    };

    Badge counted(5, child_of());
    LayoutEngine::layout(counted, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(counted.size().width, 40.0F, 1e-4F);  // max(子宽, 徽章宽 20)
    AURORA_TEST_CHECK_NEAR(counted.size().height, 29.0F, 1e-4F);  // 子高 20 + 徽章半高 9

    Badge zero(0, child_of());
    LayoutEngine::layout(zero, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(zero.size().height, 20.0F, 1e-4F);  // count=0 无高度加成
}

AURORA_TEST_CASE(badge_setters_serialize_and_describe) {
    Badge b(3);
    b.set_count(7).set_badge_color(Color(1, 2, 3, 4)).set_text_color(Color(5, 6, 7, 8));
    AURORA_TEST_CHECK_EQ(b.count(), 7);
    AURORA_TEST_CHECK_EQ(std::string{b.type_name()}, "Badge");

    Json props;
    b.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["count"].get<int>(), 7);
    AURORA_TEST_CHECK_EQ(props["badge_color"][0].get<int>(), 1);
    AURORA_TEST_CHECK_EQ(props["text_color"][1].get<int>(), 6);

    Badge dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.count(), 7);

    const auto d = Badge::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Badge");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
}

}  // namespace aurora::test_cases::utest_chip
