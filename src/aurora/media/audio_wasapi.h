#pragma once

// ============================================================
// media/audio_wasapi.h — WASAPI 音频设备后端（库内部，非公共 API）
// ------------------------------------------------------------
// pimpl 隔离：windows.h / COM 头只出现在 audio_wasapi.cpp，禁止外泄。
// AURORA_ENABLE_AUDIO_WASAPI 开启时为真实后端（shared mode event-driven，
// IAudioClient float32 格式协商，默认设备变更/设备丢失自动重路由）；宏关闭时
// .cpp 体裁切为 disabled 桩（start 恒 false → AudioContext 静默降级）。
// ============================================================

#include <memory>

#include "aurora/media/audio.h"

namespace aurora {

class WasapiDeviceBackend final : public AudioDeviceBackend {
  public:
    WasapiDeviceBackend();
    ~WasapiDeviceBackend() override;
    WasapiDeviceBackend(const WasapiDeviceBackend &) = delete;
    auto operator=(const WasapiDeviceBackend &) -> WasapiDeviceBackend & = delete;

    /// @brief 输出格式（恒 48000/2 float32：图渲染契约稳定，与实际设备差异经
    ///        WASAPI AUTOCONVERTPCM 引擎侧转换吸收，重路由后契约不变）。
    [[nodiscard]] auto format() const -> AudioDeviceFormat override;
    /// @brief 启动设备线程（COM + 默认渲染端点 + event-driven 引擎）；失败返回 false。
    auto start(RenderFn render_block) -> bool override;
    /// @brief 停止并回收设备线程与全部 COM 资源（阻塞等待线程退出）。
    auto stop() -> void override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief WASAPI 采集后端（麦克风录制链输入；库内部，非公共 API）。
///
/// 默认采集端点（eCapture/eConsole）shared mode event-driven，GetMixFormat
/// （shared 引擎恒 float32）原生采样率/声道回调；SILENT 包补零。设备丢失/失败
/// → start 返回 false 或采集线程退出（由调用方转显式错误，录制不静默降级）。
class WasapiCaptureBackend final : public AudioCaptureBackend {
  public:
    WasapiCaptureBackend();
    ~WasapiCaptureBackend() override;
    WasapiCaptureBackend(const WasapiCaptureBackend &) = delete;
    auto operator=(const WasapiCaptureBackend &) -> WasapiCaptureBackend & = delete;

    /// @brief 启动采集线程（COM + 默认捕获端点 + event-driven）；失败返回 false。
    auto start(CaptureFn on_pcm) -> bool override;
    /// @brief 停止并回收采集线程与全部 COM 资源（阻塞等待线程退出）。
    auto stop() -> void override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora
