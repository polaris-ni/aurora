/// 测试类型: unit
/// 目标单元: include/aurora/widget/spacer.h
/// 测试说明: 覆盖 Spacer——默认吸收全部可用空间、expand=false 退化为 0、
/// 自描述与序列化往返、在 Row 中吸收剩余空间把相邻子项推向两端

#include <memory>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/spacer.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_spacer {

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_spacer_absorbs_all_available_space) {
    // 默认构造 expand=true：占满父约束。
    Spacer s;
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Spacer");
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(non_expand_spacer_collapses_to_zero) {
    Spacer s(false);
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(spacer_in_row_pushes_sibling_to_far_end) {
    // Row（Max 主轴）+ Spacer：固定盒被推向左端，Spacer 吃掉其余 200px。
    Row row;
    row.add(box(100.0F, 20.0F));
    row.add(Node{std::make_shared<Spacer>()});
    row.set_main_axis_size(MainAxisSize::Max);

    LayoutEngine::layout(row, bounded(300.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(row.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    const Rect& sp = row.child_nodes()[1].bounds();
    AURORA_TEST_CHECK_NEAR(sp.origin.x, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sp.size.width, 200.0F, 1e-4F);
}

AURORA_TEST_CASE(spacer_degenerates_without_free_space) {
    // expand=true 的 Spacer 在 Min 主轴容器会占据测到它时的剩余 max（不扣除其后兄弟），
    // 真正「不占空间」需 expand=false；expand=true 的推挤行为由
    // spacer_in_row_pushes_sibling_to_far_end（MainAxisSize::Max 场景）覆盖。
    // 此处用 Spacer(false) 验证退化语义：行宽 = 80 + 0 + 120 = 200（Min 主轴），
    // spacer bounds 宽 0，第三个盒子紧跟第一个盒子。
    Row row;
    row.add(box(80.0F, 20.0F));
    row.add(Node{std::make_shared<Spacer>(false)});
    row.add(box(120.0F, 20.0F));

    LayoutEngine::layout(row, bounded(500.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(row.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[2].bounds().origin.x, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(spacer_describe_and_serialize_roundtrip) {
    const auto d = Spacer::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Spacer");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_expand = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "expand") {
            has_expand = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_expand);

    // 默认 expand=true 落盘；false 反序列化生效。
    Json props;
    Spacer().serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["expand"].get<bool>(), true);
    Spacer off(false);
    off.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["expand"].get<bool>(), false);

    Spacer restored;
    restored.deserialize_props(props);
    LayoutEngine::layout(restored, bounded(100.0F, 50.0F));
    AURORA_TEST_CHECK_NEAR(restored.size().width, 0.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_spacer
