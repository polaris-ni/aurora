/// 测试类型: unit
/// 目标单元: include/aurora/render/rhi/gpu_gl_rhi.h + include/aurora/render/rhi/rhi_frame_sink.h
/// 测试说明: GLFn 函数表完整性判定与 load_gl 空装载；GpuGlRhi 占位（未装载）契约；
/// fake GL 驱动桩下的帧生命周期（begin 零基底 / 命令翻译批切分 / end 上屏 blit——resolve
/// 懒执行 / read_pixels）；渐变 LUT 内容语义与缓存合批；图像 PMA 纹理与内容摘要缓存
///（Image::content_hash + invalidate 契约）；文本字形图集（R8 子上传 / 槽位缓存 /
/// Text-Solid 断批 / 空字形与空文本契约 / 多页架式 + 页数封顶 LRU 淘汰）；效果管线
///（Shadow 单批同构 / BlurRegion ping-pong 双 pass / Blend-Mask 单 pass 直写 /
/// Composite 仿射四角顶点 / 脏 resolve 跟踪）；初始化失败链（函数表缺项 / 版本不足 /
/// 链接失败）→ valid()=false 软件回退契约。

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "aurora/core/native_surface.h"
#include "aurora/render/detail/gpu_layer.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/gpu_gl_rhi.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "framework/aurora_test.h"
#include "support/fake_gl.h"

namespace aurora::test_cases::utest_gpu_gl_rhi {

namespace rhi = aurora::rhi;  // GLFn / GL 类型别名（GLenum_ 等）均居 rhi 命名空间

namespace {

// fake GL 驱动桩与 GL 常量值由 tests/support/fake_gl.h 提供（utest 与 bench 共用同一桩）。
using aurora::testing::AURORA_GL_FILTER_LINEAR;
using aurora::testing::AURORA_GL_FILTER_NEAREST;
using aurora::testing::FakeGl;

auto make_fill(Rect bounds, Color color) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = bounds;
    cmd.color = color;
    return cmd;
}

// 线性渐变命令：色标/停靠入 DisplayList 池并回填下标（与 Painter 录制路径同形）。
auto make_linear_grad(Rect bounds, Point start, Point end, const std::vector<Color> &colors,
                      const std::vector<float> &stops, DisplayList &dl) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::LinearGradient;
    cmd.bounds = bounds;
    cmd.pt0 = start;
    cmd.pt1 = end;
    cmd.col_idx = dl.add_colors(colors);
    cmd.flt_idx = dl.add_floats(stops);
    return cmd;
}

// 图像命令：Image 入 DisplayList 池并回填下标（add_image 拷贝 Image，与录制路径同形）。
auto make_image_cmd(Rect bounds, const Image &img, DisplayList &dl) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::DrawImage;
    cmd.bounds = bounds;
    cmd.image_idx = dl.add_image(img);
    return cmd;
}

// 文本命令：字符串/字体入池并回填下标（与 Painter 录制路径同形）。
auto make_text_cmd(Rect bounds, const std::string &s, const Font &f, DisplayList &dl) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::DrawText;
    cmd.bounds = bounds;
    cmd.color = Color{20, 20, 20, 255};
    cmd.str_idx = dl.add_string(s);
    cmd.font_idx = dl.add_font(f);
    return cmd;
}

// 阴影命令（f0/f1 = 偏移，f2 = 模糊半径，同 Painter::draw_shadow 录制）。
auto make_shadow_cmd(Rect bounds, float dx, float dy, float blur, Color color) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::Shadow;
    cmd.bounds = bounds;
    cmd.f0 = dx;
    cmd.f1 = dy;
    cmd.f2 = blur;
    cmd.color = color;
    return cmd;
}

// 区域效果命令（blur 半径 / blend 模式+tint+强度 / mask 类型+强度，同软件录制）。
auto make_blur_cmd(Rect bounds, float radius) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::BlurRegion;
    cmd.bounds = bounds;
    cmd.f0 = radius;
    return cmd;
}
auto make_blend_cmd(Rect bounds, BlendMode mode, Color tint, float strength) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::BlendRegion;
    cmd.bounds = bounds;
    cmd.blend_mode = mode;
    cmd.color = tint;
    cmd.f0 = strength;
    return cmd;
}
auto make_mask_cmd(Rect bounds, ShaderMaskKind kind, float strength) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::MaskRegion;
    cmd.bounds = bounds;
    cmd.mask_kind = kind;
    cmd.f0 = strength;
    return cmd;
}

// 离屏合成命令：图像/矩阵入池并回填下标（与 Painter::composite 录制路径同形）。
auto make_composite_cmd(const Image &img, const Matrix2D &mat, float src_scale, DisplayList &dl) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::Composite;
    cmd.image_idx = dl.add_image(img);
    cmd.matrix_idx = dl.add_matrix(mat);
    cmd.composite_scale = src_scale;
    return cmd;
}

// 任意名装载回调：恒返回非空地址，验证 load_gl 的逐项装载路径。
auto any_proc(const char * /*name*/) -> void * {
    return reinterpret_cast<void *>(1);  // NOLINT(*-reinterpret-cast)
}

}  // namespace

AURORA_TEST_CASE(glfn_completeness_and_loader) {
    // 空函数表：complete() 为 false，缺项可检测。
    const rhi::GLFn empty{};
    AURORA_TEST_CHECK_FALSE(empty.complete());
    AURORA_TEST_CHECK_TRUE(empty.create_shader == nullptr);
    AURORA_TEST_CHECK_TRUE(empty.blit_framebuffer == nullptr);

    // load_gl(nullptr)：全空装载（无上下文/驱动缺项时的安全路径）。
    const rhi::GLFn null_loaded = rhi::load_gl(nullptr);
    AURORA_TEST_CHECK_FALSE(null_loaded.complete());
    AURORA_TEST_CHECK_TRUE(null_loaded.draw_elements == nullptr);

    // 全量回填桩：complete() 为 true（签名/覆盖完整性由本桩强制）。
    FakeGl fake;
    AURORA_TEST_CHECK_TRUE(fake.fn.complete());

    // 任意名装载回调：逐项非空、整体完整。
    const rhi::GLFn any_loaded = rhi::load_gl(&any_proc);
    AURORA_TEST_CHECK_TRUE(any_loaded.complete());
    AURORA_TEST_CHECK_TRUE(any_loaded.read_pixels != nullptr);
}

AURORA_TEST_CASE(gpu_gl_default_constructed_invalid) {
    rhi::GpuGlRhi rhi_obj;
    AURORA_TEST_CHECK_FALSE(rhi_obj.valid());
    AURORA_TEST_CHECK_EQ(rhi_obj.name(), std::string_view("gpu-gl"));
    // 未装载：帧接口整体拒绝，统计为零，读回失败；end_frame 幂等安全。
    AURORA_TEST_CHECK_FALSE(rhi_obj.begin_frame(64, 48, 1.0F));
    AURORA_TEST_CHECK_NO_THROW(rhi_obj.end_frame());
    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats.draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(stats.vertices, 0U);
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);
    std::vector<std::uint8_t> pixels;
    AURORA_TEST_CHECK_FALSE(rhi_obj.read_pixels(pixels));
    AURORA_TEST_CHECK_TRUE(pixels.empty());

    // 命令消费面：未装载下 submit 为安全 no-op。
    DisplayList dl;
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 8.0F, .height = 8.0F}},
                          Color{255, 0, 0, 255}));
    AURORA_TEST_CHECK_NO_THROW(dl.replay(rhi_obj.backend()));
}

AURORA_TEST_CASE(gpu_gl_fake_frame_lifecycle_and_batching) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());

    // 非法尺寸拒绝。
    AURORA_TEST_CHECK_FALSE(rhi_obj.begin_frame(0, 48, 1.0F));
    AURORA_TEST_CHECK_FALSE(rhi_obj.begin_frame(64, -1, 1.0F));
    AURORA_TEST_CHECK_EQ(fake.clears, 0);

    // begin：帧缓冲就绪 + 零基底清屏。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    AURORA_TEST_CHECK_EQ(fake.clears, 1);
    // resolve 帧缓冲已就绪：可读回（RGBA8，64*48*4）。
    std::vector<std::uint8_t> pixels;
    AURORA_TEST_CHECK_TRUE(rhi_obj.read_pixels(pixels));
    AURORA_TEST_CHECK_EQ(pixels.size(), static_cast<std::size_t>(64) * 48U * 4U);

    // 命令流：fill / PushClip / fill / SetAlpha / DrawLine / RoundedBorder / DrawText / PopClip /
    //          ClearRect / fill
    // 批切分推导（管线/裁剪态/混合态变化断批；SetAlpha 烘焙进顶点色不断批）：
    //   A: fill（无裁剪）                    → PushClip 触发 flush   dc=1
    //   B: fill + DrawLine（同裁剪同管线）   → RoundedBorder 触发     dc=2
    //   C: RoundedBorder（Border 管线）      → PopClip 触发           dc=3
    //   D: ClearRect（blend_off 断批）       → fill 触发              dc=4
    //   E: fill                              → end_frame flush        dc=5
    DisplayList dl;
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 8.0F, .y = 8.0F}, .size = Size{.width = 40.0F, .height = 24.0F}},
                          Color{200, 40, 40, 255}));
    DrawCmd clip;
    clip.kind = CmdKind::PushClip;
    clip.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 32.0F}};
    dl.push_cmd(clip);
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 12.0F, .y = 12.0F}, .size = Size{.width = 16.0F, .height = 8.0F}},
                          Color{40, 200, 40, 255}));
    DrawCmd alpha;
    alpha.kind = CmdKind::SetAlpha;
    alpha.alpha = 0.5;
    dl.push_cmd(alpha);
    DrawCmd line;
    line.kind = CmdKind::DrawLine;
    line.pt0 = Point{.x = 4.0F, .y = 4.0F};
    line.pt1 = Point{.x = 60.0F, .y = 44.0F};
    line.f0 = 2.0F;
    line.color = Color{0, 0, 0, 255};
    dl.push_cmd(line);
    DrawCmd border;
    border.kind = CmdKind::RoundedBorder;
    border.bounds = Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = Size{.width = 60.0F, .height = 44.0F}};
    border.f0 = 8.0F;
    border.f1 = 2.0F;
    border.color = Color{20, 20, 20, 255};
    dl.push_cmd(border);
    DrawCmd text;
    text.kind = CmdKind::DrawText;  // 无文本数据：静默跳过（同空命令契约，不计数）
    dl.push_cmd(text);
    DrawCmd pop;
    pop.kind = CmdKind::PopClip;
    dl.push_cmd(pop);
    DrawCmd clear;
    clear.kind = CmdKind::ClearRect;
    clear.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}};
    dl.push_cmd(clear);
    dl.push_cmd(make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 16.0F}},
                          Color{40, 40, 200, 255}));

    // 经 RhiFrameSink::backend() 派发（公共集成路径与 Window::present_root 同形）。
    rhi::RhiFrameSink &sink = rhi_obj;
    AURORA_TEST_CHECK_EQ(sink.name(), std::string_view("gpu-gl"));
    dl.replay(sink.backend());
    rhi_obj.end_frame();

    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats.draw_calls, 5U);
    AURORA_TEST_CHECK_EQ(stats.vertices, 24U);  // A:4 + B:8 + C:4 + D:4 + E:4
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.draw_calls, 5);
    AURORA_TEST_CHECK_EQ(fake.clears, 1);
    AURORA_TEST_CHECK_EQ(fake.blits, 2);  // read_pixels 懒 resolve 1 + 上屏直 blit 1

    // 第二帧：stats 逐帧复位；同尺寸复用帧缓冲（仅一次额外 clear）。
    // 无效果命令且无读回：end_frame 走 MSAA 直 blit（零中间 resolve）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList frame2;
    frame2.push_cmd(
        make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}},
                  Color{255, 255, 255, 255}));
    frame2.replay(sink.backend());
    rhi_obj.end_frame();
    const auto stats2 = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats2.draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(stats2.vertices, 4U);
    AURORA_TEST_CHECK_EQ(stats2.skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.clears, 2);
    AURORA_TEST_CHECK_EQ(fake.blits, 3);  // 本帧仅上屏 1 次
    AURORA_TEST_CHECK_EQ(fake.draw_calls, 6);

    // 尺寸变化触发帧缓冲重建。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 24, 1.0F));
    AURORA_TEST_CHECK_TRUE(rhi_obj.read_pixels(pixels));
    AURORA_TEST_CHECK_EQ(pixels.size(), static_cast<std::size_t>(32) * 24U * 4U);
}

AURORA_TEST_CASE(gpu_gl_init_failures_fall_back_to_software) {
    // 链接失败 → valid() 为 false，帧接口拒绝（调用方整体回退软件路径）。
    FakeGl link_fail(/*fail_link=*/true);
    rhi::GpuGlRhi bad_link(link_fail.fn);
    AURORA_TEST_CHECK_FALSE(bad_link.valid());
    AURORA_TEST_CHECK_FALSE(bad_link.begin_frame(64, 48, 1.0F));

    // 版本不足（< 3.3）→ 拒绝。
    FakeGl old_gl(/*fail_link=*/false, "3.2.14893 Compatibility Profile Context");
    rhi::GpuGlRhi bad_version(old_gl.fn);
    AURORA_TEST_CHECK_FALSE(bad_version.valid());

    // 函数表缺项 → complete() 拦截。
    FakeGl ok_gl;
    rhi::GLFn truncated = ok_gl.fn;
    truncated.draw_elements = nullptr;
    rhi::GpuGlRhi bad_fn(truncated);
    AURORA_TEST_CHECK_FALSE(bad_fn.valid());
}

AURORA_TEST_CASE(gpu_gl_gradient_lut_semantics_and_batching) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}};
    const std::vector<Color> warm{Color{255, 0, 0, 255}, Color{0, 0, 255, 255}};
    const std::vector<float> full{0.0F, 1.0F};

    // 同色标双命令：同一 LUT（一次上传）+ 同几何同裁剪 → 单批单 draw。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_linear_grad(area, Point{.x = 0.0F, .y = 0.0F}, Point{.x = 64.0F, .y = 0.0F}, warm, full, dl));
    dl.push_cmd(make_linear_grad(area, Point{.x = 0.0F, .y = 0.0F}, Point{.x = 64.0F, .y = 0.0F}, warm, full, dl));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats.draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 1U);  // 仅一张 LUT（resolve 纹理空数据上传不入列）

    // LUT 内容：texel j = sample_gradient(t=j/255)。端点精确，中点 ±2（截断 + 浮点漂移容差）。
    const auto &lut = fake.uploads[0];
    AURORA_TEST_CHECK_EQ(lut.width, 256);
    AURORA_TEST_CHECK_EQ(lut.height, 1);
    AURORA_TEST_CHECK_EQ(lut.data.size(), static_cast<std::size_t>(256) * 4U);
    AURORA_TEST_CHECK_EQ(lut.data[0], 255);  // texel 0 = 首色（红）
    AURORA_TEST_CHECK_EQ(lut.data[1], 0);
    AURORA_TEST_CHECK_EQ(lut.data[2], 0);
    AURORA_TEST_CHECK_EQ(lut.data[3], 255);
    const std::size_t last = 255U * 4U;
    AURORA_TEST_CHECK_EQ(lut.data[last + 0], 0);  // texel 255 = 尾色（蓝）
    AURORA_TEST_CHECK_EQ(lut.data[last + 1], 0);
    AURORA_TEST_CHECK_EQ(lut.data[last + 2], 255);
    AURORA_TEST_CHECK_EQ(lut.data[last + 3], 255);
    AURORA_TEST_CHECK_NEAR(lut.data[128U * 4U + 0], 127.0F, 2.0F);  // 中点：红→蓝插值
    AURORA_TEST_CHECK_NEAR(lut.data[128U * 4U + 2], 128.0F, 2.0F);

    // 换色标（缓存未命中→第二次上传、批 key 变化→断批）；第三条命中缓存不再上传。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl2;
    const std::vector<Color> cool{Color{0, 200, 0, 255}, Color{255, 255, 0, 255}};
    dl2.push_cmd(make_linear_grad(area, Point{.x = 0.0F, .y = 0.0F}, Point{.x = 64.0F, .y = 0.0F}, warm, full, dl2));
    dl2.push_cmd(make_linear_grad(area, Point{.x = 0.0F, .y = 0.0F}, Point{.x = 64.0F, .y = 0.0F}, cool, full, dl2));
    dl2.push_cmd(make_linear_grad(area, Point{.x = 0.0F, .y = 0.0F}, Point{.x = 64.0F, .y = 0.0F}, warm, full, dl2));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 3U);   // warm→cool→warm 三段批
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 2U);          // cool 未命中一次，warm 命中缓存

    // 退化方向（start==end）→ 实心管线首色填充（软件 fill_rect 同形；不产生 LUT 上传）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl3;
    dl3.push_cmd(make_linear_grad(area, Point{.x = 8.0F, .y = 8.0F}, Point{.x = 8.0F, .y = 8.0F}, warm, full, dl3));
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 2U);

    // 径向：radius=0 跳过（软件同契约）；正常径向命中已有 LUT。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl4;
    DrawCmd radial;
    radial.kind = CmdKind::RadialGradient;
    radial.bounds = area;
    radial.pt0 = Point{.x = 32.0F, .y = 24.0F};
    radial.f0 = 24.0F;
    radial.col_idx = dl4.add_colors(warm);
    radial.flt_idx = dl4.add_floats(full);
    DrawCmd radial_zero = radial;
    radial_zero.f0 = 0.0F;
    dl4.push_cmd(radial_zero);
    dl4.push_cmd(radial);
    dl4.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 2U);

    // stops 不从 0 起：t=0 取首色、区间外回落尾色（与软件 sample_gradient 逐位一致）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl5;
    const std::vector<float> late{0.5F, 1.0F};
    dl5.push_cmd(make_linear_grad(area, Point{.x = 0.0F, .y = 0.0F}, Point{.x = 64.0F, .y = 0.0F}, warm, late, dl5));
    dl5.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 3U);
    const auto &lut5 = fake.uploads.back();
    AURORA_TEST_CHECK_EQ(lut5.data[0], 255);              // t=0 → 首色（红）
    AURORA_TEST_CHECK_EQ(lut5.data[100U * 4U + 2], 255);  // t≈0.39 无区间命中 → 尾色（蓝）

    // 空色标：直接跳过（软件同契约），无绘制无上传。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl6;
    DrawCmd empty_grad;
    empty_grad.kind = CmdKind::LinearGradient;
    empty_grad.bounds = area;
    empty_grad.pt0 = Point{.x = 0.0F, .y = 0.0F};
    empty_grad.pt1 = Point{.x = 64.0F, .y = 0.0F};
    empty_grad.col_idx = dl6.add_colors({});
    empty_grad.flt_idx = dl6.add_floats(full);
    dl6.push_cmd(empty_grad);
    dl6.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 3U);
}

AURORA_TEST_CASE(gpu_gl_image_tex_cache_and_batching) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 32.0F, .height = 16.0F}};

    // 2×1 图像：px0 不透明、px1 半透明——验证上传副本为预乘 alpha（PMA）。
    Image img;
    img.width = 2;
    img.height = 1;
    img.pixels = {200, 100, 50, 255, 200, 100, 50, 128};

    // 同内容双命令：一次上传 + 同纹理同裁剪 → 单批单 draw。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_image_cmd(area, img, dl));
    dl.push_cmd(make_image_cmd(area, img, dl));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 1U);
    const auto &up = fake.uploads[0];
    AURORA_TEST_CHECK_EQ(up.width, 2);
    AURORA_TEST_CHECK_EQ(up.height, 1);
    AURORA_TEST_CHECK_EQ(up.data.size(), static_cast<std::size_t>(2) * 4U);
    AURORA_TEST_CHECK_EQ(up.data[0], 200);  // 不透明像素 PMA = 原色
    AURORA_TEST_CHECK_EQ(up.data[1], 100);
    AURORA_TEST_CHECK_EQ(up.data[2], 50);
    AURORA_TEST_CHECK_EQ(up.data[3], 255);
    AURORA_TEST_CHECK_EQ(up.data[4], 100);  // 半透明像素 rgb 预乘 (200*128+127)/255 = 100
    AURORA_TEST_CHECK_EQ(up.data[5], 50);
    AURORA_TEST_CHECK_EQ(up.data[6], 25);
    AURORA_TEST_CHECK_EQ(up.data[7], 128);

    // 换内容 → 第二次上传 + PMA 混合批与渐变/实心批互斥（断批）。
    //（拷贝携带旧摘要缓存，直接改写 pixels 后须 invalidate_content_hash——公共契约。）
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl2;
    Image other = img;
    other.pixels = {10, 20, 30, 255, 40, 50, 60, 255};
    other.invalidate_content_hash();
    dl2.push_cmd(make_image_cmd(area, img, dl2));    // 缓存命中
    dl2.push_cmd(make_image_cmd(area, other, dl2));  // 未命中
    dl2.push_cmd(make_fill(area, Color{0, 0, 0, 255}));  // Solid 管线断批
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 3U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 2U);

    // 非法图像（空像素 / 尺寸不足）：跳过不绘制，与软件不变量校验同形。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl3;
    Image bad_empty;
    Image bad_short;
    bad_short.width = 4;
    bad_short.height = 4;
    bad_short.pixels = {1, 2, 3, 4};  // 远小于 4*4*4
    dl3.push_cmd(make_image_cmd(area, bad_empty, dl3));
    dl3.push_cmd(make_image_cmd(area, bad_short, dl3));
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 2U);
}

AURORA_TEST_CASE(gpu_gl_glyph_atlas_text_pipeline) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 8.0F, .y = 8.0F}, .size = Size{.width = 48.0F, .height = 32.0F}};
    const Font font;

    // 首帧：真实字形（内置字体 + FreeType）经字形发射桥上传 R8 图集槽位，Text 管线单批。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_text_cmd(area, "Ag", font, dl));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats.draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);
    AURORA_TEST_CHECK_TRUE(stats.vertices > 0U);
    AURORA_TEST_CHECK_EQ(fake.sub_uploads.size(), 2U);  // "Ag" 两个非空字形，各一次子上传
    for (const auto &up : fake.sub_uploads) {
        AURORA_TEST_CHECK_TRUE(up.width > 0);
        AURORA_TEST_CHECK_TRUE(up.height > 0);
        AURORA_TEST_CHECK_EQ(up.data.size(),
                             static_cast<std::size_t>(up.width) * static_cast<std::size_t>(up.height));
    }

    // 第二帧：同文本命中 GPU 槽位缓存 → 零新上传；同管线同裁剪 → 单批。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl2;
    dl2.push_cmd(make_text_cmd(area, "Ag", font, dl2));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(fake.sub_uploads.size(), 2U);  // 缓存命中：无新子上传

    // 新字形（未缓存）→ 一次新上传；文本批与实心批互斥（Text vs Solid 管线断批）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl3;
    dl3.push_cmd(make_text_cmd(area, "Ag", font, dl3));  // 命中缓存
    dl3.push_cmd(make_text_cmd(area, "B", font, dl3));   // 新字形（同管线同裁剪 → 同批）
    dl3.push_cmd(make_fill(area, Color{0, 0, 0, 255}));  // Solid 管线断批
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 2U);
    AURORA_TEST_CHECK_EQ(fake.sub_uploads.size(), 3U);

    // 纯空格文本：字形位图为空 → 无槽位无绘制无上传（发射核心照常推进 pen）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl4;
    dl4.push_cmd(make_text_cmd(area, " ", font, dl4));
    dl4.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.sub_uploads.size(), 3U);

    // 空字符串 / 零 alpha：跳过不绘制（软件同契约），无上传。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl5;
    dl5.push_cmd(make_text_cmd(area, "", font, dl5));
    DrawCmd zero_a = make_text_cmd(area, "A", font, dl5);
    zero_a.color = Color{0, 0, 0, 0};
    dl5.push_cmd(zero_a);
    dl5.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.sub_uploads.size(), 3U);
}

AURORA_TEST_CASE(gpu_gl_glyph_atlas_multipage_and_lru_eviction) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 4.0F, .y = 4.0F}, .size = Size{.width = 400.0F, .height = 32.0F}};
    const Font font;

    // 小页注入（默认 1024）：62 个字母数字字形远超 8 页 × 16² 容量，强制覆盖
    //「满页开新页 → 页数封顶 LRU 淘汰」全路径（含淘汰前 flush）。
    rhi_obj.set_glyph_page_size(16);
    const std::string alnum = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(512, 64, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_text_cmd(area, alnum, font, dl));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    const std::size_t first_uploads = fake.sub_uploads.size();
    AURORA_TEST_CHECK_TRUE(first_uploads > 0U);   // 每个非空字形一次放置上传
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());      // 翻页/淘汰链不判死后端
    AURORA_TEST_CHECK_TRUE(rhi_obj.stats().draw_calls > 0U);

    // 第二帧同文本：部分槽位已被 LRU 淘汰 → 重新放置上传（> 0）；后端仍可用。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(512, 64, 1.0F));
    DisplayList dl2;
    dl2.push_cmd(make_text_cmd(area, alnum, font, dl2));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_TRUE(fake.sub_uploads.size() > first_uploads);  // 淘汰路径实际触发
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    AURORA_TEST_CHECK_TRUE(rhi_obj.stats().draw_calls > 0U);
}

AURORA_TEST_CASE(gpu_gl_shadow_pipeline) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 10.0F, .y = 10.0F}, .size = Size{.width = 100.0F, .height = 50.0F}};

    // 模糊阴影：单个 Shadow 批覆盖扩展区（内部因子 1 = fill，外部距离线性衰减同软件）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_shadow_cmd(area, 2.0F, 3.0F, 5.0F, Color{0, 0, 0, 128}));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    const auto stats = rhi_obj.stats();
    AURORA_TEST_CHECK_EQ(stats.draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(stats.skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.draw_calls, 1U);
    // quad 覆盖扩展区（expand = blur×2）：偏移后矩形 (12,13)-(112,63) 外扩 10 → (2,3)-(122,73)。
    AURORA_TEST_REQUIRE(fake.last_vbo_data.size() == 4U * 20U);
    float x = 0.0F;
    float y = 0.0F;
    std::memcpy(&x, fake.last_vbo_data.data(), sizeof(float));
    std::memcpy(&y, fake.last_vbo_data.data() + 4U, sizeof(float));
    AURORA_TEST_CHECK_TRUE(std::fabs(x - 2.0F) < 1e-5F);
    AURORA_TEST_CHECK_TRUE(std::fabs(y - 3.0F) < 1e-5F);
    std::memcpy(&x, fake.last_vbo_data.data() + 20U, sizeof(float));
    AURORA_TEST_CHECK_TRUE(std::fabs(x - 122.0F) < 1e-5F);

    // 硬阴影（blur ≤ 0）退化实心管线，与模糊阴影批互斥（Solid vs Shadow 断批）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl2;
    dl2.push_cmd(make_shadow_cmd(area, 2.0F, 3.0F, 0.0F, Color{0, 0, 0, 255}));
    dl2.push_cmd(make_shadow_cmd(area, 2.0F, 3.0F, 4.0F, Color{0, 0, 0, 128}));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 2U);
}

AURORA_TEST_CASE(gpu_gl_blur_region_pingpong) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 20.0F, .y = 20.0F}, .size = Size{.width = 60.0F, .height = 40.0F}};

    // 实心打底 + 模糊：批 flush 后前置 resolve 一次 → 水平(temp) → 垂直(msaa) 双 pass。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    AURORA_TEST_REQUIRE(fake.gen_fbo_ids.size() == 3U);  // msaa / resolve / temp（首次 begin 惰性发放）
    const auto msaa_fbo = fake.gen_fbo_ids[0];
    const auto temp_fbo = fake.gen_fbo_ids[2];
    DisplayList dl;
    dl.push_cmd(make_fill(area, Color{255, 255, 255, 255}));
    dl.push_cmd(make_blur_cmd(area, 2.0F));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 3U);  // 1 实心批 + 2 效果 pass
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    // blit = 效果前置 resolve 1 + 上屏 1（end_frame MSAA 直 blit，中间 resolve 按需懒做）。
    AURORA_TEST_CHECK_EQ(fake.blits, 2);
    AURORA_TEST_REQUIRE(fake.draw_fbo_targets.size() == 3U);
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[0], msaa_fbo);
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[1], temp_fbo);
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[2], msaa_fbo);
    // 效果 quad 覆盖换算后的区域矩形（逻辑 dp：20..80 × 20..60）。
    AURORA_TEST_REQUIRE(fake.last_vbo_data.size() == 4U * 20U);
    float x = 0.0F;
    float y = 0.0F;
    std::memcpy(&x, fake.last_vbo_data.data(), sizeof(float));
    std::memcpy(&y, fake.last_vbo_data.data() + 4U, sizeof(float));
    AURORA_TEST_CHECK_TRUE(std::fabs(x - 20.0F) < 1e-5F);
    AURORA_TEST_CHECK_TRUE(std::fabs(y - 20.0F) < 1e-5F);
    std::memcpy(&x, fake.last_vbo_data.data() + 20U, sizeof(float));
    std::memcpy(&y, fake.last_vbo_data.data() + 24U, sizeof(float));
    AURORA_TEST_CHECK_TRUE(std::fabs(x - 80.0F) < 1e-5F);
    AURORA_TEST_CHECK_TRUE(std::fabs(y - 20.0F) < 1e-5F);

    // 第二帧直接模糊：begin 清屏使上一帧 resolve 作废（脏标记）→ 前置 resolve 仍执行。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl2;
    dl2.push_cmd(make_blur_cmd(area, 2.0F));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 2U);
    AURORA_TEST_CHECK_EQ(fake.blits, 4);  // 两帧各 2 次（脏跟踪不吞必要 resolve）

    // radius ≤ 0：软件契约直接返回，无 pass（本帧仅上屏直 blit 1 次）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl3;
    dl3.push_cmd(make_blur_cmd(area, 0.0F));
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.blits, 5);

    // 区域完全在画布外：空区域跳过（软件同形钳制后为空）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl4;
    const Rect outside{.origin = Point{.x = 500.0F, .y = 20.0F}, .size = Size{.width = 30.0F, .height = 30.0F}};
    dl4.push_cmd(make_blur_cmd(outside, 2.0F));
    dl4.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.blits, 6);
}

AURORA_TEST_CASE(gpu_gl_blend_mask_region_pass) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 10.0F, .y = 10.0F}, .size = Size{.width = 80.0F, .height = 50.0F}};

    // Blend（Multiply 强度 0.6）：单 pass 直写 MSAA（批 flush + 前置 resolve + 效果 quad）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    AURORA_TEST_REQUIRE(fake.gen_fbo_ids.size() == 3U);  // msaa / resolve / temp（首次 begin 惰性发放）
    const auto msaa_fbo = fake.gen_fbo_ids[0];
    DisplayList dl;
    dl.push_cmd(make_fill(area, Color{200, 100, 50, 255}));
    dl.push_cmd(make_blend_cmd(area, BlendMode::Multiply, Color{128, 200, 255, 255}, 0.6F));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 2U);
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.blits, 2);  // 前置 resolve 1 + 上屏 1
    AURORA_TEST_REQUIRE(fake.draw_fbo_targets.size() == 2U);
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[0], msaa_fbo);
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[1], msaa_fbo);

    // Mask（RadialFade 强度 1）：同形单 pass；与 Blend 互为独立命令（跨帧正常采样）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl2;
    dl2.push_cmd(make_mask_cmd(area, ShaderMaskKind::RadialFade, 1.0F));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(fake.blits, 4);

    // strength ≤ 0（blend/mask 软件契约直接返回）与半径 0：全部跳过。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl3;
    dl3.push_cmd(make_blend_cmd(area, BlendMode::Normal, Color{0, 0, 0, 255}, 0.0F));
    dl3.push_cmd(make_mask_cmd(area, ShaderMaskKind::LinearFade, -0.5F));
    dl3.push_cmd(make_blur_cmd(area, 0.0F));
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.blits, 5);  // 本帧仅上屏直 blit 1 次
}

AURORA_TEST_CASE(gpu_gl_composite_transform_quad) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;

    Image img;
    img.width = 40;
    img.height = 20;
    img.pixels.assign(static_cast<std::size_t>(40) * static_cast<std::size_t>(20) * 4U, 128);

    // 平移矩阵 + 源 scale 2：源逻辑尺寸 20×10，四角 = 平移后角点，uv 全图 0..1（NEAREST）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_composite_cmd(img, Matrix2D::from_translate(5.0F, 7.0F), 2.0F, dl));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 1U);  // 内容键缓存首次上传
    // 顶点字节验证：20 字节/顶点（pos2f + uv2f + color4ub），四角顺序 (0,0)(w,0)(w,h)(0,h)。
    AURORA_TEST_REQUIRE(fake.last_vbo_data.size() == 4U * 20U);
    const float expect_x[4] = {5.0F, 25.0F, 25.0F, 5.0F};
    const float expect_y[4] = {7.0F, 7.0F, 17.0F, 17.0F};
    const float expect_u[4] = {0.0F, 1.0F, 1.0F, 0.0F};
    const float expect_v[4] = {0.0F, 0.0F, 1.0F, 1.0F};
    for (std::size_t i = 0; i < 4U; ++i) {
        const std::size_t off = i * 20U;
        float x = 0.0F;
        float y = 0.0F;
        float u = 0.0F;
        float v = 0.0F;
        std::memcpy(&x, fake.last_vbo_data.data() + off, sizeof(float));
        std::memcpy(&y, fake.last_vbo_data.data() + off + 4U, sizeof(float));
        std::memcpy(&u, fake.last_vbo_data.data() + off + 8U, sizeof(float));
        std::memcpy(&v, fake.last_vbo_data.data() + off + 12U, sizeof(float));
        AURORA_TEST_CHECK_TRUE(std::fabs(x - expect_x[i]) < 1e-5F);
        AURORA_TEST_CHECK_TRUE(std::fabs(y - expect_y[i]) < 1e-5F);
        AURORA_TEST_CHECK_TRUE(std::fabs(u - expect_u[i]) < 1e-5F);
        AURORA_TEST_CHECK_TRUE(std::fabs(v - expect_v[i]) < 1e-5F);
        AURORA_TEST_CHECK_EQ(fake.last_vbo_data[off + 19U], 255U);  // 全局 alpha 1 → a=255
    }
    // Composite 走 NEAREST（逐像素 floor 取样同软件）；DrawImage 走 LINEAR（双线性）。
    AURORA_TEST_REQUIRE(!fake.tex_min_filters.empty());
    AURORA_TEST_CHECK_EQ(static_cast<std::uint32_t>(fake.tex_min_filters.back()), AURORA_GL_FILTER_NEAREST);

    // DrawImage 对照：同纹理管线双批（LINEAR vs NEAREST 采样模式断批）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl2;
    const Rect area{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 20.0F}};
    dl2.push_cmd(make_composite_cmd(img, Matrix2D::from_translate(1.0F, 1.0F), 2.0F, dl2));
    dl2.push_cmd(make_image_cmd(area, img, dl2));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 2U);
    AURORA_TEST_REQUIRE(!fake.tex_min_filters.empty());
    AURORA_TEST_CHECK_EQ(static_cast<std::uint32_t>(fake.tex_min_filters.back()), AURORA_GL_FILTER_LINEAR);

    // 非法图像（空像素 / 缓冲不足）：跳过不绘制（软件 composite_pixels 前置校验同形）。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(200, 150, 1.0F));
    DisplayList dl3;
    Image bad_empty;
    Image bad_short;
    bad_short.width = 4;
    bad_short.height = 4;
    bad_short.pixels = {1, 2, 3, 4};
    dl3.push_cmd(make_composite_cmd(bad_empty, Matrix2D{}, 1.0F, dl3));
    dl3.push_cmd(make_composite_cmd(bad_short, Matrix2D{}, 1.0F, dl3));
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 1U);
}

AURORA_TEST_CASE(gpu_gl_capabilities_bits) {
    // 未装载：能力位全 false（与软件回退语义一致）；流式契约整体拒绝。
    rhi::GpuGlRhi not_loaded;
    const auto cap_bad = not_loaded.capabilities();
    AURORA_TEST_CHECK_FALSE(cap_bad.gpu);
    AURORA_TEST_CHECK_FALSE(cap_bad.native_surface_import);
    AURORA_TEST_CHECK_FALSE(cap_bad.compute);
    AURORA_TEST_CHECK_EQ(not_loaded.acquire_stream_image(1, 8, 8), 0U);
    AURORA_TEST_CHECK_NO_THROW(not_loaded.update_stream_image(1, nullptr, 0, 0, 0, 1, 1));
    AURORA_TEST_CHECK_NO_THROW(not_loaded.release_stream_image(1));
    AURORA_TEST_CHECK_EQ(not_loaded.import_native_surface(aurora::NativeSurfaceFrame{}), 0U);

    // GL 3.3 core：gpu=true；原生表面导入不实现（恒 false）；无 compute。
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    const auto cap = rhi_obj.capabilities();
    AURORA_TEST_CHECK_TRUE(cap.gpu);
    AURORA_TEST_CHECK_FALSE(cap.native_surface_import);
    AURORA_TEST_CHECK_FALSE(cap.compute);
}

AURORA_TEST_CASE(gpu_gl_stream_texture_slot_version_gating) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    const Rect area{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 32.0F, .height = 16.0F}};

    // 流式图像：直色像素（未预乘）+ 流式键/版本标识。
    Image img;
    img.width = 2;
    img.height = 1;
    img.pixels = {200, 100, 50, 255, 200, 100, 50, 128};
    img.stream_key = 42;
    img.stream_version = 1;

    // 首帧：槽纹理创建走空数据 tex_image_2d（不入 uploads），像素经 RGBA 直色子上传。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 16, 1.0F));
    DisplayList dl;
    dl.push_cmd(make_image_cmd(area, img, dl));
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 0U);
    AURORA_TEST_REQUIRE(fake.rgba_sub_uploads.size() == 1U);
    const auto &up = fake.rgba_sub_uploads[0];
    AURORA_TEST_CHECK_EQ(up.x, 0);
    AURORA_TEST_CHECK_EQ(up.y, 0);
    AURORA_TEST_CHECK_EQ(up.width, 2);
    AURORA_TEST_CHECK_EQ(up.height, 1);
    AURORA_TEST_CHECK_EQ(up.data.size(), static_cast<std::size_t>(2) * 4U);
    AURORA_TEST_CHECK_EQ(up.data[0], 200);  // 直色原样上传（无 CPU 预乘；PMA 下沉片元）
    AURORA_TEST_CHECK_EQ(up.data[1], 100);
    AURORA_TEST_CHECK_EQ(up.data[2], 50);
    AURORA_TEST_CHECK_EQ(up.data[4], 200);
    AURORA_TEST_CHECK_EQ(up.data[5], 100);
    AURORA_TEST_CHECK_EQ(up.data[6], 50);
    AURORA_TEST_CHECK_EQ(up.data[7], 128);

    // 版本未变：零上传（版本门控），仅合成。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 16, 1.0F));
    DisplayList dl2;
    dl2.push_cmd(make_image_cmd(area, img, dl2));
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 1U);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.size(), 1U);

    // 版本推进：一次全帧子上传，内容为新像素。
    Image v2 = img;
    v2.stream_version = 2;
    v2.pixels = {10, 20, 30, 255, 40, 50, 60, 200};
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 16, 1.0F));
    DisplayList dl3;
    dl3.push_cmd(make_image_cmd(area, v2, dl3));
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.size(), 2U);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.back().data[0], 10);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.back().data[6], 60);

    // 尺寸变化：就地重定义存储（空数据重定义不入 uploads）+ 一次新帧上传；全程零全量上传。
    Image v3 = v2;
    v3.stream_version = 3;
    v3.width = 4;
    v3.pixels = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 16, 1.0F));
    DisplayList dl4;
    dl4.push_cmd(make_image_cmd(area, v3, dl4));
    dl4.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.size(), 3U);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.back().width, 4);
    AURORA_TEST_CHECK_EQ(fake.uploads.size(), 0U);
}

AURORA_TEST_CASE(gpu_gl_stream_public_api_contract) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(32, 32, 1.0F));

    // 取槽：非零句柄；同键复用同槽。
    const auto id = rhi_obj.acquire_stream_image(7, 8, 4);
    AURORA_TEST_CHECK_TRUE(id != 0);
    AURORA_TEST_CHECK_EQ(rhi_obj.acquire_stream_image(7, 8, 4), id);

    // 紧凑行增量更新：内容原样进入 RGBA 子上传。
    std::vector<std::uint8_t> px(static_cast<std::size_t>(8) * 4U * 4U);
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<std::uint8_t>(i & 0xFFU);
    }
    rhi_obj.update_stream_image(id, px.data(), 0, 0, 0, 8, 4);
    AURORA_TEST_REQUIRE(fake.rgba_sub_uploads.size() == 1U);
    AURORA_TEST_CHECK_TRUE(fake.rgba_sub_uploads.back().data == px);

    // 跨距行（UNPACK_ROW_LENGTH 路径）：仅记录一次。
    const std::size_t stride = static_cast<std::size_t>(8) * 4U + 8U;
    const std::vector<std::uint8_t> strided(stride * 4U, 0x5A);
    rhi_obj.update_stream_image(id, strided.data(), stride, 0, 0, 8, 4);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.size(), 2U);

    // 契约防御：id=0 / 空指针 / 零尺寸 / 越界脏矩形 → no-op（不判死后端）。
    rhi_obj.update_stream_image(0, px.data(), 0, 0, 0, 8, 4);
    rhi_obj.update_stream_image(id, nullptr, 0, 0, 0, 8, 4);
    rhi_obj.update_stream_image(id, px.data(), 0, 0, 0, 0, 0);
    rhi_obj.update_stream_image(id, px.data(), 0, 0, 0, 99, 4);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.size(), 2U);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());

    // 释放后更新为 no-op；重复释放无害；同键重新取槽得新句柄。
    rhi_obj.release_stream_image(id);
    rhi_obj.release_stream_image(id);
    rhi_obj.update_stream_image(id, px.data(), 0, 0, 0, 8, 4);
    AURORA_TEST_CHECK_EQ(fake.rgba_sub_uploads.size(), 2U);
    const auto id2 = rhi_obj.acquire_stream_image(7, 8, 4);
    AURORA_TEST_CHECK_TRUE(id2 != 0);
    AURORA_TEST_CHECK_TRUE(id2 != id);

    // 原生表面导入：恒不支持（返回 0，调用方回退 CPU 路径）。
    const aurora::NativeSurfaceFrame frame;
    AURORA_TEST_CHECK_EQ(rhi_obj.import_native_surface(frame), 0U);
}

AURORA_TEST_CASE(gpu_gl_layer_cache_lifecycle_and_miss_epoch) {
    FakeGl fake;
    rhi::GpuGlRhi rhi_obj(fake.fn);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
    rhi::RhiFrameSink &sink = rhi_obj;
    constexpr std::uint64_t KEY = 7;

    // 失效帧命令形态：BeginLayer（建常驻层 FBO）→ 子树重定向层 FBO → EndLayer → DrawLayer 回 MSAA。
    auto make_layer_dl = [&KEY](DisplayList &dl) {
        DrawCmd begin;
        begin.kind = CmdKind::BeginLayer;
        begin.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}};
        begin.aux_key = KEY;
        dl.push_cmd(begin);
        dl.push_cmd(make_fill(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 64.0F, .height = 48.0F}},
                              Color{255, 0, 0, 255}));
        DrawCmd end;
        end.kind = CmdKind::EndLayer;
        dl.push_cmd(end);
        DrawCmd draw_layer;
        draw_layer.kind = CmdKind::DrawLayer;
        draw_layer.aux_key = KEY;
        draw_layer.matrix_idx = dl.add_matrix(Matrix2D{});
        draw_layer.composite_scale = 1.0F;
        dl.push_cmd(draw_layer);
    };

    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    AURORA_TEST_REQUIRE(fake.gen_fbo_ids.size() == 3U);  // msaa / resolve / temp
    DisplayList dl;
    make_layer_dl(dl);
    dl.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 2U);  // 层内 fill + DrawLayer 合成
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().skipped_cmds, 0U);
    AURORA_TEST_CHECK_EQ(fake.gen_fbo_ids.size(), 4U);     // 新建层 FBO（常驻）
    const auto layer_fbo = fake.gen_fbo_ids.back();
    AURORA_TEST_REQUIRE(fake.draw_fbo_targets.size() == 2U);
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[0], layer_fbo);             // 子树重定向层 FBO
    AURORA_TEST_CHECK_EQ(fake.draw_fbo_targets[1], fake.gen_fbo_ids[0]);   // 合成回 MSAA
    AURORA_TEST_CHECK_EQ(static_cast<std::uint32_t>(fake.tex_min_filters.back()), AURORA_GL_FILTER_NEAREST);

    // 同键重录：层 FBO 复用（零新建），后端不判死。
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl2;
    make_layer_dl(dl2);
    dl2.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(fake.gen_fbo_ids.size(), 4U);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());

    // DrawLayer 未命中：跳过不绘制 + bump 层代际（单帧自愈信号），后端不判死。
    const auto epoch_before = render::detail::gpu_layer_epoch();
    AURORA_TEST_CHECK_TRUE(rhi_obj.begin_frame(64, 48, 1.0F));
    DisplayList dl3;
    DrawCmd miss;
    miss.kind = CmdKind::DrawLayer;
    miss.aux_key = 999;
    miss.matrix_idx = dl3.add_matrix(Matrix2D{});
    miss.composite_scale = 1.0F;
    dl3.push_cmd(miss);
    dl3.replay(sink.backend());
    rhi_obj.end_frame();
    AURORA_TEST_CHECK_EQ(rhi_obj.stats().draw_calls, 0U);
    AURORA_TEST_CHECK_EQ(render::detail::gpu_layer_epoch(), epoch_before + 1);
    AURORA_TEST_CHECK_TRUE(rhi_obj.valid());
}

}  // namespace aurora::test_cases::utest_gpu_gl_rhi
