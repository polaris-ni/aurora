#include "aurora/render/rhi/wgpu_rhi.h"

// 本 TU 整体裁切于 AURORA_BACKEND_GPU_WGPU（区别于恒编译的 gpu_gl_rhi.cpp）：实现链接
// third_party/wgpu-native 的 Rust 静态库，特性关闭时既无头也不产生符号（公共头同样裁切）。
//
// 骨架范围（基础提交）：instance/adapter/device 异步初始化 + surface/离屏双模目标、
// Solid 管线（FillRect/ClearRect）+ 裁剪栈（PushClip/PushClipRounded/PopClip）+ SetAlpha、
// read_pixels（离屏读回）。其余 18 条命令计入 skipped_cmds，随「全命令族」切片补齐，
// 语义以 gpu_gl_rhi.cpp 同源 GLSL/批切分为参照（specification/03-layout-render.md §8.7）。

#ifdef AURORA_BACKEND_GPU_WGPU

#include <webgpu.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "aurora/core/log.h"

namespace aurora::rhi {

namespace {

// ---- WGSL 着色器源（逐项移植 gpu_gl_rhi.cpp 的 GLSL 公式，逻辑 dp 坐标系） ----

constexpr const char *kWgslSolid = R"(
struct Globals {
    logical: vec2f,
    _pad0: vec2f,
    clip: vec4f,      // x,y,w,h（逻辑 dp）
    clip_ctl: vec4f,  // (radius, on, aa, _)
};
@group(0) @binding(0) var<uniform> g: Globals;

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
    let ndc = vec2f(a_pos.x / g.logical.x * 2.0 - 1.0,
                    1.0 - a_pos.y / g.logical.y * 2.0);
    o.fb = vec4f(ndc, 0.0, 1.0);
    return o;
}

fn sd_rbox(p: vec2f, b: vec2f, r: f32) -> f32 {
    let q: vec2f = abs(p) - b + vec2f(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2f(0.0))) - r;
}

// 实心 quad + shader 内 SDF 裁剪（不用 scissor，与软件路径逐像素交叠语义同源）；
// 不 discard——alpha=0 经 blend 写 dst=dst。WebGPU 帧缓冲行 0 在顶部：y 翻转后逻辑
// y=0 即首行，无 GL FBO 的底部原点回转问题。
@fragment
fn fs_solid(in: VSOut) -> @location(0) vec4f {
    var c = in.color;
    if (g.clip_ctl.y > 0.5) {
        let ctr = g.clip.xy + g.clip.zw * 0.5;
        let d = sd_rbox(in.pos - ctr, g.clip.zw * 0.5, g.clip_ctl.x);
        let aa_cov = 1.0 - smoothstep(-0.5, 0.5, d);
        let cov = select(step(d, 0.0), aa_cov, g.clip_ctl.z > 0.5);
        c.a = c.a * cov;
    }
    return c;
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

// uniform 块（与 WGSL Globals 逐字段对齐：vec2f 后补 8 字节 pad，总 48 字节）。
struct Globals {
    float logical_x = 1.0F;
    float logical_y = 1.0F;
    float pad0[2] = {};
    float clip[4] = {};
    float clip_ctl[4] = {};
};
static_assert(sizeof(Globals) == 48, "uniform block must stay 48 bytes");

constexpr std::uint64_t kUniformAlign = 256;  // uniform buffer offset 对齐下限

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

    WgpuRhiOptions options;

    // ---- surface（宿主模式）----
    WGPUSurface surface = nullptr;
    bool surface_configured = false;
    int surface_cw = 0;
    int surface_ch = 0;
    WGPUTextureFormat surface_format = WGPUTextureFormat_Undefined;
    WGPUPresentMode present_mode = WGPUPresentMode_Fifo;

    // ---- 离屏目标 ----
    WGPUTexture target_tex = nullptr;
    WGPUTextureView target_view = nullptr;
    int target_w = 0;
    int target_h = 0;

    // ---- 共享管线资源 ----
    WGPUShaderModule shader_solid = nullptr;
    WGPUBindGroupLayout bgl = nullptr;
    WGPUPipelineLayout pipeline_layout = nullptr;
    WGPURenderPipeline pipe_solid = nullptr;           // SRC_ALPHA 混合
    WGPURenderPipeline pipe_solid_noblend = nullptr;   // ClearRect 直写零
    WGPUTextureFormat pipeline_format = WGPUTextureFormat_Undefined;

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
    WGPURenderPassEncoder pass = nullptr;
    WGPUTextureView frame_view = nullptr;  // 本帧目标 view（持有引用，帧尾释放）

    struct ClipState {
        bool on = false;
        Rect rect{};
        float radius = 0.0F;
        bool aa = true;
        auto operator==(const ClipState &) const -> bool = default;
    };
    std::vector<ClipState> clip_stack;
    double alpha = 1.0;
    float scale = 1.0F;
    float logical_w = 1.0F;
    float logical_h = 1.0F;

    // 当前批（骨架只有 Solid 管线，blend_off 区分两变体）
    std::vector<Vertex> verts;
    ClipState key_clip{};
    bool key_blend_off = false;
    bool key_active = false;

    WgpuRhi::FrameStats stats;
    std::unordered_set<int> warned_kinds;  // 未实现命令每 kind 只警告一次

    // ---- readback（离屏诊断通道）----
    WGPUBuffer readback = nullptr;
    std::uint64_t readback_cap = 0;
    std::uint64_t readback_len = 0;  // 本帧 map 的实际长度
    std::uint32_t readback_bpr = 0;  // bytes_per_row（256 对齐）
    bool map_done = false;           // mapAsync 回调已触发（pump 轮询置位）
    bool map_armed = false;          // 本帧已登记读回拷贝与 map 请求

    int glyph_page_size = 1024;  // 预留：图集页边长（全命令族切片的文本管线使用）

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
        if (!init_shared_pipeline()) {
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
        AURORA_WGPU_RELEASE(pipe_solid, wgpuRenderPipelineRelease)
        AURORA_WGPU_RELEASE(pipe_solid_noblend, wgpuRenderPipelineRelease)
        AURORA_WGPU_RELEASE(pipeline_layout, wgpuPipelineLayoutRelease)
        AURORA_WGPU_RELEASE(bgl, wgpuBindGroupLayoutRelease)
        AURORA_WGPU_RELEASE(shader_solid, wgpuShaderModuleRelease)
        AURORA_WGPU_RELEASE(frame_view, wgpuTextureViewRelease)
        AURORA_WGPU_RELEASE(target_view, wgpuTextureViewRelease)
        AURORA_WGPU_RELEASE(target_tex, wgpuTextureRelease)
        AURORA_WGPU_RELEASE(surface, wgpuSurfaceRelease)
        AURORA_WGPU_RELEASE(queue, wgpuQueueRelease)
        AURORA_WGPU_RELEASE(device, wgpuDeviceRelease)
        AURORA_WGPU_RELEASE(adapter, wgpuAdapterRelease)
        AURORA_WGPU_RELEASE(instance, wgpuInstanceRelease)
        device_ok = false;
    }

    // ---- 异步初始化（v29 全异步：回调 + wgpuInstanceWaitAny 单线程泵）----

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
        if (status != WGPUMapAsyncStatus_Success) {
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
        opts.featureLevel = options.backend == WgpuRhiOptions::Backend::GLES
                                ? WGPUFeatureLevel_Compatibility
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

    WGPUBackendType adapter_backend_ = WGPUBackendType_Undefined;

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
        src.hinstance = nullptr;
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
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm
                || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
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
        WGPUCompositeAlphaMode amode = WGPUCompositeAlphaMode_Auto;
        if (caps.alphaModeCount > 0) {
            amode = caps.alphaModes[0];
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
        cfg.alphaMode = amode;
        cfg.presentMode = present_mode;
        wgpuSurfaceConfigure(surface, &cfg);  // 错误在下帧 getCurrentTexture 状态中暴露
        surface_format = fmt;
        surface_cw = w;
        surface_ch = h;
        surface_configured = true;
        return true;
    }

    // ---- 管线 ----

    [[nodiscard]] bool init_shared_pipeline() {
        WGPUShaderSourceWGSL wsrc{};
        wsrc.chain.sType = WGPUSType_ShaderSourceWGSL;
        wsrc.code = sv(kWgslSolid);
        WGPUShaderModuleDescriptor mdesc{};
        mdesc.nextInChain = &wsrc.chain;
        shader_solid = wgpuDeviceCreateShaderModule(device, &mdesc);
        if (shader_solid == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "WGSL solid module creation failed");
            return false;
        }
        WGPUBindGroupLayoutEntry bge{};
        bge.binding = 0;
        bge.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        bge.buffer.type = WGPUBufferBindingType_Uniform;
        bge.buffer.minBindingSize = sizeof(Globals);
        WGPUBindGroupLayoutDescriptor bgld{};
        bgld.entryCount = 1;
        bgld.entries = &bge;
        bgl = wgpuDeviceCreateBindGroupLayout(device, &bgld);
        WGPUPipelineLayoutDescriptor playout{};
        playout.bindGroupLayoutCount = 1;
        playout.bindGroupLayouts = &bgl;
        pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &playout);
        if (bgl == nullptr || pipeline_layout == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "layout creation failed");
            return false;
        }
        return true;
    }

    // 目标格式变化时重建两条管线（离屏 RGBA8 / surface 协商格式）。
    [[nodiscard]] bool ensure_pipelines(WGPUTextureFormat format) {
        if (pipe_solid != nullptr && pipeline_format == format) {
            return true;
        }
        AURORA_WGPU_RELEASE(pipe_solid, wgpuRenderPipelineRelease)
        AURORA_WGPU_RELEASE(pipe_solid_noblend, wgpuRenderPipelineRelease)

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
        vs.module = shader_solid;
        vs.entryPoint = sv("vs_main");
        vs.bufferCount = 1;
        vs.buffers = &vbl;

        WGPUBlendComponent bc{};
        bc.operation = WGPUBlendOperation_Add;
        bc.srcFactor = WGPUBlendFactor_SrcAlpha;
        bc.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        WGPUBlendState blend{};
        blend.color = bc;
        blend.alpha = bc;

        WGPUColorTargetState target{};
        target.format = format;
        target.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fs{};
        fs.module = shader_solid;
        fs.entryPoint = sv("fs_solid");
        fs.targetCount = 1;

        WGPUMultisampleState ms{};
        ms.count = 1;
        ms.mask = 0xFFFFFFFFU;  // ⚠️ 必须显式全采样掩码：descriptor 非 optional，零初始化 = 0 掩码 → 像素全丢弃

        auto build = [&](bool blend_on) -> WGPURenderPipeline {
            WGPUColorTargetState t2 = target;
            t2.blend = blend_on ? &blend : nullptr;
            WGPUFragmentState f2 = fs;
            f2.targets = &t2;
            WGPURenderPipelineDescriptor d{};
            d.layout = pipeline_layout;
            d.vertex = vs;
            d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            d.primitive.cullMode = WGPUCullMode_None;
            d.multisample = ms;
            d.fragment = &f2;
            return wgpuDeviceCreateRenderPipeline(device, &d);
        };
        pipe_solid = build(true);
        pipe_solid_noblend = build(false);
        if (pipe_solid == nullptr || pipe_solid_noblend == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "render pipeline creation failed (format 0x",
                             hex_u32(static_cast<std::uint32_t>(format)), ")");
            return false;
        }
        pipeline_format = format;
        return true;
    }

    // ---- 目标管理 ----

    [[nodiscard]] bool ensure_offscreen_target(int w, int h) {
        if (target_tex != nullptr && target_w == w && target_h == h) {
            return true;
        }
        AURORA_WGPU_RELEASE(target_view, wgpuTextureViewRelease)
        AURORA_WGPU_RELEASE(target_tex, wgpuTextureRelease)
        WGPUTextureDescriptor td{};
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = static_cast<std::uint32_t>(w);
        td.size.height = static_cast<std::uint32_t>(h);
        td.size.depthOrArrayLayers = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        target_tex = wgpuDeviceCreateTexture(device, &td);
        if (target_tex == nullptr) {
            AURORA_LOG_ERROR("gpu-wgpu", "offscreen texture ", w, "x", h, " creation failed");
            return false;
        }
        target_view = wgpuTextureCreateView(target_tex, nullptr);
        if (target_view == nullptr) {
            AURORA_WGPU_RELEASE(target_tex, wgpuTextureRelease)
            AURORA_LOG_ERROR("gpu-wgpu", "offscreen texture view failed");
            return false;
        }
        target_w = w;
        target_h = h;
        return true;
    }

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
        const Vertex base[4] = {
            Vertex{x0, y0, 0.0F, 0.0F, c.r, c.g, c.b, c.a},
            Vertex{x1, y0, 1.0F, 0.0F, c.r, c.g, c.b, c.a},
            Vertex{x1, y1, 1.0F, 1.0F, c.r, c.g, c.b, c.a},
            Vertex{x0, y1, 0.0F, 1.0F, c.r, c.g, c.b, c.a},
        };
        verts.insert(verts.end(), base, base + 4);
    }

    void begin_batch(const ClipState &k, bool blend_off) {
        if (key_active && (key_clip != k || key_blend_off != blend_off)) {
            flush_batch();
        }
        key_clip = k;
        key_blend_off = blend_off;
        key_active = true;
    }

    void flush_batch() {
        if (verts.empty() || pass == nullptr) {
            verts.clear();
            key_active = false;
            return;
        }
        std::vector<Vertex> tri;
        tri.reserve(verts.size() / 4 * 6);
        for (std::size_t q = 0; q + 3 < verts.size(); q += 4) {
            tri.push_back(verts[q]);
            tri.push_back(verts[q + 1]);
            tri.push_back(verts[q + 2]);
            tri.push_back(verts[q]);
            tri.push_back(verts[q + 2]);
            tri.push_back(verts[q + 3]);
        }
        verts.clear();
        key_active = false;

        const std::uint64_t vbytes = tri.size() * sizeof(Vertex);
        const std::uint64_t voff = vstage.size();
        if (!grow_vertex_buffer(voff + vbytes)) {
            return;
        }
        wgpuQueueWriteBuffer(queue, vertex_buf, voff, tri.data(), static_cast<std::size_t>(vbytes));
        vstage.insert(vstage.end(), reinterpret_cast<std::uint8_t *>(tri.data()),
                      reinterpret_cast<std::uint8_t *>(tri.data()) + vbytes);

        const std::uint64_t uoff_raw = ustage.size();
        const std::uint64_t uoff = (uoff_raw + (kUniformAlign - 1)) & ~(kUniformAlign - 1);
        if (!grow_uniform_buffer(uoff + sizeof(Globals))) {
            return;
        }
        Globals gu{};
        gu.logical_x = logical_w;
        gu.logical_y = logical_h;
        gu.clip[0] = key_clip.on ? key_clip.rect.origin.x : 0.0F;
        gu.clip[1] = key_clip.on ? key_clip.rect.origin.y : 0.0F;
        gu.clip[2] = key_clip.on ? key_clip.rect.size.width : 0.0F;
        gu.clip[3] = key_clip.on ? key_clip.rect.size.height : 0.0F;
        gu.clip_ctl[0] = key_clip.radius;
        gu.clip_ctl[1] = key_clip.on ? 1.0F : 0.0F;
        gu.clip_ctl[2] = key_clip.aa ? 1.0F : 0.0F;
        ustage.resize(static_cast<std::size_t>(uoff));
        const auto *gb = reinterpret_cast<const std::uint8_t *>(&gu);
        ustage.insert(ustage.end(), gb, gb + sizeof(Globals));
        wgpuQueueWriteBuffer(queue, uniform_buf, uoff, &gu, sizeof(Globals));

        WGPUBindGroupEntry bg_entry{};
        bg_entry.binding = 0;
        bg_entry.buffer = uniform_buf;
        bg_entry.offset = uoff;
        bg_entry.size = sizeof(Globals);
        WGPUBindGroupDescriptor bgd{};
        bgd.layout = bgl;
        bgd.entryCount = 1;
        bgd.entries = &bg_entry;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device, &bgd);
        if (bg == nullptr) {
            return;
        }

        wgpuRenderPassEncoderSetPipeline(pass, key_blend_off ? pipe_solid_noblend : pipe_solid);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buf, voff, vbytes);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, static_cast<std::uint32_t>(tri.size()), 1, 0, 0);
        wgpuBindGroupRelease(bg);  // 已录制命令内部持有引用

        stats.draw_calls++;
        stats.vertices += static_cast<std::uint32_t>(tri.size());
    }

    // ---- 命令翻译 ----

    void translate(const DrawCmd &cmd, const CmdData & /*data*/) {
        switch (cmd.kind) {
            case CmdKind::FillRect: {
                begin_batch(effective_clip(), false);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, bake_alpha(cmd.color, alpha));
                break;
            }
            case CmdKind::ClearRect: {
                // 区域归零（RGBA 全零，不走混合、不受裁剪/alpha 影响）——同 GL 路径口径。
                begin_batch(ClipState{}, true);
                push_quad(cmd.bounds.origin.x, cmd.bounds.origin.y, cmd.bounds.origin.x + cmd.bounds.size.width,
                          cmd.bounds.origin.y + cmd.bounds.size.height, Color{0, 0, 0, 0});
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
            // ---- 以下 18 条随「全命令族」切片补齐（语义参照 gpu_gl_rhi.cpp）----
            case CmdKind::DrawRect:
            case CmdKind::DrawLine:
            case CmdKind::RoundedBorder:
            case CmdKind::DrawText:
            case CmdKind::DrawImage:
            case CmdKind::LinearGradient:
            case CmdKind::RadialGradient:
            case CmdKind::Shadow:
            case CmdKind::BlurRegion:
            case CmdKind::BlendRegion:
            case CmdKind::MaskRegion:
            case CmdKind::Composite:
            case CmdKind::Polyline:
            case CmdKind::Sector:
            case CmdKind::BeginLayer:
            case CmdKind::EndLayer:
            case CmdKind::DrawLayer:
                skip_cmd(cmd.kind);
                break;
        }
    }

    void skip_cmd(CmdKind kind) {
        stats.skipped_cmds++;
        const int k = static_cast<int>(kind);
        if (warned_kinds.insert(k).second) {
            AURORA_LOG_WARN("gpu-wgpu", "CmdKind 0x", hex_u32(static_cast<std::uint32_t>(k)),
                            " not implemented in skeleton; command skipped");
        }
    }

    // ---- 帧生命周期 ----

    [[nodiscard]] bool begin_frame(int device_width, int device_height, float scale_) {
        if (!device_ok || device_width <= 0 || device_height <= 0) {
            return false;
        }
        stats = {};
        clip_stack.clear();
        alpha = 1.0;
        key_active = false;
        verts.clear();
        vstage.clear();
        ustage.clear();
        scale = scale_ > 0.0F ? scale_ : 1.0F;
        logical_w = static_cast<float>(device_width) / scale;
        logical_h = static_cast<float>(device_height) / scale;

        WGPUTextureFormat frame_format = WGPUTextureFormat_Undefined;
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
            frame_view = wgpuTextureCreateView(st.texture, nullptr);
            wgpuTextureRelease(st.texture);  // view 已接管引用
            if (frame_view == nullptr) {
                return false;
            }
            frame_format = surface_format;
        } else {
            if (!ensure_offscreen_target(device_width, device_height)) {
                return false;
            }
            AURORA_WGPU_RELEASE(frame_view, wgpuTextureViewRelease)
            frame_view = target_view;
            wgpuTextureViewAddRef(frame_view);
            frame_format = WGPUTextureFormat_RGBA8Unorm;
        }
        if (!ensure_pipelines(frame_format)) {
            device_ok = false;
            return false;
        }

        encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        if (encoder == nullptr) {
            return false;
        }
        WGPURenderPassColorAttachment att{};
        att.view = frame_view;
        att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        att.loadOp = WGPULoadOp_Clear;
        att.storeOp = WGPUStoreOp_Store;
        att.clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};
        WGPURenderPassDescriptor pd{};
        pd.colorAttachmentCount = 1;
        pd.colorAttachments = &att;
        pass = wgpuCommandEncoderBeginRenderPass(encoder, &pd);
        if (pass == nullptr) {
            AURORA_WGPU_RELEASE(encoder, wgpuCommandEncoderRelease)
            return false;
        }
        return true;
    }

    void end_frame() {
        if (!device_ok || pass == nullptr) {
            return;
        }
        flush_batch();
        wgpuRenderPassEncoderEnd(pass);
        AURORA_WGPU_RELEASE(pass, wgpuRenderPassEncoderRelease)

        if (surface == nullptr) {
            arm_readback();  // 离屏诊断通道：帧尾拷贝，read_pixels 处等待 map
        }

        WGPUCommandBuffer cmdbuf = wgpuCommandEncoderFinish(encoder, nullptr);
        AURORA_WGPU_RELEASE(encoder, wgpuCommandEncoderRelease)
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
    }

    // 目标纹理 → MAP_READ 缓冲拷贝（编入本帧 command buffer）。⚠️ mapAsync 必须延后到
    // submit 之后（map_readback）：提交前登记映射会让 wgpu-core 立即完成映射，
    // 随后的 submit 因「写已映射缓冲」直接 Validation Error panic。
    void arm_readback() {
        const std::uint32_t bpr = (static_cast<std::uint32_t>(target_w) * 4U + 255U) & ~255U;
        const std::uint64_t len = static_cast<std::uint64_t>(bpr) * static_cast<std::uint32_t>(target_h);
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
        src.texture = target_tex;
        src.mipLevel = 0;
        src.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = bpr;
        dst.layout.rowsPerImage = static_cast<std::uint32_t>(target_h);
        WGPUExtent3D extent{};
        extent.width = static_cast<std::uint32_t>(target_w);
        extent.height = static_cast<std::uint32_t>(target_h);
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
        if (!device_ok || surface != nullptr || readback == nullptr || !map_armed || target_w <= 0
            || target_h <= 0) {
            return false;
        }
        map_armed = false;
        if (!map_done && !pump(map_done, 2000)) {
            return false;
        }
        const auto w = static_cast<std::size_t>(target_w);
        const auto h = static_cast<std::size_t>(target_h);
        out.assign(w * h * 4U, 0);
        const auto *base =
            static_cast<const std::uint8_t *>(wgpuBufferGetConstMappedRange(readback, 0, static_cast<std::size_t>(readback_len)));
        if (base == nullptr) {
            return false;
        }
        for (std::size_t y = 0; y < h; ++y) {
            std::memcpy(out.data() + y * w * 4U, base + y * readback_bpr, w * 4U);
        }
        wgpuBufferUnmap(readback);
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
    if (impl_ == nullptr || !impl_->device_ok || impl_->pass == nullptr) {
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
        impl_->glyph_page_size = side;
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

// ---- 流式纹理槽：随「全命令族」切片的 Image 管线补齐（当前恒 0 = 不可用）----

auto WgpuRhi::acquire_stream_image(std::uint64_t /*key*/, int /*width*/, int /*height*/) -> StreamImageId {
    return 0;
}

auto WgpuRhi::update_stream_image(StreamImageId /*id*/, const std::uint8_t * /*pixels*/,
                                  std::size_t /*stride_bytes*/, int /*x*/, int /*y*/, int /*w*/, int /*h*/)
    -> void {}

auto WgpuRhi::release_stream_image(StreamImageId /*id*/) -> void {}

auto WgpuRhi::import_native_surface(const NativeSurfaceFrame & /*frame*/) -> StreamImageId {
    AURORA_LOG_WARN("gpu-wgpu",
                    "import_native_surface: no external texture import in wgpu-native v29 C API; CPU upload fallback");
    return 0;
}

}  // namespace aurora::rhi

#endif  // AURORA_BACKEND_GPU_WGPU
