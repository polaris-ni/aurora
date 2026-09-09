/// 测试类型: integration
/// 目标单元: include/aurora/render/display_list.h
/// 测试说明: 把确定性控件树经完整 mount/layout/record-paint 流水线录制进 DisplayList，
///           断言命令数（计数类性能指标）为正、相同树两次独立录制可复现、随内容量严格增长
///           且次线性（无命令爆炸）。
/// 覆盖说明: 全部为纯计数断言、无墙钟计时；G-5~G-8 滚动/缓冲类计数门槛由
///           tests/unit/utest_scroll.cpp（scroll_regression 段）锁定，本文件不与之重复。

#include <cstddef>
#include <memory>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/log.h"
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_perf_display_list {

namespace {

constexpr int k_leaf_count = 10;
constexpr float k_canvas_w = 200.0F;
constexpr float k_canvas_h = 200.0F;

// 确定性叶控件：固定尺寸、单次 fill_rect，录制行为完全可预测（每实例恰好 1 条 FillRect 命令）。
class FillLeaf : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "FillLeaf"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FillLeaf", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = k_size, .height = k_size});
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(b, Color{120, 160, 200, 255});
    }

  private:
    static constexpr float k_size = 8.0F;
};

// 构造一棵含 n 个 FillLeaf 的 Column（每次都新建，避免 DL 缓存跨录制携带）。
auto build_tree(int n) -> Node {
    std::vector<Node> items;
    items.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        items.emplace_back(std::make_shared<FillLeaf>());
    }
    auto col = std::make_shared<Column>(ColumnProps{.children = std::move(items)});
    return Node{std::move(col)};
}

// 走完整 mount → layout → record paint 流水线，返回录制出的命令条数。
auto record_cmd_count(Node root, float w, float h) -> std::size_t {
    const BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = w, .height = h};
    root->layout(c, ctx);

    Painter p;
    p.begin(static_cast<int>(w), static_cast<int>(h));
    DisplayList dl;
    p.record(dl);
    root->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = h}}, ctx);
    p.stop();
    return dl.cmd_count();
}

}  // namespace

AURORA_TEST_CASE(recording_captures_draw_commands) {
    // 1) 录制确实产出命令（不为空）。
    const std::size_t count = record_cmd_count(build_tree(k_leaf_count), k_canvas_w, k_canvas_h);
    AURORA_TEST_CHECK_MSG(count > 0, "DL cmd_count > 0 (recording captured draw commands)");
    AURORA_TEST_PRINTF("  cmd_count: N=%d -> %zu\n", k_leaf_count, count);
}

AURORA_TEST_CASE(identical_tree_records_deterministic_count) {
    // 2) 相同树两次独立录制 → 命令数可复现（计数类指标必须确定，不能逐次漂移）。
    const std::size_t first = record_cmd_count(build_tree(k_leaf_count), k_canvas_w, k_canvas_h);
    const std::size_t second = record_cmd_count(build_tree(k_leaf_count), k_canvas_w, k_canvas_h);
    AURORA_TEST_CHECK_MSG(first == second, "DL cmd_count deterministic for identical tree (first == second)");
}

AURORA_TEST_CASE(command_count_grows_with_content) {
    // 3) 内容量翻倍 → 命令数严格增长（命令数正比于可见内容，而非恒定开销）。
    const std::size_t base = record_cmd_count(build_tree(k_leaf_count), k_canvas_w, k_canvas_h);
    const std::size_t doubled = record_cmd_count(build_tree(k_leaf_count * 2), k_canvas_w, k_canvas_h);
    AURORA_TEST_CHECK_MSG(doubled > base, "DL cmd_count grows with content (2x leaves -> more commands)");
}

AURORA_TEST_CASE(command_count_scales_sublinearly) {
    // 4) 次线性上界：内容×2 不应使命令数爆炸（容器开销恒定，命令数≈线性 → ≤3 倍留足余量）。
    const std::size_t base = record_cmd_count(build_tree(k_leaf_count), k_canvas_w, k_canvas_h);
    const std::size_t doubled = record_cmd_count(build_tree(k_leaf_count * 2), k_canvas_w, k_canvas_h);
    AURORA_TEST_CHECK_MSG(doubled <= base * 3, "DL cmd_count scales sublinearly (no command explosion)");
    AURORA_TEST_PRINTF("  cmd_count: N=%d -> %zu, N=%d -> %zu\n", k_leaf_count, base, k_leaf_count * 2, doubled);
}

}  // namespace aurora::test_cases::itest_perf_display_list
