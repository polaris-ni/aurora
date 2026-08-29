#include "aurora/window/d3d11_surface.h"

#ifdef AURORA_BACKEND_D3D11

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3dcompiler.h>
#include <span>
#include <utility>

#include "aurora/core/log.h"

namespace aurora {

namespace {

template<typename T> auto safe_release(T *&p) -> void {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

// 全屏三角形：无顶点缓冲，经 SV_VertexID 生成；纹理采样做任意比例 GPU 缩放。
constexpr auto AURORA_VS = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 p = float2(id == 2 ? 3.0f : -1.0f, id == 1 ? 3.0f : -1.0f);
    VSOut o;
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv = float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f); // 翻转 v 以匹配 CPU 顶行优先
    return o;
}
)HLSL";

constexpr auto AURORA_PS = R"HLSL(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(VSOut i) : SV_Target {
    return tex.Sample(samp, i.uv);
}
)HLSL";

auto compile_shader(const char *src, const char *entry, const char *profile, ID3DBlob **blob) -> bool {
    ID3DBlob *err = nullptr;
    const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
    if (FAILED(hr)) {
        if (err != nullptr) {
            AURORA_LOG_ERROR("d3d11", "shader compile failed: %s", static_cast<const char *>(err->GetBufferPointer()));
            err->Release();
        }
        return false;
    }
    return true;
}

auto make_sampler_desc() -> D3D11_SAMPLER_DESC {
    D3D11_SAMPLER_DESC sd;
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = 0.0f;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.0f;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return sd;
}

auto make_blend_desc() -> D3D11_BLEND_DESC {
    D3D11_BLEND_DESC bd;
    bd.AlphaToCoverageEnable = FALSE;
    bd.IndependentBlendEnable = FALSE;
    for (auto &rt : bd.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    return bd;
}

} // namespace

D3D11Surface::D3D11Surface(int width, int height, const std::string &title, const WindowStyleOptions &style) {
    m_win = std::make_unique<Win32Window>(width, height, title, style);
    if (m_win->hwnd() == nullptr) {
        AURORA_LOG_ERROR("d3d11", "Win32Window creation failed");
        return;
    }
    m_ok = init_device(width, height);
    if (!m_ok) {
        AURORA_LOG_ERROR("d3d11", "D3D11 device init failed (no adapter?)");
    }
}

D3D11Surface::~D3D11Surface() { release_device(); }

auto D3D11Surface::release_device() -> void {
    safe_release(m_bs);
    safe_release(m_layout);
    safe_release(m_samp);
    safe_release(m_ps);
    safe_release(m_vs);
    safe_release(m_src_srv);
    safe_release(m_src);
    safe_release(m_rtv);
    safe_release(m_rt);
    safe_release(m_swap);
    safe_release(m_ctx);
    safe_release(m_device);
}

auto D3D11Surface::poll_platform_events() -> void {
    // device-lost 恢复：在 present_root 外的安全时机重建，避开重入护栏。
    if (m_need_reinit) {
        try_recover_device();
    }
    m_win->poll_platform_events();
}

auto D3D11Surface::try_recover_device() -> void {
    m_need_reinit = false;
    release_device();
    const Size sz = m_win->size();
    m_ok = init_device(static_cast<int>(sz.width), static_cast<int>(sz.height));
    if (m_ok) {
        AURORA_LOG_INFO("d3d11", "device recovered after device-lost");
        // 强制一次全量重渲染上屏：新设备的源纹理/后缓冲为空，不可沿用增量脏区。
        m_dirty.clear();
        if (m_present_request) {
            m_present_request();
        }
    } else {
        AURORA_LOG_ERROR("d3d11", "device recovery failed; surface stays unavailable");
    }
}

auto D3D11Surface::init_device(int width, int height) -> bool {
    constexpr std::array<D3D_FEATURE_LEVEL, 4> levels = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                          D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    // 优先硬件适配器；不可用时回退 WARP（软件光栅），保证无 GPU 环境（CI/无头）也能跑通上屏路径。
    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
                          static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &m_device, nullptr, &m_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
                               static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &m_device, nullptr, &m_ctx);
    }
    if (FAILED(hr) || (m_device == nullptr) || (m_ctx == nullptr)) {
        return false;
    }
    // 取 DXGI 工厂以创建交换链。
    IDXGIDevice *dxgi_dev = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    bool ok = false;
    if (SUCCEEDED(m_device->QueryInterface(&dxgi_dev))) {
        if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
            // 显式 IID（等价于 IID_IDXGIFactory2），避免 IID_PPV_ARGS 展开出的 __uuidof 扩展 token 警告。
            static constexpr IID iid_factory2 = { .Data1 = 0x50c83a1c,
                                                  .Data2 = 0xe3a3,
                                                  .Data3 = 0x4d23,
                                                  .Data4 = { 0xad, 0x65, 0x9c, 0xd2, 0x6a, 0x9c, 0x39, 0xee } };
            void *factory_tmp = nullptr;
            if (SUCCEEDED(adapter->GetParent(iid_factory2, &factory_tmp))) {
                factory = static_cast<IDXGIFactory2 *>(factory_tmp);
                ok = true;
            }
        }
    }
    safe_release(dxgi_dev);
    safe_release(adapter);
    if (!ok) {
        safe_release(factory);
        return false;
    }

    // 编译着色器。
    ID3DBlob *vs_blob = nullptr;
    ID3DBlob *ps_blob = nullptr;
    if (!compile_shader(AURORA_VS, "VSMain", "vs_5_0", &vs_blob) ||
        !compile_shader(AURORA_PS, "PSMain", "ps_5_0", &ps_blob)) {
        safe_release(vs_blob);
        safe_release(ps_blob);
        safe_release(factory);
        return false;
    }
    (void)m_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs);
    (void)m_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps);
    // 空输入布局（VS 仅用 SV_VertexID，无顶点输入）。
    (void)m_device->CreateInputLayout(nullptr, 0, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_layout);
    safe_release(vs_blob);
    safe_release(ps_blob);
    if ((m_vs == nullptr) || (m_ps == nullptr) || (m_layout == nullptr)) {
        safe_release(factory);
        return false;
    }

    // 线性采样器（GPU 缩放）+ 不透明混合。
    const D3D11_SAMPLER_DESC sd = make_sampler_desc();
    (void)m_device->CreateSamplerState(&sd, &m_samp);

    const D3D11_BLEND_DESC bd = make_blend_desc();
    (void)m_device->CreateBlendState(&bd, &m_bs);

    // 创建交换链。
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SampleDesc.Count = 1;
    scd.Scaling = DXGI_SCALING_STRETCH; // 由着色器做任意比例缩放，DXGI 不letterbox
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * m_win->scale_factor())));
    const int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * m_win->scale_factor())));
    scd.Width = static_cast<UINT>(w);
    scd.Height = static_cast<UINT>(h);
    hr = factory->CreateSwapChainForHwnd(m_device, static_cast<HWND>(m_win->hwnd()), &scd, nullptr, nullptr, &m_swap);
    safe_release(factory);
    if (FAILED(hr) || (m_swap == nullptr)) {
        return false;
    }
    m_dev_w = w;
    m_dev_h = h;
    return ensure_swap_chain(w, h);
}

auto D3D11Surface::ensure_swap_chain(int w, int h) -> bool {
    if ((m_swap == nullptr) || (w <= 0 || h <= 0)) {
        return false;
    }
    // 尺寸变化：重建交换链与源纹理，避免拉伸模糊（DWM 由着色器缩放）。
    if ((m_rt != nullptr) && m_dev_w == w && m_dev_h == h && (m_src != nullptr)) {
        return true;
    }
    safe_release(m_rtv);
    safe_release(m_rt);
    safe_release(m_src_srv);
    safe_release(m_src);

    ID3D11Texture2D *back = nullptr;
    // 显式 IID（等价于 IID_ID3D11Texture2D），避免 IID_PPV_ARGS 展开出的 __uuidof 扩展 token 警告。
    static constexpr IID iid_tex2_d = { .Data1 = 0xdc8e63f3,
                                        .Data2 = 0xd12b,
                                        .Data3 = 0x4952,
                                        .Data4 = { 0xb4, 0x7b, 0x8e, 0x7f, 0xfa, 0x51, 0x18, 0xa8 } };
    void *back_tmp = nullptr;
    if (FAILED(m_swap->GetBuffer(0, iid_tex2_d, &back_tmp))) {
        return false;
    }
    back = static_cast<ID3D11Texture2D *>(back_tmp);
    (void)m_device->CreateRenderTargetView(back, nullptr, &m_rtv);
    safe_release(back);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    (void)m_device->CreateTexture2D(&td, nullptr, &m_src);
    if (m_src != nullptr) {
        (void)m_device->CreateShaderResourceView(m_src, nullptr, &m_src_srv);
    }
    m_dev_w = w;
    m_dev_h = h;
    return (m_rtv != nullptr) && (m_src != nullptr) && (m_src_srv != nullptr);
}

auto D3D11Surface::begin_frame(int width, int height) -> Result<bool> {
    const float s = scale_factor();
    m_dev_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * s)));
    m_dev_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * s)));
    m_painter.begin(m_dev_w, m_dev_h);
    if (m_ok) {
        ensure_swap_chain(m_dev_w, m_dev_h);
    }
    return Result<bool>{ true };
}

auto D3D11Surface::upload_region(int x, int y, int w, int h) const -> bool {
    if (m_src == nullptr) {
        return false;
    }
    x = std::clamp(x, 0, m_dev_w);
    y = std::clamp(y, 0, m_dev_h);
    w = std::clamp(w, 0, m_dev_w - x);
    h = std::clamp(h, 0, m_dev_h - y);
    if (w <= 0 || h <= 0) {
        return false;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    // Painter 自下而上行序（RGBA）→ 纹理顶行优先（BGRA）。
    const std::span painter_data(m_painter.data(),
                                 static_cast<std::size_t>(m_dev_w) * static_cast<std::size_t>(m_dev_h) * 4u);
    const std::span buf_span(buf);
    for (int ry = 0; ry < h; ++ry) {
        const int src_y = m_dev_h - 1 - (y + ry);
        const std::size_t src_row_start =
            ((static_cast<std::size_t>(src_y) * static_cast<std::size_t>(m_dev_w)) + static_cast<std::size_t>(x)) * 4u;
        const auto src_row = painter_data.subspan(src_row_start, static_cast<std::size_t>(w) * 4u);
        const std::size_t dst_row_start = static_cast<std::size_t>(ry) * static_cast<std::size_t>(w) * 4u;
        const auto dst_row = buf_span.subspan(dst_row_start, static_cast<std::size_t>(w) * 4u);
        for (std::size_t cx = 0; std::cmp_less(cx ,w); ++cx) {
            const std::size_t s = cx * 4u;
            const std::size_t d = cx * 4u;
            dst_row[d + 0] = src_row[s + 2]; // B NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            dst_row[d + 1] = src_row[s + 1]; // G NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            dst_row[d + 2] = src_row[s + 0]; // R NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            dst_row[d + 3] = src_row[s + 3]; // A NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        }
    }
    D3D11_BOX box{};
    box.left = static_cast<UINT>(x);
    box.top = static_cast<UINT>(y);
    box.front = 0;
    box.right = static_cast<UINT>(x + w);
    box.bottom = static_cast<UINT>(y + h);
    box.back = 1;
    m_ctx->UpdateSubresource(m_src, 0, &box, buf.data(), static_cast<UINT>(w * 4), static_cast<UINT>(w * h * 4));
    return true;
}

auto D3D11Surface::present() -> Result<bool> {
    if (!m_ok || (m_swap == nullptr)) {
        return make_error(ErrorCode::PlatformUnavailable, "D3D11Surface::present: device unavailable.",
                          "No D3D11 adapter; use Win32Surface or HeadlessSurface.", "aurora/window/d3d11_surface.h");
    }
    if ((m_rtv == nullptr) || (m_src_srv == nullptr)) {
        return make_error(ErrorCode::PlatformUnavailable, "D3D11Surface::present: swap chain not ready.", "",
                          "aurora/window/d3d11_surface.h");
    }
    // 增量上传：脏矩形非空时仅更新变化区，否则整帧上传。
    if (m_dirty.empty()) {
        (void)upload_region(0, 0, m_dev_w, m_dev_h);
    } else {
        for (const Rect &r : m_dirty) {
            (void)upload_region(static_cast<int>(r.origin.x), static_cast<int>(r.origin.y),
                                static_cast<int>(r.size.width), static_cast<int>(r.size.height));
        }
    }
    m_dirty.clear();

    // 浅色清屏后绘制全屏三角形（GPU 缩放采样源纹理）。
    m_ctx->OMSetRenderTargets(1, &m_rtv, nullptr);
    constexpr std::array<float, 4> clear = { 0.96f, 0.96f, 0.96f, 1.0f };
    m_ctx->ClearRenderTargetView(m_rtv, clear.data());
    m_ctx->IASetInputLayout(m_layout);
    m_ctx->VSSetShader(m_vs, nullptr, 0);
    m_ctx->PSSetShader(m_ps, nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, &m_src_srv);
    m_ctx->PSSetSamplers(0, 1, &m_samp);
    m_ctx->OMSetBlendState(m_bs, nullptr, 0xFFFFFFFFu);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->Draw(3, 0);

    const HRESULT hr = m_swap->Present(m_vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        // device-lost 恢复：标记不可用 + 置重建标志，下次 poll_platform_events
        // 重建 device/swapchain 并触发全量重渲染；request_wake 确保睡眠中的帧循环即刻处理。
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            m_ok = false;
            m_need_reinit = true;
            m_win->request_wake();
        }
        return make_error(ErrorCode::PlatformUnavailable, "D3D11Surface::present failed.", "",
                          "aurora/window/d3d11_surface.h");
    }
    ++m_frame;
    return Result<bool>{ true };
}

} // namespace aurora

#endif // AURORA_BACKEND_D3D11
