// tools/verify/win32_wgpu_live_probe.cpp — Win32 + wgpu GPU 栅格真机探针（非 CTest）。
//
// 覆盖：AURORA_BACKEND_GPU_WGPU + AURORA_BACKEND_WIN32 构建下，WgpuSurface 宿主窗口与
// WgpuRhi（离屏直驱）在真实驱动（Vulkan/D3D12）上的接线——无头 CI 无法证明的部分。
//
// 自动段（无需人工）：
//   1. 宿主装配：create_window(WgpuOptions) 真实开窗；surface 动态类型 WgpuSurface；
//      gpu_backend() 非空且 name == "gpu-wgpu"；is_available() / gpu_active() 初值为真。
//   2. 能力与契约：capabilities().gpu == true、native_surface_import == false（v29 C API
//      口径：仅契约位，不兑现）；import_native_surface 空帧恒返回 0（warn-once，不崩溃）。
//   3. 流式纹理逐版本像素（离屏直驱）：同一 stream_key 红→同版本重绘→蓝，中心像素经
//      read_pixels 逐版本核对——证明真实 queueWriteTexture 上传 + 采样生效。
//   4. 层缓存持久性：BeginLayer+内容+EndLayer+DrawLayer 冷帧与「仅 DrawLayer」暖帧读回
//      像素一致——证明跨帧常驻层纹理存续且 draw_layer 路径生效。
//   5. 平台 present 链路：真实窗口多帧 present_root（色块网格 + Text + 流式「视频」），
//      frame_count 增长且 gpu_active() 保持为真（未永久回退软件路径）。
// 人工段（--interactive）：常驻窗口，流式「视频」与旋转层缓存网格并存，目视确认无花屏/
//   撕裂/错位。退出码：0 全过；1 自动断言失败；2 环境不可用（无显示 / 无 adapter /
//   特性未编译）。
//
// 运行：build 目录下 ./aurora_verify_win32_wgpu [--interactive]

#include <array>
#include <cstdint>
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

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_WIN32)

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

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WIN32

}  // namespace

auto main(int argc, char **argv) -> int {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--interactive") {
            interactive = true;
        }
    }

    emit("== aurora verify: Win32 wgpu GPU raster (host window + offscreen streams) ==");

#if !defined(AURORA_BACKEND_GPU_WGPU) || !defined(AURORA_BACKEND_WIN32)
    emit("[SKIP] 此构建未编译 WGPU 通道（AURORA_BACKEND_GPU_WGPU / AURORA_BACKEND_WIN32）");
    return 2;
#else
    aurora::enable_dpi_awareness();
    aurora::init_console();

    // ---- 宿主装配：真实窗口 + gpu-wgpu 帧调度挂点 ----
    aurora::WgpuOptions opts;
    opts.size = aurora::Size{.width = 560.0F, .height = 420.0F};
    opts.title = "aurora-verify-win32-wgpu";
    auto created = aurora::create_window(opts);
    if (!created.ok()) {
        AURORA_LOG_ERROR("verify", "create_window(Wgpu) failed (no display / no adapter / device init)");
        return 2;
    }
    auto &win = *created.value();

    auto *ws = dynamic_cast<aurora::WgpuSurface *>(&win.surface());
    check(ws != nullptr, "surface 动态类型为 WgpuSurface");
    if (ws == nullptr) {
        return 2;
    }
    auto *sink = win.surface().gpu_backend();
    if (sink == nullptr) {
        emit("[FAIL] gpu_backend() 为空（WgpuRhi 未装载或已回退）");
        return 1;
    }
    check(sink->name() == "gpu-wgpu", "GPU 通道 name == gpu-wgpu");
    check(ws->is_available(), "is_available()（宿主 HWND + wgpu device 就绪）");
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
        aurora::TextProps{.content = aurora::LocalizedString{"wgpu verify 0123"}}));
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
#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WIN32
}
