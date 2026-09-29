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

    /// @brief 帧数。
    /// @return samples.size() / channels（声道数非法时为 0）。
    [[nodiscard]] auto frame_count() const -> std::size_t {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
    /// @brief 结构合法性（采样率/声道/样本量自洽）。
    /// @return 采样率 > 0、声道为 1/2、样本非空且数量为声道整数倍时为 true。
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

// 公共 API 枚举：device_state() 的返回值、silent() 的判据，不落在结构体/容器字段里。
// 底层类型是公共 API 形态的一部分，本库按语义选型而非体积取向，改窄仅省 3 字节。
/// @brief 设备通道状态：Active = 设备线程在拉取；Silent = 静默模式（无设备/初始化失败）。
enum class AudioDeviceState {  // NOLINT(performance-enum-size)
    Active,  ///< 设备线程周期性拉取 render_block（正常出声）
    Silent,  ///< 静默模式：图照常运转、渲染样本消费后丢弃
};

// ---- 设备后端接口 ----

/// @brief 音频设备后端接口（**设备层薄**：真实后端与测试桩共用同一出口契约）。
/// 真实后端（WASAPI/ALSA/…）在 start() 内自起设备线程并周期性回调 render_block；
/// 测试桩不起线程、由测试手动触发，保证确定性。
/// **浏览器例外**（如实申报）：Web Audio 的音频渲染线程在 JS 侧、调不进 wasm，故
/// `WebAudioDeviceBackend` 由**主线程**（= UI 线程）的定间隔回调驱动 render_block，
/// 采样经推式环缓冲交给 JS 消费——「render_block 发生在设备线程」在此不成立，
/// 详见 src/aurora/media/audio_webaudio.h 文件头。
///
/// @note Thread: start/stop 由 AudioContext 所属线程调用；render_block 回调发生在设备线程
///       （Web Audio 后端：浏览器主线程）
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioDeviceBackend {
  public:
    /// @brief 渲染块回调契约：设备线程周期性调用（interleaved，AudioContext::channel_count() 声道）。
    /// @param interleaved 输出缓冲首地址：交错 float32，写入 [0, frames) 帧。
    /// @param frames 本块帧数（> 0）。
    /// @return 回调无返回值（void）：产出经 interleaved 缓冲写出。
    using RenderFn = std::function<void(float *interleaved, int frames)>;

    // 五法则（CODING_STANDARDS.md §5.1）：后端只经 unique_ptr 注入 AudioContext，从不按值转移所有权；
    // 显式禁用拷贝/移动，与各具体后端（Wasapi / Alsa / WebAudio / Fake）的声明一致。
    // 默认构造须显式保留：一旦声明拷贝/移动构造，隐式默认构造即消失，而后端派生类普遍 `X() = default`。
    /// @brief 默认构造：派生后端零参自起（显式保留，理由见上方五法则注记）。
    AudioDeviceBackend() = default;
    /// @brief 虚析构：经基类指针销毁具体后端。
    virtual ~AudioDeviceBackend() = default;
    /// @brief 拷贝构造禁用——后端独占设备资源，仅经 unique_ptr 注入（声明见上方五法则注记）。
    AudioDeviceBackend(const AudioDeviceBackend &) = delete;
    /// @brief 拷贝赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(const AudioDeviceBackend &) -> AudioDeviceBackend & = delete;
    /// @brief 移动构造禁用——设备线程/句柄所有权不可转移（声明见上方五法则注记）。
    AudioDeviceBackend(AudioDeviceBackend &&) = delete;
    /// @brief 移动赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(AudioDeviceBackend &&) -> AudioDeviceBackend & = delete;

    /// @brief 设备输出格式（图按此格式渲染）。
    /// @return 采样率与声道数快照。
    [[nodiscard]] virtual auto format() const -> AudioDeviceFormat = 0;
    /// @brief 启动设备；返回 false = 初始化失败（AudioContext 进入静默模式）。
    /// @param render_block 块渲染回调：后端设备线程周期性调用（Web Audio 后端为浏览器主线程）。
    /// @return 启动成功与否。
    virtual auto start(RenderFn render_block) -> bool = 0;
    /// @brief 停止设备（须等待设备线程退出后返回）。
    virtual auto stop() -> void = 0;
};

// 公共 API 枚举：set_src_quality()/src_quality() 的形参与返回值，存储侧为 atomic<int>（跨线程块级读取），
// 底层类型不进入任何结构体布局；改窄属纯体积取向且会动摇公共签名（口径 B 类）。
/// @brief 推流源重采样质量档位（默认 Linear；Sinc 为窗口 sinc 内核，流式语义见节点说明）。
enum class SrcQuality {  // NOLINT(performance-enum-size)
    Linear,  ///< 线性插值（默认，零前瞻延迟）
    Sinc,  ///< 窗口 sinc（32-tap Blackman 窗核 + 相位量化表；保留窗历史约 16 帧）
};

/// @brief 音频采集后端接口（麦克风等输入；**录制是显式能力**——设备/权限不可用走
///        `Result` 错显式报错，不进入静默降级）。
/// 真实后端（WASAPI capture / …）在 start() 内自起采集线程并周期性回调；测试桩手动
/// 触发保证确定性。
///
/// @note Thread: start/stop 由 AudioContext 所属线程调用；回调发生在采集线程
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioCaptureBackend {
  public:
    /// @brief 采集回调契约：采集线程周期性调用（交错 float32，设备原生采样率/声道）。
    /// @param interleaved 采集样本首地址（交错 float32）。
    /// @param frames 本包帧数。
    /// @param rate 采集采样率（Hz）。
    /// @param channels 声道数。
    /// @return 回调无返回值（void）：产出经 interleaved 缓冲写出。
    using CaptureFn = std::function<void(const float *interleaved, int frames, int rate, int channels)>;

    // 五法则（CODING_STANDARDS.md §5.1）：采集后端只经 unique_ptr 持有（context 或麦克风节点），
    // 从不按值转移所有权；显式禁用拷贝/移动，与各具体后端的声明一致。
    // 默认构造须显式保留：一旦声明拷贝/移动构造，隐式默认构造即消失，而后端派生类普遍 `X() = default`。
    /// @brief 默认构造：派生后端零参自起（显式保留，理由见上方五法则注记）。
    AudioCaptureBackend() = default;
    /// @brief 虚析构：经基类指针销毁具体采集后端。
    virtual ~AudioCaptureBackend() = default;
    /// @brief 拷贝构造禁用——独占采集设备资源，仅经 unique_ptr 持有（声明见上方五法则注记）。
    AudioCaptureBackend(const AudioCaptureBackend &) = delete;
    /// @brief 拷贝赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(const AudioCaptureBackend &) -> AudioCaptureBackend & = delete;
    /// @brief 移动构造禁用——采集线程所有权不可转移（声明见上方五法则注记）。
    AudioCaptureBackend(AudioCaptureBackend &&) = delete;
    /// @brief 移动赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(AudioCaptureBackend &&) -> AudioCaptureBackend & = delete;

    /// @brief 启动采集；返回 false = 设备不可用/权限拒绝（调用方转为显式错误）。
    /// @param on_pcm 采集回调：采集线程周期性调用。
    /// @return 启动成功与否。
    virtual auto start(CaptureFn on_pcm) -> bool = 0;
    /// @brief 停止采集（须等待采集线程退出后返回）。
    virtual auto stop() -> void = 0;
};

/// @brief 听者（Web Audio AudioListener 语义子集）：定义 PannerNode 声像的世界坐标系。
/// 默认：位置 (0,0,0)、朝向 (0,0,-1)、上向 (0,1,0)。UI 线程写（原子），渲染线程读。
///
/// @note Thread: setter 为 UI 线程；PannerNode 渲染线程读取
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioListener {
  public:
    /// @brief 听者位置（世界坐标）。
    /// @param x 横坐标。
    /// @param y 纵坐标。
    /// @param z 深度坐标。
    auto set_position(float x, float y, float z) -> void;
    /// @brief 听者朝向（forward）与上向（up）；forward/up 须近似正交（未校验归一化）。
    /// @param forward_x 朝向向量 x 分量。
    /// @param forward_y 朝向向量 y 分量。
    /// @param forward_z 朝向向量 z 分量。
    /// @param up_x 上向向量 x 分量。
    /// @param up_y 上向向量 y 分量。
    /// @param up_z 上向向量 z 分量。
    auto set_orientation(float forward_x, float forward_y, float forward_z, float up_x, float up_y, float up_z) -> void;

    /// @brief 听者位置快照。
    /// @return {x, y, z}（世界坐标）。
    [[nodiscard]] auto position() const -> std::array<float, 3>;
    /// @brief 听者朝向快照。
    /// @return forward 单位化前原值 {x, y, z}（默认 (0,0,-1)）。
    [[nodiscard]] auto forward() const -> std::array<float, 3>;
    /// @brief 听者上向快照。
    /// @return up 向量 {x, y, z}（默认 (0,1,0)）。
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
    /// @param default_value 初始直写值（秒/无量纲由宿主节点语义决定）。
    explicit AudioParam(float default_value = 0.0F) : value_(default_value) {}

    /// @brief 直写当前值（立即生效；已排程的 automation 事件保留，届时仍接管）。
    /// @param v 新当前值。
    auto set_value(float v) -> void { value_.store(v, std::memory_order_release); }
    /// @brief 最近已知值（渲染线程插值推进；UI 线程读取为近似值）。
    /// @return value_ 原子读回。
    [[nodiscard]] auto value() const -> float { return value_.load(std::memory_order_acquire); }

    /// @brief 在时刻 t 跳变到 v。
    /// @param v 目标值。
    /// @param t 生效时刻（秒，context 采样时钟；非负且不回退）。
    /// @return Ok 或 audio-param-invalid（时刻非法）。
    auto set_value_at_time(float v, double t) -> Result<void>;
    /// @brief 从上一事件落点线性过渡到时刻 t 的 v。
    /// @param v 终值。
    /// @param t 终止时刻（秒）。
    /// @return Ok 或 audio-param-invalid（时刻非法）。
    auto linear_ramp_to_value_at_time(float v, double t) -> Result<void>;
    /// @brief 从上一事件落点指数过渡到时刻 t 的 v（起终点非零且同号，否则 audio-param-invalid）。
    /// @param v 终值（须与起点同号非零）。
    /// @param t 终止时刻（秒）。
    /// @return Ok 或 audio-param-invalid。
    auto exponential_ramp_to_value_at_time(float v, double t) -> Result<void>;
    /// @brief 从时刻 t 起以时间常数 tc 指数逼近 target（v(t)=target+(v0-target)·e^(-(t-t0)/tc)）。
    /// @param target 渐近目标值。
    /// @param t 起始时刻（秒）。
    /// @param time_constant 时间常数（秒，须 > 0）。
    /// @return Ok 或 audio-param-invalid（时刻非法 / 时间常数 ≤ 0）。
    auto set_target_at_time(float target, double t, double time_constant) -> Result<void>;
    /// @brief 清空全部已排程事件（当前值保持不变）。
    auto cancel_scheduled_values() -> void;

    /// @brief 是否存在已排程事件（渲染线程判常数路径用）。
    /// @return 事件链非空即为 true。
    [[nodiscard]] auto has_automation() const -> bool;

    /// @brief 渲染线程：评估 [t0, t0 + frames/rate) 逐样本值曲线写入 out（frames 个）。
    ///        无事件时整块写当前值常量；块末回写 value()。
    /// @param t0 块起始时刻（秒）。
    /// @param frames 本块样本数。
    /// @param sample_rate 采样率（Hz，用于换算样本间隔）。
    /// @param out 输出缓冲（容量 ≥ frames）。
    auto evaluate_block(double t0, int frames, int sample_rate, float *out) -> void;

  private:
    friend class AudioContext;

    // 私有枚举且作为 Event 结构体字段存储（事件链按值装在 vector 里，UI/渲染两端频繁拷贝）：
    // 值域仅 0..3，收窄底层类型无损语义，也与仓内其它「按字段存储的枚举」(TextUnit : std::uint8_t) 一致。
    /// @brief 自动化事件种类（Set 跳变 / 两种 Ramp / SetTarget 渐近）。
    enum class EventKind : std::uint8_t { Set, LinearRamp, ExponentialRamp, SetTarget };
    /// @brief 单条自动化事件：种类 + 生效时刻 + 目标值 +（按种类取用的）锚点与时间常数。
    struct Event {
        EventKind kind{};  ///< 事件种类
        double time = 0.0;  ///< 生效/终止时刻（秒）
        float value = 0.0F;  ///< Set/Ramp 终值；SetTarget 目标值
        double time_constant = 0.0;  ///< SetTarget 时间常数
        double start_time = 0.0;  ///< Ramp 锚点起点时刻（上一事件时刻）
        float start_value = 0.0F;  ///< Ramp 锚点起点值 / SetTarget 起始值
    };
    /// @brief 事件链容器（按时刻非降序排列）。
    using EventList = std::vector<Event>;

    /// @brief 校验排程时刻：非负且不早于末事件。
    /// @param t 候选时刻（秒）。
    /// @return Ok 或 audio-param-invalid。
    auto validate_schedule_time(double t) const -> Result<void>;  // 非负 + 不早于末事件
    /// @brief Ramp 排程实现：锚点取上一事件落点（指数 ramp 另校验起终点同号非零）。
    /// @param kind 事件种类（LinearRamp / ExponentialRamp）。
    /// @param v 终值。
    /// @param t 终止时刻（秒）。
    /// @return Ok 或 audio-param-invalid。
    auto ramp_impl(EventKind kind, float v, double t) -> Result<void>;
    /// @brief 在当前事件链（UI 线程视角）上求时刻 t 的落点值（插桩 Ramp 锚点用）。
    /// @param events 事件链（非降序）。
    /// @param t 求值时刻（秒）。
    /// @return t 时刻的参数值。
    [[nodiscard]] auto chain_value_at(const EventList &events, double t) const -> float;
    /// @brief 纯函数：在给定事件链上求时刻 t 的值（渲染/UI 共用）。
    /// @param events 事件链（非降序）。
    /// @param t 求值时刻（秒）。
    /// @return t 时刻的参数值；空链返回当前直写值。
    [[nodiscard]] auto evaluate_at(const EventList &events, double t) const -> float;
    /// @brief COW 快照读取：拷贝指针即持有旧链生命期，锁外求值安全。
    /// @return 当前事件链的只读共享指针。
    [[nodiscard]] auto events_snapshot() const -> std::shared_ptr<const EventList> {
        std::scoped_lock lock(events_mutex_);
        return events_;
    }
    /// @brief COW 快照写入（UI 线程排程末尾整体替换）。
    /// @param next 新事件链快照。
    auto set_events(std::shared_ptr<const EventList> next) -> void {
        std::scoped_lock lock(events_mutex_);
        events_ = std::move(next);
    }

    std::atomic<float> value_{0.0F};  ///< 直写当前值（release/acquire 跨线程）
    // std::atomic<std::shared_ptr> 在 libc++/MSVC 未实现（仅 libstdc++ 有），不可移植；
    // 事件链为「UI 写 / 渲染读」COW 指针，用短临界区互斥保护即可。
    /// @brief 事件链快照指针的保护锁（临界区仅拷贝/替换指针）。
    mutable std::mutex events_mutex_;
    /// @brief COW 事件链快照（整体替换）。
    std::shared_ptr<const EventList> events_{std::make_shared<const EventList>()};
};

// ---- 节点 ----

class AudioContext;

/// @brief 音频节点基类（图内处理单元；只能经 AudioContext 工厂创建）。
/// 图内固定 stereo（2 声道）处理：单声道源在上游混音时复制到双声道。节点有唯一的
/// 输入总线（全部上游连接预混）与唯一的输出总线（扇出只读）。
///
/// @note Thread: process 在渲染线程调用；拓扑/参数变更经 UI 线程提交
/// @note Side-effects: none
/// @note Rebuildable: no
class AudioNode {
  public:
    /// @brief 虚析构：经 AudioNode 基指针销毁各派生节点。
    virtual ~AudioNode() = default;
    // 五法则（CODING_STANDARDS.md §5.1）：节点持 ctx 引用 + 输入/输出总线，生命周期归 AudioContext，
    // 只经 shared_ptr 流转；拷贝早已显式禁用，移动同样无意为默认（派生节点因本类而隐式不可移动）。
    /// @brief 拷贝构造禁用——节点身份即图内拓扑位置，不可复制（声明见上方五法则注记）。
    AudioNode(const AudioNode &) = delete;
    /// @brief 拷贝赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(const AudioNode &) -> AudioNode & = delete;
    /// @brief 移动构造禁用——ctx_ 引用与总线不可安全转移（声明见上方五法则注记）。
    AudioNode(AudioNode &&) = delete;
    /// @brief 移动赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(AudioNode &&) -> AudioNode & = delete;

    /// @brief 输出声道数（首切片恒 2）。
    /// @return channels_。
    [[nodiscard]] auto channel_count() const -> int { return channels_; }
    /// @brief 所属 context。
    /// @return 构造期注入的 context 引用（生命周期由工厂保证长于节点）。
    [[nodiscard]] auto context() const -> AudioContext & { return ctx_; }

  protected:
    friend class AudioContext;
    /// @brief 受保护构造：仅 context 工厂（friend AudioContext）可创建节点。
    /// @param ctx 所属音频上下文（存为引用）。
    /// @param channels 声道数（本图恒 2）。
    AudioNode(AudioContext &ctx, int channels) : ctx_(ctx), channels_(channels) {}

    /// @brief 处理一块：从输入总线（已含全部上游混音）计算本块输出。
    /// @param p 本块渲染上下文（帧数、设备采样率与块起始时间）。
    /// @note 渲染线程调用；不得触碰 UI 线程独占状态。
    virtual auto process(const AudioRenderContext &p) -> void = 0;

    /// @brief 输入总线（const）：本块预混后的上游样本。
    /// @return 交错 float32 首地址。
    [[nodiscard]] auto input_bus() const -> const float * { return in_bus_.data(); }
    /// @brief 输入总线（可写）：渲染线程混入上游样本用。
    /// @return 交错 float32 首地址。
    auto input_bus() -> float * { return in_bus_.data(); }
    /// @brief 输出总线（const）：下游扇出只读。
    /// @return 交错 float32 首地址。
    [[nodiscard]] auto output_bus() const -> const float * { return out_bus_.data(); }
    /// @brief 输出总线（可写）：process 写本节点结果用。
    /// @return 交错 float32 首地址。
    auto output_bus() -> float * { return out_bus_.data(); }
    /// @brief 预扩两条总线到 frames * channels_ 样本（不足时扩容）。
    /// @param frames 本块帧数。
    auto ensure_capacity(int frames) -> void;

    AudioContext &ctx_;  ///< 所属上下文（引用，工厂保证生命期）
    int channels_ = 2;  ///< 声道数（首切片恒 2）
    std::vector<float> in_bus_;  ///< 输入总线（交错 float32）
    std::vector<float> out_bus_;  ///< 输出总线（交错 float32）
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
    /// 内部转 float32 并按节点声道映射（mono 复制 / stereo 直写）。sample_rate 须与
    /// 首次推入一致（中途变更返回错误）；声道数可逐包变化。
    /// @param pcm 交错样本（长度须为 channels 的整数倍）
    /// @param sample_rate 源采样率（Hz，> 0；须与首次推入一致）
    /// @param channels 声道数（1 = mono / 2 = stereo；可逐包变化）
    /// @return Ok，或非法参数错误（采样率/声道/帧对齐非法，或与首包锁定的采样率不一致）。
    auto push(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> Result<void>;
    /// @brief 丢弃环内未消费样本（seek/重同步用；须在停止推流且无并发渲染时调用）。
    auto clear() -> void;
    /// @brief 环内待播帧数（源采样率口径；预取/诊断判据）。
    /// @return 写游标 - 读游标（帧）。
    [[nodiscard]] auto buffered_frames() const -> std::size_t;
    /// @brief 溢出丢弃帧数累计（源采样率口径；诊断）。
    /// @return 单调累计计数。
    [[nodiscard]] auto dropped_frames() const -> std::uint64_t;
    /// @brief 重采样质量档位（默认 Linear；Sinc 需约 16 帧前瞻，窗尾帧须续推后才输出）。
    /// @param q 目标档位。
    auto set_src_quality(SrcQuality q) -> void { src_quality_.store(static_cast<int>(q), std::memory_order_release); }
    /// @brief 当前重采样档位。
    /// @return src_quality_ 原子读回（块级生效）。
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
    /// @return gain_ 引用（直写与自动化共用）。
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
    /// @param buffer 只读共享缓冲；nullptr 或 valid() 为假时返回错误。
    /// @return Ok 或 audio-buffer-invalid。
    auto set_buffer(std::shared_ptr<const AudioBuffer> buffer) -> Result<void>;
    /// @brief 在时刻 when（context 采样时钟，秒）起播；when 已过则立即起播。
    /// @param when 起播时刻（秒，须 ≥ 0）。
    /// @return Ok 或 audio-param-invalid（when 为负）。
    auto start(double when = 0.0) -> Result<void>;
    /// @brief 停止播放（finished() 置真）。
    auto stop() -> void;
    /// @brief 循环播放开关（默认关）。
    /// @param loop true 播完回卷继续。
    auto set_loop(bool loop) -> void { loop_.store(loop, std::memory_order_release); }
    /// @brief 是否处于循环播放。
    /// @return loop_ 原子读回。
    [[nodiscard]] auto looping() const -> bool { return loop_.load(std::memory_order_acquire); }
    /// @brief 是否已播完/停止（播完后输出静音）。
    /// @return finished_ 原子读回。
    [[nodiscard]] auto finished() const -> bool { return finished_.load(std::memory_order_acquire); }

  protected:
    auto process(const AudioRenderContext &p) -> void override;

  private:
    friend class AudioContext;
    explicit AudioBufferSourceNode(AudioContext &ctx) : AudioNode(ctx, 2) {}

    std::mutex buffer_mutex_;  // 保护 buffer_（libc++/MSVC 无 atomic<shared_ptr>）
    std::shared_ptr<const AudioBuffer> buffer_;  // COW 指针（UI 写 / 渲染读）
    std::atomic<bool> loop_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    std::atomic<double> start_when_{0.0};
    double play_pos_ = 0.0;  // 渲染端播放相位（缓冲帧单位）
};

/// @brief 声像节点：equal-power 立体声声像（语义对齐 Web Audio PannerNode 的
///        `panningModel='equalpower'` 默认形态；HRTF 卷积模型属后续增量）。
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
    /// @param x 横坐标。
    /// @param y 纵坐标。
    /// @param z 深度坐标。
    auto set_position(float x, float y, float z) -> void;
    /// @brief 声源位置快照。
    /// @return {x, y, z}（原子读回，块级生效）。
    [[nodiscard]] auto position() const -> std::array<float, 3>;
    /// @brief 距离衰减参考距离（默认 1.0，须 > 0）。
    /// @param d 参考距离；非正值回退 1.0 存储。
    auto set_ref_distance(float d) -> void { ref_distance_.store(d > 0.0F ? d : 1.0F, std::memory_order_release); }
    /// @brief 当前参考距离。
    /// @return ref_distance_ 原子读回。
    [[nodiscard]] auto ref_distance() const -> float { return ref_distance_.load(std::memory_order_acquire); }
    /// @brief 距离衰减滚降系数（默认 1.0，须 ≥ 0）。
    /// @param r 滚降系数；负值钳为 0 存储。
    auto set_rolloff(float r) -> void { rolloff_.store(r < 0.0F ? 0.0F : r, std::memory_order_release); }
    /// @brief 当前滚降系数。
    /// @return rolloff_ 原子读回。
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
    /// @param n 目标 FFT 点数。
    /// @return Ok 或 audio-param-invalid。
    auto set_fft_size(int n) -> Result<void>;
    /// @brief 当前 FFT 点数。
    /// @return fft_size_ 原子读回（默认 2048）。
    [[nodiscard]] auto fft_size() const -> int { return fft_size_.load(std::memory_order_acquire); }
    /// @brief 频域桶数（= fft_size/2）。
    /// @return 桶数快照。
    [[nodiscard]] auto frequency_bin_count() const -> int { return fft_size() / 2; }
    /// @brief 频域平滑系数 τ ∈ [0,1]（默认 0.8；0 = 无平滑）。
    /// @param v 目标平滑系数（越接近 1 平滑越强）。
    auto set_smoothing_time_constant(float v) -> void;
    /// @brief 当前平滑系数。
    /// @return smoothing_ 原子读回。
    [[nodiscard]] auto smoothing_time_constant() const -> float { return smoothing_.load(std::memory_order_acquire); }
    /// @brief 频域 dB 钳位区间（默认 [-100, -30]；写入保持 min < max）。
    /// @param v 新的 dB 下限。
    auto set_min_decibels(float v) -> void { min_db_.store(v, std::memory_order_release); }
    /// @brief 当前 dB 下限。
    /// @return min_db_ 原子读回。
    [[nodiscard]] auto min_decibels() const -> float { return min_db_.load(std::memory_order_acquire); }
    /// @brief 设置 dB 上限（与 set_min_decibels 配对，保持 min < max）。
    /// @param v 新的 dB 上限。
    auto set_max_decibels(float v) -> void { max_db_.store(v, std::memory_order_release); }
    /// @brief 当前 dB 上限。
    /// @return max_db_ 原子读回。
    [[nodiscard]] auto max_decibels() const -> float { return max_db_.load(std::memory_order_acquire); }

    /// @brief 频域快照：各桶 dB 值（钳位到 [min_decibels, max_decibels]）。
    ///        out.size() 不足 frequency_bin_count() 时按可用长度填充。
    /// @param out 目标缓冲（逐桶写入）。
    auto get_float_frequency_data(std::span<float> out) const -> void;
    /// @brief 频域快照字节形：dB 归一映射到 0..255（min→0，max→255）。
    /// @param out 目标字节缓冲。
    auto get_byte_frequency_data(std::span<std::uint8_t> out) const -> void;
    /// @brief 时域快照字节形：最近 fft_size 个样本 -1..1 映射 0..255（静音 = 128）。
    /// @param out 目标字节缓冲。
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
/// 只能经 `AudioContext::create_microphone_source()` 创建——工厂启动采集失败时返回
/// 显式 `Result` 错误（audio-device-unavailable），**不静默降级**（录制是显式能力）。
/// 推入侧复用推流环（SPSC：唯一生产者 = 采集线程）。
///
/// @note Thread: 采集线程 push；渲染线程拉取
/// @note Side-effects: 持有采集后端线程生命周期（节点析构先停采集再释放环）
/// @note Rebuildable: no
class AudioMicrophoneSourceNode final : public AudioStreamSourceNode {
  public:
    /// @brief 析构：先停采集线程再释放基类推流环（避免悬垂回调）。
    ~AudioMicrophoneSourceNode() override;
    // 五法则（CODING_STANDARDS.md §5.1）：持采集后端 unique_ptr（先停采集再释放环），
    // 拷贝/移动会双重停机或丢所有权，故一律显式禁用；节点只经 shared_ptr 由 context 持有。
    /// @brief 拷贝构造禁用——独占采集后端所有权（声明见上方五法则注记）。
    AudioMicrophoneSourceNode(const AudioMicrophoneSourceNode &) = delete;
    /// @brief 拷贝赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(const AudioMicrophoneSourceNode &) -> AudioMicrophoneSourceNode & = delete;
    /// @brief 移动构造禁用——转移会双重停机或丢采集后端（声明见上方五法则注记）。
    AudioMicrophoneSourceNode(AudioMicrophoneSourceNode &&) = delete;
    /// @brief 移动赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(AudioMicrophoneSourceNode &&) -> AudioMicrophoneSourceNode & = delete;

  private:
    friend class AudioContext;
    explicit AudioMicrophoneSourceNode(AudioContext &ctx, std::unique_ptr<AudioCaptureBackend> backend);

    std::unique_ptr<AudioCaptureBackend> capture_;  // 先于基类环析构（派生成员先销毁）
};

/// @brief 录制汇节点（录制链输出端）：直通节点，录制途经样本为 PCM/WAV。
/// `start()` 起录、`stop()` 停录；样本常驻内存（交错 float32，设备采样率 stereo），
/// 可导出 WAV 字节或落盘（落盘失败返回显式 `Result` 错误，不静默）。
///
/// @note Thread: start/stop/get* 为 UI 线程（mutex）；process 在渲染线程
/// @note Side-effects: save_wav 落盘 I/O
/// @note Rebuildable: no
class AudioRecordingDestinationNode final : public AudioNode {
  public:
    /// @brief 开始录制（幂等；closed 上下文返回 audio-context-closed）。
    /// @return Ok 或 audio-context-closed。
    auto start() -> Result<void>;
    /// @brief 停止录制（幂等；保留已录样本供导出）。
    auto stop() -> void;
    /// @brief 是否正在录制。
    /// @return recording_ 原子读回。
    [[nodiscard]] auto is_recording() const -> bool { return recording_.load(std::memory_order_acquire); }
    /// @brief 已录样本快照（交错 float32 stereo；线程安全拷贝）。
    /// @return 当前录制缓冲的副本。
    [[nodiscard]] auto recording() const -> std::vector<float>;
    /// @brief 已录帧数（stereo 帧口径）。
    /// @return 样本数 / 2。
    [[nodiscard]] auto recorded_frames() const -> std::size_t;
    /// @brief 导出 WAV 字节（16-bit PCM stereo，RIFF 头；样本按 context 采样率）。
    /// @return 完整 WAV 文件字节流。
    [[nodiscard]] auto to_wav_bytes() const -> std::vector<std::uint8_t>;
    /// @brief 落盘 WAV（目录不存在/无权限等失败返回 audio-recording-failed，不静默）。
    /// @param path 目标文件路径。
    /// @return Ok 或 audio-recording-failed。
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
/// - 拓扑变更（connect/disconnect）经 UI 线程校验（禁环 DAG + 归属 + 重复边）后提交
///   命令队列，渲染线程在块首无锁消费；无设备运行时（静默/测试）直接在调用线程生效。
/// - suspend 冻结采样时钟并输出静音；close 终止（后续操作返回 audio-context-closed）。
/// - 无内置后端编译（AURORA_ENABLE_AUDIO 关）或设备初始化失败 → 静默模式：
///   图照常运转、样本消费后丢弃，device_state() 报 Silent。
/// - 浏览器（Web Audio 后端）另有一道**自动播放闸门**：`device_state()` 报 Active 只
///   表示设备存在且在收样，**不等于已出声**——上下文在用户首次交互前保持 suspended，
///   后端每秒限速重试解锁，一次点击/按键即开声（`WebAudioDeviceBackend::context_state()`
///   为该闸门的只读观测口，库内部）。
///
/// @note Thread: 图变更/参数为 UI 线程；render_block 为单渲染线程（设备线程或宿主手动驱动）
/// @note Side-effects: 设备后端线程生命周期；静默模式零系统副作用
/// @note Rebuildable: no
class AudioContext {
  public:
    /// @brief 构造：注入设备后端（测试桩）；nullptr 时按编译期后端宏创建默认设备
    ///        （未编译内置后端 → 恒静默模式）。可另注入采集后端（麦克风测试桩；
    ///        nullptr 时 create_microphone_source 按需创建默认采集后端，不可用则显式报错）。
    /// @param device_backend 设备后端（nullptr = 用编译期默认/静默）。
    /// @param capture_backend 采集后端（nullptr = 按需创建默认）。
    explicit AudioContext(std::unique_ptr<AudioDeviceBackend> device_backend = nullptr,
                          std::unique_ptr<AudioCaptureBackend> capture_backend = nullptr);
    /// @brief 析构：等价 close 语义（停设备线程、终止图），再释放后端与节点。
    ~AudioContext();
    // 五法则（CODING_STANDARDS.md §5.1）：context 是节点/拓扑/设备线程的唯一属主（持两个后端
    // unique_ptr 与原子时钟），移动会令全部节点的 ctx_ 引用悬垂，故拷贝与移动皆显式禁用。
    /// @brief 拷贝构造禁用——唯一属主不可复制（声明见上方五法则注记）。
    AudioContext(const AudioContext &) = delete;
    /// @brief 拷贝赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(const AudioContext &) -> AudioContext & = delete;
    /// @brief 移动构造禁用——转移会令全部节点 ctx_ 引用悬垂（声明见上方五法则注记）。
    AudioContext(AudioContext &&) = delete;
    /// @brief 移动赋值禁用——同上。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(AudioContext &&) -> AudioContext & = delete;

    // ---- 拓扑 ----
    /// @brief 连接 src → dst（校验归属/重复/禁环；DAG）。
    /// @param src 上游节点（须属于本 context）。
    /// @param dst 下游节点。
    /// @return Ok 或显式错误（非本 context 节点 / 重复边 / 成环 / closed）。
    auto connect(const std::shared_ptr<AudioNode> &src, const std::shared_ptr<AudioNode> &dst) -> Result<void>;
    /// @brief 断开 src → dst（边不存在返回 audio-edge-not-found）。
    /// @param src 上游节点。
    /// @param dst 下游节点。
    /// @return Ok 或显式错误（边不存在 / closed 等）。
    auto disconnect(const std::shared_ptr<AudioNode> &src, const std::shared_ptr<AudioNode> &dst) -> Result<void>;
    /// @brief 当前边数（UI 视角）。
    /// @return edges_ 表大小。
    [[nodiscard]] auto connection_count() const -> std::size_t { return edges_.size(); }

    /// @brief 创建出口节点（多入汇聚混音）。
    /// @return 节点共享指针（生命周期归 context，至 close/析构）。
    [[nodiscard]] auto create_destination() -> std::shared_ptr<AudioDestinationNode>;
    /// @brief 创建推流源节点（SPSC 无锁环，App 侧 push PCM）。
    /// @param ring_capacity_frames 环容量（源采样率帧，默认 16384）。
    /// @return 节点共享指针。
    [[nodiscard]] auto create_stream_source(std::size_t ring_capacity_frames = 16384)
        -> std::shared_ptr<AudioStreamSourceNode>;
    /// @brief 创建增益节点（gain 为 AudioParam，支持自动化）。
    /// @return 节点共享指针。
    [[nodiscard]] auto create_gain() -> std::shared_ptr<GainNode>;
    /// @brief 创建缓冲源节点（一次性/循环音效）。
    /// @return 节点共享指针。
    [[nodiscard]] auto create_buffer_source() -> std::shared_ptr<AudioBufferSourceNode>;
    /// @brief 创建声像节点（equal-power，相对 listener 计算）。
    /// @return 节点共享指针。
    [[nodiscard]] auto create_panner() -> std::shared_ptr<PannerNode>;
    /// @brief 创建频谱分析节点（直通 + 时域/频域快照）。
    /// @return 节点共享指针。
    [[nodiscard]] auto create_analyser() -> std::shared_ptr<AnalyserNode>;
    /// @brief 创建录制汇节点（直通，可录途经样本并导出 WAV）。
    /// @return 节点共享指针。
    [[nodiscard]] auto create_recording_destination() -> std::shared_ptr<AudioRecordingDestinationNode>;
    /// @brief 麦克风源（**录制是显式能力**）：采集设备不可用/权限拒绝时返回
    ///        audio-device-unavailable 显式错误，不静默降级。
    /// @return 节点共享指针或显式错误。
    auto create_microphone_source() -> Result<std::shared_ptr<AudioMicrophoneSourceNode>>;

    /// @brief 出口节点（构造时创建）。
    /// @return 唯一 destination 的共享指针常引用。
    [[nodiscard]] auto destination() const -> const std::shared_ptr<AudioDestinationNode> & { return destination_; }
    /// @brief 听者（PannerNode 世界坐标系；默认位于原点、朝 -Z、上向 +Y）。
    /// @return context 内嵌 listener 的引用（与 context 同寿命）。
    [[nodiscard]] auto listener() -> AudioListener & { return listener_; }

    /// @brief 冻结采样时钟（幂等；后续 render_block 输出静音，时钟停走）。
    /// @return Ok 或 audio-context-closed。
    auto suspend() -> Result<void>;
    /// @brief 恢复采样时钟（suspend 的逆操作）。
    /// @return Ok 或 audio-context-closed。
    auto resume() -> Result<void>;
    /// @brief 关闭（终止设备与图；终态，不可逆）。
    ///        返回前设备线程已退出并停止设备后端；后端对象保留至上下文析构时释放，
    ///        期间任何节点/连接操作均返回 audio-context-closed。
    /// @return Ok 或 audio-context-closed（已关闭时幂等报错）。
    auto close() -> Result<void>;
    /// @brief 是否已关闭（终态）。
    /// @return closed_ 原子读回。
    [[nodiscard]] auto closed() const -> bool { return closed_.load(std::memory_order_acquire); }
    /// @brief 是否被 suspend 冻结。
    /// @return suspended_ 原子读回。
    [[nodiscard]] auto suspended() const -> bool { return suspended_.load(std::memory_order_acquire); }

    /// @brief 设备通道状态。
    /// @return Active = 设备在拉取；Silent = 无设备/初始化失败/close 后终态。
    [[nodiscard]] auto device_state() const -> AudioDeviceState;
    /// @brief 静默模式（无设备/初始化失败）：图照常运转，输出被丢弃。
    /// @return device_state() == Silent。
    [[nodiscard]] auto silent() const -> bool { return device_state() == AudioDeviceState::Silent; }
    /// @brief 设备采样率（静默模式为处理格式 48000）。
    /// @return format_.sample_rate。
    [[nodiscard]] auto sample_rate() const -> int { return format_.sample_rate; }
    /// @brief 设备声道数（首切片恒 2）。
    /// @return format_.channels。
    [[nodiscard]] auto channel_count() const -> int { return format_.channels; }
    /// @brief 采样时钟（秒；suspend 冻结，close 后停走）。
    /// @return rendered_samples_ / 采样率。
    [[nodiscard]] auto current_time() const -> double;

    /// @brief 主音量（render_block 输出端统一施加，默认 1.0）。
    /// @param v 线性因子（调用侧约定 [0,1]）。
    auto set_master_volume(float v) -> void { master_volume_.store(v, std::memory_order_release); }
    /// @brief 当前主音量。
    /// @return master_volume_ 原子读回。
    [[nodiscard]] auto master_volume() const -> float { return master_volume_.load(std::memory_order_acquire); }

    /// @brief 渲染一块（设备线程由后端回调驱动；静默/测试模式可宿主手动驱动推进时钟）。
    /// @param interleaved_out 交错输出缓冲，容量须 ≥ frames * channel_count()
    /// @param frames 本块帧数（> 0）
    /// @note Thread: 单渲染线程；与 UI 线程经命令队列/SPSC 环解耦
    auto render_block(float *interleaved_out, int frames) -> void;

  private:
    friend class AudioNode;

    /// @brief 图变更命令（UI 侧入队、渲染线程块首消费；SPSC 环按值承载）。
    struct GraphCommand {
        // 私有枚举且作为命令结构体字段存储（命令环按值承载）：值域仅 0..1，收窄无损语义。
        /// @brief 命令种类（加边 / 去边）。
        enum class Kind : std::uint8_t { Connect, Disconnect };
        Kind kind{};  ///< 命令种类
        std::shared_ptr<AudioNode> src;  ///< 边源节点（借用引用，生命周期由 context 锚定）
        std::shared_ptr<AudioNode> dst;  ///< 边目标节点
    };

    /// @brief UI 侧状态（仅 UI 线程触达）。
    std::vector<std::shared_ptr<AudioNode>> nodes_;  ///< 全部节点（生命周期锚）
    std::vector<std::pair<AudioNode *, AudioNode *>> edges_;  ///< UI 视角边表（校验/计数）
    /// @brief 唯一出口节点（构造时创建，destination() 的返回目标）。
    std::shared_ptr<AudioDestinationNode> destination_;
    AudioListener listener_;  ///< PannerNode 世界坐标系听者

    /// @brief 渲染侧邻接表条目（仅渲染线程触达；direct_graph_ 时归调用线程）。
    struct Adjacency {
        std::vector<AudioNode *> sources;  ///< 上游节点（裸指针，context 锚寿命）
        std::vector<AudioNode *> consumers;  ///< 下游节点
    };
    std::unordered_map<AudioNode *, Adjacency> adj_;  ///< 渲染侧邻接表
    std::vector<AudioNode *> topo_;  ///< 拓扑序（源先于汇；不含 destination）

    /// @brief 命令队列（UI → 渲染，SPSC 定长环；direct_graph_ 时旁路）。
    static constexpr std::size_t AURORA_COMMAND_RING_CAPACITY = 256;  ///< 环容量（命令数，2 的幂）
    std::vector<GraphCommand> command_ring_;  ///< 定长命令环槽
    std::atomic<std::uint64_t> cmd_write_{0};  ///< 生产端游标（UI 写，单调）
    std::atomic<std::uint64_t> cmd_read_{0};  ///< 消费端游标（渲染读，单调）

    std::unique_ptr<AudioDeviceBackend> device_;  ///< 设备后端（nullptr = 静默模式）
    std::unique_ptr<AudioCaptureBackend> capture_backend_;  ///< 注入的采集后端（create_microphone_source 惰性消费）
    AudioDeviceFormat format_{};  ///< 输出格式快照（构造期定，静默为 48000/2）
    bool direct_graph_ = false;  ///< 无运行中设备：图变更直接生效（无渲染线程竞态）
    std::atomic<bool> suspended_{false};  ///< suspend 闸门（冻结时钟输出静音）
    std::atomic<bool> closed_{false};  ///< close 终态标记
    std::atomic<float> master_volume_{1.0F};  ///< 主音量（输出端统一施加）
    std::atomic<std::uint64_t> rendered_samples_{0};  ///< 采样时钟（帧）

    /// @brief 节点是否属于本 context（工厂登记核对）。
    /// @param n 待验节点裸指针。
    /// @return nodes_ 中登记即为 true。
    auto owns_node(const AudioNode *n) const -> bool;
    /// @brief DFS 判环：从 dst 出发是否可达 src（可达则连边成环）。
    /// @param src 拟议边源。
    /// @param dst 拟议边目标。
    /// @return 连接会构成环时为 true。
    auto would_cycle(const AudioNode *src, AudioNode *dst) const -> bool;  // DFS：src 自 dst 可达？
    /// @brief 渲染侧应用加边（邻接表 + 拓扑重建）。
    /// @param src 边源。
    /// @param dst 边目标。
    auto apply_connect(AudioNode *src, AudioNode *dst) -> void;
    /// @brief 渲染侧应用去边（邻接表 + 拓扑重建）。
    /// @param src 边源。
    /// @param dst 边目标。
    auto apply_disconnect(AudioNode *src, AudioNode *dst) -> void;
    /// @brief 重建拓扑序（Kahn 算法，源先于汇）。
    auto rebuild_topo() -> void;  // Kahn（源先于汇）
    /// @brief 渲染线程块首消费命令环（direct_graph_ 时恒空转）。
    auto drain_commands() -> void;  // 渲染线程块首消费
    /// @brief 输出静音填充（suspend/closed 时替代图渲染）。
    /// @param out 交错输出缓冲。
    /// @param frames 本块帧数。
    auto render_silence(float *out, int frames) const -> void;
};

}  // namespace aurora
