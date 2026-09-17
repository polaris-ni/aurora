/// 测试类型: unit
/// 目标单元: include/aurora/render/detail/gpu_layer.h + include/aurora/core/native_surface.h
///           + include/aurora/render/painter.h（GPU 层录制 API）+ include/aurora/render/rhi/software_rhi.h（层仿真）
/// 测试说明: 覆盖 GPU 层缓存与原生表面契约的软件侧：层键分配唯一性与 epoch 代际单调；
/// NativeSurfaceFrame 默认契约；Painter begin_layer/end_layer/draw_layer 仅录制生效
///（Direct no-op）且命令字段完整（含嵌套）；RhiBackend 默认契约（能力位全 false /
/// 流式接口 no-op）；SoftwareRhi 层捕获-离屏-存储-合成往返与直接绘制逐位一致、
/// DrawLayer 未命中跳过 + bump epoch 自愈、共享层存储跨实例命中。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/native_surface.h"
#include "aurora/core/transform.h"
#include "aurora/core/types.h"
#include "aurora/render/detail/gpu_layer.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/software_rhi.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_gpu_layers {

namespace {

using aurora::render::detail::bump_gpu_layer_epoch;
using aurora::render::detail::gpu_layer_epoch;
using aurora::render::detail::next_gpu_layer_key;

[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 记录型后端：捕获层命令的键 / 矩阵 / 缩放字段供断言（与 utest_rhi 同形）。
class LayerRecordingRhi final : public rhi::RhiBackend {
  public:
    struct Entry {
        CmdKind kind = CmdKind::FillRect;
        std::uint64_t aux_key = 0;
        Size bounds_size{};
        Matrix2D matrix{};
        float composite_scale = 0.0F;
    };

    [[nodiscard]] auto name() const -> std::string_view override { return "layer-recording"; }

    auto submit(const DrawCmd &cmd, const rhi::CmdData &data) -> void override {
        (void)data;
        Entry e;
        e.kind = cmd.kind;
        e.aux_key = cmd.aux_key;
        e.bounds_size = cmd.bounds.size;
        e.composite_scale = cmd.composite_scale;
        if (cmd.matrix_idx >= 0) {
            e.matrix = list_->matrix_at(cmd.matrix_idx);
        }
        entries.push_back(e);
    }

    std::vector<Entry> entries;
    DisplayList *list_ = nullptr;  // 测试助手：非 const（matrix_at 非只读接口）
};

}  // namespace

AURORA_TEST_CASE(gpu_layer_key_allocator_unique) {
    // 层键进程内唯一、非零（0 保留为无效），分配无锁可重复调用。
    const auto k1 = next_gpu_layer_key();
    const auto k2 = next_gpu_layer_key();
    AURORA_TEST_CHECK_TRUE(k1 != 0);
    AURORA_TEST_CHECK_TRUE(k2 != 0);
    AURORA_TEST_CHECK_TRUE(k1 != k2);
}

AURORA_TEST_CASE(gpu_layer_epoch_bump_monotonic) {
    // 代际读取 / 递增单调：bump 一次 +1，再 bump 再 +1（消费端冷存储丢弃的自愈信号）。
    const auto e0 = gpu_layer_epoch();
    bump_gpu_layer_epoch();
    AURORA_TEST_CHECK_EQ(gpu_layer_epoch(), e0 + 1);
    bump_gpu_layer_epoch();
    AURORA_TEST_CHECK_EQ(gpu_layer_epoch(), e0 + 2);
}

AURORA_TEST_CASE(native_surface_frame_defaults) {
    // 默认帧：None 种类 + 空句柄 + 零尺寸 + 可空 release 回调（产生方自管生命周期的零值形态）。
    const aurora::NativeSurfaceFrame frame;
    AURORA_TEST_CHECK_TRUE(frame.kind == aurora::NativeSurfaceKind::None);
    AURORA_TEST_CHECK_TRUE(frame.handle == nullptr);
    AURORA_TEST_CHECK_EQ(frame.width, 0);
    AURORA_TEST_CHECK_EQ(frame.height, 0);
    AURORA_TEST_CHECK_EQ(frame.format, 0U);
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(frame.release));
    // 非空 release 回调可执行（消费方按「先判空再调用」契约使用）。
    int released = 0;
    aurora::NativeSurfaceFrame owned;
    owned.release = [&released] { ++released; };
    owned.release();
    AURORA_TEST_CHECK_EQ(released, 1);
    // 五种平台种类可寻址（种类间互异）。
    AURORA_TEST_CHECK_TRUE(aurora::NativeSurfaceKind::DmaBuf != aurora::NativeSurfaceKind::IoSurface);
    AURORA_TEST_CHECK_TRUE(aurora::NativeSurfaceKind::D3D11Texture != aurora::NativeSurfaceKind::AHardwareBuffer);
}

AURORA_TEST_CASE(painter_layer_recording_commands) {
    constexpr std::uint64_t KEY_OUTER = 11;
    constexpr std::uint64_t KEY_INNER = 12;

    // 录制模式：层命令入 DL，字段完整（aux_key / 层尺寸 / 放置矩阵 / 录制缩放）。
    DisplayList dl;
    Painter rec;
    rec.begin(64, 32);
    rec.record(dl);
    rec.begin_layer(KEY_OUTER, Size{.width = 64.0F, .height = 32.0F});
    rec.begin_layer(KEY_INNER, Size{.width = 32.0F, .height = 16.0F});
    rec.fill_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), Color::red());
    rec.end_layer();
    rec.end_layer();
    rec.draw_layer(KEY_OUTER, Matrix2D::from_translate(2.0F, 3.0F), 1.5F);
    rec.stop();

    LayerRecordingRhi recorder;
    recorder.list_ = &dl;
    dl.replay(recorder);

    AURORA_TEST_REQUIRE(recorder.entries.size() == 6U);
    AURORA_TEST_CHECK_TRUE(recorder.entries[0].kind == CmdKind::BeginLayer);
    AURORA_TEST_CHECK_EQ(recorder.entries[0].aux_key, KEY_OUTER);
    AURORA_TEST_CHECK_EQ(recorder.entries[0].bounds_size.width, 64.0F);
    AURORA_TEST_CHECK_EQ(recorder.entries[0].bounds_size.height, 32.0F);
    AURORA_TEST_CHECK_TRUE(recorder.entries[1].kind == CmdKind::BeginLayer);
    AURORA_TEST_CHECK_EQ(recorder.entries[1].aux_key, KEY_INNER);
    AURORA_TEST_CHECK_EQ(recorder.entries[1].bounds_size.width, 32.0F);
    AURORA_TEST_CHECK_TRUE(recorder.entries[2].kind == CmdKind::FillRect);
    AURORA_TEST_CHECK_TRUE(recorder.entries[3].kind == CmdKind::EndLayer);
    AURORA_TEST_CHECK_TRUE(recorder.entries[4].kind == CmdKind::EndLayer);
    AURORA_TEST_CHECK_TRUE(recorder.entries[5].kind == CmdKind::DrawLayer);
    AURORA_TEST_CHECK_EQ(recorder.entries[5].aux_key, KEY_OUTER);
    AURORA_TEST_CHECK_TRUE(std::fabs(recorder.entries[5].matrix.tx - 2.0F) < 1e-6F);
    AURORA_TEST_CHECK_TRUE(std::fabs(recorder.entries[5].matrix.ty - 3.0F) < 1e-6F);
    AURORA_TEST_CHECK_EQ(recorder.entries[5].composite_scale, 1.5F);

    // Direct 模式：层 API 为 no-op（软件直绘走 paint_cache_ 路径），后续绘制不受影响。
    Painter direct;
    direct.begin(8, 8);
    direct.begin_layer(KEY_OUTER, Size{.width = 4.0F, .height = 4.0F});
    direct.end_layer();
    direct.draw_layer(KEY_OUTER, Matrix2D{}, 1.0F);
    direct.fill_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), Color::red());
    AURORA_TEST_CHECK_EQ(static_cast<int>(direct.get_pixel(4, 4).r), 255);
}

AURORA_TEST_CASE(rhi_backend_default_contract) {
    // 最小派生（仅覆写 name）：能力位全 false、流式 / 导入接口默认 no-op 返回 0。
    class MockRhi final : public rhi::RhiBackend {
      public:
        [[nodiscard]] auto name() const -> std::string_view override { return "mock"; }
        auto submit(const DrawCmd &, const rhi::CmdData &) -> void override {}
    };
    MockRhi mock;
    const auto cap = mock.capabilities();
    AURORA_TEST_CHECK_FALSE(cap.gpu);
    AURORA_TEST_CHECK_FALSE(cap.native_surface_import);
    AURORA_TEST_CHECK_FALSE(cap.compute);
    AURORA_TEST_CHECK_EQ(mock.acquire_stream_image(9, 8, 8), 0U);
    AURORA_TEST_CHECK_NO_THROW(mock.update_stream_image(9, nullptr, 0, 0, 0, 1, 1));
    AURORA_TEST_CHECK_NO_THROW(mock.release_stream_image(9));
    AURORA_TEST_CHECK_EQ(mock.import_native_surface(aurora::NativeSurfaceFrame{}), 0U);
}

AURORA_TEST_CASE(software_rhi_layer_roundtrip_matches_direct) {
    constexpr std::uint64_t KEY = 21;
    // 参照：直接绘制。
    Painter direct;
    direct.begin(64, 32);
    direct.fill_rect(rect_at(0.0F, 0.0F, 64.0F, 32.0F), Color::red());

    // 录制：BeginLayer + fill + EndLayer + DrawLayer，回放经 SoftwareRhi 层仿真。
    DisplayList dl;
    Painter recorded;
    recorded.begin(64, 32);
    recorded.record(dl);
    recorded.begin_layer(KEY, Size{.width = 64.0F, .height = 32.0F});
    recorded.fill_rect(rect_at(0.0F, 0.0F, 64.0F, 32.0F), Color::red());
    recorded.end_layer();
    recorded.draw_layer(KEY, Matrix2D{}, 1.0F);
    recorded.stop();
    dl.replay(recorded);

    AURORA_TEST_REQUIRE(direct.width() == recorded.width());
    const auto bytes = static_cast<std::size_t>(direct.width()) * static_cast<std::size_t>(direct.height()) * 4U;
    AURORA_TEST_CHECK_TRUE(std::equal(direct.data(), direct.data() + static_cast<std::ptrdiff_t>(bytes),
                                      recorded.data()));
}

AURORA_TEST_CASE(software_rhi_layer_miss_skips_and_bumps_epoch) {
    // 动态分配层键：全局层存储进程级持久，硬编码键在 --repeat 下会假命中。
    const auto KEY = next_gpu_layer_key();
    // 冷存储：DrawLayer 未命中 → 跳过（零绘制）+ bump 层代际（控件下帧重录的自愈信号）。
    DisplayList miss_dl;
    DrawCmd miss;
    miss.kind = CmdKind::DrawLayer;
    miss.aux_key = KEY;
    miss.matrix_idx = miss_dl.add_matrix(Matrix2D{});
    miss.composite_scale = 1.0F;
    miss_dl.push_cmd(miss);

    Painter target;
    target.begin(16, 16);
    const auto e0 = gpu_layer_epoch();
    miss_dl.replay(target);
    AURORA_TEST_CHECK_EQ(gpu_layer_epoch(), e0 + 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(target.get_pixel(0, 0).a), 0);  // 未绘制

    // 同一后端随后收到完整层录制：EndLayer 定稿 → DrawLayer 命中 → 内容上屏（单帧自愈）。
    DisplayList full_dl;
    Painter recorder;
    recorder.begin(16, 16);
    recorder.record(full_dl);
    recorder.begin_layer(KEY, Size{.width = 16.0F, .height = 16.0F});
    recorder.fill_rect(rect_at(0.0F, 0.0F, 16.0F, 16.0F), Color::red());
    recorder.end_layer();
    recorder.draw_layer(KEY, Matrix2D{}, 1.0F);
    recorder.stop();
    full_dl.replay(target);
    AURORA_TEST_CHECK_EQ(static_cast<int>(target.get_pixel(8, 8).r), 255);
}

AURORA_TEST_CASE(software_rhi_layer_store_shared_across_instances) {
    constexpr std::uint64_t KEY = 23;
    // 共享层存储：实例 A 定稿层位图，实例 B 仅收 DrawLayer 即命中（嵌套离屏回放场景）。
    std::unordered_map<std::uint64_t, Image> store;

    DisplayList full_dl;
    Painter recorder;
    recorder.begin(16, 16);
    recorder.record(full_dl);
    recorder.begin_layer(KEY, Size{.width = 16.0F, .height = 16.0F});
    recorder.fill_rect(rect_at(0.0F, 0.0F, 16.0F, 16.0F), Color::blue());
    recorder.end_layer();
    recorder.draw_layer(KEY, Matrix2D{}, 1.0F);
    recorder.stop();

    Painter pa;
    pa.begin(16, 16);
    rhi::SoftwareRhi swa{pa, &store};
    full_dl.replay(swa);
    AURORA_TEST_CHECK_EQ(static_cast<int>(pa.get_pixel(8, 8).b), 255);
    AURORA_TEST_CHECK_TRUE(store.count(KEY) == 1U);

    // 实例 B（冷 Painter、同一存储）：DrawLayer-only 命中，不 bump 代际。
    DisplayList draw_only;
    DrawCmd dl_cmd;
    dl_cmd.kind = CmdKind::DrawLayer;
    dl_cmd.aux_key = KEY;
    dl_cmd.matrix_idx = draw_only.add_matrix(Matrix2D{});
    dl_cmd.composite_scale = 1.0F;
    draw_only.push_cmd(dl_cmd);

    Painter pb;
    pb.begin(16, 16);
    rhi::SoftwareRhi swb{pb, &store};
    const auto e0 = gpu_layer_epoch();
    draw_only.replay(swb);
    AURORA_TEST_CHECK_EQ(gpu_layer_epoch(), e0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(pb.get_pixel(8, 8).b), 255);
}

}  // namespace aurora::test_cases::utest_gpu_layers
