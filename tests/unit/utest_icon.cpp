/// 测试类型: unit
/// 目标单元: include/aurora/icon/icon.h、include/aurora/icon/icon_svg.h、include/aurora/widget/icon_widget.h
/// 测试说明: 覆盖 IconData 值语义（变体互斥与相等）、IconRegistry 注册/查/未命中结构化错误
/// （含候选名回带）、SvgIconCache（光栅化、缓存命中、白名单拒绝）、Icon / IconButton 的
/// 布局尺寸、序列化往返、语义标注接线与点击目标语义（wants_click / activate）

#include <memory>
#include <string>
#include <vector>

#include "aurora/core/error_codes.gen.h"
#include "aurora/core/json.h"
#include "aurora/icon/icon.h"
#include "aurora/icon/icon_svg.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/icon_widget.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_icon {

using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 最小合法 SVG 源：单填充矩形，可被内置子集光栅器处理。
auto simple_svg(int size) -> std::vector<std::uint8_t> {
    const std::string src = R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" + std::to_string(size) +
                            R"(" height=")" + std::to_string(size) + R"("><rect x="0" y="0" width=")" +
                            std::to_string(size) + R"(" height=")" + std::to_string(size) +
                            R"(" fill="#FF0000"/></svg>)";
    return {src.begin(), src.end()};
}

}  // namespace

// ---- IconData 值语义 ----

AURORA_TEST_CASE(icon_data_variant_kinds_and_access) {
    const auto g = icon::IconData::glyph("MaterialSymbols", 0xE8B8);
    AURORA_TEST_CHECK_TRUE(g.kind() == icon::IconData::Kind::Glyph);
    AURORA_TEST_CHECK_TRUE(g.as_glyph() != nullptr);
    AURORA_TEST_CHECK_EQ(g.as_glyph()->font_family, "MaterialSymbols");
    AURORA_TEST_CHECK_EQ(g.as_glyph()->codepoint, static_cast<char32_t>(0xE8B8));
    // 取错变体得到空指针（variant 结构化保证），不读到残留字段。
    AURORA_TEST_CHECK_TRUE(g.as_svg() == nullptr);

    const auto s = icon::IconData::svg(simple_svg(16), "dot");
    AURORA_TEST_CHECK_TRUE(s.kind() == icon::IconData::Kind::Svg);
    AURORA_TEST_CHECK_TRUE(s.as_svg() != nullptr);
    AURORA_TEST_CHECK_EQ(s.as_svg()->source_id, "dot");
    AURORA_TEST_CHECK_TRUE(s.as_glyph() == nullptr);

    AURORA_TEST_CHECK_TRUE(g == icon::IconData::glyph("MaterialSymbols", 0xE8B8));
    AURORA_TEST_CHECK_TRUE(g != s);
}

// ---- IconRegistry ----

AURORA_TEST_CASE(icon_registry_roundtrip_and_miss) {
    icon::IconRegistry::clear();
    icon::IconRegistry::register_icon("settings", icon::IconData::glyph("MaterialSymbols", 0xE8B8));
    icon::IconRegistry::register_icon("close", icon::IconData::glyph("MaterialSymbols", 0xE5CD));

    const auto hit = icon::IconRegistry::lookup("settings");
    AURORA_TEST_CHECK_TRUE(hit.ok());
    AURORA_TEST_CHECK_EQ(hit.value().as_glyph()->codepoint, static_cast<char32_t>(0xE8B8));

    // 未命中：结构化错误 + 候选名回带（供 AI 与 validate_ui 纠错）。
    const auto miss = icon::IconRegistry::lookup("settingz");
    AURORA_TEST_CHECK_TRUE(!miss.ok());
    AURORA_TEST_CHECK_TRUE(miss.error().code_enum == ErrorCode::IconNameNotRegistered);
    AURORA_TEST_CHECK_TRUE(miss.error().message.find("close") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(miss.error().message.find("settings") != std::string::npos);

    AURORA_TEST_CHECK_TRUE(icon::IconRegistry::unregister("close"));
    AURORA_TEST_CHECK_TRUE(!icon::IconRegistry::unregister("close"));  // 二次注销为 false
    const auto names = icon::IconRegistry::names();
    AURORA_TEST_CHECK_EQ(names.size(), static_cast<std::size_t>(1));
    AURORA_TEST_CHECK_EQ(names[0], "settings");
    icon::IconRegistry::clear();
    AURORA_TEST_CHECK_TRUE(icon::IconRegistry::names().empty());
}

// ---- SvgIconCache ----

AURORA_TEST_CASE(svg_cache_rasterize_and_hit) {
    auto &cache = icon::SvgIconCache::instance();
    cache.clear();

    const icon::SvgIconSource req{.source = simple_svg(16), .target_w = 16, .target_h = 16};
    const auto first = cache.get_or_rasterize(req);
    AURORA_TEST_CHECK_TRUE(first.ok());
    AURORA_TEST_CHECK_EQ(first.value().width, 16);
    AURORA_TEST_CHECK_EQ(first.value().height, 16);
    AURORA_TEST_CHECK_EQ(first.value().pixels.size(), static_cast<std::size_t>(16) * 16U * 4U);
    AURORA_TEST_CHECK_EQ(cache.size(), static_cast<std::size_t>(1));

    // 同请求命中缓存：不再新增条目。
    const auto second = cache.get_or_rasterize(req);
    AURORA_TEST_CHECK_TRUE(second.ok());
    AURORA_TEST_CHECK_EQ(cache.size(), static_cast<std::size_t>(1));

    // 不同尺寸 = 不同缓存键。
    const auto third =
        cache.get_or_rasterize(icon::SvgIconSource{.source = simple_svg(16), .target_w = 32, .target_h = 32});
    AURORA_TEST_CHECK_TRUE(third.ok());
    AURORA_TEST_CHECK_EQ(third.value().width, 32);
    AURORA_TEST_CHECK_EQ(cache.size(), static_cast<std::size_t>(2));
    cache.clear();
}

AURORA_TEST_CASE(svg_whitelist_rejects_unsupported_features) {
    auto &cache = icon::SvgIconCache::instance();
    cache.clear();

    // 内嵌位图特性被白名单拒绝，返回 icon-svg-unsupported。
    const std::string with_image = R"(<svg xmlns="http://www.w3.org/2000/svg"><image href="x.png"/></svg>)";
    const auto rejected = cache.get_or_rasterize(
        icon::SvgIconSource{.source = {with_image.begin(), with_image.end()}, .target_w = 16, .target_h = 16});
    AURORA_TEST_CHECK_TRUE(!rejected.ok());
    AURORA_TEST_CHECK_TRUE(rejected.error().code_enum == ErrorCode::IconSvgUnsupported);

    // 空源与非法包装同样拒绝。
    const auto empty_src = cache.get_or_rasterize(icon::SvgIconSource{.target_w = 16, .target_h = 16});
    AURORA_TEST_CHECK_TRUE(!empty_src.ok());
    AURORA_TEST_CHECK_TRUE(empty_src.error().code_enum == ErrorCode::IconSvgUnsupported);

    // 拒绝路径不产生缓存条目。
    AURORA_TEST_CHECK_EQ(cache.size(), static_cast<std::size_t>(0));

    // 独立校验入口同一口径。
    const auto v = icon::validate_svg_whitelist(with_image);
    AURORA_TEST_CHECK_TRUE(!v.ok());
}

// ---- Icon 控件 ----

AURORA_TEST_CASE(icon_default_state_and_layout) {
    const Icon d;
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, "Icon");

    Json props = Json::object();
    d.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "size"), 24.0F, 1e-4F);
    // 未设语义名 / 显式色 / 语义标注：对应键不输出（跟随主题或纯装饰语义）。
    AURORA_TEST_CHECK_FALSE(props.contains("icon"));
    AURORA_TEST_CHECK_FALSE(props.contains("color"));
    AURORA_TEST_CHECK_FALSE(props.contains("semantic_label"));

    // 布局：定尺寸盒 = size_pt_ 见方。
    Icon sized;
    sized.set_size(IconSize::S32);
    LayoutEngine::layout(sized, bounded(100.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(sized.size().width, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sized.size().height, 32.0F, 1e-4F);
}

AURORA_TEST_CASE(icon_serialize_roundtrip) {
    Icon src;
    src.set_icon("settings").set_size(20.0F).set_semantic_label("Open settings").set_color(Color{12, 34, 56, 78});
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "icon"), "settings");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "semantic_label"), "Open settings");

    Icon dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(std::string{dst.type_name()}, "Icon");
    Json back = Json::object();
    dst.serialize_props(back);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(back, "icon"), "settings");
    AURORA_TEST_CHECK_NEAR(require_field<float>(back, "size"), 20.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(back, "semantic_label"), "Open settings");
    const Color c = json_to_color(*back.at("color"));
    AURORA_TEST_CHECK_EQ(c.r, 12);
    AURORA_TEST_CHECK_EQ(c.g, 34);
}

AURORA_TEST_CASE(icon_accessibility_label_semantics) {
    Icon decorative;
    AURORA_TEST_CHECK_TRUE(decorative.accessibility_label().empty());
    AURORA_TEST_CHECK_FALSE(decorative.accessibility_is_semantic());  // 纯装饰不进语义树

    Icon labeled;
    labeled.set_semantic_label("Close");
    AURORA_TEST_CHECK_EQ(labeled.accessibility_label(), "Close");
    AURORA_TEST_CHECK_TRUE(labeled.accessibility_is_semantic());
}

// ---- IconButton 控件 ----

AURORA_TEST_CASE(icon_button_layout_and_state) {
    const IconButton d;
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, "IconButton");

    Json props = Json::object();
    d.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "size"), 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "padding"), 8.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "enabled"), true);

    // 布局：点击区 = 图标盒 + 两侧内边距。
    IconButton b;
    b.set_size(20.0F).set_padding(6.0F);
    LayoutEngine::layout(b, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(b.size().width, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(b.size().height, 32.0F, 1e-4F);
}

AURORA_TEST_CASE(icon_button_click_target_semantics) {
    // 启用且有回调：作为点击目标消费事件。
    int fired = 0;
    IconButton live;
    live.set_on_click([&fired] { ++fired; });
    AURORA_TEST_CHECK_TRUE(live.wants_click());
    live.activate();
    AURORA_TEST_CHECK_EQ(fired, 1);

    // 禁用态：不消费点击、不触发回调（activate 是唯一同步派发口）。
    IconButton dead;
    dead.set_on_click([&fired] { ++fired; }).set_enabled(false);
    AURORA_TEST_CHECK_FALSE(dead.wants_click());
    dead.activate();
    AURORA_TEST_CHECK_EQ(fired, 1);

    // 启用但无回调：不作为点击目标。
    IconButton silent;
    AURORA_TEST_CHECK_FALSE(silent.wants_click());
}

AURORA_TEST_CASE(icon_button_serialize_roundtrip) {
    IconButton src;
    src.set_icon("close").set_size(20.0F).set_padding(6.0F).set_enabled(false).set_semantic_label("Close");
    Json props = Json::object();
    src.serialize_props(props);

    IconButton dst;
    dst.deserialize_props(props);
    Json back = Json::object();
    dst.serialize_props(back);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(back, "icon"), "close");
    AURORA_TEST_CHECK_NEAR(require_field<float>(back, "padding"), 6.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_field<bool>(back, "enabled"), false);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(back, "semantic_label"), "Close");
}

}  // namespace aurora::test_cases::utest_icon
