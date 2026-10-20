// Win32 OLE 拖入桥实现（契约与设计理由见同名头文件）。
//
// 本 TU 只在 Windows 且后端为 Win32 / D3D11 / WGPU 时编译：门控写在同名头文件里。

#include "win32_drag.h"

// 本 TU 的头文件已按「平台宏 ∧ 后端宏析取」门控（非 Windows 时头文件为空），故实现体须再显式
// 门控一次：源码走 GLOB，非 Windows 构建同样会把本文件编进来（与 `win32_ime.cpp` 同款写法）。
#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#include <shellapi.h>  // DragQueryFileW（HDROP 解析）

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/log.h"
#include "aurora/core/utf8.h"
#include "aurora/event/detail/system_drag_ingress.h"
#include "aurora/window/detail/system_drag_translate.h"

namespace aurora::detail {

// STGMEDIUM 的 tymed / hGlobal 是 Win32 协议固有的 tagged union 字段：本 TU 只能按协议判别后取用，
// 无 variant 可替（该结构是 OS ABI 的一部分，不得改写）。
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
namespace {

// 首批可兑现的剪贴板格式：**formats 清单声明即能兑现**，故只列本桥真正能取到字节的格式。
// `CF_DIBV5` → `image/png` 需要 DIBV5 → PNG 转码（且受 `AURORA_ENABLE_IMAGE_PNG` 门控），
// 落地前不列——声明了却取不到等于谎报。折算层的映射表已备好该档，转码落地后加入本数组即可。
constexpr std::uint16_t AURORA_DROP_IN_CLIP_FORMATS[] = {
    static_cast<std::uint16_t>(drag::Win32ClipFormat::Hdrop),
    static_cast<std::uint16_t>(drag::Win32ClipFormat::UnicodeText),
};

/// @brief 枚举数据对象上的可用格式（只取本桥能兑现的那几档，顺序按数组声明 = 优先级）。
/// @param data 数据对象；为 nullptr 时返回空清单。
/// @return 格式数值清单。
[[nodiscard]] auto supported_formats_of(IDataObject *data) -> std::vector<std::uint16_t> {
    std::vector<std::uint16_t> out;
    if (data == nullptr) {
        return out;
    }
    for (const std::uint16_t candidate : AURORA_DROP_IN_CLIP_FORMATS) {
        FORMATETC requested{};
        requested.cfFormat = static_cast<CLIPFORMAT>(candidate);
        requested.ptd = nullptr;
        requested.dwAspect = DVASPECT_CONTENT;
        requested.lindex = -1;
        requested.tymed = TYMED_HGLOBAL;
        // QueryGetData 只问「给不给得出」而不触发延迟渲染：Enter / Over 态按指针采样频率到达，
        // 用 GetData 会把源侧的真实取数提前到悬停期（跨进程 IO 打在每一帧上）。
        if (data->QueryGetData(&requested) == S_OK) {
            out.push_back(candidate);
        }
    }
    return out;
}

/// @brief 解锁并释放介质（与 `GlobalLock` 配对后交回源侧）。
///
/// 释放一律走 `ReleaseStgMedium`：介质所有权可能在源侧，直接 `GlobalFree` 会双重释放。
/// @param medium 待释放的介质（已 `GlobalLock` 过）。
auto release_locked_medium(STGMEDIUM &medium) -> void {
    if (medium.hGlobal != nullptr) {
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
}

/// @brief 读文件清单（`CF_HDROP` → uri-list 文本）。
/// @param data 数据对象。
/// @return uri-list 文本；无该格式或解析不出路径时为 `std::nullopt`。
[[nodiscard]] auto read_uri_list(IDataObject *data) -> std::optional<std::string> {
    FORMATETC requested{};
    requested.cfFormat = static_cast<CLIPFORMAT>(drag::Win32ClipFormat::Hdrop);
    requested.ptd = nullptr;
    requested.dwAspect = DVASPECT_CONTENT;
    requested.lindex = -1;
    requested.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&requested, &medium)) || medium.hGlobal == nullptr) {
        if (medium.hGlobal != nullptr) {
            ReleaseStgMedium(&medium);
        }
        return std::nullopt;
    }
    // HDROP 本身即 HGLOBAL 句柄：直接按协议使用，**不** GlobalLock（配对语义不同，
    // 误锁会让 ReleaseStgMedium 之前的解锁次数对不上）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) HDROP 就是 HGLOBAL 的别名
    auto *const hdrop = reinterpret_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFFU, nullptr, 0U);
    std::vector<std::string> paths;
    paths.reserve(count);
    for (UINT i = 0U; i < count; ++i) {
        const UINT len = DragQueryFileW(hdrop, i, nullptr, 0U);
        std::wstring wide(static_cast<std::size_t>(len) + 1U, L'\0');
        if (DragQueryFileW(hdrop, i, wide.data(), len + 1U) == 0U) {
            continue;
        }
        paths.push_back(internal::wstr_to_utf8(wide.c_str()));
    }
    ReleaseStgMedium(&medium);
    if (paths.empty()) {
        return std::nullopt;
    }
    return uri_list::encode(paths);
}

/// @brief 读 Unicode 文本（`CF_UNICODETEXT` → UTF-8）。
/// @param data 数据对象。
/// @return UTF-8 文本；无该格式时为 `std::nullopt`。
[[nodiscard]] auto read_unicode_text(IDataObject *data) -> std::optional<std::string> {
    FORMATETC requested{};
    requested.cfFormat = static_cast<CLIPFORMAT>(drag::Win32ClipFormat::UnicodeText);
    requested.ptd = nullptr;
    requested.dwAspect = DVASPECT_CONTENT;
    requested.lindex = -1;
    requested.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&requested, &medium)) || medium.hGlobal == nullptr) {
        if (medium.hGlobal != nullptr) {
            ReleaseStgMedium(&medium);
        }
        return std::nullopt;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) 全局内存块锁出的是 UTF-16 串
    const auto *const wide = reinterpret_cast<const wchar_t *>(GlobalLock(medium.hGlobal));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    std::optional<std::string> out;
    if (wide != nullptr) {
        out = internal::wstr_to_utf8(wide);
    }
    release_locked_medium(medium);
    return out;
}

/// @brief 同步取全部可兑现格式的字节（首批：小载荷同步路径）。
///
/// 大载荷异步通道（`read_async` + `pump_reads`）属后续批次；阈值常量已在折算层锚定
/// （`drag::AURORA_DRAG_SYNC_PAYLOAD_LIMIT`），届时按它分叉——本函数不另设第二个数。
/// @param data 数据对象。
/// @return 载荷句柄（无任何可取格式时返回空表构造的句柄，读取一律 `Ok` 空值，不谎报有数据）。
[[nodiscard]] auto payload_from(IDataObject *data) -> std::shared_ptr<const DragPayload> {
    std::unordered_map<std::string, std::vector<std::uint8_t>> entries;
    if (data != nullptr) {
        if (const auto uri_text = read_uri_list(data); uri_text.has_value()) {
            entries.emplace(std::string{AURORA_MIME_URI_LIST},
                            std::vector<std::uint8_t>(uri_text->begin(), uri_text->end()));
        }
        if (const auto text = read_unicode_text(data); text.has_value()) {
            entries.emplace(std::string{AURORA_MIME_TEXT_PLAIN}, std::vector<std::uint8_t>(text->begin(), text->end()));
        }
    }
    return DragPayload::from_bytes(std::move(entries));
}

}  // namespace
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

// ---- COM `IDropTarget` 实现体 ----

/// @brief OLE 拖入回调 → `SystemDropEvent` 四态（实现体不进公共头，也不进本桥头文件）。
class Win32DragBridge::DropTargetImpl final : public IDropTarget {
  public:
    explicit DropTargetImpl(Win32DragBridge *owner) : owner_(owner) {}

    // IUnknown
    auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) -> HRESULT override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget)) {
            *object = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override { return ++ref_; }

    auto STDMETHODCALLTYPE Release() -> ULONG override {
        // **不自删**：所有权归桥。OS 侧把计数放到 0 之后再回调会让「自删式实现」留下悬空指针；
        // 本桥的生命周期由 `Win32Host::Impl` 掌管，与 COM 计数解耦是刻意的（同 WebView2
        // `ComHandler::ref_` 的处置纪律）。
        if (ref_ > 0U) {
            --ref_;
        }
        return ref_;
    }

    // IDropTarget
    auto STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD key_state, POINTL pt, DWORD *effect) -> HRESULT override;
    auto STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL pt, DWORD *effect) -> HRESULT override;
    auto STDMETHODCALLTYPE DragLeave() -> HRESULT override;
    auto STDMETHODCALLTYPE Drop(IDataObject *data, DWORD key_state, POINTL pt, DWORD *effect) -> HRESULT override;

  private:
    /// @brief 登记 / 替换会话期内持有的数据对象（带引用计数转移）。
    auto hold(IDataObject *data) -> void;
    /// @brief 释放会话期内持有的数据对象。
    auto release_held() -> void;

    Win32DragBridge *owner_;  ///< 非拥有；生命周期由 `Win32Host::Impl` 掌管
    ULONG ref_ = 1U;  ///< COM 引用计数（起 1：桥自身持一份）
};

auto Win32DragBridge::DropTargetImpl::hold(IDataObject *data) -> void {
    release_held();
    owner_->data_ = data;
    if (data != nullptr) {
        data->AddRef();
    }
}

auto Win32DragBridge::DropTargetImpl::release_held() -> void {
    if (owner_->data_ != nullptr) {
        owner_->data_->Release();
        owner_->data_ = nullptr;
    }
}

auto STDMETHODCALLTYPE Win32DragBridge::DropTargetImpl::DragEnter(IDataObject *data, DWORD key_state, POINTL pt,
                                                                  DWORD *effect) -> HRESULT {
    if (effect == nullptr) {
        return E_POINTER;
    }
    hold(data);
    const DragAction allowed = owner_->dispatch_phase(SystemDropPhase::Enter, data, key_state, pt,
                                                      drag::action_from_win32_drop_effect(*effect));
    // 光标反馈回写：allowed 由派发器按 accept 查询与 proposed 求交后写定，本桥只做「契约值 →
    // OS 位」的最后一跳（折算层负责，不在本桥重写位序）。
    *effect = drag::win32_drop_effect_from_action(allowed);
    return S_OK;
}

auto STDMETHODCALLTYPE Win32DragBridge::DropTargetImpl::DragOver(DWORD key_state, POINTL pt, DWORD *effect) -> HRESULT {
    if (effect == nullptr) {
        return E_POINTER;
    }
    const DragAction allowed = owner_->dispatch_phase(SystemDropPhase::Over, owner_->data_, key_state, pt,
                                                      drag::action_from_win32_drop_effect(*effect));
    *effect = drag::win32_drop_effect_from_action(allowed);
    return S_OK;
}

auto STDMETHODCALLTYPE Win32DragBridge::DropTargetImpl::DragLeave() -> HRESULT {
    // Leave 不携带数据与坐标：派发一条纯 Leave 事件即可（控件侧的自动滚动收车由派发器处理）。
    owner_->dispatch_phase(SystemDropPhase::Leave, nullptr, 0U, POINTL{.x = 0, .y = 0}, DragAction::None);
    release_held();
    return S_OK;
}

auto STDMETHODCALLTYPE Win32DragBridge::DropTargetImpl::Drop(IDataObject *data, DWORD key_state, POINTL pt,
                                                             DWORD *effect) -> HRESULT {
    if (effect == nullptr) {
        return E_POINTER;
    }
    // OLE 保证 Drop 携带数据对象；会话期内登记的那一份作为兜底（某些源侧传 nullptr）。
    IDataObject *const effective = (data != nullptr) ? data : owner_->data_;
    const DragAction allowed = owner_->dispatch_phase(SystemDropPhase::Drop, effective, key_state, pt,
                                                      drag::action_from_win32_drop_effect(*effect));
    *effect = drag::win32_drop_effect_from_action(allowed);
    release_held();
    return S_OK;
}

// ---- 桥本体 ----

Win32DragBridge::Win32DragBridge(const HWND hwnd, Hooks hooks) : hwnd_(hwnd), hooks_(std::move(hooks)) {}

Win32DragBridge::~Win32DragBridge() { uninstall(); }

auto Win32DragBridge::install() -> bool {
    if (installed_ || hwnd_ == nullptr) {
        return false;
    }
    // OLE 初始化：`RegisterDragDrop` / `DoDragDrop` 要求线程已有 COM 单元，`CoInitializeEx`
    // 不等价（缺 OLE 的剪贴板 / 拖放子系统）。返回值 S_FALSE = 本线程此前已初始化过，同样可用。
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) {
        AURORA_LOG_WARN("system-drag", "OleInitialize failed: hr=0x%08lX", static_cast<unsigned long>(ole));
        return false;
    }
    ole_owned_ = true;
    target_ = std::make_unique<DropTargetImpl>(this);
    const HRESULT reg = RegisterDragDrop(hwnd_, target_.get());
    if (FAILED(reg)) {
        AURORA_LOG_WARN("system-drag", "RegisterDragDrop failed: hr=0x%08lX", static_cast<unsigned long>(reg));
        target_.reset();
        OleUninitialize();
        ole_owned_ = false;
        return false;
    }
    installed_ = true;
    return true;
}

auto Win32DragBridge::uninstall() -> void {
    if (installed_) {
        RevokeDragDrop(hwnd_);
        installed_ = false;
    }
    reset();
    if (ole_owned_) {
        OleUninitialize();
        ole_owned_ = false;
    }
}

auto Win32DragBridge::reset() -> void {
    if (data_ != nullptr) {
        data_->Release();
        data_ = nullptr;
    }
}

auto Win32DragBridge::dispatch_phase(const SystemDropPhase phase, IDataObject *data, const DWORD key_state,
                                     const POINTL pt, const DragAction os_suggested) -> DragAction {
    POINT client{.x = pt.x, .y = pt.y};
    ScreenToClient(hwnd_, &client);

    drag::DragInFrame frame;
    frame.clip_formats = supported_formats_of(data);
    frame.modifiers = drag::modifiers_from_win32_mk_flags(static_cast<std::uint32_t>(key_state));
    frame.os_suggested = os_suggested;
    frame.client_x = client.x;
    frame.client_y = client.y;
    frame.scale = hooks_.scale_factor != nullptr ? hooks_.scale_factor() : 1.0F;

    SystemDropEvent event = drag::make_drop_event(phase, frame);
    // 载荷只在 Drop 态装配：Enter / Over 按指针采样频率到达，在悬停期触发跨进程取数会把 IO 打进每一帧。
    if (phase == SystemDropPhase::Drop) {
        event.payload = payload_from(data);
    }
    // 投事件的唯一入口（`system_drag::ingress`）：inert 拖入后端接管期间本派发被抑制，
    // 于是测试能完整接管拖入链而不被真机后端叠加干扰。
    return system_drag::ingress::dispatch(event, [this](SystemDropEvent &e) -> DragAction {
        if (hooks_.emit != nullptr) {
            hooks_.emit(e);  // 派发器在调用内写定 e.allowed
        }
        return e.allowed;
    });
}

}  // namespace aurora::detail

#endif  // 平台 ∧ 后端门控（与文件首同款）
