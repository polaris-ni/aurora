// tools/bench/bench_gpu.cpp — GPU 特性基准（非 CTest 断言，仅相对基线）。
//
// 常驻流式纹理（视频逐帧更新）与 GPU 层缓存（transform-only 动画）在 GpuGlRhi 命令
// 翻译层的 CPU / 分配计数对比。驱动桩为 tests/support/fake_gl.h（与 utest_gpu_gl_rhi
// 共用）：全量 GLFn 桩 + 上传 memcpy 模拟驱动成本 + 纹理分配/删除/上传字节确定性计数，
// 无需真实 GL 上下文，任何机器可复现。
//
// 场景一（视频流式纹理，480×270 ×128 帧）：
//   - legacy：每帧唯一内容 Image 走 content_hash 图像缓存路径——CPU 预乘全帧副本 +
//     每帧新建纹理；超 AURORA_IMAGE_CACHE_CAP(64) 全清淘汰。
//   - streaming：固定 stream_key + 递增 stream_version——一次性建槽，逐帧全帧
//     sub-upload（直色 RGBA，无 CPU 预乘），零纹理新建、零淘汰。
//
// 场景二（GPU 层缓存，Stack{200 静态盒}，transform-only 旋转动画 60 帧）：
//   - baseline：无 cache_layer——稳态为子控件 DL 缓存重放 + 全量命令翻译。
//   - cache_layer：根挂 cache_layer——稳态为单条 DrawLayer（子树零重绘零翻译）。
//
// 场景三/四/五/六（`AURORA_BACKEND_GPU_WGPU` 编译进库时追加）为 **wgpu 真 GPU 离屏实测**：
// 同一批场景（视频流式 / 层缓存）经 `WgpuRhi` 离屏通路重放，与 fake 桩口径互补——
// fake 桩量的是 CPU 侧翻译与分配次数（跨机可复现），wgpu 段量的是含 GPU 提交与等待的
// 端到端帧成本（每帧 `read_pixels` 强制收敛，故不含队列堆积带来的假性加速）。数值随
// 适配器/驱动/机器而变，**只作同机相对基线**；无可用 adapter 或 device 申请失败时打印
// 说明行并整段跳过（不失败，与 `AURORA_TEST_SKIP` 同口径）。
//   - 场景五 wgpu 独有：静态大图 512×512 缩小重采样——冷帧含 CPU 预乘 + 整幅上传 +
//     compute mip 链生成，稳态帧只三线性采样；对照「逐帧唯一内容」变体即 mip 链 churn。
//   - 场景六 wgpu 独有：每帧整幅三区域效果族（blur/blend/mask 连做 3 遍）经
//     `set_compute_effects_enabled` 切换 compute 实路与片元兜底路，两路同帧同内容，
//     差额即 compute 存储纹理路相对「离屏渲染目标读-改-写片元路」的帧成本收益。
//
// 运行：build 目录下 ./build/bench_gpu（bazel 式路径随 aurora_add_tool 输出）。

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/gpu_gl_rhi.h"
#include "bench_common.h"
#include "support/fake_gl.h"

#ifdef AURORA_BACKEND_GPU_WGPU
#include "aurora/render/rhi/wgpu_rhi.h"
#endif

namespace aurora::bench {

namespace {

using aurora::BuildContext;
using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::DisplayList;
using aurora::DrawCmd;
using aurora::Image;
using aurora::LeafWidget;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Row;
using aurora::RowProps;
using aurora::Size;

// ---- 场景参数 ----
constexpr int AURORA_VIDEO_W = 480;
constexpr int AURORA_VIDEO_H = 270;
constexpr int AURORA_VIDEO_FRAMES = 128;  // > 2×AURORA_IMAGE_CACHE_CAP(64)：观察 legacy 淘汰抖动
constexpr int AURORA_VIDEO_WARMUP = 3;

constexpr int AURORA_SCENE_W = 480;
constexpr int AURORA_SCENE_H = 400;
constexpr int AURORA_GRID_COLS = 12;  ///< 网格列数（盒 40×24 无重叠铺满 480×384）
constexpr int AURORA_GRID_ROWS = 16;  ///< 网格行数（12×16 = 192 盒；不重叠——剔除剔除不干扰基线）
constexpr int AURORA_SCENE_FRAMES = 60;
constexpr int AURORA_SCENE_WARMUP = 3;

// ---- 计数快照（fake GL 确定性计数器） ----
struct GlCounters {
    int texture_gens = 0;
    int texture_deletes = 0;
    std::uint64_t upload_bytes = 0;  ///< tex_image_2d + RGBA tex_sub_image_2d 数据字节

    static auto capture(const testing::FakeGl &fake) -> GlCounters {
        GlCounters c;
        c.texture_gens = fake.texture_gens;
        c.texture_deletes = fake.texture_deletes;
        for (const auto &up : fake.uploads) {
            c.upload_bytes += static_cast<std::uint64_t>(up.width) * static_cast<std::uint64_t>(up.height) * 4U;
        }
        for (const auto &up : fake.rgba_sub_uploads) {
            c.upload_bytes += static_cast<std::uint64_t>(up.width) * static_cast<std::uint64_t>(up.height) * 4U;
        }
        return c;
    }

    [[nodiscard]] auto delta_from(const GlCounters &before) const -> GlCounters {
        GlCounters d;
        d.texture_gens = texture_gens - before.texture_gens;
        d.texture_deletes = texture_deletes - before.texture_deletes;
        d.upload_bytes = upload_bytes - before.upload_bytes;
        return d;
    }
};

// 内容唯一的噪声图（模拟解码输出）：seed 决定像素，RGBA8 直色（预乘由路径自行处理）。
[[nodiscard]] auto make_noise_image(int w, int h, std::uint64_t seed) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    std::uint64_t x = (seed * 0x9E3779B97F4A7C15ULL) + 0x517C'CBFB'EB41'2C25ULL;
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        x = (x * 6364136223846793005ULL) + 1442695040888963407ULL;
        img.pixels[i + 0] = static_cast<std::uint8_t>(x >> 33);
        img.pixels[i + 1] = static_cast<std::uint8_t>(x >> 41);
        img.pixels[i + 2] = static_cast<std::uint8_t>(x >> 49);
        img.pixels[i + 3] = 255;
    }
    return img;
}

// 伪视频帧：逐帧内容唯一。
auto make_video_frame(std::uint64_t seed) -> Image { return make_noise_image(AURORA_VIDEO_W, AURORA_VIDEO_H, seed); }

/// @brief 回放一帧 DrawImage（legacy：无流式字段；streaming：固定键 + 递增版本）。
auto push_video_frame(DisplayList &dl, const Image &frame, bool streaming, std::uint64_t version) -> void {
    Image img = frame;  // DL 录制按值入池（与真实帧路径同形：录制侧一帧一副本）
    if (streaming) {
        img.stream_key = 1;
        img.stream_version = version;
    }
    DrawCmd cmd;
    cmd.kind = aurora::CmdKind::DrawImage;
    cmd.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                      .size = Size{.width = static_cast<float>(img.width), .height = static_cast<float>(img.height)}};
    cmd.image_idx = dl.add_image(img);
    dl.push_cmd(cmd);
}

struct VideoResult {
    double cpu_ms_per_frame = 0.0;
    GlCounters counters;
};

/// @brief 预生成 AURORA_VIDEO_FRAMES 帧唯一内容（两后端场景共用同一批输入）。
[[nodiscard]] auto make_video_frames() -> std::vector<Image> {
    std::vector<Image> frames;
    frames.reserve(AURORA_VIDEO_FRAMES);
    for (int i = 0; i < AURORA_VIDEO_FRAMES; ++i) {
        frames.push_back(make_video_frame(static_cast<std::uint64_t>(i) + 1U));
    }
    return frames;
}

/// @brief 视频场景：预生成 N 帧唯一内容，计时回放（含 begin/replay/end 全帧路径）。
auto bench_video(bool streaming) -> VideoResult {
    const std::vector<Image> frames = make_video_frames();
    testing::FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    rhi::RhiFrameSink &sink = rhi_obj;
    if (!rhi_obj.valid()) {
        return {};
    }

    // 预热：管线/槽位等一次性成本（streaming 键同槽续版本，稳态即「只 sub-upload」）。
    std::uint64_t version = 0;
    for (int i = 0; i < AURORA_VIDEO_WARMUP; ++i) {
        (void)rhi_obj.begin_frame(AURORA_VIDEO_W, AURORA_VIDEO_H, 1.0F);
        DisplayList dl;
        push_video_frame(dl, frames[static_cast<std::size_t>(i)], streaming, ++version);
        dl.replay(sink.backend());
        rhi_obj.end_frame();
    }

    const GlCounters before = GlCounters::capture(fake);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < AURORA_VIDEO_FRAMES; ++i) {
        (void)rhi_obj.begin_frame(AURORA_VIDEO_W, AURORA_VIDEO_H, 1.0F);
        DisplayList dl;
        push_video_frame(dl, frames[static_cast<std::size_t>(i)], streaming, ++version);
        dl.replay(sink.backend());
        rhi_obj.end_frame();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const GlCounters after = GlCounters::capture(fake);

    VideoResult r;
    r.cpu_ms_per_frame =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(AURORA_VIDEO_FRAMES);
    r.counters = after.delta_from(before);
    return r;
}

// ---- 场景二：transform-only 动画的层缓存 ----

/// @brief 静态盒：内容恒定（transform-only 动画下子树无内容失效）。
class BenchBox final : public LeafWidget {
  public:
    BenchBox(float w, float h, Color color) : sz_{.width = w, .height = h}, color_(color) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "BenchBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz_); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override { p.fill_rect(b, color_); }

  private:
    Size sz_;
    Color color_;
};

struct LayerResult {
    double cold_ms = 0.0;  ///< 首帧（层冷 / 离屏位图冷）
    double steady_ms_per_frame = 0.0;  ///< 稳态帧均
};

/// @brief 层缓存场景的树装配：无重叠网格 12×16 = 192 个 40×24 静态盒（真实 Column/Row
/// 布局——Stack 叠放全部子项共享同一 paint 原点，遮挡剔除只留可见者，基线失真）。
/// 两后端共用；`mount` + `layout` 在此完成，调用方只逐帧重录 + 回放。
[[nodiscard]] auto make_grid_root() -> std::shared_ptr<Column> {
    std::vector<Node> rows;
    rows.reserve(AURORA_GRID_ROWS);
    for (int r = 0; r < AURORA_GRID_ROWS; ++r) {
        RowProps rp;
        rp.children.reserve(AURORA_GRID_COLS);
        for (int c = 0; c < AURORA_GRID_COLS; ++c) {
            const auto idx = static_cast<unsigned>((r * AURORA_GRID_COLS) + c);
            const auto hue = static_cast<std::uint8_t>((idx * 37U) % 256U);
            rp.children.emplace_back(std::make_shared<BenchBox>(40.0F, 24.0F, Color{hue, 120, 200, 255}));
        }
        rows.emplace_back(std::make_shared<Row>(std::move(rp)));
    }
    ColumnProps cp;
    cp.children = std::move(rows);
    auto root = std::make_shared<Column>(std::move(cp));
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(AURORA_SCENE_W), .height = static_cast<float>(AURORA_SCENE_H)};
    root->layout(c, ctx);
    return root;
}

/// @brief 层缓存场景：一次装配布局，逐帧重录 + 回放；冷帧与稳态分开计时。
auto bench_layer(bool cache_layer) -> LayerResult {
    const std::shared_ptr<Column> root = make_grid_root();
    constexpr BuildContext ctx;

    const testing::FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    rhi::RhiFrameSink &sink = rhi_obj;
    if (!rhi_obj.valid()) {
        return {};
    }

    // 一帧完整路径：修饰链更新（旋转角递进）→ 重录 DL → GPU 回放。
    const Modifier base_modifier = cache_layer ? Modifier{}.cache_layer() : Modifier{};
    auto run_frame = [&](int frame) {
        root->modifier.set(base_modifier.rotate(static_cast<float>(frame) * 6.0F));
        DisplayList dl;
        Painter p;
        p.begin(AURORA_SCENE_W, AURORA_SCENE_H);
        p.record(dl);
        root->paint(p,
                    Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                         .size = Size{.width = static_cast<float>(AURORA_SCENE_W),
                                      .height = static_cast<float>(AURORA_SCENE_H)}},
                    ctx);
        p.stop();
        (void)rhi_obj.begin_frame(AURORA_SCENE_W, AURORA_SCENE_H, 1.0F);
        dl.replay(sink.backend());
        rhi_obj.end_frame();
    };

    // 冷帧（首帧）单独计时；随后预热至稳态再计时 AURORA_SCENE_FRAMES 帧。
    const auto c0 = std::chrono::steady_clock::now();
    run_frame(0);
    const auto c1 = std::chrono::steady_clock::now();

    for (int i = 1; i <= AURORA_SCENE_WARMUP; ++i) {
        run_frame(AURORA_SCENE_FRAMES + i);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < AURORA_SCENE_FRAMES; ++i) {
        run_frame(AURORA_SCENE_WARMUP + 1 + i);
    }
    const auto t1 = std::chrono::steady_clock::now();

    LayerResult r;
    r.cold_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();
    r.steady_ms_per_frame =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(AURORA_SCENE_FRAMES);
    return r;
}

#ifdef AURORA_BACKEND_GPU_WGPU

// ---- 场景三/四/五：wgpu 真 GPU 离屏实测（端到端帧成本）----

constexpr int AURORA_MIP_DIM = 512;  ///< 静态大图边长（≥ 4 才有 ≥ 2 级 mip 链）
constexpr int AURORA_MIP_DRAW = 128;  ///< 缩小绘制边长（4× 降采样，三线性命中 mip ≥ 1）
constexpr int AURORA_MIP_FRAMES = 24;  ///< 稳态帧数（unique 变体须预生成同样多的唯一内容图）
constexpr int AURORA_MIP_WARMUP = 3;

constexpr int AURORA_FX_DIM = 512;  ///< 场景六画幅边长（三效果族各以整幅为一遍）
constexpr int AURORA_FX_PASSES = 3;  ///< 每帧效果三连重数（提亮 GPU 工作量，压过批尾排空地板）
constexpr int AURORA_FX_FRAMES = 128;  ///< 稳态帧数（排空量子 ≈15.6ms 摊到 0.12 ms/帧地板）
constexpr int AURORA_FX_WARMUP = 8;

/// @brief 离屏 `WgpuRhi` 装载（`native_window = nullptr` → 无 swapchain 的诊断通路，
/// 与容差 golden / 探针同一条路）。失败返回 `nullptr`（无 adapter / device 申请失败）。
[[nodiscard]] auto offscreen_rhi(int w, int h) -> std::unique_ptr<rhi::WgpuRhi> {
    rhi::WgpuRhiOptions opts;
    opts.native_window = nullptr;
    opts.offscreen_width = w;
    opts.offscreen_height = h;
    auto gpu = std::make_unique<rhi::WgpuRhi>(opts);
    if (!gpu->valid()) {
        return nullptr;
    }
    return gpu;
}

/// @brief 一帧的提交路径（不同步）：begin → replay → end。
auto wgpu_submit_frame(rhi::WgpuRhi &gpu, int w, int h, const DisplayList &dl) -> void {
    (void)gpu.begin_frame(w, h, 1.0F);
    dl.replay(gpu.backend());
    gpu.end_frame();
}

/// @brief 一批帧的双口径计时结果。
struct WgpuFrameCost {
    double submit_ms = 0.0;  ///< CPU 侧翻译 + 提交均帧（与场景一 fake 桩同口径）
    double e2e_ms = 0.0;  ///< 批尾一次排空后的均帧（含 GPU 执行与背压）
};

/// @brief 预热 warmup 帧后计时 frames 帧，给出双口径。
///
/// 提交循环期间关掉读回通道（`set_readback_enabled(false)`）：读回缓冲是单块复用缓冲，
/// 连帧登记映射而不消费会让 wgpu 在第二次 submit 上判「向映射中的缓冲拷贝」Validation
/// Error（Rust panic 经 C FFI 直接 abort）。排空则用 barrier 帧：打开读回、补一帧提交，
/// `read_pixels` 泵到该帧映射完成——队列 FIFO，故此前全部提交已退役。
///
/// 同步**只能做一次**：`read_pixels` 经 pump 等待映射回调（每轮 `sleep_for(1ms)`，Windows
/// 定时器粒度实测把一次等待放大到 ≈15.6 ms），逐帧同步会让所有场景读到同一个等待量子地板、
/// 场景差异全被淹没；批尾一次则按帧数摊薄（128 帧 ≈0.12 ms/帧）。
template <class Fn>
[[nodiscard]] auto time_batch(rhi::WgpuRhi &gpu, int w, int h, int warmup, int frames, const Fn &one) -> WgpuFrameCost {
    auto run_one = [&](int i) {
        DisplayList dl;
        one(i, dl);
        wgpu_submit_frame(gpu, w, h, dl);
    };
    gpu.set_readback_enabled(false);
    for (int i = 0; i < warmup; ++i) {
        run_one(i);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        run_one(warmup + i);
    }
    const auto t1 = std::chrono::steady_clock::now();
    gpu.set_readback_enabled(true);
    run_one(warmup + frames);
    std::vector<std::uint8_t> buf;
    (void)gpu.read_pixels(buf);
    const auto t2 = std::chrono::steady_clock::now();
    const auto n = static_cast<double>(frames);
    return {.submit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / n,
            .e2e_ms = std::chrono::duration<double, std::milli>(t2 - t0).count() / n};
}

/// @brief 场景三：视频流式 vs legacy（与场景一同一批输入与路径判别，真 GPU 双口径）。
/// @return submit/e2e 均为 0 = 无可用 adapter（调用方整段跳过）。
auto bench_video_wgpu(bool streaming) -> WgpuFrameCost {
    const std::vector<Image> frames = make_video_frames();
    const auto gpu = offscreen_rhi(AURORA_VIDEO_W, AURORA_VIDEO_H);
    if (gpu == nullptr) {
        return {};
    }
    std::uint64_t version = 0;
    return time_batch(
        *gpu, AURORA_VIDEO_W, AURORA_VIDEO_H, AURORA_VIDEO_WARMUP, AURORA_VIDEO_FRAMES, [&](int i, DisplayList &dl) {
            push_video_frame(dl, frames[static_cast<std::size_t>(i % frames.size())], streaming, ++version);
        });
}

/// @brief 场景四：GPU 层缓存（transform-only 旋转动画，与场景二同一批输入）。
auto bench_layer_wgpu(bool cache_layer) -> WgpuFrameCost {
    std::shared_ptr<Column> root = make_grid_root();
    auto gpu = offscreen_rhi(AURORA_SCENE_W, AURORA_SCENE_H);
    if (gpu == nullptr) {
        return {};
    }
    constexpr BuildContext ctx;
    const Modifier base_modifier = cache_layer ? Modifier{}.cache_layer() : Modifier{};
    return time_batch(*gpu, AURORA_SCENE_W, AURORA_SCENE_H, AURORA_SCENE_WARMUP, AURORA_SCENE_FRAMES,
                      [&](int frame, DisplayList &dl) {
                          root->modifier.set(base_modifier.rotate(static_cast<float>(frame) * 6.0F));
                          Painter p;
                          p.begin(AURORA_SCENE_W, AURORA_SCENE_H);
                          p.record(dl);
                          root->paint(p,
                                      Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                                           .size = Size{.width = static_cast<float>(AURORA_SCENE_W),
                                                        .height = static_cast<float>(AURORA_SCENE_H)}},
                                      ctx);
                          p.stop();
                      });
}

struct MipResult {
    WgpuFrameCost cost;
    bool compute = false;  ///< 该适配器是否走 compute mip 链（GLES 端 false）
};

/// @brief 场景五（wgpu 独有）：AURORA_MIP_DIM×AURORA_MIP_DIM 大图缩到 AURORA_MIP_DRAW×AURORA_MIP_DRAW 重采样。
/// - `unique_content = false`：同一张图逐帧重绘 → 图像缓存命中，mip 链只生成一次；
/// - `unique_content = true`：每帧内容唯一（模拟解码输出）→ 每帧缓存未命中，
///   PMA 全帧副本 + 整幅上传 + 整条 mip 链重建（大图走错通道的代价即在此）。
auto bench_mip(bool unique_content) -> MipResult {
    auto gpu = offscreen_rhi(AURORA_MIP_DRAW, AURORA_MIP_DRAW);
    if (gpu == nullptr) {
        return {};
    }
    MipResult r;
    r.compute = gpu->capabilities().compute;

    constexpr int total = AURORA_MIP_WARMUP + AURORA_MIP_FRAMES;
    std::vector<Image> frames;
    frames.reserve(static_cast<std::size_t>(total));
    frames.push_back(make_noise_image(AURORA_MIP_DIM, AURORA_MIP_DIM, 1));
    for (int i = 1; i < total; ++i) {
        // 唯一内容变体逐帧新图；静态变体复用同一张（下标恒 0）。
        frames.push_back(unique_content
                             ? make_noise_image(AURORA_MIP_DIM, AURORA_MIP_DIM, static_cast<std::uint64_t>(i) + 1U)
                             : frames.front());
    }
    r.cost = time_batch(*gpu, AURORA_MIP_DRAW, AURORA_MIP_DRAW, AURORA_MIP_WARMUP, AURORA_MIP_FRAMES,
                        [&](int i, DisplayList &dl) {
                            // 绘制矩形 = 缩小后的目标框（4× 降采样；等大绘制不会命中 mip≥1）。
                            DrawCmd cmd;
                            cmd.kind = aurora::CmdKind::DrawImage;
                            cmd.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                                              .size = Size{.width = static_cast<float>(AURORA_MIP_DRAW),
                                                           .height = static_cast<float>(AURORA_MIP_DRAW)}};
                            cmd.image_idx = dl.add_image(frames[static_cast<std::size_t>(i % frames.size())]);
                            dl.push_cmd(cmd);
                        });
    return r;
}

/// @brief 场景六原型帧：非均匀底图（棋盘格 + 斜线带）+ 整幅三效果族（blur/blend/mask）
/// 连做 AURORA_FX_PASSES 遍。帧内容恒定（无图像上传），两路（compute / 片元兜底）回放同一
/// DisplayList，差额即效果路本身的成本；重数放大 GPU 工作量至可测。
[[nodiscard]] auto make_fx_proto() -> DisplayList {
    const auto mk = [](int x, int y, int w, int h) {
        return Rect{.origin = Point{.x = static_cast<float>(x), .y = static_cast<float>(y)},
                    .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}};
    };
    DisplayList dl;
    Painter p;
    p.begin(AURORA_FX_DIM, AURORA_FX_DIM);
    p.record(dl);
    p.fill_rect(mk(0, 0, AURORA_FX_DIM, AURORA_FX_DIM), Color(24, 28, 38));
    // 8×8 棋盘（64px 块）：为 blur 提供恒定高频边缘；颜色随格号变化避免纯色块退化。
    for (int cy = 0; cy < 8; ++cy) {
        for (int cx = 0; cx < 8; ++cx) {
            const auto shade = static_cast<std::uint8_t>(96 + (16 * (((cx * 3) + (cy * 5)) % 8)));
            p.fill_rect(mk(cx * 64, cy * 64, 64, 64),
                        (cx + cy) % 2 == 0 ? Color(shade, shade, static_cast<std::uint8_t>(shade + 24))
                                           : Color(static_cast<std::uint8_t>(220 - (shade / 2)), 224, 232));
        }
    }
    // 斜线带：跨象限的细高频内容（1px 宽对角线，间距 8px）。
    for (int i = 0; i < AURORA_FX_DIM; i += 8) {
        p.draw_line(Point{.x = static_cast<float>(i), .y = 0.0F}, Point{.x = 0.0F, .y = static_cast<float>(i)}, 1.0F,
                    Color(255, 255, 255));
    }
    p.blur_region(mk(0, 0, 256, 256), 6.0F);
    p.blend_region(mk(256, 0, 256, 256), BlendMode::Multiply, Color(255, 0, 255), 0.6F);
    p.mask_region(mk(0, 256, 256, 256), ShaderMaskKind::LinearFade, 1.0F);
    for (int i = 0; i < AURORA_FX_PASSES; ++i) {
        p.blur_region(mk(0, 0, AURORA_FX_DIM, AURORA_FX_DIM), 6.0F);
        p.blend_region(mk(0, 0, AURORA_FX_DIM, AURORA_FX_DIM), BlendMode::Multiply, Color(255, 0, 255), 0.6F);
        p.mask_region(mk(0, 0, AURORA_FX_DIM, AURORA_FX_DIM), ShaderMaskKind::LinearFade, 1.0F);
    }
    p.stop();
    return dl;
}

/// @brief 场景六：区域效果两路帧成本。`compute = true` 走 compute 实路（默认），
/// `false` 经 `set_compute_effects_enabled(false)` 强制片元兜底路（如同管线未建成）。
/// @return submit/e2e 均为 0 = 无可用 adapter（调用方整段跳过）。
auto bench_fx_wgpu(bool compute) -> WgpuFrameCost {
    auto gpu = offscreen_rhi(AURORA_FX_DIM, AURORA_FX_DIM);
    if (gpu == nullptr) {
        return {};
    }
    gpu->set_compute_effects_enabled(compute);
    const DisplayList proto = make_fx_proto();
    return time_batch(*gpu, AURORA_FX_DIM, AURORA_FX_DIM, AURORA_FX_WARMUP, AURORA_FX_FRAMES,
                      [&](int, DisplayList &dl) { dl = proto; });
}

auto run_wgpu_scenarios() -> void {
    AURORA_LOG_RAW("bench",
                   "## Scenario 3: real wgpu GPU offscreen - per-frame video updates (end-to-end frame cost, "
                   "submit and wait included)\n\n");
    const auto probe = offscreen_rhi(1, 1);
    if (probe == nullptr) {
        AURORA_LOG_RAW("bench", "(no usable wgpu adapter/device: scenarios 3/4/5/6 skipped entirely)\n\n");
        return;
    }
    AURORA_LOG_RAW("bench",
                   "| Path | Submit per frame (CPU) | End-to-end per frame (one drain at batch end) |\n"
                   "|:---|---:|---:|\n");
    const WgpuFrameCost wl_legacy = bench_video_wgpu(false);
    const WgpuFrameCost wl_stream = bench_video_wgpu(true);
    AURORA_LOG_RAW("bench", "| legacy (content_hash cache) | ", ffmt(3, wl_legacy.submit_ms), " ms | ",
                   ffmt(3, wl_legacy.e2e_ms), " ms |\n");
    AURORA_LOG_RAW("bench", "| streaming (resident streaming slot) | ", ffmt(3, wl_stream.submit_ms), " ms | ",
                   ffmt(3, wl_stream.e2e_ms), " ms |\n");
    AURORA_LOG_RAW("bench", "\nSubmit-basis speedup ",
                   ffmt(2, wl_stream.submit_ms > 0.0 ? wl_legacy.submit_ms / wl_stream.submit_ms : 0.0),
                   "x, end-to-end basis ", ffmt(2, wl_stream.e2e_ms > 0.0 ? wl_legacy.e2e_ms / wl_stream.e2e_ms : 0.0),
                   "x. Comparable with scenario 1 (fake stub) on the same basis: legacy sees unique content every "
                   "frame here, so every frame pays a premultiplied full-frame copy plus a new texture plus a "
                   "full cache flush once the size limit is exceeded, while streaming only uploads the whole image "
                   "into the reused slot (no CPU premultiply, slot reuse).\n\n");

    AURORA_LOG_RAW("bench", "## Scenario 4: real wgpu GPU offscreen - layer cache for a transform-only rotation\n\n");
    AURORA_LOG_RAW("bench",
                   "| Variant | Submit per frame (CPU) | End-to-end per frame (one drain at batch end) |\n"
                   "|:---|---:|---:|\n");
    const WgpuFrameCost wbase = bench_layer_wgpu(false);
    const WgpuFrameCost wcached = bench_layer_wgpu(true);
    AURORA_LOG_RAW("bench", "| baseline (no cache_layer) | ", ffmt(3, wbase.submit_ms), " ms | ", ffmt(3, wbase.e2e_ms),
                   " ms |\n");
    AURORA_LOG_RAW("bench", "| cache_layer (GPU layer cache) | ", ffmt(3, wcached.submit_ms), " ms | ",
                   ffmt(3, wcached.e2e_ms), " ms |\n");
    AURORA_LOG_RAW("bench", "\nSubmit-basis speedup ",
                   ffmt(2, wcached.submit_ms > 0.0 ? wbase.submit_ms / wcached.submit_ms : 0.0), "x, end-to-end basis ",
                   ffmt(2, wcached.e2e_ms > 0.0 ? wbase.e2e_ms / wcached.e2e_ms : 0.0),
                   "x. An end-to-end gap smaller than the submit gap shows the GPU side is not the bottleneck here "
                   "(every frame is either one DrawLayer or a full command translation, so the GPU workload is "
                   "small).\n\n");

    AURORA_LOG_RAW("bench", "## Scenario 5: wgpu large-image downsample (compute mip chain, ",
                   std::to_string(AURORA_MIP_DIM), "x", std::to_string(AURORA_MIP_DIM), " -> ",
                   std::to_string(AURORA_MIP_DRAW), "x", std::to_string(AURORA_MIP_DRAW), ")\n\n");
    AURORA_LOG_RAW("bench",
                   "| Variant | Submit per frame (CPU) | End-to-end per frame (one drain at batch end) |\n"
                   "|:---|---:|---:|\n");
    const MipResult mstatic = bench_mip(false);
    const MipResult munique = bench_mip(true);
    AURORA_LOG_RAW("bench", "| static same image (cache hit, mip chain built once) | ", ffmt(3, mstatic.cost.submit_ms),
                   " ms | ", ffmt(3, mstatic.cost.e2e_ms), " ms |\n");
    AURORA_LOG_RAW("bench", "| unique content per frame (mip chain rebuilt every frame) | ",
                   ffmt(3, munique.cost.submit_ms), " ms | ", ffmt(3, munique.cost.e2e_ms), " ms |\n");
    AURORA_LOG_RAW(
        "bench", "\ncompute capability flag ", mstatic.compute ? "true" : "false",
        " (false = GLES backend, no mip chain, both rows degrade to a plain upload comparison). The gap between the "
        "two rows is what routing a large image that changes every frame through the generic image cache costs: a "
        "premultiplied full-frame copy + a full upload + a compute rebuild of the whole mip chain level by level. "
        "The conclusion points the same way as scenario 3 - large image inputs updated every frame must use the "
        "streaming slot (the slot is always single-level, no mip churn). Caveat when reading absolute values: this "
        "section rebuilds the DisplayList every frame (including the full ",
        std::to_string(AURORA_MIP_DIM), "x", std::to_string(AURORA_MIP_DIM),
        " image value copy and content_hash computation), so the floor value of the static row is the **CPU cost of "
        "frame assembly** (the recording side pools by value, and the streaming branch does the same), not a GPU "
        "cost - the net gain of the streaming path is slot reuse, no premultiplied copy and no mip churn "
        "(scenario 3 shows exactly this gap).\n\n");

    AURORA_LOG_RAW("bench", "## Scenario 6: region effects, compute path vs fragment fallback (",
                   std::to_string(AURORA_FX_DIM), "x", std::to_string(AURORA_FX_DIM),
                   ", blur r6 + Multiply + LinearFade over the whole frame, ", std::to_string(AURORA_FX_PASSES),
                   " passes, ", std::to_string(AURORA_FX_FRAMES), " frames)\n\n");
    AURORA_LOG_RAW("bench",
                   "| Path | Submit per frame (CPU) | End-to-end per frame (one drain at batch end) |\n"
                   "|:---|---:|---:|\n");
    const WgpuFrameCost fxf = bench_fx_wgpu(false);
    const WgpuFrameCost fxc = bench_fx_wgpu(true);
    AURORA_LOG_RAW("bench", "| fragment fallback path (forced, `set_compute_effects_enabled(false)`) | ",
                   ffmt(3, fxf.submit_ms), " ms | ", ffmt(3, fxf.e2e_ms), " ms |\n");
    AURORA_LOG_RAW("bench", "| compute path (default) | ", ffmt(3, fxc.submit_ms), " ms | ", ffmt(3, fxc.e2e_ms),
                   " ms |\n");
    AURORA_LOG_RAW("bench", "\nSubmit-basis speedup ",
                   ffmt(2, fxc.submit_ms > 0.0 ? fxf.submit_ms / fxc.submit_ms : 0.0), "x, end-to-end basis ",
                   ffmt(2, fxc.e2e_ms > 0.0 ? fxf.e2e_ms / fxc.e2e_ms : 0.0), "x. compute capability flag ",
                   probe->capabilities().compute ? "true" : "false",
                   " (false = the compute pipeline never came up on GLES: both rows run the fragment path, so the "
                   "ratio is meaningless). The gap sits mostly in the end-to-end basis (the compute storage-texture "
                   "path for the effect family vs the fragment offscreen read-modify-write path); on the submit "
                   "basis both paths replay the same frame content, and the fragment path is slightly higher because "
                   "every effect assembles one extra offscreen render pass and bind group. Frame content here is "
                   "constant (checkerboard + diagonal bands + three full-frame effects, no image upload); the drain "
                   "quantum (about 15.6 ms per batch-end drain) amortizes over ",
                   std::to_string(AURORA_FX_FRAMES),
                   " frames into the e2e floor value, the same amount for both paths, so "
                   "it does not affect the gap.\n\n");
}

#endif  // AURORA_BACKEND_GPU_WGPU

auto run() -> void {
    AURORA_LOG_RAW("bench",
                   "# GPU feature benchmarks (scenarios 1/2 = deterministic counters from the fake GL stub + upload "
                   "memcpy cost; scenarios 3/4/5/6 = end-to-end frame cost on a real wgpu GPU offscreen)\n\n");

    // ---- 场景一：视频流式纹理 ----
    AURORA_LOG_RAW("bench", "## Scenario 1: per-frame video updates (", std::to_string(AURORA_VIDEO_W), "x",
                   std::to_string(AURORA_VIDEO_H), " x ", std::to_string(AURORA_VIDEO_FRAMES),
                   " frames, unique content every frame)\n\n");
    AURORA_LOG_RAW("bench",
                   "| Path | Texture allocations | Texture deletions | Upload bytes per frame | CPU per "
                   "frame |\n");
    AURORA_LOG_RAW("bench", "|:---|---:|---:|---:|---:|\n");
    const auto legacy = bench_video(false);
    const auto stream = bench_video(true);
    const auto per_frame_bytes = [](const GlCounters &c) {
        return ffmt(0, static_cast<double>(c.upload_bytes) / static_cast<double>(AURORA_VIDEO_FRAMES));
    };
    AURORA_LOG_RAW("bench", "| legacy (content_hash cache) | ", std::to_string(legacy.counters.texture_gens), " | ",
                   std::to_string(legacy.counters.texture_deletes), " | ", per_frame_bytes(legacy.counters), " | ",
                   ffmt(3, legacy.cpu_ms_per_frame), " ms |\n");
    AURORA_LOG_RAW("bench", "| streaming (resident streaming slot) | ", std::to_string(stream.counters.texture_gens),
                   " | ", std::to_string(stream.counters.texture_deletes), " | ", per_frame_bytes(stream.counters),
                   " | ", ffmt(3, stream.cpu_ms_per_frame), " ms |\n");
    const auto speedup = stream.cpu_ms_per_frame > 0.0 ? legacy.cpu_ms_per_frame / stream.cpu_ms_per_frame : 0.0;
    AURORA_LOG_RAW("bench", "\nlegacy CPU per frame is ", ffmt(2, speedup),
                   "x streaming's; the gap is the CPU-premultiplied full-frame copy plus texture create/evict churn "
                   "(upload bytes are of the same order for both paths - a full-frame video always transfers the "
                   "whole image every frame).\n\n");

    // ---- 场景二：GPU 层缓存 ----
    AURORA_LOG_RAW("bench", "## Scenario 2: transform-only rotation (", std::to_string(AURORA_GRID_COLS), "x",
                   std::to_string(AURORA_GRID_ROWS), " static box grid, ", std::to_string(AURORA_SCENE_W), "x",
                   std::to_string(AURORA_SCENE_H), ")\n\n");
    AURORA_LOG_RAW("bench", "| Variant | First frame (cold) | Steady-state per frame |\n");
    AURORA_LOG_RAW("bench", "|:---|---:|---:|\n");
    const auto base = bench_layer(false);
    const auto cached = bench_layer(true);
    AURORA_LOG_RAW("bench", "| baseline (no cache_layer) | ", ffmt(3, base.cold_ms), " ms | ",
                   ffmt(3, base.steady_ms_per_frame), " ms |\n");
    AURORA_LOG_RAW("bench", "| cache_layer (GPU layer cache) | ", ffmt(3, cached.cold_ms), " ms | ",
                   ffmt(3, cached.steady_ms_per_frame), " ms |\n");
    const auto layer_speedup =
        cached.steady_ms_per_frame > 0.0 ? base.steady_ms_per_frame / cached.steady_ms_per_frame : 0.0;
    AURORA_LOG_RAW("bench", "\nSteady-state speedup ", ffmt(2, layer_speedup),
                   "x. A non-identity rotation transform in baseline goes through the render_into offscreen "
                   "compositing path: every frame re-rasterizes the whole subtree into an offscreen bitmap before "
                   "compositing it; cache_layer intercepts before that, so a steady-state frame issues only one "
                   "DrawLayer (zero subtree redraws, zero pixel movement - the rotation takes effect in the "
                   "composite matrix).\n\n");

#ifdef AURORA_BACKEND_GPU_WGPU
    run_wgpu_scenarios();
#else
    AURORA_LOG_RAW("bench", "## Scenarios 3/4/5/6: not compiled (`AURORA_BACKEND_GPU_WGPU=OFF`)\n\n");
#endif

    AURORA_LOG_RAW("bench", AURORA_BENCH_DISCLAIMER, "\n");
}

}  // namespace

}  // namespace aurora::bench

// 入口不吞异常：基准程序的失败即未捕获异常 → terminate，与各 demo 入口同口径（场景数值自报，无需 try/catch）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    aurora::bench::run();
    return 0;
}
