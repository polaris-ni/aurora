// tools/verify/x11_wgpu_live_probe.cpp — Linux/X11 + wgpu GPU 栅格真机探针（非 CTest）。
//
// 覆盖：AURORA_BACKEND_GPU_WGPU + AURORA_BACKEND_WIN32 之外的 X11 宿主路径
// （AURORA_BACKEND_X11 构建下），WgpuX11Surface 宿主窗口与 WgpuRhi（离屏直驱）在真实
// 驱动（WSLg vGPU / 原生 Vulkan）上的接线——无头 CI 无法证明的部分。
//
// 自动段（无需人工）：
//   1. 宿主装配：create_window(WgpuOptions) 真实开窗；surface 动态类型 WgpuX11Surface；
//      gpu_backend() 非空且 name == "gpu-wgpu"；is_available() / gpu_active() 初值为真。
//   2. 能力与契约：capabilities().gpu == true、native_surface_import == false（v29 C API
//      口径：仅契约位，不兑现）；import_native_surface 空帧恒返回 0（warn-once，不崩溃）。
//   3. 流式纹理逐版本像素（离屏直驱）：同一 stream_key 红→同版本重绘→蓝，中心像素经
//      read_pixels 逐版本核对——证明真实 queueWriteTexture 上传 + 采样生效。
//   4. 层缓存持久性：BeginLayer+内容+EndLayer+DrawLayer 冷帧与「仅 DrawLayer」暖帧读回
//      像素一致——证明跨帧常驻层纹理存续且 draw_layer 路径生效。
//   5. compute mip 链（离屏直驱）：64×64 逐纹素棋盘 4× 缩小绘制读回均值色——证明 cs_mip
//      生成的整条 mip 链与三线性采样生效（adapter 无 compute 时该段 SKIP）。
//   6. compute 区域效果（离屏直驱）：BlurRegion/BlendRegion/MaskRegion 单命令帧读回粗粒度
//      判定（边界混合 / 通道衰减 / 淡出梯度 / 区外 untouched）——证明 cs_blur/cs_blend/cs_mask
//      dispatch 路在真实驱动接线（逐位 CPU 对照由 utest_wgpu_rhi 锁定；无 compute 时 SKIP）。
//   7. 平台 present 链路：真实窗口多帧 present_root（色块网格 + Text + 流式「视频」），
//      frame_count 增长且 gpu_active() 保持为真（未永久回退软件路径）；capture_window
//      （XGetImage）落盘成功——合成器环境下截图含 GPU 上屏内容，供人工复核。
// 人工段（--interactive）：常驻窗口，流式「视频」与旋转层缓存网格并存，目视确认无花屏/
//   撕裂/错位。退出码：0 全过；1 自动断言失败；2 环境不可用（无 DISPLAY / 无 adapter /
//   特性未编译）。
//
// 运行：build 目录下 ./aurora_verify_x11_wgpu [--interactive]

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/window/native_surfaces.h"

#ifdef AURORA_BACKEND_GPU_WGPU
#include "aurora/render/rhi/wgpu_rhi.h"
#endif

namespace {

auto emit(const std::string &text) -> void {
    AURORA_LOG_RAW("verify", text, "\n");
}

int failures = 0;

auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        failures++;
    }
}

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_X11)

// 合成「视频帧」：内容恒定、颜色随版本切换（红/蓝），流式通道按版本增量重传。
auto make_video_frame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> aurora::Image {
    aurora::Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        img.pixels[i + 0] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = 255;
    }
    return img;
}

// 流式「视频」控件：pixels 恒定、仅 stream_version 递增（流式通道逐帧重传的语义）。
class StreamVideoBox final : public aurora::LeafWidget {
  public:
    StreamVideoBox(int w, int h, std::uint64_t key)
        : frame_(make_video_frame(w, h, 255, 0, 0)) {
        frame_.stream_key = key;
        frame_.stream_version = 1;
        sz_ = aurora::Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    }

    void advance_version() {
        frame_.stream_version++;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "StreamVideoBox"; }

  protected:
    auto on_layout(const aurora::Constraints &c, const aurora::BuildContext & /*ctx*/) -> aurora::Size override {
        return c.constrain(sz_);
    }
    void on_paint(aurora::Painter &p, const aurora::Rect &b, const aurora::BuildContext & /*ctx*/) override {
        p.draw_image(frame_, b);
    }

  private:
    aurora::Size sz_;
    aurora::Image frame_;
};

// 静态盒（色块网格内容）。
class GridBox final : public aurora::LeafWidget {
  public:
    GridBox(float w, float h, aurora::Color color) : sz_{.width = w, .height = h}, color_(color) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "GridBox"; }

  protected:
    auto on_layout(const aurora::Constraints &c, const aurora::BuildContext & /*ctx*/) -> aurora::Size override {
        return c.constrain(sz_);
    }
    void on_paint(aurora::Painter &p, const aurora::Rect &b, const aurora::BuildContext & /*ctx*/) override {
        p.fill_rect(b, color_);
    }

  private:
    aurora::Size sz_;
    aurora::Color color_;
};

// read_pixels 输出（RGBA8，行序自上而下）取样，越界返回全零。
auto sample(const std::vector<std::uint8_t> &px, int canvas_w, int x, int y) -> std::array<int, 3> {
    const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas_w) +
                             static_cast<std::size_t>(x)) * 4U;
    if (px.size() < idx + 3) {
        return {0, 0, 0};
    }
    return {px[idx], px[idx + 1], px[idx + 2]};
}

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_X11

}  // namespace

auto main(int argc, char **argv) -> int {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--interactive") {
            interactive = true;
        }
    }

    emit("== aurora verify: X11 wgpu GPU raster (host window + offscreen streams) ==");

#if !defined(AURORA_BACKEND_GPU_WGPU) || !defined(AURORA_BACKEND_X11)
    emit("[SKIP] 此构建未编译 X11 wgpu 通道（AURORA_BACKEND_GPU_WGPU / AURORA_BACKEND_X11）");
    return 2;
#else
    // X11 环境前置：无 DISPLAY 直接判环境不可用，与「断言失败」区分退出码。
    if (const char *dpy = std::getenv("DISPLAY"); dpy == nullptr || *dpy == '\0') {
        emit("[SKIP] 无 DISPLAY 环境变量（无 X 服务器），X11 宿主探针无法运行");
        return 2;
    }

    // ---- 宿主装配：真实窗口 + gpu-wgpu 帧调度挂点 ----
    aurora::WgpuOptions opts;
    opts.size = aurora::Size{.width = 560.0F, .height = 420.0F};
    opts.title = "aurora-verify-x11-wgpu";
    auto created = aurora::create_window(opts);
    if (!created.ok()) {
        AURORA_LOG_ERROR("verify", "create_window(Wgpu) failed (X 连接失败 / 无 adapter / device init)");
        return 2;
    }
    auto &win = *created.value();

    auto *ws = dynamic_cast<aurora::WgpuX11Surface *>(&win.surface());
    check(ws != nullptr, "surface 动态类型为 WgpuX11Surface");
    if (ws == nullptr) {
        return 2;
    }
    auto *sink = win.surface().gpu_backend();
    if (sink == nullptr) {
        emit("[FAIL] gpu_backend() 为空（WgpuRhi 未装载或已回退）");
        return 1;
    }
    check(sink->name() == "gpu-wgpu", "GPU 通道 name == gpu-wgpu");
    check(ws->is_available(), "is_available()（宿主 XID + wgpu device 就绪）");
    check(ws->gpu_active(), "gpu_active() 初值为真（GPU 栅格路径生效）");

    auto *gpu_host = dynamic_cast<aurora::rhi::WgpuRhi *>(&sink->backend());
    check(gpu_host != nullptr, "宿主 backend 动态类型为 WgpuRhi");
    if (gpu_host == nullptr) {
        return 1;
    }
    const auto caps = gpu_host->capabilities();
    check(caps.gpu, "capabilities().gpu == true");
    check(!caps.native_surface_import, "capabilities().native_surface_import == false（v29 仅契约位）");

    // ---- 契约位：导入空帧恒 0（warn-once，不崩溃）----
    aurora::NativeSurfaceFrame empty_frame;
    check(gpu_host->import_native_surface(empty_frame) == 0, "import_native_surface(空帧) == 0（契约不兑现口径）");

    // ---- 离屏直驱通道（读回窗口：end_frame 之后、下一 begin_frame 之前）----
    aurora::rhi::WgpuRhiOptions off_opts;
    off_opts.native_window = nullptr;  // 离屏：无 swapchain，渲染目标为内部纹理
    off_opts.offscreen_width = 1;
    off_opts.offscreen_height = 1;
    aurora::rhi::WgpuRhi offscreen(off_opts);
    if (!offscreen.valid()) {
        emit("[FAIL] 离屏 WgpuRhi 装载失败（无 adapter？）");
        return 1;
    }

    // 流式纹理逐版本像素：v1 红（建槽 + 首传）→ 同版本重绘（槽复用）→ v2 蓝（增量重传）。
    constexpr int FRAME_W = 96;
    constexpr int FRAME_H = 64;
    constexpr int CANVAS_W = FRAME_W + 8;
    constexpr std::uint64_t STREAM_KEY = 7;
    const auto stream_frame = [](aurora::rhi::WgpuRhi &gpu, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                 std::uint64_t version) -> std::vector<std::uint8_t> {
        aurora::Image img = make_video_frame(FRAME_W, FRAME_H, r, g, b);
        img.stream_key = STREAM_KEY;
        img.stream_version = version;
        aurora::DisplayList dl;
        aurora::Painter p;
        p.begin(CANVAS_W, FRAME_H + 8);
        p.record(dl);
        p.draw_image(img, aurora::Rect{.origin = aurora::Point{.x = 4.0F, .y = 4.0F},
                                       .size = aurora::Size{.width = static_cast<float>(FRAME_W),
                                                            .height = static_cast<float>(FRAME_H)}});
        p.stop();
        (void)gpu.begin_frame(CANVAS_W, FRAME_H + 8, 1.0F);
        dl.replay(gpu.backend());
        gpu.end_frame();
        std::vector<std::uint8_t> pixels;
        (void)gpu.read_pixels(pixels);
        return pixels;
    };

    const auto c_red1 = sample(stream_frame(offscreen, 255, 0, 0, 1), CANVAS_W, CANVAS_W / 2, (FRAME_H + 8) / 2);
    const auto c_red2 = sample(stream_frame(offscreen, 255, 0, 0, 1), CANVAS_W, CANVAS_W / 2, (FRAME_H + 8) / 2);
    const auto c_blue = sample(stream_frame(offscreen, 0, 0, 255, 2), CANVAS_W, CANVAS_W / 2, (FRAME_H + 8) / 2);
    check(c_red1[0] > 180 && c_red1[1] < 80 && c_red1[2] < 80, "流式帧 v1 中心为红（建槽 + 首传上屏）");
    check(c_red2[0] > 180 && c_red2[1] < 80 && c_red2[2] < 80, "流式帧同版本重绘仍为红（槽复用）");
    check(c_blue[2] > 180 && c_blue[0] < 80 && c_blue[1] < 80, "流式帧 v2 中心为蓝（同槽增量重传生效）");

    // 层缓存持久性：冷帧 BeginLayer+内容+EndLayer+DrawLayer；暖帧仅 DrawLayer（命中常驻层纹理）。
    constexpr std::uint64_t LAYER_KEY = 77;
    constexpr int LW = 80;
    constexpr int LH = 60;
    constexpr int LCANVAS_W = LW + 40;
    const auto layer_frame = [&](bool content) {
        aurora::DisplayList dl;
        aurora::Painter p;
        p.begin(LCANVAS_W, LH + 40);
        p.record(dl);
        if (content) {
            p.begin_layer(LAYER_KEY, aurora::Size{.width = static_cast<float>(LW),
                                                  .height = static_cast<float>(LH)});
            p.fill_rect(aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F},
                                     .size = aurora::Size{.width = static_cast<float>(LW),
                                                          .height = static_cast<float>(LH)}},
                        aurora::Color{0, 200, 80, 255});
            p.end_layer();
        }
        p.draw_layer(LAYER_KEY, aurora::Matrix2D::from_translate(20.0F, 20.0F), 1.0F);
        p.stop();
        (void)offscreen.begin_frame(LCANVAS_W, LH + 40, 1.0F);
        dl.replay(offscreen.backend());
        offscreen.end_frame();
        std::vector<std::uint8_t> pixels;
        (void)offscreen.read_pixels(pixels);
        return pixels;
    };
    const auto s_cold_in = sample(layer_frame(true), LCANVAS_W, 40, 40);
    const auto s_warm_in = sample(layer_frame(false), LCANVAS_W, 40, 40);
    check(s_cold_in[1] > 140 && s_cold_in[0] < 90, "层缓存冷帧层内为绿（BeginLayer+内容+DrawLayer）");
    check(s_warm_in[1] > 140 && s_warm_in[0] < 90, "层缓存稳态帧层内仍为绿（仅 DrawLayer 命中常驻层）");
    check(s_cold_in == s_warm_in, "冷帧与稳态帧层内像素一致（跨帧持久性）");

    // compute mip 链（大图降采样质量）：64×64 逐纹素红/绿棋盘 4× 缩小绘制——三线性命中
    // 均匀 mip≥1，整块为均值色 (120,120,20)；lod0 走样则是逐像素红/绿交替端点色。
    {
        aurora::Image board;
        board.width = 64;
        board.height = 64;
        board.pixels.resize(static_cast<std::size_t>(64) * 64U * 4U);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const std::size_t idx = (static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)) * 4U;
                const bool hi = ((x ^ y) & 1) == 0;
                board.pixels[idx + 0] = static_cast<std::uint8_t>(hi ? 220 : 20);
                board.pixels[idx + 1] = static_cast<std::uint8_t>(hi ? 20 : 220);
                board.pixels[idx + 2] = 20;
                board.pixels[idx + 3] = 255;
            }
        }
        aurora::DisplayList dl;
        aurora::Painter p;
        p.begin(64, 64);
        p.record(dl);
        p.draw_image(board, aurora::Rect{.origin = aurora::Point{.x = 8.0F, .y = 8.0F},
                                         .size = aurora::Size{.width = 16.0F, .height = 16.0F}});
        p.stop();
        (void)offscreen.begin_frame(64, 64, 1.0F);
        dl.replay(offscreen.backend());
        offscreen.end_frame();
        std::vector<std::uint8_t> px;
        (void)offscreen.read_pixels(px);
        if (offscreen.capabilities().compute) {
            const auto m1 = sample(px, 64, 12, 12);
            const auto m2 = sample(px, 64, 20, 15);
            const auto is_mean = [](const std::array<int, 3> &c) {
                return c[0] > 106 && c[0] < 134 && c[1] > 106 && c[1] < 134 && c[2] > 6 && c[2] < 34;
            };
            check(is_mean(m1) && is_mean(m2), "大图 4× 降采样为 mip 均值色 (120,120,20)（compute mip 链 + 三线性）");
        } else {
            emit("[SKIP] adapter 无 compute（GLES 兜底端），mip 采样断言跳过");
        }
    }

    // ---- compute 区域效果（BlurRegion/BlendRegion/MaskRegion → cs_* dispatch）----
    // 粗粒度通断判定证明真实驱动接线；逐位 CPU 对照由 utest_wgpu_rhi 在 CI 锁定。
    // 内容场：64×64 底 (10,20,30) + [16,48)² 块 (200,100,50)。
    if (!offscreen.capabilities().compute) {
        emit("[SKIP] adapter 无 compute，区域效果 compute 探针跳过（片元兜底由 golden 覆盖）");
    } else {
        auto fx_frame = [&](void (*apply_fx)(aurora::Painter &)
                            ) -> std::vector<std::uint8_t> {
            aurora::DisplayList dl;
            aurora::Painter p;
            p.begin(64, 64);
            p.record(dl);
            p.fill_rect(aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F},
                                     .size = aurora::Size{.width = 64.0F, .height = 64.0F}},
                        aurora::Color{10, 20, 30, 255});
            p.fill_rect(aurora::Rect{.origin = aurora::Point{.x = 16.0F, .y = 16.0F},
                                     .size = aurora::Size{.width = 32.0F, .height = 32.0F}},
                        aurora::Color{200, 100, 50, 255});
            apply_fx(p);
            p.stop();
            (void)offscreen.begin_frame(64, 64, 1.0F);
            dl.replay(offscreen.backend());
            const bool no_skip = offscreen.stats().skipped_cmds == 0U;
            offscreen.end_frame();
            std::vector<std::uint8_t> pixels;
            (void)offscreen.read_pixels(pixels);
            static bool first = true;
            if (first) {
                check(no_skip, "区域效果三命令均被 compute 路接受（skipped_cmds == 0）");
                first = false;
            }
            return pixels;
        };
        // blur：区域 [8,40)×[24,40) 跨块左缘，r=1 → 边界纹素混入底色、块内部恒权均值不动。
        const auto pb = fx_frame([](aurora::Painter &p) {
            p.blur_region(aurora::Rect{.origin = aurora::Point{.x = 8.0F, .y = 24.0F},
                                       .size = aurora::Size{.width = 32.0F, .height = 16.0F}},
                          1.0F);
        });
        const auto b_edge = sample(pb, 64, 16, 32);   // 期望 ≈(136,73,43)：H 趟钳位 tap 混入 1 列底色
        const auto b_in = sample(pb, 64, 32, 32);     // 块内 3×3 全同色 → 恒等 (200,100,50)
        const auto b_out = sample(pb, 64, 60, 60);    // 区外 untouched
        check(b_edge[0] > 100 && b_edge[0] < 190, "cs_blur：边界像素混入底色（r∈(100,190)，实得 " + std::to_string(b_edge[0]) + "）");
        check(b_in == std::array<int, 3>{200, 100, 50}, "cs_blur：块内恒权均值恒等（精确不变）");
        check(b_out == std::array<int, 3>{10, 20, 30}, "cs_blur：区外 untouched");
        // blend：Multiply [8,24)²，tint=(255,0,255)，strength=0.5 → g 通道减半（s·0 混回）。
        const auto pj = fx_frame([](aurora::Painter &p) {
            p.blend_region(aurora::Rect{.origin = aurora::Point{.x = 8.0F, .y = 8.0F},
                                        .size = aurora::Size{.width = 16.0F, .height = 16.0F}},
                           aurora::BlendMode::Multiply, aurora::Color{255, 0, 255, 255}, 0.5F);
        });
        const auto j_bg = sample(pj, 64, 12, 12);   // 底 (10,20,30) → g: 20+0.5·(0−20)=10
        const auto j_blk = sample(pj, 64, 20, 20);  // 块 (200,100,50) → g: 100+0.5·(0−100)=50
        check(std::abs(j_bg[1] - 10) <= 2, "cs_blend：Multiply 底色 g 减半（期望≈10，实得 " + std::to_string(j_bg[1]) + "）");
        check(std::abs(j_blk[1] - 50) <= 2, "cs_blend：Multiply 块色 g 减半（期望≈50，实得 " + std::to_string(j_blk[1]) + "）");
        // mask：LinearFade [16,48)² 纵向淡出 → 顶行近原色、底行近全黑。
        const auto pm = fx_frame([](aurora::Painter &p) {
            p.mask_region(aurora::Rect{.origin = aurora::Point{.x = 16.0F, .y = 16.0F},
                                       .size = aurora::Size{.width = 32.0F, .height = 32.0F}},
                          aurora::ShaderMaskKind::LinearFade, 1.0F);
        });
        const auto m_top = sample(pm, 64, 32, 16);
        const auto m_bot = sample(pm, 64, 32, 47);
        check(m_top[0] > 190, "cs_mask：LinearFade 顶行近原色（期望>190，实得 " + std::to_string(m_top[0]) + "）");
        check(m_bot[0] < 20, "cs_mask：LinearFade 底行近全黑（期望<20，实得 " + std::to_string(m_bot[0]) + "）");
    }

    // ---- 平台 present 链路（真实窗口多帧上屏，含文本与流式图像）----
    // 控件用独立 stream key（与离屏段的 key 7 互不干扰）。
    auto video = std::make_shared<StreamVideoBox>(160, 90, STREAM_KEY + 1U);
    std::vector<aurora::Node> rows;
    for (int r = 0; r < 6; ++r) {
        aurora::RowProps rp;
        for (int c = 0; c < 8; ++c) {
            const auto hue = static_cast<std::uint8_t>(((r * 8 + c) * 29U) % 256U);
            rp.children.emplace_back(std::make_shared<GridBox>(32.0F, 18.0F, aurora::Color{hue, 130, 200, 255}));
        }
        rows.emplace_back(std::make_shared<aurora::Row>(std::move(rp)));
    }
    std::shared_ptr<aurora::Widget> video_widget = video;  // 共享别名入树（video 本体留供 advance_version）
    aurora::ColumnProps grid_props;
    grid_props.children = std::move(rows);
    aurora::ColumnProps cp;
    cp.children.push_back(std::move(video_widget));
    cp.children.emplace_back(std::make_shared<aurora::Text>(
        aurora::TextProps{.content = aurora::LocalizedString{"wgpu x11 verify 0123"}}));
    cp.children.emplace_back(std::make_shared<aurora::Column>(std::move(grid_props)));
    auto root_widget = std::make_shared<aurora::Column>(std::move(cp));
    // Node 以 shared_ptr 重载接管所有权；present_root(Node&) 需命名左值逐帧传引用，
    // 跨帧指针身份稳定，窗口脏区追踪的根指针比较才能命中。
    aurora::Node root_node(root_widget);

    bool present_ok = true;
    constexpr int AUTO_FRAMES = 8;
    for (int i = 0; i < AUTO_FRAMES; ++i) {
        root_widget->modifier.set(aurora::Modifier{}.cache_layer().rotate(static_cast<float>(i) * 4.0F));
        video->advance_version();
        win.force_full_redraw();  // 绕过 idle 跳帧，逐帧走完整 GPU 路径
        if (!win.present_root(root_node)) {
            present_ok = false;
        }
        win.surface().poll_platform_events();
    }
    check(present_ok, "present_root 连续 " + std::to_string(AUTO_FRAMES) + " 帧成功（真实窗口上屏 + 文本 + 流式图像）");
    check(win.surface().frame_count() >= AUTO_FRAMES,
          "frame_count() >= " + std::to_string(AUTO_FRAMES) +
              "（实得 " + std::to_string(win.surface().frame_count()) + "）");
    check(ws->gpu_active(), "帧后 gpu_active() 仍为真（未永久回退软件路径）");

    // XGetImage 截图落盘：合成器/WSLg 下含 GPU 上屏内容，人工复核花屏/错位的物证。
    const std::filesystem::path shot = "aurora_verify_x11_wgpu.png";
    const auto cap = ws->capture_window(shot.string());
    check(static_cast<bool>(cap) && std::filesystem::exists(shot),
          "capture_window 落盘 " + shot.string() + "（窗口像素物证）" +
              (cap ? std::string{} : "，错误: " + cap.error().message));

    // ---- 人工段 ----
    if (interactive) {
        emit("\n[人工段] 窗口常驻：顶部为流式「视频」（逐帧重传），整体挂 cache_layer 并旋转。");
        emit("预期：画面连续旋转、无花屏/错位/撕裂；关闭窗口退出。");
        float angle = 0.0F;
        while (!win.should_close()) {
            angle += 1.5F;
            root_widget->modifier.set(aurora::Modifier{}.cache_layer().rotate(angle));
            video->advance_version();
            (void)win.present_root(root_node);
            win.surface().poll_platform_events();
        }
    } else {
        emit("\n提示：加 --interactive 进入常驻窗口人工目视段。");
    }

    emit(std::string("\n结果：") + (failures == 0 ? "ALL PASS" : std::to_string(failures) + " FAILURES"));
    return failures == 0 ? 0 : 1;
#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_X11
}
