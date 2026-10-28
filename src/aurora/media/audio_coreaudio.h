#pragma once

// ============================================================
// media/audio_coreaudio.h — CoreAudio 音频设备后端（库内部，非公共 API）
// ------------------------------------------------------------
// pimpl 隔离：AudioToolbox / CoreAudio 头只出现在 audio_coreaudio.cpp，禁止外泄。
// AURORA_ENABLE_AUDIO_COREAUDIO 开启（Apple，宏传播给全库）时为真实后端：
// AudioQueue（AudioToolbox）建队列，系统框架恒在故**无运行期绑定**（与 ALSA 的
// dlopen 逃逸不同因：ALSA 是为让无 dev 包的 Linux 构建机能编译，Apple 的两枚
// framework 属系统 SDK）。无输出设备 / 建队列失败 / AudioQueueStart 报错 →
// start 返回 false，AudioContext 按既有契约静默降级；宏关闭时 .cpp 体裁切为
// disabled 桩（start 恒 false，与 Wasapi* / Alsa* 桩对称）。
// 与 WASAPI / ALSA 后端共用同一内部契约（AudioDeviceBackend / AudioCaptureBackend，
// 见 include/aurora/media/audio.h），语义逐项对齐：48000/2 float32 图契约、
// 设备侧差异不外露、默认输出设备变更自动重路由、采集 failed() 观察口。
// ============================================================

#include <memory>

#include "aurora/media/audio.h"

namespace aurora {

/// @brief CoreAudio 默认输出设备渲染后端（macOS 播放腿；库内部，非公共 API）。
///
/// `AudioQueue` 输出腿：客户端格式恒 48000/2 float32 交错，队列内建的格式转换
/// 吸收设备侧采样率/声道差异（与 ALSA 插件层、WASAPI AUTOCONVERTPCM 同口径），
/// 故图侧契约恒定、重路由后亦不变。队列自持线程周期性回调填块（≈20ms 一块、
/// 3 块水位）；`AudioQueueStop/Dispose(sync=true)` 为同步语义，直接满足
/// `stop()` 须等设备线程退出的契约。后端另自持一枚控制线程做重路由：默认输出
/// 设备变更事件只置位，重建在控制线程上做（失败退避 200ms 重试，期间无回调 ⇒
/// 图时钟冻结，对齐 ALSA 断连重开与 WASAPI 重路由语义），并以 200ms 轮询
/// `kAudioQueueProperty_IsRunning` 兜底事件不到的拔除形态。
class CoreAudioDeviceBackend final : public AudioDeviceBackend {
  public:
    CoreAudioDeviceBackend();
    ~CoreAudioDeviceBackend() override;
    CoreAudioDeviceBackend(const CoreAudioDeviceBackend &) = delete;
    auto operator=(const CoreAudioDeviceBackend &) -> CoreAudioDeviceBackend & = delete;
    /// @brief 控制线程在 start() 起即捕获 Impl 地址，且「析构必 join 线程」是本后端的
    ///        收尾契约；移动会把 Impl 转给新主人而线程照旧跑，源对象沦为不再 join 的
    ///        空壳。后端恒以 unique_ptr<AudioDeviceBackend> 就地持有，无移动需求。
    CoreAudioDeviceBackend(CoreAudioDeviceBackend &&) = delete;
    auto operator=(CoreAudioDeviceBackend &&) -> CoreAudioDeviceBackend & = delete;

    /// @brief 输出格式（恒 48000/2 float32：图渲染契约稳定，设备差异由队列内建
    ///        格式转换吸收，端点重开后契约不变）。
    [[nodiscard]] auto format() const -> AudioDeviceFormat override;
    /// @brief 启动渲染队列（建队列 + 分配/预灌缓冲 + AudioQueueStart + 挂默认输出
    ///        设备监听 + 起重路由控制线程）；无输出设备/任一步失败返回 false。
    auto start(RenderFn render_block) -> bool override;
    /// @brief 停止并回收队列、设备监听与控制线程（阻塞等待线程退出与队列同步销毁）。
    auto stop() -> void override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief CoreAudio 采集后端（麦克风录制链输入；库内部，非公共 API）。
///
/// 默认输入设备的**原生**采样率与声道直取（读 `kAudioDevicePropertyNominalSampleRate`
/// 与 Input scope 的 `kAudioDevicePropertyStreamConfiguration`），回调携带该实际
/// rate/channels——不做输入侧重采样（CoreAudio 输入队列不保证率转换，猜格式的建立
/// 失败率高于读设备真值，与 ALSA 的顺位协商同语义：交付什么就报什么）。
/// 启动期无输入设备 / 权限缺失 → start 返回 false（调用方转显式错误）；
/// **权限缺失的判定口径**：main bundle 未声明 `NSMicrophoneUsageDescription` 时
/// 直接拒启——macOS 对未声明该键就访问麦克风的进程是**终止**而非报错，裸可执行
/// 文件（单测/无壳探针）据此永不被杀。中段失败（队列错误 / 设备消失）→ 队列销毁、
/// 回调止流，经 `failed()` 观察（与 Wasapi/AlsaCaptureBackend 同一审计契约：
/// 内部 `running` 是 stop 握手位，不可作死活判据）。
class CoreAudioCaptureBackend final : public AudioCaptureBackend {
  public:
    CoreAudioCaptureBackend();
    ~CoreAudioCaptureBackend() override;
    CoreAudioCaptureBackend(const CoreAudioCaptureBackend &) = delete;
    auto operator=(const CoreAudioCaptureBackend &) -> CoreAudioCaptureBackend & = delete;
    /// @brief 同渲染端：采集腿的失败观察与控制线程捕获 Impl 地址，移动会留下不再
    ///        join 的空壳源对象。
    CoreAudioCaptureBackend(CoreAudioCaptureBackend &&) = delete;
    auto operator=(CoreAudioCaptureBackend &&) -> CoreAudioCaptureBackend & = delete;

    /// @brief 启动采集（读默认输入设备原生格式 + 建输入队列 + 预灌缓冲 + Start）；
    ///        无设备、无 `NSMicrophoneUsageDescription` 声明、或任一步失败返回 false。
    auto start(CaptureFn on_pcm) -> bool override;
    /// @brief 停止并回收采集队列与设备监听（阻塞等待队列同步销毁）。
    auto stop() -> void override;
    /// @brief 中段设备失败观察口：true = 采集腿已因队列错误/设备消失止流（回调不再到达）。
    ///        disabled 桩恒 false（start 恒 false → 无中段可言）。
    [[nodiscard]] auto failed() const -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora
