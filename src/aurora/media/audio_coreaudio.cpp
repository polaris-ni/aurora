// ============================================================
// media/audio_coreaudio.cpp — CoreAudio 音频设备后端实现
// ------------------------------------------------------------
// AudioQueue（AudioToolbox）腿：队列自持线程回调填块/取块，客户端格式恒
// 48000/2 float32 交错，设备侧采样率/声道差异由队列内建的格式转换吸收
// （与 ALSA 插件层、WASAPI AUTOCONVERTPCM 同口径，图侧契约恒定）。系统框架
// 恒在，故**无运行期绑定**：ALSA 的 dlopen 逃逸是为让无 dev 包的 Linux 构建
// 机能编译，Apple 的两枚 framework 属系统 SDK，没有该动机。
//
// 失败口径（本机实测）：无输出设备的机器上 AudioQueueNewOutput 仍返回 noErr，
// 失败落在 AudioQueueStart = kAudioQueueErr_InvalidDevice(-66680)——故拒启判据
// 取 start 的返回码，不取队列创建；无设备 → start false → AudioContext 静默降级。
//
// 重路由：默认输出设备变更经 AudioObjectAddPropertyListener 申报，通知回调**只
// 置位不重建**（不在 CoreAudio 通知线程里做设备级操作），重建在自持的控制线程
// 上做，失败按 200ms 退避重试；期间队列不存在 ⇒ 无回调 ⇒ 图时钟冻结（对齐 ALSA
// 断连重开与 WASAPI 重路由）。事件不到的拔除形态由同一节拍轮询
// kAudioQueueProperty_IsRunning 兜底。
//
// 生命周期纪律：回调里的 `render` / `capture` 在队列启动**之前**写好（启动后
// 回调可能立即到达，空 std::function 调用即崩溃）；`AudioQueueStop/Dispose(true)`
// 为同步语义（返回时在飞回调已尽），故 stop() 之后 Impl 成员可安全析构。
//
// 采集腿：中段失败置 device_failed 观察位后止流，`running` 留作 stop 握手位
// （若在线程内清零，stop 早退不 join → ~thread joinable → std::terminate，
// 同 WASAPI 审计缺陷 ③ 的教训）。麦克风权限：macOS 对未声明
// NSMicrophoneUsageDescription 就访问麦克风的进程是**终止**而非报错，故建队列前
// 先查 main bundle 的该 Info.plist 键，缺键直接拒启——裸可执行文件（单测、无壳
// 探针）据此永不被 TCC 杀掉。
// ============================================================

#include "audio_coreaudio.h"

#include <atomic>

#ifdef AURORA_ENABLE_AUDIO_COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/log.h"

namespace aurora {

namespace {

// ---- 图契约与队列几何 ----

constexpr int AURORA_RENDER_RATE = 48000;  // 图契约采样率（队列侧转换吸收设备差异）
constexpr int AURORA_RENDER_CHANNELS = 2;  // 图契约声道数
constexpr int AURORA_RENDER_BLOCK_FRAMES = 960;  // 单块帧数（20ms @48k）
constexpr int AURORA_QUEUE_BUFFER_COUNT = 3;  // 水位 3 块 ≈ 40–60ms 端到端
constexpr int AURORA_POLL_MS = 200;  // 控制线程节拍：重路由重试 + IsRunning 兜底
constexpr int AURORA_CAPTURE_BLOCK_MS = 20;  // 采集块时长（按协商到的率算帧数）

// TCC 麦克风声明键：缺键即拒启（见文件头「终止而非报错」段）。
constexpr const char *AURORA_MIC_USAGE_KEY = "NSMicrophoneUsageDescription";

/// @brief 交错 float32 线性 PCM 的 AudioStreamBasicDescription。
/// @param rate 采样率（Hz）。
/// @param channels 声道数。
/// @return 填好全部字段的 ASBD；bytesPerFrame = channels × sizeof(float)。
auto make_pcm_format(double rate, UInt32 channels) -> AudioStreamBasicDescription {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    // 三枚标志分属 SDK 里两个匿名枚举，直接 `|` 触发 -Wdeprecated-anon-enum-enum-conversion；
    // mFormatFlags 本体是 UInt32 位域，先各自归到整型上下文再按位或才是本意的拼法。
    fmt.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagIsFloat) |
                       static_cast<UInt32>(kAudioFormatFlagsNativeEndian) |
                       static_cast<UInt32>(kAudioFormatFlagIsPacked);
    fmt.mBitsPerChannel = 8U * static_cast<UInt32>(sizeof(float));
    fmt.mChannelsPerFrame = channels;
    fmt.mFramesPerPacket = 1U;
    fmt.mBytesPerFrame = channels * static_cast<UInt32>(sizeof(float));
    fmt.mBytesPerPacket = fmt.mBytesPerFrame * fmt.mFramesPerPacket;
    fmt.mReserved = 0U;
    return fmt;
}

/// @brief OSStatus 的十进制串（诊断用；fourcc 类错误码在终端上不可读，数值唯一可辨）。
/// @param status CoreAudio / AudioToolbox 返回的状态码。
/// @return 形如 "-66680" 的 ASCII 串。
auto describe(OSStatus status) -> std::string { return std::to_string(static_cast<long>(status)); }

/// @brief 系统对象上的一枚属性地址（渲染/采集腿都只监听默认设备选择项）。
/// @param property 属性 fourcc。
/// @return scope=Global、element=Main 的属性地址。
auto global_address(AudioObjectPropertySelector property) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress addr{};
    addr.mSelector = property;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;
    return addr;
}

/// @brief 读取系统级 AudioObjectID 属性（默认输出/输入设备 id）。
/// @param property 属性 fourcc（本处只取设备选择项，值型恒 AudioDeviceID）。
/// @param out 输出参数：读到的对象 id。
/// @return 读取成功且 id 非零为 true；无该设备（id==0）或属性失败为 false。
auto read_system_device(AudioObjectPropertySelector property, AudioDeviceID &out) -> bool {
    out = 0U;
    UInt32 size = sizeof(AudioDeviceID);
    const auto addr = global_address(property);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0U, nullptr, &size, &out) != noErr) {
        return false;
    }
    return out != 0U;
}

/// @brief 宿主麦克风权限声明检查：main bundle 的 Info.plist 是否含该键。
/// @return true = 宿主已声明（可安全触达采集设备）；false = 未声明或无 bundle。
auto declares_microphone_usage() -> bool {
    const CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, AURORA_MIC_USAGE_KEY, kCFStringEncodingUTF8);
    if (key == nullptr) {
        return false;
    }
    const CFBundleRef bundle = CFBundleGetMainBundle();
    const void *value = (bundle != nullptr) ? CFBundleGetValueForInfoDictionaryKey(bundle, key) : nullptr;
    CFRelease(key);
    return value != nullptr;
}

}  // namespace

// ============================================================
// CoreAudioDeviceBackend — 默认输出设备的渲染后端
// ============================================================

struct CoreAudioDeviceBackend::Impl {
    RenderFn render;  ///< start() 建队列前写好，此后只读（队列回调线程读）
    std::thread control;
    std::mutex mutex;  ///< 护队列句柄与缓冲清单（控制线程 / stop 之间）
    std::condition_variable cv;
    std::atomic<bool> running{false};  ///< stop 握手位（承载 join 义务）
    std::atomic<bool> armed{false};  ///< 队列在跑闸门：回调首行判此位，免读受 mutex 护的句柄
    std::atomic<bool> reroute{false};  ///< 设备变更事件：只置位，重建在控制线程
    std::atomic<bool> warned{false};  ///< 重路由失败的限速告警（每轮一次，免 200ms 刷屏）
    AudioQueueRef queue = nullptr;  ///< 受 mutex 保护；回调只用入参句柄，不读本字段
    std::vector<AudioQueueBufferRef> buffers;
    bool listener_attached = false;

    Impl() = default;  // 成员全 NSDMI，default 即可（声明下列拷贝/移动后须显式补回）。

    // 资源属主（std::thread + 队列句柄）语义上不可复制：显式删除拷贝/移动补齐五件套口径。
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    ~Impl() { teardown_locked(); }

    /// @brief 建队列 + 分配/预灌静音块 + AudioQueueStart（调用方须持 mutex）。
    /// @return 任一步失败即回收并返回 false。
    auto create_locked() -> bool {
        queue = nullptr;
        const AudioStreamBasicDescription fmt =
            make_pcm_format(static_cast<double>(AURORA_RENDER_RATE), static_cast<UInt32>(AURORA_RENDER_CHANNELS));
        AudioQueueRef created = nullptr;
        // 入参 runloop 传 nullptr ⇒ AudioQueue 自持一条内部线程（本后端即以此满足
        // 「设备线程周期性回调 render_block」契约，无需自起渲染线程）。
        OSStatus status = AudioQueueNewOutput(&fmt, &Impl::on_buffer, this, nullptr, kCFRunLoopCommonModes, 0U,
                                              &created);
        if (status != noErr || created == nullptr) {
            return false;
        }
        queue = created;
        const auto bytes = static_cast<UInt32>(AURORA_RENDER_BLOCK_FRAMES * AURORA_RENDER_CHANNELS) *
                           static_cast<UInt32>(sizeof(float));
        buffers.reserve(static_cast<std::size_t>(AURORA_QUEUE_BUFFER_COUNT));
        for (int index = 0; index < AURORA_QUEUE_BUFFER_COUNT; ++index) {
            AudioQueueBufferRef buffer = nullptr;
            status = AudioQueueAllocateBuffer(queue, bytes, &buffer);
            if (status != noErr || buffer == nullptr) {
                teardown_locked();
                return false;
            }
            std::memset(buffer->mAudioData, 0, bytes);
            buffer->mAudioDataByteSize = bytes;
            status = AudioQueueEnqueueBuffer(queue, buffer, 0U, nullptr);
            if (status != noErr) {
                teardown_locked();
                return false;
            }
            buffers.push_back(buffer);
        }
        status = AudioQueueStart(queue, nullptr);
        if (status != noErr) {
            // 无输出设备走此路（kAudioQueueErr_InvalidDevice）：静默降级的判据点。
            AURORA_LOG_WARN("audio",
                            "CoreAudio output start failed (status=" + describe(status) + "); no usable device?");
            teardown_locked();
            return false;
        }
        armed.store(true, std::memory_order_release);  // 开闸：此后回调才开始写块
        warned.store(false, std::memory_order_release);
        return true;
    }

    /// @brief 停队列并销毁句柄与缓冲清单（调用方须持 mutex；幂等）。
    auto teardown_locked() -> void {
        armed.store(false, std::memory_order_release);  // 先关闸：在飞回调此后只丢块不再写
        if (queue != nullptr) {
            AudioQueueStop(queue, true);  // 同步：返回时队列已停、在飞回调已尽
            AudioQueueDispose(queue, true);  // 同步：缓冲随句柄回收，buffers 集体失效
            queue = nullptr;
        }
        buffers.clear();
    }

    /// @brief 队列是否真在跑（IsRunning 兜底：设备被拔且默认设备未变时事件不到）。
    /// @return 队列存在且 IsRunning 为真值；句柄为空视为「需要重建」。
    auto is_running_locked() -> bool {
        if (queue == nullptr) {
            return false;
        }
        UInt32 value = 0U;
        UInt32 size = sizeof(value);
        if (AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &value, &size) != noErr) {
            return false;
        }
        return value != 0U;
    }

    /// @brief 控制线程主体：200ms 节拍等事件/兜底轮询，需要时重建队列。
    auto run() -> void {
        std::unique_lock<std::mutex> lock(mutex);
        while (running.load(std::memory_order_acquire)) {
            cv.wait_for(lock, std::chrono::milliseconds(AURORA_POLL_MS),
                        [this] { return !running.load(std::memory_order_acquire); });
            if (!running.load(std::memory_order_acquire)) {
                break;
            }
            const bool need_rebuild = reroute.exchange(false, std::memory_order_acq_rel) || !is_running_locked();
            if (!need_rebuild) {
                continue;
            }
            teardown_locked();
            if (create_locked()) {
                warned.store(false, std::memory_order_release);
            } else if (!warned.exchange(true, std::memory_order_acq_rel)) {
                AURORA_LOG_WARN("audio", "CoreAudio reroute pending: no output endpoint; graph clock frozen");
            }
        }
        teardown_locked();  // 线程内收尾（stop() join 后句柄已空，析构幂等）
    }

    /// @brief 队列回调（AudioQueue 自持线程）：填一块交错 float32 后把 buffer 归还。
    /// @param client Impl 地址（start 期捕获，与队列同寿）。
    /// @param in_queue 回调所属队列（直接用入参句柄，免与重路由争抢 Impl::queue）。
    /// @param buffer 待填充的队列缓冲。
    static auto on_buffer(void *client, AudioQueueRef in_queue, AudioQueueBufferRef buffer) -> void {
        auto *impl = static_cast<Impl *>(client);
        if (impl == nullptr || in_queue == nullptr || buffer == nullptr) {
            return;
        }
        const auto bytes = static_cast<UInt32>(AURORA_RENDER_BLOCK_FRAMES * AURORA_RENDER_CHANNELS) *
                           static_cast<UInt32>(sizeof(float));
        if (!impl->armed.load(std::memory_order_acquire) || buffer->mAudioDataBytesCapacity < bytes) {
            return;  // 已关闸（重路由/收尾期）或容量不足：宁可断流也不越界写
        }
        impl->render(static_cast<float *>(buffer->mAudioData), AURORA_RENDER_BLOCK_FRAMES);
        buffer->mAudioDataByteSize = bytes;
        const OSStatus status = AudioQueueEnqueueBuffer(in_queue, buffer, 0U, nullptr);
        if (status != noErr) {
            // 队列已停/重置（重路由与关闭期）：本块被丢，下一轮由 IsRunning 兜底重建。
            impl->reroute.store(true, std::memory_order_release);
        }
    }

    /// @brief 默认输出设备变更监听（CoreAudio 通知线程）：只置位，不做设备级操作。
    /// @param 设备 id 未使用（监听只关心「默认输出变了」这一事实）。
    /// @param 属性地址数未使用。
    /// @param 属性地址数组未使用。
    /// @param client Impl 地址（CoreAudio 的 clientData 在**形参末位**，与 AudioQueue 回调相反）。
    /// @return noErr（本回调不消费属性，无需返回具体状态）。
    static auto on_device_changed(AudioObjectID, UInt32, const AudioObjectPropertyAddress[], void *client)
        -> OSStatus {
        auto *impl = static_cast<Impl *>(client);
        if (impl != nullptr) {
            impl->reroute.store(true, std::memory_order_release);
        }
        return noErr;
    }
};

CoreAudioDeviceBackend::CoreAudioDeviceBackend() : impl_(std::make_unique<Impl>()) {}

CoreAudioDeviceBackend::~CoreAudioDeviceBackend() { stop(); }

auto CoreAudioDeviceBackend::format() const -> AudioDeviceFormat { return AudioDeviceFormat{}; }

auto CoreAudioDeviceBackend::start(RenderFn render_block) -> bool {
    if (impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->render = std::move(render_block);  // 必须先于建队列：Start 后回调可立即到达
    if (!impl_->create_locked()) {
        impl_->render = nullptr;
        return false;
    }
    impl_->reroute.store(false, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    // 监听在起线程前挂：事件早到只会多一次无害的重建请求。
    const auto addr = global_address(kAudioHardwarePropertyDefaultOutputDevice);
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, &Impl::on_device_changed, impl_.get()) ==
        noErr) {
        impl_->listener_attached = true;
    }
    impl_->control = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

auto CoreAudioDeviceBackend::stop() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未启动（或已停止）
    }
    impl_->cv.notify_all();
    if (impl_->control.joinable()) {
        impl_->control.join();  // 线程在 ≤ 一节拍内退出并自行 teardown_locked
    }
    if (impl_->listener_attached) {
        const auto addr = global_address(kAudioHardwarePropertyDefaultOutputDevice);
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr, &Impl::on_device_changed, impl_.get());
        impl_->listener_attached = false;
    }
}

// ============================================================
// CoreAudioCaptureBackend — 默认输入设备的采集后端
// ============================================================

struct CoreAudioCaptureBackend::Impl {
    CaptureFn capture;  ///< 同渲染端：建队列前写好，此后只读
    std::thread control;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running{false};  ///< stop 握手位（承载 join 义务），非死活判据
    std::atomic<bool> armed{false};  ///< 队列在跑闸门：回调首行判此位，免读受 mutex 护的句柄
    std::atomic<bool> device_failed{false};  ///< 中段失败（队列错误 / 设备消失）
    std::atomic<bool> stale{false};  ///< 控制线程请求位：设备消失或回调报错
    AudioQueueRef queue = nullptr;
    std::vector<AudioQueueBufferRef> buffers;
    UInt32 channels = 0U;  ///< 协商结果（回调携带）；建队列期写、关闸期不变，故回调可直读
    int rate = 0;
    bool listener_attached = false;

    Impl() = default;

    // 同渲染端 Impl：资源属主显式删除拷贝/移动，补齐五件套口径。
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    ~Impl() { teardown_locked(); }

    /// @brief 读默认输入设备的原生率与声道（Input scope 的流配置求和）。
    /// @param out_rate 输出参数：原生采样率（Hz）。
    /// @param out_channels 输出参数：输入声道数。
    /// @return 无默认输入设备 / 属性读失败 / 声道为 0 均为 false。
    auto negotiate(double &out_rate, UInt32 &out_channels) -> bool {
        AudioDeviceID device = 0U;
        if (!read_system_device(kAudioHardwarePropertyDefaultInputDevice, device)) {
            return false;
        }
        Float64 nominal = 0.0;
        UInt32 size = sizeof(nominal);
        const auto rate_addr = global_address(kAudioDevicePropertyNominalSampleRate);
        if (AudioObjectGetPropertyData(device, &rate_addr, 0U, nullptr, &size, &nominal) != noErr ||
            nominal <= 0.0) {
            return false;
        }
        AudioObjectPropertyAddress config{};
        config.mSelector = kAudioDevicePropertyStreamConfiguration;
        config.mScope = kAudioObjectPropertyScopeInput;
        config.mElement = kAudioObjectPropertyElementMain;
        UInt32 config_size = 0U;
        if (AudioObjectGetPropertyDataSize(device, &config, 0U, nullptr, &config_size) != noErr ||
            config_size < sizeof(AudioBufferList)) {
            return false;
        }
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(config_size), 0U);
        if (AudioObjectGetPropertyData(device, &config, 0U, nullptr, &config_size, raw.data()) != noErr) {
            return false;
        }
        // AudioBufferList 是 C 变长尾结构（柔性数组），取址后按头字段遍历是系统契约的唯一拼法。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *list = reinterpret_cast<const AudioBufferList *>(raw.data());
        UInt32 found_channels = 0U;
        for (UInt32 index = 0U; index < list->mNumberBuffers; ++index) {
            found_channels += list->mBuffers[index].mNumberChannels;
        }
        if (found_channels == 0U) {
            return false;  // 无输入流（例如只有输出能力的设备）
        }
        out_rate = nominal;
        out_channels = found_channels;
        return true;
    }

    /// @brief 建输入队列 + 预灌空缓冲供内核填 + Start（调用方须持 mutex）。
    /// @return 无设备 / 协商失败 / 任一步失败即回收并返回 false。
    auto create_locked() -> bool {
        queue = nullptr;
        double nominal = 0.0;
        UInt32 found_channels = 0U;
        if (!negotiate(nominal, found_channels)) {
            return false;
        }
        channels = found_channels;
        rate = static_cast<int>(nominal);
        const AudioStreamBasicDescription fmt = make_pcm_format(nominal, channels);
        const auto frames = static_cast<UInt32>(rate) / static_cast<UInt32>(1000 / AURORA_CAPTURE_BLOCK_MS);
        const UInt32 bytes = frames * fmt.mBytesPerFrame;
        AudioQueueRef created = nullptr;
        OSStatus status =
            AudioQueueNewInput(&fmt, &Impl::on_capture, this, nullptr, kCFRunLoopCommonModes, 0U, &created);
        if (status != noErr || created == nullptr) {
            return false;
        }
        queue = created;
        buffers.reserve(static_cast<std::size_t>(AURORA_QUEUE_BUFFER_COUNT));
        for (int index = 0; index < AURORA_QUEUE_BUFFER_COUNT; ++index) {
            AudioQueueBufferRef buffer = nullptr;
            status = AudioQueueAllocateBuffer(queue, bytes, &buffer);
            if (status != noErr || buffer == nullptr) {
                teardown_locked();
                return false;
            }
            status = AudioQueueEnqueueBuffer(queue, buffer, 0U, nullptr);  // 输入腿：交空缓冲待填
            if (status != noErr) {
                teardown_locked();
                return false;
            }
            buffers.push_back(buffer);
        }
        status = AudioQueueStart(queue, nullptr);
        if (status != noErr) {
            AURORA_LOG_WARN("audio",
                            "CoreAudio input start failed (status=" + describe(status) + "); no usable device?");
            teardown_locked();
            return false;
        }
        armed.store(true, std::memory_order_release);  // 开闸：此后回调才开始交付采集包
        return true;
    }

    /// @brief 停队列并销毁句柄（调用方须持 mutex；幂等）。协商位不清零——它是「本轮
    ///        队列交付什么格式」的只读事实，回收后由下一次 create_locked() 覆盖。
    auto teardown_locked() -> void {
        armed.store(false, std::memory_order_release);  // 先关闸：在飞回调此后只丢包
        if (queue != nullptr) {
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
            queue = nullptr;
        }
        buffers.clear();
    }

    /// @brief 控制线程主体：等节拍，发现队列停摆/设备消失即按中段失败止流。
    auto run() -> void {
        std::unique_lock<std::mutex> lock(mutex);
        while (running.load(std::memory_order_acquire)) {
            cv.wait_for(lock, std::chrono::milliseconds(AURORA_POLL_MS), [this] {
                return !running.load(std::memory_order_acquire) || stale.load(std::memory_order_acquire);
            });
            if (!running.load(std::memory_order_acquire)) {
                break;
            }
            bool dead = stale.exchange(false, std::memory_order_acq_rel);
            if (!dead) {
                UInt32 value = 0U;
                UInt32 size = sizeof(value);
                dead = (queue == nullptr) ||
                       AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &value, &size) != noErr ||
                       value == 0U;
            }
            if (dead) {
                break;  // 中段失败：置观察位后止流（下方统一处理）
            }
        }
        if (running.load(std::memory_order_acquire)) {
            // 线程因设备错误先行退出：置观察位。`running` 不清零——它是 stop
            // 握手位（清零会让 stop 早退不 join，~thread joinable → terminate）。
            device_failed.store(true, std::memory_order_release);
        }
        teardown_locked();
    }

    /// @brief 队列回调（AudioQueue 自持线程）：交付内核填入的 PCM 后把 buffer 归还。
    /// @param client Impl 地址。
    /// @param in_queue 回调所属队列。
    /// @param buffer 已填入的队列缓冲。
    /// @param 首帧时间戳未使用（本后端不外露采集时钟）。
    /// @param 包描述数未使用（线性 PCM 恒 0）。
    /// @param 包描述数组未使用（同上）。
    static auto on_capture(void *client, AudioQueueRef in_queue, AudioQueueBufferRef buffer, const AudioTimeStamp *,
                           UInt32, const AudioStreamPacketDescription *) -> void {
        auto *impl = static_cast<Impl *>(client);
        if (impl == nullptr || in_queue == nullptr || buffer == nullptr) {
            return;
        }
        const UInt32 bytes_per_frame = impl->channels * static_cast<UInt32>(sizeof(float));
        if (!impl->armed.load(std::memory_order_acquire) || bytes_per_frame == 0U) {
            return;  // 已关闸（stop 后的迟到回调）或协商位未就绪：丢弃本包
        }
        const auto frames = static_cast<int>(buffer->mAudioDataByteSize / bytes_per_frame);
        if (frames > 0) {
            impl->capture(static_cast<const float *>(buffer->mAudioData), frames, impl->rate,
                          static_cast<int>(impl->channels));
        }
        const OSStatus status = AudioQueueEnqueueBuffer(in_queue, buffer, 0U, nullptr);
        if (status != noErr) {
            impl->stale.store(true, std::memory_order_release);  // 队列已停：交控制线程判定止流
        }
    }

    /// @brief 默认输入设备变更监听：置 stale，由控制线程按中段失败处理（采集不自动重开）。
    /// @param 设备 id 未使用。
    /// @param 属性地址数未使用。
    /// @param 属性地址数组未使用。
    /// @param client Impl 地址（同渲染端：clientData 在形参末位）。
    /// @return noErr。
    static auto on_input_device_changed(AudioObjectID, UInt32, const AudioObjectPropertyAddress[], void *client)
        -> OSStatus {
        auto *impl = static_cast<Impl *>(client);
        if (impl != nullptr) {
            impl->stale.store(true, std::memory_order_release);
        }
        return noErr;
    }
};

CoreAudioCaptureBackend::CoreAudioCaptureBackend() : impl_(std::make_unique<Impl>()) {}

CoreAudioCaptureBackend::~CoreAudioCaptureBackend() { stop(); }

auto CoreAudioCaptureBackend::start(CaptureFn on_pcm) -> bool {
    if (impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    if (!declares_microphone_usage()) {
        // 未声明就触达麦克风 = 系统终止进程；拒启把风险变成可读的显式失败。
        AURORA_LOG_WARN("audio", "host bundle does not declare NSMicrophoneUsageDescription; capture refused");
        return false;
    }
    impl_->capture = std::move(on_pcm);
    impl_->device_failed.store(false, std::memory_order_relaxed);  // 重开清旧败
    impl_->stale.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->create_locked()) {
        impl_->teardown_locked();
        impl_->capture = nullptr;
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    const auto addr = global_address(kAudioHardwarePropertyDefaultInputDevice);
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, &Impl::on_input_device_changed,
                                       impl_.get()) == noErr) {
        impl_->listener_attached = true;
    }
    impl_->control = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

auto CoreAudioCaptureBackend::stop() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未启动（或已停止）
    }
    impl_->cv.notify_all();
    if (impl_->control.joinable()) {
        impl_->control.join();
    }
    if (impl_->listener_attached) {
        const auto addr = global_address(kAudioHardwarePropertyDefaultInputDevice);
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr, &Impl::on_input_device_changed,
                                          impl_.get());
        impl_->listener_attached = false;
    }
}

auto CoreAudioCaptureBackend::failed() const -> bool {
    return impl_->device_failed.load(std::memory_order_acquire);
}

}  // namespace aurora

#else  // !AURORA_ENABLE_AUDIO_COREAUDIO —— disabled 桩：start 恒 false → 静默模式

#include <utility>

namespace aurora {

struct CoreAudioDeviceBackend::Impl {};  // 桩无状态（pimpl 完整定义供 ctor/dtor 实例化）

CoreAudioDeviceBackend::CoreAudioDeviceBackend() = default;

CoreAudioDeviceBackend::~CoreAudioDeviceBackend() = default;

auto CoreAudioDeviceBackend::format() const -> AudioDeviceFormat { return AudioDeviceFormat{}; }

auto CoreAudioDeviceBackend::start(RenderFn /*render_block*/) -> bool { return false; }

auto CoreAudioDeviceBackend::stop() -> void {}

struct CoreAudioCaptureBackend::Impl {};  // 桩无状态（start 恒 false → 录制显式报错）

CoreAudioCaptureBackend::CoreAudioCaptureBackend() = default;

CoreAudioCaptureBackend::~CoreAudioCaptureBackend() = default;

auto CoreAudioCaptureBackend::start(CaptureFn /*on_pcm*/) -> bool { return false; }

auto CoreAudioCaptureBackend::stop() -> void {}

auto CoreAudioCaptureBackend::failed() const -> bool { return false; }  // 桩无采集线程

}  // namespace aurora

#endif  // AURORA_ENABLE_AUDIO_COREAUDIO
