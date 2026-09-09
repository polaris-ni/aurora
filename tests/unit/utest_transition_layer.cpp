/// 测试类型: unit
/// 目标单元: include/aurora/navigation/transition_layer.h
/// 测试说明: 覆盖 TransitionLayer 转场合成——布局满约束与旧→新遍历序、自描述/不可缓存/不订阅信号、
/// Fade 端点与中点像素混合、progress 钳制、Slide 水平分屏像素、命中优先新页再回退旧页、
/// Hero 注册表配对捕获与 morphing 标记填充

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/navigation/transition_layer.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/containers.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_transition_layer {

namespace {

/// 纯色填充探针叶控件：记录 paint 次数并整块填充指定颜色（像素断言无字体依赖）。
class SolidBox final : public LeafWidget {
  public:
    SolidBox(Color fill, int *paint_count = nullptr) : fill_(fill), paint_count_(paint_count) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        if (paint_count_ != nullptr) {
            ++*paint_count_;
        }
        p.fill_rect(bounds, fill_);
    }

  private:
    Color fill_;
    int *paint_count_;
};

constexpr int AURORA_TEST_TRANSITION_LAYER_WIDTH = 100;
constexpr int AURORA_TEST_TRANSITION_LAYER_HEIGHT = 50;

auto solid_page(Color fill, int *paint_count = nullptr) -> Node {
    return Node{std::make_shared<SolidBox>(fill, paint_count)};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto full_rect() -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                .size = Size{.width = static_cast<float>(AURORA_TEST_TRANSITION_LAYER_WIDTH),
                             .height = static_cast<float>(AURORA_TEST_TRANSITION_LAYER_HEIGHT)}};
}

}  // namespace

AURORA_TEST_CASE(layer_layout_fills_and_visits_children) {
    auto old_page = solid_page(Color{255, 0, 0});
    auto new_page = solid_page(Color{0, 0, 255});
    State<double> progress{0.0};
    TransitionLayer layer(old_page, new_page, &progress, TransitionKind::Fade);

    constexpr BuildContext ctx;
    const Size out = layer.layout(bounded(100.0F, 50.0F), ctx);
    AURORA_TEST_CHECK_NEAR(out.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out.height, 50.0F, 1e-4F);

    // 遍历序：旧页在前、新页在后。
    std::vector<const Widget *> visited;
    layer.for_each_child([&visited](const Widget &w) -> void { visited.push_back(&w); });
    AURORA_TEST_REQUIRE_EQ(visited.size(), 2U);
    AURORA_TEST_CHECK_EQ(visited[0], &old_page.widget());
    AURORA_TEST_CHECK_EQ(visited[1], &new_page.widget());
}

AURORA_TEST_CASE(layer_metadata_and_signals) {
    State<double> progress{0.0};
    TransitionLayer layer(solid_page(Color{255, 0, 0}), solid_page(Color{0, 0, 255}), &progress, TransitionKind::Fade);

    AURORA_TEST_CHECK_STREQ(layer.type_name(), "TransitionLayer");
    AURORA_TEST_CHECK_FALSE(layer.can_cache_display_list());  // 内容每帧变化，禁 DL 缓存

    std::vector<SignalViewBase *> sigs;
    layer.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.size(), 0U);  // progress 由宿主统一订阅

    const auto desc = layer.describe();
    AURORA_TEST_CHECK_EQ(desc.name, std::string{"TransitionLayer"});
    AURORA_TEST_CHECK_EQ(desc.children_policy, std::string{"multiple"});
}

AURORA_TEST_CASE(layer_fade_endpoints) {
    State<double> progress{0.0};
    TransitionLayer layer(solid_page(Color{255, 0, 0}), solid_page(Color{0, 0, 255}), &progress, TransitionKind::Fade);

    constexpr BuildContext ctx;
    Painter p;
    p.begin(AURORA_TEST_TRANSITION_LAYER_WIDTH, AURORA_TEST_TRANSITION_LAYER_HEIGHT);

    // t=0：旧页全显、新页 alpha 0。
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(50, 25), Color{255, 0, 0});

    // t=1：旧页 alpha 0、新页全显。
    progress.set(1.0);
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(50, 25), Color{0, 0, 255});
}

AURORA_TEST_CASE(layer_fade_midpoint_blends) {
    State<double> progress{0.5};
    TransitionLayer layer(solid_page(Color{255, 0, 0}), solid_page(Color{0, 0, 255}), &progress, TransitionKind::Fade);

    constexpr BuildContext ctx;
    Painter p;
    p.begin(AURORA_TEST_TRANSITION_LAYER_WIDTH, AURORA_TEST_TRANSITION_LAYER_HEIGHT);
    layer.paint(p, full_rect(), ctx);

    // 中点为交叉淡变混合像素：既非纯旧页色也非纯新页色。
    const Color mid = p.get_pixel(50, 25);
    AURORA_TEST_CHECK_NE(mid, Color{255, 0, 0});
    AURORA_TEST_CHECK_NE(mid, Color{0, 0, 255});
}

AURORA_TEST_CASE(layer_progress_clamped) {
    State<double> progress{0.0};
    TransitionLayer layer(solid_page(Color{255, 0, 0}), solid_page(Color{0, 0, 255}), &progress, TransitionKind::Fade);

    constexpr BuildContext ctx;
    Painter p;
    p.begin(AURORA_TEST_TRANSITION_LAYER_WIDTH, AURORA_TEST_TRANSITION_LAYER_HEIGHT);

    progress.set(1.7);  // 越上界钳制为 1
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(50, 25), Color{0, 0, 255});

    progress.set(-0.5);  // 越下界钳制为 0
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(50, 25), Color{255, 0, 0});
}

AURORA_TEST_CASE(layer_slide_positions_pages) {
    State<double> progress{0.0};
    TransitionLayer layer(solid_page(Color{255, 0, 0}), solid_page(Color{0, 0, 255}), &progress, TransitionKind::Slide);

    constexpr BuildContext ctx;
    Painter p;
    p.begin(AURORA_TEST_TRANSITION_LAYER_WIDTH, AURORA_TEST_TRANSITION_LAYER_HEIGHT);

    // t=0：旧页占满，新页在右外侧被裁剪。
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(25, 25), Color{255, 0, 0});
    AURORA_TEST_CHECK_EQ(p.get_pixel(75, 25), Color{255, 0, 0});

    // t=0.5：旧页左半、新页右半。
    progress.set(0.5);
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(25, 25), Color{255, 0, 0});
    AURORA_TEST_CHECK_EQ(p.get_pixel(75, 25), Color{0, 0, 255});

    // t=1：旧页滑出左缘，新页占满。
    progress.set(1.0);
    layer.paint(p, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(p.get_pixel(25, 25), Color{0, 0, 255});
    AURORA_TEST_CHECK_EQ(p.get_pixel(75, 25), Color{0, 0, 255});
}

AURORA_TEST_CASE(layer_hit_test_prefers_new_then_old) {
    State<double> progress{0.5};
    auto old_probe = std::make_shared<SolidBox>(Color{255, 0, 0});
    auto new_probe = std::make_shared<SolidBox>(Color{0, 0, 255});
    TransitionLayer layer(Node{old_probe}, Node{new_probe}, &progress, TransitionKind::Fade);

    constexpr BuildContext ctx;
    Widget *hit = layer.hit_test(Point{.x = 50.0F, .y = 25.0F}, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(hit, new_probe.get());  // 新页在视觉顶层，优先命中

    // 新页未命中（空 Column 无子节点）→ 回退旧页。
    TransitionLayer fallback(Node{old_probe}, Node{std::make_shared<Column>()}, &progress, TransitionKind::Fade);
    Widget *hit_old = fallback.hit_test(Point{.x = 50.0F, .y = 25.0F}, full_rect(), ctx);
    AURORA_TEST_CHECK_EQ(hit_old, old_probe.get());

    Widget *miss = fallback.hit_test(Point{.x = 150.0F, .y = 25.0F}, full_rect(), ctx);
    AURORA_TEST_CHECK_NULL(miss);
}

AURORA_TEST_CASE(layer_hero_morph_pairs_and_marks) {
    auto reg = std::make_shared<HeroRegistry>();
    Environment env;
    env.set_local<std::shared_ptr<HeroRegistry>>(reg);
    BuildContext ctx;
    ctx.env = &env;

    int red_paints = 0;
    int green_paints = 0;
    int blue_paints = 0;
    // 旧页两 Hero 各占一半高度（expand 权重）：fill-max 子项会把 Column 主轴空间吃满，
    // 后续子项会布局出零面积矩形，被 Container::on_paint 的遮挡剔除（默认开启）跳过，
    // Hero::on_paint 不执行则几何捕获不发生。
    auto hero_shared = std::make_shared<Hero>("shared", Node{SolidBox{Color{255, 0, 0}, &red_paints}});
    auto hero_old_only = std::make_shared<Hero>("old_only", Node{SolidBox{Color{0, 160, 0}, &green_paints}});
    hero_shared->modifier.set(Modifier{}.expand(1.0F));
    hero_old_only->modifier.set(Modifier{}.expand(1.0F));
    auto old_page = Node{Column{
        Node{hero_shared},
        Node{hero_old_only},
    }};
    auto new_page = Node{std::make_shared<Hero>("shared", Node{SolidBox{Color{0, 0, 255}, &blue_paints}})};

    State<double> progress{0.5};
    TransitionLayer layer(old_page, new_page, &progress, TransitionKind::Fade);
    std::unordered_set<std::string> morphed;
    layer.set_hero_registry(reg, &morphed);

    // Hero 的几何捕获发生在 paint 阶段，而容器子项矩形来自布局结果：未布局时 Hero 的
    // Node 边界为零面积矩形，会被遮挡剔除跳过，注册表捕获不到任何条目。绘制前先布局。
    layer.layout(bounded(AURORA_TEST_TRANSITION_LAYER_WIDTH, AURORA_TEST_TRANSITION_LAYER_HEIGHT), ctx);

    Painter p;
    p.begin(AURORA_TEST_TRANSITION_LAYER_WIDTH, AURORA_TEST_TRANSITION_LAYER_HEIGHT);
    layer.paint(p, full_rect(), ctx);

    // 捕获：旧页双条目、新页单条目，捕获态复位。
    AURORA_TEST_CHECK_EQ(reg->source.size(), 2U);
    AURORA_TEST_CHECK_TRUE(reg->source.contains("shared"));
    AURORA_TEST_CHECK_TRUE(reg->source.contains("old_only"));
    AURORA_TEST_REQUIRE_EQ(reg->target.size(), 1U);
    AURORA_TEST_CHECK_TRUE(reg->target.contains("shared"));
    AURORA_TEST_CHECK_TRUE(reg->capture_mode == HeroRegistry::CaptureMode::None);

    // morphing 标记仅含配对成功的共享 tag。
    AURORA_TEST_REQUIRE_EQ(morphed.size(), 1U);
    AURORA_TEST_CHECK_TRUE(morphed.contains("shared"));
    AURORA_TEST_CHECK_FALSE(morphed.contains("old_only"));

    // 共享元素：页内自绘 1 次 + 覆盖层插值绘制 1 次 = 2；旧页独有仅页内 1 次（退化为淡出）。
    AURORA_TEST_CHECK_EQ(red_paints, 2);
    AURORA_TEST_CHECK_EQ(blue_paints, 2);
    AURORA_TEST_CHECK_EQ(green_paints, 1);
}

}  // namespace aurora::test_cases::utest_transition_layer
