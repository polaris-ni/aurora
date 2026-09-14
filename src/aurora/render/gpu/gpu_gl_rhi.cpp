#include "aurora/render/rhi/gpu_gl_rhi.h"

// 本 TU 无条件编译（不裁切于 AURORA_BACKEND_GPU_GL）：全部 GL 访问经 GLFn 函数表指针，
// 不含任何 GL 原生头。公共头无条件声明 GpuGlRhi / load_gl，库必须恒提供符号——feature 宏
// 只控制 GlfwSurface 是否接线 GPU 模式（见 glfw_surface.cpp / AuroraBackends.cmake），
// 未开启时本类构造即 invalid（valid() = false），调用方走软件回退。

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "aurora/core/log.h"
#include "aurora/render/glyph_emit.h"
#include "aurora/render/gpu/gl_core.h"

namespace aurora::rhi {

// GL 3.3 core 常量子集（aurora::rhi::gl，见 gl_core.h）：本 TU 内以裸名直接引用，
// 函数调用则恒经 GLFn 函数表成员（fn 指针），二者命名空间互不重叠。
using namespace gl;

namespace {

// ---- GLSL 3.3 core 着色器源（顶点共享；逻辑 dp 坐标系 → NDC y 翻转） ----

constexpr const char *AURORA_GLSL_VERT = R"(#version 330 core
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_color;
uniform vec2 u_logical;   // 逻辑画布尺寸（dp）：NDC 映射基准
out vec2 v_pos;
out vec2 v_uv;
out vec4 v_color;
void main() {
    v_pos = a_pos;
    v_uv = a_uv;
    v_color = a_color;
    vec2 ndc = vec2(a_pos.x / u_logical.x * 2.0 - 1.0,
                    1.0 - a_pos.y / u_logical.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

// 实心 quad（FillRect/ClearRect/DrawRect/DrawLine）+ shader 内 SDF 裁剪。
// 裁剪不用 scissor：矩形/圆角统一 SDF alpha（float 精度、与软件路径逐像素交叠语义同源）；
// 不 discard——alpha=0 经 blend 写 dst=dst，语义等价且保留 early-z 之外的路径简单性。
constexpr const char *AURORA_GLSL_SOLID = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform vec4 u_clip;      // x,y,w,h（逻辑 dp）
uniform vec3 u_clip_ctl;  // (radius, on, aa)
out vec4 o;
float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}
void main() {
    vec4 c = v_color;
    if (u_clip_ctl.y > 0.5) {
        vec2 ctr = u_clip.xy + u_clip.zw * 0.5;
        float d = sd_rbox(v_pos - ctr, u_clip.zw * 0.5, u_clip_ctl.x);
        float cov = (u_clip_ctl.z > 0.5) ? (1.0 - smoothstep(-0.5, 0.5, d)) : step(d, 0.0);
        c.a *= cov;
    }
    o = c;
}
)";

// 圆角描边带（RoundedBorder）：外缘 d=0、内缘 d=-thickness（向内描边）、两侧 0.5px 羽化，
// 与软件 painter::draw_rounded_border 的覆盖度语义逐项对齐。
constexpr const char *AURORA_GLSL_BORDER = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform vec4 u_clip;
uniform vec3 u_clip_ctl;
uniform vec4 u_shape_box;  // cx, cy, half_w, half_h（逻辑 dp）
uniform vec2 u_border;     // (radius, thickness)
out vec4 o;
float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}
void main() {
    vec4 c = v_color;
    if (u_clip_ctl.y > 0.5) {
        vec2 ctr = u_clip.xy + u_clip.zw * 0.5;
        float d = sd_rbox(v_pos - ctr, u_clip.zw * 0.5, u_clip_ctl.x);
        float cov = (u_clip_ctl.z > 0.5) ? (1.0 - smoothstep(-0.5, 0.5, d)) : step(d, 0.0);
        c.a *= cov;
    }
    float d = sd_rbox(v_pos - u_shape_box.xy, u_shape_box.zw, u_border.x);
    float t = u_border.y;
    float cov = smoothstep(-t - 0.5, -t + 0.5, d) * (1.0 - smoothstep(-0.5, 0.5, d));
    c.a *= cov;
    o = c;
}
)";

// 渐变（Linear/Radial）：1D LUT 纹理采样（256×1 RGBA8，内容键缓存，见 acquire_lut）。
// t 由几何参数在片元内计算：linear = dot(P-a,d)/dot(d,d)（退化方向回落首色，镜像软件
// sample_gradient 的 front 回退）；radial = |P-c|/r。1D 采样做半像素对齐（t*255+0.5)/256，
// 保证 t=0/1 精确落在端点 texel。全局 alpha 经 v_color.a 烘焙进片元，与实心管线同源。
constexpr const char *AURORA_GLSL_GRAD = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_lut;
uniform vec2 u_ga;        // linear: 起点；radial: 圆心（逻辑 dp）
uniform vec2 u_gb;        // linear: 终点；radial: 未用
uniform float u_gr;       // radial: 半径（逻辑 dp）
uniform int u_radial;
uniform vec4 u_clip;
uniform vec3 u_clip_ctl;
out vec4 o;
float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}
void main() {
    float t;
    if (u_radial == 1) {
        t = length(v_pos - u_ga) / u_gr;
    } else {
        vec2 d = u_gb - u_ga;
        float dd = dot(d, d);
        t = (dd < 1e-6) ? 0.0 : dot(v_pos - u_ga, d) / dd;
    }
    t = clamp(t, 0.0, 1.0);
    vec4 g = texture(u_lut, vec2((t * 255.0 + 0.5) / 256.0, 0.5));
    vec4 c = vec4(g.rgb, g.a * v_color.a);
    if (u_clip_ctl.y > 0.5) {
        vec2 ctr = u_clip.xy + u_clip.zw * 0.5;
        float d = sd_rbox(v_pos - ctr, u_clip.zw * 0.5, u_clip_ctl.x);
        float cov = (u_clip_ctl.z > 0.5) ? (1.0 - smoothstep(-0.5, 0.5, d)) : step(d, 0.0);
        c.a *= cov;
    }
    o = c;
}
)";

// 图像（DrawImage）：RGBA8 直色图上传时预乘 alpha（PMA），LINEAR 双线性滤波在 PMA 空间
// 插值——与软件路径的 premultiplied 采样语义同源，避免半透明缩放边缘暗晕。片元输出按
// PMA 语义整体缩放（rgb 与 a 同乘全局 alpha × 裁剪 coverage），混合走 ONE/ONE_MINUS_SRC_ALPHA。
constexpr const char *AURORA_GLSL_IMAGE = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_tex;
uniform vec4 u_clip;
uniform vec3 u_clip_ctl;
out vec4 o;
float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}
void main() {
    vec4 t = texture(u_tex, v_uv);
    float cov = 1.0;
    if (u_clip_ctl.y > 0.5) {
        vec2 ctr = u_clip.xy + u_clip.zw * 0.5;
        float d = sd_rbox(v_pos - ctr, u_clip.zw * 0.5, u_clip_ctl.x);
        cov = (u_clip_ctl.z > 0.5) ? (1.0 - smoothstep(-0.5, 0.5, d)) : step(d, 0.0);
    }
    o = t * (v_color.a * cov);
}
)";

// 文本（DrawText）：灰度字形图集（R8 coverage）采样着色，coverage 进入 alpha（颜色不变，
// 再乘全局 alpha × 裁剪 coverage），混合走直色 src-over——与软件 blend_subpixel_span 的
// 灰度路径同源。字形位图按物理像素 1:1 对齐（原点与行基线均 snap 整数物理像素，同软件
// 路径），采样点恒落 texel 中心，NEAREST 即逐位精确。LCD 子像素在 GPU 路径降级灰度
//（发射桥强制 Supersample，设计容差决策），故无 RGB 分量分支。
constexpr const char *AURORA_GLSL_TEXT = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_atlas;
uniform vec4 u_clip;
uniform vec3 u_clip_ctl;
out vec4 o;
float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}
void main() {
    float cov = texture(u_atlas, v_uv).r;
    vec4 c = vec4(v_color.rgb, v_color.a * cov);
    if (u_clip_ctl.y > 0.5) {
        vec2 ctr = u_clip.xy + u_clip.zw * 0.5;
        float d = sd_rbox(v_pos - ctr, u_clip.zw * 0.5, u_clip_ctl.x);
        float clip_cov = (u_clip_ctl.z > 0.5) ? (1.0 - smoothstep(-0.5, 0.5, d)) : step(d, 0.0);
        c.a *= clip_cov;
    }
    o = c;
}
)";

// 阴影（Shadow）：矩形外部的欧氏距离线性衰减。软件 draw_shadow 的衰减因子为
// alpha = max(0, 1 - dist / blur_px)，其中 dist = 到阴影矩形（偏移后）的欧氏距离
//（外部；内部恒 1 由 fill_rect 快路径承担）。片元内 length(max(q, 0)) 与软件逐像素
// 的 sqrt(dx²+dy²) 逐项同构（内部 = 0 → 因子 1 = fill），blur 半径逻辑/物理换算后
// scale 相消，直接用逻辑 dp 计算。硬阴影（blur ≤ 0）不走本管线，翻译期退化为实心 quad。
constexpr const char *AURORA_GLSL_SHADOW = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform vec4 u_box;       // 阴影矩形 cx, cy, half_w, half_h（逻辑 dp）
uniform float u_blur;     // 模糊半径（逻辑 dp，> 0）
uniform vec4 u_clip;
uniform vec3 u_clip_ctl;
out vec4 o;
float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}
void main() {
    vec4 c = v_color;
    if (u_clip_ctl.y > 0.5) {
        vec2 ctr = u_clip.xy + u_clip.zw * 0.5;
        float d = sd_rbox(v_pos - ctr, u_clip.zw * 0.5, u_clip_ctl.x);
        float cov = (u_clip_ctl.z > 0.5) ? (1.0 - smoothstep(-0.5, 0.5, d)) : step(d, 0.0);
        c.a *= cov;
    }
    vec2 q = abs(v_pos - u_box.xy) - u_box.zw;
    float dist = length(max(q, vec2(0.0)));
    c.a *= max(0.0, 1.0 - dist / u_blur);
    o = c;
}
)";

// 区域模糊（BlurRegion）：两遍分离 box blur（水平 → 垂直），FBO ping-pong。采样与软件
// blur_region_scalar 逐项同构：n = 2r+1 恒权、tap 索引钳制在区域内（毛玻璃不漏采区外）、
// 整数和除 n 截断。texel 值经 unorm 往返需先 ×255 取整还原成整数域再做整数平均（截断
// floor），写回时 /255 由 GL unorm 转换逐字节精确复原。区域坐标全部为设备像素。
constexpr const char *AURORA_GLSL_BLUR = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_src;
uniform vec2 u_origin;    // 区域原点（设备像素）
uniform vec2 u_size;      // 区域尺寸（设备像素）
uniform vec2 u_canvas;    // 画布尺寸（设备像素）
uniform float u_scale;    // 设备像素 / 逻辑 dp
uniform int u_radius;     // box 半径（设备像素，≥ 1）
uniform int u_dir;        // 0 = 水平 / 1 = 垂直
out vec4 o;
void main() {
    vec2 ipx = floor(v_pos * u_scale) - u_origin;  // 区域内像素索引
    vec2 cl = clamp(ipx, vec2(0.0), u_size - vec2(1.0));
    vec4 acc = vec4(0.0);
    for (int k = -u_radius; k <= u_radius; ++k) {
        vec2 t = cl;
        if (u_dir == 0) {
            t.x = clamp(ipx.x + float(k), 0.0, u_size.x - 1.0);
        } else {
            t.y = clamp(ipx.y + float(k), 0.0, u_size.y - 1.0);
        }
        acc += floor(texture(u_src, (u_origin + t + vec2(0.5)) / u_canvas) * 255.0 + 0.5);
    }
    o = floor(acc / float(2 * u_radius + 1)) / 255.0;
}
)";

// 区域混合（BlendRegion）：把已绘内容与 tint 按 BlendMode 逐像素回写（CSS mix-blend-mode
// 子集）。软件 blend_region 全程整数运算（乘除 255 截断、强度回插向零截断），GPU 在浮点域
// 近似（归一化乘法 / mix），逐通道偏差 ≤ 1 LSB——设计容差内。alpha 通道不参与（软件只写 RGB）。
constexpr const char *AURORA_GLSL_BLEND = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_src;
uniform int u_mode;       // BlendMode 枚举值
uniform vec3 u_tint;      // tint（归一化）
uniform float u_strength; // 强度 [0,1]
out vec4 o;
void main() {
    vec4 s4 = floor(texture(u_src, v_uv) * 255.0 + 0.5);
    vec3 s = s4.rgb / 255.0;
    vec3 t = u_tint;
    vec3 r;
    if (u_mode == 0) {
        r = t;
    } else if (u_mode == 1) {
        r = s * t;
    } else if (u_mode == 2) {
        r = 1.0 - (1.0 - s) * (1.0 - t);
    } else if (u_mode == 3) {
        r = mix(2.0 * s * t, 1.0 - 2.0 * (1.0 - s) * (1.0 - t), step(vec3(0.5), s));
    } else if (u_mode == 4) {
        r = min(s, t);
    } else if (u_mode == 5) {
        r = max(s, t);
    } else if (u_mode == 6) {
        r = abs(s - t);
    } else if (u_mode == 7) {
        r = s + t - 2.0 * s * t;
    } else {
        r = s;
    }
    vec3 outc = clamp(s + u_strength * (r - s), 0.0, 1.0);
    o = vec4(outc, s4.a / 255.0);
}
)";

// 区域遮罩（MaskRegion）：把区域像素 RGB 乘以渐变因子（LinearFade/LinearRise/RadialFade），
// 与软件 mask_region 同构：因子基于区域内像素索引（整数域），radial 中心/最大半径按区域
// 设备像素尺寸计算，factor = 1 - strength*(1-base)，alpha 通道不变。
constexpr const char *AURORA_GLSL_MASK = R"(#version 330 core
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_src;
uniform int u_kind;       // ShaderMaskKind 枚举值
uniform float u_strength; // 强度 [0,1]
uniform vec2 u_origin;    // 区域原点（设备像素）
uniform vec2 u_size;      // 区域尺寸（设备像素）
uniform float u_scale;    // 设备像素 / 逻辑 dp
out vec4 o;
void main() {
    vec2 ipx = floor(v_pos * u_scale) - u_origin;
    float base = 1.0;
    if (u_kind == 0) {
        base = 1.0 - ipx.y / u_size.y;
    } else if (u_kind == 1) {
        base = ipx.y / u_size.y;
    } else if (u_kind == 2) {
        vec2 c = u_size * 0.5;
        base = 1.0 - length(ipx - c) / (length(c) + 0.001);
    }
    base = clamp(base, 0.0, 1.0);
    float factor = 1.0 - u_strength * (1.0 - base);
    vec4 s4 = floor(texture(u_src, v_uv) * 255.0 + 0.5);
    vec3 rgb = clamp(floor(s4.rgb / 255.0 * factor * 255.0), 0.0, 255.0);
    o = vec4(rgb / 255.0, s4.a / 255.0);
}
)";

// ---- 顶点布局：20 字节（pos2f + uv2f + color4ub 归一化）----

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

// 常量：MSAA 采样数（GL 3.3 core 保证 GL_MAX_SAMPLES ≥ 4）
constexpr int AURORA_MSAA_SAMPLES = 4;

// ---- 编译/链接辅助 ----

// GL 状态/错误码的 8 位十六进制诊断串（info log 为空的驱动上提供最低限度可定位信息）。
auto hex_u32(std::uint32_t v) -> std::string {
    const char *digits = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = digits[v & 0xFU];
        v >>= 4U;
    }
    return s;
}

auto compile_shader(const GLFn &gl, GLenum_ type, const char *src, std::string &err) -> GLuint_ {
    const GLuint_ sh = gl.create_shader(type);
    gl.shader_source(sh, 1, &src, nullptr);
    gl.compile_shader(sh);
    GLint_ ok = 0;
    gl.get_shader_iv(sh, COMPILE_STATUS, &ok);
    if (ok == 0) {
        char log[512] = {};
        GLsizei_ len = 0;
        gl.get_shader_info_log(sh, static_cast<GLsizei_>(sizeof(log)), &len, log);
        err = std::string(log, static_cast<std::size_t>(len > 0 ? len : 0));
        if (err.empty()) {
            // 部分驱动失败时 info log 为空：附加 GL 错误码辅助定位（编译期无上下文错误也明确）。
            err = "compile failed, GL error 0x" + hex_u32(gl.get_error()) + ", type 0x" + hex_u32(type);
        }
        gl.delete_shader(sh);
        return 0;
    }
    return sh;
}

auto link_program(const GLFn &gl, const char *vert_src, const char *frag_src, std::string &err) -> GLuint_ {
    const GLuint_ vs = compile_shader(gl, VERTEX_SHADER, vert_src, err);
    if (vs == 0) {
        return 0;
    }
    const GLuint_ fs = compile_shader(gl, FRAGMENT_SHADER, frag_src, err);
    if (fs == 0) {
        gl.delete_shader(vs);
        return 0;
    }
    const GLuint_ prog = gl.create_program();
    gl.attach_shader(prog, vs);
    gl.attach_shader(prog, fs);
    gl.link_program(prog);
    gl.delete_shader(vs);
    gl.delete_shader(fs);
    GLint_ ok = 0;
    gl.get_program_iv(prog, LINK_STATUS, &ok);
    if (ok == 0) {
        char log[512] = {};
        GLsizei_ len = 0;
        gl.get_program_info_log(prog, static_cast<GLsizei_>(sizeof(log)), &len, log);
        err = std::string(log, static_cast<std::size_t>(len > 0 ? len : 0));
        if (err.empty()) {
            err = "link failed, GL error 0x" + hex_u32(gl.get_error());
        }
        gl.delete_program(prog);
        return 0;
    }
    return prog;
}

// 软件端 sample_gradient（painter.cpp）的逐位镜像，仅供 LUT 生成期采样：texel j 以
// t = j/255 取值，保证 GPU 端 1D 采样与软件路径的插值语义精确一致——含 t<=0 取首色、
// t>=1 取尾色、无区间命中回落尾色（stops 不从 0 起 / 不到 1 止的边界怪癖）等全部行为。
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
    // colors/stops 长度可能不一致（反序列化 / 用户直传），索引上界取较小者，避免越界读。
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

// 渐变 LUT 尺寸：256 texel 覆盖 t∈[0,1]，半像素对齐采样下端点精确落在 texel 0/255。
constexpr int AURORA_LUT_WIDTH = 256;
// LUT 缓存容量上限：超出即整体清空重建（每帧渐变种类少量，抖动概率可忽略）。
constexpr std::size_t AURORA_LUT_CACHE_CAP = 64;
// 图像纹理缓存容量上限（与 LUT 同策略：溢出清空重建）。
constexpr std::size_t AURORA_IMAGE_CACHE_CAP = 64;
// 字形图集初始边长（px，R8）；满页倍增重建，2048 封顶后改为整页失效（槽位清空重排）。
constexpr int AURORA_GLYPH_ATLAS_START = 512;
constexpr int AURORA_GLYPH_ATLAS_MAX = 2048;

// FNV-1a 64 位内容摘要：图像纹理缓存键（维度混入尾部）。同帧逐命令重算 O(N)——
// 相对软件路径的逐像素双线性采样可忽略；命中后零上传，收益远大于摘要成本。
auto fnv1a_64(const std::uint8_t *data, std::size_t n, std::uint64_t seed = 14695981039346656037ULL)
    -> std::uint64_t {
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

// ---- GLFn：装载与完整性 ----

auto GLFn::complete() const -> bool {
    return create_shader != nullptr && shader_source != nullptr && compile_shader != nullptr
        && get_shader_iv != nullptr && get_shader_info_log != nullptr && delete_shader != nullptr
        && create_program != nullptr && attach_shader != nullptr && link_program != nullptr
        && get_program_iv != nullptr && get_program_info_log != nullptr && delete_program != nullptr
        && use_program != nullptr && get_uniform_location != nullptr && uniform1i != nullptr && uniform1f != nullptr
        && uniform2f != nullptr && uniform3f != nullptr && uniform4f != nullptr && uniform4fv != nullptr
        && gen_vertex_arrays != nullptr && delete_vertex_arrays != nullptr && bind_vertex_array != nullptr
        && gen_buffers != nullptr && delete_buffers != nullptr && bind_buffer != nullptr && buffer_data != nullptr
        && enable_vertex_attrib_array != nullptr && vertex_attrib_pointer != nullptr
        && gen_textures != nullptr && delete_textures != nullptr && bind_texture != nullptr
        && active_texture != nullptr && tex_image_2d != nullptr && tex_sub_image_2d != nullptr
        && tex_parameter_i != nullptr
        && pixel_store_i != nullptr
        && viewport != nullptr && clear_color != nullptr && clear != nullptr && enable != nullptr && disable != nullptr
        && scissor != nullptr && blend_func_separate != nullptr && draw_elements != nullptr && draw_arrays != nullptr
        && flush != nullptr
        && gen_framebuffers != nullptr && delete_framebuffers != nullptr && bind_framebuffer != nullptr
        && framebuffer_texture_2d != nullptr && framebuffer_renderbuffer != nullptr
        && check_framebuffer_status != nullptr && gen_renderbuffers != nullptr && delete_renderbuffers != nullptr
        && bind_renderbuffer != nullptr && renderbuffer_storage_multisample != nullptr && blit_framebuffer != nullptr
        && read_pixels != nullptr
        && get_string != nullptr && get_integer_v != nullptr && get_error != nullptr;
}

auto load_gl(void *(*proc)(const char *name)) -> GLFn {
    GLFn fn{};
    if (proc == nullptr) {
        return fn;
    }
    const auto load = [proc](const char *name, auto &dst) {
        dst = reinterpret_cast<std::remove_reference_t<decltype(dst)>>(proc(name));  // NOLINT(*-pro-type-reinterpret-cast)
    };
    // 着色器与程序
    load("glCreateShader", fn.create_shader);
    load("glShaderSource", fn.shader_source);
    load("glCompileShader", fn.compile_shader);
    load("glGetShaderiv", fn.get_shader_iv);
    load("glGetShaderInfoLog", fn.get_shader_info_log);
    load("glDeleteShader", fn.delete_shader);
    load("glCreateProgram", fn.create_program);
    load("glAttachShader", fn.attach_shader);
    load("glLinkProgram", fn.link_program);
    load("glGetProgramiv", fn.get_program_iv);
    load("glGetProgramInfoLog", fn.get_program_info_log);
    load("glDeleteProgram", fn.delete_program);
    load("glUseProgram", fn.use_program);
    load("glGetUniformLocation", fn.get_uniform_location);
    load("glUniform1i", fn.uniform1i);
    load("glUniform1f", fn.uniform1f);
    load("glUniform2f", fn.uniform2f);
    load("glUniform3f", fn.uniform3f);
    load("glUniform4f", fn.uniform4f);
    load("glUniform4fv", fn.uniform4fv);
    // 顶点数组与缓冲
    load("glGenVertexArrays", fn.gen_vertex_arrays);
    load("glDeleteVertexArrays", fn.delete_vertex_arrays);
    load("glBindVertexArray", fn.bind_vertex_array);
    load("glGenBuffers", fn.gen_buffers);
    load("glDeleteBuffers", fn.delete_buffers);
    load("glBindBuffer", fn.bind_buffer);
    load("glBufferData", fn.buffer_data);
    load("glEnableVertexAttribArray", fn.enable_vertex_attrib_array);
    load("glVertexAttribPointer", fn.vertex_attrib_pointer);
    // 纹理
    load("glGenTextures", fn.gen_textures);
    load("glDeleteTextures", fn.delete_textures);
    load("glBindTexture", fn.bind_texture);
    load("glActiveTexture", fn.active_texture);
    load("glTexImage2D", fn.tex_image_2d);
    load("glTexSubImage2D", fn.tex_sub_image_2d);
    load("glTexParameteri", fn.tex_parameter_i);
    load("glPixelStorei", fn.pixel_store_i);
    // 状态与绘制
    load("glViewport", fn.viewport);
    load("glClearColor", fn.clear_color);
    load("glClear", fn.clear);
    load("glEnable", fn.enable);
    load("glDisable", fn.disable);
    load("glScissor", fn.scissor);
    load("glBlendFuncSeparate", fn.blend_func_separate);
    load("glDrawElements", fn.draw_elements);
    load("glDrawArrays", fn.draw_arrays);
    load("glFlush", fn.flush);
    // 帧缓冲
    load("glGenFramebuffers", fn.gen_framebuffers);
    load("glDeleteFramebuffers", fn.delete_framebuffers);
    load("glBindFramebuffer", fn.bind_framebuffer);
    load("glFramebufferTexture2D", fn.framebuffer_texture_2d);
    load("glFramebufferRenderbuffer", fn.framebuffer_renderbuffer);
    load("glCheckFramebufferStatus", fn.check_framebuffer_status);
    load("glGenRenderbuffers", fn.gen_renderbuffers);
    load("glDeleteRenderbuffers", fn.delete_renderbuffers);
    load("glBindRenderbuffer", fn.bind_renderbuffer);
    load("glRenderbufferStorageMultisample", fn.renderbuffer_storage_multisample);
    load("glBlitFramebuffer", fn.blit_framebuffer);
    load("glReadPixels", fn.read_pixels);
    // 查询
    load("glGetString", fn.get_string);
    load("glGetIntegerv", fn.get_integer_v);
    load("glGetError", fn.get_error);
    return fn;
}

// ---- Impl ----

struct GpuGlRhi::Impl {
    // 管线标识（批 key 用）。Composite 命令复用 Image 管线（采样模式以 nearest_filter 区分）。
    enum class Pipeline : std::uint8_t { Solid = 1, Border = 2, Grad = 3, Image = 4, Text = 5, Shadow = 6 };

    // 裁剪态（批 key 成员；变化即断批）
    struct ClipState {
        bool on = false;
        Rect rect{};
        float radius = 0.0F;
        bool aa = true;
        auto operator==(const ClipState &) const -> bool = default;
    };

    // 批 key：任一成员变化即 flush 当前批
    struct BatchKey {
        Pipeline pipeline = Pipeline::Solid;
        ClipState clip{};
        bool blend_off = false;
        float border_radius = 0.0F;  // Border 管线专用（其余管线恒 0）
        float border_width = 0.0F;
        float shape_cx = 0.0F;  // Border 管线：圆角矩形中心/半宽半高（SDF uniform 对）
        float shape_cy = 0.0F;
        float shape_hw = 0.0F;
        float shape_hh = 0.0F;
        // Grad 管线专用：渐变几何参数与 LUT 纹理（纹理名即内容身份，同内容同纹理可合批）
        bool grad_radial = false;
        float grad_ax = 0.0F;
        float grad_ay = 0.0F;
        float grad_bx = 0.0F;
        float grad_by = 0.0F;
        float grad_r = 0.0F;
        GLuint_ grad_lut = 0;
        // Image 管线专用：PMA 纹理与混合模式（PMA 走 ONE/ONE_MINUS_SRC_ALPHA，变化断批）
        bool blend_pma = false;
        GLuint_ image_tex = 0;
        // Text 管线专用：字形图集纹理（纹理对象跨页重建沿用同名，整生命周期仅一个名字）
        GLuint_ glyph_atlas_tex = 0;
        // Shadow 管线专用：阴影矩形（偏移后）中心/半宽半高与模糊半径（逻辑 dp）
        float shadow_cx = 0.0F;
        float shadow_cy = 0.0F;
        float shadow_hw = 0.0F;
        float shadow_hh = 0.0F;
        float shadow_blur = 0.0F;
        // Image 管线专用：采样模式（Composite 离屏合成走 NEAREST——软件 composite_pixels
        // 为逐像素 floor 取样；DrawImage 双线性插值走 LINEAR）。纹理参数随批即时切换。
        bool nearest_filter = false;
        auto operator==(const BatchKey &) const -> bool = default;
    };

    // 渐变 LUT 缓存条目：键 = 色标数组内容拷贝（线性查找精确比对），值 = 1D RGBA8 纹理。
    struct LutEntry {
        std::vector<Color> colors;
        std::vector<float> stops;
        GLuint_ tex = 0;
    };

    // 图像纹理缓存条目：键 = 像素内容摘要 + 维度（64 位摘要，碰撞概率可忽略）。
    struct ImageTexEntry {
        std::uint64_t hash = 0;
        int width = 0;
        int height = 0;
        GLuint_ tex = 0;
    };

    // 字形图集槽位：纹理内像素矩形（Gray 条目按 width×rows 紧密排列，无 padding）。
    struct GlyphSlotRect {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    GLFn gl;
    bool failed = false;

    // GL 对象
    GLuint_ program_solid = 0;
    GLuint_ program_border = 0;
    GLuint_ program_grad = 0;
    GLuint_ program_image = 0;
    GLuint_ program_text = 0;
    GLuint_ program_shadow = 0;
    GLuint_ program_blur = 0;
    GLuint_ program_blend = 0;
    GLuint_ program_mask = 0;
    GLint_ solid_u_logical = -1;
    GLint_ solid_u_clip = -1;
    GLint_ solid_u_clip_ctl = -1;
    GLint_ border_u_logical = -1;
    GLint_ border_u_clip = -1;
    GLint_ border_u_clip_ctl = -1;
    GLint_ border_u_shape_box = -1;
    GLint_ border_u_border = -1;
    GLint_ grad_u_logical = -1;
    GLint_ grad_u_clip = -1;
    GLint_ grad_u_clip_ctl = -1;
    GLint_ grad_u_ga = -1;
    GLint_ grad_u_gb = -1;
    GLint_ grad_u_gr = -1;
    GLint_ grad_u_radial = -1;
    GLint_ grad_u_lut = -1;
    GLint_ image_u_logical = -1;
    GLint_ image_u_clip = -1;
    GLint_ image_u_clip_ctl = -1;
    GLint_ image_u_tex = -1;
    GLint_ text_u_logical = -1;
    GLint_ text_u_clip = -1;
    GLint_ text_u_clip_ctl = -1;
    GLint_ text_u_atlas = -1;
    GLint_ shadow_u_logical = -1;
    GLint_ shadow_u_clip = -1;
    GLint_ shadow_u_clip_ctl = -1;
    GLint_ shadow_u_box = -1;
    GLint_ shadow_u_blur = -1;
    GLint_ blur_u_src = -1;
    GLint_ blur_u_origin = -1;
    GLint_ blur_u_size = -1;
    GLint_ blur_u_canvas = -1;
    GLint_ blur_u_scale = -1;
    GLint_ blur_u_radius = -1;
    GLint_ blur_u_dir = -1;
    GLint_ blend_u_src = -1;
    GLint_ blend_u_mode = -1;
    GLint_ blend_u_tint = -1;
    GLint_ blend_u_strength = -1;
    GLint_ mask_u_src = -1;
    GLint_ mask_u_kind = -1;
    GLint_ mask_u_strength = -1;
    GLint_ mask_u_origin = -1;
    GLint_ mask_u_size = -1;
    GLint_ mask_u_scale = -1;
    // 效果程序共用顶点着色器（NDC 映射依赖 u_logical），各自持有独立 location
    GLint_ blur_u_logical = -1;
    GLint_ blend_u_logical = -1;
    GLint_ mask_u_logical = -1;
    GLuint_ vao = 0;
    GLuint_ vbo = 0;
    GLuint_ ibo = 0;
    std::uint32_t ibo_quads = 0;  // 索引缓冲当前覆盖的 quad 数
    GLuint_ msaa_fbo = 0;
    GLuint_ msaa_rb = 0;
    GLuint_ resolve_fbo = 0;
    GLuint_ resolve_tex = 0;
    // 效果管线（Blur/Blend/Mask）用普通纹理 FBO：内容采样回写的 ping-pong 中转。
    GLuint_ temp_fbo = 0;
    GLuint_ temp_tex = 0;
    bool msaa_dirty = true;  // MSAA 自上次 resolve 后有新绘制（效果采样前置与 end_frame 复用）
    std::vector<LutEntry> lut_cache;
    std::vector<ImageTexEntry> image_cache;

    // 字形图集（GPU 侧独立大图集，R8 架式打包）：槽位键 = 软件图集键（同字形同键 → 跨帧
    // 复用零重复上传）；位图内容来自软件 GlyphAtlas 条目（发射回调内即时拷贝上传，规避
    // LRU 悬垂）。纹理对象名整生命周期唯一，页重建只重定义存储不改名——批 key 无需感知页代。
    std::unordered_map<std::uint64_t, GlyphSlotRect> glyph_slots;
    GLuint_ glyph_atlas = 0;
    int atlas_w = 0;
    int atlas_h = 0;
    int pack_x = 0;
    int pack_y = 0;
    int pack_row_h = 0;

    // 画布
    int device_w = 0;
    int device_h = 0;
    float scale = 1.0F;  // 设备像素 / 逻辑 dp

    // 批与状态
    std::vector<Vertex> verts;
    BatchKey key{};
    bool key_active = false;
    double alpha = 1.0;
    std::vector<ClipState> clip_stack;

    FrameStats stats;

    // ---- GL 错误 ----
    auto check_error(const char *where) -> void {
        if (failed) {
            return;
        }
        const GLenum_ e = gl.get_error();
        if (e != NO_ERROR) {
            AURORA_LOG_ERROR("gpu-gl", "GL error ", static_cast<unsigned>(e), " at ", where);
            failed = true;
        }
    }

    // ---- 初始化（构造期调用；任何失败 → failed，valid() 即 false）----
    auto init() -> void {
        if (!gl.complete()) {
            AURORA_LOG_ERROR("gpu-gl", "GL 3.3 core function table incomplete (driver too old?)");
            failed = true;
            return;
        }
        // 版本门槛：核心版本 ≥ 3.3（字符串形如 "4.5.0 - build 27" / "3.3 (Core Profile) ..."）
        const char *ver = reinterpret_cast<const char *>(gl.get_string(VERSION));  // NOLINT(*-pro-type-reinterpret-cast)
        if (ver == nullptr || ver[0] < '3' || (ver[0] == '3' && ver[1] == '.' && ver[2] < '3')) {
            AURORA_LOG_ERROR("gpu-gl", "OpenGL 3.3+ required, got: ", ver != nullptr ? ver : "(null)");
            failed = true;
            return;
        }

        std::string err;
        program_solid = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_SOLID, err);
        if (program_solid == 0) {
            AURORA_LOG_ERROR("gpu-gl", "solid program link failed: ", err);
            failed = true;
            return;
        }
        program_border = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_BORDER, err);
        if (program_border == 0) {
            AURORA_LOG_ERROR("gpu-gl", "border program link failed: ", err);
            failed = true;
            return;
        }
        program_grad = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_GRAD, err);
        if (program_grad == 0) {
            AURORA_LOG_ERROR("gpu-gl", "gradient program link failed: ", err);
            failed = true;
            return;
        }
        program_image = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_IMAGE, err);
        if (program_image == 0) {
            AURORA_LOG_ERROR("gpu-gl", "image program link failed: ", err);
            failed = true;
            return;
        }
        program_text = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_TEXT, err);
        if (program_text == 0) {
            AURORA_LOG_ERROR("gpu-gl", "text program link failed: ", err);
            failed = true;
            return;
        }
        program_shadow = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_SHADOW, err);
        if (program_shadow == 0) {
            AURORA_LOG_ERROR("gpu-gl", "shadow program link failed: ", err);
            failed = true;
            return;
        }
        program_blur = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_BLUR, err);
        if (program_blur == 0) {
            AURORA_LOG_ERROR("gpu-gl", "blur program link failed: ", err);
            failed = true;
            return;
        }
        program_blend = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_BLEND, err);
        if (program_blend == 0) {
            AURORA_LOG_ERROR("gpu-gl", "blend program link failed: ", err);
            failed = true;
            return;
        }
        program_mask = link_program(gl, AURORA_GLSL_VERT, AURORA_GLSL_MASK, err);
        if (program_mask == 0) {
            AURORA_LOG_ERROR("gpu-gl", "mask program link failed: ", err);
            failed = true;
            return;
        }
        solid_u_logical = gl.get_uniform_location(program_solid, "u_logical");
        solid_u_clip = gl.get_uniform_location(program_solid, "u_clip");
        solid_u_clip_ctl = gl.get_uniform_location(program_solid, "u_clip_ctl");
        border_u_logical = gl.get_uniform_location(program_border, "u_logical");
        border_u_clip = gl.get_uniform_location(program_border, "u_clip");
        border_u_clip_ctl = gl.get_uniform_location(program_border, "u_clip_ctl");
        border_u_shape_box = gl.get_uniform_location(program_border, "u_shape_box");
        border_u_border = gl.get_uniform_location(program_border, "u_border");
        grad_u_logical = gl.get_uniform_location(program_grad, "u_logical");
        grad_u_clip = gl.get_uniform_location(program_grad, "u_clip");
        grad_u_clip_ctl = gl.get_uniform_location(program_grad, "u_clip_ctl");
        grad_u_ga = gl.get_uniform_location(program_grad, "u_ga");
        grad_u_gb = gl.get_uniform_location(program_grad, "u_gb");
        grad_u_gr = gl.get_uniform_location(program_grad, "u_gr");
        grad_u_radial = gl.get_uniform_location(program_grad, "u_radial");
        grad_u_lut = gl.get_uniform_location(program_grad, "u_lut");
        image_u_logical = gl.get_uniform_location(program_image, "u_logical");
        image_u_clip = gl.get_uniform_location(program_image, "u_clip");
        image_u_clip_ctl = gl.get_uniform_location(program_image, "u_clip_ctl");
        image_u_tex = gl.get_uniform_location(program_image, "u_tex");
        text_u_logical = gl.get_uniform_location(program_text, "u_logical");
        text_u_clip = gl.get_uniform_location(program_text, "u_clip");
        text_u_clip_ctl = gl.get_uniform_location(program_text, "u_clip_ctl");
        text_u_atlas = gl.get_uniform_location(program_text, "u_atlas");
        shadow_u_logical = gl.get_uniform_location(program_shadow, "u_logical");
        shadow_u_clip = gl.get_uniform_location(program_shadow, "u_clip");
        shadow_u_clip_ctl = gl.get_uniform_location(program_shadow, "u_clip_ctl");
        shadow_u_box = gl.get_uniform_location(program_shadow, "u_box");
        shadow_u_blur = gl.get_uniform_location(program_shadow, "u_blur");
        blur_u_src = gl.get_uniform_location(program_blur, "u_src");
        blur_u_origin = gl.get_uniform_location(program_blur, "u_origin");
        blur_u_size = gl.get_uniform_location(program_blur, "u_size");
        blur_u_canvas = gl.get_uniform_location(program_blur, "u_canvas");
        blur_u_scale = gl.get_uniform_location(program_blur, "u_scale");
        blur_u_radius = gl.get_uniform_location(program_blur, "u_radius");
        blur_u_dir = gl.get_uniform_location(program_blur, "u_dir");
        blend_u_src = gl.get_uniform_location(program_blend, "u_src");
        blend_u_mode = gl.get_uniform_location(program_blend, "u_mode");
        blend_u_tint = gl.get_uniform_location(program_blend, "u_tint");
        blend_u_strength = gl.get_uniform_location(program_blend, "u_strength");
        mask_u_src = gl.get_uniform_location(program_mask, "u_src");
        mask_u_kind = gl.get_uniform_location(program_mask, "u_kind");
        mask_u_strength = gl.get_uniform_location(program_mask, "u_strength");
        mask_u_origin = gl.get_uniform_location(program_mask, "u_origin");
        mask_u_size = gl.get_uniform_location(program_mask, "u_size");
        mask_u_scale = gl.get_uniform_location(program_mask, "u_scale");
        blur_u_logical = gl.get_uniform_location(program_blur, "u_logical");
        blend_u_logical = gl.get_uniform_location(program_blend, "u_logical");
        mask_u_logical = gl.get_uniform_location(program_mask, "u_logical");

        // VAO + 流式 VBO + 共享 quad 索引（GL core 必须经 VAO 绘制）
        GLuint_ va = 0;
        GLuint_ bufs[2] = {0, 0};
        gl.gen_vertex_arrays(1, &va);
        gl.gen_buffers(2, bufs);
        vao = va;
        vbo = bufs[0];
        ibo = bufs[1];
        gl.bind_vertex_array(vao);
        gl.bind_buffer(ARRAY_BUFFER, vbo);
        gl.enable_vertex_attrib_array(0);
        gl.vertex_attrib_pointer(0, 2, FLOAT, FALSE_, 20, reinterpret_cast<const void *>(0));  // NOLINT
        gl.enable_vertex_attrib_array(1);
        gl.vertex_attrib_pointer(1, 2, FLOAT, FALSE_, 20, reinterpret_cast<const void *>(8));  // NOLINT
        gl.enable_vertex_attrib_array(2);
        gl.vertex_attrib_pointer(2, 4, UNSIGNED_BYTE, TRUE_, 20, reinterpret_cast<const void *>(16));  // NOLINT
        gl.bind_buffer(ELEMENT_ARRAY_BUFFER, ibo);
        gl.bind_vertex_array(0);

        // 状态初始化（blend 语义与软件路径同源：直色 src-over）
        gl.disable(SCISSOR_TEST);
        gl.blend_func_separate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA);
        check_error("init");
    }

    void destroy() {
        if (vao != 0) {
            const GLuint_ va = vao;
            gl.delete_vertex_arrays(1, &va);
        }
        const GLuint_ bufs[2] = {vbo, ibo};
        if (bufs[0] != 0 || bufs[1] != 0) {
            gl.delete_buffers(2, bufs);
        }
        if (msaa_fbo != 0 || resolve_fbo != 0 || temp_fbo != 0) {
            const GLuint_ fbos[3] = {msaa_fbo, resolve_fbo, temp_fbo};
            gl.delete_framebuffers(3, fbos);
        }
        if (msaa_rb != 0) {
            const GLuint_ rb = msaa_rb;
            gl.delete_renderbuffers(1, &rb);
        }
        if (resolve_tex != 0 || temp_tex != 0) {
            const GLuint_ texs[2] = {resolve_tex, temp_tex};
            gl.delete_textures(2, texs);
        }
        for (const LutEntry &e : lut_cache) {
            if (e.tex != 0) {
                GLuint_ tex = e.tex;
                gl.delete_textures(1, &tex);
            }
        }
        for (const ImageTexEntry &e : image_cache) {
            if (e.tex != 0) {
                GLuint_ tex = e.tex;
                gl.delete_textures(1, &tex);
            }
        }
        if (glyph_atlas != 0) {
            const GLuint_ tex = glyph_atlas;
            gl.delete_textures(1, &tex);
        }
        if (program_solid != 0) {
            gl.delete_program(program_solid);
        }
        if (program_border != 0) {
            gl.delete_program(program_border);
        }
        if (program_grad != 0) {
            gl.delete_program(program_grad);
        }
        if (program_image != 0) {
            gl.delete_program(program_image);
        }
        if (program_text != 0) {
            gl.delete_program(program_text);
        }
        if (program_shadow != 0) {
            gl.delete_program(program_shadow);
        }
        if (program_blur != 0) {
            gl.delete_program(program_blur);
        }
        if (program_blend != 0) {
            gl.delete_program(program_blend);
        }
        if (program_mask != 0) {
            gl.delete_program(program_mask);
        }
    }

    // ---- 渐变 LUT 缓存 ----
    // 键 = 色标数组内容（精确比对）；命中即复用纹理，未命中生成 256×1 RGBA8 并入缓存。
    // 纹理过滤 LINEAR：半像素对齐采样下 t=0/1 精确落在端点 texel，texel 间线性过渡逼近
    // 软件连续插值；wrap CLAMP_TO_EDGE 防 t 精度漂移越界。
    auto acquire_lut(const std::vector<Color> &colors, const std::vector<float> &stops) -> GLuint_ {
        for (const LutEntry &e : lut_cache) {
            if (e.colors == colors && e.stops == stops) {
                return e.tex;
            }
        }
        if (lut_cache.size() >= AURORA_LUT_CACHE_CAP) {
            for (const LutEntry &e : lut_cache) {
                if (e.tex != 0) {
                    GLuint_ tex = e.tex;
                    gl.delete_textures(1, &tex);
                }
            }
            lut_cache.clear();
        }
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_LUT_WIDTH) * 4U> texels{};
        for (int j = 0; j < AURORA_LUT_WIDTH; ++j) {
            const Color c = sample_gradient_lut(colors, stops, static_cast<float>(j) / 255.0F);
            texels[static_cast<std::size_t>(j) * 4U + 0] = c.r;
            texels[static_cast<std::size_t>(j) * 4U + 1] = c.g;
            texels[static_cast<std::size_t>(j) * 4U + 2] = c.b;
            texels[static_cast<std::size_t>(j) * 4U + 3] = c.a;
        }
        GLuint_ tex = 0;
        gl.gen_textures(1, &tex);
        gl.bind_texture(TEXTURE_2D, tex);
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MIN_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MAG_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_S, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_T, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_image_2d(TEXTURE_2D, 0, static_cast<GLint_>(RGBA8), AURORA_LUT_WIDTH, 1, 0, RGBA, UNSIGNED_BYTE,
                        texels.data());
        check_error("acquire_lut");
        if (failed || tex == 0) {
            return 0;
        }
        lut_cache.push_back(LutEntry{colors, stops, tex});
        return tex;
    }

    // ---- 图像纹理缓存 ----
    // 键 = 像素内容摘要 + 维度；未命中时上传预乘 alpha（PMA）副本，LINEAR 滤波在 PMA
    // 空间插值（与软件双线性语义同源）。管线输出按 PMA 语义整体缩放，混合走
    // ONE/ONE_MINUS_SRC_ALPHA（blend_pma 批成员）。
    auto acquire_image_tex(const Image &img) -> GLuint_ {
        std::uint64_t hash = fnv1a_64(img.pixels.data(), img.pixels.size());
        hash ^= static_cast<std::uint64_t>(img.width);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint64_t>(img.height);
        hash *= 1099511628211ULL;
        for (const ImageTexEntry &e : image_cache) {
            if (e.hash == hash && e.width == img.width && e.height == img.height) {
                return e.tex;
            }
        }
        if (image_cache.size() >= AURORA_IMAGE_CACHE_CAP) {
            for (const ImageTexEntry &e : image_cache) {
                if (e.tex != 0) {
                    GLuint_ tex = e.tex;
                    gl.delete_textures(1, &tex);
                }
            }
            image_cache.clear();
        }
        // 预乘 alpha：dst = src.rgb * src.a / 255（四舍五入）；一次上传成本，换取
        // GPU 端 PMA 空间双线性滤波与软件路径一致的边缘语义。
        std::vector<std::uint8_t> pma(img.pixels.size());
        const std::size_t n = img.pixels.size();
        for (std::size_t i = 0; i + 3 < n; i += 4) {
            const unsigned a = img.pixels[i + 3];
            pma[i + 0] = static_cast<std::uint8_t>((static_cast<unsigned>(img.pixels[i + 0]) * a + 127) / 255);
            pma[i + 1] = static_cast<std::uint8_t>((static_cast<unsigned>(img.pixels[i + 1]) * a + 127) / 255);
            pma[i + 2] = static_cast<std::uint8_t>((static_cast<unsigned>(img.pixels[i + 2]) * a + 127) / 255);
            pma[i + 3] = static_cast<std::uint8_t>(a);
        }
        GLuint_ tex = 0;
        gl.gen_textures(1, &tex);
        gl.bind_texture(TEXTURE_2D, tex);
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MIN_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MAG_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_S, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_T, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.pixel_store_i(UNPACK_ALIGNMENT, 1);
        gl.tex_image_2d(TEXTURE_2D, 0, static_cast<GLint_>(RGBA8), img.width, img.height, 0, RGBA, UNSIGNED_BYTE,
                        pma.data());
        check_error("acquire_image_tex");
        if (failed || tex == 0) {
            return 0;
        }
        image_cache.push_back(ImageTexEntry{hash, img.width, img.height, tex});
        return tex;
    }

    // ---- GPU 字形图集（R8 架式打包）----
    // 页策略（设计文档 §6.5「GPU 侧独立大图集」）：初始 AURORA_GLYPH_ATLAS_START 见方，
    // 满页倍增重建；AURORA_GLYPH_ATLAS_MAX 封顶后整页失效（槽位清空，字形按需重传）。
    // 纹理对象名整生命周期唯一——重建只重定义存储（tex_image_2d），批 key 与已提交批次
    // 不受影响（旧批在重建前先 flush，GL 命令流保序，旧内容采样已完成）。
    auto ensure_glyph_atlas(int w, int h) -> bool {
        if (glyph_atlas != 0 && atlas_w == w && atlas_h == h) {
            return !failed;
        }
        if (glyph_atlas == 0) {
            GLuint_ tex = 0;
            gl.gen_textures(1, &tex);
            glyph_atlas = tex;
        }
        atlas_w = w;
        atlas_h = h;
        pack_x = 0;
        pack_y = 0;
        pack_row_h = 0;
        glyph_slots.clear();  // 尺寸变化后旧槽位坐标失效
        gl.bind_texture(TEXTURE_2D, glyph_atlas);
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MIN_FILTER, static_cast<GLint_>(NEAREST));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MAG_FILTER, static_cast<GLint_>(NEAREST));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_S, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_T, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.pixel_store_i(UNPACK_ALIGNMENT, 1);
        gl.tex_image_2d(TEXTURE_2D, 0, static_cast<GLint_>(R8), w, h, 0, RED, UNSIGNED_BYTE, nullptr);
        check_error("ensure_glyph_atlas");
        return !failed;
    }

    // 取字形槽位；未命中即打包上传。返回空矩形 = 无需绘制（空位图字形或异常大字形）。
    auto acquire_glyph_slot(std::uint64_t key, const render::GlyphAtlas::Entry &e) -> GlyphSlotRect {
        const auto it = glyph_slots.find(key);
        if (it != glyph_slots.end()) {
            return it->second;
        }
        // 空字形（空格等）：位图为空，无需图集槽位（发射核心照常推进 pen）。
        if (e.width <= 0 || e.rows <= 0 || e.buf.empty()) {
            return GlyphSlotRect{};
        }
        if (!ensure_glyph_atlas(AURORA_GLYPH_ATLAS_START, AURORA_GLYPH_ATLAS_START)) {
            return GlyphSlotRect{};
        }
        const int w = e.width;
        const int h = e.rows;
        // 超大字形（比当前页还宽/高）：先倍增到能装下；封顶仍装不下则放弃该字形。
        while ((w > atlas_w || h > atlas_h) && atlas_w < AURORA_GLYPH_ATLAS_MAX) {
            if (!ensure_glyph_atlas(std::min(atlas_w * 2, AURORA_GLYPH_ATLAS_MAX),
                                    std::min(atlas_h * 2, AURORA_GLYPH_ATLAS_MAX))) {
                return GlyphSlotRect{};
            }
        }
        if (w > atlas_w || h > atlas_h) {
            return GlyphSlotRect{};
        }
        if (pack_x + w > atlas_w) {
            pack_x = 0;
            pack_y += pack_row_h;
            pack_row_h = 0;
        }
        if (pack_y + h > atlas_h) {
            // 满页：先 flush（旧页上的顶点先画完，重建重定义存储不影响已提交批次），再扩页。
            flush_batch();
            const int next = std::min(atlas_w * 2, AURORA_GLYPH_ATLAS_MAX);
            if (next == atlas_w) {
                // 已封顶：整页失效（槽位清空重排，纹理复用；旧内容无需清除，槽位重排覆盖）。
                glyph_slots.clear();
                pack_x = 0;
                pack_y = 0;
                pack_row_h = 0;
            } else if (!ensure_glyph_atlas(next, next)) {
                return GlyphSlotRect{};
            }
            if (pack_x + w > atlas_w) {
                return GlyphSlotRect{};  // 封顶重排后仍放不下一行（异常大字形），放弃
            }
        }
        const GlyphSlotRect slot{pack_x, pack_y, w, h};
        gl.bind_texture(TEXTURE_2D, glyph_atlas);
        gl.pixel_store_i(UNPACK_ALIGNMENT, 1);
        gl.tex_sub_image_2d(TEXTURE_2D, 0, slot.x, slot.y, w, h, RED, UNSIGNED_BYTE, e.buf.data());
        check_error("acquire_glyph_slot");
        if (failed) {
            return GlyphSlotRect{};
        }
        pack_x += w;
        pack_row_h = std::max(pack_row_h, h);
        glyph_slots.emplace(key, slot);
        return slot;
    }

    // ---- 帧缓冲尺寸管理 ----
    auto ensure_framebuffer(int w, int h) -> bool {
        if (w == device_w && h == device_h && msaa_fbo != 0) {
            return true;
        }
        if (msaa_fbo == 0) {
            GLuint_ fbo = 0;
            gl.gen_framebuffers(1, &fbo);
            msaa_fbo = fbo;
            GLuint_ rb = 0;
            gl.gen_renderbuffers(1, &rb);
            msaa_rb = rb;
            GLuint_ rfbo = 0;
            gl.gen_framebuffers(1, &rfbo);
            resolve_fbo = rfbo;
            GLuint_ tex = 0;
            gl.gen_textures(1, &tex);
            resolve_tex = tex;
            GLuint_ tfbo = 0;
            gl.gen_framebuffers(1, &tfbo);
            temp_fbo = tfbo;
            GLuint_ ttex = 0;
            gl.gen_textures(1, &ttex);
            temp_tex = ttex;
        }
        gl.bind_renderbuffer(RENDERBUFFER, msaa_rb);
        gl.renderbuffer_storage_multisample(RENDERBUFFER, AURORA_MSAA_SAMPLES, RGBA8, w, h);
        gl.bind_framebuffer(FRAMEBUFFER, msaa_fbo);
        gl.framebuffer_renderbuffer(FRAMEBUFFER, COLOR_ATTACHMENT0, RENDERBUFFER, msaa_rb);
        if (gl.check_framebuffer_status(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
            AURORA_LOG_ERROR("gpu-gl", "MSAA framebuffer incomplete (", w, "x", h, ")");
            failed = true;
            return false;
        }
        // resolve 目标：普通纹理 FBO（效果管线采样与 read_pixels 依赖）
        gl.pixel_store_i(UNPACK_ALIGNMENT, 1);
        gl.bind_texture(TEXTURE_2D, resolve_tex);
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MIN_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MAG_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_S, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_T, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_image_2d(TEXTURE_2D, 0, static_cast<GLint_>(RGBA8), w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
        gl.bind_framebuffer(FRAMEBUFFER, resolve_fbo);
        gl.framebuffer_texture_2d(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, resolve_tex, 0);
        if (gl.check_framebuffer_status(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
            AURORA_LOG_ERROR("gpu-gl", "resolve framebuffer incomplete (", w, "x", h, ")");
            failed = true;
            return false;
        }
        // 效果 ping-pong 中转纹理（画布同尺寸；Blur/Blend/Mask pass 的采样/回写缓冲）
        gl.bind_texture(TEXTURE_2D, temp_tex);
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MIN_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MAG_FILTER, static_cast<GLint_>(LINEAR));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_S, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_parameter_i(TEXTURE_2D, TEXTURE_WRAP_T, static_cast<GLint_>(CLAMP_TO_EDGE));
        gl.tex_image_2d(TEXTURE_2D, 0, static_cast<GLint_>(RGBA8), w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
        gl.bind_framebuffer(FRAMEBUFFER, temp_fbo);
        gl.framebuffer_texture_2d(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, temp_tex, 0);
        if (gl.check_framebuffer_status(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
            AURORA_LOG_ERROR("gpu-gl", "effect framebuffer incomplete (", w, "x", h, ")");
            failed = true;
            return false;
        }
        gl.bind_framebuffer(FRAMEBUFFER, msaa_fbo);
        // 视口随画布（效果 pass 与常规批共用同一全局视口）
        gl.viewport(0, 0, w, h);
        device_w = w;
        device_h = h;
        msaa_dirty = true;  // 尺寸变化后 resolve 内容作废
        // 逻辑画布尺寸（NDC 映射与裁剪基准）：随 scale 换算
        check_error("ensure_framebuffer");
        return !failed;
    }

    [[nodiscard]] auto logical_w() const -> float {
        return static_cast<float>(device_w) / (scale > 0.0F ? scale : 1.0F);
    }
    [[nodiscard]] auto logical_h() const -> float {
        return static_cast<float>(device_h) / (scale > 0.0F ? scale : 1.0F);
    }

    // ---- 顶点发射 ----
    auto ensure_ibo(std::uint32_t quads) -> void {
        if (quads <= ibo_quads) {
            return;
        }
        const std::uint32_t want = quads * 2 < 4096 ? 4096 : quads * 2;
        static thread_local std::vector<GLuint_> pattern;
        pattern.clear();
        pattern.reserve(static_cast<std::size_t>(want) * 6);
        for (std::uint32_t q = 0; q < want; ++q) {
            const GLuint_ b = q * 4;
            pattern.insert(pattern.end(), {b, b + 1, b + 2, b + 2, b + 3, b});
        }
        gl.bind_buffer(ELEMENT_ARRAY_BUFFER, ibo);
        gl.buffer_data(ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr_>(pattern.size() * sizeof(GLuint_)),
                       pattern.data(), STATIC_DRAW);
        ibo_quads = want;
    }

    static auto bake_alpha(Color c, double a) -> Color {
        const double scaled = static_cast<double>(c.a) * a;
        c.a = static_cast<std::uint8_t>(scaled < 0.0 ? 0 : (scaled > 255.0 ? 255 : scaled + 0.5));
        return c;
    }

    auto push_quad(float x0, float y0, float x1, float y1, Color c) -> void {
        push_quad_uv(x0, y0, x1, y1, 0.0F, 0.0F, 1.0F, 1.0F, c);
    }

    auto push_quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c)
        -> void {
        const Vertex base[4] = {
            Vertex{x0, y0, u0, v0, c.r, c.g, c.b, c.a},
            Vertex{x1, y0, u1, v0, c.r, c.g, c.b, c.a},
            Vertex{x1, y1, u1, v1, c.r, c.g, c.b, c.a},
            Vertex{x0, y1, u0, v1, c.r, c.g, c.b, c.a},
        };
        verts.insert(verts.end(), base, base + 4);
    }

    auto begin_batch(const BatchKey &k) -> void {
        if (key_active && !(key == k)) {
            flush_batch();
        }
        key = k;
        key_active = true;
    }

    auto flush_batch() -> void {
        if (verts.empty()) {
            return;
        }
        const bool is_border = key.pipeline == Pipeline::Border;
        const bool is_grad = key.pipeline == Pipeline::Grad;
        const bool is_image = key.pipeline == Pipeline::Image;
        const bool is_text = key.pipeline == Pipeline::Text;
        const bool is_shadow = key.pipeline == Pipeline::Shadow;
        const GLuint_ program = is_border ? program_border
                                          : (is_grad ? program_grad
                                                     : (is_image ? program_image
                                                                 : (is_shadow ? program_shadow
                                                                              : (is_text ? program_text
                                                                                         : program_solid))));
        gl.use_program(program);
        gl.uniform2f(is_border ? border_u_logical
                               : (is_grad ? grad_u_logical
                                          : (is_image  ? image_u_logical
                                                       : (is_shadow ? shadow_u_logical
                                                                    : (is_text ? text_u_logical
                                                                               : solid_u_logical)))),
                     logical_w(), logical_h());
        // 裁剪 uniform（栈顶 = 各层矩形交集，语义与 Painter::push_clip 一致）
        const ClipState &clip = key.clip;
        const GLint_ u_clip =
            is_border ? border_u_clip
                      : (is_grad ? grad_u_clip
                                 : (is_image ? image_u_clip
                                             : (is_shadow ? shadow_u_clip
                                                          : (is_text ? text_u_clip : solid_u_clip))));
        const GLint_ u_ctl = is_border ? border_u_clip_ctl
                                       : (is_grad  ? grad_u_clip_ctl
                                                   : (is_image ? image_u_clip_ctl
                                                               : (is_shadow ? shadow_u_clip_ctl
                                                                            : (is_text ? text_u_clip_ctl
                                                                                       : solid_u_clip_ctl))));
        gl.uniform4f(u_clip, clip.rect.origin.x, clip.rect.origin.y, clip.rect.size.width, clip.rect.size.height);
        gl.uniform3f(u_ctl, clip.radius, clip.on ? 1.0F : 0.0F, clip.aa ? 1.0F : 0.0F);
        if (is_border) {
            gl.uniform4f(border_u_shape_box, key.shape_cx, key.shape_cy, key.shape_hw, key.shape_hh);
            gl.uniform2f(border_u_border, key.border_radius, key.border_width);
        }
        if (is_grad) {
            // LUT 绑定到纹理单元 0（采样器 uniform 一次设 0；纹理名随批 key 变化即断批重绑）。
            gl.active_texture(TEXTURE0);
            gl.bind_texture(TEXTURE_2D, key.grad_lut);
            gl.uniform1i(grad_u_lut, 0);
            gl.uniform2f(grad_u_ga, key.grad_ax, key.grad_ay);
            gl.uniform2f(grad_u_gb, key.grad_bx, key.grad_by);
            gl.uniform1f(grad_u_gr, key.grad_r);
            gl.uniform1i(grad_u_radial, key.grad_radial ? 1 : 0);
        }
        if (is_image) {
            gl.active_texture(TEXTURE0);
            gl.bind_texture(TEXTURE_2D, key.image_tex);
            gl.uniform1i(image_u_tex, 0);
            // 采样模式随批切换（DrawImage 双线性 / Composite 逐像素取样）——纹理参数是
            // 纹理对象状态，同纹理可能被两种管线复用，flush 时显式设定消除跨批残留。
            const GLint_ filter = static_cast<GLint_>(key.nearest_filter ? NEAREST : LINEAR);
            gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MIN_FILTER, filter);
            gl.tex_parameter_i(TEXTURE_2D, TEXTURE_MAG_FILTER, filter);
        }
        if (is_text) {
            gl.active_texture(TEXTURE0);
            gl.bind_texture(TEXTURE_2D, key.glyph_atlas_tex);
            gl.uniform1i(text_u_atlas, 0);
        }
        if (key.blend_off) {
            gl.disable(BLEND);
        } else {
            gl.enable(BLEND);
            // 混合函数逐批设置（直色 src-over vs PMA 全量 src-over；GL 全局态跨批残留）。
            if (key.blend_pma) {
                gl.blend_func_separate(ONE, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA);
            } else {
                gl.blend_func_separate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA);
            }
        }
        gl.bind_vertex_array(vao);
        gl.bind_buffer(ARRAY_BUFFER, vbo);
        gl.buffer_data(ARRAY_BUFFER, static_cast<GLsizeiptr_>(verts.size() * sizeof(Vertex)), verts.data(),
                       STREAM_DRAW);
        ensure_ibo(static_cast<std::uint32_t>(verts.size() / 4));
        gl.draw_elements(TRIANGLES, static_cast<GLsizei_>(verts.size() / 4 * 6), UNSIGNED_INT, nullptr);
        stats.draw_calls++;
        stats.vertices += static_cast<std::uint32_t>(verts.size());
        verts.clear();
        msaa_dirty = true;  // MSAA 内容已变：效果采样前置与后续 resolve 需刷新
        check_error("flush");
    }

    // ---- 效果 pass 机制（BlurRegion/BlendRegion/MaskRegion：读已绘内容 → 回写画布）----
    // 已绘内容位于 MSAA 渲染缓冲（不可采样），效果 pass 前先 resolve 成 resolve 纹理。
    // msaa_dirty 门控：同帧连续效果只在内容变化后重新 resolve，避免重复 blit。

    /// 效果前置：待提交批先落 MSAA，再把 MSAA 内容 resolve 成可采样纹理（脏时才 blit）。
    auto flush_and_resolve() -> void {
        flush_batch();
        if (!msaa_dirty) {
            return;
        }
        gl.bind_framebuffer(READ_FRAMEBUFFER, msaa_fbo);
        gl.bind_framebuffer(DRAW_FRAMEBUFFER, resolve_fbo);
        gl.blit_framebuffer(0, 0, device_w, device_h, 0, 0, device_w, device_h, COLOR_BUFFER_BIT, NEAREST);
        check_error("effect-resolve");
        msaa_dirty = false;
    }

    auto bind_sample_tex(GLuint_ tex) -> void {
        gl.active_texture(TEXTURE0);
        gl.bind_texture(TEXTURE_2D, tex);
    }

    /// 效果 pass 单 quad 即时绘制（不经批系统：专用程序 + 直写替换语义，混合由调用方禁用）。
    /// 坐标为逻辑 dp；uv 为画布归一化采样矩形（blend/mask 经 v_uv 采样，blur 在片元内用
    /// 绝对设备坐标计算采样，v_uv 传入保持顶点形状一致）。
    auto draw_effect_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) -> void {
        const Vertex base[4] = {
            Vertex{x0, y0, u0, v0, 255, 255, 255, 255},
            Vertex{x1, y0, u1, v0, 255, 255, 255, 255},
            Vertex{x1, y1, u1, v1, 255, 255, 255, 255},
            Vertex{x0, y1, u0, v1, 255, 255, 255, 255},
        };
        gl.bind_vertex_array(vao);
        gl.bind_buffer(ARRAY_BUFFER, vbo);
        gl.buffer_data(ARRAY_BUFFER, static_cast<GLsizeiptr_>(sizeof(base)), base, STREAM_DRAW);
        ensure_ibo(1);
        gl.draw_elements(TRIANGLES, 6, UNSIGNED_INT, nullptr);
        stats.draw_calls++;
        stats.vertices += 4;
    }

    /// 区域效果共享的物理像素区域换算：floor/ceil + 画布钳制（同软件三原语）。
    /// 返回 false = 空区域（调用方直接跳过，同软件提前返回）。
    [[nodiscard]] auto effect_region_px(const Rect &region, int &rx0, int &ry0, int &rx1, int &ry1) const -> bool {
        const float s = scale > 0.0F ? scale : 1.0F;
        rx0 = std::max(0, static_cast<int>(std::floor(region.origin.x * s)));
        ry0 = std::max(0, static_cast<int>(std::floor(region.origin.y * s)));
        rx1 = std::min(device_w, static_cast<int>(std::ceil((region.origin.x + region.size.width) * s)));
        ry1 = std::min(device_h, static_cast<int>(std::ceil((region.origin.y + region.size.height) * s)));
        return rx1 > rx0 && ry1 > ry0;
    }

    // ---- 命令翻译 ----

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

    void translate(const DrawCmd &cmd, const CmdData &data) {
        switch (cmd.kind) {
            case CmdKind::FillRect: {
                BatchKey k{};
                k.pipeline = Pipeline::Solid;
                k.clip = effective_clip();
                begin_batch(k);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(cmd.color, alpha));
                break;
            }
            case CmdKind::ClearRect: {
                // 语义：区域归零（RGBA 全零，不走混合、不受裁剪/alpha 影响）——镜像 Painter::clear_rect。
                BatchKey k{};
                k.pipeline = Pipeline::Solid;
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
                k.pipeline = Pipeline::Solid;
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
                k.pipeline = Pipeline::Solid;
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
            case CmdKind::RoundedBorder: {
                if (cmd.f1 <= 0.0F || cmd.color.a == 0 || cmd.bounds.size.width <= 0.0F
                    || cmd.bounds.size.height <= 0.0F) {
                    break;
                }
                BatchKey k{};
                k.pipeline = Pipeline::Border;
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
            case CmdKind::DrawText: {
                // 与 software_rhi 同形的数据契约；缺文本/字体直接跳过（录制方恒带 font_idx，
                // 正常路径不会触发）。空色不绘（软件同契约）。
                if (data.text == nullptr || data.font == nullptr || data.text->empty() || cmd.color.a == 0
                    || failed) {
                    break;
                }
                const render::TextLayoutOpts opts{
                    .letter_spacing = cmd.text_ls, .word_spacing = cmd.text_ws, .italic = cmd.text_italic};
                // 软件路径同源：FontEngine 全程物理像素语义；DrawCmd.bounds 为逻辑 dp，原点先换算。
                const float origin_x = cmd.bounds.origin.x * scale;
                const float origin_y = cmd.bounds.origin.y * scale;
                const ClipState clip = effective_clip();
                // aa 恒 Supersample：GPU v1 容差决策——LCD 子像素着色依赖精确的 RGB 条纹布局，
                // 降级灰度保证颜色安全（设计文档 §5 DrawText 行）。
                const bool ok = render::emit_text_glyphs(
                    *data.text, *data.font, opts, scale, render::TextAAMode::Supersample, cmd.color, origin_x,
                    origin_y,
                    [this, &clip, &cmd](const render::GlyphAtlas::Entry &entry, render::GlyphAtlas::Mode mode,
                                        int dx0, int dy0, std::uint64_t key) {
                        if (mode != render::GlyphAtlas::Mode::Gray) {
                            return;  // 防御：GPU 路径恒灰度
                        }
                        // 取槽位可能触发满页 flush/扩页，须在 begin_batch 之前完成。
                        const GlyphSlotRect slot = acquire_glyph_slot(key, entry);
                        if (slot.w <= 0 || slot.h <= 0 || failed) {
                            return;
                        }
                        BatchKey k{};
                        k.pipeline = Pipeline::Text;
                        k.clip = clip;
                        k.glyph_atlas_tex = glyph_atlas;
                        begin_batch(k);
                        // 顶点坐标：物理像素 → 逻辑 dp（NDC 映射基准）；uv = 图集槽位归一化矩形。
                        const float inv_s = 1.0F / scale;
                        const float aw = static_cast<float>(atlas_w);
                        const float ah = static_cast<float>(atlas_h);
                        const float x0 = static_cast<float>(dx0) * inv_s;
                        const float y0 = static_cast<float>(dy0) * inv_s;
                        push_quad_uv(x0, y0, x0 + static_cast<float>(slot.w) * inv_s,
                                     y0 + static_cast<float>(slot.h) * inv_s,
                                     static_cast<float>(slot.x) / aw, static_cast<float>(slot.y) / ah,
                                     static_cast<float>(slot.x + slot.w) / aw,
                                     static_cast<float>(slot.y + slot.h) / ah, bake_alpha(cmd.color, alpha));
                    });
                (void)ok;  // 无字体面（引擎恒有内置字体，理论不触发）：GPU 路径无位图兜底，跳过
                break;
            }
            case CmdKind::DrawImage: {
                // 软件端契约：空像素 / 非法维度 / 缓冲不足 w*h*4 直接返回（不变量校验同形）。
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
                const GLuint_ tex = acquire_image_tex(img);
                if (tex == 0 || failed) {
                    break;
                }
                BatchKey k{};
                k.pipeline = Pipeline::Image;
                k.clip = effective_clip();
                k.blend_pma = true;
                k.image_tex = tex;
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
                // 软件端退化阈值：物理像素 len_sq < 0.001 → fill_rect(首色)（走实心管线，
                // 含裁剪 + 全局 alpha）；此处同形翻译，scale² 把逻辑长度折算到物理域。
                const float len_sq_phys = (dx * dx + dy * dy) * scale * scale;
                if (len_sq_phys < 0.001F) {
                    BatchKey k{};
                    k.pipeline = Pipeline::Solid;
                    k.clip = effective_clip();
                    begin_batch(k);
                    push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y,
                              cmd.bounds.origin.x + cmd.bounds.size.width,
                              cmd.bounds.origin.y + cmd.bounds.size.height,
                              bake_alpha(data.colors->front(), alpha));
                    break;
                }
                const GLuint_ lut = acquire_lut(*data.colors, *data.stops);
                if (lut == 0 || failed) {
                    break;
                }
                BatchKey k{};
                k.pipeline = Pipeline::Grad;
                k.clip = effective_clip();
                k.grad_ax = ax;
                k.grad_ay = ay;
                k.grad_bx = cmd.pt1.x;
                k.grad_by = cmd.pt1.y;
                k.grad_lut = lut;
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
                const GLuint_ lut = acquire_lut(*data.colors, *data.stops);
                if (lut == 0 || failed) {
                    break;
                }
                BatchKey k{};
                k.pipeline = Pipeline::Grad;
                k.clip = effective_clip();
                k.grad_radial = true;
                k.grad_ax = cmd.pt0.x;
                k.grad_ay = cmd.pt0.y;
                k.grad_r = cmd.f0;
                k.grad_lut = lut;
                begin_batch(k);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(Color{255, 255, 255, 255}, alpha));
                break;
            }
            case CmdKind::Shadow: {
                // 软件契约：blur ≤ 0 硬阴影 = 偏移矩形实心填充；模糊阴影 = 内部实心 + 外环
                // 距离衰减。GPU 用单个 Shadow quad 覆盖扩展区（内部 dist=0 → 因子 1 = fill，
                // 外部线性衰减），与软件语义同构且一次批提交；扩展区 = blur × 2（软件同形）。
                const Rect shadow_rect{
                    .origin = Point{.x = cmd.bounds.origin.x + cmd.f0, .y = cmd.bounds.origin.y + cmd.f1},
                    .size = cmd.bounds.size};
                if (cmd.f2 <= 0.0F) {
                    BatchKey k{};
                    k.pipeline = Pipeline::Solid;
                    k.clip = effective_clip();
                    begin_batch(k);
                    push_quad(shadow_rect.origin.x, shadow_rect.origin.y,
                              shadow_rect.origin.x + shadow_rect.size.width,
                              shadow_rect.origin.y + shadow_rect.size.height, bake_alpha(cmd.color, alpha));
                    break;
                }
                const float expand = cmd.f2 * 2.0F;
                BatchKey k{};
                k.pipeline = Pipeline::Shadow;
                k.clip = effective_clip();
                k.shadow_cx = shadow_rect.origin.x + shadow_rect.size.width * 0.5F;
                k.shadow_cy = shadow_rect.origin.y + shadow_rect.size.height * 0.5F;
                k.shadow_hw = shadow_rect.size.width * 0.5F;
                k.shadow_hh = shadow_rect.size.height * 0.5F;
                k.shadow_blur = cmd.f2;
                begin_batch(k);
                push_quad(shadow_rect.origin.x - expand, shadow_rect.origin.y - expand,
                          shadow_rect.origin.x + shadow_rect.size.width + expand,
                          shadow_rect.origin.y + shadow_rect.size.height + expand,
                          bake_alpha(cmd.color, alpha));
                break;
            }
            case CmdKind::BlurRegion: {
                // 软件契约：radius ≤ 0 直接返回；区域物理像素换算同软件（floor/ceil + 画布钳制），
                // 半径 r = max(1, trunc(radius × scale))。两遍分离 box blur（水平 → 垂直）经
                // resolve → temp → MSAA ping-pong，tap 钳制在区域内（毛玻璃不漏采区外），
                // 回写为直写替换（结果已含源内容），混合禁用。
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
                flush_and_resolve();
                gl.disable(BLEND);
                gl.use_program(program_blur);
                gl.uniform2f(blur_u_logical, logical_w(), logical_h());
                gl.uniform1i(blur_u_src, 0);
                gl.uniform1i(blur_u_radius, r);
                gl.uniform2f(blur_u_canvas, static_cast<float>(device_w), static_cast<float>(device_h));
                gl.uniform1f(blur_u_scale, s);
                gl.uniform2f(blur_u_origin, static_cast<float>(rx0), static_cast<float>(ry0));
                gl.uniform2f(blur_u_size, static_cast<float>(rx1 - rx0), static_cast<float>(ry1 - ry0));
                const float inv_s = 1.0F / s;
                const float lx0 = static_cast<float>(rx0) * inv_s;
                const float ly0 = static_cast<float>(ry0) * inv_s;
                const float lx1 = static_cast<float>(rx1) * inv_s;
                const float ly1 = static_cast<float>(ry1) * inv_s;
                const float u0 = static_cast<float>(rx0) / static_cast<float>(device_w);
                const float v0 = static_cast<float>(ry0) / static_cast<float>(device_h);
                const float u1 = static_cast<float>(rx1) / static_cast<float>(device_w);
                const float v1 = static_cast<float>(ry1) / static_cast<float>(device_h);
                gl.uniform1i(blur_u_dir, 0);
                bind_sample_tex(resolve_tex);
                gl.bind_framebuffer(FRAMEBUFFER, temp_fbo);
                draw_effect_quad(lx0, ly0, lx1, ly1, u0, v0, u1, v1);
                gl.uniform1i(blur_u_dir, 1);
                bind_sample_tex(temp_tex);
                gl.bind_framebuffer(FRAMEBUFFER, msaa_fbo);
                draw_effect_quad(lx0, ly0, lx1, ly1, u0, v0, u1, v1);
                msaa_dirty = true;
                check_error("blur-region");
                break;
            }
            case CmdKind::BlendRegion: {
                // 软件契约：strength 截断到 [0,1]，≤ 0 直接返回；只改 RGB，alpha 原样保留。
                // 枚举序与着色器 u_mode 分支一一对应（BlendMode Normal..Exclusion = 0..7）。
                // 浮点域近似软件整数运算（乘除 255 截断、强度回插向零截断），逐通道偏差
                // ≤ 1 LSB（设计容差内）。单 pass：resolve 采样 → 回写 MSAA。
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
                const float s = scale > 0.0F ? scale : 1.0F;
                flush_and_resolve();
                gl.bind_framebuffer(FRAMEBUFFER, msaa_fbo);
                gl.disable(BLEND);
                gl.use_program(program_blend);
                gl.uniform2f(blend_u_logical, logical_w(), logical_h());
                gl.uniform1i(blend_u_src, 0);
                gl.uniform1i(blend_u_mode, static_cast<int>(cmd.blend_mode));
                gl.uniform3f(blend_u_tint, static_cast<float>(cmd.color.r) / 255.0F,
                             static_cast<float>(cmd.color.g) / 255.0F, static_cast<float>(cmd.color.b) / 255.0F);
                gl.uniform1f(blend_u_strength, strength);
                bind_sample_tex(resolve_tex);
                draw_effect_quad(static_cast<float>(rx0) / s, static_cast<float>(ry0) / s,
                                 static_cast<float>(rx1) / s, static_cast<float>(ry1) / s,
                                 static_cast<float>(rx0) / static_cast<float>(device_w),
                                 static_cast<float>(ry0) / static_cast<float>(device_h),
                                 static_cast<float>(rx1) / static_cast<float>(device_w),
                                 static_cast<float>(ry1) / static_cast<float>(device_h));
                msaa_dirty = true;
                check_error("blend-region");
                break;
            }
            case CmdKind::MaskRegion: {
                // 软件契约：strength 截断到 [0,1]，≤ 0 直接返回；RGB 乘渐变因子（基于区域内
                // 像素索引，radial 中心/最大半径按区域设备像素尺寸），alpha 不变。单 pass。
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
                const float s = scale > 0.0F ? scale : 1.0F;
                flush_and_resolve();
                gl.bind_framebuffer(FRAMEBUFFER, msaa_fbo);
                gl.disable(BLEND);
                gl.use_program(program_mask);
                gl.uniform2f(mask_u_logical, logical_w(), logical_h());
                gl.uniform1i(mask_u_src, 0);
                gl.uniform1i(mask_u_kind, static_cast<int>(cmd.mask_kind));
                gl.uniform1f(mask_u_strength, strength);
                gl.uniform2f(mask_u_origin, static_cast<float>(rx0), static_cast<float>(ry0));
                gl.uniform2f(mask_u_size, static_cast<float>(rx1 - rx0), static_cast<float>(ry1 - ry0));
                gl.uniform1f(mask_u_scale, s);
                bind_sample_tex(resolve_tex);
                draw_effect_quad(static_cast<float>(rx0) / s, static_cast<float>(ry0) / s,
                                 static_cast<float>(rx1) / s, static_cast<float>(ry1) / s,
                                 static_cast<float>(rx0) / static_cast<float>(device_w),
                                 static_cast<float>(ry0) / static_cast<float>(device_h),
                                 static_cast<float>(rx1) / static_cast<float>(device_w),
                                 static_cast<float>(ry1) / static_cast<float>(device_h));
                msaa_dirty = true;
                check_error("mask-region");
                break;
            }
            case CmdKind::Composite: {
                // 软件契约（composite_pixels 前置校验同形）：空像素 / 非法维度 / 缓冲不足
                // 直接返回。仿射矩阵直烘进四角顶点（旋转/错切正确；纯平移/缩放退化为轴对齐
                // quad），uv = 源逻辑角点归一化——NEAREST 采样下 texel = floor(uv × 尺寸)，
                // 与软件逆映射逐像素 floor 取样同构；PMA 直色（acquire_image_tex 预乘上传）。
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
                const GLuint_ tex = acquire_image_tex(img);
                if (tex == 0 || failed) {
                    break;
                }
                const Matrix2D identity{};
                const Matrix2D &mat = data.matrix != nullptr ? *data.matrix : identity;
                const float src_scale = cmd.composite_scale > 0.0F ? cmd.composite_scale : 1.0F;
                const float lw = static_cast<float>(img.width) / src_scale;
                const float lh = static_cast<float>(img.height) / src_scale;
                BatchKey k{};
                k.pipeline = Pipeline::Image;
                k.clip = effective_clip();
                k.blend_pma = true;
                k.image_tex = tex;
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
};

// ---- GpuGlRhi 公共方法 ----

GpuGlRhi::GpuGlRhi() = default;

GpuGlRhi::GpuGlRhi(GLFn fn) : impl_(std::make_unique<Impl>()) {
    impl_->gl = fn;
    impl_->init();
}

GpuGlRhi::~GpuGlRhi() {
    if (impl_ != nullptr) {
        impl_->destroy();
    }
}

auto GpuGlRhi::valid() const -> bool {
    return impl_ != nullptr && !impl_->failed;
}

auto GpuGlRhi::submit(const DrawCmd &cmd, const CmdData &data) -> void {
    if (impl_ == nullptr || impl_->failed) {
        return;
    }
    impl_->translate(cmd, data);
}

auto GpuGlRhi::begin_frame(int device_width, int device_height, float scale) -> bool {
    if (impl_ == nullptr || impl_->failed || device_width <= 0 || device_height <= 0) {
        return false;
    }
    impl_->scale = scale > 0.0F ? scale : 1.0F;
    if (!impl_->ensure_framebuffer(device_width, device_height)) {
        return false;
    }
    // 帧零基底：整帧清透明（与软件 begin 后零基底同源；窗口底色由 DL 内的 FillRect 承担）。
    impl_->gl.bind_framebuffer(FRAMEBUFFER, impl_->msaa_fbo);
    impl_->gl.clear_color(0.0F, 0.0F, 0.0F, 0.0F);
    impl_->gl.clear(COLOR_BUFFER_BIT);
    impl_->msaa_dirty = true;  // 清屏使上一帧 resolve 内容作废：效果采样必须重新 resolve
    impl_->stats = FrameStats{};
    impl_->clip_stack.clear();
    impl_->alpha = 1.0;
    return !impl_->failed;
}

auto GpuGlRhi::end_frame() -> void {
    if (impl_ == nullptr || impl_->failed) {
        return;
    }
    impl_->flush_batch();
    if (impl_->failed) {
        return;
    }
    const GLsizei_ w = impl_->device_w;
    const GLsizei_ h = impl_->device_h;
    // resolve：MSAA 渲染缓冲 → 普通纹理 FBO（read_pixels / 后续效果 pass 采样依赖）。
    impl_->gl.bind_framebuffer(READ_FRAMEBUFFER, impl_->msaa_fbo);
    impl_->gl.bind_framebuffer(DRAW_FRAMEBUFFER, impl_->resolve_fbo);
    impl_->gl.blit_framebuffer(0, 0, w, h, 0, 0, w, h, COLOR_BUFFER_BIT, NEAREST);
    // 呈现：blit 至默认帧缓冲（GLFW 侧 present() 只做 swapBuffers）。
    impl_->gl.bind_framebuffer(DRAW_FRAMEBUFFER, 0);
    impl_->gl.blit_framebuffer(0, 0, w, h, 0, 0, w, h, COLOR_BUFFER_BIT, NEAREST);
    impl_->check_error("end_frame");
}

auto GpuGlRhi::stats() const -> FrameStats {
    return impl_ != nullptr ? impl_->stats : FrameStats{};
}

auto GpuGlRhi::read_pixels(std::vector<std::uint8_t> &out) -> bool {
    if (impl_ == nullptr || impl_->failed || impl_->resolve_fbo == 0 || impl_->device_w <= 0
        || impl_->device_h <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(impl_->device_w) * static_cast<std::size_t>(impl_->device_h) * 4U);
    impl_->gl.bind_framebuffer(READ_FRAMEBUFFER, impl_->resolve_fbo);
    impl_->gl.read_pixels(0, 0, impl_->device_w, impl_->device_h, RGBA, UNSIGNED_BYTE, out.data());
    impl_->check_error("read_pixels");
    return !impl_->failed;
}

}  // namespace aurora::rhi
