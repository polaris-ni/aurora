#pragma once

// ============================================================
// media/audio_webaudio.h — Web Audio 音频设备后端（库内部，非公共 API）
// ------------------------------------------------------------
// 浏览器对位后端（Windows WASAPI / Linux ALSA / 浏览器 Web Audio 三足对称）。
// 与另两者共用同一内部契约（`AudioDeviceBackend` / `AudioCaptureBackend`），但
// **线程模型根本不同**，故此处把差异如实写清，避免照抄桌面口径：
//
//  1. **没有设备线程**：Web Audio 的音频渲染线程在 JS 侧，既看不见也调不进 wasm。
//     可达的只有主线程回调（`ScriptProcessorNode.onaudioprocess`），而主线程 = UI
//     线程。故 `render_block` 发生在**浏览器主线程**的定间隔回调里，而非任何
//     「设备线程」——桌面的「音频线程不阻塞于 UI」不变量在此退化为
//     「同一线程，靠事件循环交错」，UI 长任务（>一块时长）会饿死取样。
//  2. **推式环缓冲**：JS 回调必须同步填满输出缓冲，而它无法调用 wasm 函数（不带
//     `EXPORTED_FUNCTIONS` 时导出符号不进 `Module`，`embind` 在本工具链又是 port）。
//     故反转为 C++ 定时（`emscripten_set_interval`）把图渲染成帧**推**入 wasm 线性
//     内存里的环，JS 经 `HEAPF32` 视图按 `head`/`tail` 两个 int32 地址消费——
//     零链接标志、零导出符号，跨边界只传地址。
//  3. **自动播放闸门**：`new AudioContext()` 在非用户手势下得到 `suspended` 上下文，
//     `resume()` 需瞬时激活（transient activation）。故 `start()` 返回 true 只表示
//     「设备存在且在收样」，**不等于出声**；闸门由浏览器掌握，后端每个排空拍在
//     `suspended` 时限速重试 `resume()`，用户一旦在页面上有任何交互即自动开声。
//     Aurora 侧的 `suspend()/resume()` 与此正交（前者靠渲染静音实现，见 audio.cpp）。
//  4. **延迟与丢样**：环水位 + `ScriptProcessorNode` 块长（2048 帧）决定端到端延迟
//     约 60–150 ms（WASAPI event-driven 约 20 ms）；欠载即补零并计数（`underruns()`
//     可观测）。`AudioWorklet` + `SharedArrayBuffer` 才是桌面对位的正解，但它要求
//     `-pthread` 构建与跨源隔离，见 specification/06-app-platform.md §12.1 线程模型。
//
// 采集（麦克风）：`getUserMedia` 是**异步权限流**，与本契约的同步 `start() -> bool` 不同形，
// 折中口径见 `WebAudioCaptureBackend` 类注——`start()` 发起授权并建环即返回 true（宿主无
// `mediaDevices` 才 false），授权结果异步落地：running 后 JS 把输入帧推入环（渲染环的角色
// 对调），C++ 排空拍读出并调 `on_pcm`（主线程）；拒绝/失败经 `failed()` 观测，不投递假数据。
// ============================================================

#include <cstdint>
#include <memory>
#include <vector>

#include "aurora/media/audio.h"

namespace aurora {

/// @brief Web Audio 输出后端（浏览器默认输出设备；库内部，非公共 API）。
///
/// 构造即探测 `AudioContext` 可用性并读取设备协商的采样率/声道（`format()` 在
/// `start()` **之前**被 `AudioContext` 查询，故协商必须发生在构造期）；宿主无
/// `AudioContext`（如裸 Node）→ 标记不可用，`start()` 返回 false 走静默降级。
class WebAudioDeviceBackend final : public AudioDeviceBackend {
  public:
    WebAudioDeviceBackend();
    ~WebAudioDeviceBackend() override;
    WebAudioDeviceBackend(const WebAudioDeviceBackend &) = delete;
    auto operator=(const WebAudioDeviceBackend &) -> WebAudioDeviceBackend & = delete;
    /// @brief 排空定时器（emscripten_set_interval）与 render 回调都挂在 Impl 上：移动只
    ///        转走 unique_ptr，定时器照旧在册，而源对象此后的 stop() 因 impl_ 已空直接
    ///        no-op——「同线程 ⇒ stop 返回即无回调」的收尾契约即被打破。后端由 AudioContext
    ///        以 unique_ptr<AudioDeviceBackend> 就地持有，无移动需求。
    WebAudioDeviceBackend(WebAudioDeviceBackend &&) = delete;
    auto operator=(WebAudioDeviceBackend &&) -> WebAudioDeviceBackend & = delete;

    /// @brief 输出格式（采样率取浏览器协商的 `ctx.sampleRate`，声道恒 2 = 图侧契约——
    ///        多声道输出设备由浏览器自动上混；协商不可得时为处理格式 48000/2）。
    [[nodiscard]] auto format() const -> AudioDeviceFormat override;
    /// @brief 启动：建 `AudioContext` + `ScriptProcessorNode` 消费链与主线程排空定时器。
    ///        返回 false = 宿主无 `AudioContext`（静默降级）。**true 不等于已出声**
    ///        （自动播放闸门，见文件头第 3 条）。
    auto start(RenderFn render_block) -> bool override;
    /// @brief 停止并回收（同一线程 ⇒ 返回后即无回调，无 join 义务）。
    auto stop() -> void override;

    /// @brief 浏览器上下文状态：-1 无实例，0 suspended（待用户手势），1 running，
    ///        2 其他（closed/interrupting）。自动播放闸门的只读观测口（真机探针据此判据）。
    [[nodiscard]] static auto context_state() -> int;
    /// @brief 排空后仍卡在闸门里时的限速重试（每秒至多一次），返回是否发起了重试。
    auto retry_resume() -> bool;
    /// @brief 欠载次数（JS 侧在环空时补零的累计）与已消费帧数——推式环的自证观测口。
    [[nodiscard]] auto underruns() const -> int;
    [[nodiscard]] auto consumed_frames() const -> long long;

  private:
    struct Impl;
    /// @brief 主线程排空拍回调（`emscripten_set_interval` 的蹦床）：把图渲染成帧推入环，
    ///        并在上下文 `suspended` 时限速重试 `resume()`。真实定义仅在
    ///        AURORA_ENABLE_AUDIO_WEBAUDIO 分支的编译单元内；disabled 桩分支提供 no-op 定义
    ///        （start 恒 false，永不注册定时器，故桩构建下不会被调用，仅用于满足 ODR）。
    static auto pump(void *user_data) -> void;
    std::unique_ptr<Impl> impl_;
};

/// @brief Web Audio 采集后端（浏览器默认输入设备；库内部，非公共 API）。
///
/// `getUserMedia` 是**异步权限流**，与本契约的同步 `start() -> bool` 不同形——折中口径：
/// `start()` 只发起授权并建环（宿主无 `mediaDevices` / 非安全上下文才返回 false），授权结果
/// 经会话状态字异步落地：`running` 后 `onaudioprocess` 把输入帧推入 wasm 环（与渲染方向
/// 同一份 `WebAudioRing`、角色对调），C++ 排空拍（`emscripten_set_interval`）从环读出并
/// 调 `on_pcm`（**主线程**，与渲染排空拍同线程）。授权拒绝 / 设备缺失 / 建链失败 ⇒
/// `failed()` 翻 true 且不再有样本投递（与 WASAPI 的 `failed()` 中段失败观察口同位）；
/// 录制侧表现为样本停 0，无静默假数据。
class WebAudioCaptureBackend final : public AudioCaptureBackend {
  public:
    WebAudioCaptureBackend();
    ~WebAudioCaptureBackend() override;
    /// @brief 虽然核心状态在 wasm 全局槽，仍与 Alsa/Wasapi 采集端同口径：后端只以
    ///        unique_ptr<AudioCaptureBackend> 就地持有，复制/移动一律封死。
    WebAudioCaptureBackend(const WebAudioCaptureBackend &) = delete;
    auto operator=(const WebAudioCaptureBackend &) -> WebAudioCaptureBackend & = delete;
    WebAudioCaptureBackend(WebAudioCaptureBackend &&) = delete;
    auto operator=(WebAudioCaptureBackend &&) -> WebAudioCaptureBackend & = delete;

    /// @brief 启动采集：发起 `getUserMedia` 授权 + 建环与排空定时器。
    ///        返回 false = 宿主无 `mediaDevices`（非安全上下文 / 裸 Node），能力如实缺席；
    ///        **true 不等于已拿到麦克风**（授权异步，见类注）。
    auto start(CaptureFn on_pcm) -> bool override;
    /// @brief 停止采集并回收（停 track、拆链、关上下文；同线程 ⇒ 返回后即无回调）。
    auto stop() -> void override;
    /// @brief 中段失败观察口：true = 授权被拒 / 建链失败（回调不再到达）。
    ///        未启动或仍在授权中为 false（与 WASAPI 的 `failed()` 同位语义）。
    [[nodiscard]] auto failed() const -> bool;

  private:
    struct Impl;
    /// @brief 排空拍回调（`emscripten_set_interval` 的蹦床）：从环读出输入帧调 `on_pcm`。
    ///        真实定义仅在 AURORA_ENABLE_AUDIO_WEBAUDIO 分支的编译单元内；disabled 桩分支
    ///        提供 no-op 定义（start 恒 false，永不注册定时器，仅用于满足 ODR）。
    static auto pump(void *user_data) -> void;
    std::unique_ptr<Impl> impl_;
};

namespace detail {

/// @brief 推式环缓冲（交错 float32，定长，**单写单读**）：写端是 C++ 排空拍，读端是
///        JS `onaudioprocess`（经 `HEAPF32`/`HEAP32` 视图直接触碰本类的三个 int32 与
///        数据块）。留出「一格空」以区分满/空，故可用容量 = capacity - 1。
///
/// @note Thread: 无锁——浏览器无 `-pthread` 时写读均在主线程，仅事件循环交错；指针
///       交接为普通 int32 读写，无撕裂风险。跨线程复用本类前须自行加同步。
class WebAudioRing {
  public:
    WebAudioRing(int capacity_frames, int channels);

    [[nodiscard]] auto capacity_frames() const -> int { return capacity_frames_; }
    [[nodiscard]] auto channels() const -> int { return channels_; }
    /// @brief 交错数据块基址（帧 × 声道 × float32）。JS 侧据此建 `Float32Array` 视图。
    [[nodiscard]] auto data() -> float * { return data_.data(); }
    /// @brief 三个交接量地址：写指针（仅 C++ 推进）、读指针（仅 JS 推进）、欠载计数（仅 JS 累加）。
    [[nodiscard]] auto writer_position() -> std::int32_t * { return &writer_; }
    [[nodiscard]] auto reader_position() -> std::int32_t * { return &reader_; }
    [[nodiscard]] auto underrun_counter() -> std::int32_t * { return &underruns_; }

    /// @brief 环内可读帧数（对端读指针的最新值直接从内存读，无需额外同步）。
    [[nodiscard]] auto avail_frames() const -> int;
    /// @brief 可安全写入的帧数（恒留一格空 ⇒ 满/空可辨）。
    [[nodiscard]] auto free_frames() const -> int;
    /// @brief 写入至多 `frames` 帧（按 `free_frames()` 裁剪），返回实写帧数。
    auto write(const float *interleaved, int frames) -> int;
    /// @brief 吸收对端读指针推进：累计已消费帧数（`consumed_frames` 供真机判据）。
    auto sync_reader() -> void;
    [[nodiscard]] auto consumed_frames() const -> long long { return consumed_; }
    [[nodiscard]] auto underruns() const -> int { return underruns_; }

    // ---- 采集方向（写端 = JS，读端 = C++ 排空拍；与上方渲染方向同一份环、角色对调） ----
    /// @brief 采集方向环内可读帧数（对端写指针的最新值直接从内存读）。
    [[nodiscard]] auto capture_avail_frames() const -> int;
    /// @brief 采集方向读出至多 `frames` 帧（推进读指针），返回实读帧数。
    auto capture_read(float *interleaved, int frames) -> int;
    /// @brief 采集方向已投递帧数（`capture_read` 累计；真机判据用）。
    [[nodiscard]] auto capture_delivered_frames() const -> long long { return delivered_; }

  private:
    std::vector<float> data_;
    int capacity_frames_ = 0;
    int channels_ = 0;
    // 三个交接量必须是**独立可取址的 int32**（JS 侧按地址读写，成员布局不可依赖）。
    std::int32_t writer_ = 0;
    std::int32_t reader_ = 0;
    std::int32_t underruns_ = 0;
    std::int32_t last_reader_ = 0;
    long long consumed_ = 0;
    long long delivered_ = 0;  ///< 采集方向累计投递帧数（capture_read 累计）
};

}  // namespace detail

}  // namespace aurora
