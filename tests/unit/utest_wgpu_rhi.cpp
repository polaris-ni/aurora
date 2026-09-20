/// 测试类型: unit
/// 目标单元: include/aurora/render/rhi/wgpu_rhi.h
/// 测试说明: WgpuRhi 占位（未初始化）契约——valid/begin/read 全拒、replay 安全 no-op；
/// 离屏真实设备帧生命周期——非法尺寸拒绝、begin 零基底、命令流（fill/clip/clear）回放后
/// read_pixels 像素断言（精确色与透明零基底）、stats 逐帧复位且 skipped_cmds 恒零；
/// 流式纹理槽（acquire 键稳定 / update 不抛 / release 幂等）与 import_native_surface
/// 恒 0 回退契约（wgpu-native v29 无外部共享纹理导入入口）；compute mip 链——64×64
/// 棋盘图 4× 降采样整块为均匀均值色（三线性命中 mip≥1），1:1 绘制仍是端点色（lod 0 无混）；
/// 离屏读回通道开关与连帧 submit——连帧不逐帧消费 read_pixels 不踩「缓冲仍映射」验证错误，
/// 关闭期间 read_pixels 整体拒绝、重新打开后下一帧恢复；CSD 装饰 DL 追加回放在内容帧之上——
/// 读回证明装饰条带/悬停红底/图标像素上屏、装饰带以下仍是内容色、命令族零 skipped；
/// compute 区域效果实路径（cs_blur/cs_blend/cs_mask）——blur 整数域可分离 box CPU 参照
/// 精确匹配、blend Multiply/mask LinearFade ±1 量化容差比对、区域外 untouched，无 compute
/// adapter 整体 SKIP。
/// 依赖 Vulkan/D3D12 adapter：无可用设备环境整体 SKIP（真实 GPU 断言不做假通过）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/native_surface.h"
#include "framework/aurora_test.h"

#ifdef AURORA_BACKEND_GPU_WGPU

#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/detail/title_bar_painter.h"
#include "aurora/window/title_bar_geometry.h"

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

// 纯色图标（CSD 图标槽用；`shared_ptr` 语义由绘制层按引用取像素，此处只填内容）。
[[nodiscard]] auto solid_icon_ptr(int side, Color c) -> std::shared_ptr<Image> {
    auto img = std::make_shared<Image>();
    img->width = side;
    img->height = side;
    img->pixels.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U, 0U);
    for (int i = 0; i < side * side; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 4U;
        img->pixels[o + 0U] = c.r;
        img->pixels[o + 1U] = c.g;
        img->pixels[o + 2U] = c.b;
        img->pixels[o + 3U] = c.a;
    }
    img->invalidate_content_hash();
    return img;
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

namespace {

// 逻辑矩形速构（帧尺寸/scale=1，设备像素 = 逻辑像素）。
[[nodiscard]] auto mkrect(int x, int y, int w, int h) -> Rect {
    return Rect{.origin = Point{.x = static_cast<float>(x), .y = static_cast<float>(y)},
                .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}};
}

// 内容场（三帧共用、可解析重建）：64×64 底 (10,20,30) + [16,48)² 块 (200,100,50)。
[[nodiscard]] auto field_at(int x, int y, int ch) -> int {
    static constexpr int kColors[2][3] = {{10, 20, 30}, {200, 100, 50}};
    const bool blk = x >= 16 && x < 48 && y >= 16 && y < 48;
    return kColors[blk ? 1 : 0][ch];
}

}  // namespace

AURORA_TEST_CASE(wgpu_compute_region_effects_match_cpu_reference) {
    // blur/blend/mask 区域效果 compute 实路径（cs_blur/cs_blend/cs_mask）逐像素验收：
    // 与 WGSL 同源公式（整数域恒权 box + 区域 tap 钳位 / blend_rgb / mask_base）的 CPU
    // 参照比对——blur 两端整型运算精确无容差；blend/mask 经 rgba8unorm 落纹素量化，±1 吸收
    // f32 中间域与 double 参照的边界差。adapter 无 compute 整体 SKIP（片元兜底语义由
    // golden 容差测试覆盖，此处锁定 compute 路，不做假通过）。
    rhi::WgpuRhi rhi_obj(offscreen(64, 64));
    if (!rhi_obj.valid()) {
        AURORA_TEST_SKIP("无可用 wgpu adapter/device，compute 区域效果断言跳过");
    }
    if (!rhi_obj.backend().capabilities().compute) {
        AURORA_TEST_SKIP("adapter 无 compute（GLES 兜底端），效果走片元路，本用例锁定 compute 实路径");
    }
    auto content = [](DisplayList &dl) {
        dl.push_cmd(make_fill(mkrect(0, 0, 64, 64), Color{10, 20, 30, 255}));
        dl.push_cmd(make_fill(mkrect(16, 16, 32, 32), Color{200, 100, 50, 255}));
    };

    // ---- 帧 1：BlurRegion [16,48)²，r=1 —— H(canvas→alt)+V(alt→canvas) 两趟 compute dispatch。
    // CPU 参照：整数域可分离 box，tap 钳位区域本地索引 [0,31]（不漏采区外），整除即 floor。
    AURORA_TEST_REQUIRE(rhi_obj.begin_frame(64, 64, 1.0F));
    {
        DisplayList dl;
        content(dl);
        DrawCmd blur;
        blur.kind = CmdKind::BlurRegion;
        blur.bounds = mkrect(16, 16, 32, 32);
        blur.f0 = 1.0F;  // scale=1 → r = max(1, trunc(1)) = 1
        dl.push_cmd(blur);
        dl.replay(rhi_obj.backend());
        AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    }
    rhi_obj.end_frame();
    std::vector<std::uint8_t> px;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px));
    int hp[32][32][3];  // H 趟中间态（整数域，rgba8unorm 精确存回）
    for (int ch = 0; ch < 3; ++ch) {
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                int acc = 0;
                for (int k = -1; k <= 1; ++k) {
                    acc += field_at(16 + std::clamp(x + k, 0, 31), 16 + y, ch);
                }
                hp[y][x][ch] = acc / 3;
            }
        }
    }
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            for (int ch = 0; ch < 3; ++ch) {
                int acc = 0;
                for (int k = -1; k <= 1; ++k) {
                    acc += hp[std::clamp(y + k, 0, 31)][x][ch];
                }
                const auto got = pixel_at(px, 64, 16 + x, 16 + y);
                AURORA_TEST_CHECK_EQ(got[ch], acc / 3);  // 整数域两端精确匹配
            }
            const auto got = pixel_at(px, 64, 16 + x, 16 + y);
            AURORA_TEST_CHECK_EQ(got[3], 255);
        }
    }
    // 区域外 untouched：纯底 (10,20,30)。
    AURORA_TEST_CHECK_EQ(pixel_at(px, 64, 4, 4), (std::array<int, 4>{10, 20, 30, 255}));
    AURORA_TEST_CHECK_EQ(pixel_at(px, 64, 60, 60), (std::array<int, 4>{10, 20, 30, 255}));

    // ---- 帧 2：BlendRegion Multiply [8,24)²，strength=0.5，tint=(255,0,255)。
    // 255 域参照：m = s·t/255，o = clamp(s + a·(m−s))；±1 容差（compute 写 alt → 区域拷回，
    // rgba8 RN 量化 vs double 参照）。alpha 通道原样保留。
    AURORA_TEST_REQUIRE(rhi_obj.begin_frame(64, 64, 1.0F));
    {
        DisplayList dl;
        content(dl);
        DrawCmd blend;
        blend.kind = CmdKind::BlendRegion;
        blend.bounds = mkrect(8, 8, 16, 16);
        blend.blend_mode = BlendMode::Multiply;
        blend.f0 = 0.5F;
        blend.color = Color{255, 0, 255, 255};
        dl.push_cmd(blend);
        dl.replay(rhi_obj.backend());
        AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    }
    rhi_obj.end_frame();
    std::vector<std::uint8_t> px2;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px2));
    static constexpr int kTint[3] = {255, 0, 255};
    for (int y = 8; y < 24; ++y) {
        for (int x = 8; x < 24; ++x) {
            const auto got = pixel_at(px2, 64, x, y);
            for (int ch = 0; ch < 3; ++ch) {
                const double s = static_cast<double>(field_at(x, y, ch));
                const double m = s * static_cast<double>(kTint[ch]) / 255.0;
                const double o = std::clamp(s + 0.5 * (m - s), 0.0, 255.0);
                AURORA_TEST_CHECK(std::abs(static_cast<double>(got[ch]) - o) <= 1.5);
            }
            AURORA_TEST_CHECK_EQ(got[3], 255);
        }
    }
    // 混合区外 untouched。
    AURORA_TEST_CHECK_EQ(pixel_at(px2, 64, 40, 40), (std::array<int, 4>{200, 100, 50, 255}));

    // ---- 帧 3：MaskRegion LinearFade [16,48)²，strength=1 —— RGB 乘区域纵向渐变因子。
    // factor = base = 1 − y_local/32（ipx.y∈[0,31]，clamp 后不截）；输出 floor(s·factor)，
    // ±1 容差吸收 f32 乘除往返（如 s/255·255 的 99.999… 边界）。alpha 保留。
    AURORA_TEST_REQUIRE(rhi_obj.begin_frame(64, 64, 1.0F));
    {
        DisplayList dl;
        content(dl);
        DrawCmd mask;
        mask.kind = CmdKind::MaskRegion;
        mask.bounds = mkrect(16, 16, 32, 32);
        mask.mask_kind = ShaderMaskKind::LinearFade;
        mask.f0 = 1.0F;
        dl.push_cmd(mask);
        dl.replay(rhi_obj.backend());
        AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    }
    rhi_obj.end_frame();
    std::vector<std::uint8_t> px3;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px3));
    for (int y = 16; y < 48; ++y) {
        const double factor = 1.0 - static_cast<double>(y - 16) / 32.0;
        for (int x = 16; x < 48; ++x) {
            const auto got = pixel_at(px3, 64, x, y);
            for (int ch = 0; ch < 3; ++ch) {
                const double expected = std::floor(static_cast<double>(field_at(x, y, ch)) * factor);
                AURORA_TEST_CHECK(std::abs(static_cast<double>(got[ch]) - expected) <= 1.5);
            }
            AURORA_TEST_CHECK_EQ(got[3], 255);  // alpha 原样
        }
    }
    // 遮罩区外 untouched（首行恰证渐变起点无衰减语义）。
    AURORA_TEST_CHECK_EQ(pixel_at(px3, 64, 4, 60), (std::array<int, 4>{10, 20, 30, 255}));
}

AURORA_TEST_CASE(wgpu_readback_toggle_and_multi_frame_submit) {
    rhi::WgpuRhi rhi_obj(offscreen());
    if (!rhi_obj.valid()) {
        AURORA_TEST_SKIP("无可用 wgpu adapter/device，离屏读回通道契约跳过");
    }
    auto fill_frame = [&](Color color) {
        AURORA_TEST_REQUIRE(rhi_obj.begin_frame(64, 48, 1.0F));
        DisplayList dl;
        dl.push_cmd(make_fill(
            Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}}, color));
        dl.replay(rhi_obj.backend());
        rhi_obj.end_frame();
    };

    // 连帧提交、只消费末帧读回：帧尾待 map 由下一帧 begin 退役，不得复用仍在映射的缓冲
    // （修复前：wgpuQueueSubmit Validation → Rust panic 不可展开 → 进程 abort）。
    fill_frame(Color{200, 40, 40, 255});
    fill_frame(Color{40, 180, 40, 255});
    fill_frame(Color{40, 40, 200, 255});
    std::vector<std::uint8_t> px;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px));
    AURORA_TEST_CHECK_EQ(pixel_at(px, 64, 32, 24), (std::array<int, 4>{40, 40, 200, 255}));  // 末帧色，非首帧

    // 关闭读回：连帧零读回开销，read_pixels 整体拒绝；重新打开后下一帧起恢复。
    rhi_obj.set_readback_enabled(false);
    fill_frame(Color{240, 240, 40, 255});
    fill_frame(Color{240, 240, 40, 255});
    std::vector<std::uint8_t> none;
    AURORA_TEST_CHECK_FALSE(rhi_obj.read_pixels(none));
    AURORA_TEST_CHECK_TRUE(none.empty());

    rhi_obj.set_readback_enabled(true);
    fill_frame(Color{20, 220, 220, 255});
    std::vector<std::uint8_t> px2;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px2));
    AURORA_TEST_CHECK_EQ(pixel_at(px2, 64, 32, 24), (std::array<int, 4>{20, 220, 220, 255}));
}

AURORA_TEST_CASE(wgpu_replays_csd_decoration_over_content) {
    // CSD 装饰合成进 GPU 帧的最小复现现场：内容帧 DL 之后追加装饰 DL（即
    // `WgpuWaylandSurface::Sink::end_frame` 的做法），离屏读回证明装饰真的盖在内容之上、
    // 且不越界涂抹内容区。装饰命令族含 FillRect/RoundedRect/DrawLine/DrawText/DrawImage
    // 四类，`skipped_cmds` 恒零即「WgpuRhi 全族可消费、无静默丢命令」。
    rhi::WgpuRhi rhi_obj(offscreen(160, 80));
    if (!rhi_obj.valid()) {
        AURORA_TEST_SKIP("无可用 wgpu adapter/device，装饰回放像素断言跳过");
    }
    constexpr Color kContent{0, 160, 0, 255};
    csd::TitleBarPaintState s;
    s.width = 160.0F;
    s.title_bar = true;
    s.hovered_button = 2;  // 悬停关闭钮 → 特征红圆底
    s.title = "Aa 01";
    s.icon = solid_icon_ptr(16, Color{60, 130, 228, 255});

    // 装饰 DL：录制态 Painter（与 WaylandSurface::record_client_decoration 同一条通路）。
    DisplayList deco;
    {
        Painter rec;
        rec.record(deco);
        csd::paint_title_bar(rec, s);
        rec.stop();
    }
    AURORA_TEST_REQUIRE_FALSE(deco.empty());

    AURORA_TEST_REQUIRE(rhi_obj.begin_frame(160, 80, 1.0F));
    DisplayList content;
    content.push_cmd(make_fill(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 160.0F, .height = 80.0F}}, kContent));
    content.replay(rhi_obj.backend());
    deco.replay(rhi_obj.backend());  // ← 帧尾追加回放：z 序在 app 内容之上
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);

    std::vector<std::uint8_t> px;
    AURORA_TEST_REQUIRE(rhi_obj.read_pixels(px));
    const TitleBarGeometry g = title_bar_geometry(160.0F, s.style, false, true);
    const Color bg = s.style.bg_active;
    // 标题栏条带（图标槽左侧的纯底色区）。
    AURORA_TEST_CHECK_EQ(pixel_at(px, 160, 4, 18), (std::array<int, 4>{bg.r, bg.g, bg.b, bg.a}));
    // 悬停关闭钮圆底 = 特征红（与内容绿显著区分）。圆心本身被白色 ✕ 字形穿过，故取圆心上方。
    const Point above_close{g.close.origin.x + g.close.size.width * 0.5F, g.close.origin.y + 3.0F};
    const auto cc = pixel_at(px, 160, static_cast<int>(above_close.x), static_cast<int>(above_close.y));
    AURORA_TEST_CHECK(cc[0] > 180 && cc[1] < 90 && cc[2] < 90);
    // 图标槽：DrawImage 经纹理链路上屏（蓝色占优即证图标像素而非底色/内容色）。
    const Point icon_c{g.icon.origin.x + g.icon.size.width * 0.5F, g.icon.origin.y + g.icon.size.height * 0.5F};
    const auto ic = pixel_at(px, 160, static_cast<int>(icon_c.x), static_cast<int>(icon_c.y));
    AURORA_TEST_CHECK(ic[2] > 150 && ic[2] > ic[0] && ic[2] > ic[1]);
    // 装饰带以下：仍是 app 内容绿（装饰未越界涂抹）。
    AURORA_TEST_CHECK_EQ(pixel_at(px, 160, 4, 60), (std::array<int, 4>{kContent.r, kContent.g, kContent.b, 255}));
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
AURORA_TEST_CASE(wgpu_compute_region_effects_match_cpu_reference) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(wgpu_readback_toggle_and_multi_frame_submit) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(wgpu_replays_csd_decoration_over_content) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}

}  // namespace aurora::test_cases::utest_wgpu_rhi

#endif  // AURORA_BACKEND_GPU_WGPU
