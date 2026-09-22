#pragma once

// ============================================================
// media/audio_alsa.h — ALSA 音频设备后端（库内部，非公共 API）
// ------------------------------------------------------------
// pimpl 隔离：dlfcn / ALSA ABI 声明只出现在 audio_alsa.cpp，禁止外泄。
// AURORA_ENABLE_AUDIO_ALSA 开启（Linux，宏传播给全库）时为真实后端：运行时
// dlopen("libasound.so.2") 绑定，**零构建期依赖**（不需 libasound dev 包，
// 与消费者 ABI 无耦合）；libasound 缺失/无设备 → start 返回 false，
// AudioContext 按既有契约静默降级。宏关闭时 .cpp 体裁切为 disabled 桩
// （start 恒 false，与 Wasapi* 桩对称）。
// 与 WASAPI 后端共用同一内部契约（AudioDeviceBackend / AudioCaptureBackend，
// 见 include/aurora/media/audio.h），语义逐项对齐：48000/2 float32 图契约、
// 设备线程自起自收、中段设备失败退避重开、采集 failed() 观察口。
// ============================================================

#include <memory>

#include "aurora/media/audio.h"

namespace aurora {

/// @brief ALSA 渲染端点输出后端（Linux 默认播放设备；库内部，非公共 API）。
///
/// "default" 设备 + 简单参数 API（snd_pcm_set_params：RW_INTERLEAVED float32
/// 48000/2，soft_resync 开启——插件层（plug/dmix/pulse 桥）吸收设备差异，图侧
/// 契约恒定）。设备线程 avail/writei 驱动；XRUN/挂起经 snd_pcm_recover 自愈，
/// 设备拔除（DISCONNECTED）→ 关闭端点退避 200ms 重开，期间时钟冻结（对齐
/// WASAPI 重路由语义）。
class AlsaDeviceBackend final : public AudioDeviceBackend {
  public:
    AlsaDeviceBackend();
    ~AlsaDeviceBackend() override;
    AlsaDeviceBackend(const AlsaDeviceBackend &) = delete;
    auto operator=(const AlsaDeviceBackend &) -> AlsaDeviceBackend & = delete;
    /// @brief 设备线程在 start() 起即捕获 Impl 地址，且「析构必 join 线程」是本后端的
    ///        收尾契约；移动会把 Impl 转给新主人而线程照旧跑，源对象沦为不再 join 的
    ///        空壳。后端恒以 unique_ptr<AudioDeviceBackend> 就地持有，无移动需求。
    AlsaDeviceBackend(AlsaDeviceBackend &&) = delete;
    auto operator=(AlsaDeviceBackend &&) -> AlsaDeviceBackend & = delete;

    /// @brief 输出格式（恒 48000/2 float32：图渲染契约稳定，设备差异由 ALSA
    ///        插件层重采样吸收，端点重开后契约不变）。
    [[nodiscard]] auto format() const -> AudioDeviceFormat override;
    /// @brief 启动设备线程（dlopen libasound + 打开 "default" 播放端点 + 参数
    ///        协商）；库缺失/无设备/协商失败返回 false。
    auto start(RenderFn render_block) -> bool override;
    /// @brief 停止并回收设备线程与 PCM 句柄（阻塞等待线程退出）。
    auto stop() -> void override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief ALSA 采集后端（麦克风录制链输入；库内部，非公共 API）。
///
/// "default" 捕获端点，格式协商按 48000/2 → 48000/1 → 44100/2 → 44100/1 顺位
/// 尝试（readi 交付的即协商后 float32 交错），回调携带实际 rate/channels。
/// 启动期库缺失/无设备 → start 返回 false（调用方转显式错误）；**中段失败**
/// （设备错误且不可恢复 / DISCONNECTED）→ 采集线程退出、回调止流，经
/// `failed()` 观察（与 WasapiCaptureBackend 同一审计契约：内部 `running` 是
/// stop 握手位，不可作死活判据）。
class AlsaCaptureBackend final : public AudioCaptureBackend {
  public:
    AlsaCaptureBackend();
    ~AlsaCaptureBackend() override;
    AlsaCaptureBackend(const AlsaCaptureBackend &) = delete;
    auto operator=(const AlsaCaptureBackend &) -> AlsaCaptureBackend & = delete;
    /// @brief 同渲染端：采集线程捕获 Impl 地址，移动会留下不再 join 的空壳源对象。
    AlsaCaptureBackend(AlsaCaptureBackend &&) = delete;
    auto operator=(AlsaCaptureBackend &&) -> AlsaCaptureBackend & = delete;

    /// @brief 启动采集线程（dlopen libasound + 打开捕获端点 + 格式协商）；失败返回 false。
    auto start(CaptureFn on_pcm) -> bool override;
    /// @brief 停止并回收采集线程与 PCM 句柄（阻塞等待线程退出）。
    auto stop() -> void override;
    /// @brief 中段设备失败观察口：true = 采集线程已因设备错误退出（回调不再到达）。
    ///        disabled 桩恒 false（start 恒 false → 无中段可言）。
    [[nodiscard]] auto failed() const -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora
