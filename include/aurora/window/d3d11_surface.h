#pragma once

// D3D11 后端：仅当 AURORA_BACKEND_D3D11 定义（CMake 选项，默认 OFF）时编译，
// 避免默认零三方依赖构建引入 d3d11/dxgi/d3dcompiler 链接。
#ifdef AURORA_BACKEND_D3D11

#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/window/surface.h"
#include "aurora/window/win32_window.h" // 共享宿主：窗口/事件/DPI/同步重渲染

namespace aurora {

/**
 * @brief D3D11 表面（ARCHITECTURE.md §8.4 后端家族）：复用共享 `Win32Window` 宿主，像素经 D3D11 纹理-
 * 增量上传 + GPU 缩放呈现。paint 管线不变（仍由 `Window::present_root` 驱动 CPU `Painter`），
 * 仅把「CPU RGBA8 帧缓冲 → 屏幕」替换为 GPU 合成，解决大窗口 GDI 上屏瓶颈。
 *
 * - 增量上屏：脏矩形（逻辑坐标 × scale 转设备坐标）经 `UpdateSubresource` 仅更新变化区；
 *   首帧/整帧脏全量上传。GPU 以全屏三角形 + 线性采样器做任意比例缩放。
 * - device-lost 恢复桩预留（`on_device_lost`），单线程（UI 线程）使用 device/context。
 */
class D3D11Surface : public Surface {
  public:
    D3D11Surface(int width, int height, const std::string &title, const WindowStyleOptions &style);
    ~D3D11Surface() override;

    D3D11Surface(const D3D11Surface &) = delete;
    auto operator=(const D3D11Surface &) -> D3D11Surface & = delete;
    D3D11Surface(D3D11Surface &&) = delete;
    auto operator=(D3D11Surface &&) -> D3D11Surface & = delete;

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override { return m_painter; }
    [[nodiscard]] auto present() -> Result<bool> override;
    [[nodiscard]] auto size() const -> Size override { return m_win->size(); }
    [[nodiscard]] auto scale_factor() const -> float override { return m_win->scale_factor(); }
    [[nodiscard]] auto should_close() const -> bool override { return m_win->should_close(); }

    auto poll_platform_events() -> void override;
    auto set_event_handler(const EventHandler &h) -> void override { m_win->set_event_handler(h); }
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        m_win->set_window_state_handler(std::move(h));
    }
    auto set_window_mode_handler(WindowModeHandler h) -> void override { m_win->set_window_mode_handler(std::move(h)); }
    auto set_present_request(PresentRequest h) -> void override {
        m_present_request = h; // 本地留存：device-lost 恢复后触发全量重渲染
        m_win->set_present_request(std::move(h));
    }
    /// @brief 阻塞等待消息或超时（转发共享宿主）。
    auto wait_events(double timeout_ms) -> void override { m_win->wait_events(timeout_ms); }
    /// @brief 跨线程唤醒主循环（转发共享宿主；PostMessage 线程安全）。
    auto request_wake() -> void override { m_win->request_wake(); }
    /// @brief vsync 开启且设备可用时，`Present(1,0)` 阻塞到 vblank 自带帧节拍；
    /// 帧调度据此在活跃帧跳过 CPU 端 sleep 节流，避免双重限速。
    [[nodiscard]] auto paces_frames() const -> bool override { return m_ok && m_vsync; }
    /// @brief 启用/关闭垂直同步（默认开；关闭后 `Present(0,0)` 不等 vblank，交还 CPU 节流）。
    auto set_vsync(bool on) -> void { m_vsync = on; }
    [[nodiscard]] auto vsync() const -> bool { return m_vsync; }
    /// @brief 运行时更新窗口标题（转发给共享宿主）。
    auto set_title(const std::string &title) -> void override { m_win->set_title(title); }

    /// @brief 接收本帧脏矩形（设备坐标）；present 时仅增量上传这些区域，空向量 = 全量上传。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override { m_dirty = device_rects; }

    [[nodiscard]] auto data() const -> const std::uint8_t * override { return m_painter.data(); }
    /// @brief 帧缓冲物理像素尺寸：D3D11 painter 按 DPI 物理分辨率（m_dev_w/m_dev_h = 逻辑×scale）分配，
    /// 故返回 painter 缓冲像素尺寸，而非逻辑 `size()`（缩放比≠1 时避免 PNG 宽高与像素数据错位）。
    [[nodiscard]] auto framebuffer_size() const -> Size override {
        return Size{ .width = static_cast<float>(m_painter.width()), .height = static_cast<float>(m_painter.height()) };
    }
    [[nodiscard]] auto frame_count() const -> int override { return m_frame; }
    /// @brief 设备是否可用（无适配器时为 false，测试应跳过）。
    [[nodiscard]] auto is_available() const -> bool { return m_ok; }
    /// @brief 测试 seam：模拟 device-lost（置不可用 + 重建标志），
    /// 下次 `poll_platform_events` 走恢复路径（重建设备 + 全量重渲染）。
    auto simulate_device_lost() -> void {
        m_ok = false;
        m_need_reinit = true;
    }

  private:
    auto init_device(int width, int height) -> bool;
    auto ensure_swap_chain(int w, int h) -> bool;
    [[nodiscard]] auto upload_region(int x, int y, int w, int h) const -> bool;
    /// @brief 释放全部 D3D11 资源（析构 / device-lost 重建前）。
    auto release_device() -> void;
    /// @brief device-lost 恢复：释放旧资源 → 重建 device/swapchain →
    /// 成功后经 present_request 触发一次全量重渲染上屏。在 poll_platform_events
    /// 中执行（present_root 外，避开重入护栏）。
    auto try_recover_device() -> void;

    std::unique_ptr<Win32Window> m_win; ///< 共享窗口宿主
    Painter m_painter;                  ///< CPU 帧缓冲（RGBA8，绘制目标）

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_ctx = nullptr;
    IDXGISwapChain1 *m_swap = nullptr;
    ID3D11Texture2D *m_rt = nullptr;
    ID3D11RenderTargetView *m_rtv = nullptr;
    ID3D11Texture2D *m_src = nullptr; ///< 源纹理（CPU 上传目标，与 painter 同尺寸）
    ID3D11ShaderResourceView *m_src_srv = nullptr;
    ID3D11VertexShader *m_vs = nullptr;
    ID3D11PixelShader *m_ps = nullptr;
    ID3D11SamplerState *m_samp = nullptr;
    ID3D11InputLayout *m_layout = nullptr;
    ID3D11BlendState *m_bs = nullptr;

    std::vector<Rect> m_dirty; ///< 本帧脏矩形（设备坐标），present 时消费。
    int m_dev_w = 0, m_dev_h = 0;
    int m_frame = 0;
    bool m_ok = false;          ///< 设备初始化是否成功（失败则 present 直接报错，便于测试跳过）。
    bool m_vsync = true;        ///< 垂直同步（Present 第一参数 1/0）。
    bool m_need_reinit = false; ///< device-lost 后置位；下次 poll 时重建。
};

} // namespace aurora

#endif // AURORA_BACKEND_D3D11
