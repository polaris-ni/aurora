// ============================================================
// media/audio_alsa.cpp — ALSA 音频设备后端实现
// ------------------------------------------------------------
// 运行时绑定（dlopen "libasound.so.2"）：不引 <alsa/asoundlib.h>、不链
// libasound——**零构建期依赖**（无需 dev 包即可编译，本仓 WSL 验证门径即此），
// ABI 常量按内核 UAPI（include/uapi/sound/asound.h，用户态 ABI 自 v2.6 不变）
// 本地声明。库缺失/符号不全 → api().ok == false → start 返回 false（与
// WASAPI 后端「start 失败即静默降级」同一契约）。
//
// 渲染线程：avail_update → render_block → writei（≤ 一块 20ms，阻塞式天然
// 节流）；XRUN(-EPIPE)/系统挂起(-ESTRPIPE) 经 snd_pcm_recover 自愈，不可恢复
// 错误/DISCONNECTED → 关闭端点退避 200ms 重开（对齐 WASAPI 重路由语义，期间
// 图时钟冻结）。采集线程对称：wait → avail → readi → 回调；中段不可恢复失败
// → device_failed 观察位置位后线程退出（`running` 留作 stop 握手位——若在线程
// 内清零，stop 早退不 join，~thread joinable → std::terminate，同 WASAPI 审计
// 缺陷 ③ 的教训）。
// ============================================================

#include "audio_alsa.h"

#include <atomic>

#ifdef AURORA_ENABLE_AUDIO_ALSA

#include <dlfcn.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/log.h"

namespace aurora {

namespace {

// ---- ALSA ABI 常量（内核 UAPI 转录，见文件头注释）----

constexpr int AURORA_STREAM_PLAYBACK = 0;  // SND_PCM_STREAM_PLAYBACK
constexpr int AURORA_STREAM_CAPTURE = 1;  // SND_PCM_STREAM_CAPTURE
constexpr int AURORA_FORMAT_FLOAT_LE = 18;  // SNDRV_PCM_FORMAT_FLOAT_LE
constexpr int AURORA_ACCESS_RW_INTERLEAVED = 3;  // SND_PCM_ACCESS_RW_INTERLEAVED
constexpr int AURORA_STATE_DISCONNECTED = 7;  // SND_PCM_STATE_DISCONNECTED

constexpr int AURORA_RENDER_RATE = 48000;  // 图契约采样率（插件层吸收设备差异）
constexpr int AURORA_RENDER_CHANNELS = 2;  // 图契约声道数
constexpr unsigned AURORA_LATENCY_US = 20000;  // 缓冲目标延迟 20ms（≈ WASAPI 引擎缓冲）
constexpr int AURORA_MAX_BLOCK_FRAMES = 960;  // 单轮渲染上限（20ms @48k）
constexpr int AURORA_WAIT_TIMEOUT_MS = 20;  // 无余量时 wait 超时（stop 响应性上界）
constexpr int AURORA_OPEN_FAILURE_BACKOFF_MS = 200;  // 端点重开退避（对齐 WASAPI 重路由）

using PcmHandle = void;  // snd_pcm_t 为不透明句柄

// snd_pcm_uframes_t == unsigned long（ILP32/LP64 同为机器字长）；
// snd_pcm_sframes_t == long。set_params 的 format/access 为枚举（int 表示）。
using UFrames = unsigned long;
auto dlsym_fn(void *lib, const char *name) -> void * { return dlsym(lib, name); }

struct AlsaApi {
    using Open = int (*)(PcmHandle **, const char *, int, int);
    using Close = int (*)(PcmHandle *);
    using SetParams = int (*)(PcmHandle *, int, int, unsigned int, unsigned int, int, unsigned int);
    using Recover = int (*)(PcmHandle *, int, int);
    using Writei = long (*)(PcmHandle *, const void *, UFrames);
    using Readi = long (*)(PcmHandle *, void *, UFrames);
    using AvailUpdate = long (*)(PcmHandle *);
    using Wait = int (*)(PcmHandle *, int);
    using State = int (*)(PcmHandle *);
    using Drop = int (*)(PcmHandle *);
    using StrError = const char *(*)(int);

    Open open = nullptr;
    Close close = nullptr;
    SetParams set_params = nullptr;
    Recover recover = nullptr;
    Writei writei = nullptr;
    Readi readi = nullptr;
    AvailUpdate avail_update = nullptr;
    Wait wait = nullptr;
    State state = nullptr;
    Drop drop = nullptr;
    StrError strerror_ = nullptr;
    bool ok = false;

    AlsaApi() { load(); }

    auto load() -> void {
        // 只试标准 SONAME；成功后不再 dlclose——音频线程生命周期与进程同寿，
        // 卸载库会让已解析函数指针悬垂（静态库消费者无 dlopen 句柄管理义务）。
        void *lib = dlopen("libasound.so.2", RTLD_LAZY | RTLD_LOCAL);
        if (lib == nullptr) {
            lib = dlopen("libasound.so", RTLD_LAZY | RTLD_LOCAL);
        }
        if (lib == nullptr) {
            return;  // 无 ALSA 运行库：start 恒 false → 静默降级
        }
        open = reinterpret_cast<Open>(dlsym_fn(lib, "snd_pcm_open"));
        close = reinterpret_cast<Close>(dlsym_fn(lib, "snd_pcm_close"));
        set_params = reinterpret_cast<SetParams>(dlsym_fn(lib, "snd_pcm_set_params"));
        recover = reinterpret_cast<Recover>(dlsym_fn(lib, "snd_pcm_recover"));
        writei = reinterpret_cast<Writei>(dlsym_fn(lib, "snd_pcm_writei"));
        readi = reinterpret_cast<Readi>(dlsym_fn(lib, "snd_pcm_readi"));
        avail_update = reinterpret_cast<AvailUpdate>(dlsym_fn(lib, "snd_pcm_avail_update"));
        wait = reinterpret_cast<Wait>(dlsym_fn(lib, "snd_pcm_wait"));
        state = reinterpret_cast<State>(dlsym_fn(lib, "snd_pcm_state"));
        drop = reinterpret_cast<Drop>(dlsym_fn(lib, "snd_pcm_drop"));
        strerror_ = reinterpret_cast<StrError>(dlsym_fn(lib, "snd_strerror"));
        ok = open != nullptr && close != nullptr && set_params != nullptr && recover != nullptr && writei != nullptr &&
             readi != nullptr && avail_update != nullptr && wait != nullptr && state != nullptr && drop != nullptr;
        if (!ok) {
            AURORA_LOG_WARN("audio", "libasound present but required snd_pcm symbols missing; ALSA backend disabled");
        }
    }
};

/// 进程级单例：首次触达加载，线程安全（magic statics）。
auto api() -> const AlsaApi & {
    static const AlsaApi instance;
    return instance;
}

auto describe(int err) -> std::string {
    const auto &a = api();
    if (a.strerror_ != nullptr) {
        const char *text = a.strerror_(err);
        return text != nullptr ? std::string(text) : std::string("unknown");
    }
    return std::string("unavailable");
}

}  // namespace

struct AlsaDeviceBackend::Impl {
    RenderFn render;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> reroute{false};
    PcmHandle *pcm = nullptr;  // 启动期由调用线程建立；运行期仅设备线程触达

    ~Impl() { close_endpoint(); }

    auto open_endpoint() -> bool {
        if (pcm != nullptr) {
            return true;
        }
        const auto &a = api();
        if (!a.ok || a.open(&pcm, "default", AURORA_STREAM_PLAYBACK, 0) != 0 || pcm == nullptr) {
            pcm = nullptr;
            return false;
        }
        // soft_resync=1：XRUN 后自动重同步（恢复静音对齐），缓冲延迟 20ms。
        if (a.set_params(pcm, AURORA_FORMAT_FLOAT_LE, AURORA_ACCESS_RW_INTERLEAVED, AURORA_RENDER_CHANNELS,
                         AURORA_RENDER_RATE, 1, AURORA_LATENCY_US) != 0) {
            a.close(pcm);
            pcm = nullptr;
            return false;
        }
        return true;
    }

    auto close_endpoint() -> void {
        if (pcm != nullptr) {
            const auto &a = api();
            a.drop(pcm);
            a.close(pcm);
            pcm = nullptr;
        }
    }

    /// snd_pcm_recover 自愈 XRUN/挂起；失败（设备级错误）返回 false → 重路由。
    auto recover_or_reroute(int err) -> void {
        if (api().recover(pcm, err, 1) != 0) {
            AURORA_LOG_WARN("audio", "ALSA recover failed (" + describe(err) + "); rerouting endpoint");
            reroute.store(true, std::memory_order_release);
        }
    }

    auto run() -> void {
        std::vector<float> scratch;
        const auto &a = api();
        while (running.load(std::memory_order_acquire)) {
            if (reroute.exchange(false, std::memory_order_acq_rel)) {
                close_endpoint();
                if (!open_endpoint()) {
                    // 端点不可用：退避重试；期间时钟冻结（对齐 WASAPI 重路由）
                    reroute.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(AURORA_OPEN_FAILURE_BACKOFF_MS));
                    continue;
                }
            }
            if (a.state(pcm) == AURORA_STATE_DISCONNECTED) {
                reroute.store(true, std::memory_order_release);
                continue;
            }
            const long avail = a.avail_update(pcm);
            if (avail < 0) {
                recover_or_reroute(static_cast<int>(avail));
                continue;
            }
            if (avail == 0) {
                const int w = a.wait(pcm, AURORA_WAIT_TIMEOUT_MS);
                if (w < 0) {
                    recover_or_reroute(w);
                }
                continue;  // 无余量/超时——不空推时钟（stop 响应性上界 AURORA_WAIT_TIMEOUT_MS）
            }
            const int frames =
                static_cast<int>(static_cast<unsigned long>(avail) < static_cast<unsigned long>(AURORA_MAX_BLOCK_FRAMES)
                                     ? static_cast<unsigned long>(avail)
                                     : static_cast<unsigned long>(AURORA_MAX_BLOCK_FRAMES));
            scratch.resize(static_cast<std::size_t>(frames) * AURORA_RENDER_CHANNELS);
            render(scratch.data(), frames);
            const long written = a.writei(pcm, scratch.data(), static_cast<UFrames>(frames));
            if (written < 0) {
                recover_or_reroute(static_cast<int>(written));
            }
        }
        close_endpoint();  // 线程内收尾（stop() join 后 pcm 已为 null，析构幂等）
    }
};

AlsaDeviceBackend::AlsaDeviceBackend() : impl_(std::make_unique<Impl>()) {}

AlsaDeviceBackend::~AlsaDeviceBackend() { stop(); }

auto AlsaDeviceBackend::format() const -> AudioDeviceFormat { return AudioDeviceFormat{}; }

auto AlsaDeviceBackend::start(RenderFn render_block) -> bool {
    if (impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    if (!api().ok || !impl_->open_endpoint()) {
        return false;
    }
    impl_->render = std::move(render_block);
    impl_->reroute.store(false, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

auto AlsaDeviceBackend::stop() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未启动（或已停止）
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();  // 线程在 ≤ wait 超时 + 一块写入内退出并自行 close_endpoint
    }
}

// ============================================================
// AlsaCaptureBackend — 默认捕获端点采集后端
// ============================================================

struct AlsaCaptureBackend::Impl {
    CaptureFn capture;
    std::thread thread;
    std::atomic<bool> running{false};  ///< stop 握手位（承载 join 义务），非死活判据
    std::atomic<bool> device_failed{false};  ///< 中段设备失败（线程已退出、回调止流）
    PcmHandle *pcm = nullptr;
    int rate = 0;  // 协商结果（回调携带）
    int channels = 0;

    ~Impl() { close_endpoint(); }

    /// "default" 捕获端点 + 格式顺位协商（48k 优先、stereo 优先）。
    auto open_endpoint() -> bool {
        static constexpr int AURORA_RATES[] = {48000, 44100};
        static constexpr int AURORA_CHANNELS[] = {2, 1};
        const auto &a = api();
        if (!a.ok) {
            return false;
        }
        for (const int candidate_rate : AURORA_RATES) {
            for (const int candidate_channels : AURORA_CHANNELS) {
                PcmHandle *handle = nullptr;
                if (a.open(&handle, "default", AURORA_STREAM_CAPTURE, 0) != 0 || handle == nullptr) {
                    return false;  // 端点本身不可用：换格式无意义
                }
                if (a.set_params(handle, AURORA_FORMAT_FLOAT_LE, AURORA_ACCESS_RW_INTERLEAVED,
                                 static_cast<unsigned int>(candidate_channels),
                                 static_cast<unsigned int>(candidate_rate), 1, AURORA_LATENCY_US) == 0) {
                    pcm = handle;
                    rate = candidate_rate;
                    channels = candidate_channels;
                    return true;
                }
                a.close(handle);
            }
        }
        return false;
    }

    auto close_endpoint() -> void {
        if (pcm != nullptr) {
            const auto &a = api();
            a.drop(pcm);
            a.close(pcm);
            pcm = nullptr;
        }
        rate = 0;
        channels = 0;
    }

    auto run() -> void {
        std::vector<float> scratch;
        const auto &a = api();
        while (running.load(std::memory_order_acquire)) {
            const long avail = a.avail_update(pcm);
            if (avail < 0) {
                if (a.recover(pcm, static_cast<int>(avail), 1) != 0) {
                    break;  // 设备级失败 → 置观察位退出（下方统一处理）
                }
                continue;
            }
            if (avail == 0) {
                const int w = a.wait(pcm, AURORA_WAIT_TIMEOUT_MS);
                if (w < 0 && a.recover(pcm, w, 1) != 0) {
                    break;
                }
                continue;
            }
            const int frames =
                static_cast<int>(static_cast<unsigned long>(avail) < static_cast<unsigned long>(AURORA_MAX_BLOCK_FRAMES)
                                     ? static_cast<unsigned long>(avail)
                                     : static_cast<unsigned long>(AURORA_MAX_BLOCK_FRAMES));
            scratch.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
            const long got = a.readi(pcm, scratch.data(), static_cast<UFrames>(frames));
            if (got < 0) {
                if (a.recover(pcm, static_cast<int>(got), 1) != 0) {
                    break;
                }
                continue;
            }
            if (got > 0) {
                capture(scratch.data(), static_cast<int>(got), rate, channels);
            }
        }
        if (running.load(std::memory_order_acquire)) {
            // 线程因设备错误先行退出：置观察位。`running` 不清零——它是 stop
            // 握手位（清零会让 stop 早退不 join，~thread joinable → terminate）。
            device_failed.store(true, std::memory_order_release);
        }
        close_endpoint();
    }
};

AlsaCaptureBackend::AlsaCaptureBackend() : impl_(std::make_unique<Impl>()) {}

AlsaCaptureBackend::~AlsaCaptureBackend() { stop(); }

auto AlsaCaptureBackend::start(CaptureFn on_pcm) -> bool {
    if (impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    impl_->capture = std::move(on_pcm);
    impl_->device_failed.store(false, std::memory_order_relaxed);  // 重开清旧败
    if (!impl_->open_endpoint()) {
        impl_->close_endpoint();
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

auto AlsaCaptureBackend::stop() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未启动（或已停止）
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

auto AlsaCaptureBackend::failed() const -> bool { return impl_->device_failed.load(std::memory_order_acquire); }

}  // namespace aurora

#else  // !AURORA_ENABLE_AUDIO_ALSA —— disabled 桩：start 恒 false → 静默模式

#include <utility>

namespace aurora {

struct AlsaDeviceBackend::Impl {};  // 桩无状态（pimpl 完整定义供 ctor/dtor 实例化）

AlsaDeviceBackend::AlsaDeviceBackend() = default;

AlsaDeviceBackend::~AlsaDeviceBackend() = default;

auto AlsaDeviceBackend::format() const -> AudioDeviceFormat { return AudioDeviceFormat{}; }

auto AlsaDeviceBackend::start(RenderFn /*render_block*/) -> bool { return false; }

auto AlsaDeviceBackend::stop() -> void {}

struct AlsaCaptureBackend::Impl {};  // 桩无状态（start 恒 false → 录制显式报错）

AlsaCaptureBackend::AlsaCaptureBackend() = default;

AlsaCaptureBackend::~AlsaCaptureBackend() = default;

auto AlsaCaptureBackend::start(CaptureFn /*on_pcm*/) -> bool { return false; }

auto AlsaCaptureBackend::stop() -> void {}

auto AlsaCaptureBackend::failed() const -> bool { return false; }  // 桩无采集线程

}  // namespace aurora

#endif  // AURORA_ENABLE_AUDIO_ALSA
