#pragma once

// ============================================================
// media/audio.h — 音频子系统：图模型（Web Audio 语义）
// ------------------------------------------------------------
// AudioContext → AudioNode 图 → AudioDestinationNode 汇聚，内部统一 float32 混音。
// 设备层薄、混音图独立演进：真实平台后端（WASAPI/CoreAudio/ALSA/WebAudio）实现
// AudioDeviceBackend 注入；无后端编译或设备初始化失败 → 静默模式（图照常运转、
// 样本消费后丢弃），对齐 GPU 通道回退语义。规格落点：specification/03-layout-render.md §9。
//
// 线程模型：
//   - UI 线程：图变更（connect/disconnect）、参数/自动化写入、推流 push()。
//   - 渲染线程（设备线程）：render_block 消费命令队列快照 + SPSC 环拉取，对控件层不可见。
//   - 无设备（静默/测试）时 render_block 可由宿主手动驱动推进采样时钟。
//
// 格式管线：推入 int16（对齐 media/video_source.h AudioSink 口径）→ 图内 float32 →
// 设备格式输出；推入采样率 ≠ 设备采样率时按节点线性插值 SRC（Sinc 为后续增量）。
// ============================================================

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

// ---- 值类型 ----

/// @brief 音频缓冲（一次性播放/循环音效的样本载体；图内统一 float32 交错存储）。
struct AudioBuffer {
    int sample_rate = 0;  ///< 采样率（Hz，> 0）
    int channels = 0;  ///< 声道数（1 = mono / 2 = stereo）
    std::vector<float> samples;  ///< 交错样本（size == 帧数 * channels）

    [[nodiscard]] auto frame_count() const -> std::size_t {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
    /// @brief 结构合法性（采样率/声道/样本量自洽）。
    [[nodiscard]] auto valid() const -> bool {
        return sample_rate > 0 && (channels == 1 || channels == 2) && !samples.empty() &&
               samples.size() % static_cast<std::size_t>(channels) == 0;
    }
};

/// @brief 音频设备格式。
struct AudioDeviceFormat {
    int sample_rate = 48000;  ///< 采样率（Hz）
    int channels = 2;  ///< 声道数（首切片恒 2 = stereo 图）
};

/// @brief 传给 AudioNode::process 的单块渲染上下文。
struct AudioRenderContext {
    int frames = 0;  ///< 本块帧数
    int sample_rate = 48000;  ///< 设备采样率（Hz）
    double time = 0.0;  ///< 块起始时间（秒，context 采样时钟）
};

/// @brief 设备通道状态：Active = 设备线程在拉取；Silent = 静默模式（无设备/初始化失败）。
enum class AudioDeviceState {
    Active,
    Silent,
};

// ---- 设备后端接口 ----

/// @brief 音频设备后端接口（**设备层薄**：真实后端与测试桩共用同一出口契约）。
///
/// 真实后端（WASAPI/CoreAudio/…）在 start() 内自起设备线程并周期性回调 render_block；
/// 测试桩不起线程、由测试手动触发，保证确定性。
///
/// @note Thread: start/stop 由 AudioContext 所属线程调用；render_block 回调发生在设备线程
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioDeviceBackend {
  public:
    /// 渲染块回调：设备线程周期性调用（interleaved，AudioContext::channel_count() 声道）。
    using RenderFn = std::function<void(float *interleaved, int frames)>;

    virtual ~AudioDeviceBackend() = default;
    /// @brief 设备输出格式（图按此格式渲染）。
    [[nodiscard]] virtual auto format() const -> AudioDeviceFormat = 0;
    /// @brief 启动设备；返回 false = 初始化失败（AudioContext 进入静默模式）。
    virtual auto start(RenderFn render_block) -> bool = 0;
    /// @brief 停止设备（须等待设备线程退出后返回）。
    virtual auto stop() -> void = 0;
};

/// @brief 推流源重采样质量档位（默认 Linear；Sinc 为窗口 sinc 内核，流式语义见节点说明）。
enum class SrcQuality {
    Linear,  ///< 线性插值（默认，零前瞻延迟）
    Sinc,  ///< 窗口 sinc（32-tap Blackman 窗核 + 相位量化表；保留窗历史约 16 帧）
};

/// @brief 音频采集后端接口（麦克风等输入；**录制是显式能力**——设备/权限不可用走
///        `Result` 错显式报错，不进入静默降级）。
///
/// 真实后端（WASAPI capture / …）在 start() 内自起采集线程并周期性回调；测试桩手动
/// 触发保证确定性。
///
/// @note Thread: start/stop 由 AudioContext 所属线程调用；回调发生在采集线程
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioCaptureBackend {
  public:
    /// 采集回调：采集线程周期性调用（交错 float32，设备原生采样率/声道）。
    using CaptureFn = std::function<void(const float *interleaved, int frames, int rate, int channels)>;

    virtual ~AudioCaptureBackend() = default;
    /// @brief 启动采集；返回 false = 设备不可用/权限拒绝（调用方转为显式错误）。
    virtual auto start(CaptureFn on_pcm) -> bool = 0;
    /// @brief 停止采集（须等待采集线程退出后返回）。
    virtual auto stop() -> void = 0;
};

/// @brief 听者（Web Audio AudioListener 语义子集）：定义 PannerNode 声像的世界坐标系。
///
/// 默认：位置 (0,0,0)、朝向 (0,0,-1)、上向 (0,1,0)。UI 线程写（原子），渲染线程读。
///
/// @note Thread: setter 为 UI 线程；PannerNode 渲染线程读取
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioListener {
  public:
    /// @brief 听者位置（世界坐标）。
    auto set_position(float x, float y, float z) -> void;
    /// @brief 听者朝向（forward）与上向（up）；forward/up 须近似正交（未校验归一化）。
    auto set_orientation(float forward_x, float forward_y, float forward_z, float up_x, float up_y, float up_z) -> void;

    [[nodiscard]] auto position() const -> std::array<float, 3>;
    [[nodiscard]] auto forward() const -> std::array<float, 3>;
    [[nodiscard]] auto up() const -> std::array<float, 3>;

  private:
    std::atomic<float> px_{0.0F};
    std::atomic<float> py_{0.0F};
    std::atomic<float> pz_{0.0F};
    std::atomic<float> fx_{0.0F};
    std::atomic<float> fy_{0.0F};
    std::atomic<float> fz_{-1.0F};
    std::atomic<float> ux_{0.0F};
    std::atomic<float> uy_{1.0F};
    std::atomic<float> uz_{0.0F};
};

// ---- 参数与自动化 ----

/// @brief 音频参数：直写 + AutomationTimeline（语义对齐 Web Audio AudioParam）。
///
/// 时间单位为秒（context 采样时钟）；事件时刻要求**非负且不回退**（显式校验，
/// 违规返回 audio-param-invalid）。UI 线程经 COW 快照写入（原子 shared_ptr），
/// 渲染线程无锁读取——音频线程不阻塞于 UI。
///
/// @note Thread: set_value/automation 为 UI 线程；evaluate_block/has_automation 为渲染线程
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioParam {
  public:
    /// @brief 构造（default_value 为无自动化时的取值，如 Gain 节点默认 1.0）。
    explicit AudioParam(float default_value = 0.0F) : value_(default_value) {}

    /// @brief 直写当前值（立即生效；已排程的 automation 事件保留，届时仍接管）。
    auto set_value(float v) -> void { value_.store(v, std::memory_order_release); }
    /// @brief 最近已知值（渲染线程插值推进；UI 线程读取为近似值）。
    [[nodiscard]] auto value() const -> float { return value_.load(std::memory_order_acquire); }

    /// @brief 在时刻 t 跳变到 v。
    auto set_value_at_time(float v, double t) -> Result<void>;
    /// @brief 从上一事件落点线性过渡到时刻 t 的 v。
    auto linear_ramp_to_value_at_time(float v, double t) -> Result<void>;
    /// @brief 从上一事件落点指数过渡到时刻 t 的 v（起终点非零且同号，否则 audio-param-invalid）。
    auto exponential_ramp_to_value_at_time(float v, double t) -> Result<void>;
    /// @brief 从时刻 t 起以时间常数 tc 指数逼近 target（v(t)=target+(v0-target)·e^(-(t-t0)/tc)）。
    auto set_target_at_time(float target, double t, double time_constant) -> Result<void>;
    /// @brief 清空全部已排程事件（当前值保持不变）。
    auto cancel_scheduled_values() -> void;

    /// @brief 是否存在已排程事件（渲染线程判常数路径用）。
    [[nodiscard]] auto has_automation() const -> bool;

    /// @brief 渲染线程：评估 [t0, t0 + frames/rate) 逐样本值曲线写入 out（frames 个）。
    ///        无事件时整块写当前值常量；块末回写 value()。
    auto evaluate_block(double t0, int frames, int sample_rate, float *out) -> void;

  private:
    friend class AudioContext;

    enum class EventKind { Set, LinearRamp, ExponentialRamp, SetTarget };
    struct Event {
        EventKind kind{};
        double time = 0.0;  ///< 生效/终止时刻（秒）
        float value = 0.0F;  ///< Set/Ramp 终值；SetTarget 目标值
        double time_constant = 0.0;  ///< SetTarget 时间常数
        double start_time = 0.0;  ///< Ramp 锚点起点时刻（上一事件时刻）
        float start_value = 0.0F;  ///< Ramp 锚点起点值 / SetTarget 起始值
    };
    using EventList = std::vector<Event>;

    auto validate_schedule_time(double t) const -> Result<void>;  // 非负 + 不早于末事件
    auto ramp_impl(EventKind kind, float v, double t) -> Result<void>;
    /// 在当前事件链（UI 线程视角）上求时刻 t 的落点值（插桩 Ramp 锚点用）。
    [[nodiscard]] auto chain_value_at(const EventList &events, double t) const -> float;
    /// 纯函数：在给定事件链上求时刻 t 的值（渲染/UI 共用）。
    [[nodiscard]] auto evaluate_at(const EventList &events, double t) const -> float;
    /// COW 快照读取：拷贝指针即持有旧链生命期，锁外求值安全。
    [[nodiscard]] auto events_snapshot() const -> std::shared_ptr<const EventList> {
        std::lock_guard<std::mutex> lock(events_mutex_);
        return events_;
    }
    auto set_events(std::shared_ptr<const EventList> next) -> void {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_ = std::move(next);
    }

    std::atomic<float> value_{0.0F};
    // std::atomic<std::shared_ptr> 在 libc++/MSVC 未实现（仅 libstdc++ 有），不可移植；
    // 事件链为「UI 写 / 渲染读」COW 指针，用短临界区互斥保护即可。
    mutable std::mutex events_mutex_;
    std::shared_ptr<const EventList> events_{std::make_shared<const EventList>()};
};

// ---- 节点 ----

class AudioContext;

/// @brief 音频节点基类（图内处理单元；只能经 AudioContext 工厂创建）。
///
/// 图内固定 stereo（2 声道）处理：单声道源在上游混音时复制到双声道。节点有唯一的
/// 输入总线（全部上游连接预混）与唯一的输出总线（扇出只读）。
///
/// @note Thread: process 在渲染线程调用；拓扑/参数变更经 UI 线程提交
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioNode {
  public:
    virtual ~AudioNode() = default;
    AudioNode(const AudioNode &) = delete;
    auto operator=(const AudioNode &) -> AudioNode & = delete;

    /// @brief 输出声道数（首切片恒 2）。
    [[nodiscard]] auto channel_count() const -> int { return channels_; }
    /// @brief 所属 context。
    [[nodiscard]] auto context() const -> AudioContext & { return ctx_; }

  protected:
    friend class AudioContext;
    AudioNode(AudioContext &ctx, int channels) : ctx_(ctx), channels_(channels) {}

    /// @brief 处理一块：从输入总线（已含全部上游混音）计算本块输出。
    /// @note 渲染线程调用；不得触碰 UI 线程独占状态。
    virtual auto process(const AudioRenderContext &p) -> void = 0;

    [[nodiscard]] auto input_bus() const -> const float * { return in_bus_.data(); }
    auto input_bus() -> float * { return in_bus_.data(); }
    [[nodiscard]] auto output_bus() const -> const float * { return out_bus_.data(); }
    auto output_bus() -> float * { return out_bus_.data(); }
    auto ensure_capacity(int frames) -> void;

    AudioContext &ctx_;
    int channels_ = 2;
    std::vector<float> in_bus_;
    std::vector<float> out_bus_;
};

/// @brief 出口节点：多入汇聚混音（混音由图前置完成，process 恒等拷贝输入总线）。
class AudioDestinationNode final : public AudioNode {
  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit AudioDestinationNode(AudioContext &ctx) : AudioNode(ctx, 2) {}
};

/// @brief 推流源节点（**Aurora 特有 push 桥**）：App 推 int16 PCM → SPSC 无锁环 → 渲染线程拉取。
///
/// 与 VideoSource::set_audio_callback 的推模型直接对接。溢出策略：环满丢弃最旧
/// （CAS 单调推进读端，不回退消费进度）+ 键级一次告警；欠载输出静音并停在环头
/// （不跳相位）。推入采样率 ≠ 设备采样率时渲染侧线性插值 SRC。
///
/// @note Thread: push 为生产者线程（UI/解码线程）；拉取在渲染线程——二者构成 SPSC
/// @note Side-effects: 溢出时发一次 Warn 日志
/// @note Rebuildable: no
class AudioStreamSourceNode : public AudioNode {
  public:
    /// @brief 推入一包 16-bit PCM（签名对齐 VideoSource 音频回调）。
    ///
    /// 内部转 float32 并按节点声道映射（mono 复制 / stereo 直写）。sample_rate 须与
    /// 首次推入一致（中途变更返回错误）；声道数可逐包变化。
    /// @param pcm 交错样本（长度须为 channels 的整数倍）
    /// @param sample_rate 源采样率（Hz，> 0；须与首次推入一致）
    /// @param channels 声道数（1 = mono / 2 = stereo；可逐包变化）
    auto push(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> Result<void>;
    /// @brief 丢弃环内未消费样本（seek/重同步用；须在停止推流且无并发渲染时调用）。
    auto clear() -> void;
    /// @brief 环内待播帧数（源采样率口径；预取/诊断判据）。
    [[nodiscard]] auto buffered_frames() const -> std::size_t;
    /// @brief 溢出丢弃帧数累计（源采样率口径；诊断）。
    [[nodiscard]] auto dropped_frames() const -> std::uint64_t;
    /// @brief 重采样质量档位（默认 Linear；Sinc 需约 16 帧前瞻，窗尾帧须续推后才输出）。
    auto set_src_quality(SrcQuality q) -> void { src_quality_.store(static_cast<int>(q), std::memory_order_release); }
    [[nodiscard]] auto src_quality() const -> SrcQuality {
        return static_cast<SrcQuality>(src_quality_.load(std::memory_order_acquire));
    }

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    friend class AudioMicrophoneSourceNode;  // 派生类复用推流环构造
    explicit AudioStreamSourceNode(AudioContext &ctx, std::size_t ring_capacity_frames);

    std::size_t capacity_ = 0;  // 环容量（源采样率帧）
    std::atomic<std::uint64_t> read_{0};  // 消费端游标（帧，单调递增）
    std::atomic<std::uint64_t> write_{0};  // 生产端游标（帧，单调递增）
    std::vector<float> ring_;  // 交错 float32（capacity_ * 2 帧）
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> overflow_warned_{false};
    int src_rate_ = 0;  // 最近推入采样率（生产端写、渲染端读：推流前一致）
    double src_pos_ = 0.0;  // 渲染端 SRC 相位（源帧单位，环游标内偏移）
    bool pull_active_ = false;  // 渲染端是否已开始拉取（欠载停相位判据）
    std::vector<float> conv_;  // 推入端 int16→float 转换暂存（仅生产者线程触达）
    std::atomic<int> src_quality_{0};  // SrcQuality（0=Linear 1=Sinc；块级读取）
    std::vector<float> sinc_table_;  // Sinc 内核相位量化表（构造时生成，其后只读）
};

/// @brief 增益节点：out = in · gain（gain 为 AudioParam，支持 AutomationTimeline）。
class GainNode final : public AudioNode {
  public:
    /// @brief 增益参数（默认 1.0）。
    [[nodiscard]] auto gain() -> AudioParam & { return gain_; }

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit GainNode(AudioContext &ctx) : AudioNode(ctx, 2), gain_(1.0F) {}

    AudioParam gain_;
    std::vector<float> scratch_;  // 自动化逐样本值暂存（渲染线程私有）
};

/// @brief 缓冲源节点：一次性播放/循环音效（start 后按采样时钟推进，播完输出静音）。
class AudioBufferSourceNode final : public AudioNode {
  public:
    /// @brief 设置播放缓冲（结构非法返回 audio-buffer-invalid；可在播放前替换）。
    auto set_buffer(std::shared_ptr<const AudioBuffer> buffer) -> Result<void>;
    /// @brief 在时刻 when（context 采样时钟，秒）起播；when 已过则立即起播。
    auto start(double when = 0.0) -> Result<void>;
    /// @brief 停止播放（finished() 置真）。
    auto stop() -> void;
    /// @brief 循环播放开关（默认关）。
    auto set_loop(bool loop) -> void { loop_.store(loop, std::memory_order_release); }
    [[nodiscard]] auto looping() const -> bool { return loop_.load(std::memory_order_acquire); }
    /// @brief 是否已播完/停止（播完后输出静音）。
    [[nodiscard]] auto finished() const -> bool { return finished_.load(std::memory_order_acquire); }

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit AudioBufferSourceNode(AudioContext &ctx) : AudioNode(ctx, 2) {}

    std::mutex buffer_mutex_;                    // 保护 buffer_（libc++/MSVC 无 atomic<shared_ptr>）
    std::shared_ptr<const AudioBuffer> buffer_;  // COW 指针（UI 写 / 渲染读）
    std::atomic<bool> loop_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    std::atomic<double> start_when_{0.0};
    double play_pos_ = 0.0;  // 渲染端播放相位（缓冲帧单位）
};

/// @brief 声像节点：equal-power 立体声声像（语义对齐 Web Audio PannerNode 的
///        `panningModel='equalpower'` 默认形态；HRTF 卷积模型属后续增量）。
///
/// 由声源相对听者（`AudioContext::listener()`）的方位角计算声像 `pan = sin(az) ∈ [-1,1]`，
/// 左右增益 `L = cos((pan+1)·π/4)`、`R = sin((pan+1)·π/4)`（能量守恒：L²+R²=1）；距离衰减
/// inverse 模型 `gain = ref / (ref + rolloff·(max(d,ref) - ref))`。锥形衰减（orientation 锥）
/// 属后续增量。位置/衰减按**块级**更新（慢变参数）。
///
/// @note Thread: set_position/set_orientation 为 UI 线程（原子）；process 在渲染线程
/// @note Side-effects: none
/// @note Rebuildable: no
class PannerNode final : public AudioNode {
  public:
    /// @brief 声源位置（世界坐标，相对 listener 计算方位角与距离）。
    auto set_position(float x, float y, float z) -> void;
    [[nodiscard]] auto position() const -> std::array<float, 3>;
    /// @brief 距离衰减参考距离（默认 1.0，须 > 0）。
    auto set_ref_distance(float d) -> void { ref_distance_.store(d > 0.0F ? d : 1.0F, std::memory_order_release); }
    [[nodiscard]] auto ref_distance() const -> float { return ref_distance_.load(std::memory_order_acquire); }
    /// @brief 距离衰减滚降系数（默认 1.0，须 ≥ 0）。
    auto set_rolloff(float r) -> void { rolloff_.store(r < 0.0F ? 0.0F : r, std::memory_order_release); }
    [[nodiscard]] auto rolloff() const -> float { return rolloff_.load(std::memory_order_acquire); }

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit PannerNode(AudioContext &ctx) : AudioNode(ctx, 2) {}

    std::atomic<float> px_{0.0F};
    std::atomic<float> py_{0.0F};
    std::atomic<float> pz_{0.0F};
    std::atomic<float> ref_distance_{1.0F};
    std::atomic<float> rolloff_{1.0F};
};

/// @brief 频谱分析节点：时域波形 + FFT 频域快照（语义对齐 Web Audio AnalyserNode）。
///
/// 直通节点（输出 = 输入）；分析在渲染线程逐块进行（Blackman 窗 + radix-2 复 FFT，
/// 自实现零三方；频域 dB 域平滑 `smoothed = τ·prev + (1-τ)·cur`）。快照访问器在 UI
/// 线程读取最近一次分析结果（mutex 保护）。
///
/// @note Thread: set_* 为 UI 线程；get_* 为 UI 线程（读快照）；process 在渲染线程
/// @note Side-effects: none
/// @note Rebuildable: no
class AnalyserNode final : public AudioNode {
  public:
    /// @brief 设置 FFT 点数（2 的幂，32..32768；非法返回 audio-param-invalid）。
    auto set_fft_size(int n) -> Result<void>;
    [[nodiscard]] auto fft_size() const -> int { return fft_size_.load(std::memory_order_acquire); }
    /// @brief 频域桶数（= fft_size/2）。
    [[nodiscard]] auto frequency_bin_count() const -> int { return fft_size() / 2; }
    /// @brief 频域平滑系数 τ ∈ [0,1]（默认 0.8；0 = 无平滑）。
    auto set_smoothing_time_constant(float v) -> void;
    [[nodiscard]] auto smoothing_time_constant() const -> float { return smoothing_.load(std::memory_order_acquire); }
    /// @brief 频域 dB 钳位区间（默认 [-100, -30]；写入保持 min < max）。
    auto set_min_decibels(float v) -> void { min_db_.store(v, std::memory_order_release); }
    [[nodiscard]] auto min_decibels() const -> float { return min_db_.load(std::memory_order_acquire); }
    auto set_max_decibels(float v) -> void { max_db_.store(v, std::memory_order_release); }
    [[nodiscard]] auto max_decibels() const -> float { return max_db_.load(std::memory_order_acquire); }

    /// @brief 频域快照：各桶 dB 值（钳位到 [min_decibels, max_decibels]）。
    ///        out.size() 不足 frequency_bin_count() 时按可用长度填充。
    auto get_float_frequency_data(std::span<float> out) const -> void;
    /// @brief 频域快照字节形：dB 归一映射到 0..255（min→0，max→255）。
    auto get_byte_frequency_data(std::span<std::uint8_t> out) const -> void;
    /// @brief 时域快照字节形：最近 fft_size 个样本 -1..1 映射 0..255（静音 = 128）。
    auto get_byte_time_data(std::span<std::uint8_t> out) -> void;

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit AnalyserNode(AudioContext &ctx) : AudioNode(ctx, 2) {}
    auto run_fft_locked() -> void;  // 渲染线程：对当前时域环做 FFT + 平滑（持锁）

    std::atomic<int> fft_size_{2048};
    std::atomic<float> smoothing_{0.8F};
    std::atomic<float> min_db_{-100.0F};
    std::atomic<float> max_db_{-30.0F};

    mutable std::mutex mutex_;  // 保护以下渲染端状态与 UI 快照读取
    std::vector<float> time_ring_;  // 时域环形缓冲（mono，capacity = fft_size）
    std::size_t time_write_ = 0;  // 环写游标
    std::size_t time_fill_ = 0;  // 环内有效样本数
    std::vector<float> window_;  // Blackman 窗（fft_size 变更时重建）
    std::vector<float> fft_re_;  // FFT 工作缓冲（实部）
    std::vector<float> fft_im_;  // FFT 工作缓冲（虚部）
    std::vector<float> spectrum_db_;  // 平滑后频域 dB 快照（bin 数）
    std::vector<std::uint8_t> time_bytes_;  // 时域字节快照（fft_size）
};

/// @brief 麦克风源节点（录制链输入端）：`AudioCaptureBackend` 采集线程推入，图内拉取。
///
/// 只能经 `AudioContext::create_microphone_source()` 创建——工厂启动采集失败时返回
/// 显式 `Result` 错误（audio-device-unavailable），**不静默降级**（录制是显式能力）。
/// 推入侧复用推流环（SPSC：唯一生产者 = 采集线程）。
///
/// @note Thread: 采集线程 push；渲染线程拉取
/// @note Side-effects: 持有采集后端线程生命周期（节点析构先停采集再释放环）
/// @note Rebuildable: no
class AudioMicrophoneSourceNode final : public AudioStreamSourceNode {
  public:
    ~AudioMicrophoneSourceNode() override;

  private:
    friend class AudioContext;
    explicit AudioMicrophoneSourceNode(AudioContext &ctx, std::unique_ptr<AudioCaptureBackend> backend);

    std::unique_ptr<AudioCaptureBackend> capture_;  // 先于基类环析构（派生成员先销毁）
};

/// @brief 录制汇节点（录制链输出端）：直通节点，录制途经样本为 PCM/WAV。
///
/// `start()` 起录、`stop()` 停录；样本常驻内存（交错 float32，设备采样率 stereo），
/// 可导出 WAV 字节或落盘（落盘失败返回显式 `Result` 错误，不静默）。
///
/// @note Thread: start/stop/get* 为 UI 线程（mutex）；process 在渲染线程
/// @note Side-effects: save_wav 落盘 I/O
/// @note Rebuildable: no
class AudioRecordingDestinationNode final : public AudioNode {
  public:
    /// @brief 开始录制（幂等；closed 上下文返回 audio-context-closed）。
    auto start() -> Result<void>;
    /// @brief 停止录制（幂等；保留已录样本供导出）。
    auto stop() -> void;
    [[nodiscard]] auto is_recording() const -> bool { return recording_.load(std::memory_order_acquire); }
    /// @brief 已录样本快照（交错 float32 stereo；线程安全拷贝）。
    [[nodiscard]] auto recording() const -> std::vector<float>;
    /// @brief 已录帧数（stereo 帧口径）。
    [[nodiscard]] auto recorded_frames() const -> std::size_t;
    /// @brief 导出 WAV 字节（16-bit PCM stereo，RIFF 头；样本按 context 采样率）。
    [[nodiscard]] auto to_wav_bytes() const -> std::vector<std::uint8_t>;
    /// @brief 落盘 WAV（目录不存在/无权限等失败返回 audio-recording-failed，不静默）。
    auto save_wav(const std::string &path) const -> Result<void>;

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit AudioRecordingDestinationNode(AudioContext &ctx) : AudioNode(ctx, 2) {}

    std::atomic<bool> recording_{false};
    mutable std::mutex data_mutex_;  // 保护录制缓冲（渲染线程写 / UI 线程读）
    std::vector<float> data_;  // 交错 float32 stereo（设备采样率）
};

// ---- 上下文 ----

/// @brief 音频上下文：节点生命周期与连接拓扑的唯一属主（Web Audio AudioContext 语义）。
///
/// - 拓扑变更（connect/disconnect）经 UI 线程校验（禁环 DAG + 归属 + 重复边）后提交
///   命令队列，渲染线程在块首无锁消费；无设备运行时（静默/测试）直接在调用线程生效。
/// - suspend 冻结采样时钟并输出静音；close 终止（后续操作返回 audio-context-closed）。
/// - 无内置后端编译（AURORA_ENABLE_AUDIO 关）或设备初始化失败 → 静默模式：
///   图照常运转、样本消费后丢弃，device_state() 报 Silent。
///
/// @note Thread: 图变更/参数为 UI 线程；render_block 为单渲染线程（设备线程或宿主手动驱动）
/// @note Side-effects: 设备后端线程生命周期；静默模式零系统副作用
/// @note Rebuildable: no
class AudioContext {
  public:
    /// @brief 构造：注入设备后端（测试桩）；nullptr 时按编译期后端宏创建默认设备
    ///        （未编译内置后端 → 恒静默模式）。可另注入采集后端（麦克风测试桩；
    ///        nullptr 时 create_microphone_source 按需创建默认采集后端，不可用则显式报错）。
    explicit AudioContext(std::unique_ptr<AudioDeviceBackend> device_backend = nullptr,
                          std::unique_ptr<AudioCaptureBackend> capture_backend = nullptr);
    ~AudioContext();
    AudioContext(const AudioContext &) = delete;
    auto operator=(const AudioContext &) -> AudioContext & = delete;

    // ---- 拓扑 ----
    /// @brief 连接 src → dst（校验归属/重复/禁环；DAG）。
    auto connect(const std::shared_ptr<AudioNode> &src, const std::shared_ptr<AudioNode> &dst) -> Result<void>;
    /// @brief 断开 src → dst（边不存在返回 audio-edge-not-found）。
    auto disconnect(const std::shared_ptr<AudioNode> &src, const std::shared_ptr<AudioNode> &dst) -> Result<void>;
    /// @brief 当前边数（UI 视角）。
    [[nodiscard]] auto connection_count() const -> std::size_t { return edges_.size(); }

    // ---- 工厂（节点生命周期归 context，存活至 close/析构） ----
    [[nodiscard]] auto create_destination() -> std::shared_ptr<AudioDestinationNode>;
    [[nodiscard]] auto create_stream_source(std::size_t ring_capacity_frames = 16384)
        -> std::shared_ptr<AudioStreamSourceNode>;
    [[nodiscard]] auto create_gain() -> std::shared_ptr<GainNode>;
    [[nodiscard]] auto create_buffer_source() -> std::shared_ptr<AudioBufferSourceNode>;
    [[nodiscard]] auto create_panner() -> std::shared_ptr<PannerNode>;
    [[nodiscard]] auto create_analyser() -> std::shared_ptr<AnalyserNode>;
    [[nodiscard]] auto create_recording_destination() -> std::shared_ptr<AudioRecordingDestinationNode>;
    /// @brief 麦克风源（**录制是显式能力**）：采集设备不可用/权限拒绝时返回
    ///        audio-device-unavailable 显式错误，不静默降级。
    auto create_microphone_source() -> Result<std::shared_ptr<AudioMicrophoneSourceNode>>;

    /// @brief 出口节点（构造时创建）。
    [[nodiscard]] auto destination() const -> const std::shared_ptr<AudioDestinationNode> & { return destination_; }
    /// @brief 听者（PannerNode 世界坐标系；默认位于原点、朝 -Z、上向 +Y）。
    [[nodiscard]] auto listener() -> AudioListener & { return listener_; }

    // ---- 生命周期 ----
    auto suspend() -> Result<void>;
    auto resume() -> Result<void>;
    /// @brief 关闭（终止设备与图；终态，不可逆）。
    ///        返回前设备线程已退出并停止设备后端；后端对象保留至上下文析构时释放，
    ///        期间任何节点/连接操作均返回 audio-context-closed。
    auto close() -> Result<void>;
    [[nodiscard]] auto closed() const -> bool { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] auto suspended() const -> bool { return suspended_.load(std::memory_order_acquire); }

    // ---- 设备/格式/时钟 ----
    [[nodiscard]] auto device_state() const -> AudioDeviceState;
    /// @brief 静默模式（无设备/初始化失败）：图照常运转，输出被丢弃。
    [[nodiscard]] auto silent() const -> bool { return device_state() == AudioDeviceState::Silent; }
    /// @brief 设备采样率（静默模式为处理格式 48000）。
    [[nodiscard]] auto sample_rate() const -> int { return format_.sample_rate; }
    /// @brief 设备声道数（首切片恒 2）。
    [[nodiscard]] auto channel_count() const -> int { return format_.channels; }
    /// @brief 采样时钟（秒；suspend 冻结，close 后停走）。
    [[nodiscard]] auto current_time() const -> double;

    // ---- 主音量 ----
    auto set_master_volume(float v) -> void { master_volume_.store(v, std::memory_order_release); }
    [[nodiscard]] auto master_volume() const -> float { return master_volume_.load(std::memory_order_acquire); }

    /// @brief 渲染一块（设备线程由后端回调驱动；静默/测试模式可宿主手动驱动推进时钟）。
    /// @param interleaved_out 交错输出缓冲，容量须 ≥ frames * channel_count()
    /// @param frames 本块帧数（> 0）
    /// @note Thread: 单渲染线程；与 UI 线程经命令队列/SPSC 环解耦
    auto render_block(float *interleaved_out, int frames) -> void;

  private:
    friend class AudioNode;

    struct GraphCommand {
        enum class Kind { Connect, Disconnect };
        Kind kind{};
        std::shared_ptr<AudioNode> src;
        std::shared_ptr<AudioNode> dst;
    };

    // UI 侧（仅 UI 线程触达）
    std::vector<std::shared_ptr<AudioNode>> nodes_;  // 全部节点（生命周期锚）
    std::vector<std::pair<AudioNode *, AudioNode *>> edges_;  // UI 视角边表（校验/计数）
    std::shared_ptr<AudioDestinationNode> destination_;
    AudioListener listener_;  // PannerNode 世界坐标系听者

    // 渲染侧（仅渲染线程触达；direct_graph_ 时归调用线程）
    struct Adjacency {
        std::vector<AudioNode *> sources;  // 上游
        std::vector<AudioNode *> consumers;  // 下游
    };
    std::unordered_map<AudioNode *, Adjacency> adj_;  // 渲染侧邻接表
    std::vector<AudioNode *> topo_;  // 拓扑序（源先于汇；不含 destination）

    // 命令队列（UI → 渲染，SPSC 定长环；direct_graph_ 时旁路）
    static constexpr std::size_t kCommandRingCapacity = 256;
    std::vector<GraphCommand> command_ring_;
    std::atomic<std::uint64_t> cmd_write_{0};
    std::atomic<std::uint64_t> cmd_read_{0};

    std::unique_ptr<AudioDeviceBackend> device_;
    std::unique_ptr<AudioCaptureBackend> capture_backend_;  // 注入的采集后端（create_microphone_source 惰性消费）
    AudioDeviceFormat format_{};
    bool direct_graph_ = false;  // 无运行中设备：图变更直接生效（无渲染线程竞态）
    std::atomic<bool> suspended_{false};
    std::atomic<bool> closed_{false};
    std::atomic<float> master_volume_{1.0F};
    std::atomic<std::uint64_t> rendered_samples_{0};  // 采样时钟（帧）

    auto owns_node(const AudioNode *n) const -> bool;
    auto would_cycle(const AudioNode *src, AudioNode *dst) const -> bool;  // DFS：src 自 dst 可达？
    auto apply_connect(AudioNode *src, AudioNode *dst) -> void;
    auto apply_disconnect(AudioNode *src, AudioNode *dst) -> void;
    auto rebuild_topo() -> void;  // Kahn（源先于汇）
    auto drain_commands() -> void;  // 渲染线程块首消费
    auto render_silence(float *out, int frames) const -> void;
};

}  // namespace aurora
