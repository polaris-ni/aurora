#pragma once

#include <cstdint>
#include <functional>

namespace aurora {

/// @brief 平台原生 GPU 表面种类（解码器 / 平台媒体框架交出的帧承载物）。
///
/// 核心零三方依赖：句柄以不透明指针 / 整型承载，语义按 `kind` 解释，平台头由实现方
/// （后端 / 消费者）自行包含。导入能力由 `rhi::RhiCapabilities::native_surface_import`
/// 查询；`GpuGlRhi`（GL 3.3）仅实现探测与回退，真实导入随 wgpu 阶梯
/// （Vulkan external memory / Metal IOSurface / D3D12 `OpenSharedHandle`）。
enum class NativeSurfaceKind : std::uint8_t {
    None,  ///< 无原生表面（CPU 帧路径）
    DmaBuf,  ///< Linux：dmabuf 文件描述符（`handle` = fd 数值）
    IoSurface,  ///< macOS：`IOSurfaceRef`（不透明指针）
    D3D11Texture,  ///< Windows：D3D11 共享纹理（`HANDLE` / `ID3D11Texture2D*`）
    AHardwareBuffer,  ///< Android：`AHardwareBuffer*`
};

/// @brief 原生 GPU 表面帧（解码器扩展点「原生表面」帧变体的载荷）。
///
/// 由 `VideoFrame::native_surface` 携带（非空时该帧走 GPU 导入路径，`image` 可为空）。
/// 句柄生命周期由产生方管理，`release` 在帧消费完毕后回调（可空 = 产生方自管）。
///
/// @note Thread: 主要由解码线程产生、主线程消费；`release` 回调在消费线程执行
/// @note Side-effects: none（纯值载荷）
struct NativeSurfaceFrame {
    NativeSurfaceKind kind = NativeSurfaceKind::None;
    void *handle = nullptr;  ///< 平台句柄（语义按 `kind`：fd 数值 / 指针 / HANDLE）
    int width = 0;  ///< 帧像素宽
    int height = 0;  ///< 帧像素高
    std::uint32_t format = 0;  ///< 像素格式（DRM fourcc 等；0 = 未指定，按后端默认）
    std::function<void()> release;  ///< 帧释放回调（导入方持帧期间有效；可空）
};

}  // namespace aurora
