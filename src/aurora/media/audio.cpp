// ============================================================
// media/audio.cpp — 音频图实现（平台无关）
// ------------------------------------------------------------
// 契约见 include/aurora/media/audio.h 与 specification/03-layout-render.md §9。
// 关键实现决策：
//  - AudioParam 事件链为 COW 快照（atomic<shared_ptr>）：UI 写 / 渲染读均无阻塞。
//  - 推流环为 SPSC 单调帧游标；溢出「丢弃最旧」由生产端 CAS 单调推进读端完成，
//    消费进度永不回退；渲染端检测落后即重同步到消费头（溢出毛刺有界）。
//  - 图变更走 SPSC 命令环（UI → 渲染，块首消费）；无运行中设备（静默/测试）时
//    直接在调用线程生效（无渲染线程竞态，命令环旁路）。
//  - 拓扑禁环在 UI 侧校验（DFS 可达性）；渲染侧 Kahn 重建拓扑序（源先于汇）。
// ============================================================

#include "aurora/media/audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <ranges>
#include <utility>

#include "audio_alsa.h"
#include "audio_wasapi.h"
#include "aurora/core/log.h"

namespace aurora {

namespace {

constexpr int AURORA_AUDIO_RENDER_CHANNELS = 2;  // 图内固定 stereo
constexpr double AURORA_AUDIO_PI = 3.14159265358979323846;
constexpr int AURORA_AUDIO_SINC_TAPS = 32;  // Sinc 核抽头数
constexpr int AURORA_AUDIO_SINC_PHASES = 256;  // Sinc 相位量化子相位数
// 抽头中心偏移与窗半宽：先在整型/浮点各自上下文里算好，避免整型除法落在浮点上下文（bugprone-integer-division）
constexpr int AURORA_AUDIO_SINA_CENTER = AURORA_AUDIO_SINC_TAPS / 2 - 1;  // tap k ∈ [-15, 16] 的 -15 端
constexpr double AURORA_AUDIO_SINC_HALF_WIDTH = static_cast<double>(AURORA_AUDIO_SINC_TAPS) / 2.0;  // Blackman 窗半宽

auto make_param_error(const std::string &reason) -> Error {
    return make_error(ErrorCode::AudioParamInvalid, ErrorParams{{"reason", reason}});
}

}  // namespace

// ============================================================
// AudioParam
// ============================================================

auto AudioParam::validate_schedule_time(double t) const -> Result<void> {
    if (t < 0.0) {
        return make_param_error("time must be non-negative");
    }
    const auto events = events_snapshot();
    if (events && !events->empty() && t < events->back().time) {
        return make_param_error("event times must be non-decreasing");
    }
    return Result<void>{};
}

auto AudioParam::chain_value_at(const EventList &events, double t) const -> float {
    if (events.empty()) {
        return value_.load(std::memory_order_acquire);
    }
    return evaluate_at(events, t);
}

// 纯函数求值：cur = 末个 time<=t 的事件；若下一事件是 Ramp 则处于插值进行中。
auto AudioParam::evaluate_at(const EventList &events, double t) const -> float {
    const Event *cur = nullptr;
    const Event *next = nullptr;
    for (const auto &e : events) {
        if (e.time <= t) {
            cur = &e;
        } else {
            next = &e;
            break;
        }
    }
    if (next != nullptr && (next->kind == EventKind::LinearRamp || next->kind == EventKind::ExponentialRamp)) {
        const double span = next->time - next->start_time;
        if (span <= 0.0) {
            return next->value;
        }
        const double x = std::clamp((t - next->start_time) / span, 0.0, 1.0);
        if (next->kind == EventKind::LinearRamp) {
            return next->start_value + (next->value - next->start_value) * static_cast<float>(x);
        }
        // 指数 ramp：起点非零由插入期校验保证
        const float base = next->start_value;
        return base * static_cast<float>(std::pow(static_cast<double>(next->value) / static_cast<double>(base), x));
    }
    if (cur != nullptr) {
        if (cur->kind == EventKind::SetTarget) {
            const double dt = t - cur->time;
            const double tc = cur->time_constant > 0.0 ? cur->time_constant : 1e-6;
            return cur->value + (cur->start_value - cur->value) * static_cast<float>(std::exp(-dt / tc));
        }
        // Set / 已到位的 Ramp
        return cur->value;
    }
    // t 早于全部事件
    return value_.load(std::memory_order_acquire);
}

auto AudioParam::set_value_at_time(float v, double t) -> Result<void> {
    if (const auto r = validate_schedule_time(t); !r.ok()) {
        return r;
    }
    auto events = std::make_shared<EventList>(*events_snapshot());
    events->push_back(Event{
        .kind = EventKind::Set, .time = t, .value = v, .time_constant = 0.0, .start_time = 0.0, .start_value = 0.0F});
    set_events(std::move(events));
    return Result<void>{};
}

// Ramp 插桩：锚点 = 上一事件的时刻与落点值（首事件锚 0 时刻的当前值）；
// 指数 ramp 起终点非零且同号（否则 audio-param-invalid）。
auto AudioParam::ramp_impl(EventKind kind, float v, double t) -> Result<void> {
    if (const auto r = validate_schedule_time(t); !r.ok()) {
        return r;
    }
    auto events = std::make_shared<EventList>(*events_snapshot());
    const double start_time = events->empty() ? 0.0 : events->back().time;
    const float start_value = chain_value_at(*events, start_time);
    if (kind == EventKind::ExponentialRamp &&
        (start_value == 0.0F || v == 0.0F || (start_value > 0.0F) != (v > 0.0F))) {
        return make_param_error("exponential ramp must not cross zero (start and end non-zero, same sign)");
    }
    events->push_back(Event{.kind = kind,
                            .time = t,
                            .value = v,
                            .time_constant = 0.0,
                            .start_time = start_time,
                            .start_value = start_value});
    set_events(std::move(events));
    return Result<void>{};
}

auto AudioParam::linear_ramp_to_value_at_time(float v, double t) -> Result<void> {
    return ramp_impl(EventKind::LinearRamp, v, t);
}

auto AudioParam::exponential_ramp_to_value_at_time(float v, double t) -> Result<void> {
    return ramp_impl(EventKind::ExponentialRamp, v, t);
}

auto AudioParam::set_target_at_time(float target, double t, double time_constant) -> Result<void> {
    if (const auto r = validate_schedule_time(t); !r.ok()) {
        return r;
    }
    if (!(time_constant > 0.0)) {
        return make_param_error("time_constant must be positive");
    }
    auto events = std::make_shared<EventList>(*events_snapshot());
    const float start_value = chain_value_at(*events, t);
    events->push_back(Event{.kind = EventKind::SetTarget,
                            .time = t,
                            .value = target,
                            .time_constant = time_constant,
                            .start_time = 0.0,
                            .start_value = start_value});
    set_events(std::move(events));
    return Result<void>{};
}

auto AudioParam::cancel_scheduled_values() -> void {
    set_events(std::make_shared<const EventList>());
}

auto AudioParam::has_automation() const -> bool {
    const auto events = events_snapshot();
    return events != nullptr && !events->empty();
}

auto AudioParam::evaluate_block(double t0, int frames, int sample_rate, float *out) -> void {
    const auto events = events_snapshot();
    if (events == nullptr || events->empty()) {
        const float v = value_.load(std::memory_order_acquire);
        std::fill_n(out, frames, v);
        return;
    }
    const double dt = 1.0 / static_cast<double>(sample_rate);
    for (int i = 0; i < frames; ++i) {
        out[i] = evaluate_at(*events, t0 + static_cast<double>(i) * dt);
    }
    value_.store(out[frames - 1], std::memory_order_release);
}

// ============================================================
// AudioNode
// ============================================================

auto AudioNode::ensure_capacity(int frames) -> void {
    const std::size_t need = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
    if (in_bus_.size() < need) {
        in_bus_.resize(need);
    }
    if (out_bus_.size() < need) {
        out_bus_.resize(need);
    }
}

auto AudioDestinationNode::process(const AudioRenderContext &p) -> void {
    const std::size_t total = static_cast<std::size_t>(p.frames) * static_cast<std::size_t>(channels_);
    std::copy_n(in_bus_.data(), total, out_bus_.data());
}

// ============================================================
// AudioStreamSourceNode — SPSC 环 + 线性 SRC
// ============================================================

AudioStreamSourceNode::AudioStreamSourceNode(AudioContext &ctx, std::size_t ring_capacity_frames)
    : AudioNode(ctx, AURORA_AUDIO_RENDER_CHANNELS), capacity_(ring_capacity_frames < 2 ? 2 : ring_capacity_frames) {
    ring_.assign(capacity_ * static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS), 0.0F);
    // Sinc 内核相位量化表：256 子相位 × 32 tap，Blackman 窗 sinc，行归一（DC 增益 1）。
    // tap k ∈ [-15, 16]，权重 h(frac - k)（frac = 子相位）；仅构造期写入，其后只读。
    sinc_table_.assign(
        static_cast<std::size_t>(AURORA_AUDIO_SINC_PHASES) * static_cast<std::size_t>(AURORA_AUDIO_SINC_TAPS), 0.0F);
    for (int s = 0; s < AURORA_AUDIO_SINC_PHASES; ++s) {
        const double frac = static_cast<double>(s) / static_cast<double>(AURORA_AUDIO_SINC_PHASES);
        double sum = 0.0;
        for (int t = 0; t < AURORA_AUDIO_SINC_TAPS; ++t) {
            const double d = frac - static_cast<double>(t - AURORA_AUDIO_SINA_CENTER);
            const double x = d / AURORA_AUDIO_SINC_HALF_WIDTH;
            const double win = 0.42 + 0.5 * std::cos(AURORA_AUDIO_PI * x) + 0.08 * std::cos(2.0 * AURORA_AUDIO_PI * x);
            const double kern = (std::abs(d) < 1e-9) ? 1.0 : std::sin(AURORA_AUDIO_PI * d) / (AURORA_AUDIO_PI * d);
            sinc_table_[static_cast<std::size_t>(s) * static_cast<std::size_t>(AURORA_AUDIO_SINC_TAPS) +
                        static_cast<std::size_t>(t)] = static_cast<float>(kern * win);
            sum += kern * win;
        }
        if (sum > 1e-12) {
            for (int t = 0; t < AURORA_AUDIO_SINC_TAPS; ++t) {
                sinc_table_[static_cast<std::size_t>(s) * static_cast<std::size_t>(AURORA_AUDIO_SINC_TAPS) +
                            static_cast<std::size_t>(t)] /= static_cast<float>(sum);
            }
        }
    }
}

auto AudioStreamSourceNode::push(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> Result<void> {
    if (sample_rate <= 0 || (channels != 1 && channels != 2) || pcm.size() % static_cast<std::size_t>(channels) != 0) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          std::string("invalid PCM packet (sample_rate/channels/frame alignment)"));
    }
    if (src_rate_ == 0) {
        src_rate_ = sample_rate;
    } else if (src_rate_ != sample_rate) {
        return make_error(ErrorCode::GeneralInvalidArgument, std::string("stream sample rate changed mid-stream"));
    }
    const std::size_t in_frames = pcm.size() / static_cast<std::size_t>(channels);
    if (in_frames == 0) {
        return Result<void>{};
    }
    // int16 → float32（Q15 归一）+ 声道映射到图 stereo：mono 复制 / stereo 直写
    conv_.clear();
    conv_.reserve(in_frames * static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS));
    constexpr float scale = 1.0F / 32768.0F;
    for (std::size_t f = 0; f < in_frames; ++f) {
        const float l = static_cast<float>(pcm[f * static_cast<std::size_t>(channels)]) * scale;
        const float r = channels == 2 ? static_cast<float>(pcm[f * 2 + 1]) * scale : l;
        conv_.push_back(l);
        conv_.push_back(r);
    }
    std::size_t frames = in_frames;
    const float *data = conv_.data();
    if (frames > capacity_) {  // 单包超容量：保留最新 capacity_ 帧（对齐丢弃最旧语义）
        const std::size_t skip = frames - capacity_;
        data += skip * static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS);
        frames = capacity_;
        dropped_.fetch_add(skip, std::memory_order_relaxed);
    }
    // 溢出：丢弃最旧（CAS 单调推进读端；消费进度不回退）
    std::uint64_t w = write_.load(std::memory_order_relaxed);
    const std::uint64_t r = read_.load(std::memory_order_acquire);
    if (w - r + frames > capacity_) {
        const std::uint64_t drop = w - r + frames - capacity_;
        std::uint64_t cur = r;
        while (!read_.compare_exchange_weak(cur, cur + drop, std::memory_order_release, std::memory_order_acquire)) {
        }
        dropped_.fetch_add(drop, std::memory_order_relaxed);
        if (!overflow_warned_.exchange(true)) {
            AURORA_LOG_WARN("audio", "audio stream ring overflow; dropped " + std::to_string(drop) +
                                         " oldest frames (capacity " + std::to_string(capacity_) + ")");
        }
    }
    w = write_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t slot = ((w + i) % capacity_) * static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS);
        ring_[slot] = data[i * 2];
        ring_[slot + 1] = data[i * 2 + 1];
    }
    write_.store(w + frames, std::memory_order_release);
    return Result<void>{};
}

auto AudioStreamSourceNode::clear() -> void {
    // 约束：调用方保证无并发渲染（如暂停态 seek）。重置游标与渲染相位。
    read_.store(0, std::memory_order_release);
    write_.store(0, std::memory_order_release);
    pull_active_ = false;
    src_pos_ = 0.0;
}

auto AudioStreamSourceNode::buffered_frames() const -> std::size_t {
    return static_cast<std::size_t>(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
}

auto AudioStreamSourceNode::dropped_frames() const -> std::uint64_t { return dropped_.load(std::memory_order_relaxed); }

auto AudioStreamSourceNode::process(const AudioRenderContext &p) -> void {
    const std::uint64_t w = write_.load(std::memory_order_acquire);
    const std::uint64_t r = read_.load(std::memory_order_acquire);
    if (!pull_active_ || src_pos_ < static_cast<double>(r)) {
        // 首块对齐 / 溢出丢最旧后重同步到消费头（毛刺有界，不回放已丢内容）
        src_pos_ = static_cast<double>(r);
        pull_active_ = true;
    }
    const double step = (src_rate_ > 0 && src_rate_ != p.sample_rate)
                            ? static_cast<double>(src_rate_) / static_cast<double>(p.sample_rate)
                            : 1.0;
    const auto end = static_cast<double>(w);
    const bool sinc = src_quality_.load(std::memory_order_acquire) == static_cast<int>(SrcQuality::Sinc);
    constexpr int kHalf = AURORA_AUDIO_SINC_TAPS / 2;  // 前瞻 half 帧、回看 half-1 帧
    for (int i = 0; i < p.frames; ++i) {
        const double fp = std::floor(src_pos_);
        const auto frac = static_cast<float>(src_pos_ - fp);
        if (sinc) {
            if (fp + static_cast<double>(kHalf) >= end) {
                break;  // 前瞻不足：停相位（窗尾帧须续推后才输出）
            }
            const int sub = std::min(AURORA_AUDIO_SINC_PHASES - 1,
                                     static_cast<int>(frac * static_cast<float>(AURORA_AUDIO_SINC_PHASES)));
            const float *row =
                sinc_table_.data() + static_cast<std::size_t>(sub) * static_cast<std::size_t>(AURORA_AUDIO_SINC_TAPS);
            const std::int64_t base = static_cast<std::int64_t>(fp) - (kHalf - 1);
            float acc_l = 0.0F;
            float acc_r = 0.0F;
            for (int t = 0; t < AURORA_AUDIO_SINC_TAPS; ++t) {
                std::int64_t idx = base + static_cast<std::int64_t>(t);
                if (idx < 0) {
                    idx = 0;  // 流起点无历史：复制首帧（行归一保证 DC 保真）
                }
                const std::size_t s = (static_cast<std::size_t>(idx) % capacity_) *
                                      static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS);
                acc_l += ring_[s] * row[t];
                acc_r += ring_[s + 1] * row[t];
            }
            out_bus_[static_cast<std::size_t>(i) * 2] = acc_l;
            out_bus_[static_cast<std::size_t>(i) * 2 + 1] = acc_r;
            src_pos_ += step;
            continue;
        }
        if (fp >= end) {
            break;  // 欠载：输出静音并停在环头（不跳相位）
        }
        if (frac != 0.0F && fp + 1.0 >= end) {
            break;  // 非对齐相位需要下一帧作插值锚，数据不足则停相位
        }
        const std::size_t i0 = static_cast<std::size_t>(fp) % capacity_;
        const std::size_t i1 = (static_cast<std::size_t>(fp) + 1U) % capacity_;
        const std::size_t s0 = i0 * static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS);
        const std::size_t s1 = i1 * static_cast<std::size_t>(AURORA_AUDIO_RENDER_CHANNELS);
        out_bus_[static_cast<std::size_t>(i) * 2] = ring_[s0] + (ring_[s1] - ring_[s0]) * frac;
        out_bus_[static_cast<std::size_t>(i) * 2 + 1] = ring_[s0 + 1] + (ring_[s1 + 1] - ring_[s0 + 1]) * frac;
        src_pos_ += step;
    }
    // 读端推进：线性模式保留插值锚 1 帧；Sinc 模式保留窗历史 half-1 帧（单调不回退）
    const std::int64_t keep = sinc ? static_cast<std::int64_t>(std::floor(src_pos_)) - (kHalf - 1)
                                   : static_cast<std::int64_t>(std::floor(src_pos_));
    std::uint64_t new_read = keep > 0 ? static_cast<std::uint64_t>(keep) : 0U;
    new_read = std::min<std::uint64_t>(new_read, w);
    if (new_read > r) {
        read_.store(new_read, std::memory_order_release);
    }
}

// ============================================================
// GainNode
// ============================================================

auto GainNode::process(const AudioRenderContext &p) -> void {
    const std::size_t total = static_cast<std::size_t>(p.frames) * static_cast<std::size_t>(channels_);
    if (!gain_.has_automation()) {
        const float v = gain_.value();
        for (std::size_t i = 0; i < total; ++i) {
            out_bus_[i] = in_bus_[i] * v;
        }
        return;
    }
    if (scratch_.size() < static_cast<std::size_t>(p.frames)) {
        scratch_.resize(static_cast<std::size_t>(p.frames));
    }
    gain_.evaluate_block(p.time, p.frames, p.sample_rate, scratch_.data());
    for (int f = 0; f < p.frames; ++f) {
        const float v = scratch_[static_cast<std::size_t>(f)];
        out_bus_[static_cast<std::size_t>(f) * 2] = in_bus_[static_cast<std::size_t>(f) * 2] * v;
        out_bus_[static_cast<std::size_t>(f) * 2 + 1] = in_bus_[static_cast<std::size_t>(f) * 2 + 1] * v;
    }
}

// ============================================================
// AudioBufferSourceNode
// ============================================================

auto AudioBufferSourceNode::set_buffer(std::shared_ptr<const AudioBuffer> buffer) -> Result<void> {
    if (buffer == nullptr || !buffer->valid()) {
        return make_error(
            ErrorCode::AudioBufferInvalid,
            ErrorParams{{"reason", buffer == nullptr ? "null buffer" : "invalid sample_rate/channels/samples"}});
    }
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_ = std::move(buffer);
    return Result<void>{};
}

auto AudioBufferSourceNode::start(double when) -> Result<void> {
    if (when < 0.0) {
        return make_param_error("start time must be non-negative");
    }
    finished_.store(false, std::memory_order_release);
    play_pos_ = 0.0;
    start_when_.store(when, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    return Result<void>{};
}

auto AudioBufferSourceNode::stop() -> void {
    started_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
}

auto AudioBufferSourceNode::process(const AudioRenderContext &p) -> void {
    std::shared_ptr<const AudioBuffer> buf;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buf = buffer_;
    }
    if (buf == nullptr || !buf->valid() || !started_.load(std::memory_order_acquire) ||
        finished_.load(std::memory_order_acquire)) {
        return;  // out_bus_ 已由渲染循环清零
    }
    if (p.time < start_when_.load(std::memory_order_acquire)) {
        return;  // 未到起播时刻（起播精度为块级）
    }
    const std::size_t buf_frames = buf->frame_count();
    const auto ch = static_cast<std::size_t>(buf->channels);
    const bool loop = loop_.load(std::memory_order_acquire);
    const double step = buf->sample_rate != p.sample_rate
                            ? static_cast<double>(buf->sample_rate) / static_cast<double>(p.sample_rate)
                            : 1.0;
    for (int i = 0; i < p.frames; ++i) {
        if (play_pos_ >= static_cast<double>(buf_frames)) {
            if (!loop) {
                finished_.store(true, std::memory_order_release);
                break;
            }
            play_pos_ = std::fmod(play_pos_, static_cast<double>(buf_frames));
        }
        const auto i0 = static_cast<std::size_t>(play_pos_);
        std::size_t i1 = i0 + 1U;
        if (loop) {
            i1 %= buf_frames;
        } else if (i1 >= buf_frames) {
            i1 = buf_frames - 1U;  // 尾帧插值钳制（下一帧即 finished）
        }
        const double frac_d = play_pos_ - static_cast<double>(i0);
        const auto frac = static_cast<float>(frac_d);
        const float l = buf->samples[i0 * ch] + (buf->samples[i1 * ch] - buf->samples[i0 * ch]) * frac;
        const float r =
            ch == 2 ? buf->samples[i0 * 2 + 1] + (buf->samples[i1 * 2 + 1] - buf->samples[i0 * 2 + 1]) * frac : l;
        out_bus_[static_cast<std::size_t>(i) * 2] = l;
        out_bus_[static_cast<std::size_t>(i) * 2 + 1] = r;
        play_pos_ += step;
    }
    if (!loop && play_pos_ >= static_cast<double>(buf_frames)) {
        finished_.store(true, std::memory_order_release);
    }
}

// ============================================================
// AudioListener
// ============================================================

auto AudioListener::set_position(float x, float y, float z) -> void {
    px_.store(x, std::memory_order_release);
    py_.store(y, std::memory_order_release);
    pz_.store(z, std::memory_order_release);
}

auto AudioListener::set_orientation(float forward_x, float forward_y, float forward_z, float up_x, float up_y,
                                    float up_z) -> void {
    fx_.store(forward_x, std::memory_order_release);
    fy_.store(forward_y, std::memory_order_release);
    fz_.store(forward_z, std::memory_order_release);
    ux_.store(up_x, std::memory_order_release);
    uy_.store(up_y, std::memory_order_release);
    uz_.store(up_z, std::memory_order_release);
}

auto AudioListener::position() const -> std::array<float, 3> {
    return {px_.load(std::memory_order_acquire), py_.load(std::memory_order_acquire),
            pz_.load(std::memory_order_acquire)};
}

auto AudioListener::forward() const -> std::array<float, 3> {
    return {fx_.load(std::memory_order_acquire), fy_.load(std::memory_order_acquire),
            fz_.load(std::memory_order_acquire)};
}

auto AudioListener::up() const -> std::array<float, 3> {
    return {ux_.load(std::memory_order_acquire), uy_.load(std::memory_order_acquire),
            uz_.load(std::memory_order_acquire)};
}

// ============================================================
// PannerNode — equal-power 声像 + inverse 距离衰减（块级更新）
// ============================================================

auto PannerNode::set_position(float x, float y, float z) -> void {
    px_.store(x, std::memory_order_release);
    py_.store(y, std::memory_order_release);
    pz_.store(z, std::memory_order_release);
}

auto PannerNode::position() const -> std::array<float, 3> {
    return {px_.load(std::memory_order_acquire), py_.load(std::memory_order_acquire),
            pz_.load(std::memory_order_acquire)};
}

auto PannerNode::process(const AudioRenderContext &p) -> void {
    const auto lpos = ctx_.listener().position();
    const auto lfwd = ctx_.listener().forward();
    const auto lup = ctx_.listener().up();
    const float dx = px_.load(std::memory_order_acquire) - lpos[0];
    const float dy = py_.load(std::memory_order_acquire) - lpos[1];
    const float dz = pz_.load(std::memory_order_acquire) - lpos[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // inverse 距离模型：gain = ref / (ref + rolloff·(max(d, ref) - ref))
    const float ref = ref_distance_.load(std::memory_order_acquire);
    const float rolloff = rolloff_.load(std::memory_order_acquire);
    const float dmax = std::max(dist, ref);
    const float dist_gain = ref / (ref + rolloff * (dmax - ref));

    // right = normalize(cross(forward, up))；pan = sin(方位角) = dot(dir, right)
    float pan = 0.0F;
    if (dist > 1e-9F) {
        const float rx = lfwd[1] * lup[2] - lfwd[2] * lup[1];
        const float ry = lfwd[2] * lup[0] - lfwd[0] * lup[2];
        const float rz = lfwd[0] * lup[1] - lfwd[1] * lup[0];
        const float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rlen > 1e-9F) {
            pan = (dx * rx + dy * ry + dz * rz) / (dist * rlen);
        }
    }
    const float angle = (pan + 1.0F) * static_cast<float>(AURORA_AUDIO_PI) * 0.25F;
    const float lg = std::cos(angle);  // 能量守恒：L² + R² = 1
    const float rg = std::sin(angle);
    for (int i = 0; i < p.frames; ++i) {
        out_bus_[static_cast<std::size_t>(i) * 2] = in_bus_[static_cast<std::size_t>(i) * 2] * dist_gain * lg;
        out_bus_[static_cast<std::size_t>(i) * 2 + 1] = in_bus_[static_cast<std::size_t>(i) * 2 + 1] * dist_gain * rg;
    }
}

// ============================================================
// AnalyserNode — Blackman 窗 + radix-2 复 FFT（自实现零三方）
// ============================================================

auto AnalyserNode::set_fft_size(int n) -> Result<void> {
    if (n < 32 || n > 32768 || (n & (n - 1)) != 0) {
        return make_param_error("fft_size must be a power of two in [32, 32768]");
    }
    fft_size_.store(n, std::memory_order_release);
    return Result<void>{};
}

auto AnalyserNode::set_smoothing_time_constant(float v) -> void {
    smoothing_.store(std::clamp(v, 0.0F, 1.0F), std::memory_order_release);
}

auto AnalyserNode::process(const AudioRenderContext &p) -> void {
    const std::size_t total = static_cast<std::size_t>(p.frames) * 2U;
    std::copy_n(in_bus_.data(), total, out_bus_.data());  // 直通
    std::lock_guard<std::mutex> lock(mutex_);
    const int n = fft_size_.load(std::memory_order_acquire);
    if (static_cast<int>(time_ring_.size()) != n) {
        // fft_size 变更（或首块）：重建环/窗/工作缓冲；频域快照按 min_db 起平滑（数块收敛）
        time_ring_.assign(static_cast<std::size_t>(n), 0.0F);
        time_write_ = 0;
        time_fill_ = 0;
        window_.resize(static_cast<std::size_t>(n));
        for (int j = 0; j < n; ++j) {
            const double ph = 2.0 * AURORA_AUDIO_PI * static_cast<double>(j) / static_cast<double>(n - 1);
            window_[static_cast<std::size_t>(j)] =
                static_cast<float>(0.42 - 0.5 * std::cos(ph) + 0.08 * std::cos(2.0 * ph));
        }
        fft_re_.assign(static_cast<std::size_t>(n), 0.0F);
        fft_im_.assign(static_cast<std::size_t>(n), 0.0F);
        spectrum_db_.assign(static_cast<std::size_t>(n) / 2U, min_db_.load(std::memory_order_acquire));
        time_bytes_.assign(static_cast<std::size_t>(n), 128);
    }
    // mono 下混 (L+R)/2 写入时域环
    for (int i = 0; i < p.frames; ++i) {
        time_ring_[time_write_] =
            (in_bus_[static_cast<std::size_t>(i) * 2] + in_bus_[static_cast<std::size_t>(i) * 2 + 1]) * 0.5F;
        time_write_ = (time_write_ + 1U) % static_cast<std::size_t>(n);
    }
    time_fill_ = std::min(static_cast<std::size_t>(n), time_fill_ + static_cast<std::size_t>(p.frames));

    // 时域字节快照（时间序；缺历史前段补静音 128）
    const std::size_t silent_lead = static_cast<std::size_t>(n) - time_fill_;
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
        float s = 0.0F;
        if (i >= silent_lead) {
            const std::size_t j = i - silent_lead;
            s = time_ring_[(time_write_ + j + static_cast<std::size_t>(n) - time_fill_) % static_cast<std::size_t>(n)];
        }
        s = std::clamp(s, -1.0F, 1.0F);
        time_bytes_[i] =
            static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround((s + 1.0F) * 127.5F)), 0, 255));
    }
    run_fft_locked();
}

auto AnalyserNode::run_fft_locked() -> void {
    const int n = fft_size_.load(std::memory_order_acquire);
    const std::size_t bins = static_cast<std::size_t>(n) / 2U;
    // 时域环（时间序）加窗 → 复 FFT（fill < n 时环内未写区即 0，直接取环）
    for (int j = 0; j < n; ++j) {
        const std::size_t idx = (time_write_ + static_cast<std::size_t>(n) - time_fill_ + static_cast<std::size_t>(j)) %
                                static_cast<std::size_t>(n);
        fft_re_[static_cast<std::size_t>(j)] = time_ring_[idx] * window_[static_cast<std::size_t>(j)];
        fft_im_[static_cast<std::size_t>(j)] = 0.0F;
    }
    // 位反转
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(fft_re_[static_cast<std::size_t>(i)], fft_re_[static_cast<std::size_t>(j)]);
            std::swap(fft_im_[static_cast<std::size_t>(i)], fft_im_[static_cast<std::size_t>(j)]);
        }
    }
    // 蝶形
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * AURORA_AUDIO_PI / static_cast<double>(len);
        const auto wr = static_cast<float>(std::cos(ang));
        const auto wi = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0F;
            float cwi = 0.0F;
            for (int j = 0; j < len / 2; ++j) {
                const auto a = static_cast<std::size_t>(i) + static_cast<std::size_t>(j);
                const auto b = a + static_cast<std::size_t>(len) / 2U;
                const float vr = fft_re_[b] * cwr - fft_im_[b] * cwi;
                const float vi = fft_re_[b] * cwi + fft_im_[b] * cwr;
                fft_re_[b] = fft_re_[a] - vr;
                fft_im_[b] = fft_im_[a] - vi;
                fft_re_[a] += vr;
                fft_im_[a] += vi;
                const float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
    // 幅度归一（mag/(N/4)：全幅正弦峰值桶 ≈ -1.5 dB）→ dB 域平滑
    const float norm = static_cast<float>(n) / 4.0F;
    const float tau = smoothing_.load(std::memory_order_acquire);
    if (spectrum_db_.size() != bins) {
        spectrum_db_.assign(bins, min_db_.load(std::memory_order_acquire));
    }
    for (std::size_t k = 0; k < bins; ++k) {
        const float mag = std::sqrt(fft_re_[k] * fft_re_[k] + fft_im_[k] * fft_im_[k]) / norm;
        const float db = 20.0F * std::log10(std::max(mag, 1e-10F));
        spectrum_db_[k] = tau * spectrum_db_[k] + (1.0F - tau) * db;
    }
}

auto AnalyserNode::get_float_frequency_data(std::span<float> out) const -> void {
    std::lock_guard<std::mutex> lock(mutex_);
    const float lo = min_db_.load(std::memory_order_acquire);
    const float hi = max_db_.load(std::memory_order_acquire);
    const std::size_t count = std::min(out.size(), spectrum_db_.size());
    for (std::size_t k = 0; k < count; ++k) {
        out[k] = std::clamp(spectrum_db_[k], lo, hi);
    }
}

auto AnalyserNode::get_byte_frequency_data(std::span<std::uint8_t> out) const -> void {
    std::lock_guard<std::mutex> lock(mutex_);
    const float lo = min_db_.load(std::memory_order_acquire);
    const float hi = max_db_.load(std::memory_order_acquire);
    const float range = std::max(hi - lo, 1.0F);
    const std::size_t count = std::min(out.size(), spectrum_db_.size());
    for (std::size_t k = 0; k < count; ++k) {
        const float db = std::clamp(spectrum_db_[k], lo, hi);
        out[k] = static_cast<std::uint8_t>(std::clamp(std::lround((db - lo) / range * 255.0F), 0L, 255L));
    }
}

auto AnalyserNode::get_byte_time_data(std::span<std::uint8_t> out) -> void {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = std::min(out.size(), time_bytes_.size());
    std::copy_n(time_bytes_.data(), count, out.begin());
}

// ============================================================
// AudioMicrophoneSourceNode — 采集线程为推流环唯一生产者
// ============================================================

AudioMicrophoneSourceNode::AudioMicrophoneSourceNode(AudioContext &ctx, std::unique_ptr<AudioCaptureBackend> backend)
    : AudioStreamSourceNode(ctx, 16384), capture_(std::move(backend)) {}

AudioMicrophoneSourceNode::~AudioMicrophoneSourceNode() {
    if (capture_ != nullptr) {
        capture_->stop();  // 先停采集线程，再析构基类推流环（派生成员先销毁）
    }
}

// ============================================================
// AudioRecordingDestinationNode — 直通录制汇 + WAV 导出
// ============================================================

auto AudioRecordingDestinationNode::start() -> Result<void> {
    if (ctx_.closed()) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    recording_.store(true, std::memory_order_release);
    return Result<void>{};
}

auto AudioRecordingDestinationNode::stop() -> void { recording_.store(false, std::memory_order_release); }

auto AudioRecordingDestinationNode::recording() const -> std::vector<float> {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return data_;
}

auto AudioRecordingDestinationNode::recorded_frames() const -> std::size_t {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return data_.size() / 2U;
}

auto AudioRecordingDestinationNode::process(const AudioRenderContext &p) -> void {
    const std::size_t total = static_cast<std::size_t>(p.frames) * 2U;
    if (recording_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        data_.insert(data_.end(), in_bus_.data(), in_bus_.data() + total);
    }
    std::copy_n(in_bus_.data(), total, out_bus_.data());  // 直通
}

namespace {

auto append_u16(std::vector<std::uint8_t> &b, std::uint16_t v) -> void {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

auto append_u32(std::vector<std::uint8_t> &b, std::uint32_t v) -> void {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8U * static_cast<unsigned>(i))) & 0xFFU));
    }
}

}  // namespace

auto AudioRecordingDestinationNode::to_wav_bytes() const -> std::vector<std::uint8_t> {
    // 16-bit PCM stereo RIFF：44 字节头 + 样本载荷（样本按 context 采样率）
    const std::vector<float> snap = recording();
    const auto data_bytes = static_cast<std::uint32_t>(snap.size() * 2U);
    const auto rate = static_cast<std::uint32_t>(ctx_.sample_rate());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44U + snap.size() * 2U);
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    append_u32(bytes, 36U + data_bytes);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    append_u32(bytes, 16U);
    append_u16(bytes, 1U);  // PCM
    append_u16(bytes, 2U);  // stereo
    append_u32(bytes, rate);
    append_u32(bytes, rate * 4U);  // byte rate = rate * channels * bytes/sample
    append_u16(bytes, 4U);  // block align
    append_u16(bytes, 16U);  // bits per sample
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    append_u32(bytes, data_bytes);
    for (float s : snap) {
        s = std::clamp(s, -1.0F, 1.0F);
        const auto v = static_cast<std::int16_t>(std::lround(s * 32767.0F));
        append_u16(bytes, static_cast<std::uint16_t>(v));  // 二补码位型一致
    }
    return bytes;
}

auto AudioRecordingDestinationNode::save_wav(const std::string &path) const -> Result<void> {
    const std::vector<std::uint8_t> bytes = to_wav_bytes();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return make_error(ErrorCode::AudioRecordingFailed,
                          ErrorParams{{"reason", "cannot open '" + path + "' for writing"}});
    }
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file) {
        return make_error(ErrorCode::AudioRecordingFailed, ErrorParams{{"reason", "write failed: '" + path + "'"}});
    }
    return Result<void>{};
}

// ============================================================
// AudioContext
// ============================================================

namespace {

// 默认设备后端工厂：Linux 开启 AURORA_ENABLE_AUDIO_ALSA 时构造 AlsaDeviceBackend，
// 其余平台构造 WasapiDeviceBackend——两者宏关闭时 .cpp 体均为 disabled 桩
// （start 恒 false），AudioContext 随即静默降级；真实实现见 audio_alsa.cpp /
// audio_wasapi.cpp。
auto create_default_device_backend() -> std::unique_ptr<AudioDeviceBackend> {
#ifdef AURORA_ENABLE_AUDIO_ALSA
    return std::make_unique<AlsaDeviceBackend>();
#else
    return std::make_unique<WasapiDeviceBackend>();
#endif
}

// 默认采集后端工厂：与设备后端同口径（ALSA/WASAPI 按宏择路）——disabled 桩
// start 恒 false → create_microphone_source 显式报错，录制不静默降级。
[[maybe_unused]] auto create_default_capture_backend() -> std::unique_ptr<AudioCaptureBackend> {
#ifdef AURORA_ENABLE_AUDIO_ALSA
    return std::make_unique<AlsaCaptureBackend>();
#else
    return std::make_unique<WasapiCaptureBackend>();
#endif
}

}  // namespace

AudioContext::AudioContext(std::unique_ptr<AudioDeviceBackend> device_backend,
                           std::unique_ptr<AudioCaptureBackend> capture_backend) {
    capture_backend_ = std::move(capture_backend);
    device_ = std::move(device_backend);
    if (device_ == nullptr) {
        device_ = create_default_device_backend();
    }
    destination_ = std::shared_ptr<AudioDestinationNode>(new AudioDestinationNode(*this));
    nodes_.push_back(destination_);
    command_ring_.resize(kCommandRingCapacity);
    if (device_ != nullptr) {
        format_ = device_->format();
        auto render_fn = [this](float *out, int frames) { render_block(out, frames); };
        if (!device_->start(std::move(render_fn))) {
            AURORA_LOG_WARN("audio", "audio device start failed; running in silent mode");
            device_ = nullptr;  // 静默模式：图照常运转，样本丢弃
        }
    }
    direct_graph_ = device_ == nullptr;
    if (direct_graph_) {
        format_ = AudioDeviceFormat{};  // 处理格式（48000/2）
    }
}

AudioContext::~AudioContext() {
    if (!closed_.load(std::memory_order_acquire)) {
        close();
    }
}

// ---- 拓扑 ----

auto AudioContext::owns_node(const AudioNode *n) const -> bool {
    return std::ranges::any_of(nodes_, [n](const std::shared_ptr<AudioNode> &p) { return p.get() == n; });
}

// 增加 src→dst 会成环 ⇔ 从 dst 沿既有边可达 src。
auto AudioContext::would_cycle(const AudioNode *src, AudioNode *dst) const -> bool {
    std::vector<AudioNode *> stack{dst};
    // 小图（UI 速率校验）：visited 用线性表即可
    std::vector<AudioNode *> visited;
    while (!stack.empty()) {
        AudioNode *n = stack.back();
        stack.pop_back();
        if (n == src) {
            return true;
        }
        if (std::ranges::find(visited, n) != visited.end()) {
            continue;
        }
        visited.push_back(n);
        for (const auto &e : edges_) {
            if (e.first == n) {
                stack.push_back(e.second);
            }
        }
    }
    return false;
}

auto AudioContext::connect(const std::shared_ptr<AudioNode> &src, const std::shared_ptr<AudioNode> &dst)
    -> Result<void> {
    if (closed_.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    if (src == nullptr || dst == nullptr) {
        return make_error(ErrorCode::GeneralInvalidArgument, std::string("null audio node"));
    }
    if (src.get() == dst.get()) {
        return make_error(ErrorCode::AudioGraphCycle, std::string("connecting a node to itself"));
    }
    if (!owns_node(src.get()) || !owns_node(dst.get())) {
        return make_error(ErrorCode::GeneralInvalidArgument, std::string("node not owned by this context"));
    }
    if (src.get() == destination_.get()) {
        return make_error(ErrorCode::GeneralInvalidArgument, std::string("destination is a terminal node"));
    }
    if (would_cycle(src.get(), dst.get())) {
        return make_error(ErrorCode::AudioGraphCycle, std::string("connection would create a cycle"));
    }
    const bool exists =
        std::ranges::any_of(edges_, [&](const auto &e) { return e.first == src.get() && e.second == dst.get(); });
    if (exists) {
        return make_error(ErrorCode::GeneralInvalidArgument, std::string("nodes already connected"));
    }
    if (direct_graph_) {
        apply_connect(src.get(), dst.get());
    } else {
        const std::uint64_t w = cmd_write_.load(std::memory_order_relaxed);
        if (w - cmd_read_.load(std::memory_order_acquire) >= kCommandRingCapacity) {
            return make_error(ErrorCode::GeneralUnknown, std::string("audio command queue full; retry"));
        }
        command_ring_[w % kCommandRingCapacity] = GraphCommand{GraphCommand::Kind::Connect, src, dst};
        cmd_write_.store(w + 1, std::memory_order_release);
    }
    edges_.emplace_back(src.get(), dst.get());
    return Result<void>{};
}

auto AudioContext::disconnect(const std::shared_ptr<AudioNode> &src, const std::shared_ptr<AudioNode> &dst)
    -> Result<void> {
    if (closed_.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    const auto it =
        std::ranges::find_if(edges_, [&](const auto &e) { return e.first == src.get() && e.second == dst.get(); });
    if (it == edges_.end()) {
        return make_error(ErrorCode::AudioEdgeNotFound, ErrorParams{});
    }
    edges_.erase(it);
    if (direct_graph_) {
        apply_disconnect(src.get(), dst.get());
    } else {
        const std::uint64_t w = cmd_write_.load(std::memory_order_relaxed);
        if (w - cmd_read_.load(std::memory_order_acquire) >= kCommandRingCapacity) {
            edges_.emplace_back(src.get(), dst.get());  // 回滚 UI 视角
            return make_error(ErrorCode::GeneralUnknown, std::string("audio command queue full; retry"));
        }
        command_ring_[w % kCommandRingCapacity] = GraphCommand{GraphCommand::Kind::Disconnect, src, dst};
        cmd_write_.store(w + 1, std::memory_order_release);
    }
    return Result<void>{};
}

auto AudioContext::apply_connect(AudioNode *src, AudioNode *dst) -> void {
    Adjacency &s = adj_[src];
    Adjacency &d = adj_[dst];
    s.consumers.push_back(dst);
    d.sources.push_back(src);
    rebuild_topo();
}

auto AudioContext::apply_disconnect(AudioNode *src, AudioNode *dst) -> void {
    if (auto it = adj_.find(src); it != adj_.end()) {
        auto &cs = it->second.consumers;
        std::erase(cs, dst);
        if (it->second.sources.empty() && cs.empty()) {
            adj_.erase(it);
        }
    }
    if (auto it = adj_.find(dst); it != adj_.end()) {
        auto &ss = it->second.sources;
        std::erase(ss, src);
        if (ss.empty() && it->second.consumers.empty()) {
            adj_.erase(it);
        }
    }
    rebuild_topo();
}

// Kahn：源先于汇。环已被 UI 侧校验排除；防御性兜底按剩余顺序追加。
auto AudioContext::rebuild_topo() -> void {
    topo_.clear();
    std::unordered_map<AudioNode *, std::size_t> indegree;
    indegree.reserve(adj_.size());
    for (const auto &[n, adj] : adj_) {
        indegree[n] = adj.sources.size();
    }
    std::vector<AudioNode *> ready;
    for (const auto &[n, deg] : indegree) {
        if (deg == 0) {
            ready.push_back(n);
        }
    }
    while (!ready.empty()) {
        AudioNode *n = ready.back();
        ready.pop_back();
        topo_.push_back(n);
        const auto it = adj_.find(n);
        if (it == adj_.end()) {
            continue;
        }
        for (AudioNode *c : it->second.consumers) {
            if (--indegree[c] == 0) {
                ready.push_back(c);
            }
        }
    }
    if (topo_.size() != indegree.size()) {  // 防御：不应到达（UI 侧禁环）
        for (const auto &n : indegree | std::views::keys) {
            if (std::ranges::find(topo_, n) == topo_.end()) {
                topo_.push_back(n);
            }
        }
    }
}

// ---- 工厂 ----
//
// 下列工厂语句内的 `new` 立即移交 `std::shared_ptr`，对象由 `nodes_`（唯一属主）与返回值共同持有，不存在泄漏；
// 节点构造函数为私有（只能经这些工厂创建），故无法改用 `std::make_shared` 把 `new` 藏进库内。
auto AudioContext::create_destination() -> std::shared_ptr<AudioDestinationNode> {
    auto n = std::shared_ptr<AudioDestinationNode>(new AudioDestinationNode(*this));  // NOLINT
    nodes_.push_back(n);
    return n;
}

auto AudioContext::create_stream_source(std::size_t ring_capacity_frames) -> std::shared_ptr<AudioStreamSourceNode> {
    auto n = std::shared_ptr<AudioStreamSourceNode>(new AudioStreamSourceNode(*this, ring_capacity_frames));  // NOLINT
    nodes_.push_back(n);
    return n;
}

auto AudioContext::create_gain() -> std::shared_ptr<GainNode> {
    auto n = std::shared_ptr<GainNode>(new GainNode(*this));  // NOLINT
    nodes_.push_back(n);
    return n;
}

auto AudioContext::create_buffer_source() -> std::shared_ptr<AudioBufferSourceNode> {
    auto n = std::shared_ptr<AudioBufferSourceNode>(new AudioBufferSourceNode(*this));  // NOLINT
    nodes_.push_back(n);
    return n;
}

auto AudioContext::create_panner() -> std::shared_ptr<PannerNode> {
    auto n = std::shared_ptr<PannerNode>(new PannerNode(*this));  // NOLINT
    nodes_.push_back(n);
    return n;
}

auto AudioContext::create_analyser() -> std::shared_ptr<AnalyserNode> {
    auto n = std::shared_ptr<AnalyserNode>(new AnalyserNode(*this));  // NOLINT
    nodes_.push_back(n);
    return n;
}

auto AudioContext::create_recording_destination() -> std::shared_ptr<AudioRecordingDestinationNode> {
    auto n = std::shared_ptr<AudioRecordingDestinationNode>(new AudioRecordingDestinationNode(*this));  // NOLINT
    nodes_.push_back(n);
    return n;
}

// 同上：`new` 立即归 `shared_ptr`（失败路径由局部 shared_ptr、成功路径由 nodes_释放）
auto AudioContext::create_microphone_source() -> Result<std::shared_ptr<AudioMicrophoneSourceNode>> {
    if (closed_.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    // 注入采集后端优先（测试桩/自定义）；否则按需创建默认后端（不可用 → disabled 桩 start false）
    std::unique_ptr<AudioCaptureBackend> backend = std::move(capture_backend_);
    // NOLINTNEXTLINE
    auto node = std::shared_ptr<AudioMicrophoneSourceNode>(new AudioMicrophoneSourceNode(*this, std::move(backend)));
    // 采集线程是推流环的唯一生产者：float32 → int16 复用推流通道（Q15 往返误差 ~3e-5，可忽略）
    AudioMicrophoneSourceNode *raw = node.get();
    AudioCaptureBackend::CaptureFn on_pcm = [raw](const float *pcm, int frames, int rate, int channels) {
        if (pcm == nullptr || frames <= 0 || rate <= 0 || (channels != 1 && channels != 2)) {
            return;
        }
        const std::size_t total = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
        std::vector<std::int16_t> conv(total);
        for (std::size_t i = 0; i < total; ++i) {
            const float s = std::clamp(pcm[i], -1.0F, 1.0F);
            conv[i] = static_cast<std::int16_t>(std::lround(s * 32767.0F));
        }
        (void)raw->push(conv, rate, channels);
    };
    if (!node->capture_->start(std::move(on_pcm))) {
        node->capture_->stop();  // 失败即停采集（stop_calls==1）
        capture_backend_ = std::move(node->capture_);  // 保留后端，避免悬垂；析构时不再重复 stop
        return make_error(ErrorCode::AudioDeviceUnavailable, // NOLINT
                          std::string("audio capture device unavailable or permission denied"));  // NOLINT
    }
    nodes_.push_back(node);
    return node;  // NOLINT
}

// ---- 生命周期 ----

auto AudioContext::suspend() -> Result<void> {
    if (closed_.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    suspended_.store(true, std::memory_order_release);
    return Result<void>{};
}

auto AudioContext::resume() -> Result<void> {
    if (closed_.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    suspended_.store(false, std::memory_order_release);
    return Result<void>{};
}

auto AudioContext::close() -> Result<void> {
    if (closed_.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::AudioContextClosed, ErrorParams{});
    }
    closed_.store(true, std::memory_order_release);
    if (device_ != nullptr) {
        device_->stop();  // 契约：返回前设备线程已退出；后端保留至上下文析构时释放
    }
    return Result<void>{};
}

// ---- 设备/时钟 ----

auto AudioContext::device_state() const -> AudioDeviceState {
    if (closed_.load(std::memory_order_acquire)) {
        return AudioDeviceState::Silent;  // close 后终态：设备已停，等同静默
    }
    return device_ != nullptr ? AudioDeviceState::Active : AudioDeviceState::Silent;
}

auto AudioContext::current_time() const -> double {
    return static_cast<double>(rendered_samples_.load(std::memory_order_acquire)) /
           static_cast<double>(format_.sample_rate);
}

// ---- 渲染 ----

auto AudioContext::drain_commands() -> void {
    std::uint64_t r = cmd_read_.load(std::memory_order_relaxed);
    const std::uint64_t w = cmd_write_.load(std::memory_order_acquire);
    while (r < w) {
        const GraphCommand &c = command_ring_[r % kCommandRingCapacity];
        if (c.kind == GraphCommand::Kind::Connect) {
            apply_connect(c.src.get(), c.dst.get());
        } else {
            apply_disconnect(c.src.get(), c.dst.get());
        }
        ++r;
    }
    cmd_read_.store(r, std::memory_order_release);
}

auto AudioContext::render_silence(float *out, int frames) const -> void {
    std::memset(out, 0, static_cast<std::size_t>(frames) * static_cast<std::size_t>(format_.channels) * sizeof(float));
}

auto AudioContext::render_block(float *interleaved_out, int frames) -> void {
    if (frames <= 0) {
        return;
    }
    const std::size_t total = static_cast<std::size_t>(frames) * static_cast<std::size_t>(format_.channels);
    if (closed_.load(std::memory_order_acquire) || suspended_.load(std::memory_order_acquire)) {
        render_silence(interleaved_out, frames);  // 时钟冻结
        return;
    }
    drain_commands();
    // 缓冲清零 + 容量保障（in/out 全零起点，节点可只写非零段）
    const auto zero_bus = [frames](AudioNode *n) {
        n->ensure_capacity(frames);
        const std::size_t total_node = static_cast<std::size_t>(frames) * static_cast<std::size_t>(n->channel_count());
        std::fill_n(n->input_bus(), total_node, 0.0F);
        std::fill_n(n->output_bus(), total_node, 0.0F);
    };
    for (AudioNode *n : topo_) {
        zero_bus(n);
    }
    destination_->ensure_capacity(frames);
    const std::size_t dest_total = static_cast<std::size_t>(frames) * 2U;
    std::fill_n(destination_->input_bus(), dest_total, 0.0F);
    std::fill_n(destination_->output_bus(), dest_total, 0.0F);

    const AudioRenderContext p{frames, format_.sample_rate,
                               static_cast<double>(rendered_samples_.load(std::memory_order_relaxed)) /
                                   static_cast<double>(format_.sample_rate)};
    for (AudioNode *n : topo_) {
        n->process(p);
        const auto it = adj_.find(n);
        if (it == adj_.end()) {
            continue;
        }
        const std::size_t src_total = static_cast<std::size_t>(frames) * static_cast<std::size_t>(n->channel_count());
        for (AudioNode *c : it->second.consumers) {
            float *dst_bus = c == destination_.get() ? destination_->input_bus() : c->input_bus();
            for (std::size_t i = 0; i < src_total; ++i) {
                dst_bus[i] += n->output_bus()[i];
            }
        }
    }
    destination_->process(p);
    const float mv = master_volume_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < total; ++i) {
        interleaved_out[i] = destination_->output_bus()[i] * mv;
    }
    rendered_samples_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
}

}  // namespace aurora
