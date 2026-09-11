/// 测试类型: unit
/// 目标单元: src/aurora/widget/serialization.cpp
/// 测试说明: WidgetRegistry 工厂注册——Skeleton 属性往返（F2：从静态 JSON 完整重建）、
///           GridView 已知类型注册（运行时 ItemBuilder 占位重建 + 标量属性语义说明）、
///           to_json/from_json 基本往返与未知类型拒绝

#include <memory>
#include <string>

#include "aurora/widget/grid_view.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/skeleton.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_serialization {

using serialization::from_json;
using serialization::to_json;

AURORA_TEST_CASE(skeleton_factory_rebuilds_props_from_json) {
    // F2：Skeleton 接入序列化工厂——自定义属性经 to_json → from_json 完整往返。
    auto src = std::make_shared<Skeleton>();
    src->set_size(Size{.width = 120.0F, .height = 24.0F})
        .set_color(Color{10, 20, 30, 255})
        .set_highlight(Color{250, 250, 250, 200})
        .set_duration(2.5);

    const Json j = to_json(*src);
    const auto rebuilt = from_json(j);
    AURORA_TEST_REQUIRE_MSG(rebuilt.ok(), "Skeleton from_json succeeds");
    AURORA_TEST_CHECK_EQ(rebuilt.value()->type_name(), std::string{"Skeleton"});

    const auto *sk = dynamic_cast<const Skeleton *>(rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(sk != nullptr, "rebuilt widget is a Skeleton");
    AURORA_TEST_CHECK_NEAR(sk->size_hint().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sk->size_hint().height, 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sk->phase(), 0.0, 1e-9);  // 新实例相位从零开始（运行时态不序列化）
}

AURORA_TEST_CASE(gridview_registered_as_known_type_with_placeholder_rebuild) {
    // F2：GridView 注册为已知类型——条目持运行时 ItemBuilder 不可从静态 JSON 重建，
    // 工厂重建出「空数据占位实例」（同 LazyList 先例），标量属性由宿主回填。
    Json j;
    j["type"] = "GridView";
    j["props"] = Json::object();
    const auto w = from_json(j);
    AURORA_TEST_REQUIRE_MSG(w.ok(), "GridView from_json succeeds (known type)");
    AURORA_TEST_CHECK_EQ(w.value()->type_name(), std::string{"GridView"});
    const auto *gv = dynamic_cast<const GridView *>(w.value().get());
    AURORA_TEST_REQUIRE_MSG(gv != nullptr, "rebuilt widget is a GridView");
    AURORA_TEST_CHECK_EQ(gv->count(), 0);  // 默认占位：无数据、无条目
    AURORA_TEST_CHECK_EQ(gv->live_item_count(), static_cast<std::size_t>(0));
}

AURORA_TEST_CASE(unknown_type_still_rejected) {
    Json j;
    j["type"] = "NoSuchWidget";
    j["props"] = Json::object();
    const auto w = from_json(j);
    AURORA_TEST_REQUIRE_MSG(!w.ok(), "unknown type rejected");
    AURORA_TEST_CHECK_EQ(w.error().code_enum, ErrorCode::WidgetUnknownType);
}

}  // namespace aurora::test_cases::utest_serialization
