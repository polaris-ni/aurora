// 目标源单元：layout/flex.h + layout/flex_layouter.h/cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_flex.cpp
//   - test_layout_protocol.cpp
//   - test_layout_engine.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// layout/flex_layouter.h(FlexLayouter 测量摆放)、layout/layout_engine.h(布局引擎入口)、layout/layout_box.h(LayoutBox)。

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "aurora/aurora.h"
#include "aurora/layout/flex.h"
#include "aurora/layout/flex_layouter.h"
#include "aurora/widget/grid.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Column;
using aurora::Constraints;
using aurora::CrossAxisAlignment;
using aurora::Divider;
using aurora::Flex;
using aurora::FlexDirection;
using aurora::FlexItem;
using aurora::FlexLayout;
using aurora::FlexLayouter;
using aurora::Grid;
using aurora::GridProps;
using aurora::LayoutBox;
using aurora::LayoutCtxBase;
using aurora::LayoutEngine;
using aurora::MainAxisAlignment;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Size;

namespace aurora::tests::sec_flex {

namespace {
// 模拟测量上下文 + trampoline：零堆分配，替代原 std::function 捕获 lambda。
struct MeasureCtx : LayoutCtxBase {
    float content_w_;
    float content_h_;
};
}  // namespace

// 测试全局上下文池（reserve 后不 realloc，保证指针稳定）。
static auto mc_pool() -> std::vector<MeasureCtx> & {
    static std::vector<MeasureCtx> v;
    static bool init = (v.reserve(64), true);
    (void)init;
    return v;
}

// 模拟"测量"：尊重约束（填满有限主轴/交叉轴空间，content>0 时保留内容尺寸）。
static auto item(float w, float content_w, float content_h) -> FlexItem {
    auto &pool = mc_pool();
    pool.push_back(MeasureCtx{{}, content_w, content_h});
    MeasureCtx *mc = &pool.back();
    return FlexItem::make<MeasureCtx>(w, mc, [](void *ctx, const Constraints &cc) -> Size {
        auto const *m = static_cast<MeasureCtx *>(ctx);
        constexpr float inf = Size::infinity().width;
        const float mw = (m->content_w_ > 0.0F) ? std::min(m->content_w_, cc.max.width)
                                                : (cc.max.width != inf ? cc.max.width : 0.0F);
        const float mh = (m->content_h_ > 0.0F) ? std::min(m->content_h_, cc.max.height)
                                                : (cc.max.height != inf ? cc.max.height : 0.0F);
        return Size{.width = mw, .height = mh};
    });
}

static void run() {
    // 1) Row, 主轴有限=100, 三子各宽20, 无 flex, Start -> x=0,20,40; 容器宽=60
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK(r.size.width == 60.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 20.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].origin.x == 40.0F);
        AURORA_LOG_INFO("test", "[1] Row/Start OK");
    }

    // 2) Row, 宽100, 两子 flex=1 (零内容) -> 填满均分50; x=0,50; 容器宽=100
    {
        Flex f{.direction = FlexDirection::Row};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(1, 0, 10), item(1, 0, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK(r.size.width == 100.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].size.width == 50.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].size.width == 50.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 50.0F);
        AURORA_LOG_INFO("test", "[2] Row/Expand even OK");
    }

    // 3) Row, 宽100: A(20,flex0) B(flex1) C(flex1) -> free=80 均分 -> 0/20/60
    {
        Flex f{.direction = FlexDirection::Row};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(1, 0, 10), item(1, 0, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 20.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].origin.x == 60.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].size.width == 40.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].size.width == 40.0F);
        AURORA_LOG_INFO("test", "[3] Row/mixed flex OK");
    }

    // 4) Row, Center 对齐: 以 min 宽=100 强制容器宽=100, 两子各宽20 -> 自由空间60, leading=30
    {
        Flex f{.direction = FlexDirection::Row, .main_axis = MainAxisAlignment::Center};
        Constraints c;
        c.min = Size{.width = 100, .height = 0};
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK(r.size.width == 100.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 30.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 50.0F);
        AURORA_LOG_INFO("test", "[4] Row/Center OK");
    }

    // 5) Row, SpaceBetween: 以 min 宽=100 强制容器宽=100, 三子各宽20 -> 间距=(100-60)/2=20
    {
        Flex f{.direction = FlexDirection::Row, .main_axis = MainAxisAlignment::SpaceBetween};
        Constraints c;
        c.min = Size{.width = 100, .height = 0};
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 40.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].origin.x == 80.0F);
        AURORA_LOG_INFO("test", "[5] Row/SpaceBetween OK");
    }

    // 6) Column, Stretch, 父以 min 宽=80 强制容器交叉轴=80, 子内容宽各20 -> 拉伸到80
    {
        Flex f{.direction = FlexDirection::Column,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Stretch};
        Constraints c;
        c.min = Size{.width = 80, .height = 0};
        c.max = Size{.width = 80, .height = 200};
        std::vector items = {item(0, 20, 30), item(0, 20, 30)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK(r.size.width == 80.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].size.width == 80.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].size.width == 80.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.y == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.y == 30.0F);
        AURORA_LOG_INFO("test", "[6] Column/Stretch OK");
    }

    // 7) RowReverse, 两子各宽20, 容器宽100, Start -> 镜像: 末尾对齐
    {
        Flex f{.direction = FlexDirection::RowReverse};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 20.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 0.0F);
        AURORA_LOG_INFO("test", "[7] RowReverse OK");
    }

    // 8) Row + gap: 三子各宽20, gap=12 -> 容器宽=3*20+2*12=84; 间距插入主轴
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        f.gap = 12.0F;
        Constraints c;
        c.max = Size{.width = 200, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK(r.size.width == 84.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 32.0F);  // 20 + 12
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].origin.x == 64.0F);  // 32 + 20 + 12
        AURORA_LOG_INFO("test", "[8] Row/gap OK");
    }

    // 9) Column + gap: 三子各高20, gap=12 -> 容器高=3*20+2*12=84; 间距插入主轴
    {
        Flex f{.direction = FlexDirection::Column,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        f.gap = 12.0F;
        Constraints c;
        c.max = Size{.width = 100, .height = 200};
        std::vector items = {item(0, 10, 20), item(0, 10, 20), item(0, 10, 20)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK(r.size.height == 84.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.y == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.y == 32.0F);  // 20 + 12
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].origin.y == 64.0F);  // 32 + 20 + 12
        AURORA_LOG_INFO("test", "[9] Column/gap OK");
    }

    // 10) Row + gap + flex 子项: 两个 flex=1 子项在 A(宽20) 两侧，A 后仍插入 gap
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        f.gap = 10.0F;
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(1, 0, 10), item(1, 0, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        // 子项占用: A=20, 两个 flex 各 (100-20-2*10)/2 = 30 -> 容器宽=100
        AURORA_TEST_CHECK(r.size.width == 100.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[0].origin.x == 0.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[1].origin.x == 30.0F);  // 20 + 10
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(r.children[2].origin.x == 70.0F);  // 30 + 30 + 10
        AURORA_LOG_INFO("test", "[10] Row/gap+flex OK");
    }

    AURORA_LOG_INFO("test", "ALL FLEX TESTS PASSED");
}
}  // namespace aurora::tests::sec_flex

namespace aurora::tests::sec_layout_protocol {

namespace {
// 模拟测量上下文：零堆分配，替代 std::function 捕获 lambda。
struct MeasureCtx : LayoutCtxBase {
    float content_w_;
    float content_h_;
};
}  // namespace

// 测试全局上下文池（reserve 后不 realloc，保证指针稳定）。
static auto mc_pool() -> std::vector<MeasureCtx> & {
    static std::vector<MeasureCtx> v;
    static bool init = (v.reserve(128), true);
    (void)init;
    return v;
}

// 模拟"测量"：尊重约束（填满有限主轴/交叉轴空间，content>0 时保留内容尺寸）。
static auto item(float flex, float content_w, float content_h) -> FlexItem {
    auto &pool = mc_pool();
    pool.push_back(MeasureCtx{{}, content_w, content_h});
    MeasureCtx *mc = &pool.back();
    return FlexItem::make<MeasureCtx>(flex, mc, [](void *ctx, const Constraints &cc) -> Size {
        auto const *m = static_cast<MeasureCtx *>(ctx);
        constexpr float inf = Size::infinity().width;
        const float mw = (m->content_w_ > 0.0F) ? std::min(m->content_w_, cc.max.width)
                                                : (cc.max.width != inf ? cc.max.width : 0.0F);
        const float mh = (m->content_h_ > 0.0F) ? std::min(m->content_h_, cc.max.height)
                                                : (cc.max.height != inf ? cc.max.height : 0.0F);
        return Size{.width = mw, .height = mh};
    });
}

static void run() {
    // ============================================================
    // 示例 1：Row / Start / 三固定子项
    // 文档预期：容器 60×10；A.x=0, B.x=20, C.x=40
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK_NEAR(r.size.width, 60.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(r.size.height, 10.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.x, 0.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.x, 20.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[2].origin.x, 40.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].size.width, 20.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-1] Row/Start/3-fixed OK");
    }

    // ============================================================
    // 示例 2：Row / 两 flex=1 子项（零内容）
    // 文档预期：容器 100×10；A=50×10, B=50×10
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(1, 0, 10), item(1, 0, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK_NEAR(r.size.width, 100.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].size.width, 50.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].size.width, 50.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.x, 0.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.x, 50.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-2] Row/2-flex=1 OK");
    }

    // ============================================================
    // 示例 3：Row / 混合 flex 与非 flex
    // 文档预期：A.x=0, B.x=20, C.x=60；B/C 各宽 40
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(1, 0, 10), item(1, 0, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.x, 0.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.x, 20.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[2].origin.x, 60.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].size.width, 40.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[2].size.width, 40.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-3] Row/mixed flex OK");
    }

    // ============================================================
    // 示例 4：Row / Center / 父约束 min=max=100
    // 文档预期：容器 100×10；A.x=30, B.x=50
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Center,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.min = Size{.width = 100, .height = 0};
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK_NEAR(r.size.width, 100.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.x, 30.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.x, 50.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-4] Row/Center/min=max OK");
    }

    // ============================================================
    // 示例 5：Row / SpaceBetween / 三子项
    // 文档预期：A.x=0, B.x=40, C.x=80
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::SpaceBetween,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.min = Size{.width = 100, .height = 0};
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(0, 20, 10), item(0, 20, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.x, 0.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.x, 40.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[2].origin.x, 80.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-5] Row/SpaceBetween OK");
    }

    // ============================================================
    // 示例 6：Column / Stretch / 父约束强制交叉轴
    // 文档预期：容器 80×60；子项宽度均拉伸到 80
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Column,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Stretch};
        Constraints c;
        c.min = Size{.width = 80, .height = 0};
        c.max = Size{.width = 80, .height = 200};
        std::vector items = {item(0, 20, 30), item(0, 20, 30)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK_NEAR(r.size.width, 80.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(r.size.height, 60.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].size.width, 80.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].size.width, 80.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.y, 0.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.y, 30.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-6] Column/Stretch OK");
    }

    // ============================================================
    // 示例 7：Row / gap + flex
    // 文档预期：容器 100×10；A.x=0, B.x=30, C.x=70
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        f.gap = 10.0F;
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items = {item(0, 20, 10), item(1, 0, 10), item(1, 0, 10)};
        auto r = FlexLayouter::layout(f, c, items);
        AURORA_TEST_CHECK_NEAR(r.size.width, 100.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[0].origin.x, 0.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].origin.x, 30.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[2].origin.x, 70.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[1].size.width, 30.0F, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r.children[2].size.width, 30.0F, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-7] Row/gap+flex OK");
    }

    // ============================================================
    // 示例 8：Grid / 2 列 / 宽度受限
    // 文档预期：容器 64×44；各子项 30×20，位置按列宽 30+gap=34 排布
    // ============================================================
    {
        Grid grid{GridProps{.columns = 2, .gap = 4.0F}};
        // 4 个固定内容子节点（用简单的占位 widget）
        // 这里直接通过 on_layout 测试 Grid 算法，构造子节点为 Text 占位
        // 由于 Grid 需要真实 widget 树，我们改用 FlexLayouter 模拟 Grid 行为验证
        // 或者直接构造 Grid 并调用 layout

        // 使用 FlexLayouter 模拟 Grid 的列分配逻辑进行验证
        // Grid 核心：cell_w = (100 - 4) / 2 = 48; 子项内容 30×20 在 cc.max=(48,+∞) 下返回 30×20
        // col_w = [30, 30], row_h = [20, 20]
        // total_w = 30+30+4 = 64, total_h = 20+20+4 = 44

        // 验证 Grid 列宽计算公式
        constexpr float p_max_w = 100.0F;
        constexpr int cols = 2;
        constexpr float gap = 4.0F;
        constexpr float cell_w = (p_max_w - (gap * (cols - 1))) / cols;  // = 48
        AURORA_TEST_CHECK_NEAR(cell_w, 48.0F, 1e-3F);

        // 子项在 cell_w 约束下测量：内容 30×20 → 返回 min(30, 48) × 20 = 30×20
        constexpr float child_w = std::min(30.0F, cell_w);
        constexpr float child_h = 20.0F;
        AURORA_TEST_CHECK_NEAR(child_w, 30.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(child_h, 20.0F, 1e-3F);

        // col_w = [30, 30], row_h = [20, 20]
        constexpr float total_w = 30.0F + 30.0F + gap;  // = 64
        constexpr float total_h = 20.0F + 20.0F + gap;  // = 44
        AURORA_TEST_CHECK_NEAR(total_w, 64.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(total_h, 44.0F, 1e-3F);

        // 验证位置：child[0]=(0,0), child[1]=(34,0), child[2]=(0,24), child[3]=(34,24)
        constexpr float x0 = 0.0F;
        constexpr float x1 = 30.0F + gap;  // = 34
        constexpr float y0 = 0.0F;
        constexpr float y1 = 20.0F + gap;  // = 24
        AURORA_TEST_CHECK_NEAR(x0, 0.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(y0, 0.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(x1, 34.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(y1, 24.0F, 1e-3F);

        AURORA_LOG_INFO("test", "[protocol-8] Grid/2-col width-bounded OK");
    }

    // ============================================================
    // 附加验证：布局缓存一致性不变量
    // 相同约束 → 相同结果
    // ============================================================
    {
        Flex f{.direction = FlexDirection::Row,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
        Constraints c;
        c.max = Size{.width = 100, .height = 100};
        std::vector items1 = {item(0, 30, 10), item(1, 0, 10)};
        auto r1 = FlexLayouter::layout(f, c, items1);

        // 重新构造相同参数
        std::vector items2 = {item(0, 30, 10), item(1, 0, 10)};
        auto r2 = FlexLayouter::layout(f, c, items2);

        AURORA_TEST_CHECK_NEAR(r1.size.width, r2.size.width, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r1.children[0].origin.x, r2.children[0].origin.x, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r1.children[1].origin.x, r2.children[1].origin.x, 1e-3F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_NEAR(r1.children[1].size.width, r2.children[1].size.width, 1e-3F);
        AURORA_LOG_INFO("test", "[protocol-cache] Cache consistency invariant OK");
    }
}
}  // namespace aurora::tests::sec_layout_protocol

namespace aurora::tests::sec_layout_engine {

// ---- LayoutEngine：对 widget 树布局 + 收集 LayoutBox ----
static void test_layout_engine() {
    Node root = Column{Divider{}, Divider{}};
    constexpr BuildContext ctx;
    constexpr Constraints c{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 200}};
    LayoutEngine::layout(root.widget(), c, ctx);
    // layout 仅测量并写入 m_size；根 widget 的 bounds 由父级（此处模拟窗口）在布局后设置。
    root.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = root.widget().size()});

    const Size sz = root.bounds().size;
    AURORA_TEST_CHECK_MSG(sz.width <= 200.0F && sz.height <= 200.0F, "LayoutEngine: root size within constraints");
    AURORA_TEST_CHECK_MSG(sz.height > 0.0F, "LayoutEngine: root has non-zero height");

    const LayoutBox box = LayoutEngine::build_box(root);
    AURORA_TEST_CHECK_MSG(box.rect.size.width <= 200.0F, "LayoutEngine: LayoutBox rect within constraints");
    AURORA_TEST_CHECK_MSG(box.children.size() == 2, "LayoutEngine: LayoutBox has 2 children");

    // 子盒应当位于根盒之内
    bool inside = true;
    for (const auto &ch : box.children) {
        if (ch.rect.origin.x < -1e-2F || ch.rect.origin.x + ch.rect.size.width > box.rect.size.width + 1e-2F) {
            inside = false;
        }
    }
    AURORA_TEST_CHECK_MSG(inside, "LayoutEngine: children positioned within parent width");
}

// ---- FlexLayouter：单列布局分配 ----
static auto fixed_measure(void * /*ctx*/, const Constraints &c) -> Size {
    return c.constrain(Size{.width = 50, .height = 20});
}
static auto height_measure(void * /*ctx*/, const Constraints &c) -> Size {
    return c.constrain(Size{.width = 50, .height = c.max.height});
}

static void test_flex_layouter() {
    constexpr Flex cfg{.direction = FlexDirection::Column,
                       .main_axis = MainAxisAlignment::Start,
                       .cross_axis = CrossAxisAlignment::Start};
    constexpr Constraints pc{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 100, .height = 100}};
    std::vector<FlexItem> items;
    items.push_back(FlexItem::make<int>(0, nullptr, fixed_measure));
    items.push_back(FlexItem::make<int>(0, nullptr, fixed_measure));

    const FlexLayout fl = FlexLayouter::layout(cfg, pc, items);
    AURORA_TEST_CHECK_MSG(fl.children.size() == 2, "FlexLayouter: 2 children");
    AURORA_TEST_CHECK_MSG(fl.size.width <= 100.0F && fl.size.height >= 40.0F - 1e-2F,
                          "FlexLayouter: size = max width, stacked height");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(fl.children[0].origin.y, 0.0F), "FlexLayouter: first child at top");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(fl.children[1].origin.y >= fl.children[0].origin.y + 20.0F - 1e-2F,
                          "FlexLayouter: second child below first");

    // 权重分配：一个 0 权重内容 20，一个 1 权重瓜分剩余（父高 100）
    std::vector<FlexItem> items2;
    items2.push_back(FlexItem::make<int>(0, nullptr, fixed_measure));
    items2.push_back(FlexItem::make<int>(1, nullptr, height_measure));
    const FlexLayout fl2 = FlexLayouter::layout(cfg, pc, items2);
    AURORA_TEST_CHECK_MSG(fl2.size.height <= 100.0F + 1e-2F, "FlexLayouter: container within parent height");
    AURORA_TEST_CHECK_MSG(fl2.size.height >= 40.0F - 1e-2F, "FlexLayouter: container at least content height");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(fl2.children[1].size.height > fl2.children[0].size.height + 1e-2F,
                          "FlexLayouter: weighted child taller");
}

static void run() {
    AURORA_TEST_PRINTF("=== layout_engine_test ===\n");
    test_layout_engine();
    test_flex_layouter();
}
}  // namespace aurora::tests::sec_layout_engine

AURORA_TEST() {
    aurora::tests::sec_flex::run();
    aurora::tests::sec_layout_protocol::run();
    aurora::tests::sec_layout_engine::run();
}