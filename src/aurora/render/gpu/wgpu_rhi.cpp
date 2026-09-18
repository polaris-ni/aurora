#include "aurora/render/rhi/wgpu_rhi.h"

// 本 TU 整体裁切于 AURORA_BACKEND_GPU_WGPU（区别于恒编译的 gpu_gl_rhi.cpp）：实现链接
// third_party/wgpu-native 的 Rust 静态库，特性关闭时既无头也不产生符号（公共头同样裁切）。
//
// 全命令族（23 条 CmdKind）逐条移植 gpu_gl_rhi.cpp 的 GLSL/批切分/效果语义（同源参照，
// specification/03-layout-render.md §8.7）：九管线 + 层缓存 + 区域效果（两遍 A/B pass）
// + 常驻流式槽。WebGPU 读写危险约束驱动与 GL 的结构性差异：
//   • MSAA 纹理不可采样 → 画布效果经「关 pass（resolve 自动落 canvas 纹理）→ A pass 写
//     alt（读 canvas 纹理）→ B pass 回写画布」两遍，替代 GL 的手动 blit；
//   • 同一纹理不能既是被附着又是被采样 → 层内效果先把层纹 encoder 拷贝到 aux 再读；
//   • 纹理不可原地改尺寸 → 尺寸变化一律新建（旧对象经已录制 bind group 引用保活，
//     淘汰无需 flush，与 GL 的悬垂名问题不同源）。
// 同帧多批的 queue 写（writeBuffer/writeTexture）统一在下次 submit 前生效，批间无写入
// 顺序（与 GL 的「上传即落地」存在已接受偏差，见 run_effect_pair 注释）。

#ifdef AURORA_BACKEND_GPU_WGPU

#include <webgpu.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#include <windows.h>  // GetWindowLongPtrW / GetModuleHandle：HWND surface 的 HINSTANCE 推导
#undef DrawText       // wingdi.h 宏会吞掉 CmdKind::DrawText 枚举名
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/render/detail/gpu_layer.h"
#include "aurora/render/glyph_emit.h"

namespace aurora::rhi {

namespace {

// ---- WGSL 单一模块：全部管线共享（vs_main + 12 个入口片元着色器）----
// Globals 布局 11×vec4f（176B）：逐管线取用相关分量，未用分量恒 0；uniform 与
// bind group（binding 1..3 = 源纹理 + 两级采样器）跨管线统一，断批仅由 key 变化驱动。

constexpr const char *kWgsl = R"(
struct Globals {
    cv4: vec4f,        // 当前附着目标逻辑尺寸 (w, h, _, _)：NDC 映射基准
    clip: vec4f,       // 裁剪矩形 x,y,w,h（逻辑 dp）
    clip_ctl: vec4f,   // (radius, on, aa, _)
    shape: vec4f,      // Border/Shadow：SDF 盒 cx, cy, half_w, half_h（逻辑 dp）
    grad_ab: vec4f,    // 渐变：起点 a（linear 终点 b 或 radial 圆心占 xy）+ bx,by
    grad_cd: vec4f,    // 渐变：radial 半径、radial 标志（0/1）
    ctl: vec4f,        // border: (radius, thickness)；shadow: blur@z；blend: mode@w；mask: kind@z
    fx: vec4f,         // blend tint.rgb / mask+blend strength@a
    region: vec4f,     // 效果区域（设备像素）ox, oy, w, h
    canvas_ctl: vec4f, // 效果源纹理设备尺寸 w, h（uv 归一化基准）
    tex_ctl: vec4f,    // image: (片元内 PMA, NEAREST 采样)；blur: (0,0, radius, dir)
};
@group(0) @binding(0) var<uniform> g: Globals;
@group(0) @binding(1) var t_src: texture_2d<f32>;
@group(0) @binding(2) var smp_point: sampler;
@group(0) @binding(3) var smp_linear: sampler;

struct VSOut {
    @builtin(position) fb: vec4f,
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

@vertex
fn vs_main(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f, @location(2) a_color: vec4f) -> VSOut {
    var o: VSOut;
    o.pos = a_pos;
    o.uv = a_uv;
    o.color = a_color;
    let ndc = vec2f(a_pos.x / g.cv4.x * 2.0 - 1.0, 1.0 - a_pos.y / g.cv4.y * 2.0);
    o.fb = vec4f(ndc, 0.0, 1.0);
    return o;
}

// present：a_pos 已是裁剪坐标 NDC（全屏三角/四边形），uv 自上而下映射画布纹理。
@vertex
fn vs_present(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f, @location(2) a_color: vec4f) -> VSOut {
    var o: VSOut;
    o.pos = a_pos;
    o.uv = a_uv;
    o.color = a_color;
    o.fb = vec4f(a_pos, 0.0, 1.0);
    return o;
}

fn sd_rbox(p: vec2f, b: vec2f, r: f32) -> f32 {
    let q: vec2f = abs(p) - b + vec2f(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2f(0.0))) - r;
}

// shader 内 SDF 裁剪（不用 scissor，与软件路径逐像素交叠语义同源）；不 discard——
// alpha=0 经 blend 写 dst=dst。WebGPU 帧缓冲行 0 在顶部：y 翻转后逻辑 y=0 即首行。
fn clip_cov(p: vec2f) -> f32 {
    if (g.clip_ctl.y < 0.5) {
        return 1.0;
    }
    let ctr = g.clip.xy + g.clip.zw * 0.5;
    let d = sd_rbox(p - ctr, g.clip.zw * 0.5, g.clip_ctl.x);
    return select(step(d, 0.0), 1.0 - smoothstep(-0.5, 0.5, d), g.clip_ctl.z > 0.5);
}

@fragment
fn fs_solid(in: VSOut) -> @location(0) vec4f {
    return vec4f(in.color.rgb, in.color.a * clip_cov(in.pos));
}

// 圆角描边带：外缘 d=0、内缘 d=-thickness、两侧 0.5px 羽化（同 GL border 公式）。
@fragment
fn fs_border(in: VSOut) -> @location(0) vec4f {
    let d = sd_rbox(in.pos - g.shape.xy, g.shape.zw, g.ctl.x);
    let t = g.ctl.y;
    let band = smoothstep(-t - 0.5, -t + 0.5, d) * (1.0 - smoothstep(-0.5, 0.5, d));
    return vec4f(in.color.rgb, in.color.a * clip_cov(in.pos) * band);
}

// 渐变：1D LUT（256×1 RGBA8）采样；t 片元内计算（linear 退化回落首色，同软件
// sample_gradient front 分支）；半像素对齐 (t*255+0.5)/256 保证端点精确落 texel。
@fragment
fn fs_grad(in: VSOut) -> @location(0) vec4f {
    var t = 0.0;
    if (g.grad_cd.y > 0.5) {
        t = length(in.pos - g.grad_ab.xy) / g.grad_cd.x;
    } else {
        let d = g.grad_ab.zw - g.grad_ab.xy;
        let dd = dot(d, d);
        t = select(dot(in.pos - g.grad_ab.xy, d) / max(dd, 1e-12), 0.0, dd < 1e-6);
    }
    t = clamp(t, 0.0, 1.0);
    let lg = textureSampleLevel(t_src, smp_linear, vec2f((t * 255.0 + 0.5) / 256.0, 0.5), 0.0);
    return vec4f(lg.rgb, lg.a * in.color.a * clip_cov(in.pos));
}

// 图像：LINEAR/NEAREST 随批切换（DrawImage 双线性 vs 层合成/Composite 逐像素 floor）；
// u_pma=1 直色纹理片元内一乘 PMA（常驻流式通道免 CPU 预乘副本）。输出整体缩放
// （rgb 与 a 同乘 alpha×裁剪 coverage），混合走 ONE/ONE_MINUS_SRC_ALPHA。
@fragment
fn fs_image(in: VSOut) -> @location(0) vec4f {
    var tex = textureSampleLevel(t_src, smp_linear, in.uv, 0.0);
    if (g.tex_ctl.y > 0.5) {
        tex = textureSampleLevel(t_src, smp_point, in.uv, 0.0);
    }
    if (g.tex_ctl.x > 0.5) {
        tex = vec4f(tex.rgb * tex.a, tex.a);
    }
    return tex * (in.color.a * clip_cov(in.pos));
}

// 文本：R8 字形图集 coverage 进 alpha（颜色不变 × 全局 alpha × 裁剪），NEAREST 采样
// 逐位精确（字形物理像素 1:1 对齐）。LCD 子像素降级灰度（GPU v1 容差决策，同 GL）。
@fragment
fn fs_text(in: VSOut) -> @location(0) vec4f {
    let cov = textureSampleLevel(t_src, smp_point, in.uv, 0.0).r;
    return vec4f(in.color.rgb, in.color.a * cov * clip_cov(in.pos));
}

// 阴影：矩形外欧氏距离线性衰减（内部 dist=0 → 因子 1 = fill），blur 逻辑 dp 直算。
@fragment
fn fs_shadow(in: VSOut) -> @location(0) vec4f {
    let q = abs(in.pos - g.shape.xy) - g.shape.zw;
    let dist = length(max(q, vec2f(0.0)));
    let atten = max(0.0, 1.0 - dist / max(g.ctl.z, 1e-6));
    return vec4f(in.color.rgb, in.color.a * atten * clip_cov(in.pos));
}

// 效果族共用直写拷贝：present（surface 上屏）与画布效果 B pass（alt → 画布区域回写）。
@fragment
fn fs_copy(in: VSOut) -> @location(0) vec4f {
    return textureSampleLevel(t_src, smp_point, in.uv, 0.0);
}

// 区域模糊单遍：整数域恒权 box（tap 钳制在区域内，毛玻璃不漏采区外），设备像素索引
// 域 = 片元 framebuffer 坐标（wgpu fb.xy 已是附着设备像素，免 GL 的 v_pos*scale 换算）。
@fragment
fn fs_blur(in: VSOut) -> @location(0) vec4f {
    let ipx = floor(in.fb.xy) - g.region.xy;
    let r = i32(g.tex_ctl.z);
    let diry = g.tex_ctl.w > 0.5;
    var acc = vec4f(0.0);
    for (var k: i32 = -r; k <= r; k = k + 1) {
        var t = clamp(ipx, vec2f(0.0), g.region.zw - vec2f(1.0));
        if (diry) {
            t.y = clamp(ipx.y + f32(k), 0.0, g.region.w - 1.0);
        } else {
            t.x = clamp(ipx.x + f32(k), 0.0, g.region.z - 1.0);
        }
        let uv = (g.region.xy + t + vec2f(0.5)) / g.canvas_ctl.xy;
        acc = acc + floor(textureSampleLevel(t_src, smp_point, uv, 0.0) * 255.0 + 0.5);
    }
    return floor(acc / f32(2 * r + 1)) / 255.0;
}

// 区域混合（CSS mix-blend-mode 子集）：浮点域近似软件整数运算，逐通道 ≤ 1 LSB 容差；
// alpha 通道不参与（软件只写 RGB）。
@fragment
fn fs_blend(in: VSOut) -> @location(0) vec4f {
    let s4 = floor(textureSampleLevel(t_src, smp_linear, in.uv, 0.0) * 255.0 + 0.5);
    let s = s4.rgb / 255.0;
    let t = g.fx.rgb;
    var r = s;
    let mode = i32(g.ctl.w);
    if (mode == 0) {
        r = t;
    } else if (mode == 1) {
        r = s * t;
    } else if (mode == 2) {
        r = vec3f(1.0) - (vec3f(1.0) - s) * (vec3f(1.0) - t);
    } else if (mode == 3) {
        r = mix(2.0 * s * t, vec3f(1.0) - 2.0 * (vec3f(1.0) - s) * (vec3f(1.0) - t), step(vec3f(0.5), s));
    } else if (mode == 4) {
        r = min(s, t);
    } else if (mode == 5) {
        r = max(s, t);
    } else if (mode == 6) {
        r = abs(s - t);
    } else if (mode == 7) {
        r = s + t - 2.0 * s * t;
    }
    let outc = clamp(s + g.fx.a * (r - s), vec3f(0.0), vec3f(1.0));
    return vec4f(outc, s4.a / 255.0);
}

// 区域遮罩：RGB 乘渐变因子（LinearFade/LinearRise/RadialFade，基于区域设备像素索引），
// alpha 原样保留（同 GL 公式）。
@fragment
fn fs_mask(in: VSOut) -> @location(0) vec4f {
    let ipx = floor(in.fb.xy) - g.region.xy;
    var base = 1.0;
    let kind = i32(g.ctl.z);
    if (kind == 0) {
        base = 1.0 - ipx.y / g.region.w;
    } else if (kind == 1) {
        base = ipx.y / g.region.w;
    } else if (kind == 2) {
        let c = g.region.zw * 0.5;
        base = 1.0 - length(ipx - c) / (length(c) + 0.001);
    }
    base = clamp(base, 0.0, 1.0);
    let factor = 1.0 - g.fx.a * (1.0 - base);
    let s4 = floor(textureSampleLevel(t_src, smp_linear, in.uv, 0.0) * 255.0 + 0.5);
    let rgb = clamp(floor(s4.rgb / 255.0 * factor * 255.0), vec3f(0.0), vec3f(255.0));
    return vec4f(rgb / 255.0, s4.a / 255.0);
}
)";

// 顶点布局与 GL 路径同构：20 字节（pos2f + uv2f + color4ub 归一化）。
struct Vertex {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};
static_assert(sizeof(Vertex) == 20, "vertex layout must stay packed at 20 bytes");

// uniform 块（与 WGSL Globals 逐 vec4f 对齐，176 字节）。
struct Globals {
    float cv4[4] = {};
    float clip[4] = {};
    float clip_ctl[4] = {};
    float shape[4] = {};
    float grad_ab[4] = {};
    float grad_cd[4] = {};
    float ctl[4] = {};
    float fx[4] = {};
    float region[4] = {};
    float canvas_ctl[4] = {};
    float tex_ctl[4] = {};
};
static_assert(sizeof(Globals) == 176, "uniform block must stay 176 bytes");

// ---- 管线目录：13 条（含 solid 无混合变体与 image PMA/直色变体）× 2 采样数集 ----
// 混合语义：std = 直色 src-over（color SRC_ALPHA/OMSA + alpha ONE/OMSA）；pma = 全量
// ONE/OMSA（PMA 内容）；none = 直写替换（ClearRect 与效果回写族）。

enum PipeId : int {
    kPipeSolid = 0,
    kPipeSolidNo,    ///< ClearRect：无混合直写零
    kPipeBorder,
    kPipeGrad,
    kPipeImagePma,   ///< DrawImage / Composite（PMA 混合）
    kPipeImageSrc,   ///< DrawLayer（层内容直色 src-over）
    kPipeText,
    kPipeShadow,
    kPipeBlur,
    kPipeBlend,
    kPipeMask,
    kPipeCopy,       ///< 效果 B pass 区域回写 + present 上屏
    kPipeCount
};

struct PipeSpec {
    const char *fs;  ///< 片元入口名
    int blend;       ///< 0 = 无混合，1 = 直色 src-over，2 = PMA
};

// 批 key 基管线（GL 路径 Pipeline 同构；Solid 无混合 / Image PMA 变体在 flush 期解析）。
enum BasePipe : int { kBaseSolid = 0, kBaseBorder = 1, kBaseGrad = 2, kBaseImage = 3, kBaseText = 4, kBaseShadow = 5 };

constexpr PipeSpec kPipeSpecs[kPipeCount] = {
    PipeSpec{"fs_solid", 1}, PipeSpec{"fs_solid", 0},   PipeSpec{"fs_border", 1}, PipeSpec{"fs_grad", 1},
    PipeSpec{"fs_image", 2}, PipeSpec{"fs_image", 1},   PipeSpec{"fs_text", 1},   PipeSpec{"fs_shadow", 1},
    PipeSpec{"fs_blur", 0},  PipeSpec{"fs_blend", 0},   PipeSpec{"fs_mask", 0},   PipeSpec{"fs_copy", 0},
};

constexpr std::uint64_t kUniformAlign = 256;  // uniform buffer offset 对齐下限
constexpr std::uint32_t kMsaaSamples = 4;

// ---- 缓存容量（与 GL 路径同参）----
constexpr int kLutWidth = 256;
constexpr std::size_t kLutCacheCap = 64;
constexpr std::size_t kImageCacheCap = 64;
constexpr int kGlyphPage = 1024;
constexpr int kGlyphPageMax = 2048;
constexpr std::size_t kGlyphPageCap = 8;

// 软件端 sample_gradient（painter.cpp）的逐位镜像，仅供 LUT 生成期采样（同 GL 路径）：
// texel j 以 t = j/255 取值——含 t<=0 取首色、t>=1 取尾色、无区间命中回落尾色的全部行为。
auto sample_gradient_lut(const std::vector<Color> &colors, const std::vector<float> &stops, float t) -> Color {
    if (colors.empty()) {
        return Color{};
    }
    if (colors.size() == 1 || t <= 0.0F) {
        return colors.front();
    }
    if (t >= 1.0F) {
        return colors.back();
    }
    const std::size_t n = std::min(colors.size(), stops.size());
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (t >= stops[i] && t <= stops[i + 1]) {
            const float range = stops[i + 1] - stops[i];
            const float frac = (range > 0.0F) ? (t - stops[i]) / range : 0.0F;
            const Color &a = colors[i];
            const Color &b = colors[i + 1];
            return Color{
                static_cast<std::uint8_t>(a.r + ((b.r - a.r) * frac)),
                static_cast<std::uint8_t>(a.g + ((b.g - a.g) * frac)),
                static_cast<std::uint8_t>(a.b + ((b.b - a.b) * frac)),
                static_cast<std::uint8_t>(a.a + ((b.a - a.a) * frac)),
            };
        }
    }
    return colors.back();
}

auto sv(const char *s) -> WGPUStringView {
    return WGPUStringView{.data = s, .length = std::strlen(s)};
}

auto sv_view(const WGPUStringView &v) -> std::string {
    if (v.data == nullptr) {
        return {};
    }
    const std::size_t len = v.length == WGPU_STRLEN ? std::strlen(v.data) : v.length;
    return std::string(v.data, len);
}

auto hex_u32(std::uint32_t v) -> std::string {
    const char *digits = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = digits[v & 0xFU];
        v >>= 4U;
    }
    return s;
}

// 类型化 Release（webgpu.h 无泛型 wgpuObjectRelease）：置空 + 调用对应 Release 函数。
#define AURORA_WGPU_RELEASE(obj, fn) \
    if ((obj) != nullptr) {          \
        (fn)(obj);                   \
        (obj) = nullptr;             \
    }

}  // namespace

// ============================================================
// Impl：全部 wgpu 资源与帧状态（pimpl，wgpu 头不外泄）
// ============================================================

struct WgpuRhi::Impl {
    // ---- 对象层级 ----
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    bool device_ok = false;
    bool compute_cap = false;  // capabilities().compute（GL/GLES 后端为 false）
    WGPUBackendType adapter_backend_ = WGPUBackendType_Undefined;

    WgpuRhiOptions options;

    // ---- surface（宿主模式）----
    WGPUSurface surface = nullptr;
    bool surface_configured = false;
    int surface_cw = 0;
    int surface_ch = 0;
    WGPUTextureFormat surface_format = WGPUTextureFormat_Undefined;
    WGPUPresentMode present_mode = WGPUPresentMode_Fifo;

    // ---- 帧目标：MSAA 附着 + resolve 画布纹理 + 效果中转 alt + 1×1 占位 ----
    struct Tex {
        WGPUTexture tex = nullptr;
        WGPUTextureView view = nullptr;
        int width = 0;
        int height = 0;
    };
    Tex canvas_{};   // RGBA8 resolve 目标（可采样 + 读回拷贝源）
    Tex msaa_{};     // 4x 渲染附着（不可采样，pass 关闭时自动 resolve）
    Tex alt_{};      // 画布区域效果 A pass 目标（≥ 画布尺寸，层效果可撑大）
    Tex dummy_{};    // 1×1 RGBA8 全零：批不采样时的 binding 1 占位
    int device_w = 0;
    int device_h = 0;

    // ---- 采样器（bind group 恒绑两枚；着色器按管线取用）----
    WGPUSampler samp_point_ = nullptr;
    WGPUSampler samp_linear_ = nullptr;

    // ---- 着色器 / 管线 ----
    WGPUShaderModule shader_ = nullptr;
    WGPUBindGroupLayout bgl_ = nullptr;
    WGPUPipelineLayout pipeline_layout_ = nullptr;
    WGPURenderPipeline pipes1_[kPipeCount] = {};  // 采样数 1：层 pass / alt pass / present
    WGPURenderPipeline pipes4_[kPipeCount] = {};  // 采样数 4：画布 MSAA pass
    WGPURenderPipeline pipe_present_ = nullptr;   // vs_present + fs_copy（surface 格式）
    WGPUTextureFormat present_format_ = WGPUTextureFormat_Undefined;

    // ---- 顶点/uniform 帧内环（CPU 暂存 + GPU 镜像；扩容时整体重放暂存，偏移稳定）----
    // 帧内各批共用一次 submit：同区域 writeBuffer 只保留最后一次写，故每批写独立区段。
    WGPUBuffer vertex_buf = nullptr;
    std::uint64_t vertex_cap = 0;
    std::vector<std::uint8_t> vstage;
    WGPUBuffer uniform_buf = nullptr;
    std::uint64_t uniform_cap = 0;
    std::vector<std::uint8_t> ustage;

    // ---- 帧状态 ----
    WGPUCommandEncoder encoder = nullptr;
    WGPURenderPassEncoder pass = nullptr;  // 当前打开的目标 pass（效果序列中可短暂为空）
    WGPUTextureView frame_view = nullptr;  // 本帧 swapchain view（持有引用，帧尾释放）
    // swapchain 纹理本体引用：⚠️ 必须活到 submit/present 之后（texture_arrays example 同序），
    // 提前释放会让 wgpu-core 在提交时判定「附着纹理已销毁」直接 Validation Error panic。
    WGPUTexture frame_tex = nullptr;
    enum PassKind : int { kPassNone = 0, kPassCanvas, kPassLayer, kPassAlt };
    PassKind pass_kind = kPassNone;
    bool frame_open = false;

    // 裁剪态（批 key 成员；变化即断批）
    struct ClipState {
        bool on = false;
        Rect rect{};
        float radius = 0.0F;
        bool aa = true;
        auto operator==(const ClipState &) const -> bool = default;
    };

    // 批 key：任一成员变化即 flush 当前批（对照 gpu_gl_rhi.cpp::BatchKey；GL 纹理名换
    // wgpu view 指针——view 即内容身份，同内容同 view 可合批）。
    struct BatchKey {
        int pipeline = kPipeSolid;  // 基管线（Solid/Border/Grad/Image/Text/Shadow）
        ClipState clip{};
        bool blend_off = false;   // Solid：ClearRect 无混合变体
        bool blend_pma = false;   // Image：PMA 混合变体
        // Border / Shadow 共用 shape 盒（逻辑 dp 中心 + 半宽半高）
        float shape_cx = 0.0F;
        float shape_cy = 0.0F;
        float shape_hw = 0.0F;
        float shape_hh = 0.0F;
        float border_radius = 0.0F;  // Border 专用
        float border_width = 0.0F;
        float shadow_blur = 0.0F;    // Shadow 专用
        // Grad 专用：渐变几何参数 + LUT view
        bool grad_radial = false;
        float grad_ax = 0.0F;
        float grad_ay = 0.0F;
        float grad_bx = 0.0F;
        float grad_by = 0.0F;
        float grad_r = 0.0F;
        // Image 专用
        bool pma_in_shader = false;  // 1 = 直色纹理片元内 PMA（常驻流式通道）
        bool nearest_filter = false;  // Composite/DrawLayer 逐像素 floor 取样
        // 源纹理 view（Grad LUT / Image / 字形页 / 层纹理；nullptr = dummy 占位）
        WGPUTextureView view = nullptr;
        auto operator==(const BatchKey &) const -> bool = default;
    };

    std::vector<ClipState> clip_stack;
    std::vector<Vertex> verts;
    BatchKey key{};
    bool key_active = false;
    double alpha = 1.0;
    float scale = 1.0F;  // 设备像素 / 逻辑 dp

    // ---- 渐变 LUT 缓存（键 = 色标数组内容精确比对；容量溢出整体清空）----
    struct LutEntry {
        std::vector<Color> colors;
        std::vector<float> stops;
        Tex tex{};
    };
    std::vector<LutEntry> lut_cache;

    // ---- 图像纹理缓存（键 = content_hash ^ 维度混列；PMA 上传）----
    struct ImageTexEntry {
        std::uint64_t hash = 0;
        int width = 0;
        int height = 0;
        Tex tex{};
    };
    std::vector<ImageTexEntry> image_cache;

    // ---- GPU 字形图集（多页 R8 架式打包 + LRU 页淘汰，同 GL 策略）----
    struct GlyphSlotRect {
        WGPUTextureView view = nullptr;  // 所在页纹理 view（跨页文本自然断批）
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        float u0 = 0.0F;
        float v0 = 0.0F;
        float u1 = 0.0F;
        float v1 = 0.0F;
    };
    struct GlyphPage {
        Tex tex{};
        int pack_x = 0;
        int pack_y = 0;
        int pack_row_h = 0;
        std::uint64_t lru = 0;
    };
    std::unordered_map<std::uint64_t, GlyphSlotRect> glyph_slots;
    std::vector<GlyphPage> glyph_pages;
    int active_glyph_page_ = -1;
    std::uint64_t glyph_lru_clock_ = 0;
    int glyph_page_size_ = kGlyphPage;

    // ---- 常驻流式纹理槽（键寻址；直色上传，不参与通用缓存淘汰）----
    struct StreamSlot {
        Tex tex{};
        int width = 0;
        int height = 0;
        std::uint64_t version = 0;  // 已上传内容对应的流式版本
    };
    std::unordered_map<std::uint64_t, StreamSlot> stream_slots;

    // ---- GPU 层缓存（常驻层纹理 + 效果采样拷贝 aux，惰性分配）----
    struct LayerEntry {
        Tex tex{};
        Tex aux{};  // 层内效果的采样拷贝（首次效果时分配）
        int width = 0;
        int height = 0;
    };
    struct LayerFrame {
        std::uint64_t key = 0;
        std::vector<ClipState> saved_clip;
        double saved_alpha = 1.0;
        int width = 0;
        int height = 0;
        float logical_w = 0.0F;
        float logical_h = 0.0F;
    };
    std::unordered_map<std::uint64_t, LayerEntry> layer_cache;
    std::vector<LayerFrame> layer_stack;
    bool layer_miss_warned = false;  // DrawLayer 未命中告警只发一次

    WgpuRhi::FrameStats stats;

    // ---- readback（离屏诊断通道）----
    WGPUBuffer readback = nullptr;
    std::uint64_t readback_cap = 0;
    std::uint64_t readback_len = 0;
    std::uint32_t readback_bpr = 0;
    bool map_done = false;
    bool map_armed = false;
    bool readback_mapped = false;  // 自行跟踪映射态：v29 的 wgpuBufferGetMapState 是 unimplemented 存根，调用即 panic

    std::vector<std::uint8_t> upload_scratch;  // writeTexture 行 256 对齐暂存

    // ---- 异步回调状态 ----
    bool cb_done = false;
    std::string cb_error;

    Impl() = default;

    explicit Impl(const WgpuRhiOptions &opts) : options(opts) {
        if (!init_instance() || !init_adapter() || !init_device()) {
            shutdown();
            return;
        }
        if (options.native_window != nullptr && !create_surface()) {
            shutdown();
            return;
        }
        if (!init_gpu()) {
            shutdown();
            return;
        }
        device_ok = true;
        AURORA_LOG_INFO("gpu-wgpu", "backend up (", backend_name(adapter_backend_), ", surface=",
                        surface != nullptr ? "host" : "offscreen", ")");
    }

    ~Impl() { shutdown(); }
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;

    void shutdown() {
        AURORA_WGPU_RELEASE(pass, wgpuRenderPassEncoderRelease)
        AURORA_WGPU_RELEASE(encoder, wgpuCommandEncoderRelease)
        AURORA_WGPU_RELEASE(readback, wgpuBufferRelease)
        AURORA_WGPU_RELEASE(uniform_buf, wgpuBufferRelease)
        AURORA_WGPU_RELEASE(vertex_buf, wgpuBufferRelease)
        for (int i = 0; i < kPipeCount; ++i) {
            AURORA_WGPU_RELEASE(pipes1_[i], wgpuRenderPipelineRelease)
            AURORA_WGPU_RELEASE(pipes4_[i], wgpuRenderPipelineRelease)
        }
        AURORA_WGPU_RELEASE(pipe_present_, wgpuRenderPipelineRelease)
        for (LutEntry &e : lut_cache) {
            release_tex(&e.tex);
        }
        lut_cache.clear();
        for (ImageTexEntry &e : image_cache) {
            release_tex(&e.tex);
        }
        image_cache.clear();
        for (GlyphPage &pg : glyph_pages) {
            release_tex(&pg.tex);
        }
        glyph_pages.clear();
        glyph_slots.clear();
        for (auto &kv : stream_slots) {
            release_tex(&kv.second.tex);
        }
        stream_slots.clear();
        for (auto &kv : layer_cache) {
            release_tex(&kv.second.tex);
            release_tex(&kv.second.aux);
        }
        layer_cache.clear();
        release_tex(&canvas_);
        release_tex(&msaa_);
        release_tex(&alt_);
        release_tex(&dummy_);
        AURORA_WGPU_RELEASE(samp_point_, wgpuSamplerRelease)
        AURORA_WGPU_RELEASE(samp_linear_, wgpuSamplerRelease)
        AURORA_WGPU_RELEASE(pipeline_layout_, wgpuPipelineLayoutRelease)
        AURORA_WGPU_RELEASE(bgl_, wgpuBindGroupLayoutRelease)
        AURORA_WGPU_RELEASE(shader_, wgpuShaderModuleRelease)
        AURORA_WGPU_RELEASE(frame_view, wgpuTextureViewRelease)
        AURORA_WGPU_RELEASE(frame_tex, wgpuTextureRelease)
        AURORA_WGPU_RELEASE(surface, wgpuSurfaceRelease)
        AURORA_WGPU_RELEASE(queue, wgpuQueueRelease)
        AURORA_WGPU_RELEASE(device, wgpuDeviceRelease)
        AURORA_WGPU_RELEASE(adapter, wgpuAdapterRelease)
        AURORA_WGPU_RELEASE(instance, wgpuInstanceRelease)
        device_ok = false;
    }

    // ---- 异步初始化（v29 全异步：回调 + 事件泵）----

    static void on_adapter_cb(WGPURequestAdapterStatus status, WGPUAdapter ad, WGPUStringView msg, void *u1, void *) {
        auto *self = static_cast<Impl *>(u1);
        self->cb_done = true;
        if (status == WGPURequestAdapterStatus_Success && ad != nullptr) {
            self->adapter = ad;  // PassedWithOwnership
            return;
        }
        self->cb_error = "requestAdapter: " + sv_view(msg);
    }
    static void on_device_cb(WGPURequestDeviceStatus status, WGPUDevice dev, WGPUStringView msg, void *u1, void *) {
        auto *self = static_cast<Impl *>(u1);
        self->cb_done = true;
        if (status == WGPURequestDeviceStatus_Success && dev != nullptr) {
            self->device = dev;
            return;
        }
        self->cb_error = "requestDevice: " + sv_view(msg);
    }
    static void on_map_cb(WGPUMapAsyncStatus status, WGPUStringView msg, void *u1, void *) {
        auto *self = static_cast<Impl *>(u1);
        self->map_done = true;
        if (status == WGPUMapAsyncStatus_Success) {
            self->readback_mapped = true;
        } else {
            AURORA_LOG_WARN("gpu-wgpu", "bufferMapAsync failed: ", sv_view(msg));
        }
    }

    // ⚠️ wgpu-native v29.0.1.1 实态：requestAdapter/requestDevice 在返回前**同步**触发
    // 回调；bufferMapAsync 的回调经 wgpuInstanceProcessEvents 轮询触发；而
    // wgpuInstanceWaitAny 是上游 unimplemented!() 存根（调用即 panic）——一律不可用。
    // 事件泵 = ProcessEvents 循环 + 1ms 步进 + 超时上限。
    [[nodiscard]] bool pump(bool &done_flag, std::uint64_t timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!done_flag) {
            if (instance == nullptr) {
                return false;
            }
            wgpuInstanceProcessEvents(instance);
            if (done_flag) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    [[nodiscard]] bool init_instance() {
        instance = wgpuCreateInstance(nullptr);
        if (instance == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "wgpuCreateInstance failed");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool init_adapter() {
        cb_done = false;
        cb_error.clear();
        WGPURequestAdapterOptions opts{};
        opts.featureLevel =
            options.backend == WgpuRhiOptions::Backend::GLES ? WGPUFeatureLevel_Compatibility
                                                             : WGPUFeatureLevel_Undefined;  // Undefined → Core
        opts.powerPreference = WGPUPowerPreference_Undefined;
        opts.forceFallbackAdapter = WGPU_FALSE;
        opts.backendType = backend_filter();
        if (surface != nullptr) {
            opts.compatibleSurface = surface;
        }
        WGPURequestAdapterCallbackInfo cb{};
        cb.mode = WGPUCallbackMode_WaitAnyOnly;
        cb.callback = &Impl::on_adapter_cb;
        cb.userdata1 = this;
        wgpuInstanceRequestAdapter(instance, &opts, cb);  // 回调在本调用返回前同步触发
        if (!cb_done || adapter == nullptr) {
            if (cb_error.empty()) {
                cb_error = "no adapter available (timeout or none matched)";
            }
            AURORA_LOG_ERROR("gpu-wgpu", "adapter request failed: ", cb_error);
            return false;
        }
        WGPUAdapterInfo info{};
        if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
            compute_cap = info.backendType != WGPUBackendType_OpenGL && info.backendType != WGPUBackendType_OpenGLES;
            adapter_backend_ = info.backendType;
        }
        return true;
    }

    [[nodiscard]] bool init_device() {
        cb_done = false;
        cb_error.clear();
        WGPURequestDeviceCallbackInfo cb{};
        cb.mode = WGPUCallbackMode_WaitAnyOnly;
        cb.callback = &Impl::on_device_cb;
        cb.userdata1 = this;
        wgpuAdapterRequestDevice(adapter, nullptr, cb);  // 回调同步触发（同上）
        if (!cb_done || device == nullptr) {
            if (cb_error.empty()) {
                cb_error = "device request failed";
            }
            AURORA_LOG_ERROR("gpu-wgpu", cb_error);
            return false;
        }
        queue = wgpuDeviceGetQueue(device);
        return queue != nullptr;
    }

    [[nodiscard]] auto backend_filter() const -> WGPUBackendType {
        switch (options.backend) {
            case WgpuRhiOptions::Backend::Vulkan:
                return WGPUBackendType_Vulkan;
            case WgpuRhiOptions::Backend::D3D12:
                return WGPUBackendType_D3D12;
            case WgpuRhiOptions::Backend::Metal:
                return WGPUBackendType_Metal;
            case WgpuRhiOptions::Backend::GLES:
                return WGPUBackendType_OpenGLES;
            case WgpuRhiOptions::Backend::Auto:
            default:
                return WGPUBackendType_Undefined;
        }
    }

    static auto backend_name(WGPUBackendType t) -> const char * {
        switch (t) {
            case WGPUBackendType_Vulkan:
                return "vulkan";
            case WGPUBackendType_D3D12:
                return "d3d12";
            case WGPUBackendType_Metal:
                return "metal";
            case WGPUBackendType_OpenGL:
                return "opengl";
            case WGPUBackendType_OpenGLES:
                return "opengles";
            default:
                return "unknown";
        }
    }

    // ---- surface ----

    [[nodiscard]] bool create_surface() {
        WGPUSurfaceDescriptor desc{};
#ifdef _WIN32
        WGPUSurfaceSourceWindowsHWND src{};
        src.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        // hinstance 不可为 NULL：v29 下 `wgpuSurfaceGetCapabilities` 对 null-HINSTANCE 的
        // HWND surface 直接返回 Error（真机 cap 探针隔离证实）。优先取窗口实主实例，
        // 兜底 GetModuleHandle(nullptr)（webgpu.h 头注推荐值）。
        auto *hwnd = static_cast<HWND>(options.native_window);
        auto *hinstance = reinterpret_cast<void *>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        if (hinstance == nullptr) {
            hinstance = GetModuleHandle(nullptr);
        }
        src.hinstance = hinstance;
        src.hwnd = options.native_window;
        desc.nextInChain = &src.chain;
#elif defined(__linux__)
        if (options.native_display == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "Xlib surface requires native_display (Display*)");
            return false;
        }
        WGPUSurfaceSourceXlibWindow src{};
        src.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
        src.display = options.native_display;
        src.window = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(options.native_window));
        desc.nextInChain = &src.chain;
#else
        AURORA_LOG_ERROR("gpu-wgpu", "native window surface not supported on this platform yet");
        return false;
#endif
        surface = wgpuInstanceCreateSurface(instance, &desc);
        if (surface == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "wgpuInstanceCreateSurface returned null");
            return false;
        }
        return true;
    }

    // 依据 surface capabilities 协商配置 swapchain；失败 = 本帧起宿主回退软件路径。
    [[nodiscard]] bool configure_surface(int w, int h) {
        WGPUSurfaceCapabilities caps{};
        if (wgpuSurfaceGetCapabilities(surface, adapter, &caps) != WGPUStatus_Success) {
            AURORA_LOG_ERROR("gpu-wgpu", "surfaceGetCapabilities failed");
            return false;
        }
        // 格式偏好：BGRA8Unorm / RGBA8Unorm 优先（读回/合成语义直接），否则取列表首项。
        WGPUTextureFormat fmt = WGPUTextureFormat_Undefined;
        for (std::size_t i = 0; i < caps.formatCount; ++i) {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                fmt = caps.formats[i];
                break;
            }
        }
        if (fmt == WGPUTextureFormat_Undefined && caps.formatCount > 0) {
            fmt = caps.formats[0];
        }
        const WGPUPresentMode want = options.vsync ? WGPUPresentMode_Fifo : WGPUPresentMode_Immediate;
        present_mode = WGPUPresentMode_Fifo;  // FIFO 恒有保证（头注明）
        for (std::size_t i = 0; i < caps.presentModeCount; ++i) {
            if (caps.presentModes[i] == want) {
                present_mode = want;
                break;
            }
        }
        const bool has_format = fmt != WGPUTextureFormat_Undefined;
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        if (!has_format) {
            AURORA_LOG_ERROR("gpu-wgpu", "surface exposes no usable format");
            return false;
        }

        WGPUSurfaceConfiguration cfg{};
        cfg.device = device;
        cfg.format = fmt;
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = static_cast<std::uint32_t>(w);
        cfg.height = static_cast<std::uint32_t>(h);
        cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
        cfg.presentMode = present_mode;
        wgpuSurfaceConfigure(surface, &cfg);  // 错误在下帧 getCurrentTexture 状态中暴露
        surface_format = fmt;
        surface_cw = w;
        surface_ch = h;
        surface_configured = true;
        return true;
    }

    // ---- 纹理所（创建 / 释放 / 上传）----

    [[nodiscard]] Tex make_tex(int w, int h, WGPUTextureFormat fmt, WGPUTextureUsage usage,
                               std::uint32_t samples = 1) {
        Tex t;
        WGPUTextureDescriptor td{};
        td.usage = usage;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = static_cast<std::uint32_t>(w);
        td.size.height = static_cast<std::uint32_t>(h);
        td.size.depthOrArrayLayers = 1;
        td.format = fmt;
        td.mipLevelCount = 1;
        td.sampleCount = samples;
        t.tex = wgpuDeviceCreateTexture(device, &td);
        if (t.tex == nullptr) {
            return t;
        }
        t.view = wgpuTextureCreateView(t.tex, nullptr);
        if (t.view == nullptr) {
            wgpuTextureRelease(t.tex);
            t.tex = nullptr;
            return t;
        }
        t.width = w;
        t.height = h;
        return t;
    }

    static void release_tex(Tex *t) {
        AURORA_WGPU_RELEASE(t->view, wgpuTextureViewRelease)
        AURORA_WGPU_RELEASE(t->tex, wgpuTextureRelease)
        t->width = 0;
        t->height = 0;
    }

    /// @brief 紧凑行像素 → 纹理子区上传（writeTexture 行跨距 256 对齐，经暂存补齐）。
    /// `pixels` 行跨距 `src_stride`（0 = 紧凑 w*bpp）；(x,y) 为目标纹素原点。
    auto write_tex_sub(const Tex &t, int x, int y, int w, int h, const std::uint8_t *pixels,
                       std::size_t src_stride, int bpp) -> bool {
        if (t.tex == nullptr || w <= 0 || h <= 0 || pixels == nullptr) {
            return false;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(bpp);
        const auto bpr = static_cast<std::uint32_t>((row_bytes + 255U) & ~255ULL);
        const std::size_t stride = src_stride != 0 ? src_stride : row_bytes;
        const std::uint8_t *data = pixels;
        if (bpr != row_bytes) {
            upload_scratch.assign(static_cast<std::size_t>(bpr) * static_cast<std::size_t>(h), 0);
            for (int r = 0; r < h; ++r) {
                std::memcpy(upload_scratch.data() + static_cast<std::size_t>(r) * bpr,
                            pixels + static_cast<std::size_t>(r) * stride, row_bytes);
            }
            data = upload_scratch.data();
        }
        WGPUTexelCopyTextureInfo dest{};
        dest.texture = t.tex;
        dest.mipLevel = 0;
        dest.origin.x = static_cast<std::uint32_t>(x);
        dest.origin.y = static_cast<std::uint32_t>(y);
        dest.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout layout{};
        layout.offset = 0;
        layout.bytesPerRow = bpr == row_bytes ? static_cast<std::size_t>(row_bytes) : bpr;
        layout.rowsPerImage = static_cast<std::uint32_t>(h);
        WGPUExtent3D extent{};
        extent.width = static_cast<std::uint32_t>(w);
        extent.height = static_cast<std::uint32_t>(h);
        extent.depthOrArrayLayers = 1;
        wgpuQueueWriteTexture(queue, &dest, data, static_cast<std::size_t>(layout.bytesPerRow) * h, &layout, &extent);
        return true;
    }

    // ---- 着色器 / 管线初始化 ----

    [[nodiscard]] bool init_gpu() {
        WGPUShaderSourceWGSL wsrc{};
        wsrc.chain.sType = WGPUSType_ShaderSourceWGSL;
        wsrc.code = sv(kWgsl);
        WGPUShaderModuleDescriptor mdesc{};
        mdesc.nextInChain = &wsrc.chain;
        shader_ = wgpuDeviceCreateShaderModule(device, &mdesc);
        if (shader_ == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "WGSL module creation failed");
            return false;
        }
        // bind group：0 uniform，1 源纹理（Float：可滤性由格式推导），2/3 采样器（point/linear）。
        WGPUBindGroupLayoutEntry bge[4] = {};
        bge[0].binding = 0;
        bge[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        bge[0].buffer.type = WGPUBufferBindingType_Uniform;
        bge[0].buffer.minBindingSize = sizeof(Globals);
        bge[1].binding = 1;
        bge[1].visibility = WGPUShaderStage_Fragment;
        bge[1].texture.sampleType = WGPUTextureSampleType_Float;
        bge[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        bge[2].binding = 2;
        bge[2].visibility = WGPUShaderStage_Fragment;
        bge[2].sampler.type = WGPUSamplerBindingType_Filtering;
        bge[3].binding = 3;
        bge[3].visibility = WGPUShaderStage_Fragment;
        bge[3].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor bgld{};
        bgld.entryCount = 4;
        bgld.entries = bge;
        bgl_ = wgpuDeviceCreateBindGroupLayout(device, &bgld);
        WGPUPipelineLayoutDescriptor playout{};
        playout.bindGroupLayoutCount = 1;
        playout.bindGroupLayouts = &bgl_;
        pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device, &playout);
        if (bgl_ == nullptr || pipeline_layout_ == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "layout creation failed");
            return false;
        }
        auto make_sampler = [&](WGPUFilterMode filter) -> WGPUSampler {
            WGPUSamplerDescriptor sd{};
            sd.addressModeU = WGPUAddressMode_ClampToEdge;
            sd.addressModeV = WGPUAddressMode_ClampToEdge;
            sd.addressModeW = WGPUAddressMode_ClampToEdge;
            sd.magFilter = filter;
            sd.minFilter = filter;
            sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            sd.lodMinClamp = 0.0F;
            sd.lodMaxClamp = 1.0F;
            sd.maxAnisotropy = 1;
            return wgpuDeviceCreateSampler(device, &sd);
        };
        samp_point_ = make_sampler(WGPUFilterMode_Nearest);
        samp_linear_ = make_sampler(WGPUFilterMode_Linear);
        if (samp_point_ == nullptr || samp_linear_ == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "sampler creation failed");
            return false;
        }
        dummy_ = make_tex(1, 1, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
        if (dummy_.tex == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "dummy texture creation failed");
            return false;
        }
        static const std::uint8_t zeros[4] = {0, 0, 0, 0};
        write_tex_sub(dummy_, 0, 0, 1, 1, zeros, 0, 4);
        return ensure_pipelines();
    }

    // 13 管线 × 2 采样数集（目标恒 RGBA8Unorm：画布/层/alt 全部离屏纹理；present 独立）。
    [[nodiscard]] bool ensure_pipelines() {
        if (pipes1_[kPipeSolid] != nullptr && pipes4_[kPipeSolid] != nullptr) {
            return true;
        }
        WGPUVertexAttribute attrs[3] = {};
        const std::uint64_t sizes[3] = {8, 8, 4};
        std::uint64_t off = 0;
        for (int i = 0; i < 3; ++i) {
            attrs[i].offset = off;
            attrs[i].shaderLocation = static_cast<std::uint32_t>(i);
            off += sizes[i];
        }
        attrs[0].format = WGPUVertexFormat_Float32x2;
        attrs[1].format = WGPUVertexFormat_Float32x2;
        attrs[2].format = WGPUVertexFormat_Unorm8x4;

        WGPUVertexBufferLayout vbl{};
        vbl.stepMode = WGPUVertexStepMode_Vertex;
        vbl.arrayStride = sizeof(Vertex);
        vbl.attributeCount = 3;
        vbl.attributes = attrs;

        WGPUVertexState vs{};
        vs.module = shader_;
        vs.entryPoint = sv("vs_main");
        vs.bufferCount = 1;
        vs.buffers = &vbl;

        WGPUBlendComponent std_color{.operation = WGPUBlendOperation_Add,
                                     .srcFactor = WGPUBlendFactor_SrcAlpha,
                                     .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha};
        WGPUBlendComponent std_alpha{.operation = WGPUBlendOperation_Add,
                                     .srcFactor = WGPUBlendFactor_One,
                                     .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha};
        WGPUBlendComponent pma_factor{.operation = WGPUBlendOperation_Add,
                                      .srcFactor = WGPUBlendFactor_One,
                                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha};
        WGPUBlendState blend_std{};
        blend_std.color = std_color;
        blend_std.alpha = std_alpha;
        WGPUBlendState blend_pma{};
        blend_pma.color = pma_factor;
        blend_pma.alpha = pma_factor;

        WGPUColorTargetState target{};
        target.format = WGPUTextureFormat_RGBA8Unorm;
        target.writeMask = WGPUColorWriteMask_All;

        // ⚠️ mask 必须显式全采样掩码：descriptor 非 optional，零初始化 = 0 掩码 → 像素全丢弃。
        auto build = [&](std::uint32_t samples, int pipe) -> WGPURenderPipeline {
            WGPUFragmentState fs{};
            fs.module = shader_;
            fs.entryPoint = sv(kPipeSpecs[pipe].fs);
            fs.targetCount = 1;
            WGPUColorTargetState t2 = target;
            const int blend_kind = kPipeSpecs[pipe].blend;
            if (blend_kind != 0) {
                t2.blend = blend_kind == 2 ? &blend_pma : &blend_std;
            }
            fs.targets = &t2;
            WGPURenderPipelineDescriptor d{};
            d.layout = pipeline_layout_;
            d.vertex = vs;
            d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            d.primitive.cullMode = WGPUCullMode_None;
            d.multisample.count = samples;
            d.multisample.mask = 0xFFFFFFFFU;
            d.fragment = &fs;
            return wgpuDeviceCreateRenderPipeline(device, &d);
        };
        for (int i = 0; i < kPipeCount; ++i) {
            if (pipes1_[i] == nullptr) {
                pipes1_[i] = build(1, i);
            }
            if (pipes4_[i] == nullptr) {
                pipes4_[i] = build(4, i);
            }
            if (pipes1_[i] == nullptr || pipes4_[i] == nullptr) {
                AURORA_LOG_ERROR("gpu-wgpu", "pipeline ", i, " creation failed (fs=", kPipeSpecs[i].fs, ")");
                return false;
            }
        }
        return true;
    }

    // present 管线：surface 协商格式变化时重建（vs_present 直通裁剪坐标 + fs_copy）。
    [[nodiscard]] bool ensure_present_pipeline(WGPUTextureFormat format) {
        if (pipe_present_ != nullptr && present_format_ == format) {
            return true;
        }
        AURORA_WGPU_RELEASE(pipe_present_, wgpuRenderPipelineRelease)
        WGPUVertexAttribute attrs[3] = {};
        const std::uint64_t sizes[3] = {8, 8, 4};
        std::uint64_t off = 0;
        for (int i = 0; i < 3; ++i) {
            attrs[i].offset = off;
            attrs[i].shaderLocation = static_cast<std::uint32_t>(i);
            off += sizes[i];
        }
        attrs[0].format = WGPUVertexFormat_Float32x2;
        attrs[1].format = WGPUVertexFormat_Float32x2;
        attrs[2].format = WGPUVertexFormat_Unorm8x4;
        WGPUVertexBufferLayout vbl{};
        vbl.stepMode = WGPUVertexStepMode_Vertex;
        vbl.arrayStride = sizeof(Vertex);
        vbl.attributeCount = 3;
        vbl.attributes = attrs;
        WGPUVertexState vs{};
        vs.module = shader_;
        vs.entryPoint = sv("vs_present");
        vs.bufferCount = 1;
        vs.buffers = &vbl;
        WGPUColorTargetState target{};
        target.format = format;
        target.writeMask = WGPUColorWriteMask_All;
        WGPUFragmentState fs{};
        fs.module = shader_;
        fs.entryPoint = sv("fs_copy");
        fs.targetCount = 1;
        fs.targets = &target;
        WGPURenderPipelineDescriptor d{};
        d.layout = pipeline_layout_;
        d.vertex = vs;
        d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        d.primitive.cullMode = WGPUCullMode_None;
        d.multisample.count = 1;
        d.multisample.mask = 0xFFFFFFFFU;
        d.fragment = &fs;
        pipe_present_ = wgpuDeviceCreateRenderPipeline(device, &d);
        if (pipe_present_ == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "present pipeline creation failed (format 0x",
                             hex_u32(static_cast<std::uint32_t>(format)), ")");
            return false;
        }
        present_format_ = format;
        return true;
    }

    // ---- 目标管理 ----

    /// @brief 画布三件套随设备尺寸建/重建（旧对象经已录制引用保活，直接弃置安全）。
    [[nodiscard]] bool ensure_canvas_targets(int w, int h) {
        if (canvas_.width == w && canvas_.height == h && msaa_.tex != nullptr) {
            return true;
        }
        release_tex(&canvas_);
        release_tex(&msaa_);
        release_tex(&alt_);
        msaa_ = make_tex(w, h, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment, kMsaaSamples);
        canvas_ = make_tex(w, h, WGPUTextureFormat_RGBA8Unorm,
                           WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc);
        alt_ = make_tex(w, h, WGPUTextureFormat_RGBA8Unorm,
                        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                            WGPUTextureUsage_CopyDst);
        if (msaa_.tex == nullptr || canvas_.tex == nullptr || alt_.tex == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "canvas targets ", w, "x", h, " creation failed");
            return false;
        }
        device_w = w;
        device_h = h;
        return true;
    }

    /// @brief 效果中转纹理：层尺寸可能大于画布，不足时按 max 重建（增长-only）。
    [[nodiscard]] bool ensure_alt(int w, int h) {
        if (alt_.width >= w && alt_.height >= h && alt_.tex != nullptr) {
            return true;
        }
        const int nw = std::max({w, device_w, 1});
        const int nh = std::max({h, device_h, 1});
        release_tex(&alt_);
        alt_ = make_tex(nw, nh, WGPUTextureFormat_RGBA8Unorm,
                        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                            WGPUTextureUsage_CopyDst);
        return alt_.tex != nullptr;
    }

    // ---- pass 管理 ----
    // 不变量：画布 pass 一律「MSAA 附着 + canvas resolveTarget」，关闭即自动 resolve——
    // canvas_ 纹理新鲜度与画布 pass 关闭一一对应，效果采样前必先关闭画布 pass。
    // resolveTarget 仅对 MSAA 附着合法（单采样 pass 置空）。

    void close_pass() {
        if (pass != nullptr) {
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            pass = nullptr;
        }
        pass_kind = kPassNone;
    }

    [[nodiscard]] bool open_pass(const Tex &target, WGPUTextureView resolve, bool clear, PassKind kind) {
        WGPURenderPassColorAttachment att{};
        att.view = target.view;
        att.resolveTarget = resolve;
        att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        att.loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
        att.storeOp = WGPUStoreOp_Store;
        att.clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};
        WGPURenderPassDescriptor pd{};
        pd.colorAttachmentCount = 1;
        pd.colorAttachments = &att;
        pass = wgpuCommandEncoderBeginRenderPass(encoder, &pd);
        pass_kind = pass != nullptr ? kind : kPassNone;
        return pass != nullptr;
    }

    /// @brief 当前层重定向的附着（层内命令的目标）；无层时 null。
    [[nodiscard]] auto current_layer_entry() -> LayerEntry * {
        if (layer_stack.empty()) {
            return nullptr;
        }
        const auto it = layer_cache.find(layer_stack.back().key);
        return it == layer_cache.end() ? nullptr : &it->second;
    }

    /// @brief pass 意外关闭后的恢复（效果序列/防御路径）：按当前目标重开 Load pass。
    [[nodiscard]] bool ensure_target_pass() {
        if (pass != nullptr) {
            return true;
        }
        if (LayerEntry *entry = current_layer_entry(); entry != nullptr && entry->tex.tex != nullptr) {
            return open_pass(entry->tex, nullptr, false, kPassLayer);
        }
        return open_pass(msaa_, canvas_.view, false, kPassCanvas);
    }

    // ---- 逻辑尺寸（层重定向感知，同 GL 路径）----

    [[nodiscard]] auto logical_w() const -> float {
        if (!layer_stack.empty()) {
            return layer_stack.back().logical_w;
        }
        return static_cast<float>(device_w) / (scale > 0.0F ? scale : 1.0F);
    }
    [[nodiscard]] auto logical_h() const -> float {
        if (!layer_stack.empty()) {
            return layer_stack.back().logical_h;
        }
        return static_cast<float>(device_h) / (scale > 0.0F ? scale : 1.0F);
    }
    /// @brief 当前绘制目标设备尺寸（层重定向中 = 层纹理尺寸；否则 = 画布）。
    [[nodiscard]] auto target_w() const -> int { return layer_stack.empty() ? device_w : layer_stack.back().width; }
    [[nodiscard]] auto target_h() const -> int { return layer_stack.empty() ? device_h : layer_stack.back().height; }

    // ---- 环上传（扩容重放 CPU 暂存，旧缓冲交给 wgpu 提交后回收）----

    [[nodiscard]] bool grow_vertex_buffer(std::uint64_t need) {
        std::uint64_t cap = vertex_cap == 0 ? (1U << 20) : vertex_cap;
        while (cap < need) {
            cap *= 2;
        }
        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = cap;
        WGPUBuffer nb = wgpuDeviceCreateBuffer(device, &bd);
        if (nb == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "vertex buffer alloc failed (", cap, " bytes)");
            return false;
        }
        if (!vstage.empty()) {
            wgpuQueueWriteBuffer(queue, nb, 0, vstage.data(), vstage.size());
        }
        AURORA_WGPU_RELEASE(vertex_buf, wgpuBufferRelease)
        vertex_buf = nb;
        vertex_cap = cap;
        return true;
    }

    [[nodiscard]] bool grow_uniform_buffer(std::uint64_t need) {
        std::uint64_t cap = uniform_cap == 0 ? (1U << 16) : uniform_cap;
        while (cap < need) {
            cap *= 2;
        }
        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = cap;
        WGPUBuffer nb = wgpuDeviceCreateBuffer(device, &bd);
        if (nb == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "uniform buffer alloc failed (", cap, " bytes)");
            return false;
        }
        if (!ustage.empty()) {
            wgpuQueueWriteBuffer(queue, nb, 0, ustage.data(), ustage.size());
        }
        AURORA_WGPU_RELEASE(uniform_buf, wgpuBufferRelease)
        uniform_buf = nb;
        uniform_cap = cap;
        return true;
    }

    // ---- 渐变 LUT 缓存（内容键线性查找精确比对；超容量整体清空重建）----

    [[nodiscard]] auto acquire_lut(const std::vector<Color> &colors, const std::vector<float> &stops)
        -> WGPUTextureView {
        for (const LutEntry &e : lut_cache) {
            if (e.colors == colors && e.stops == stops) {
                return e.tex.view;
            }
        }
        if (lut_cache.size() >= kLutCacheCap) {
            // wgpu 纹理对象经已录制 bind group 引用保活，删除即时缓存不影响待提交批
            // （与 GL 悬垂名不同源，无需先 flush）。
            for (LutEntry &e : lut_cache) {
                release_tex(&e.tex);
            }
            lut_cache.clear();
        }
        std::array<std::uint8_t, static_cast<std::size_t>(kLutWidth) * 4U> texels{};
        for (int j = 0; j < kLutWidth; ++j) {
            const Color c = sample_gradient_lut(colors, stops, static_cast<float>(j) / 255.0F);
            texels[static_cast<std::size_t>(j) * 4U + 0] = c.r;
            texels[static_cast<std::size_t>(j) * 4U + 1] = c.g;
            texels[static_cast<std::size_t>(j) * 4U + 2] = c.b;
            texels[static_cast<std::size_t>(j) * 4U + 3] = c.a;
        }
        LutEntry e;
        e.colors = colors;
        e.stops = stops;
        e.tex = make_tex(kLutWidth, 1, WGPUTextureFormat_RGBA8Unorm,
                         WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
        if (e.tex.tex == nullptr) {
            AURORA_LOG_WARN("gpu-wgpu", "LUT texture alloc failed");
            return nullptr;
        }
        write_tex_sub(e.tex, 0, 0, kLutWidth, 1, texels.data(), 0, 4);
        lut_cache.push_back(std::move(e));
        return lut_cache.back().tex.view;
    }

    // ---- 图像纹理缓存（键 = content_hash ^ 维度混列；PMA 上传，LINEAR 空间插值）----

    [[nodiscard]] auto acquire_image_tex(const Image &img) -> WGPUTextureView {
        std::uint64_t hash = img.content_hash();
        hash ^= static_cast<std::uint64_t>(img.width);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint64_t>(img.height);
        hash *= 1099511628211ULL;
        for (const ImageTexEntry &e : image_cache) {
            if (e.hash == hash && e.width == img.width && e.height == img.height) {
                return e.tex.view;
            }
        }
        if (image_cache.size() >= kImageCacheCap) {
            for (ImageTexEntry &e : image_cache) {
                release_tex(&e.tex);
            }
            image_cache.clear();
        }
        // 预乘 alpha：dst = src.rgb * src.a / 255（四舍五入）——与软件双线性语义同源的
        // 边缘暗晕消除（同 GL 路径逐字节一致）。
        std::vector<std::uint8_t> pma(img.pixels.size());
        const std::size_t n = img.pixels.size();
        for (std::size_t i = 0; i + 3 < n; i += 4) {
            const unsigned a = img.pixels[i + 3];
            pma[i + 0] = static_cast<std::uint8_t>((static_cast<unsigned>(img.pixels[i + 0]) * a + 127) / 255);
            pma[i + 1] = static_cast<std::uint8_t>((static_cast<unsigned>(img.pixels[i + 1]) * a + 127) / 255);
            pma[i + 2] = static_cast<std::uint8_t>((static_cast<unsigned>(img.pixels[i + 2]) * a + 127) / 255);
            pma[i + 3] = static_cast<std::uint8_t>(a);
        }
        ImageTexEntry e;
        e.hash = hash;
        e.width = img.width;
        e.height = img.height;
        e.tex = make_tex(img.width, img.height, WGPUTextureFormat_RGBA8Unorm,
                         WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
        if (e.tex.tex == nullptr) {
            AURORA_LOG_WARN("gpu-wgpu", "image texture alloc failed");
            return nullptr;
        }
        write_tex_sub(e.tex, 0, 0, img.width, img.height, pma.data(), 0, 4);
        image_cache.push_back(std::move(e));
        return image_cache.back().tex.view;
    }

    // ---- GPU 字形图集（多页 R8 架式打包 + LRU 页淘汰，策略同 GL 路径）----

    [[nodiscard]] int new_glyph_page(int w, int h) {
        GlyphPage page;
        page.tex = make_tex(w, h, WGPUTextureFormat_R8Unorm,
                            WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
        if (page.tex.tex == nullptr) {
            AURORA_LOG_WARN("gpu-wgpu", "glyph page ", w, "x", h, " alloc failed");
            return -1;
        }
        glyph_pages.push_back(page);
        return static_cast<int>(glyph_pages.size()) - 1;
    }

    // 淘汰最久未用页并按新尺寸重建纹理（wgpu 纹理不可改存储：新建替换，旧对象经
    // 已录制 bind group 引用保活，待提交批不受影响——GL 的「先 flush 防悬垂名」在此
    // 无必要性，仅保留槽位清空的语义步骤）。
    auto evict_glyph_page(int new_w, int new_h) -> int {
        std::size_t victim = 0;
        for (std::size_t i = 1; i < glyph_pages.size(); ++i) {
            if (glyph_pages[i].tex.view != nullptr && glyph_pages[victim].tex.view != nullptr
                && glyph_pages[i].lru < glyph_pages[victim].lru) {
                victim = i;
            }
        }
        GlyphPage &pg = glyph_pages[victim];
        const WGPUTextureView old_view = pg.tex.view;
        release_tex(&pg.tex);
        std::erase_if(glyph_slots, [old_view](const auto &kv) { return kv.second.view == old_view; });
        pg.tex = make_tex(new_w, new_h, WGPUTextureFormat_R8Unorm,
                          WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
        pg.pack_x = 0;
        pg.pack_y = 0;
        pg.pack_row_h = 0;
        pg.lru = ++glyph_lru_clock_;
        return pg.tex.tex != nullptr ? static_cast<int>(victim) : -1;
    }

    // 取字形槽位；未命中即放置上传。返回空矩形（w/h ≤ 0）= 无需绘制（空位图字形或
    // 超出 kGlyphPageMax 的异常大字形）——同 GL 契约。
    [[nodiscard]] auto acquire_glyph_slot(std::uint64_t key, const render::GlyphAtlas::Entry &e) -> GlyphSlotRect {
        const auto it = glyph_slots.find(key);
        if (it != glyph_slots.end()) {
            for (GlyphPage &pg : glyph_pages) {
                if (pg.tex.view == it->second.view) {
                    pg.lru = ++glyph_lru_clock_;
                    break;
                }
            }
            return it->second;
        }
        if (e.width <= 0 || e.rows <= 0 || e.buf.empty()) {
            return GlyphSlotRect{};
        }
        const int w = e.width;
        const int h = e.rows;
        const int side = glyph_page_size_;
        if (w > side || h > side) {
            // 超大字形：pow2 专用页；页数封顶则淘汰 victim 重建为大页。
            int big = side;
            while (big < w || big < h) {
                big *= 2;
            }
            if (big > kGlyphPageMax) {
                return GlyphSlotRect{};
            }
            if (static_cast<int>(glyph_pages.size()) >= static_cast<int>(kGlyphPageCap)) {
                active_glyph_page_ = evict_glyph_page(big, big);
            } else {
                active_glyph_page_ = new_glyph_page(big, big);
            }
            if (active_glyph_page_ < 0) {
                return GlyphSlotRect{};
            }
        } else if (active_glyph_page_ < 0) {
            if (static_cast<int>(glyph_pages.size()) >= static_cast<int>(kGlyphPageCap)) {
                active_glyph_page_ = evict_glyph_page(side, side);
            } else {
                active_glyph_page_ = new_glyph_page(side, side);
            }
            if (active_glyph_page_ < 0) {
                return GlyphSlotRect{};
            }
        }
        // 用指针而非引用：页满分支可能 push 新页使 vector 重分配，须重取。
        GlyphPage *pg = &glyph_pages[static_cast<std::size_t>(active_glyph_page_)];
        if (pg->pack_x + w > pg->tex.width) {
            pg->pack_x = 0;
            pg->pack_y += pg->pack_row_h;
            pg->pack_row_h = 0;
        }
        if (pg->pack_y + h > pg->tex.height) {
            if (static_cast<int>(glyph_pages.size()) < static_cast<int>(kGlyphPageCap)) {
                active_glyph_page_ = new_glyph_page(side, side);
            } else {
                active_glyph_page_ = evict_glyph_page(side, side);
            }
            if (active_glyph_page_ < 0) {
                return GlyphSlotRect{};
            }
            pg = &glyph_pages[static_cast<std::size_t>(active_glyph_page_)];
            if (w > pg->tex.width || h > pg->tex.height || pg->pack_x + w > pg->tex.width) {
                return GlyphSlotRect{};  // 换页后仍放不下（异常大字形），放弃
            }
        }
        GlyphSlotRect slot;
        slot.view = pg->tex.view;
        slot.x = pg->pack_x;
        slot.y = pg->pack_y;
        slot.w = w;
        slot.h = h;
        slot.u0 = static_cast<float>(pg->pack_x) / static_cast<float>(pg->tex.width);
        slot.v0 = static_cast<float>(pg->pack_y) / static_cast<float>(pg->tex.height);
        slot.u1 = static_cast<float>(pg->pack_x + w) / static_cast<float>(pg->tex.width);
        slot.v1 = static_cast<float>(pg->pack_y + h) / static_cast<float>(pg->tex.height);
        if (!write_tex_sub(pg->tex, slot.x, slot.y, w, h, e.buf.data(), 0, 1)) {
            return GlyphSlotRect{};
        }
        pg->pack_x += w;
        pg->pack_row_h = std::max(pg->pack_row_h, h);
        pg->lru = ++glyph_lru_clock_;
        glyph_slots.emplace(key, slot);
        return slot;
    }

    // ---- 常驻流式纹理槽（键寻址，槽复用；尺寸变化新建重定义）----

    [[nodiscard]] StreamSlot *ensure_stream_slot(std::uint64_t key, int width, int height) {
        if (key == 0 || width <= 0 || height <= 0) {
            return nullptr;
        }
        auto it = stream_slots.find(key);
        if (it == stream_slots.end()) {
            StreamSlot slot;
            slot.width = width;
            slot.height = height;
            slot.tex = make_tex(width, height, WGPUTextureFormat_RGBA8Unorm,
                                WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
            if (slot.tex.tex == nullptr) {
                return nullptr;
            }
            it = stream_slots.emplace(key, std::move(slot)).first;
        } else if (it->second.width != width || it->second.height != height) {
            release_tex(&it->second.tex);
            it->second.tex = make_tex(width, height, WGPUTextureFormat_RGBA8Unorm,
                                      WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
            if (it->second.tex.tex == nullptr) {
                stream_slots.erase(it);
                return nullptr;
            }
            it->second.width = width;
            it->second.height = height;
            it->second.version = 0;
        }
        return &it->second;
    }

    // ---- GPU 层缓存 ----

    [[nodiscard]] bool create_layer_attachment(LayerEntry *entry, int w, int h) {
        const auto usage = static_cast<WGPUTextureUsage>(WGPUTextureUsage_RenderAttachment
                                                              | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc
                                                              | WGPUTextureUsage_CopyDst);
        release_tex(&entry->tex);
        release_tex(&entry->aux);  // 尺寸变化：aux 采样拷贝一并作废重建（惰性分配）
        entry->tex = make_tex(w, h, WGPUTextureFormat_RGBA8Unorm, usage);
        if (entry->tex.tex == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "layer texture ", w, "x", h, " creation failed");
            return false;
        }
        entry->width = w;
        entry->height = h;
        return true;
    }

    // 层内容 → aux 采样拷贝：flush + 关层 pass（内容仅在 pass 关闭时提交，encoder 拷贝
    // 必须排在关闭之后），调用方经 ensure_target_pass 以 Load 重开层 pass 继续回写。
    [[nodiscard]] bool copy_layer_to_aux(LayerEntry &entry) {
        if (entry.aux.width != entry.width || entry.aux.height != entry.height) {
            release_tex(&entry.aux);
            entry.aux = make_tex(entry.width, entry.height, WGPUTextureFormat_RGBA8Unorm,
                                 static_cast<WGPUTextureUsage>(WGPUTextureUsage_RenderAttachment
                                                                   | WGPUTextureUsage_TextureBinding
                                                                   | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst));
            if (entry.aux.tex == nullptr) {
                return false;
            }
        }
        flush_batch();
        close_pass();
        WGPUTexelCopyTextureInfo src{};
        src.texture = entry.tex.tex;
        src.mipLevel = 0;
        src.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = entry.aux.tex;
        dst.mipLevel = 0;
        dst.aspect = WGPUTextureAspect_All;
        WGPUExtent3D extent{};
        extent.width = static_cast<std::uint32_t>(entry.width);
        extent.height = static_cast<std::uint32_t>(entry.height);
        extent.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToTexture(encoder, &src, &dst, &extent);
        return true;
    }

    // ---- 批处理 ----

    static auto bake_alpha(Color c, double a) -> Color {
        const double scaled = static_cast<double>(c.a) * a;
        c.a = static_cast<std::uint8_t>(scaled < 0.0 ? 0 : (scaled > 255.0 ? 255 : scaled + 0.5));
        return c;
    }

    [[nodiscard]] auto effective_clip() const -> ClipState {
        if (clip_stack.empty()) {
            return ClipState{};
        }
        return clip_stack.back();
    }

    static auto intersect(const Rect &a, const Rect &b) -> Rect {
        const float x0 = std::max(a.origin.x, b.origin.x);
        const float y0 = std::max(a.origin.y, b.origin.y);
        const float x1 = std::min(a.origin.x + a.size.width, b.origin.x + b.size.width);
        const float y1 = std::min(a.origin.y + a.size.height, b.origin.y + b.size.height);
        return Rect{.origin = Point{.x = x0, .y = y0}, .size = Size{.width = x1 - x0, .height = y1 - y0}};
    }

    void push_quad(float x0, float y0, float x1, float y1, Color c) {
        push_quad_uv(x0, y0, x1, y1, 0.0F, 0.0F, 1.0F, 1.0F, c);
    }

    void push_quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c) {
        const Vertex base[4] = {
            Vertex{x0, y0, u0, v0, c.r, c.g, c.b, c.a},
            Vertex{x1, y0, u1, v0, c.r, c.g, c.b, c.a},
            Vertex{x1, y1, u1, v1, c.r, c.g, c.b, c.a},
            Vertex{x0, y1, u0, v1, c.r, c.g, c.b, c.a},
        };
        verts.insert(verts.end(), base, base + 4);
    }

    void begin_batch(const BatchKey &k) {
        if (key_active && !(key == k)) {
            flush_batch();
        }
        key = k;
        key_active = true;
    }

    // uniform → 环（256 对齐偏移 + CPU 暂存重放保偏移稳定），并建 bind group。
    [[nodiscard]] bool upload_globals_and_bind(const Globals &gu, WGPUTextureView view, std::uint64_t *uoff_out,
                                               WGPUBindGroup *bg_out) {
        const std::uint64_t uoff_raw = ustage.size();
        const std::uint64_t uoff = (uoff_raw + (kUniformAlign - 1)) & ~(kUniformAlign - 1);
        if (!grow_uniform_buffer(uoff + sizeof(Globals))) {
            return false;
        }
        ustage.resize(static_cast<std::size_t>(uoff));
        const auto *gb = reinterpret_cast<const std::uint8_t *>(&gu);
        ustage.insert(ustage.end(), gb, gb + sizeof(Globals));
        wgpuQueueWriteBuffer(queue, uniform_buf, uoff, &gu, sizeof(Globals));

        WGPUBindGroupEntry entries[4] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniform_buf;
        entries[0].offset = uoff;
        entries[0].size = sizeof(Globals);
        entries[1].binding = 1;
        entries[1].textureView = view != nullptr ? view : dummy_.view;
        entries[2].binding = 2;
        entries[2].sampler = samp_point_;
        entries[3].binding = 3;
        entries[3].sampler = samp_linear_;
        WGPUBindGroupDescriptor bgd{};
        bgd.layout = bgl_;
        bgd.entryCount = 4;
        bgd.entries = entries;
        *bg_out = wgpuDeviceCreateBindGroup(device, &bgd);
        *uoff_out = uoff;
        return *bg_out != nullptr;
    }

    [[nodiscard]] auto current_pipeline(int pipe) -> WGPURenderPipeline {
        return pass_kind == kPassCanvas ? pipes4_[pipe] : pipes1_[pipe];
    }

    auto upload_vertices(const std::vector<Vertex> &tri, std::uint64_t *voff_out, std::uint64_t *vbytes_out) -> bool {
        const std::uint64_t vbytes = tri.size() * sizeof(Vertex);
        const std::uint64_t voff = vstage.size();
        if (!grow_vertex_buffer(voff + vbytes)) {
            return false;
        }
        wgpuQueueWriteBuffer(queue, vertex_buf, voff, tri.data(), static_cast<std::size_t>(vbytes));
        const auto *vb = reinterpret_cast<const std::uint8_t *>(tri.data());
        vstage.insert(vstage.end(), vb, vb + vbytes);
        *voff_out = voff;
        *vbytes_out = vbytes;
        return true;
    }

    void flush_batch() {
        if (verts.empty()) {
            key_active = false;
            return;
        }
        const BatchKey bk = key;
        std::vector<Vertex> src = std::move(verts);
        verts.clear();
        key_active = false;
        if (!ensure_target_pass()) {
            return;
        }
        // 批键存 BasePipe 基底，混合/预乘变体在此解析为具体管线。
        int pipe = kPipeSolid;
        switch (bk.pipeline) {
            case kBaseSolid:
                pipe = bk.blend_off ? kPipeSolidNo : kPipeSolid;
                break;
            case kBaseBorder:
                pipe = kPipeBorder;
                break;
            case kBaseGrad:
                pipe = kPipeGrad;
                break;
            case kBaseImage:
                pipe = bk.blend_pma ? kPipeImagePma : kPipeImageSrc;
                break;
            case kBaseText:
                pipe = kPipeText;
                break;
            case kBaseShadow:
                pipe = kPipeShadow;
                break;
            default:
                break;
        }
        WGPURenderPipeline rp = current_pipeline(pipe);
        if (rp == nullptr) {
            return;
        }
        std::vector<Vertex> tri;
        tri.reserve(src.size() / 4 * 6);
        for (std::size_t q = 0; q + 3 < src.size(); q += 4) {
            tri.push_back(src[q]);
            tri.push_back(src[q + 1]);
            tri.push_back(src[q + 2]);
            tri.push_back(src[q]);
            tri.push_back(src[q + 2]);
            tri.push_back(src[q + 3]);
        }
        std::uint64_t voff = 0;
        std::uint64_t vbytes = 0;
        if (!upload_vertices(tri, &voff, &vbytes)) {
            return;
        }
        std::uint64_t uoff = 0;
        WGPUBindGroup bg = nullptr;
        if (!upload_globals_and_bind(batch_globals_for(bk), bk.view, &uoff, &bg)) {
            return;
        }
        wgpuRenderPassEncoderSetPipeline(pass, rp);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buf, voff, vbytes);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, static_cast<std::uint32_t>(tri.size()), 1, 0, 0);
        wgpuBindGroupRelease(bg);  // 已录制命令内部持有引用

        stats.draw_calls++;
        stats.vertices += static_cast<std::uint32_t>(tri.size());
    }

    // 批 key 快照 → uniform 块（cv4/裁剪 + 管线专用分量；未用分量保持零）。
    [[nodiscard]] auto batch_globals_for(const BatchKey &bk) const -> Globals {
        Globals gu{};
        gu.cv4[0] = logical_w();
        gu.cv4[1] = logical_h();
        gu.clip[0] = bk.clip.on ? bk.clip.rect.origin.x : 0.0F;
        gu.clip[1] = bk.clip.on ? bk.clip.rect.origin.y : 0.0F;
        gu.clip[2] = bk.clip.on ? bk.clip.rect.size.width : 0.0F;
        gu.clip[3] = bk.clip.on ? bk.clip.rect.size.height : 0.0F;
        gu.clip_ctl[0] = bk.clip.radius;
        gu.clip_ctl[1] = bk.clip.on ? 1.0F : 0.0F;
        gu.clip_ctl[2] = bk.clip.aa ? 1.0F : 0.0F;
        gu.shape[0] = bk.shape_cx;
        gu.shape[1] = bk.shape_cy;
        gu.shape[2] = bk.shape_hw;
        gu.shape[3] = bk.shape_hh;
        switch (bk.pipeline) {
            case kBaseBorder:
                gu.ctl[0] = bk.border_radius;
                gu.ctl[1] = bk.border_width;
                break;
            case kBaseGrad:
                gu.grad_ab[0] = bk.grad_ax;
                gu.grad_ab[1] = bk.grad_ay;
                gu.grad_ab[2] = bk.grad_bx;
                gu.grad_ab[3] = bk.grad_by;
                gu.grad_cd[0] = bk.grad_r;
                gu.grad_cd[1] = bk.grad_radial ? 1.0F : 0.0F;
                break;
            case kBaseShadow:
                gu.ctl[2] = bk.shadow_blur;
                break;
            case kBaseImage:
                gu.tex_ctl[0] = bk.pma_in_shader ? 1.0F : 0.0F;
                gu.tex_ctl[1] = bk.nearest_filter ? 1.0F : 0.0F;
                break;
            default:
                break;
        }
        return gu;
    }

    // ---- 效果 pass 即时 quad（不经批系统：直写替换语义，混合管线内建为无）----

    // 效果 pass 即时 quad；失败仅计诊断（无分支消费方），不设 nodiscard。
    bool immediate_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                        int pipe, WGPUTextureView src_view, const Globals &gu) {
        if (pass == nullptr) {
            return false;
        }
        WGPURenderPipeline rp = current_pipeline(pipe);
        if (rp == nullptr) {
            return false;
        }
        const Color c{255, 255, 255, 255};
        const Vertex base[4] = {
            Vertex{x0, y0, u0, v0, c.r, c.g, c.b, c.a},
            Vertex{x1, y0, u1, v0, c.r, c.g, c.b, c.a},
            Vertex{x1, y1, u1, v1, c.r, c.g, c.b, c.a},
            Vertex{x0, y1, u0, v1, c.r, c.g, c.b, c.a},
        };
        std::vector<Vertex> tri;
        tri.reserve(6);
        for (const int idx : {0, 1, 2, 0, 2, 3}) {
            tri.push_back(base[idx]);
        }
        std::uint64_t voff = 0;
        std::uint64_t vbytes = 0;
        if (!upload_vertices(tri, &voff, &vbytes)) {
            return false;
        }
        std::uint64_t uoff = 0;
        WGPUBindGroup bg = nullptr;
        if (!upload_globals_and_bind(gu, src_view, &uoff, &bg)) {
            return false;
        }
        wgpuRenderPassEncoderSetPipeline(pass, rp);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buf, voff, vbytes);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
        wgpuBindGroupRelease(bg);
        stats.draw_calls++;
        stats.vertices += 6;
        return true;
    }

    /// 区域效果共享的物理像素区域换算：floor/ceil + 当前绘制目标钳制（同软件三原语；
    /// 层重定向中区域坐标为层局部，钳制基准 = 层尺寸）。返回 false = 空区域。
    [[nodiscard]] bool effect_region_px(const Rect &region, int &rx0, int &ry0, int &rx1, int &ry1) const {
        const float s = scale > 0.0F ? scale : 1.0F;
        rx0 = std::max(0, static_cast<int>(std::floor(region.origin.x * s)));
        ry0 = std::max(0, static_cast<int>(std::floor(region.origin.y * s)));
        rx1 = std::min(target_w(), static_cast<int>(std::ceil((region.origin.x + region.size.width) * s)));
        ry1 = std::min(target_h(), static_cast<int>(std::ceil((region.origin.y + region.size.height) * s)));
        return rx1 > rx0 && ry1 > ry0;
    }

    // ---- 画布区域效果：两遍 A/B（WebGPU 不可采样 MSAA / 不可读写同纹理的结构性约束）----
    // A：关画布 pass（resolve 落 canvas_）→ alt pass 读 canvas_ 跑 fx（pipe_a）；
    // B：Load 重开画布 pass → fs_copy（pipe_b=kPipeCopy）读 alt 回写区域 → 关 pass。
    // ⚠️ 同帧 queue 写（ring writeBuffer）统一在 submit 前生效：两 pass 读取的纹理内容
    // 均为本帧已提交状态，效果采样前已 flush + 关闭画布 pass，无脏读窗口。

    void canvas_blend_mask(int pipe_fx, const Globals &gu_fx, int rx0, int ry0, int rx1, int ry1) {
        flush_batch();
        close_pass();  // resolve 落地：canvas_ 此刻新鲜
        if (!ensure_alt(device_w, device_h)) {
            return;
        }
        const float s = scale > 0.0F ? scale : 1.0F;
        Globals gu_a = gu_fx;
        gu_a.cv4[0] = static_cast<float>(alt_.width) / s;
        gu_a.cv4[1] = static_cast<float>(alt_.height) / s;
        if (open_pass(alt_, nullptr, true, kPassAlt)) {
            immediate_quad(static_cast<float>(rx0) / s, static_cast<float>(ry0) / s,
                           static_cast<float>(rx1) / s, static_cast<float>(ry1) / s,
                           static_cast<float>(rx0) / static_cast<float>(device_w),
                           static_cast<float>(ry0) / static_cast<float>(device_h),
                           static_cast<float>(rx1) / static_cast<float>(device_w),
                           static_cast<float>(ry1) / static_cast<float>(device_h), pipe_fx, canvas_.view, gu_a);
            close_pass();
        }
        // B：copy 回写只需 vs_main 的 NDC 换算（cv4），其余分量清零、裁剪关闭。
        Globals gu_b{};
        gu_b.cv4[0] = static_cast<float>(device_w) / s;
        gu_b.cv4[1] = static_cast<float>(device_h) / s;
        if (open_pass(msaa_, canvas_.view, false, kPassCanvas)) {
            immediate_quad(static_cast<float>(rx0) / s, static_cast<float>(ry0) / s,
                           static_cast<float>(rx1) / s, static_cast<float>(ry1) / s,
                           static_cast<float>(rx0) / static_cast<float>(alt_.width),
                           static_cast<float>(ry0) / static_cast<float>(alt_.height),
                           static_cast<float>(rx1) / static_cast<float>(alt_.width),
                           static_cast<float>(ry1) / static_cast<float>(alt_.height), kPipeCopy, alt_.view, gu_b);
            close_pass();
        }
    }

    void canvas_blur(int r, int rx0, int ry0, int rx1, int ry1) {
        flush_batch();
        close_pass();
        if (!ensure_alt(device_w, device_h)) {
            return;
        }
        const float s = scale > 0.0F ? scale : 1.0F;
        const auto lx0 = static_cast<float>(rx0) / s;
        const auto ly0 = static_cast<float>(ry0) / s;
        const auto lx1 = static_cast<float>(rx1) / s;
        const auto ly1 = static_cast<float>(ry1) / s;
        auto make_gu = [&](float src_w, float src_h, int dir, float cv4_w, float cv4_h) {
            Globals gu{};
            gu.cv4[0] = cv4_w;
            gu.cv4[1] = cv4_h;
            gu.region[0] = static_cast<float>(rx0);
            gu.region[1] = static_cast<float>(ry0);
            gu.region[2] = static_cast<float>(rx1 - rx0);
            gu.region[3] = static_cast<float>(ry1 - ry0);
            gu.canvas_ctl[0] = src_w;
            gu.canvas_ctl[1] = src_h;
            gu.tex_ctl[2] = static_cast<float>(r);
            gu.tex_ctl[3] = static_cast<float>(dir);
            return gu;
        };
        // A：alt ← 水平（dir0），源 = canvas_。
        if (open_pass(alt_, nullptr, true, kPassAlt)) {
            immediate_quad(lx0, ly0, lx1, ly1, 0.0F, 0.0F, 1.0F, 1.0F, kPipeBlur, canvas_.view,
                           make_gu(static_cast<float>(device_w), static_cast<float>(device_h), 0,
                                   static_cast<float>(alt_.width) / s, static_cast<float>(alt_.height) / s));
            close_pass();
        }
        // B：画布 ← 垂直（dir1），源 = alt。
        if (open_pass(msaa_, canvas_.view, false, kPassCanvas)) {
            immediate_quad(lx0, ly0, lx1, ly1, 0.0F, 0.0F, 1.0F, 1.0F, kPipeBlur, alt_.view,
                           make_gu(static_cast<float>(alt_.width), static_cast<float>(alt_.height), 1,
                                   static_cast<float>(device_w) / s, static_cast<float>(device_h) / s));
            close_pass();
        }
    }

    // ---- 层内效果（目标 = 层 pass，源 = aux 采样拷贝；区域坐标层局部）----

    void layer_blend_mask(int pipe_fx, const Globals &gu_fx, int rx0, int ry0, int rx1, int ry1) {
        LayerEntry *entry = current_layer_entry();
        if (entry == nullptr || entry->tex.tex == nullptr) {
            return;
        }
        if (!copy_layer_to_aux(*entry)) {
            return;
        }
        if (!ensure_target_pass()) {
            return;
        }
        const float s = scale > 0.0F ? scale : 1.0F;
        Globals gu = gu_fx;
        gu.cv4[0] = logical_w();
        gu.cv4[1] = logical_h();
        immediate_quad(static_cast<float>(rx0) / s, static_cast<float>(ry0) / s, static_cast<float>(rx1) / s,
                       static_cast<float>(ry1) / s, static_cast<float>(rx0) / static_cast<float>(entry->aux.width),
                       static_cast<float>(ry0) / static_cast<float>(entry->aux.height),
                       static_cast<float>(rx1) / static_cast<float>(entry->aux.width),
                       static_cast<float>(ry1) / static_cast<float>(entry->aux.height), pipe_fx, entry->aux.view, gu);
        // 层 pass 保持打开（后续层内命令继续重定向）。
    }

    void layer_blur(int r, int rx0, int ry0, int rx1, int ry1) {
        LayerEntry *entry = current_layer_entry();
        if (entry == nullptr || entry->tex.tex == nullptr) {
            return;
        }
        if (!copy_layer_to_aux(*entry)) {
            return;
        }
        if (!ensure_alt(entry->width, entry->height)) {
            (void)ensure_target_pass();
            return;
        }
        const float s = scale > 0.0F ? scale : 1.0F;
        const auto lx0 = static_cast<float>(rx0) / s;
        const auto ly0 = static_cast<float>(ry0) / s;
        const auto lx1 = static_cast<float>(rx1) / s;
        const auto ly1 = static_cast<float>(ry1) / s;
        auto make_gu = [&](float src_w, float src_h, int dir, float cv4_w, float cv4_h) {
            Globals gu{};
            gu.cv4[0] = cv4_w;
            gu.cv4[1] = cv4_h;
            gu.region[0] = static_cast<float>(rx0);
            gu.region[1] = static_cast<float>(ry0);
            gu.region[2] = static_cast<float>(rx1 - rx0);
            gu.region[3] = static_cast<float>(ry1 - ry0);
            gu.canvas_ctl[0] = src_w;
            gu.canvas_ctl[1] = src_h;
            gu.tex_ctl[2] = static_cast<float>(r);
            gu.tex_ctl[3] = static_cast<float>(dir);
            return gu;
        };
        // A：alt ← 水平（dir0），源 = aux（层尺寸）。
        if (open_pass(alt_, nullptr, true, kPassAlt)) {
            immediate_quad(lx0, ly0, lx1, ly1, 0.0F, 0.0F, 1.0F, 1.0F, kPipeBlur, entry->aux.view,
                           make_gu(static_cast<float>(entry->aux.width), static_cast<float>(entry->aux.height), 0,
                                   static_cast<float>(alt_.width) / s, static_cast<float>(alt_.height) / s));
            close_pass();
        }
        // B：层 pass（Load 重开）← 垂直（dir1），源 = alt。
        if (!ensure_target_pass()) {
            return;
        }
        immediate_quad(lx0, ly0, lx1, ly1, 0.0F, 0.0F, 1.0F, 1.0F, kPipeBlur, alt_.view,
                       make_gu(static_cast<float>(alt_.width), static_cast<float>(alt_.height), 1, logical_w(),
                               logical_h()));
        // 层 pass 保持打开。
    }

    // ---- 命令翻译（23 条 CmdKind，逐条对照 gpu_gl_rhi.cpp::translate）----

    void translate(const DrawCmd &cmd, const CmdData &data) {
        switch (cmd.kind) {
            case CmdKind::FillRect: {
                BatchKey k{};
                k.pipeline = kBaseSolid;
                k.clip = effective_clip();
                begin_batch(k);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(cmd.color, alpha));
                break;
            }
            case CmdKind::ClearRect: {
                // 语义：区域归零（RGBA 全零，不走混合、不受裁剪/alpha 影响）——镜像 Painter::clear_rect。
                BatchKey k{};
                k.pipeline = kBaseSolid;
                k.clip = ClipState{};  // 明确关闭裁剪
                k.blend_off = true;
                begin_batch(k);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, Color{0, 0, 0, 0});
                break;
            }
            case CmdKind::DrawRect: {
                // 1px 内缩边框：与 Painter::draw_rect 的四条 fill_rect 逐边对齐。
                BatchKey k{};
                k.pipeline = kBaseSolid;
                k.clip = effective_clip();
                begin_batch(k);
                const float x0 = cmd.bounds.origin.x;
                const float y0 = cmd.bounds.origin.y;
                const float w = cmd.bounds.size.width;
                const float h = cmd.bounds.size.height;
                const Color c = bake_alpha(cmd.color, alpha);
                push_quad(x0, y0, x0 + w, y0 + 1.0F, c);
                push_quad(x0, y0 + std::max(0.0F, h - 1.0F), x0 + w, y0 + h, c);
                push_quad(x0, y0, x0 + 1.0F, y0 + h, c);
                push_quad(x0 + std::max(0.0F, w - 1.0F), y0, x0 + w, y0 + h, c);
                break;
            }
            case CmdKind::DrawLine: {
                if (cmd.f0 <= 0.0F || cmd.color.a == 0) {
                    break;
                }
                const float dx = cmd.pt1.x - cmd.pt0.x;
                const float dy = cmd.pt1.y - cmd.pt0.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4F) {
                    break;
                }
                const float ux = dx / len;
                const float uy = dy / len;
                const float nx = -uy * cmd.f0 * 0.5F;
                const float ny = ux * cmd.f0 * 0.5F;
                // 方头端帽：两端各延伸半宽（近似软件 AA 线段包围盒，容差覆盖）
                const float ax = cmd.pt0.x - ux * cmd.f0 * 0.5F;
                const float ay = cmd.pt0.y - uy * cmd.f0 * 0.5F;
                const float bx = cmd.pt1.x + ux * cmd.f0 * 0.5F;
                const float by = cmd.pt1.y + uy * cmd.f0 * 0.5F;
                BatchKey k{};
                k.pipeline = kBaseSolid;
                k.clip = effective_clip();
                begin_batch(k);
                const Color c = bake_alpha(cmd.color, alpha);
                const Vertex base[4] = {
                    Vertex{ax + nx, ay + ny, 0.0F, 0.0F, c.r, c.g, c.b, c.a},
                    Vertex{bx + nx, by + ny, 1.0F, 0.0F, c.r, c.g, c.b, c.a},
                    Vertex{bx - nx, by - ny, 1.0F, 1.0F, c.r, c.g, c.b, c.a},
                    Vertex{ax - nx, ay - ny, 0.0F, 1.0F, c.r, c.g, c.b, c.a},
                };
                verts.insert(verts.end(), base, base + 4);
                break;
            }
            case CmdKind::Polyline: {
                if (data.points == nullptr || data.points->size() < 2 || cmd.f0 <= 0.0F || cmd.color.a == 0) {
                    break;
                }
                const float hw = cmd.f0 * 0.5F;
                BatchKey k{};
                k.pipeline = kBaseSolid;
                k.clip = effective_clip();
                begin_batch(k);
                const Color c = bake_alpha(cmd.color, alpha);
                // 首版降级：逐段方头 quad（与 DrawLine 同形）+ 顶点方形帽近似软件的 round
                // join/cap（半透明系列呈串珠）——GPU 与软件的逐位对齐由「容差 golden」阶梯
                // 收口，本波只保证结构正确（同 GL 路径口径）。
                const std::vector<Point> &pts = *data.points;
                for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                    const float dx = pts[i + 1].x - pts[i].x;
                    const float dy = pts[i + 1].y - pts[i].y;
                    const float len = std::sqrt((dx * dx) + (dy * dy));
                    if (len < 1e-4F) {
                        continue;
                    }
                    const float ux = dx / len;
                    const float uy = dy / len;
                    const float nx = -uy * hw;
                    const float ny = ux * hw;
                    const float ax = pts[i].x - (ux * hw);
                    const float ay = pts[i].y - (uy * hw);
                    const float bx = pts[i + 1].x + (ux * hw);
                    const float by = pts[i + 1].y + (uy * hw);
                    const Vertex quad[4] = {
                        Vertex{ax + nx, ay + ny, 0.0F, 0.0F, c.r, c.g, c.b, c.a},
                        Vertex{bx + nx, by + ny, 1.0F, 0.0F, c.r, c.g, c.b, c.a},
                        Vertex{bx - nx, by - ny, 1.0F, 1.0F, c.r, c.g, c.b, c.a},
                        Vertex{ax - nx, ay - ny, 0.0F, 1.0F, c.r, c.g, c.b, c.a},
                    };
                    verts.insert(verts.end(), quad, quad + 4);
                }
                for (const Point &pt : pts) {
                    push_quad(pt.x - hw, pt.y - hw, pt.x + hw, pt.y + hw, c);
                }
                break;
            }
            case CmdKind::Sector: {
                constexpr float TWO_PI = 6.28318530717958647692F;
                if (cmd.f0 <= 0.0F || cmd.color.a == 0) {
                    break;
                }
                const float sweep = std::min(cmd.f3 - cmd.f2, TWO_PI);
                const float outer = cmd.f0;
                const float inner = std::max(0.0F, cmd.f1);
                if (sweep <= 0.0F || inner >= outer) {
                    break;
                }
                BatchKey k{};
                k.pipeline = kBaseSolid;
                k.clip = effective_clip();
                begin_batch(k);
                const Color c = bake_alpha(cmd.color, alpha);
                // 首版降级：（环）扇按角度细分为梯形 quad，以弦逼近弧（无 SDF 羽化）。
                constexpr float STEP = 0.12F;  // 每段弧度（≈6.9°），弦误差 < 外径的 0.1%
                const int slices = std::clamp(static_cast<int>(std::ceil(sweep / STEP)), 8, 256);
                for (int i = 0; i < slices; ++i) {
                    const float t0 = cmd.f2 + (sweep * static_cast<float>(i)) / static_cast<float>(slices);
                    const float t1 = cmd.f2 + (sweep * static_cast<float>(i + 1)) / static_cast<float>(slices);
                    const float c0 = std::cos(t0);
                    const float s0 = std::sin(t0);
                    const float c1 = std::cos(t1);
                    const float s1 = std::sin(t1);
                    const Vertex quad[4] = {
                        Vertex{cmd.pt0.x + (c0 * inner), cmd.pt0.y + (s0 * inner), 0.0F, 0.0F, c.r, c.g, c.b, c.a},
                        Vertex{cmd.pt0.x + (c0 * outer), cmd.pt0.y + (s0 * outer), 1.0F, 0.0F, c.r, c.g, c.b, c.a},
                        Vertex{cmd.pt0.x + (c1 * outer), cmd.pt0.y + (s1 * outer), 1.0F, 1.0F, c.r, c.g, c.b, c.a},
                        Vertex{cmd.pt0.x + (c1 * inner), cmd.pt0.y + (s1 * inner), 0.0F, 1.0F, c.r, c.g, c.b, c.a},
                    };
                    verts.insert(verts.end(), quad, quad + 4);
                }
                break;
            }
            case CmdKind::RoundedBorder: {
                if (cmd.f1 <= 0.0F || cmd.color.a == 0 || cmd.bounds.size.width <= 0.0F
                    || cmd.bounds.size.height <= 0.0F) {
                    break;
                }
                BatchKey k{};
                k.pipeline = kBaseBorder;
                k.clip = effective_clip();
                const float radius =
                    std::min(cmd.f0, std::min(cmd.bounds.size.width, cmd.bounds.size.height) * 0.5F);
                k.border_radius = radius;
                k.border_width = cmd.f1;
                k.shape_cx = cmd.bounds.origin.x + cmd.bounds.size.width * 0.5F;
                k.shape_cy = cmd.bounds.origin.y + cmd.bounds.size.height * 0.5F;
                k.shape_hw = cmd.bounds.size.width * 0.5F;
                k.shape_hh = cmd.bounds.size.height * 0.5F;
                begin_batch(k);
                // 外扩 1px 容纳外侧羽化带
                push_quad(cmd.bounds.origin.x - 1.0F, cmd.bounds.origin.y - 1.0F,
                          cmd.bounds.origin.x + cmd.bounds.size.width + 1.0F,
                          cmd.bounds.origin.y + cmd.bounds.size.height + 1.0F, bake_alpha(cmd.color, alpha));
                break;
            }
            case CmdKind::PushClip: {
                flush_batch();
                ClipState cur = effective_clip();
                ClipState next;
                next.on = true;
                next.rect = cur.on ? intersect(cur.rect, cmd.bounds) : cmd.bounds;
                next.radius = 0.0F;
                next.aa = true;
                clip_stack.push_back(next);
                break;
            }
            case CmdKind::PushClipRounded: {
                flush_batch();
                ClipState cur = effective_clip();
                ClipState next;
                next.on = true;
                next.rect = cur.on ? intersect(cur.rect, cmd.bounds) : cmd.bounds;
                next.radius = std::min(cmd.f0, cur.on ? cur.radius : cmd.f0);
                next.aa = cmd.rounded_aa && (cur.on ? cur.aa : true);
                clip_stack.push_back(next);
                break;
            }
            case CmdKind::PopClip: {
                flush_batch();
                if (!clip_stack.empty()) {
                    clip_stack.pop_back();
                }
                break;
            }
            case CmdKind::SetAlpha:
                alpha = cmd.alpha;
                break;
            case CmdKind::BeginLayer: {
                // GPU 层缓存（specification/03 §8.7）：重定向到常驻层纹理。层键缺席 /
                // 尺寸变化时（重）分配；随后清空层内裁剪栈（层局部坐标），全局 alpha 保持
                // （子树命令自带绝对 SetAlpha，与 DL 语义一致）。
                const std::uint64_t lkey = cmd.aux_key;
                if (lkey == 0) {
                    break;
                }
                const float s = scale > 0.0F ? scale : 1.0F;
                const int lw = std::max(1, static_cast<int>(std::lround(cmd.bounds.size.width * s)));
                const int lh = std::max(1, static_cast<int>(std::lround(cmd.bounds.size.height * s)));
                flush_batch();
                close_pass();  // 父目标内容落地；后续 flush 按需 Load 重开
                LayerEntry &entry = layer_cache[lkey];
                if (entry.width != lw || entry.height != lh) {
                    if (!create_layer_attachment(&entry, lw, lh)) {
                        layer_cache.erase(lkey);
                        break;
                    }
                }
                LayerFrame frame;
                frame.key = lkey;
                frame.saved_clip = clip_stack;
                frame.saved_alpha = alpha;
                frame.width = lw;
                frame.height = lh;
                frame.logical_w = cmd.bounds.size.width;
                frame.logical_h = cmd.bounds.size.height;
                layer_stack.push_back(std::move(frame));
                // Clear 层内零基底；失败由后续 flush 的 ensure_target_pass 兜底重开。
                (void)open_pass(entry.tex, nullptr, true, kPassLayer);
                clip_stack.clear();  // 层局部坐标：外部裁剪不带入（合成时经 DrawLayer 批裁剪）
                break;
            }
            case CmdKind::EndLayer: {
                // 层内容定稿（pass 关闭即提交），恢复重定向前的目标与状态（裁剪栈 / alpha）。
                if (layer_stack.empty()) {
                    break;  // 防御：不配对的 EndLayer
                }
                flush_batch();
                close_pass();
                LayerFrame frame = std::move(layer_stack.back());
                layer_stack.pop_back();
                clip_stack = std::move(frame.saved_clip);
                alpha = frame.saved_alpha;
                break;
            }
            case CmdKind::DrawLayer: {
                // 层合成：常驻层纹理按放置矩阵上屏（直色 src-over 语义 + NEAREST 采样）。
                const auto it = layer_cache.find(cmd.aux_key);
                if (it == layer_cache.end() || it->second.tex.tex == nullptr) {
                    // 冷存储未命中（后端重建等）：本帧跳过并整体失效层代际，下帧重录自愈。
                    if (!layer_miss_warned) {
                        AURORA_LOG_WARN("gpu-wgpu", "DrawLayer 未命中常驻层纹理，本帧跳过（下帧重录）");
                        layer_miss_warned = true;
                    }
                    render::detail::bump_gpu_layer_epoch();
                    break;
                }
                const LayerEntry &entry = it->second;
                const Matrix2D identity_mat{};
                const Matrix2D &mat = data.matrix != nullptr ? *data.matrix : identity_mat;
                const float src_scale = cmd.composite_scale > 0.0F ? cmd.composite_scale : 1.0F;
                const float lw = static_cast<float>(entry.width) / src_scale;
                const float lh = static_cast<float>(entry.height) / src_scale;
                BatchKey k{};
                k.pipeline = kBaseImage;  // blend_pma=false：层内容为直色 src-over
                k.clip = effective_clip();
                k.nearest_filter = true;  // 与软件位图 floor 采样同语义
                k.view = entry.tex.view;
                begin_batch(k);
                // 仿射矩阵直烘进四角顶点（与 Composite 同构）；uv = 层逻辑角点归一化。
                const Point c0 = mat.apply_to_point(Point{.x = 0.0F, .y = 0.0F});
                const Point c1 = mat.apply_to_point(Point{.x = lw, .y = 0.0F});
                const Point c2 = mat.apply_to_point(Point{.x = lw, .y = lh});
                const Point c3 = mat.apply_to_point(Point{.x = 0.0F, .y = lh});
                const Color c = bake_alpha(Color{255, 255, 255, 255}, alpha);
                const Vertex quad[4] = {
                    Vertex{c0.x, c0.y, 0.0F, 0.0F, c.r, c.g, c.b, c.a},
                    Vertex{c1.x, c1.y, 1.0F, 0.0F, c.r, c.g, c.b, c.a},
                    Vertex{c2.x, c2.y, 1.0F, 1.0F, c.r, c.g, c.b, c.a},
                    Vertex{c3.x, c3.y, 0.0F, 1.0F, c.r, c.g, c.b, c.a},
                };
                verts.insert(verts.end(), quad, quad + 4);
                break;
            }
            case CmdKind::DrawText: {
                // 与 software_rhi 同形的数据契约；缺文本/字体直接跳过。空色不绘。
                if (data.text == nullptr || data.font == nullptr || data.text->empty() || cmd.color.a == 0) {
                    break;
                }
                const render::TextLayoutOpts opts{
                    .letter_spacing = cmd.text_ls, .word_spacing = cmd.text_ws, .italic = cmd.text_italic};
                // 软件路径同源：FontEngine 全程物理像素语义；DrawCmd.bounds 为逻辑 dp，原点先换算。
                const float origin_x = cmd.bounds.origin.x * scale;
                const float origin_y = cmd.bounds.origin.y * scale;
                const ClipState clip = effective_clip();
                // aa 恒 Supersample：GPU v1 容差决策——LCD 子像素降级灰度（同 GL）。
                const bool ok = render::emit_text_glyphs(
                    *data.text, *data.font, opts, scale, render::TextAAMode::Supersample, cmd.color, origin_x,
                    origin_y,
                    [this, &clip, &cmd](const render::GlyphAtlas::Entry &entry, render::GlyphAtlas::Mode mode,
                                        int dx0, int dy0, std::uint64_t key) {
                        if (mode != render::GlyphAtlas::Mode::Gray) {
                            return;  // 防御：GPU 路径恒灰度
                        }
                        // 取槽位可能触发页新建/淘汰（不 flush：wgpu 引用保活），须在
                        // begin_batch 之前完成。
                        const GlyphSlotRect slot = acquire_glyph_slot(key, entry);
                        if (slot.w <= 0 || slot.h <= 0) {
                            return;
                        }
                        BatchKey k{};
                        k.pipeline = kBaseText;
                        k.clip = clip;
                        k.view = slot.view;  // 槽位所在页纹理（跨页文本自然断批）
                        begin_batch(k);
                        // 顶点坐标：物理像素 → 逻辑 dp（NDC 映射基准）；uv = 槽位预归一化矩形。
                        const float inv_s = 1.0F / scale;
                        const float x0 = static_cast<float>(dx0) * inv_s;
                        const float y0 = static_cast<float>(dy0) * inv_s;
                        push_quad_uv(x0, y0, x0 + static_cast<float>(slot.w) * inv_s,
                                     y0 + static_cast<float>(slot.h) * inv_s, slot.u0, slot.v0, slot.u1, slot.v1,
                                     bake_alpha(cmd.color, alpha));
                    });
                (void)ok;  // 无字体面（引擎恒有内置字体，理论不触发）：GPU 路径无位图兜底，跳过
                break;
            }
            case CmdKind::DrawImage: {
                // 软件端契约：空像素 / 非法维度 / 缓冲不足 w*h*4 直接返回。
                if (data.image == nullptr) {
                    break;
                }
                const Image &img = *data.image;
                if (img.pixels.empty() || img.width <= 0 || img.height <= 0) {
                    break;
                }
                if (img.pixels.size()
                    < static_cast<std::uint64_t>(img.width) * static_cast<std::uint64_t>(img.height) * 4U) {
                    break;
                }
                // 常驻流式通道（specification/03 §8.7）：stream_key != 0 走固定纹理槽复用 +
                // 版本增量上传（无 PMA 全帧副本、不参与通用缓存淘汰）；版本未变零上传。
                if (img.stream_key != 0) {
                    StreamSlot *slot = ensure_stream_slot(img.stream_key, img.width, img.height);
                    if (slot == nullptr) {
                        break;
                    }
                    if (slot->version != img.stream_version) {
                        flush_batch();  // 整帧重传前行 0 与已录制批的读危险：先落地引用旧内容的批
                        if (!write_tex_sub(slot->tex, 0, 0, img.width, img.height, img.pixels.data(), 0, 4)) {
                            break;
                        }
                        slot->version = img.stream_version;
                    }
                    BatchKey k{};
                    k.pipeline = kBaseImage;
                    k.clip = effective_clip();
                    k.blend_pma = true;      // 输出 PMA 语义，混合同静态图
                    k.pma_in_shader = true;  // 直色纹理：PMA 下沉到片元（上传期无 CPU 预乘）
                    k.view = slot->tex.view;
                    begin_batch(k);
                    push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y,
                              cmd.bounds.origin.x + cmd.bounds.size.width,
                              cmd.bounds.origin.y + cmd.bounds.size.height,
                              bake_alpha(Color{255, 255, 255, 255}, alpha));
                    break;
                }
                const WGPUTextureView tex = acquire_image_tex(img);
                if (tex == nullptr) {
                    break;
                }
                BatchKey k{};
                k.pipeline = kBaseImage;
                k.clip = effective_clip();
                k.blend_pma = true;
                k.view = tex;
                begin_batch(k);
                // 片元色取自 PMA 纹理；顶点色只承载全局 alpha（uv 0..1 全图映射）。
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(Color{255, 255, 255, 255}, alpha));
                break;
            }
            case CmdKind::LinearGradient: {
                // 软件端契约：空色标直接返回（不绘制）。
                if (data.colors == nullptr || data.stops == nullptr || data.colors->empty() || data.stops->empty()) {
                    break;
                }
                const float ax = cmd.pt0.x;
                const float ay = cmd.pt0.y;
                const float dx = cmd.pt1.x - ax;
                const float dy = cmd.pt1.y - ay;
                // 软件端退化阈值：物理像素 len_sq < 0.001 → fill_rect(首色)（走实心管线）；
                // 此处同形翻译，scale² 把逻辑长度折算到物理域。
                const float len_sq_phys = (dx * dx + dy * dy) * scale * scale;
                if (len_sq_phys < 0.001F) {
                    BatchKey k{};
                    k.pipeline = kBaseSolid;
                    k.clip = effective_clip();
                    begin_batch(k);
                    push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y,
                              cmd.bounds.origin.x + cmd.bounds.size.width,
                              cmd.bounds.origin.y + cmd.bounds.size.height,
                              bake_alpha(data.colors->front(), alpha));
                    break;
                }
                const WGPUTextureView lut = acquire_lut(*data.colors, *data.stops);
                if (lut == nullptr) {
                    break;
                }
                BatchKey k{};
                k.pipeline = kBaseGrad;
                k.clip = effective_clip();
                k.grad_ax = ax;
                k.grad_ay = ay;
                k.grad_bx = cmd.pt1.x;
                k.grad_by = cmd.pt1.y;
                k.view = lut;
                begin_batch(k);
                // 片元色取自 LUT；顶点色只承载全局 alpha（RGB 填白仅为可读性）。
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(Color{255, 255, 255, 255}, alpha));
                break;
            }
            case CmdKind::RadialGradient: {
                // 软件端契约：空色标 / radius<=0 直接返回。
                if (data.colors == nullptr || data.stops == nullptr || data.colors->empty() || data.stops->empty()
                    || cmd.f0 <= 0.0F) {
                    break;
                }
                const WGPUTextureView lut = acquire_lut(*data.colors, *data.stops);
                if (lut == nullptr) {
                    break;
                }
                BatchKey k{};
                k.pipeline = kBaseGrad;
                k.clip = effective_clip();
                k.grad_radial = true;
                k.grad_ax = cmd.pt0.x;
                k.grad_ay = cmd.pt0.y;
                k.grad_r = cmd.f0;
                k.view = lut;
                begin_batch(k);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(Color{255, 255, 255, 255}, alpha));
                break;
            }
            case CmdKind::Shadow: {
                // 软件契约：blur ≤ 0 硬阴影 = 偏移矩形实心填充；模糊阴影 = 单个 Shadow quad
                // 覆盖扩展区（内部 dist=0 → 因子 1 = fill，外部线性衰减），扩展区 = blur×2。
                const Rect shadow_rect{
                    .origin = Point{.x = cmd.bounds.origin.x + cmd.f0, .y = cmd.bounds.origin.y + cmd.f1},
                    .size = cmd.bounds.size};
                if (cmd.f2 <= 0.0F) {
                    BatchKey k{};
                    k.pipeline = kBaseSolid;
                    k.clip = effective_clip();
                    begin_batch(k);
                    push_quad(shadow_rect.origin.x, shadow_rect.origin.y,
                              shadow_rect.origin.x + shadow_rect.size.width,
                              shadow_rect.origin.y + shadow_rect.size.height, bake_alpha(cmd.color, alpha));
                    break;
                }
                const float expand = cmd.f2 * 2.0F;
                BatchKey k{};
                k.pipeline = kBaseShadow;
                k.clip = effective_clip();
                k.shape_cx = shadow_rect.origin.x + shadow_rect.size.width * 0.5F;
                k.shape_cy = shadow_rect.origin.y + shadow_rect.size.height * 0.5F;
                k.shape_hw = shadow_rect.size.width * 0.5F;
                k.shape_hh = shadow_rect.size.height * 0.5F;
                k.shadow_blur = cmd.f2;
                begin_batch(k);
                push_quad(shadow_rect.origin.x - expand, shadow_rect.origin.y - expand,
                          shadow_rect.origin.x + shadow_rect.size.width + expand,
                          shadow_rect.origin.y + shadow_rect.size.height + expand,
                          bake_alpha(cmd.color, alpha));
                break;
            }
            case CmdKind::BlurRegion: {
                // 软件契约：radius ≤ 0 直接返回；区域物理像素换算同软件（floor/ceil + 目标
                // 钳制），半径 r = max(1, trunc(radius × scale))。两遍分离 box blur（水平 →
                // 垂直），tap 钳制在区域内（毛玻璃不漏采区外），回写直写替换。
                if (cmd.f0 <= 0.0F) {
                    break;
                }
                int rx0 = 0;
                int ry0 = 0;
                int rx1 = 0;
                int ry1 = 0;
                if (!effect_region_px(cmd.bounds, rx0, ry0, rx1, ry1)) {
                    break;
                }
                const float s = scale > 0.0F ? scale : 1.0F;
                const int r = std::max(1, static_cast<int>(cmd.f0 * s));
                if (!layer_stack.empty()) {
                    layer_blur(r, rx0, ry0, rx1, ry1);  // aux → alt（dir0）→ 层（dir1）
                } else {
                    canvas_blur(r, rx0, ry0, rx1, ry1);  // canvas resolve → alt（dir0）→ 画布（dir1）
                }
                break;
            }
            case CmdKind::BlendRegion: {
                // 软件契约：strength 截断到 [0,1]，≤ 0 直接返回；只改 RGB，alpha 原样保留。
                // 枚举序与 fs_blend 的 mode 分支一一对应（BlendMode Normal..Exclusion = 0..7）。
                const float strength = cmd.f0 < 0.0F ? 0.0F : (cmd.f0 > 1.0F ? 1.0F : cmd.f0);
                if (strength <= 0.0F) {
                    break;
                }
                int rx0 = 0;
                int ry0 = 0;
                int rx1 = 0;
                int ry1 = 0;
                if (!effect_region_px(cmd.bounds, rx0, ry0, rx1, ry1)) {
                    break;
                }
                Globals gu{};
                gu.ctl[3] = static_cast<float>(static_cast<int>(cmd.blend_mode));
                gu.fx[0] = static_cast<float>(cmd.color.r) / 255.0F;
                gu.fx[1] = static_cast<float>(cmd.color.g) / 255.0F;
                gu.fx[2] = static_cast<float>(cmd.color.b) / 255.0F;
                gu.fx[3] = strength;
                if (!layer_stack.empty()) {
                    layer_blend_mask(kPipeBlend, gu, rx0, ry0, rx1, ry1);
                } else {
                    canvas_blend_mask(kPipeBlend, gu, rx0, ry0, rx1, ry1);
                }
                break;
            }
            case CmdKind::MaskRegion: {
                // 软件契约：strength 截断到 [0,1]，≤ 0 直接返回；RGB 乘渐变因子（基于区域内
                // 设备像素索引，radial 中心/最大半径按区域尺寸），alpha 不变。
                const float strength = cmd.f0 < 0.0F ? 0.0F : (cmd.f0 > 1.0F ? 1.0F : cmd.f0);
                if (strength <= 0.0F) {
                    break;
                }
                int rx0 = 0;
                int ry0 = 0;
                int rx1 = 0;
                int ry1 = 0;
                if (!effect_region_px(cmd.bounds, rx0, ry0, rx1, ry1)) {
                    break;
                }
                Globals gu{};
                gu.ctl[2] = static_cast<float>(static_cast<int>(cmd.mask_kind));
                gu.fx[3] = strength;
                gu.region[0] = static_cast<float>(rx0);
                gu.region[1] = static_cast<float>(ry0);
                gu.region[2] = static_cast<float>(rx1 - rx0);
                gu.region[3] = static_cast<float>(ry1 - ry0);
                if (!layer_stack.empty()) {
                    layer_blend_mask(kPipeMask, gu, rx0, ry0, rx1, ry1);
                } else {
                    canvas_blend_mask(kPipeMask, gu, rx0, ry0, rx1, ry1);
                }
                break;
            }
            case CmdKind::Composite: {
                // 软件契约（composite_pixels 前置校验同形）：空像素 / 非法维度 / 缓冲不足
                // 直接返回。仿射矩阵直烘进四角顶点；uv = 源逻辑角点归一化——NEAREST 采样下
                // texel = floor(uv × 尺寸)，与软件逆映射逐像素 floor 取样同构；PMA 直色。
                if (data.image == nullptr) {
                    break;
                }
                const Image &img = *data.image;
                if (img.pixels.empty() || img.width <= 0 || img.height <= 0) {
                    break;
                }
                if (img.pixels.size()
                    < static_cast<std::uint64_t>(img.width) * static_cast<std::uint64_t>(img.height) * 4U) {
                    break;
                }
                const WGPUTextureView tex = acquire_image_tex(img);
                if (tex == nullptr) {
                    break;
                }
                const Matrix2D identity{};
                const Matrix2D &mat = data.matrix != nullptr ? *data.matrix : identity;
                const float src_scale = cmd.composite_scale > 0.0F ? cmd.composite_scale : 1.0F;
                const float lw = static_cast<float>(img.width) / src_scale;
                const float lh = static_cast<float>(img.height) / src_scale;
                BatchKey k{};
                k.pipeline = kBaseImage;
                k.clip = effective_clip();
                k.blend_pma = true;
                k.view = tex;
                k.nearest_filter = true;
                begin_batch(k);
                const Point c0 = mat.apply_to_point(Point{.x = 0.0F, .y = 0.0F});
                const Point c1 = mat.apply_to_point(Point{.x = lw, .y = 0.0F});
                const Point c2 = mat.apply_to_point(Point{.x = lw, .y = lh});
                const Point c3 = mat.apply_to_point(Point{.x = 0.0F, .y = lh});
                const Color c = bake_alpha(Color{255, 255, 255, 255}, alpha);
                const Vertex quad[4] = {
                    Vertex{c0.x, c0.y, 0.0F, 0.0F, c.r, c.g, c.b, c.a},
                    Vertex{c1.x, c1.y, 1.0F, 0.0F, c.r, c.g, c.b, c.a},
                    Vertex{c2.x, c2.y, 1.0F, 1.0F, c.r, c.g, c.b, c.a},
                    Vertex{c3.x, c3.y, 0.0F, 1.0F, c.r, c.g, c.b, c.a},
                };
                verts.insert(verts.end(), quad, quad + 4);
                break;
            }
        }
    }

    // ---- present：画布 resolve 纹理 → swapchain 全屏 quad（fs_copy 直贴）----

    void draw_present() {
        if (frame_view == nullptr || !ensure_present_pipeline(surface_format)) {
            return;
        }
        // canvas_ 行 0 = 逻辑 y = 0 = NDC +1：uv 自上而下直贴，无翻转。
        const Vertex quad[4] = {
            Vertex{-1.0F, 1.0F, 0.0F, 0.0F, 255, 255, 255, 255},
            Vertex{1.0F, 1.0F, 1.0F, 0.0F, 255, 255, 255, 255},
            Vertex{1.0F, -1.0F, 1.0F, 1.0F, 255, 255, 255, 255},
            Vertex{-1.0F, -1.0F, 0.0F, 1.0F, 255, 255, 255, 255},
        };
        const Vertex tri[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};
        const std::uint64_t vbytes = sizeof(tri);
        const std::uint64_t voff = vstage.size();
        if (!grow_vertex_buffer(voff + vbytes)) {
            return;
        }
        wgpuQueueWriteBuffer(queue, vertex_buf, voff, tri, sizeof(tri));
        vstage.insert(vstage.end(), reinterpret_cast<const std::uint8_t *>(tri),
                      reinterpret_cast<const std::uint8_t *>(tri) + sizeof(tri));
        Globals gu{};  // vs_present 不用 cv4；fs_copy 只用 uv
        std::uint64_t uoff = 0;
        WGPUBindGroup bg = nullptr;
        if (!upload_globals_and_bind(gu, canvas_.view, &uoff, &bg)) {
            return;
        }
        WGPURenderPassColorAttachment att{};
        att.view = frame_view;
        att.resolveTarget = nullptr;
        att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        att.loadOp = WGPULoadOp_Clear;
        att.storeOp = WGPUStoreOp_Store;
        att.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
        WGPURenderPassDescriptor pd{};
        pd.colorAttachmentCount = 1;
        pd.colorAttachments = &att;
        WGPURenderPassEncoder ppass = wgpuCommandEncoderBeginRenderPass(encoder, &pd);
        if (ppass == nullptr) {
            wgpuBindGroupRelease(bg);
            return;
        }
        wgpuRenderPassEncoderSetPipeline(ppass, pipe_present_);
        wgpuRenderPassEncoderSetVertexBuffer(ppass, 0, vertex_buf, voff, vbytes);
        wgpuRenderPassEncoderSetBindGroup(ppass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(ppass, 6, 1, 0, 0);
        wgpuRenderPassEncoderEnd(ppass);
        wgpuRenderPassEncoderRelease(ppass);
        wgpuBindGroupRelease(bg);
        stats.draw_calls++;
        stats.vertices += 6;
    }

    // ---- 帧生命周期 ----

    [[nodiscard]] bool begin_frame(int device_width, int device_height, float scale_) {
        if (!device_ok || device_width <= 0 || device_height <= 0) {
            return false;
        }
        // 上一帧读回若未被消费（宿主跳过 read_pixels），先解除滞留映射再复用缓冲。
        if (readback_mapped) {
            wgpuBufferUnmap(readback);
            readback_mapped = false;
            map_armed = false;
            map_done = false;
        }
        stats = {};
        clip_stack.clear();
        layer_stack.clear();  // 防御：上帧不配对 BeginLayer 残留
        alpha = 1.0;
        key_active = false;
        key = BatchKey{};
        verts.clear();
        vstage.clear();
        ustage.clear();
        scale = scale_ > 0.0F ? scale_ : 1.0F;
        map_armed = false;
        map_done = false;

        if (surface != nullptr) {
            if (!surface_configured || surface_cw != device_width || surface_ch != device_height) {
                if (!configure_surface(device_width, device_height)) {
                    return false;
                }
            }
            WGPUSurfaceTexture st{};
            wgpuSurfaceGetCurrentTexture(surface, &st);
            if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated
                || st.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
                // 尺寸/设备变化：重配一次再取；仍失败则本帧放弃。
                if (st.texture != nullptr) {
                    wgpuTextureRelease(st.texture);  // Outdated 亦回纹理：先弃再重取
                }
                surface_configured = false;
                if (!configure_surface(device_width, device_height)) {
                    return false;
                }
                wgpuSurfaceGetCurrentTexture(surface, &st);
            }
            switch (st.status) {
                case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
                case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
                    break;
                case WGPUSurfaceGetCurrentTextureStatus_Lost:
                    device_ok = false;  // 设备丢失级：永久回退（契约语义）
                    return false;
                default:
                    return false;
            }
            if (st.texture == nullptr) {
                return false;
            }
            AURORA_WGPU_RELEASE(frame_view, wgpuTextureViewRelease)
            AURORA_WGPU_RELEASE(frame_tex, wgpuTextureRelease)  // 上帧滞留引用（防御：end_frame 未跑）
            frame_view = wgpuTextureCreateView(st.texture, nullptr);
            frame_tex = st.texture;  // 本体引用持到 end_frame submit 后释放（见成员注释）
            if (frame_view == nullptr) {
                return false;
            }
        }
        // 画布三件套与帧同尺寸（离屏/宿主统一渲染目标；present 另走上屏 pass）。
        if (!ensure_canvas_targets(device_width, device_height)) {
            return false;
        }

        encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        if (encoder == nullptr) {
            return false;
        }
        // 帧零基底：整帧清透明（与软件 begin 后零基底同源；窗口底色由 DL 内 FillRect 承担）。
        if (!open_pass(msaa_, canvas_.view, true, kPassCanvas)) {
            AURORA_WGPU_RELEASE(encoder, wgpuCommandEncoderRelease)
            return false;
        }
        frame_open = true;
        return true;
    }

    void end_frame() {
        if (!device_ok || !frame_open) {
            return;
        }
        flush_batch();
        close_pass();  // 最终 resolve：canvas_ 纹理此刻承载整帧内容

        if (surface == nullptr) {
            arm_readback();  // 离屏诊断通道：帧尾拷贝，read_pixels 处等待 map
        } else {
            draw_present();
        }

        WGPUCommandBuffer cmdbuf = wgpuCommandEncoderFinish(encoder, nullptr);
        AURORA_WGPU_RELEASE(encoder, wgpuCommandEncoderRelease)
        frame_open = false;
        if (cmdbuf == nullptr) {
            return;
        }
        wgpuQueueSubmit(queue, 1, &cmdbuf);
        wgpuCommandBufferRelease(cmdbuf);
        if (surface == nullptr) {
            map_readback();  // 提交完成后才能登记映射（见 arm_readback 注释）
        }

        if (surface != nullptr && wgpuSurfacePresent(surface) != WGPUStatus_Success) {
            AURORA_LOG_WARN("gpu-wgpu", "surfacePresent failed");
        }
        AURORA_WGPU_RELEASE(frame_view, wgpuTextureViewRelease)
        AURORA_WGPU_RELEASE(frame_tex, wgpuTextureRelease)
    }

    // 目标纹理 → MAP_READ 缓冲拷贝（编入本帧 command buffer）。⚠️ mapAsync 必须延后到
    // submit 之后（map_readback）：提交前登记映射会让 wgpu-core 立即完成映射，
    // 随后的 submit 因「写已映射缓冲」直接 Validation Error panic。
    void arm_readback() {
        if (canvas_.tex == nullptr || device_w <= 0 || device_h <= 0) {
            return;
        }
        const std::uint32_t bpr = (static_cast<std::uint32_t>(device_w) * 4U + 255U) & ~255U;
        const std::uint64_t len = static_cast<std::uint64_t>(bpr) * static_cast<std::uint32_t>(device_h);
        if (readback != nullptr && readback_cap < len) {
            AURORA_WGPU_RELEASE(readback, wgpuBufferRelease)
        }
        if (readback == nullptr) {
            WGPUBufferDescriptor bd{};
            bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
            bd.size = len;
            readback = wgpuDeviceCreateBuffer(device, &bd);
            if (readback == nullptr) {
                AURORA_LOG_WARN("gpu-wgpu", "readback buffer alloc failed (", len, " bytes)");
                return;
            }
            readback_cap = len;
        }
        WGPUTexelCopyTextureInfo src{};
        src.texture = canvas_.tex;
        src.mipLevel = 0;
        src.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = bpr;
        dst.layout.rowsPerImage = static_cast<std::uint32_t>(device_h);
        WGPUExtent3D extent{};
        extent.width = static_cast<std::uint32_t>(device_w);
        extent.height = static_cast<std::uint32_t>(device_h);
        extent.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
        readback_bpr = bpr;
        readback_len = len;
        map_done = false;
        map_armed = true;
    }

    // submit 之后登记映射；回调经 pump(ProcessEvents) 触发。
    void map_readback() {
        if (!map_armed || readback == nullptr) {
            return;
        }
        WGPUBufferMapCallbackInfo cb{};
        cb.mode = WGPUCallbackMode_WaitAnyOnly;
        cb.callback = &Impl::on_map_cb;
        cb.userdata1 = this;
        wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, readback_len, cb);
    }

    [[nodiscard]] bool read_pixels(std::vector<std::uint8_t> &out) {
        // 仅离屏模式提供读回（宿主 swapchain 的读回由宿主自行缓存，对齐 GL 路径）。
        if (!device_ok || surface != nullptr || readback == nullptr || !map_armed || device_w <= 0
            || device_h <= 0) {
            return false;
        }
        map_armed = false;
        if (!map_done && !pump(map_done, 2000)) {
            return false;
        }
        const auto w = static_cast<std::size_t>(device_w);
        const auto h = static_cast<std::size_t>(device_h);
        out.assign(w * h * 4U, 0);
        const auto *base = static_cast<const std::uint8_t *>(
            wgpuBufferGetConstMappedRange(readback, 0, static_cast<std::size_t>(readback_len)));
        if (base == nullptr) {
            return false;
        }
        for (std::size_t y = 0; y < h; ++y) {
            std::memcpy(out.data() + y * w * 4U, base + y * readback_bpr, w * 4U);
        }
        wgpuBufferUnmap(readback);
        readback_mapped = false;
        return true;
    }
};

// ============================================================
// WgpuRhi 公共面
// ============================================================

WgpuRhi::WgpuRhi() : impl_(std::make_unique<Impl>()) {}

WgpuRhi::WgpuRhi(const WgpuRhiOptions &options) : impl_(std::make_unique<Impl>(options)) {}

WgpuRhi::~WgpuRhi() = default;

auto WgpuRhi::valid() const -> bool { return impl_ && impl_->device_ok; }

auto WgpuRhi::submit(const DrawCmd &cmd, const CmdData &data) -> void {
    if (impl_ == nullptr || !impl_->device_ok || !impl_->frame_open) {
        return;  // 帧外提交的命令不消费（对齐 GL 路径 active 守卫）
    }
    impl_->translate(cmd, data);
}

auto WgpuRhi::begin_frame(int device_width, int device_height, float scale) -> bool {
    return impl_ != nullptr && impl_->begin_frame(device_width, device_height, scale);
}

auto WgpuRhi::end_frame() -> void {
    if (impl_ != nullptr) {
        impl_->end_frame();
    }
}

auto WgpuRhi::stats() const -> FrameStats {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->stats;
}

auto WgpuRhi::set_glyph_page_size(int side) -> void {
    if (impl_ != nullptr && side > 0) {
        impl_->glyph_page_size_ = side;
    }
}

auto WgpuRhi::read_pixels(std::vector<std::uint8_t> &out) -> bool {
    return impl_ != nullptr && impl_->read_pixels(out);
}

auto WgpuRhi::capabilities() const -> RhiCapabilities {
    RhiCapabilities caps;
    if (impl_ != nullptr && impl_->device_ok) {
        caps.gpu = true;
        caps.compute = impl_->compute_cap;
    }
    // native_surface_import：wgpu-native v29 C API 无外部共享纹理导入入口，恒 false。
    return caps;
}

// ---- 流式纹理槽（specification/03 §8.7）：句柄 = 槽键，与 DrawImage 流式分支同存储 ----

auto WgpuRhi::acquire_stream_image(std::uint64_t key, int width, int height) -> StreamImageId {
    if (impl_ == nullptr || !impl_->device_ok) {
        return 0;
    }
    return impl_->ensure_stream_slot(key, width, height) != nullptr ? key : 0;
}

auto WgpuRhi::update_stream_image(StreamImageId id, const std::uint8_t *pixels, std::size_t stride_bytes, int x,
                                  int y, int w, int h) -> void {
    if (impl_ == nullptr || !impl_->device_ok || id == 0 || pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const auto it = impl_->stream_slots.find(id);
    if (it == impl_->stream_slots.end()) {
        return;
    }
    Impl::StreamSlot &slot = it->second;
    if (x < 0 || y < 0 || x + w > slot.width || y + h > slot.height) {
        return;  // 脏矩形越界：按契约忽略（界内性由调用方保证）
    }
    impl_->flush_batch();  // 上传区可能与已录制批同区：先落地避免帧内读到半新内容
    const std::size_t stride =
        stride_bytes != 0 ? stride_bytes : static_cast<std::size_t>(slot.width) * 4U;
    impl_->write_tex_sub(slot.tex, x, y, w, h,
                         pixels + stride * static_cast<std::size_t>(y) + static_cast<std::size_t>(x) * 4U, stride,
                         4);
}

auto WgpuRhi::release_stream_image(StreamImageId id) -> void {
    if (impl_ == nullptr || !impl_->device_ok || id == 0) {
        return;
    }
    const auto it = impl_->stream_slots.find(id);
    if (it == impl_->stream_slots.end()) {
        return;
    }
    // wgpu 纹理由已录制 bind group 引用保活：即时删除不影响待提交批（无需先 flush）。
    Impl::release_tex(&it->second.tex);
    impl_->stream_slots.erase(it);
}

auto WgpuRhi::import_native_surface(const NativeSurfaceFrame & /*frame*/) -> StreamImageId {
    AURORA_LOG_WARN("gpu-wgpu",
                    "import_native_surface: no external texture import in wgpu-native v29 C API; CPU upload fallback");
    return 0;
}

}  // namespace aurora::rhi

#endif
