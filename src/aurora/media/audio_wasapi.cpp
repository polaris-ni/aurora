// ============================================================
// media/audio_wasapi.cpp — WASAPI 音频设备后端实现
// ------------------------------------------------------------
// shared mode + event-driven：引擎按周期触发事件，设备线程逐块
// GetCurrentPadding → render_block → GetBuffer/ReleaseBuffer。
// 格式协商：优先以图契约格式（48000/2 float32）+ AUTOCONVERTPCM 初始化
// （引擎侧重采样/声道转换，重路由后契约稳定）；旧系统回退混合格式直用。
// 设备丢失/默认设备变更：IMMNotificationClient 置重路由标志，设备线程
// 重建端点客户端（重试退避 200ms），期间图时钟冻结（对齐 suspend 语义）。
// ============================================================

#include "audio_wasapi.h"

#include <atomic>

#ifdef AURORA_ENABLE_AUDIO_WASAPI

#ifndef WIN32_LEAN_AND_MEAN
// Windows SDK 规定的宏名，无法冠 AURORA_ 前缀满足命名表，只能就地豁免
// NOLINTNEXTLINE(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <chrono>
#include <thread>
#include <utility>

namespace aurora {

namespace {

constexpr REFERENCE_TIME AURORA_AUDIO_WASAPI_BUFFER_DURATION_HNS = 200000;  // 引擎缓冲 20ms（100ns 单位）
constexpr DWORD AURORA_AUDIO_WASAPI_EVENT_TIMEOUT_MS = 2000;  // 事件等待超时（设备暂停时不空转推进）
constexpr int AURORA_AUDIO_WASAPI_RENDER_RATE = 48000;
constexpr int AURORA_AUDIO_WASAPI_RENDER_CHANNELS = 2;

// WAVE_FORMAT_EXTENSIBLE 的 IEEE float32 子格式 GUID（避免引入 ksmedia.h）。
constexpr GUID AURORA_AUDIO_WASAPI_IEEE_FLOAT_SUB_FORMAT = {
    .Data1 = 0x00000003, .Data2 = 0x0000, .Data3 = 0x0010, .Data4 = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

auto is_float_format(const WAVEFORMATEX *f) -> bool {
    if (f == nullptr) {
        return false;
    }
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // Win32 约定：WAVE_FORMAT_EXTENSIBLE 的格式块前导即 WAVEFORMATEX，向下强转读子格式是唯一取法
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
        return IsEqualGUID(ext->SubFormat, AURORA_AUDIO_WASAPI_IEEE_FLOAT_SUB_FORMAT) != 0;
    }
    return false;
}

/// IMMNotificationClient 最小实现：默认渲染设备变更/任意设备状态变化 → 置重路由标志。
/// 标准 COM 自毁引用计数：new 携带 Impl 的一份自有引用（ref=1），注册期若 COM 内部
/// 另有持有由协议自行 AddRef/Release；`teardown_all` 注销后 Release 掉自有引用，
/// 归零即 `delete this`——此前 Release 不归零删除且全链无 delete，对象每次后端生命
/// 周期泄漏一份（审计缺陷 ①）。
class DeviceNotifyClient final : public IMMNotificationClient {
  public:
    explicit DeviceNotifyClient(std::atomic<bool> &reroute) : reroute_(reroute) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) {
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*device_id*/, DWORD /*new_state*/) override {
        reroute_.store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*device_id*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*device_id*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/, LPCWSTR /*device_id*/) override {
        if (flow == eRender) {
            reroute_.store(true, std::memory_order_release);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*device_id*/, const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

  private:
    std::atomic<bool> &reroute_;
    std::atomic<ULONG> ref_{1};
};

}  // namespace

struct WasapiDeviceBackend::Impl {
    RenderFn render;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> reroute{false};
    bool com_owned = false;  // CoInitializeEx 成功（S_OK/S_FALSE）→ 须配对 CoUninitialize

    // COM 接口（仅在 start/stop 与设备线程触达；render 线程为 MTA，接口方法线程安全）
    IMMDeviceEnumerator *enumerator = nullptr;
    DeviceNotifyClient *notify = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render_client = nullptr;
    UINT32 buffer_frames = 0;

    Impl() = default;
    ~Impl() { teardown_all(); }
    // pimpl 持有 COM 接口指针 / 事件句柄 / 设备线程的所有权，拷贝或移动即两份对象争着
    // Release/CloseHandle/join，按 Rule of Five 显式禁用（thread/atomic 本已隐式禁用，此处显式化）。
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    auto init_com() -> bool {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            com_owned = true;
            return true;
        }
        return hr == RPC_E_CHANGED_MODE;  // 已以 STA 初始化：可用但不得配对反初始化
    }

    auto com_uninit() -> void {
        if (com_owned) {
            CoUninitialize();
            com_owned = false;
        }
    }

    auto create_enumerator() -> bool {
        if (enumerator != nullptr) {
            return true;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 COM 边界——CoCreateInstance
        // 出参只接受 void**，接口指针地址只能 reinterpret 传递
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(&enumerator))) ||
            enumerator == nullptr) {
            return false;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        notify = new DeviceNotifyClient(reroute);
        enumerator->RegisterEndpointNotificationCallback(notify);
        return true;
    }

    /// 以图契约格式初始化引擎客户端；AUTOCONVERTPCM 失败（旧系统）时回退混合格式直用。
    auto init_client() -> bool {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = static_cast<WORD>(AURORA_AUDIO_WASAPI_RENDER_CHANNELS);
        fmt.nSamplesPerSec = static_cast<DWORD>(AURORA_AUDIO_WASAPI_RENDER_RATE);
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = static_cast<WORD>(AURORA_AUDIO_WASAPI_RENDER_CHANNELS * 32 / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                        AURORA_AUDIO_WASAPI_BUFFER_DURATION_HNS, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            WAVEFORMATEX *mix = nullptr;
            if (FAILED(client->GetMixFormat(&mix))) {
                return false;
            }
            const bool usable = is_float_format(mix) && mix->nChannels == AURORA_AUDIO_WASAPI_RENDER_CHANNELS;
            if (usable) {
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        AURORA_AUDIO_WASAPI_BUFFER_DURATION_HNS, 0, mix, nullptr);
            }
            CoTaskMemFree(mix);
            if (!usable || FAILED(hr)) {
                return false;
            }
        }
        if (FAILED(client->SetEventHandle(event))) {
            return false;
        }
        if (FAILED(client->GetBufferSize(&buffer_frames)) || buffer_frames == 0) {
            return false;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 COM 边界——GetService
        // 出参只接受 void**，接口指针地址只能 reinterpret 传递
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render_client))) ||
            render_client == nullptr) {
            return false;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        // 预滚：一次性取满缓冲再空还，重置引擎写指针。
        BYTE *p = nullptr;
        if (SUCCEEDED(render_client->GetBuffer(buffer_frames, &p)) && p != nullptr) {
            render_client->ReleaseBuffer(0, 0);
        }
        return true;
    }

    /// 激活当前默认渲染端点并初始化引擎客户端（重路由共用入口）。
    auto activate_default_device() -> bool {
        if (!create_enumerator()) {
            return false;
        }
        teardown_client();
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || device == nullptr) {
            return false;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 COM 边界——Activate
        // 出参只接受 void**，接口指针地址只能 reinterpret 传递
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) ||
            client == nullptr) {
            return false;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        return init_client();
    }

    auto teardown_client() -> void {
        if (client != nullptr) {
            client->Stop();
            client->Release();
            client = nullptr;
        }
        if (render_client != nullptr) {
            render_client->Release();
            render_client = nullptr;
        }
        if (device != nullptr) {
            device->Release();
            device = nullptr;
        }
        buffer_frames = 0;
    }

    auto teardown_all() -> void {
        teardown_client();
        if (enumerator != nullptr) {
            if (notify != nullptr) {
                enumerator->UnregisterEndpointNotificationCallback(notify);
                notify->Release();
                notify = nullptr;
            }
            enumerator->Release();
            enumerator = nullptr;
        }
    }

    /// 渲染一轮：按引擎余量取缓冲、调图渲染、提交。失败返回 false（触发重路由）。
    [[nodiscard]] auto render_pass() const -> bool {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            return false;
        }
        UINT32 avail = buffer_frames > padding ? buffer_frames - padding : 0;
        if (avail == 0) {
            return true;
        }
        BYTE *p = nullptr;
        if (FAILED(render_client->GetBuffer(avail, &p)) || p == nullptr) {
            return false;
        }
        // 渲染回调契约为 float32 交织缓冲，引擎给的 BYTE* 只能 reinterpret 为 float*（确定单行，用 NEXTLINE）
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        render(reinterpret_cast<float *>(p), static_cast<int>(avail));
        return SUCCEEDED(render_client->ReleaseBuffer(avail, 0));
    }

    auto run() -> void {
        // 设备线程独立 MTA 初始化（与启动线程状态解耦）。
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool thread_com = SUCCEEDED(hr);
        while (running.load(std::memory_order_acquire)) {
            if (reroute.exchange(false, std::memory_order_acq_rel)) {
                if (!activate_default_device()) {
                    // 设备不可用：退避重试；期间时钟冻结（对齐 suspend 语义）
                    reroute.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
            }
            const DWORD wait = WaitForSingleObject(event, AURORA_AUDIO_WASAPI_EVENT_TIMEOUT_MS);
            if (!running.load(std::memory_order_acquire)) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;  // 超时（设备暂停/引擎空转）——不空推时钟
            }
            if (client == nullptr || !render_pass()) {
                reroute.store(true, std::memory_order_release);
            }
        }
        if (thread_com) {
            CoUninitialize();
        }
    }
};

WasapiDeviceBackend::WasapiDeviceBackend() : impl_(std::make_unique<Impl>()) {}

WasapiDeviceBackend::~WasapiDeviceBackend() { stop(); }

auto WasapiDeviceBackend::format() const -> AudioDeviceFormat { return AudioDeviceFormat{}; }

auto WasapiDeviceBackend::start(RenderFn render_block) -> bool {
    if (impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    if (!impl_->init_com()) {
        return false;
    }
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->event == nullptr) {
        impl_->com_uninit();
        return false;
    }
    impl_->render = std::move(render_block);
    // 失败路径统一回收 event（审计缺陷 ②：此前只 stop() 成功路径关句柄，
    // start 半途失败即漏一个事件句柄，重试 start 会再建一个覆盖旧值）。
    if (!impl_->create_enumerator() || !impl_->activate_default_device()) {
        impl_->teardown_all();
        impl_->com_uninit();
        CloseHandle(impl_->event);
        impl_->event = nullptr;
        return false;
    }
    if (FAILED(impl_->client->Start())) {
        impl_->teardown_all();
        impl_->com_uninit();
        CloseHandle(impl_->event);
        impl_->event = nullptr;
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

auto WasapiDeviceBackend::stop() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未启动（或已停止）
    }
    SetEvent(impl_->event);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->teardown_all();
    if (impl_->event != nullptr) {
        CloseHandle(impl_->event);
        impl_->event = nullptr;
    }
    impl_->com_uninit();
}

// ============================================================
// WasapiCaptureBackend — 默认捕获端点采集后端
// ============================================================

struct WasapiCaptureBackend::Impl {
    CaptureFn capture;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> device_failed{false};  ///< 中段设备失败（线程已退出、回调止流）
    bool com_owned = false;

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture_client = nullptr;
    UINT32 rate = 0;
    int channels = 0;

    Impl() = default;
    ~Impl() { teardown_all(); }
    // pimpl 持有 COM 接口指针 / 事件句柄 / 采集线程的所有权，拷贝或移动即两份对象争着
    // Release/CloseHandle/join，按 Rule of Five 显式禁用（thread/atomic 本已隐式禁用，此处显式化）。
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    auto init_com() -> bool {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            com_owned = true;
            return true;
        }
        return hr == RPC_E_CHANGED_MODE;
    }

    auto com_uninit() -> void {
        if (com_owned) {
            CoUninitialize();
            com_owned = false;
        }
    }

    /// 激活默认捕获端点（shared 引擎混合格式恒 float32；非 float 视为不可用）。
    auto activate() -> bool {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 COM 边界——CoCreateInstance/
        // Activate/GetService 出参只接受 void**，接口指针地址只能 reinterpret 传递
        if (enumerator == nullptr &&
            FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(&enumerator)))) {
            return false;
        }
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) || device == nullptr) {
            return false;
        }
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) ||
            client == nullptr) {
            return false;
        }
        WAVEFORMATEX *fmt = nullptr;
        if (FAILED(client->GetMixFormat(&fmt)) || fmt == nullptr) {
            return false;
        }
        const bool float_ok = is_float_format(fmt);
        rate = float_ok ? fmt->nSamplesPerSec : 0;
        channels = float_ok ? static_cast<int>(fmt->nChannels) : 0;
        const bool usable = float_ok && (channels == 1 || channels == 2) && rate > 0;
        HRESULT hr = E_FAIL;
        if (usable) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    AURORA_AUDIO_WASAPI_BUFFER_DURATION_HNS, 0, fmt, nullptr);
        }
        CoTaskMemFree(fmt);
        if (!usable || FAILED(hr)) {
            return false;
        }
        if (FAILED(client->SetEventHandle(event))) {
            return false;
        }
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture_client))) ||
            capture_client == nullptr) {
            return false;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        return SUCCEEDED(client->Start());
    }

    auto teardown_all() -> void {
        if (client != nullptr) {
            client->Stop();
            client->Release();
            client = nullptr;
        }
        if (capture_client != nullptr) {
            capture_client->Release();
            capture_client = nullptr;
        }
        if (device != nullptr) {
            device->Release();
            device = nullptr;
        }
        if (enumerator != nullptr) {
            enumerator->Release();
            enumerator = nullptr;
        }
        rate = 0;
        channels = 0;
    }

    /// 排空当前可用包（GetNextPacketSize/GetBuffer 循环）；SILENT 包补零回调。
    auto pump() -> bool {
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture_client->GetNextPacketSize(&packet)) || packet == 0) {
                return true;
            }
            BYTE *p = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture_client->GetBuffer(&p, &frames, &flags, nullptr, nullptr))) {
                return false;
            }
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Win32 采集缓冲是字节指针，
            // 混合格式恒 float32，只能 reinterpret 为 const float* 交给回调
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                silent.assign(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels), 0.0F);
                capture(silent.data(), static_cast<int>(frames), static_cast<int>(rate), channels);
            } else if (p != nullptr) {
                capture(reinterpret_cast<const float *>(p), static_cast<int>(frames), static_cast<int>(rate), channels);
            }
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
            if (FAILED(capture_client->ReleaseBuffer(frames))) {
                return false;
            }
        }
    }

    auto run() -> void {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool thread_com = SUCCEEDED(hr);
        while (running.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(event, AURORA_AUDIO_WASAPI_EVENT_TIMEOUT_MS);
            if (!running.load(std::memory_order_acquire)) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;  // 超时（无新包）——空转重试
            }
            if (capture_client == nullptr || !pump()) {
                // 设备失败：置观察位后线程退出（`running` 留作 stop 握手位——
                // 若在此清零，stop 早退不 join，~thread joinable → std::terminate）。
                device_failed.store(true, std::memory_order_release);
                break;
            }
        }
        if (thread_com) {
            CoUninitialize();
        }
    }

    std::vector<float> silent;  // SILENT 包补零暂存（采集线程私有；struct 公有成员不带 `_` 后缀，见命名表）
};

WasapiCaptureBackend::WasapiCaptureBackend() : impl_(std::make_unique<Impl>()) {}

WasapiCaptureBackend::~WasapiCaptureBackend() { stop(); }

auto WasapiCaptureBackend::start(CaptureFn on_pcm) -> bool {
    if (impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    if (!impl_->init_com()) {
        return false;
    }
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->event == nullptr) {
        impl_->com_uninit();
        return false;
    }
    impl_->capture = std::move(on_pcm);
    impl_->device_failed.store(false, std::memory_order_relaxed);  // 重开清旧败
    // 失败路径统一回收 event（同设备后端审计缺陷 ②）。
    if (!impl_->activate()) {
        impl_->teardown_all();
        impl_->com_uninit();
        CloseHandle(impl_->event);
        impl_->event = nullptr;
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

auto WasapiCaptureBackend::stop() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未启动（或已停止）
    }
    SetEvent(impl_->event);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->teardown_all();
    if (impl_->event != nullptr) {
        CloseHandle(impl_->event);
        impl_->event = nullptr;
    }
    impl_->com_uninit();
}

auto WasapiCaptureBackend::failed() const -> bool { return impl_->device_failed.load(std::memory_order_acquire); }

}  // namespace aurora

#else  // !AURORA_ENABLE_AUDIO_WASAPI —— disabled 桩：start 恒 false → 静默模式

#include <utility>

namespace aurora {

struct WasapiDeviceBackend::Impl {};  // 桩无状态（pimpl 完整定义供 ctor/dtor 实例化）

WasapiDeviceBackend::WasapiDeviceBackend() = default;

WasapiDeviceBackend::~WasapiDeviceBackend() = default;

auto WasapiDeviceBackend::format() const -> AudioDeviceFormat { return AudioDeviceFormat{}; }

auto WasapiDeviceBackend::start(RenderFn /*render_block*/) -> bool { return false; }

auto WasapiDeviceBackend::stop() -> void {}

struct WasapiCaptureBackend::Impl {};  // 桩无状态（start 恒 false → 录制显式报错）

WasapiCaptureBackend::WasapiCaptureBackend() = default;

WasapiCaptureBackend::~WasapiCaptureBackend() = default;

auto WasapiCaptureBackend::start(CaptureFn /*on_pcm*/) -> bool { return false; }

auto WasapiCaptureBackend::stop() -> void {}

auto WasapiCaptureBackend::failed() const -> bool { return false; }  // 桩无采集线程

}  // namespace aurora

#endif  // AURORA_ENABLE_AUDIO_WASAPI
