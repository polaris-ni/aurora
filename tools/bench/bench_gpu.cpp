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
constexpr int VIDEO_W = 480;
constexpr int VIDEO_H = 270;
constexpr int VIDEO_FRAMES = 128;  // > 2×AURORA_IMAGE_CACHE_CAP(64)：观察 legacy 淘汰抖动
constexpr int VIDEO_WARMUP = 3;

constexpr int SCENE_W = 480;
constexpr int SCENE_H = 400;
constexpr int GRID_COLS = 12;   ///< 网格列数（盒 40×24 无重叠铺满 480×384）
constexpr int GRID_ROWS = 16;   ///< 网格行数（12×16 = 192 盒；不重叠——剔除剔除不干扰基线）
constexpr int SCENE_FRAMES = 60;
constexpr int SCENE_WARMUP = 3;

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

// 伪视频帧：逐帧内容唯一（模拟解码输出），RGBA8 直色（预乘由路径自行处理）。
auto make_video_frame(std::uint64_t seed) -> Image {
    Image img;
    img.width = VIDEO_W;
    img.height = VIDEO_H;
    img.pixels.resize(static_cast<std::size_t>(VIDEO_W) * static_cast<std::size_t>(VIDEO_H) * 4U);
    std::uint64_t h = seed * 0x9E3779B97F4A7C15ULL + 0x517C'CBFB'EB41'2C25ULL;
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        h = h * 6364136223846793005ULL + 1442695040888963407ULL;
        img.pixels[i + 0] = static_cast<std::uint8_t>(h >> 33);
        img.pixels[i + 1] = static_cast<std::uint8_t>(h >> 41);
        img.pixels[i + 2] = static_cast<std::uint8_t>(h >> 49);
        img.pixels[i + 3] = 255;
    }
    return img;
}

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
                      .size = Size{.width = static_cast<float>(img.width),
                                   .height = static_cast<float>(img.height)}};
    cmd.image_idx = dl.add_image(img);
    dl.push_cmd(cmd);
}

struct VideoResult {
    double cpu_ms_per_frame = 0.0;
    GlCounters counters;
};

/// @brief 视频场景：预生成 N 帧唯一内容，计时回放（含 begin/replay/end 全帧路径）。
auto bench_video(bool streaming) -> VideoResult {
    std::vector<Image> frames;
    frames.reserve(VIDEO_FRAMES);
    for (int i = 0; i < VIDEO_FRAMES; ++i) {
        frames.push_back(make_video_frame(static_cast<std::uint64_t>(i) + 1U));
    }

    testing::FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    rhi::RhiFrameSink &sink = rhi_obj;
    if (!rhi_obj.valid()) {
        return {};
    }

    // 预热：管线/槽位等一次性成本（streaming 键同槽续版本，稳态即「只 sub-upload」）。
    std::uint64_t version = 0;
    for (int i = 0; i < VIDEO_WARMUP; ++i) {
        (void)rhi_obj.begin_frame(VIDEO_W, VIDEO_H, 1.0F);
        DisplayList dl;
        push_video_frame(dl, frames[static_cast<std::size_t>(i)], streaming, ++version);
        dl.replay(sink.backend());
        rhi_obj.end_frame();
    }

    const GlCounters before = GlCounters::capture(fake);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < VIDEO_FRAMES; ++i) {
        (void)rhi_obj.begin_frame(VIDEO_W, VIDEO_H, 1.0F);
        DisplayList dl;
        push_video_frame(dl, frames[static_cast<std::size_t>(i)], streaming, ++version);
        dl.replay(sink.backend());
        rhi_obj.end_frame();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const GlCounters after = GlCounters::capture(fake);

    VideoResult r;
    r.cpu_ms_per_frame =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(VIDEO_FRAMES);
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
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(sz_);
    }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override {
        p.fill_rect(b, color_);
    }

  private:
    Size sz_;
    Color color_;
};

struct LayerResult {
    double cold_ms = 0.0;              ///< 首帧（层冷 / 离屏位图冷）
    double steady_ms_per_frame = 0.0;  ///< 稳态帧均
};

/// @brief 层缓存场景：一次装配布局，逐帧重录 + 回放；冷帧与稳态分开计时。
auto bench_layer(bool cache_layer) -> LayerResult {
    // 无重叠网格布局：12×16 = 192 个 40×24 静态盒（真实 Column/Row 布局——Stack 叠放
    // 全部子项共享同一 paint 原点，遮挡剔除只留可见者，基线失真）。
    std::vector<Node> rows;
    rows.reserve(GRID_ROWS);
    for (int r = 0; r < GRID_ROWS; ++r) {
        RowProps rp;
        rp.children.reserve(GRID_COLS);
        for (int c = 0; c < GRID_COLS; ++c) {
            const auto idx = static_cast<unsigned>(r * GRID_COLS + c);
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
    c.max = Size{.width = static_cast<float>(SCENE_W), .height = static_cast<float>(SCENE_H)};
    root->layout(c, ctx);

    testing::FakeGl fake;
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
        p.begin(SCENE_W, SCENE_H);
        p.record(dl);
        root->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                            .size = Size{.width = static_cast<float>(SCENE_W),
                                         .height = static_cast<float>(SCENE_H)}},
                    ctx);
        p.stop();
        (void)rhi_obj.begin_frame(SCENE_W, SCENE_H, 1.0F);
        dl.replay(sink.backend());
        rhi_obj.end_frame();
    };

    // 冷帧（首帧）单独计时；随后预热至稳态再计时 SCENE_FRAMES 帧。
    const auto c0 = std::chrono::steady_clock::now();
    run_frame(0);
    const auto c1 = std::chrono::steady_clock::now();

    for (int i = 1; i <= SCENE_WARMUP; ++i) {
        run_frame(SCENE_FRAMES + i);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < SCENE_FRAMES; ++i) {
        run_frame(SCENE_WARMUP + 1 + i);
    }
    const auto t1 = std::chrono::steady_clock::now();

    LayerResult r;
    r.cold_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();
    r.steady_ms_per_frame =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(SCENE_FRAMES);
    return r;
}

auto run() -> void {
    AURORA_LOG_RAW("bench", "# GPU 特性基准（fake GL 桩：确定性计数 + 上传 memcpy 成本）\n\n");

    // ---- 场景一：视频流式纹理 ----
    AURORA_LOG_RAW("bench", "## 场景一：视频逐帧更新（", std::to_string(VIDEO_W), "x", std::to_string(VIDEO_H),
                   " x ", std::to_string(VIDEO_FRAMES), " 帧，每帧内容唯一）\n\n");
    AURORA_LOG_RAW("bench", "| 路径 | 纹理分配次数 | 纹理删除次数 | 每帧上传字节 | 每帧 CPU |\n");
    AURORA_LOG_RAW("bench", "|:---|---:|---:|---:|---:|\n");
    const auto legacy = bench_video(false);
    const auto stream = bench_video(true);
    const auto per_frame_bytes = [](const GlCounters &c) {
        return ffmt(0, static_cast<double>(c.upload_bytes) / static_cast<double>(VIDEO_FRAMES));
    };
    AURORA_LOG_RAW("bench", "| legacy（content_hash 缓存） | ", std::to_string(legacy.counters.texture_gens), " | ",
                   std::to_string(legacy.counters.texture_deletes), " | ", per_frame_bytes(legacy.counters),
                   " | ", ffmt(3, legacy.cpu_ms_per_frame), " ms |\n");
    AURORA_LOG_RAW("bench", "| streaming（常驻流式槽） | ", std::to_string(stream.counters.texture_gens), " | ",
                   std::to_string(stream.counters.texture_deletes), " | ", per_frame_bytes(stream.counters),
                   " | ", ffmt(3, stream.cpu_ms_per_frame), " ms |\n");
    const auto speedup = stream.cpu_ms_per_frame > 0.0 ? legacy.cpu_ms_per_frame / stream.cpu_ms_per_frame : 0.0;
    AURORA_LOG_RAW("bench", "\nlegacy 每帧 CPU 为 streaming 的 ", ffmt(2, speedup),
                   " 倍；差额即 CPU 预乘全帧副本 + 纹理新建/淘汰 churn（上传字节两路径同量级，"
                   "全帧视频逐帧必有整幅传输）。\n\n");

    // ---- 场景二：GPU 层缓存 ----
    AURORA_LOG_RAW("bench", "## 场景二：transform-only 旋转动画（", std::to_string(GRID_COLS), "×",
                   std::to_string(GRID_ROWS), " 静态盒网格，", std::to_string(SCENE_W), "x",
                   std::to_string(SCENE_H), "）\n\n");
    AURORA_LOG_RAW("bench", "| 变体 | 首帧（冷） | 稳态每帧 |\n");
    AURORA_LOG_RAW("bench", "|:---|---:|---:|\n");
    const auto base = bench_layer(false);
    const auto cached = bench_layer(true);
    AURORA_LOG_RAW("bench", "| baseline（无 cache_layer） | ", ffmt(3, base.cold_ms), " ms | ",
                   ffmt(3, base.steady_ms_per_frame), " ms |\n");
    AURORA_LOG_RAW("bench", "| cache_layer（GPU 层缓存） | ", ffmt(3, cached.cold_ms), " ms | ",
                   ffmt(3, cached.steady_ms_per_frame), " ms |\n");
    const auto layer_speedup =
        cached.steady_ms_per_frame > 0.0 ? base.steady_ms_per_frame / cached.steady_ms_per_frame : 0.0;
    AURORA_LOG_RAW("bench", "\n稳态加速比 ", ffmt(2, layer_speedup),
                   " 倍。baseline 的非恒等旋转变换走 render_into 离屏合成路径：每帧整棵子树重栅格化到"
                   "离屏位图再 composite；cache_layer 在此之前拦截，稳态帧仅一条 DrawLayer（子树零重绘、"
                   "零像素搬运，旋转在合成矩阵侧生效）。\n\n");

    AURORA_LOG_RAW("bench", AURORA_BENCH_DISCLAIMER, "\n");
}

}  // namespace

}  // namespace aurora::bench

auto main() -> int {
    aurora::bench::run();
    return 0;
}
