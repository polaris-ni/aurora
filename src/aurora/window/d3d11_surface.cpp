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
    o.uv = float2(p.x * 0.5f + 0.5f, p.y * 0.5f + 0.5f); // upload_region 已将数据转为顶行优先，无需再翻转 v
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
            AURORA_LOG_ERROR("d3d11", "shader compile failed: ", static_cast<const char *>(err->GetBufferPointer()));
            err->Release();
        }
        return false;
    }
    return true;
}

auto make_sampler_desc() -> D3D11_SAMPLER_DESC {
    D3D11_SAMPLER_DESC sd;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
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
    constexpr std::array levels = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0 };
    const int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * m_win->scale_factor())));
    const int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * m_win->scale_factor())));

    // 使用 D3D11CreateDeviceAndSwapChain 直接创建设备和交换链，避免手动获取工厂
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.OutputWindow = static_cast<HWND>(m_win->hwnd());
    scd.Windowed = TRUE;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.Width = static_cast<UINT>(w);
    scd.BufferDesc.Height = static_cast<UINT>(h);
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SampleDesc.Count = 1;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    IDXGISwapChain *swap_raw = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &scd, &swap_raw, &m_device, nullptr, &m_ctx);
    if (FAILED(hr)) {
        AURORA_LOG_INFO("d3d11", "Hardware adapter failed, trying WARP...");
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &scd,
                                           &swap_raw, &m_device, nullptr, &m_ctx);
        if (FAILED(hr)) {
            AURORA_LOG_ERROR("d3d11", "Both hardware and WARP adapters failed, hr=", hr);
            return false;
        }
    }
    if (m_device == nullptr || m_ctx == nullptr || swap_raw == nullptr) {
        AURORA_LOG_ERROR("d3d11", "Device, context or swapchain is null");
        safe_release(swap_raw);
        return false;
    }
    m_swap = swap_raw;

    // 编译着色器。
    ID3DBlob *vs_blob = nullptr;
    ID3DBlob *ps_blob = nullptr;
    if (!compile_shader(AURORA_VS, "VSMain", "vs_5_0", &vs_blob)) {
        AURORA_LOG_ERROR("d3d11", "Vertex shader compilation failed");
        safe_release(vs_blob);
        safe_release(ps_blob);
        return false;
    }
    if (!compile_shader(AURORA_PS, "PSMain", "ps_5_0", &ps_blob)) {
        AURORA_LOG_ERROR("d3d11", "Pixel shader compilation failed");
        safe_release(vs_blob);
        safe_release(ps_blob);
        return false;
    }
    const HRESULT vs_hr =
        m_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs);
    const HRESULT ps_hr =
        m_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps);
    // VS 仅用 SV_VertexID，无真实顶点输入；提供一个虚拟输入元素以满足 CreateInputLayout 的 E_INVALIDARG 检查。
    constexpr D3D11_INPUT_ELEMENT_DESC dummy_desc = { .SemanticName = "POSITION",
                                                      .SemanticIndex = 0,
                                                      .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                      .InputSlot = 0,
                                                      .AlignedByteOffset = 0,
                                                      .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
                                                      .InstanceDataStepRate = 0 };
    const HRESULT layout_hr =
        m_device->CreateInputLayout(&dummy_desc, 1, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_layout);
    safe_release(vs_blob);
    safe_release(ps_blob);
    if (FAILED(vs_hr) || (m_vs == nullptr)) {
        AURORA_LOG_ERROR("d3d11", "CreateVertexShader failed, hr=", vs_hr);
        return false;
    }
    if (FAILED(ps_hr) || (m_ps == nullptr)) {
        AURORA_LOG_ERROR("d3d11", "CreatePixelShader failed, hr=", ps_hr);
        return false;
    }
    if (FAILED(layout_hr) || (m_layout == nullptr)) {
        AURORA_LOG_ERROR("d3d11", "CreateInputLayout failed, hr=", layout_hr);
        return false;
    }

    // 线性采样器（GPU 缩放）+ 不透明混合。
    const D3D11_SAMPLER_DESC sd = make_sampler_desc();
    const HRESULT samp_hr = m_device->CreateSamplerState(&sd, &m_samp);
    if (FAILED(samp_hr) || (m_samp == nullptr)) {
        AURORA_LOG_ERROR("d3d11", "CreateSamplerState failed, hr=", samp_hr);
        return false;
    }

    const D3D11_BLEND_DESC bd = make_blend_desc();
    const HRESULT blend_hr = m_device->CreateBlendState(&bd, &m_bs);
    if (FAILED(blend_hr) || (m_bs == nullptr)) {
        AURORA_LOG_ERROR("d3d11", "CreateBlendState failed, hr=", blend_hr);
        return false;
    }

    m_dev_w = w;
    m_dev_h = h;
    return ensure_swap_chain(w, h);
}

auto D3D11Surface::ensure_swap_chain(int w, int h) -> bool {
    if (m_swap == nullptr) {
        AURORA_LOG_ERROR("d3d11", "ensure_swap_chain: swapchain is null");
        return false;
    }
    if (w <= 0 || h <= 0) {
        AURORA_LOG_ERROR("d3d11", "ensure_swap_chain: invalid dimensions ", w, "x", h);
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
    static constexpr IID iid_tex2_d = { .Data1 = 0x6f15aaf2,
                                        .Data2 = 0xd208,
                                        .Data3 = 0x4e89,
                                        .Data4 = { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
    void *back_tmp = nullptr;
    const HRESULT get_hr = m_swap->GetBuffer(0, iid_tex2_d, &back_tmp);
    if (FAILED(get_hr)) {
        AURORA_LOG_ERROR("d3d11", "ensure_swap_chain: GetBuffer failed, hr=", get_hr);
        return false;
    }
    back = static_cast<ID3D11Texture2D *>(back_tmp);
    const HRESULT rtv_hr = m_device->CreateRenderTargetView(back, nullptr, &m_rtv);
    safe_release(back);
    if (FAILED(rtv_hr)) {
        AURORA_LOG_ERROR("d3d11", "ensure_swap_chain: CreateRenderTargetView failed, hr=", rtv_hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const HRESULT tex_hr = m_device->CreateTexture2D(&td, nullptr, &m_src);
    if (FAILED(tex_hr)) {
        AURORA_LOG_ERROR("d3d11", "ensure_swap_chain: CreateTexture2D failed, hr=", tex_hr);
        return false;
    }
    const HRESULT srv_hr = m_device->CreateShaderResourceView(m_src, nullptr, &m_src_srv);
    if (FAILED(srv_hr)) {
        AURORA_LOG_ERROR("d3d11", "ensure_swap_chain: CreateShaderResourceView failed, hr=", srv_hr);
        return false;
    }
    m_dev_w = w;
    m_dev_h = h;
    return true;
}

auto D3D11Surface::begin_frame(int width, int height) -> Result<bool> {
    const float s = scale_factor();
    m_dev_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * s)));
    m_dev_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * s)));
    m_painter.begin(m_dev_w, m_dev_h);
    // 用不透明背景色填充 painter 缓冲区（与 present() 的 ClearRenderTargetView 颜色一致）。
    // Painter 的 set_pixel 在混合后将 alpha 强制写 255，若缓冲区初始为透明黑(0,0,0,0)，
    // 文字抗锯齿边缘会与黑色混合导致发暗/黑边。预填不透明背景使抗锯齿与正确底色混合。
    m_painter.fill_rect(
        Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
              .size = Size{ .width = static_cast<float>(m_dev_w), .height = static_cast<float>(m_dev_h) } },
        Color{ 245, 245, 245, 255 });
    if (m_ok) {
        ensure_swap_chain(m_dev_w, m_dev_h);
    }
    return Result{ true };
}

auto D3D11Surface::upload_region(int x, int y, int w, int h) const -> bool {
    if (m_src == nullptr) {
        AURORA_LOG_ERROR("d3d11", "upload_region: m_src is null");
        return false;
    }
    x = std::clamp(x, 0, m_dev_w);
    y = std::clamp(y, 0, m_dev_h);
    w = std::clamp(w, 0, m_dev_w - x);
    h = std::clamp(h, 0, m_dev_h - y);
    if (w <= 0 || h <= 0) {
        AURORA_LOG_ERROR("d3d11", "upload_region: w=", w, " or h=", h, " <= 0");
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
        for (std::size_t cx = 0; std::cmp_less(cx, w); ++cx) {
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
    bool upload_ok = false;
    if (m_dirty.empty()) {
        upload_ok = upload_region(0, 0, m_dev_w, m_dev_h);
    } else {
        for (const Rect &r : m_dirty) {
            upload_ok = upload_region(static_cast<int>(r.origin.x), static_cast<int>(r.origin.y),
                                      static_cast<int>(r.size.width), static_cast<int>(r.size.height));
        }
    }
    m_dirty.clear();

    // 清屏后绘制全屏三角形（GPU 缩放采样源纹理）。
    // 先清 RTV，再上传纹理（避免 GPU 还在读 m_src 时写入导致 stall）。
    m_ctx->OMSetRenderTargets(1, &m_rtv, nullptr);
    // 设置视口（D3D11 默认 viewport 宽高为 0，不设则光栅化全部裁剪 → 白屏）。
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(m_dev_w);
    vp.Height = static_cast<float>(m_dev_h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_ctx->RSSetViewports(1, &vp);
    constexpr std::array clear = { 0.96f, 0.96f, 0.96f, 1.0f };
    m_ctx->ClearRenderTargetView(m_rtv, clear.data());
    // 上传纹理数据（清屏后执行）。
    if (!upload_ok) {
        AURORA_LOG_ERROR("d3d11", "present upload FAILED, skipping draw");
    }
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
    return Result{ true };
}

} // namespace aurora

#endif // AURORA_BACKEND_D3D11
