// tools/verify/glfw_gpu_features_live_probe.cpp — GL GPU 特性真机探针（非 CTest）。
//
// 覆盖：AURORA_BACKEND_GLFW + AURORA_ENABLE_GLFW_GPU_GL 构建下，常驻流式纹理与
// GPU 层缓存（cache_layer）在真实 GL 驱动上的接线——无头 CI 无法证明的部分。
//
// 自动段（无需人工）：
//   1. 能力位核对：gpu 通道存在且 name == "gpu-gl"；capabilities().gpu == true；
//      native_surface_import == false（GL 3.3 契约口径：仅契约位，不兑现）。
//   2. 契约核对：import_native_surface 空帧恒返回 0（warn-once，不崩溃）。
//   3. 流式纹理逐版本像素：同一 stream_key 连续两版本（红→蓝）经 DrawImage 命令回放，
//      read_pixels 读回中心像素应逐版本变化——证明真实 glTexSubImage2D 上传 + 采样生效。
//   4. 层缓存持久性：BeginLayer+内容+EndLayer+DrawLayer 帧与「仅 DrawLayer」帧读回像素
//      一致——证明 FBO 常驻层纹理跨帧存续且 draw_layer 路径生效。
//   5. 平台 present 链路：present_root 连续多帧成功（真实窗口上屏）。
// 人工段（--interactive）：常驻窗口，流式「视频」与旋转层缓存网格并存，目视确认无花屏/
//   撕裂/错位。退出码：0 全过；1 自动断言失败；2 环境不可用（无显示 / 无 GPU 通道）。
//
// 运行：build 目录下 ./aurora_verify_glfw_gpu_features [--interactive]

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/rhi/gpu_gl_rhi.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

int failures = 0;

auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        failures++;
    }
}

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
    StreamVideoBox(int w, int h, std::uint64_t key) : frame_(make_video_frame(w, h, 255, 0, 0)) {
        frame_.stream_key = key;
        frame_.stream_version = 1;
        sz_ = aurora::Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    }

    void advance_version() { frame_.stream_version++; }

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

// 静态盒（层缓存网格内容）。
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

// 直驱一帧：录 DL → begin_frame → replay →（可选 read_pixels）→ end_frame。
// 手动 begin/end 不触发窗口 swap，read_pixels 读到的是本帧合成结果。

}  // namespace

auto main(int argc, char **argv) -> int {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--interactive") {
            interactive = true;
        }
    }

    emit("== aurora verify: GLFW GPU features (stream texture + layer cache) ==");

    // ---- 窗口与 GPU 通道 ----
    aurora::GlfwOptions opts;
    opts.size = aurora::Size{.width = 560.0F, .height = 420.0F};
    opts.title = "aurora-verify-glfw-gpu-features";
    opts.resizable = false;
    opts.gpu = true;
    opts.max_fps = 0;
    auto created = aurora::create_window(opts);
    if (!created.ok()) {
        AURORA_LOG_ERROR("verify", "create_window failed (no display / GLFW init failed)");
        return 2;
    }
    auto &win = *created.value();

    auto *sink = win.surface().gpu_backend();
    if (sink == nullptr) {
        emit("[SKIP] 此构建未启用 GLFW GPU 通道（AURORA_ENABLE_GLFW_GPU_GL=OFF 或回退软件路径）");
        return 2;
    }
    auto &backend = sink->backend();
    check(backend.name() == "gpu-gl", "GPU 通道 name == gpu-gl");
    auto *gpu = dynamic_cast<aurora::rhi::GpuGlRhi *>(&backend);

    const auto caps = gpu->capabilities();
    check(caps.gpu, "capabilities().gpu == true");
    check(!caps.native_surface_import, "capabilities().native_surface_import == false（GL 3.3 仅契约位）");

    // ---- 契约位：导入空帧恒 0（warn-once，不崩溃）----
    aurora::NativeSurfaceFrame empty_frame;
    const auto imported = gpu->import_native_surface(empty_frame);
    check(imported == 0, "import_native_surface(空帧) == 0（契约不兑现口径）");

    // ---- 流式纹理逐版本像素（真实 GL 上传 + 采样）----
    // 同一 stream_key：v1 红（建槽 + 首传）→ v1 重绘（同版本命中常驻槽，无新上传）→
    // v2 蓝（版本递进触发增量重传）。中心像素逐帧读回核对。
    constexpr int FRAME_W = 96;
    constexpr int FRAME_H = 64;
    constexpr std::uint64_t STREAM_KEY = 7;
    const auto stream_frame = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                  std::uint64_t version) -> std::vector<std::uint8_t> {
        aurora::Image img = make_video_frame(FRAME_W, FRAME_H, r, g, b);
        img.stream_key = STREAM_KEY;
        img.stream_version = version;
        aurora::DisplayList dl;
        aurora::Painter p;
        p.begin(FRAME_W + 8, FRAME_H + 8);
        p.record(dl);
        p.draw_image(img, aurora::Rect{.origin = aurora::Point{.x = 4.0F, .y = 4.0F},
                                       .size = aurora::Size{.width = static_cast<float>(FRAME_W),
                                                            .height = static_cast<float>(FRAME_H)}});
        p.stop();
        (void)gpu->begin_frame(FRAME_W + 8, FRAME_H + 8, 1.0F);
        dl.replay(gpu->backend());
        std::vector<std::uint8_t> pixels;
        (void)gpu->read_pixels(pixels);
        gpu->end_frame();
        return pixels;
    };

    const auto red1 = stream_frame(255, 0, 0, 1);
    const auto red2 = stream_frame(255, 0, 0, 1);
    const auto blue = stream_frame(0, 0, 255, 2);

    const auto center_of = [&](const std::vector<std::uint8_t> &px) -> std::array<int, 3> {
        // read_pixels 为 GL 底行序；图像矩形在画布内居中对称，中心取样与行序无关。
        constexpr std::size_t row = FRAME_H + 8;
        constexpr std::size_t col = FRAME_W + 8;
        constexpr std::size_t idx = ((row / 2) * col + (col / 2)) * 4U;
        if (px.size() < idx + 3) {
            return {0, 0, 0};
        }
        return {px[idx], px[idx + 1], px[idx + 2]};
    };
    const auto c_red1 = center_of(red1);
    const auto c_red2 = center_of(red2);
    const auto c_blue = center_of(blue);
    check(c_red1[0] > 180 && c_red1[1] < 80 && c_red1[2] < 80, "流式帧 v1 中心为红（建槽 + 首传上屏）");
    check(c_red2[0] > 180 && c_red2[1] < 80 && c_red2[2] < 80, "流式帧同版本重绘仍为红（槽复用）");
    check(c_blue[2] > 180 && c_blue[0] < 80 && c_blue[1] < 80, "流式帧 v2 中心为蓝（同槽增量重传生效）");

    // ---- 层缓存持久性（FBO 常驻层 + DrawLayer）----
    // 冷帧：BeginLayer + 内容 + EndLayer + DrawLayer；稳态帧：仅 DrawLayer（命中 FBO）。
    constexpr std::uint64_t LAYER_KEY = 77;
    constexpr int LW = 80;
    constexpr int LH = 60;
    const auto layer_frame = [&](bool content) {
        aurora::DisplayList dl;
        aurora::Painter p;
        p.begin(LW + 40, LH + 40);
        p.record(dl);
        if (content) {
            p.begin_layer(LAYER_KEY, aurora::Size{.width = static_cast<float>(LW), .height = static_cast<float>(LH)});
            p.fill_rect(
                aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F},
                             .size = aurora::Size{.width = static_cast<float>(LW), .height = static_cast<float>(LH)}},
                aurora::Color{0, 200, 80, 255});
            p.end_layer();
        }
        p.draw_layer(LAYER_KEY, aurora::Matrix2D::from_translate(20.0F, 20.0F), 1.0F);
        p.stop();
        (void)gpu->begin_frame(LW + 40, LH + 40, 1.0F);
        dl.replay(gpu->backend());
        std::vector<std::uint8_t> pixels;
        (void)gpu->read_pixels(pixels);
        gpu->end_frame();
        return pixels;
    };
    const auto layer_cold = layer_frame(true);
    const auto layer_warm = layer_frame(false);
    const auto sample = [&](const std::vector<std::uint8_t> &px, int x, int y) -> std::array<int, 3> {
        constexpr std::size_t col = LW + 40;
        const std::size_t idx = (static_cast<std::size_t>(y) * col + static_cast<std::size_t>(x)) * 4U;
        if (px.size() < idx + 3) {
            return {0, 0, 0};
        }
        return {px[idx], px[idx + 1], px[idx + 2]};
    };
    // 层矩形在画布内居中（20..100 x 20..80 于 120x100），取样点与行序无关。
    const auto s_cold_in = sample(layer_cold, 40, 40);
    const auto s_warm_in = sample(layer_warm, 40, 40);
    check(s_cold_in[1] > 140 && s_cold_in[0] < 90, "层缓存冷帧层内为绿（BeginLayer+内容+DrawLayer）");
    check(s_warm_in[1] > 140 && s_warm_in[0] < 90, "层缓存稳态帧层内仍为绿（仅 DrawLayer 命中 FBO）");
    check(s_cold_in == s_warm_in, "冷帧与稳态帧层内像素一致（跨帧持久性）");

    // ---- 平台 present 链路（真实窗口多帧上屏）----
    // 控件用独立 stream key（与上面直驱段的 key 7 互不干扰）。
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
    std::shared_ptr<aurora::Widget> video_widget = video;  // 共享别名入树（video 本体保留供 advance_version）
    aurora::ColumnProps grid_props;
    grid_props.children = std::move(rows);
    aurora::ColumnProps cp;
    cp.children.emplace_back(std::move(video_widget));
    cp.children.emplace_back(std::make_shared<aurora::Column>(std::move(grid_props)));
    auto root_widget = std::make_shared<aurora::Column>(std::move(cp));
    // Node 以 shared_ptr 重载接管所有权（无控件拷贝）；present_root(Node&) 需命名左值逐帧传引用，
    // 跨帧指针身份稳定，窗口脏区追踪的根指针比较才能命中。
    aurora::Node root_node(root_widget);

    bool present_ok = true;
    constexpr int AUTO_FRAMES = 8;
    for (int i = 0; i < AUTO_FRAMES; ++i) {
        root_widget->modifier.set(aurora::Modifier{}.cache_layer().rotate(static_cast<float>(i) * 4.0F));
        video->advance_version();
        if (!win.present_root(root_node)) {
            present_ok = false;
        }
    }
    check(present_ok, "present_root 连续 " + std::to_string(AUTO_FRAMES) + " 帧成功（平台上屏链路）");

    // ---- 人工段 ----
    if (interactive) {
        emit("\n[人工段] 窗口常驻：上半为流式「视频」（逐帧重传），整体挂 cache_layer 并旋转。");
        emit("预期：画面连续旋转、无花屏/错位；关闭窗口退出。");
        float angle = 0.0F;
        while (!win.should_close()) {
            angle += 1.5F;
            root_widget->modifier.set(aurora::Modifier{}.cache_layer().rotate(angle));
            video->advance_version();
            (void)win.present_root(root_node);
        }
    } else {
        emit("\n提示：加 --interactive 进入常驻窗口人工目视段。");
    }

    emit(std::string("\n结果：") + (failures == 0 ? "ALL PASS" : std::to_string(failures) + " FAILURES"));
    return failures == 0 ? 0 : 1;
}
