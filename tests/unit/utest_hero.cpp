/// 测试类型: unit
/// 目标单元: include/aurora/navigation/hero.h
/// 测试说明: 覆盖 Hero 的 tag 存取、tag 属性序列化往返、adopt_children 取首项、
/// 自描述元数据、不可缓存 Display List 与无信号、经环境注册表的 Source/Target 几何捕获、
/// morphing 期跳过自绘、布局透传子约束

#include <memory>
#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/navigation/hero.h"
#include "aurora/render/painter.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_hero {

namespace {

/// 纯色填充探针叶控件：记录 paint 次数并整块填充指定颜色。
class SolidBox final : public LeafWidget {
  public:
    SolidBox(Color fill, int* paint_count = nullptr) : fill_(fill), paint_count_(paint_count) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "SolidBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override { return c.constrain(c.max); }

    auto on_paint(Painter& p, const Rect& bounds, const BuildContext& /*ctx*/) -> void override {
        if (paint_count_ != nullptr) {
            ++*paint_count_;
        }
        p.fill_rect(bounds, fill_);
    }

  private:
    Color fill_;
    int* paint_count_;
};

/// 就地构造注入 HeroRegistry 的绘制环境（Hero 经 ctx.environment 读取）。
/// env 须存活于调用方作用域，故以出参形态装配而非按值返回 BuildContext。
auto make_registry_context(Environment& env, const std::shared_ptr<HeroRegistry>& reg) -> BuildContext {
    env.set_local<std::shared_ptr<HeroRegistry>>(reg);
    BuildContext ctx;
    ctx.env = &env;
    return ctx;
}

auto paint_rect(float w, float h) -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = h}};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(hero_tag_accessors) {
    Hero hero{"logo", Node{SolidBox{Color{255, 0, 0}}}};
    AURORA_TEST_CHECK_EQ(hero.tag(), std::string{"logo"});

    hero.set_tag("banner");
    AURORA_TEST_CHECK_EQ(hero.tag(), std::string{"banner"});
}

AURORA_TEST_CASE(hero_serialize_props_roundtrip) {
    Hero hero{"logo", Node{SolidBox{Color{255, 0, 0}}}};
    Json props;
    hero.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["tag"].get<std::string>(), std::string{"logo"});

    Hero parsed{"", Node{SolidBox{Color{255, 0, 0}}}};
    Json in;
    in["tag"] = std::string{"banner"};
    parsed.deserialize_props(in);
    AURORA_TEST_CHECK_EQ(parsed.tag(), std::string{"banner"});
}

AURORA_TEST_CASE(hero_adopt_children_takes_first) {
    auto first = std::make_shared<SolidBox>(Color{255, 0, 0});
    auto second = std::make_shared<SolidBox>(Color{0, 0, 255});
    Hero hero{"logo", Node{first}};

    std::vector<Node> kids{Node{first}, Node{second}};
    hero.adopt_children(std::move(kids));

    AURORA_TEST_REQUIRE_EQ(hero.child_nodes().size(), 1U);
    AURORA_TEST_CHECK_EQ(&hero.child_nodes()[0].widget(), first.get());
}

AURORA_TEST_CASE(hero_describe_metadata) {
    const auto desc = Hero{"logo", Node{SolidBox{Color{255, 0, 0}}}}.describe();
    AURORA_TEST_CHECK_EQ(desc.name, std::string{"Hero"});
    AURORA_TEST_CHECK_EQ(desc.children_policy, std::string{"single"});
    AURORA_TEST_REQUIRE_EQ(desc.properties.size(), 1U);
    AURORA_TEST_CHECK_EQ(desc.properties[0].name, std::string{"tag"});
    AURORA_TEST_CHECK_EQ(desc.properties[0].type, std::string{"string"});
}

AURORA_TEST_CASE(hero_cache_policy_and_signals) {
    Hero hero{"logo", Node{SolidBox{Color{255, 0, 0}}}};
    AURORA_TEST_CHECK_STREQ(hero.type_name(), "Hero");
    AURORA_TEST_CHECK_FALSE(hero.can_cache_display_list());  // 绘制有副作用（几何注册）

    std::vector<SignalViewBase*> sigs;
    hero.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.size(), 0U);
}

AURORA_TEST_CASE(hero_paint_without_registry_paints_child) {
    int paints = 0;
    Hero hero{"logo", Node{SolidBox{Color{0, 160, 0}, &paints}}};

    const BuildContext ctx;  // 无环境：registry 查找为 nullptr，正常自绘
    Painter p;
    p.begin(60, 30);
    hero.paint(p, paint_rect(60.0F, 30.0F), ctx);

    AURORA_TEST_CHECK_EQ(paints, 1);
}

AURORA_TEST_CASE(hero_paint_captures_source_geometry) {
    auto reg = std::make_shared<HeroRegistry>();
    int paints = 0;
    auto child = std::make_shared<SolidBox>(Color{0, 160, 0}, &paints);
    Hero hero{"logo", Node{child}};

    Environment env;
    const BuildContext ctx = make_registry_context(env, reg);
    reg->capture_mode = HeroRegistry::CaptureMode::Source;

    Painter p;
    p.begin(60, 30);
    hero.paint(p, paint_rect(60.0F, 30.0F), ctx);

    AURORA_TEST_REQUIRE_EQ(reg->source.size(), 1U);
    AURORA_TEST_REQUIRE_TRUE(reg->source.contains("logo"));
    const HeroEntry& entry = reg->source["logo"];
    AURORA_TEST_CHECK_NEAR(entry.bounds.origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(entry.bounds.origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(entry.bounds.size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(entry.bounds.size.height, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(&entry.child.widget(), child.get());  // 共享元素内容同实例
    AURORA_TEST_CHECK_EQ(paints, 1);  // 非 morphing：捕获后正常自绘
}

AURORA_TEST_CASE(hero_paint_captures_target_geometry) {
    auto reg = std::make_shared<HeroRegistry>();
    auto child = std::make_shared<SolidBox>(Color{0, 0, 255});
    Hero hero{"logo", Node{child}};

    Environment env;
    const BuildContext ctx = make_registry_context(env, reg);
    reg->capture_mode = HeroRegistry::CaptureMode::Target;

    Painter p;
    p.begin(80, 40);
    hero.paint(p, paint_rect(80.0F, 40.0F), ctx);

    AURORA_TEST_REQUIRE_EQ(reg->target.size(), 1U);
    AURORA_TEST_REQUIRE_TRUE(reg->target.contains("logo"));
    AURORA_TEST_CHECK_NEAR(reg->target["logo"].bounds.size.width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(reg->target["logo"].bounds.size.height, 40.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(reg->source.empty());  // Source 桶不受 Target 捕获影响
}

AURORA_TEST_CASE(hero_paint_skips_child_while_morphing) {
    auto reg = std::make_shared<HeroRegistry>();
    int paints = 0;
    Hero hero{"logo", Node{SolidBox{Color{255, 0, 0}, &paints}}};

    Environment env;
    const BuildContext ctx = make_registry_context(env, reg);
    reg->capture_mode = HeroRegistry::CaptureMode::Source;
    reg->morphing.insert("logo");  // 本帧由覆盖层绘制，Hero 跳过自绘

    Painter p;
    p.begin(60, 30);
    hero.paint(p, paint_rect(60.0F, 30.0F), ctx);

    AURORA_TEST_CHECK_EQ(paints, 0);  // 跳过自绘，避免双重影像
    AURORA_TEST_CHECK_TRUE(reg->source.contains("logo"));  // 捕获仍先于跳绘完成
}

AURORA_TEST_CASE(hero_layout_passes_constraints_to_child) {
    Hero hero{"logo", Node{SolidBox{Color{255, 0, 0}}}};

    const BuildContext ctx;
    const Size out = hero.layout(bounded(120.0F, 40.0F), ctx);
    AURORA_TEST_CHECK_NEAR(out.width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out.height, 40.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(hero.size().width, 120.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_hero
