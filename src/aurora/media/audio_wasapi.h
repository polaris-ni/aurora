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
    /// @brief 设备线程在 start() 起即捕获 Impl 地址，且「析构必 join 线程」是本后端的
    ///        收尾契约；移动会把 Impl 转给新主人而线程照旧跑，源对象沦为不再 join 的
    ///        空壳。后端恒以 unique_ptr<AudioDeviceBackend> 就地持有，无移动需求。
    WasapiDeviceBackend(WasapiDeviceBackend &&) = delete;
    auto operator=(WasapiDeviceBackend &&) -> WasapiDeviceBackend & = delete;

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
/// （shared 引擎恒 float32）原生采样率/声道回调；SILENT 包补零。启动期设备/权限
/// 不可用 → start 返回 false（调用方转显式错误）；**中段失败**（端点被移除 /
/// GetBuffer 出错）→ 采集线程退出、回调止流，经 `failed()` 观察（审计修正：此前
/// 注释声称「由调用方感知」但无任何观察通道，实为静默止流）。
class WasapiCaptureBackend final : public AudioCaptureBackend {
  public:
    WasapiCaptureBackend();
    ~WasapiCaptureBackend() override;
    WasapiCaptureBackend(const WasapiCaptureBackend &) = delete;
    auto operator=(const WasapiCaptureBackend &) -> WasapiCaptureBackend & = delete;
    /// @brief 同渲染端：采集线程捕获 Impl 地址，移动会留下不再 join 的空壳源对象。
    WasapiCaptureBackend(WasapiCaptureBackend &&) = delete;
    auto operator=(WasapiCaptureBackend &&) -> WasapiCaptureBackend & = delete;

    /// @brief 启动采集线程（COM + 默认捕获端点 + event-driven）；失败返回 false。
    auto start(CaptureFn on_pcm) -> bool override;
    /// @brief 停止并回收采集线程与全部 COM 资源（阻塞等待线程退出）。
    auto stop() -> void override;
    /// @brief 中段设备失败观察口：true = 采集线程已因设备错误退出（回调不再到达）。
    ///        内部 `running` 标志是 stop 握手位（承载 join 义务），不可作死活判据。
    ///        disabled 桩恒 false（start 恒 false → 无中段可言）。
    [[nodiscard]] auto failed() const -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora
