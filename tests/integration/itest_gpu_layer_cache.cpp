/// 测试类型: integration
/// 目标单元: src/aurora/widget/widget.cpp（GPU 层缓存路径，specification/03 §8.7）
/// 测试说明: 验证 cache_layer 控件在录制 Painter（GPU 帧 DL 同形）下的层命令失效矩阵：
/// 首帧记 BeginLayer + 子树 + EndLayer + DrawLayer（子树重绘一次）；干净帧仅记 DrawLayer
///（子树零重绘）；invalidate_paint_cache / 尺寸变化 / epoch 推进均触发重录；层录制经
/// SoftwareRhi 回放与直接绘制像素一致；DrawLayer 冷存储未命中 bump epoch（单帧自愈信号）。

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/detail/gpu_layer.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "framework/aurora_test.h"

using au::BuildContext;
using au::CmdKind;
using au::Color;
using au::Constraints;
using au::DisplayList;
using au::DrawCmd;
using au::LeafWidget;
using au::Matrix2D;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Stack;

namespace aurora::test_cases::itest_gpu_layer_cache {

namespace {

class CountingBox : public LeafWidget {
  public:
    Size sz{.width = 100.0F, .height = 20.0F};
    Color color{255, 0, 0, 255};
    static int m_paint_count;

    [[nodiscard]] auto type_name() const -> const char * override { return "CountingBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        ++m_paint_count;
        p.fill_rect(b, color);
    }
};
int CountingBox::m_paint_count = 0;

/// @brief 记录型后端：捕获命令种类与层键（DisplayList 无命令只读访问器，经回放捕获）。
class KindRecorder final : public rhi::RhiBackend {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "kind-recorder"; }
    auto submit(const DrawCmd &cmd, const rhi::CmdData & /*data*/) -> void override {
        kinds.push_back(cmd.kind);
        if (cmd.kind == CmdKind::BeginLayer) {
            begin_key = cmd.aux_key;
        }
        if (cmd.kind == CmdKind::DrawLayer) {
            draw_keys.push_back(cmd.aux_key);
        }
    }
    std::vector<CmdKind> kinds;
    std::uint64_t begin_key = 0;
    std::vector<std::uint64_t> draw_keys;

    [[nodiscard]] auto count(CmdKind kind) const -> int {
        int n = 0;
        for (const auto k : kinds) {
            if (k == kind) {
                ++n;
            }
        }
        return n;
    }
};

/// @brief 组装并挂载布局（每帧全新根、同一子树——GPU 帧路径同形）。
auto make_root(const std::shared_ptr<CountingBox> &w, int ww, int hh) -> std::shared_ptr<Stack> {
    auto root = std::make_shared<Stack>(std::vector{Node{w}});
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
    root->layout(c, ctx);
    return root;
}

/// @brief 把控件**直接作为根**绘制录制进 DisplayList（中间无 DL 缓存容器拦截，
/// 层命令原样出现在帧 DL——与 cache_layer 控件直挂 GPU 帧根的行为同形）。
auto record_tree(const std::shared_ptr<CountingBox> &w, int ww, int hh) -> DisplayList {
    DisplayList dl;
    constexpr BuildContext ctx;
    w->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
    w->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    p.record(dl);
    // 绘制盒 = 布局产物尺寸（尺寸键失效判定依据 bounds.size，须真实反映 sz 变化）。
    w->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = w->size()}, ctx);
    p.stop();
    return dl;
}

/// @brief 统计 DL 中指定种类命令数（经记录型后端回放）。
auto count_kind(const DisplayList &dl, CmdKind kind) -> int {
    KindRecorder rec;
    dl.replay(rec);
    return rec.count(kind);
}

}  // namespace

AURORA_TEST_CASE(gpu_layer_first_paint_records_layer_commands) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(au::Modifier{}.cache_layer());

    // 首帧（缓存冷）：BeginLayer + 子树（on_paint 一次）+ EndLayer + DrawLayer。
    const auto dl = record_tree(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 1, "first paint renders subtree once");
    AURORA_TEST_CHECK_MSG(count_kind(dl, CmdKind::BeginLayer) == 1, "cold paint records BeginLayer");
    AURORA_TEST_CHECK_MSG(count_kind(dl, CmdKind::EndLayer) == 1, "cold paint records EndLayer");
    AURORA_TEST_CHECK_MSG(count_kind(dl, CmdKind::DrawLayer) == 1, "cold paint records DrawLayer");

    // 层键非零且 Begin/Draw 一致。
    KindRecorder rec;
    dl.replay(rec);
    AURORA_TEST_CHECK_MSG(rec.begin_key != 0, "layer key nonzero");
    AURORA_TEST_REQUIRE(rec.draw_keys.size() == 1U);
    AURORA_TEST_CHECK_MSG(rec.begin_key == rec.draw_keys[0], "BeginLayer/DrawLayer key consistent");
}

AURORA_TEST_CASE(gpu_layer_clean_paint_records_draw_layer_only) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(au::Modifier{}.cache_layer());

    (void)record_tree(cb, 200, 200);
    // 干净帧：仅一条 DrawLayer（子树零重绘、零像素搬运——GPU 常驻层纹理直接合成）。
    const auto dl2 = record_tree(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 1, "clean paint skips subtree repaint");
    AURORA_TEST_CHECK_MSG(count_kind(dl2, CmdKind::BeginLayer) == 0, "clean paint has no BeginLayer");
    AURORA_TEST_CHECK_MSG(count_kind(dl2, CmdKind::DrawLayer) == 1, "clean paint records DrawLayer only");
}

AURORA_TEST_CASE(gpu_layer_invalidate_rerecords_and_replay_matches_direct) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(au::Modifier{}.cache_layer());

    (void)record_tree(cb, 200, 200);
    (void)record_tree(cb, 200, 200);
    // 内容失效：重录完整层命令（on_paint 再执行一次）。
    cb->invalidate_paint_cache();
    const auto dl3 = record_tree(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2, "invalidate triggers re-render");
    AURORA_TEST_CHECK_MSG(count_kind(dl3, CmdKind::BeginLayer) == 1, "invalidated paint re-records BeginLayer");
    AURORA_TEST_CHECK_MSG(count_kind(dl3, CmdKind::DrawLayer) == 1, "invalidated paint records DrawLayer");

    // 层录制经软件回放与软件直绘（paint_cache_ 路径）像素一致（回退正确性红线）。
    Painter replayed;
    replayed.begin(200, 200);
    dl3.replay(replayed);
    AURORA_TEST_CHECK_MSG(replayed.get_pixel(50, 10).r == 255, "replayed layer shows red");

    const auto cb2 = std::make_shared<CountingBox>();
    cb2->modifier.set(au::Modifier{}.cache_layer());
    auto root2 = make_root(cb2, 200, 200);
    constexpr BuildContext ctx;
    Painter direct;
    direct.begin(200, 200);
    root2->paint(direct,
                 Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                      .size = Size{.width = 200.0F, .height = 200.0F}},
                 ctx);
    AURORA_TEST_CHECK_MSG(direct.get_pixel(50, 10).r == 255, "software direct path shows red");
}

AURORA_TEST_CASE(gpu_layer_epoch_bump_invalidates_cache) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(au::Modifier{}.cache_layer());

    (void)record_tree(cb, 200, 200);
    // 消费端层存储整体丢弃（epoch 推进）：控件下帧整体重录 BeginLayer（单帧自愈）。
    au::render::detail::bump_gpu_layer_epoch();
    const auto dl2 = record_tree(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2, "epoch bump triggers full re-record");
    AURORA_TEST_CHECK_MSG(count_kind(dl2, CmdKind::BeginLayer) == 1, "epoch bump re-records BeginLayer");
}

AURORA_TEST_CASE(gpu_layer_draw_miss_bumps_epoch_self_heal) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(au::Modifier{}.cache_layer());

    // 冷帧建层 + 干净帧 DrawLayer-only DL，喂给冷软件存储：未命中 → bump epoch（自愈信号）。
    (void)record_tree(cb, 200, 200);
    const auto clean = record_tree(cb, 200, 200);
    AURORA_TEST_REQUIRE(count_kind(clean, CmdKind::BeginLayer) == 0);
    const auto e0 = au::render::detail::gpu_layer_epoch();
    Painter cold;
    cold.begin(200, 200);
    clean.replay(cold);
    AURORA_TEST_CHECK_MSG(au::render::detail::gpu_layer_epoch() == e0 + 1,
                          "cold-store DrawLayer miss bumps epoch (self-heal signal)");
    // 自愈闭环：bump 后重录即恢复完整层命令。
    const auto dl2 = record_tree(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(count_kind(dl2, CmdKind::BeginLayer) == 1, "epoch bump leads to full re-record");
}

AURORA_TEST_CASE(gpu_layer_size_change_rerecords) {
    CountingBox::m_paint_count = 0;
    const auto cb = std::make_shared<CountingBox>();
    cb->modifier.set(au::Modifier{}.cache_layer());

    (void)record_tree(cb, 200, 200);
    // 尺寸变化：层尺寸键失配 → 重录（布局缓存契约同辙：须先 mark_needs_layout）。
    cb->sz = Size{.width = 50.0F, .height = 20.0F};
    cb->mark_needs_layout();
    const auto dl2 = record_tree(cb, 200, 200);
    AURORA_TEST_CHECK_MSG(CountingBox::m_paint_count == 2, "size change triggers re-render");
    AURORA_TEST_CHECK_MSG(count_kind(dl2, CmdKind::BeginLayer) == 1, "size change re-records BeginLayer");
}

}  // namespace aurora::test_cases::itest_gpu_layer_cache
