/// 测试类型: unit
/// 目标单元: include/aurora/render/rhi/wgpu_rhi.h
/// 测试说明: WgpuRhi 占位（未初始化）契约——valid/begin/read 全拒、replay 安全 no-op；
/// 离屏真实设备帧生命周期——非法尺寸拒绝、begin 零基底、命令流（fill/clip/clear）回放后
/// read_pixels 像素断言（精确色与透明零基底）、stats 逐帧复位且 skipped_cmds 恒零；
/// 流式纹理槽（acquire 键稳定 / update 不抛 / release 幂等）与 import_native_surface
/// 恒 0 回退契约（wgpu-native v29 无外部共享纹理导入入口）；compute mip 链——64×64
/// 棋盘图 4× 降采样整块为均匀均值色（三线性命中 mip≥1），1:1 绘制仍是端点色（lod 0 无混）。
/// 依赖 Vulkan/D3D12 adapter：无可用设备环境整体 SKIP（真实 GPU 断言不做假通过）。

#include <array>
#include <cstdint>
#include <vector>

#include "aurora/core/native_surface.h"
#include "framework/aurora_test.h"

#ifdef AURORA_BACKEND_GPU_WGPU

#include "aurora/render/display_list.h"
#include "aurora/render/rhi/wgpu_rhi.h"

namespace aurora::test_cases::utest_wgpu_rhi {

namespace rhi = aurora::rhi;

namespace {

auto make_fill(Rect bounds, Color color) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = bounds;
    cmd.color = color;
    return cmd;
}

// 64×64 单纹素红/绿棋盘：任一 2×2 块恒含 2A+2B → mip≥1 各级均为均匀均值色 (120,120,20)。
// 4× 降采样三线性命中 mip2 → 整块均值；lod0 双线性则走样成红/绿逐像素交替。
[[nodiscard]] auto make_checkerboard() -> Image {
    Image img;
    img.width = 64;
    img.height = 64;
    img.pixels.resize(static_cast<std::size_t>(64) * 64U * 4U);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)) * 4U;
            const bool hi = ((x ^ y) & 1) == 0;
            img.pixels[idx + 0] = hi ? 220 : 20;
            img.pixels[idx + 1] = hi ? 20 : 220;
            img.pixels[idx + 2] = 20;
            img.pixels[idx + 3] = 255;
        }
    }
    return img;
}

auto make_draw_image(Rect bounds, const Image &img, DisplayList &dl) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::DrawImage;
    cmd.bounds = bounds;
    cmd.image_idx = dl.add_image(img);
    return cmd;
}

[[nodiscard]] auto offscreen(int w = 64, int h = 48) -> rhi::WgpuRhiOptions {
    rhi::WgpuRhiOptions opts;
    opts.native_window = nullptr;  // 离屏：渲染目标为内部纹理，read_pixels 可读回
    opts.offscreen_width = w;
    opts.offscreen_height = h;
    return opts;
}

// 取设备像素（RGBA8，自上而下行序）某点颜色；越界返回全零。
auto pixel_at(const std::vector<std::uint8_t> &px, int w, int x, int y) -> std::array<int, 4> {
    const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x)) *
                            4U;
    if (px.size() < idx + 4U) {
        return {0, 0, 0, 0};
    }
    return {px[idx], px[idx + 1], px[idx + 2], px[idx + 3]};
}

}  // namespace

AURORA_TEST_CASE(wgpu_default_constructed_invalid) {
    rhi::WgpuRhi rhi_obj;
    AURORA_TEST_CHECK_FALSE(rhi_obj.valid());
    AURORA_TEST_CHECK_EQ(rhi_obj.name(), std::string_view("gpu-wgpu"));
    // 未初始化：帧接口整体拒绝，读回失败；end_frame 幂等安全。
    AURORA_TEST_CHECK_FALSE(rhi_obj.begin_frame(64, 48, 1.0F));
    AURORA_TEST_CHECK_NO_THROW(rhi_obj.end_frame());
    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats.draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(stats.vertices, 0U);
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);
    std::vector<std::uint8_t> pixels;
    AURORA_TEST_CHECK_FALSE(rhi_obj.read_pixels(pixels));

    // 命令消费面：未初始化下 replay 为安全 no-op（不崩不计数）。
    DisplayList dl;
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 8.0F, .height = 8.0F}},
                          Color{255, 0, 0, 255}));
    AURORA_TEST_CHECK_NO_THROW(dl.replay(rhi_obj.backend()));
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
}

AURORA_TEST_CASE(wgpu_offscreen_frame_lifecycle_and_pixels) {
    rhi::WgpuRhi rhi_obj(offscreen());
    if (!rhi_obj.valid()) {
        AURORA_TEST_SKIP("无可用 wgpu adapter/device（CI 或驱动缺失），真实 GPU 断言跳过");
    }

    // 非法尺寸拒绝（设备像素为正）。
    AURORA_TEST_CHECK_FALSE(rhi_obj.begin_frame(0, 48, 1.0F));
    AURORA_TEST_CHECK_FALSE(rhi_obj.begin_frame(64, -1, 1.0F));

    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));

    // 命令流：红底 → 裁剪内绿块 → ClearRect 挖左上一条 → 端帧。
    DisplayList dl;
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}},
                          Color{200, 40, 40, 255}));
    DrawCmd clip;
    clip.kind = CmdKind::PushClip;
    clip.bounds = Rect{.origin = Point{.x = 8.0F, .y = 8.0F}, .size = Size{.width = 32.0F, .height = 24.0F}};
    dl.push_cmd(clip);
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 4.0F, .y = 4.0F}, .size = Size{.width = 48.0F, .height = 40.0F}},
                          Color{40, 180, 40, 255}));
    DrawCmd pop;
    pop.kind = CmdKind::PopClip;
    dl.push_cmd(pop);
    DrawCmd clear;
    clear.kind = CmdKind::ClearRect;
    clear.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 4.0F}};
    dl.push_cmd(clear);
    rhi::RhiFrameSink &sink = rhi_obj;
    AURORA_TEST_CHECK_EQ(sink.name(), std::string_view("gpu-wgpu"));
    dl.replay(sink.backend());
    rhi_obj.end_frame();

    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_TRUE(stats.draw_calls > 0U);
    AURORA_TEST_CHECK_TRUE(stats.vertices > 0U);
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);  // 全部命令均有实路径

    std::vector<std::uint8_t> px;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px));
    AURORA_TEST_CHECK_EQ(px.size(), static_cast<std::size_t>(64) * 48U * 4U);
    // 裁剪内（16,16）绿、裁剪外（56,32）红：不透明纯色 fill 精确匹配。
    AURORA_TEST_CHECK_EQ(pixel_at(px, 64, 16, 16), (std::array<int, 4>{40, 180, 40, 255}));
    AURORA_TEST_CHECK_EQ(pixel_at(px, 64, 56, 32), (std::array<int, 4>{200, 40, 40, 255}));
    // ClearRect 行（y=1）归零基底。
    AURORA_TEST_CHECK_EQ(pixel_at(px, 64, 32, 1), (std::array<int, 4>{0, 0, 0, 0}));

    // 第二帧：stats 逐帧复位，同尺寸复用画布。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList frame2;
    frame2.push_cmd(
        make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}},
                  Color{255, 255, 255, 255}));
    frame2.replay(sink.backend());
    rhi_obj.end_frame();
    const auto stats2 = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats2.draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(stats2.vertices, 6U);  // wgpu 无索引三角列：单 fill = 2 三角形 = 6 顶点
    AURORA_TEST_CHECK_EQ(stats2.skipped_cmds, 0U);
    std::vector<std::uint8_t> px2;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px2));
    AURORA_TEST_CHECK_EQ(pixel_at(px2, 64, 3, 3), (std::array<int, 4>{255, 255, 255, 255}));

    // 尺寸变化：重设目标仍出帧。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 24, 2.0F));
    rhi_obj.end_frame();
    std::vector<std::uint8_t> px3;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px3));
    AURORA_TEST_CHECK_EQ(px3.size(), static_cast<std::size_t>(32) * 24U * 4U);
}

AURORA_TEST_CASE(wgpu_stream_image_and_native_import_contract) {
    rhi::WgpuRhi rhi_obj(offscreen(32, 32));
    if (!rhi_obj.valid()) {
        AURORA_TEST_SKIP("无可用 wgpu adapter/device，流式槽契约跳过");
    }

    // 占位（未 begin_frame）下的读回与 acquire 契约仍安全。
    auto *backend = &rhi_obj.backend();
    const std::vector<std::uint8_t> buf(static_cast<std::size_t>(16) * 16U * 4U, 7U);
    const std::uint64_t key = 0xFEEDC0DEULL;
    const auto id = backend->acquire_stream_image(key, 16, 16);
    AURORA_TEST_CHECK_TRUE(id != 0U);
    // 键寻址槽复用：同键重取返回同一句柄。
    AURORA_TEST_CHECK_EQ(backend->acquire_stream_image(key, 16, 16), id);
    AURORA_TEST_CHECK_NO_THROW(backend->update_stream_image(id, buf.data(), 0, 0, 0, 16, 16));
    AURORA_TEST_CHECK_NO_THROW(backend->release_stream_image(id));
    AURORA_TEST_CHECK_NO_THROW(backend->release_stream_image(id));  // 重复释放无害

    // 原生表面导入：v29 C API 无导入入口 → 恒 0（能力位 false），调用方回退 CPU 上传。
    NativeSurfaceFrame nsf;
    nsf.kind = NativeSurfaceKind::DmaBuf;
    nsf.handle = reinterpret_cast<void *>(static_cast<std::uintptr_t>(42));
    nsf.width = 16;
    nsf.height = 16;
    AURORA_TEST_CHECK_EQ(backend->import_native_surface(nsf), 0U);
    AURORA_TEST_CHECK_FALSE(backend->capabilities().native_surface_import);
    AURORA_TEST_CHECK_TRUE(backend->capabilities().gpu);
}

AURORA_TEST_CASE(wgpu_compute_mip_downscale_sampling) {
    rhi::WgpuRhi rhi_obj(offscreen(64, 64));
    if (!rhi_obj.valid()) {
        AURORA_TEST_SKIP("无可用 wgpu adapter/device，mip 采样断言跳过");
    }
    if (!rhi_obj.backend().capabilities().compute) {
        AURORA_TEST_SKIP("adapter 无 compute（GLES 兜底端），mip 链不生成，采样保持 lod0 行为");
    }
    const Image board = make_checkerboard();

    // 帧 1：64×64 棋盘缩到 16×16（4× 降采样，lod 恰为 2）——三线性命中均匀 mip，
    // 块内像素应为均值色 (120,120,20)；容差 ±14 吸收跨驱动 LOD 抖动与边界 AA 余量。
    AURORA_TEST_REQUIRE(rhi_obj.begin_frame(64, 64, 1.0F));
    DisplayList down;
    down.push_cmd(make_draw_image(
        Rect{.origin = Point{.x = 8.0F, .y = 8.0F}, .size = Size{.width = 16.0F, .height = 16.0F}}, board, down));
    down.replay(rhi_obj.backend());
    rhi_obj.end_frame();
    std::vector<std::uint8_t> px;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px));
    for (int y = 11; y <= 20; ++y) {
        for (int x = 11; x <= 20; ++x) {
            const auto c = pixel_at(px, 64, x, y);
            AURORA_TEST_CHECK(c[0] > 106 && c[0] < 134);  // r ≈ 120（远离端点 220/20）
            AURORA_TEST_CHECK(c[1] > 106 && c[1] < 134);  // g ≈ 120
            AURORA_TEST_CHECK(c[2] > 6 && c[2] < 34);     // b ≈ 20
            AURORA_TEST_CHECK_EQ(c[3], 255);
        }
    }

    // 帧 2：同图 1:1 绘制——lod 0 仍取原始纹素（端点色），证明 mip 采样不破坏放大/等大。
    AURORA_TEST_REQUIRE(rhi_obj.begin_frame(64, 64, 1.0F));
    DisplayList full;
    full.push_cmd(make_draw_image(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 64.0F}}, board, full));
    full.replay(rhi_obj.backend());
    rhi_obj.end_frame();
    std::vector<std::uint8_t> px2;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px2));
    for (int y = 4; y <= 60; y += 8) {
        for (int x = 4; x <= 60; x += 8) {
            const auto c = pixel_at(px2, 64, x, y);
            const bool hi = ((x ^ y) & 1) == 0;  // 与 make_checkerboard 同式
            AURORA_TEST_CHECK_EQ(c[0], hi ? 220 : 20);
            AURORA_TEST_CHECK_EQ(c[1], hi ? 20 : 220);
            AURORA_TEST_CHECK_EQ(c[2], 20);
        }
    }
}

}  // namespace aurora::test_cases::utest_wgpu_rhi

#else  // !AURORA_BACKEND_GPU_WGPU

namespace aurora::test_cases::utest_wgpu_rhi {

AURORA_TEST_CASE(wgpu_default_constructed_invalid) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启（需 Rust 工具链 + wgpu-native 源码构建），头与实现整体被宏剔除");
}
AURORA_TEST_CASE(wgpu_offscreen_frame_lifecycle_and_pixels) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(wgpu_stream_image_and_native_import_contract) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(wgpu_compute_mip_downscale_sampling) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}

}  // namespace aurora::test_cases::utest_wgpu_rhi

#endif  // AURORA_BACKEND_GPU_WGPU
