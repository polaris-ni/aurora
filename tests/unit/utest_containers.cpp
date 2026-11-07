/// 测试类型: unit
/// 目标单元: include/aurora/widget/containers.h
/// 测试说明: 覆盖 Column/Row 容器级行为——子项构造与所有权、gap 落位、
/// MainAxisSize::Max 撑满、MainAxisAlignment::End 收尾对齐、CrossAxisAlignment::Stretch
/// 拉伸子项交叉轴、CrossAxisAlignment::Baseline 基线对齐（Text/Button 混排、Modifier 内边距换算、
/// Column 回退 Start 并一次性降级提示）、反向排布是控件级开关而不翻转主轴、负 gap 校验、
/// 属性序列化往返，以及容器子树绘制缓存
/// （Display List / cache_layer）随全局光栅状态（AA 模式）世代失效。
/// 属性单一声明迁移后另覆盖权威表的读消费面：逐行撤表态该键退出序列化、四枚常量档读件在
/// 明 / 暗两档与库内解析全同且不存在带上下文的第二件、全量表态的 JSON 逐行复原生效值并落同一帧几何、
/// 非法 JSON 逐档降级（`gap` 越界 / 形态不符拒写并撤表态、未登记校验的枚举名按宽容通路表态不上报）、
/// 挂载后写入按行声明的布局档标脏而绘制档不动、反向开关不被序列化

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "aurora/core/diagnostics.h"
#include "aurora/core/directionality.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_containers {

using aurora::testing::require_field;

namespace {

/// 固定尺寸文本盒（宽高锁定，尺寸可预期）。
auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

/// 交叉轴可拉伸盒（对应轴用 Expand）。
auto box_cross_expand(float main_extent, bool stretch_w) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(stretch_w ? aurora::Length::expand() : aurora::Length::fixed(main_extent));
    t->height(stretch_w ? aurora::Length::fixed(main_extent) : aurora::Length::expand());
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

// ---- 子树绘制缓存（Display List / cache_layer）观测设施 ----
constexpr int AURORA_CACHE_W = 260;
constexpr int AURORA_CACHE_H = 48;

/// 父 Column + 子 Text：后代绘制缓存是观测对象，故必须有层级。
auto text_column(bool cache_layer) -> std::shared_ptr<Column> {
    auto txt = std::make_shared<Text>("Aurora Wedge 123");
    txt->font_size(20.0F);
    auto col = std::make_shared<Column>(std::vector<Node>{Node{std::shared_ptr<Widget>(txt)}});
    if (cache_layer) {
        col->set_modifier(Modifier().cache_layer());
    }
    LayoutEngine::layout(*col, bounded(static_cast<float>(AURORA_CACHE_W), static_cast<float>(AURORA_CACHE_H)));
    return col;
}

/// 把容器子树绘制进离屏画布并返回像素校验和（同一实例连续绘制可观察缓存是否命中）。
auto paint_hash(Widget &w) -> std::uint64_t {
    Painter p;
    p.begin(AURORA_CACHE_W, AURORA_CACHE_H);
    // 白底 + 不透明黑字：满足 ClearType 生效条件（c.a == 255）。
    p.fill_rect(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
             .size = Size{.width = static_cast<float>(AURORA_CACHE_W), .height = static_cast<float>(AURORA_CACHE_H)}},
        Color{255, 255, 255, 255});
    constexpr BuildContext ctx;
    w.paint(
        p,
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
             .size = Size{.width = static_cast<float>(AURORA_CACHE_W), .height = static_cast<float>(AURORA_CACHE_H)}},
        ctx);
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const std::uint8_t *d = p.data();
    constexpr std::size_t n = static_cast<std::size_t>(AURORA_CACHE_W) * static_cast<std::size_t>(AURORA_CACHE_H) * 4U;
    for (std::size_t i = 0; i < n; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 对像素缓冲做字节级哈希，下标即字节偏移
        h ^= d[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/// AA 模式是进程级状态：用例结束还原，避免污染同文件后续用例。
class AaModeRestore {
  public:
    AaModeRestore() : saved_(render::FontEngine::text_aa_mode()) {}
    ~AaModeRestore() { render::FontEngine::set_text_aa_mode(saved_); }
    AaModeRestore(const AaModeRestore &) = delete;
    auto operator=(const AaModeRestore &) -> AaModeRestore & = delete;
    AaModeRestore(AaModeRestore &&) = delete;
    auto operator=(AaModeRestore &&) -> AaModeRestore & = delete;

  private:
    render::TextAAMode saved_;
};

// ---- 属性权威表的观测设施（属性单一声明迁移批） ----

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class ColumnProbe final : public Column {
  public:
    using Column::Column;
    using Column::present;
    using Column::resolve;
};

/// 同上，行容器的探针。
class RowProbe final : public Row {
  public:
    using Row::present;
    using Row::resolve;
    using Row::Row;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct ContainersDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    template <typename W>
    auto attach_to(W &target) -> void {
        paints = 0;
        layouts = 0;
        target.on_dirty = [this](bool layout) {
            if (layout) {
                ++layouts;
            } else {
                ++paints;
            }
        };
    }
};

/// 「带上下文的 gap 读件」形态：常量档行不应存在这第二件。
/// 用概念而非裸 `requires` 式判定：GCC 在非模板上下文把非法简单要求判成硬错误而非 false。
template <typename W>
concept ScopedGapReader = requires(const W &probe, const BuildContext &scope) { probe.gap(scope); };

/// 同上，对主轴尺寸策略行。
template <typename W>
concept ScopedMainAxisSizeReader = requires(const W &probe, const BuildContext &scope) { probe.main_axis_size(scope); };

}  // namespace

AURORA_TEST_CASE(initializer_list_registers_children_in_order) {
    Column col{box(10.0F, 10.0F), box(20.0F, 10.0F), box(30.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(std::string{col.type_name()}, "Column");
    AURORA_TEST_REQUIRE_EQ(col.child_nodes().size(), 3U);
    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().size.width, 10.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().size.width, 20.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[2].bounds().size.width, 30.0F, 0.0F);
    // 纵向依序落位：第 2 项 y = 10 + 10（前项高 + 无 gap）。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[2].bounds().origin.y, 20.0F, 1e-4F);

    Row row{box(10.0F, 10.0F), box(20.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(std::string{row.type_name()}, "Row");
    AURORA_TEST_REQUIRE_EQ(row.child_nodes().size(), 2U);
    LayoutEngine::layout(row, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.x, 10.0F, 1e-4F);
}

AURORA_TEST_CASE(column_gap_places_children_vertically) {
    Column col;
    col.add(box(100.0F, 20.0F));
    col.add(box(100.0F, 20.0F));
    col.set_gap(10.0F);

    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.y, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(row_gap_places_children_horizontally) {
    Row row;
    row.add(box(40.0F, 20.0F));
    row.add(box(60.0F, 20.0F));
    row.set_gap(8.0F);

    LayoutEngine::layout(row, bounded(300.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.x, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(column_and_row_reverse_keep_own_axis) {
    // 主轴归属控件类型：容器不再接受外部 Flex，轴向无从被异轴取值带偏（曾因部分指定的
    // `Flex{.main_axis = X}` 把 direction 打回 Row，让 Column 横向排布）。取向只剩本轴 Reverse 一档。
    Column col({box(100.0F, 20.0F), box(100.0F, 20.0F)}, ColumnProps()
                                                             .set_main_axis_alignment(MainAxisAlignment::Center)
                                                             .set_cross_axis_alignment(CrossAxisAlignment::Center));
    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_FALSE(col.reverse());
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.y, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);

    Column rev_col({box(100.0F, 20.0F), box(100.0F, 20.0F)});
    rev_col.set_reverse();
    AURORA_TEST_CHECK_TRUE(rev_col.reverse());
    LayoutEngine::layout(rev_col, bounded(200.0F, 200.0F));
    // 纵向反序：首项落底（容器高 40 - 子项高 20）。
    AURORA_TEST_CHECK_NEAR(rev_col.child_nodes()[0].bounds().origin.y, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rev_col.child_nodes()[1].bounds().origin.y, 0.0F, 1e-4F);

    Row row({box(40.0F, 20.0F), box(60.0F, 20.0F)}, RowProps().set_main_axis_alignment(MainAxisAlignment::Center));
    LayoutEngine::layout(row, bounded(300.0F, 100.0F));
    AURORA_TEST_CHECK_FALSE(row.reverse());
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.y, 0.0F, 1e-4F);

    Row rev_row({box(40.0F, 20.0F), box(60.0F, 20.0F)});
    rev_row.set_reverse();
    LayoutEngine::layout(rev_row, bounded(300.0F, 100.0F));
    // 横向反序：首项贴右缘（容器宽 100 - 首项宽 40），次项占剩余左段。
    AURORA_TEST_CHECK_NEAR(rev_row.child_nodes()[0].bounds().origin.x, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rev_row.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(column_main_axis_size_max_fills_height) {
    Column col;
    col.add(box(100.0F, 20.0F));
    col.set_main_axis_size(MainAxisSize::Max);

    LayoutEngine::layout(col, bounded(200.0F, 150.0F));
    const Size s = col.size();
    AURORA_TEST_CHECK_NEAR(s.height, 150.0F, 1e-4F);
    // 默认 Start 对齐：子项仍在顶部。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(column_main_axis_end_pushes_children_to_bottom) {
    Column col;
    col.add(box(100.0F, 20.0F));
    col.add(box(100.0F, 20.0F));
    col.set_main_axis_size(MainAxisSize::Max);
    col.set_main_axis_alignment(MainAxisAlignment::End);

    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    // 占用 40，End：首项 y = 200-40 = 160。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.y, 160.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.y, 180.0F, 1e-4F);
}

AURORA_TEST_CASE(cross_axis_stretch_expands_child) {
    // Row 交叉轴（竖直）Stretch：height 为 Expand 的子项被拉伸到容器高。
    Row row;
    row.add(box_cross_expand(30.0F, false));  // 宽固定 30，高 Expand
    row.set_cross_axis_alignment(CrossAxisAlignment::Stretch);

    LayoutEngine::layout(row, bounded(300.0F, 100.0F));
    const Size s = row.size();
    AURORA_TEST_CHECK_NEAR(s.height, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().size.height, 100.0F, 1e-4F);

    // Column 交叉轴（水平）Stretch：width 为 Expand 的子项被拉伸到容器宽。
    Column col;
    col.add(box_cross_expand(20.0F, true));  // 宽 Expand，高固定 20
    col.set_cross_axis_alignment(CrossAxisAlignment::Stretch);
    LayoutEngine::layout(col, bounded(200.0F, 100.0F));
    const Size cs = col.size();
    AURORA_TEST_CHECK_NEAR(cs.width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().size.width, 200.0F, 1e-4F);
}

/// @brief 判定诊断列表中是否含「Column + Baseline」降级提示
///        （消息取自 Column::on_layout 的英文提示开头、where 为 layout）。
auto has_column_baseline_notice(const std::vector<Diagnostic> &diags) -> bool {
    return std::ranges::any_of(diags, [](const auto &d) {
        return std::string{d.where} == "layout" && std::string{d.message}.starts_with("The cross axis of Column");
    });
}

AURORA_TEST_CASE(row_baseline_aligns_text_widgets_on_common_line) {
    // Row + Baseline：不同字号的两个 Text 首行基线共线。Text 的钩子即有效字体 ascent，
    // 容器按 cross_pos = max_above - ascent 定位 ⇒ 各子项「bounds.y + ascent」相等。
    auto small = std::make_shared<Text>("small");
    small->font_size(12.0F);
    auto large = std::make_shared<Text>("large");
    large->font_size(24.0F);

    Row row;
    row.add(Node{std::shared_ptr<Widget>(small)});
    row.add(Node{std::shared_ptr<Widget>(large)});
    row.set_cross_axis_alignment(CrossAxisAlignment::Baseline);

    LayoutEngine::layout(row, bounded(400.0F, 200.0F));

    const float ascent_small = render::FontEngine::measure_ascent(Font{.size_pt = 12.0F});
    const float ascent_large = render::FontEngine::measure_ascent(Font{.size_pt = 24.0F});
    const float baseline_small = row.child_nodes()[0].bounds().origin.y + ascent_small;
    const float baseline_large = row.child_nodes()[1].bounds().origin.y + ascent_large;
    AURORA_TEST_CHECK_NEAR(baseline_small, baseline_large, 1e-3F);
    // 大字号上沿更高 ⇒ 小字号整体下移（基线对齐的必然结果）。
    AURORA_TEST_CHECK_GT(row.child_nodes()[0].bounds().origin.y, row.child_nodes()[1].bounds().origin.y);
    // 容器交叉轴 = max(基线上沿) + max(基线下沿)，其中下沿按各子项「实测高 - 自身基线」取最大。
    const float h_small = row.child_nodes()[0].bounds().size.height;
    const float h_large = row.child_nodes()[1].bounds().size.height;
    const float expected_cross = ascent_large + std::max(h_small - ascent_small, h_large - ascent_large);
    AURORA_TEST_CHECK_NEAR(row.size().height, expected_cross, 1e-3F);
}

AURORA_TEST_CASE(row_baseline_accounts_for_modifier_padding) {
    // 带 `Modifier::padding` 的 Text：钩子给「内容盒顶 → 基线」，容器须补 Modifier 的内容盒位移，
    // 否则带内边距的文本基线会整体上移 padding.top。
    auto plain = std::make_shared<Text>("Ag");
    plain->font_size(16.0F);
    auto padded = std::make_shared<Text>("Ag");
    padded->font_size(16.0F);
    padded->set_modifier(Modifier().padding(10.0F));

    Row row;
    row.add(Node{std::shared_ptr<Widget>(plain)});
    row.add(Node{std::shared_ptr<Widget>(padded)});
    row.set_cross_axis_alignment(CrossAxisAlignment::Baseline);
    LayoutEngine::layout(row, bounded(400.0F, 200.0F));

    const float ascent = render::FontEngine::measure_ascent(Font{.size_pt = 16.0F});
    const float baseline_plain = row.child_nodes()[0].bounds().origin.y + ascent;
    const float baseline_padded = row.child_nodes()[1].bounds().origin.y + 10.0F + ascent;
    AURORA_TEST_CHECK_NEAR(baseline_plain, baseline_padded, 1e-3F);
    // 带内边距者内容盒下移 10dp ⇒ 其布局盒须整体上移 10dp 才能与前者的基线共线
    // （即 max_above 由 padding 那一项决定）。
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.y, 0.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.y, 10.0F, 1e-3F);
}

AURORA_TEST_CASE(row_baseline_mixes_button_and_text) {
    // Button（内边距 + 标签垂直居中）与 Text 同行：按钮钩子 = 居中偏移 + ascent（见 paint_label），
    // 与 Text 的 ascent 在容器内共线。
    auto button = std::make_shared<Button>("OK");
    auto label = std::make_shared<Text>("ok");
    label->font_size(14.0F);

    Row row;
    row.add(Node{std::shared_ptr<Widget>(button)});
    row.add(Node{std::shared_ptr<Widget>(label)});
    row.set_cross_axis_alignment(CrossAxisAlignment::Baseline);
    LayoutEngine::layout(row, bounded(400.0F, 200.0F));

    const Font btn_font{.size_pt = 14.0F};
    const float ascent = render::FontEngine::measure_ascent(btn_font);
    const float text_h = render::FontEngine::measure_height(btn_font);
    const float btn_h = row.child_nodes()[0].bounds().size.height;
    const float baseline_button = row.child_nodes()[0].bounds().origin.y + ((btn_h - text_h) * 0.5F) + ascent;
    const float baseline_text = row.child_nodes()[1].bounds().origin.y + ascent;
    AURORA_TEST_CHECK_NEAR(baseline_button, baseline_text, 1e-3F);
}

AURORA_TEST_CASE(column_baseline_falls_back_to_start_and_warns_once) {
    (void)Diagnostics::take();  // 清场：只观测本用例产生的提示
    auto a = std::make_shared<Text>("A");
    a->font_size(20.0F);
    auto b = std::make_shared<Text>("B");
    b->font_size(20.0F);

    Column col;
    col.add(Node{std::shared_ptr<Widget>(a)});
    col.add(Node{std::shared_ptr<Widget>(b)});
    col.set_cross_axis_alignment(CrossAxisAlignment::Baseline);

    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    // Column 交叉轴是水平的：按 Start（x 均为 0），不崩。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_MSG(has_column_baseline_notice(Diagnostics::take()),
                          "Column + Baseline must emit one degraded notice");

    // 重复布局不再提示（每实例一次，避免逐帧刷屏）。
    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_MSG(!has_column_baseline_notice(Diagnostics::take()),
                          "the degraded notice must not repeat per frame");
}

AURORA_TEST_CASE(validate_props_rejects_negative_gap) {
    Column bad;
    bad.set_gap(-1.0F);
    AURORA_TEST_CHECK_FALSE(bad.validate_props().ok());

    Column good;
    good.set_gap(4.0F);
    AURORA_TEST_CHECK_TRUE(good.validate_props().ok());
}

AURORA_TEST_CASE(props_serialize_deserialize_roundtrip) {
    Column src;
    src.set_main_axis_alignment(MainAxisAlignment::SpaceBetween);
    src.set_cross_axis_alignment(CrossAxisAlignment::Center);
    src.set_main_axis_size(MainAxisSize::Max);
    src.set_gap(12.0F);

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "main_axis_alignment"), "SpaceBetween");
    AURORA_TEST_CHECK_EQ(require_field<float>(props, "gap"), 12.0F);

    Column dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.main_axis_alignment() == MainAxisAlignment::SpaceBetween);
    AURORA_TEST_CHECK_TRUE(dst.cross_axis_alignment() == CrossAxisAlignment::Center);
    AURORA_TEST_CHECK_TRUE(dst.main_axis_size() == MainAxisSize::Max);
    AURORA_TEST_CHECK_NEAR(dst.gap(), 12.0F, 0.0F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Column::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Column");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
    bool has_gap = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "gap") {
            has_gap = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_gap);
}

AURORA_TEST_CASE(row_rtl_via_environment_mirrors_children) {
    // 布局镜像端到端：经 Environment 注入 Directionality(RTL) 后，
    // Row 子项视觉顺序翻转（Start 排布首项贴右缘）——容器按 resolved_text_direction 注入。
    const Environment env =
        Environment{}.with<Directionality>(Directionality{.direction = TextDirection::RTL, .host_set = true});
    BuildContext ctx;
    ctx.env = &env;

    Row row;
    row.add(box(40.0F, 20.0F));
    row.add(box(60.0F, 20.0F));

    row.mount(ctx);
    row.layout(bounded(300.0F, 100.0F), ctx);

    // 内容宽 100：LTR 时 child0@0、child1@40；RTL 镜像后 child0@60、child1@0。
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.x, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);

    // 无环境注入的对照组：保持 LTR 物理序（镜像不生效）。
    Row ltr;
    ltr.add(box(40.0F, 20.0F));
    ltr.add(box(60.0F, 20.0F));
    BuildContext plain;
    ltr.mount(plain);
    ltr.layout(bounded(300.0F, 100.0F), plain);
    AURORA_TEST_CHECK_NEAR(ltr.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ltr.child_nodes()[1].bounds().origin.x, 40.0F, 1e-4F);
}

AURORA_TEST_CASE(column_subtree_re_records_when_text_aa_mode_changes) {
    const AaModeRestore guard;
    render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);
    const auto col = text_column(false);
    const auto before = paint_hash(*col);

    // 切换 AA 模式后只给根标脏：后代 Display List 也须按光栅世代失效并重录，
    // 否则回放的是录制时固化的旧光栅（表现为「切换瞬间无变化、过一会儿才零星生效」）。
    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
    col->mark_needs_paint();
    AURORA_TEST_CHECK_TRUE(paint_hash(*col) != before);
}

AURORA_TEST_CASE(column_cache_layer_rebuilds_when_text_aa_mode_changes) {
    const AaModeRestore guard;
    render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);
    const auto col = text_column(true);
    const auto before = paint_hash(*col);

    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
    col->mark_needs_paint();
    AURORA_TEST_CHECK_TRUE(paint_hash(*col) != before);
}

AURORA_TEST_CASE(column_subtree_cache_hits_when_raster_state_unchanged) {
    const AaModeRestore guard;
    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
    const auto col = text_column(false);
    const auto first = paint_hash(*col);
    // 光栅状态未变：标脏重绘后像素必须完全一致——世代校验不得误伤缓存命中。
    col->mark_needs_paint();
    AURORA_TEST_CHECK_EQ(paint_hash(*col), first);
}

AURORA_TEST_CASE(default_extra_hit_box_equals_own_layout_box) {
    // 追加命中盒的缺省必须「等于没有」：Container 下降闸的判定逐点不变。
    // 两条判据：① 树上任何控件都不额外声明命中盒；② 密集取点上，命中链最深节点与
    // 「只按子布局盒判定」的参考实现逐点相同（参考实现即改动前的闸，故本用例可捕获任何漂移）。
    auto root = std::make_shared<Column>(
        std::vector<Node>{Node{Button(ButtonProps().set_label("A"))}, Node{Button(ButtonProps().set_label("B"))},
                          Node{Button(ButtonProps().set_label("C"))}},
        ColumnProps().set_gap(8.0F));
    LayoutEngine::layout(*root, bounded(240.0F, 200.0F));
    AURORA_TEST_REQUIRE_EQ(root->child_count(), 3U);

    for (const Node &n : root->child_nodes()) {
        AURORA_TEST_CHECK_FALSE(n.widget().extra_hit_box(BuildContext{}).has_value());
    }
    AURORA_TEST_CHECK_FALSE(root->extra_hit_box(BuildContext{}).has_value());

    // 参考实现：按改动前的闸（只看子布局盒）反向遍历，第一个命中的子即最深节点。
    auto reference = [&root](const Point &p) -> const Widget * {
        const auto &kids = root->child_nodes();
        for (std::size_t i = kids.size(); i > 0; --i) {
            const Node &n = kids[i - 1];
            if (n.bounds().contains(p)) {
                return &n.widget();
            }
        }
        return nullptr;
    };

    const Rect host{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = root->size()};
    for (int iy = 0; iy < 80; ++iy) {
        const float y = static_cast<float>(iy) * 2.5F;
        for (int ix = 0; ix < 15; ++ix) {
            const float x = static_cast<float>(ix) * 17.0F;
            const Point probe{.x = x, .y = y};
            const std::vector<HitNode> chain = root->hit_test_chain(probe, host, BuildContext{});
            const Widget *actual = chain.empty() ? nullptr : chain.back().get();
            AURORA_TEST_CHECK_EQ(actual, reference(probe));
        }
    }
}

// ===========================================================================
// 权威表的读消费面（属性单一声明迁移批）：逐行撤表态该键退盘 / 四枚常量档两档恒等 /
// 全量键复原落同一帧几何 / 非法值降级 / 挂载后逐档标脏而反向开关保持控件级
// ===========================================================================

AURORA_TEST_CASE(column_row_clear_unstates_each_row_and_drops_every_key) {
    // 四行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且取值回到该行回落点」。
    // 子项与反向开关都不进表：序列化里既没有 children，也没有 flex / direction / reverse。
    ColumnProbe c(std::vector<Node>{box(10.0F, 10.0F), box(20.0F, 10.0F)});
    c.set_main_axis_alignment(MainAxisAlignment::SpaceBetween)
        .set_cross_axis_alignment(CrossAxisAlignment::Stretch)
        .set_main_axis_size(MainAxisSize::Max)
        .set_gap(6.0F);
    Json stated = Json::object();
    c.serialize_props(stated);
    for (const char *key : {"main_axis_alignment", "cross_axis_alignment", "main_axis_size", "gap"}) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 四行全表态 ⇒ 四键逐键在盘
    }
    for (const char *key : {"children", "flex", "direction", "reverse"}) {
        AURORA_TEST_CHECK_FALSE(stated.contains(key));  // 表外名字（子项 / 反向）不是属性键
    }

    c.clear(ColumnProps::main_axis_alignment_row());
    AURORA_TEST_CHECK_FALSE(c.present(ColumnProps::main_axis_alignment_row()));
    AURORA_TEST_CHECK(c.main_axis_alignment() == MainAxisAlignment::Start);  // 回落基线
    c.clear(ColumnProps::cross_axis_alignment_row());
    AURORA_TEST_CHECK_FALSE(c.present(ColumnProps::cross_axis_alignment_row()));
    AURORA_TEST_CHECK(c.cross_axis_alignment() == CrossAxisAlignment::Start);
    c.clear(ColumnProps::main_axis_size_row());
    AURORA_TEST_CHECK_FALSE(c.present(ColumnProps::main_axis_size_row()));
    AURORA_TEST_CHECK(c.main_axis_size() == MainAxisSize::Min);
    c.clear(ColumnProps::gap_row());
    AURORA_TEST_CHECK_FALSE(c.present(ColumnProps::gap_row()));
    AURORA_TEST_CHECK_NEAR(c.gap(), 0.0F, 1e-6F);

    Json cleared = Json::object();
    c.serialize_props(cleared);
    for (const char *key : {"main_axis_alignment", "cross_axis_alignment", "main_axis_size", "gap"}) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }
    AURORA_TEST_CHECK_EQ(c.child_count(), 2U);  // 撤表态不动子项所有权

    // 行容器同形：同一张表形态、同一批键、同一套撤表态动作。
    RowProbe r;
    r.set_main_axis_alignment(MainAxisAlignment::End)
        .set_cross_axis_alignment(CrossAxisAlignment::Center)
        .set_main_axis_size(MainAxisSize::Max)
        .set_gap(5.0F);
    Json row_stated = Json::object();
    r.serialize_props(row_stated);
    for (const char *key : {"main_axis_alignment", "cross_axis_alignment", "main_axis_size", "gap"}) {
        AURORA_TEST_CHECK_TRUE(row_stated.contains(key));
    }
    r.clear(RowProps::main_axis_alignment_row());
    r.clear(RowProps::cross_axis_alignment_row());
    r.clear(RowProps::main_axis_size_row());
    r.clear(RowProps::gap_row());
    Json row_cleared = Json::object();
    r.serialize_props(row_cleared);
    for (const char *key : {"main_axis_alignment", "cross_axis_alignment", "main_axis_size", "gap"}) {
        AURORA_TEST_CHECK_FALSE(row_cleared.contains(key));
    }
}

AURORA_TEST_CASE(column_row_constant_rows_read_identically_under_light_and_dark) {
    // 恒等关系的正面取证：四行全是常量档，没有 `x(ctx)` 第二件，其无参读件**就是**生效值——
    // 明 / 暗两档主题下与库内 `resolve(row, ctx)` 取值全同；缺这组断言则「窄面不丢语义」只是断言。
    static_assert(!ScopedGapReader<Column>);
    static_assert(!ScopedGapReader<Row>);
    static_assert(!ScopedMainAxisSizeReader<Column>);
    static_assert(!ScopedMainAxisSizeReader<Row>);

    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    ColumnProbe c;
    AURORA_TEST_CHECK(c.main_axis_alignment() == c.resolve(ColumnProps::main_axis_alignment_row(), light));
    AURORA_TEST_CHECK(c.main_axis_alignment() == c.resolve(ColumnProps::main_axis_alignment_row(), dark));
    AURORA_TEST_CHECK(c.cross_axis_alignment() == c.resolve(ColumnProps::cross_axis_alignment_row(), light));
    AURORA_TEST_CHECK(c.cross_axis_alignment() == c.resolve(ColumnProps::cross_axis_alignment_row(), dark));
    AURORA_TEST_CHECK(c.main_axis_size() == c.resolve(ColumnProps::main_axis_size_row(), light));
    AURORA_TEST_CHECK(c.main_axis_size() == c.resolve(ColumnProps::main_axis_size_row(), dark));
    AURORA_TEST_CHECK_NEAR(c.gap(), c.resolve(ColumnProps::gap_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(c.gap(), c.resolve(ColumnProps::gap_row(), dark), 1e-6F);

    // 表态后仍与解析同值（两半都恒等，才排除「无参读件读的是另一份存储」）；
    // 控件面同名写件返回控件引用，链式表的是「同一个对象」而非第二份属性。
    Column &chain = c.set_gap(5.0F).set_main_axis_size(MainAxisSize::Max);
    AURORA_TEST_CHECK(&chain == static_cast<Column *>(&c));
    AURORA_TEST_CHECK_NEAR(c.gap(), 5.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(c.gap(), c.resolve(ColumnProps::gap_row(), light), 1e-6F);
    AURORA_TEST_CHECK(c.main_axis_size() == MainAxisSize::Max);
    AURORA_TEST_CHECK(c.main_axis_size() == c.resolve(ColumnProps::main_axis_size_row(), dark));

    RowProbe r;
    AURORA_TEST_CHECK(r.main_axis_alignment() == r.resolve(RowProps::main_axis_alignment_row(), light));
    AURORA_TEST_CHECK(r.cross_axis_alignment() == r.resolve(RowProps::cross_axis_alignment_row(), dark));
    AURORA_TEST_CHECK(r.main_axis_size() == r.resolve(RowProps::main_axis_size_row(), light));
    AURORA_TEST_CHECK_NEAR(r.gap(), r.resolve(RowProps::gap_row(), dark), 1e-6F);
    Row &row_chain = r.set_cross_axis_alignment(CrossAxisAlignment::Stretch);
    AURORA_TEST_CHECK(&row_chain == static_cast<Row *>(&r));
    AURORA_TEST_CHECK(r.cross_axis_alignment() == CrossAxisAlignment::Stretch);
}

AURORA_TEST_CASE(column_row_full_key_json_restores_every_effective_value) {
    // 逐行表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态（旧写件恒写四键）；
    // 把它灌回一份空白属性对象 + 同样的子项，每行都必须重新表态且生效值逐键相等，
    // 最终落同一帧几何：每个子项的布局盒逐值相同（布局级的「同一帧像素」）。
    Column src(std::vector<Node>{box(10.0F, 10.0F), box(20.0F, 10.0F), box(30.0F, 10.0F)},
               ColumnProps()
                   .set_main_axis_alignment(MainAxisAlignment::SpaceBetween)
                   .set_cross_axis_alignment(CrossAxisAlignment::Center)
                   .set_main_axis_size(MainAxisSize::Max)
                   .set_gap(4.0F));
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : {"main_axis_alignment", "cross_axis_alignment", "main_axis_size", "gap"}) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }

    ColumnProbe dst(std::vector<Node>{box(10.0F, 10.0F), box(20.0F, 10.0F), box(30.0F, 10.0F)});
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(ColumnProps::main_axis_alignment_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ColumnProps::cross_axis_alignment_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ColumnProps::main_axis_size_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ColumnProps::gap_row()));
    AURORA_TEST_CHECK(dst.main_axis_alignment() == MainAxisAlignment::SpaceBetween);
    AURORA_TEST_CHECK(dst.cross_axis_alignment() == CrossAxisAlignment::Center);
    AURORA_TEST_CHECK(dst.main_axis_size() == MainAxisSize::Max);
    AURORA_TEST_CHECK_NEAR(dst.gap(), 4.0F, 1e-6F);

    const BuildContext ctx;
    LayoutEngine::layout(src, bounded(200.0F, 120.0F), ctx);
    LayoutEngine::layout(dst, bounded(200.0F, 120.0F), ctx);
    AURORA_TEST_REQUIRE_NEAR(src.size().width, dst.size().width, 1e-4F);
    AURORA_TEST_REQUIRE_NEAR(src.size().height, dst.size().height, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(src.child_nodes().size(), dst.child_nodes().size());
    for (std::size_t i = 0; i < src.child_nodes().size(); ++i) {
        const Rect a = src.child_nodes()[i].bounds();
        const Rect b = dst.child_nodes()[i].bounds();
        AURORA_TEST_CHECK_NEAR(a.origin.x, b.origin.x, 1e-4F);
        AURORA_TEST_CHECK_NEAR(a.origin.y, b.origin.y, 1e-4F);
        AURORA_TEST_CHECK_NEAR(a.size.width, b.size.width, 1e-4F);
        AURORA_TEST_CHECK_NEAR(a.size.height, b.size.height, 1e-4F);
    }

    // 行容器同形：同一份全量键 JSON 在另一张同形表上逐行复原。
    Row row_src(std::vector<Node>{box(10.0F, 10.0F), box(10.0F, 20.0F)},
                RowProps()
                    .set_main_axis_alignment(MainAxisAlignment::SpaceEvenly)
                    .set_cross_axis_alignment(CrossAxisAlignment::End)
                    .set_main_axis_size(MainAxisSize::Max)
                    .set_gap(3.0F));
    Json row_legacy = Json::object();
    row_src.serialize_props(row_legacy);
    RowProbe row_dst(std::vector<Node>{box(10.0F, 10.0F), box(10.0F, 20.0F)});
    row_dst.deserialize_props(row_legacy);
    AURORA_TEST_CHECK_TRUE(row_dst.present(RowProps::main_axis_alignment_row()));
    AURORA_TEST_CHECK_TRUE(row_dst.present(RowProps::gap_row()));
    AURORA_TEST_CHECK(row_dst.main_axis_alignment() == MainAxisAlignment::SpaceEvenly);
    AURORA_TEST_CHECK(row_dst.cross_axis_alignment() == CrossAxisAlignment::End);
    AURORA_TEST_CHECK_NEAR(row_dst.gap(), 3.0F, 1e-6F);
    LayoutEngine::layout(row_src, bounded(200.0F, 120.0F), ctx);
    LayoutEngine::layout(row_dst, bounded(200.0F, 120.0F), ctx);
    AURORA_TEST_REQUIRE_EQ(row_src.child_nodes().size(), row_dst.child_nodes().size());
    for (std::size_t i = 0; i < row_src.child_nodes().size(); ++i) {
        AURORA_TEST_CHECK_NEAR(row_src.child_nodes()[i].bounds().origin.x, row_dst.child_nodes()[i].bounds().origin.x,
                               1e-4F);
        AURORA_TEST_CHECK_NEAR(row_src.child_nodes()[i].bounds().size.width,
                               row_dst.child_nodes()[i].bounds().size.width, 1e-4F);
    }
}

AURORA_TEST_CASE(column_row_invalid_json_degrades_and_enum_names_stay_lenient) {
    // 非法值一律不写入：可空行撤表态退回该行回落点，且降级经 `Diagnostics` 上报。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    // ① 数值行越界（`gap` 下界 0）⇒ 撤表态，生效值回 0，而不是把 -3 烘进字段。
    ColumnProbe c;
    c.set_gap(2.0F);
    Json below_min = Json::object();
    below_min.set("gap", Json{-3.0});
    c.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(c.present(ColumnProps::gap_row()));  // 撤的是表态位
    AURORA_TEST_CHECK_NEAR(c.gap(), 0.0F, 1e-6F);  // 退回回落点而非保留 2.0 / 烘进 -3.0
    AURORA_TEST_CHECK(Diagnostics::count() >= 1);

    // ② 数值行形态不符（喂串）⇒ 同样拒写并撤表态。
    c.set_gap(2.0F);
    Json wrong_type = Json::object();
    wrong_type.set("gap", Json{"wide"});
    c.deserialize_props(wrong_type);
    AURORA_TEST_CHECK_FALSE(c.present(ColumnProps::gap_row()));
    AURORA_TEST_CHECK_NEAR(c.gap(), 0.0F, 1e-6F);

    // ③ 枚举名走的是另一条通路：`PropCodec<枚举>` 未登记 `validate`，未知名字按既定解析退
    //    默认档**且照常写入表态位**，不产生降级上报。
    const std::size_t reported = Diagnostics::count();
    c.clear(ColumnProps::main_axis_alignment_row());
    Json unknown_enum = Json::object();
    unknown_enum.set("main_axis_alignment", Json{"diagonal"});
    c.deserialize_props(unknown_enum);
    AURORA_TEST_CHECK_TRUE(c.present(ColumnProps::main_axis_alignment_row()));  // 宽容通路照样表态
    AURORA_TEST_CHECK(c.main_axis_alignment() == MainAxisAlignment::Start);  // 未知名 ⇒ 该枚举默认档
    AURORA_TEST_CHECK(Diagnostics::count() == reported);  // 不报降级

    // ④ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("main_axis_alignment", Json{"Center"});
    good.set("cross_axis_alignment", Json{"Stretch"});
    good.set("main_axis_size", Json{"Max"});
    good.set("gap", Json{4.0});
    c.deserialize_props(good);
    AURORA_TEST_CHECK(c.main_axis_alignment() == MainAxisAlignment::Center);
    AURORA_TEST_CHECK(c.cross_axis_alignment() == CrossAxisAlignment::Stretch);
    AURORA_TEST_CHECK(c.main_axis_size() == MainAxisSize::Max);
    AURORA_TEST_CHECK_NEAR(c.gap(), 4.0F, 1e-6F);

    // 行容器同形：同一套降级口径不因控件而变。
    RowProbe r;
    r.set_gap(1.0F);
    Json row_bad = Json::object();
    row_bad.set("gap", Json{-2.0});
    r.deserialize_props(row_bad);
    AURORA_TEST_CHECK_FALSE(r.present(RowProps::gap_row()));
    AURORA_TEST_CHECK_NEAR(r.gap(), 0.0F, 1e-6F);

    Diagnostics::take();
}

AURORA_TEST_CASE(column_row_mounted_writers_mark_layout_tier_and_reverse_is_control_level) {
    // 四行都只声明布局档：挂载后每次写入都只落布局档，绘制档不落账；同值写入由行上的判等守卫短路。
    // 反向排布是控件级开关（既不是属性行也不序列化），只标布局档。
    Column c;
    const BuildContext ctx;
    c.mount(ctx);
    ContainersDirty dirty;
    dirty.attach_to(c);

    c.set_gap(3.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
    c.set_gap(3.0F);  // 同值：判等短路，不落账
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    c.set_main_axis_alignment(MainAxisAlignment::Center);
    c.set_cross_axis_alignment(CrossAxisAlignment::Stretch);
    c.set_main_axis_size(MainAxisSize::Max);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 4);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);

    AURORA_TEST_CHECK_FALSE(c.reverse());
    c.set_reverse();
    AURORA_TEST_CHECK_TRUE(c.reverse());
    AURORA_TEST_CHECK_EQ(dirty.layouts, 5);
    c.set_reverse();  // 同值：不落账
    AURORA_TEST_CHECK_EQ(dirty.layouts, 5);
    c.set_reverse(false);
    AURORA_TEST_CHECK_FALSE(c.reverse());
    AURORA_TEST_CHECK_EQ(dirty.layouts, 6);

    // 反向不改变四行的生效值，也不进序列化。
    c.set_reverse();
    AURORA_TEST_CHECK(c.main_axis_size() == MainAxisSize::Max);
    Json out = Json::object();
    c.serialize_props(out);
    for (const char *key : {"reverse", "direction", "flex"}) {
        AURORA_TEST_CHECK_FALSE(out.contains(key));
    }

    // 行容器同形：同一条控件级反向开关与同一档标脏。
    Row r;
    r.mount(ctx);
    ContainersDirty row_dirty;
    row_dirty.attach_to(r);
    r.set_gap(2.0F);
    r.set_reverse();
    AURORA_TEST_CHECK_TRUE(r.reverse());
    AURORA_TEST_CHECK_EQ(row_dirty.layouts, 2);
    AURORA_TEST_CHECK_EQ(row_dirty.paints, 0);
}

}  // namespace aurora::test_cases::utest_containers
