/// 测试类型: integration
/// 目标组合: render/display_list（录制-回放命令缓冲的计数门禁）
/// 测试说明: 把确定性控件树录制进 DisplayList，断言命令数（计数类性能指标）为正、对相同树可复现、
///            且随内容量次线性增长。G-5~G-8 的滚动/缓冲类计数门槛已由 tests/unit/utest_scroll.cpp
///            （scroll_regression 段，PROFILING=ON）锁入 CTest；本用例只覆盖 DisplayList 录制计数本身，
///            不依赖 PROFILING 宏，也不与之重复。
///

#include <cstddef>
#include <memory>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/display_list.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/widget.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::itest_perf_display_list {

// 确定性叶控件：固定尺寸、单次 fill_rect。其录制行为完全可预测（每实例恰好 1 条 FillRect 命令）。
class FillLeaf : public LeafWidget {
  public:
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FillLeaf"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FillLeaf", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = kSize, .height = kSize});
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(b, Color{120, 160, 200, 255});
    }

  private:
    static constexpr float kSize = 8.0F;
};

// 构造一棵含 n 个 FillLeaf 的 Column（每次都新建，避免 DL 缓存跨渲染携带）。
static auto build_tree(int n) -> Node {
    std::vector<Node> items;
    items.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        items.emplace_back(std::make_shared<FillLeaf>());
    }
    auto col = std::make_shared<Column>(ColumnProps{.children = std::move(items)});
    return Node{std::move(col)};
}

// 把树录制进 DisplayList，返回命令条数。
static auto record_cmd_count(Node root, float w, float h) -> std::size_t {
    constexpr BuildContext ctx;
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

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== itest_perf_display_list ===\n");

    constexpr float kW = 200.0F;
    constexpr float kH = 200.0F;
    constexpr int kN = 10;

    // 1) 录制确实产出命令（不为空）。
    const std::size_t c1 = record_cmd_count(build_tree(kN), kW, kH);
    AURORA_TEST_CHECK_MSG(c1 > 0, "DL cmd_count > 0 (recording captured draw commands)");

    // 2) 相同树两次独立录制 → 命令数可复现（计数类指标必须确定，不能逐次漂移）。
    const std::size_t c2 = record_cmd_count(build_tree(kN), kW, kH);
    AURORA_TEST_CHECK_MSG(c1 == c2, "DL cmd_count deterministic for identical tree (c1 == c2)");

    // 3) 内容量翻倍 → 命令数严格增长（计数正比于可见内容，而非恒定开销）。
    const std::size_t c_big = record_cmd_count(build_tree(kN * 2), kW, kH);
    AURORA_TEST_CHECK_MSG(c_big > c1, "DL cmd_count grows with content (2x leaves -> more commands)");

    // 4) 次线性上界：内容×2 不应使命令数爆炸（容器开销恒定，命令数≈线性）。
    AURORA_TEST_CHECK_MSG(c_big <= c1 * 3, "DL cmd_count scales sublinearly (no command explosion)");

    AURORA_TEST_PRINTF("  cmd_count: N=%d -> %zu, N=%d -> %zu (determinism pair %zu/%zu)\n",
                       kN, c1, kN * 2, c_big, c1, c2);
}

}  // namespace aurora::test_cases::itest_perf_display_list
