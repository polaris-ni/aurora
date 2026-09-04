// 验证 ToolBar/StatusBar：水平排列、垂直居中、尾项右对齐、序列化、无头渲染。
#include <memory>

#include "aurora/widget/button.h"
#include "aurora/widget/text.h"
#include "aurora/widget/toolbar.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::ButtonProps;
using aurora::Constraints;
using aurora::LocalizedString;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::StatusBar;
using aurora::Text;
using aurora::ToolBar;

namespace {

auto make_text(const char *s) -> Node {
    auto t = Text();
    t.content = LocalizedString{s};
    return Node{std::move(t)};
}

auto make_button(const char *label) -> Node { return Node(Button(ButtonProps{.label = label})); }

}  // namespace

AURORA_TEST() {
    // ---- 1. ToolBar 布局：水平排列 + 垂直居中 ----
    {
        std::vector<Node> kids;
        kids.push_back(make_button("A"));
        kids.push_back(make_button("B"));
        auto tb = std::make_shared<ToolBar>(std::move(kids));

        BuildContext ctx;
        tb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 640.0F, .height = 480.0F};
        const Size s = tb->layout(c, ctx);

        AURORA_TEST_CHECK(s.width == 640.0F);
        AURORA_TEST_CHECK(s.height == tb->bar_height());

        const auto kids2 = tb->child_nodes();
        AURORA_TEST_CHECK(kids2.size() == 2);
        // 第一项从 padding 开始
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(kids2[0].bounds().origin.x == 6.0F);
        // 第二项在第一项之后（+gap）
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(kids2[1].bounds().origin.x > kids2[0].bounds().origin.x + kids2[0].bounds().size.width);
        // 垂直居中
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(kids2[0].bounds().origin.y > 0.0F);
    }

    // ---- 2. ToolBar 链式配置 ----
    {
        auto tb = std::make_shared<ToolBar>();
        tb->set_bar_height(56.0F).set_gap(10.0F);
        AURORA_TEST_CHECK(tb->bar_height() == 56.0F);

        // 非法值降级
        auto tb2 = std::make_shared<ToolBar>();
        tb2->set_bar_height(-1.0F);
        AURORA_TEST_CHECK(tb2->bar_height() == 40.0F);
    }

    // ---- 3. StatusBar 布局：尾项右对齐 ----
    {
        std::vector<Node> kids;
        kids.push_back(make_text("Ready"));
        kids.push_back(make_text("UTF-8"));
        kids.push_back(make_text("Ln 1, Col 1"));
        auto sb = std::make_shared<StatusBar>(std::move(kids));

        BuildContext ctx;
        sb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 640.0F, .height = 480.0F};
        const Size s = sb->layout(c, ctx);

        AURORA_TEST_CHECK(s.height == sb->bar_height());

        const auto kids2 = sb->child_nodes();
        AURORA_TEST_CHECK(kids2.size() == 3);
        // 前两项左排
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(kids2[0].bounds().origin.x == 8.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(kids2[1].bounds().origin.x > kids2[0].bounds().origin.x);
        // 尾项右对齐（右边缘接近 640-8）
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const float tail_right = kids2[2].bounds().origin.x + kids2[2].bounds().size.width;
        AURORA_TEST_CHECK(std::abs(tail_right - 632.0F) < 1.0F);
    }

    // ---- 4. StatusBar 单子项不右对齐 ----
    {
        std::vector<Node> kids;
        kids.push_back(make_text("Only"));
        auto sb = std::make_shared<StatusBar>(std::move(kids));

        BuildContext ctx;
        sb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 640.0F, .height = 480.0F};
        sb->layout(c, ctx);

        const auto kids2 = sb->child_nodes();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(kids2[0].bounds().origin.x == 8.0F);  // 左对齐
    }

    // ---- 5. 序列化往返 ----
    {
        auto tb = std::make_shared<ToolBar>();
        tb->set_bar_height(48.0F).set_gap(8.0F);
        aurora::Json props;
        tb->serialize_props(props);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["bar_height"].get<float>() == 48.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["gap"].get<float>() == 8.0F);

        auto tb2 = std::make_shared<ToolBar>();
        tb2->deserialize_props(props);
        AURORA_TEST_CHECK(tb2->bar_height() == 48.0F);

        auto sb = std::make_shared<StatusBar>();
        sb->set_bar_height(28.0F);
        aurora::Json sprops;
        sb->serialize_props(sprops);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(sprops["bar_height"].get<float>() == 28.0F);
    }

    // ---- 6. 无头渲染不崩溃 ----
    {
        std::vector<Node> tkids;
        tkids.push_back(make_button("Run"));
        auto tb = std::make_shared<ToolBar>(std::move(tkids));

        std::vector<Node> skids;
        skids.push_back(make_text("OK"));
        skids.push_back(make_text("v1.0"));
        auto sb = std::make_shared<StatusBar>(std::move(skids));

        BuildContext ctx;
        tb->mount(ctx);
        sb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 320.0F, .height = 240.0F};
        tb->layout(c, ctx);
        sb->layout(c, ctx);

        aurora::Painter p;
        p.begin(320, 240);
        tb->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 40.0F}}, ctx);
        sb->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 216.0F}, .size = Size{.width = 320.0F, .height = 24.0F}},
                  ctx);
        AURORA_TEST_CHECK(p.width() == 320);
    }
}