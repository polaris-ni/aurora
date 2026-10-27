// Win32 OLE 拖入桥实现（契约与设计理由见同名头文件）。
//
// 本 TU 只在 Windows 且后端为 Win32 / D3D11 / WGPU 时编译：门控写在同名头文件里。

#include "win32_drag.h"

// 本 TU 的头文件已按「平台宏 ∧ 后端宏析取」门控（非 Windows 时头文件为空），故实现体须再显式
// 门控一次：源码走 GLOB，非 Windows 构建同样会把本文件编进来（与 `win32_ime.cpp` 同款写法）。
#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#include <shellapi.h>  // DragQueryFileW（HDROP 解析）
#include <shlobj.h>  // IDragSourceHelper / SHDRAGIMAGE / CLSID_DragDropHelper（拖拽影像）

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/core/utf8.h"
#include "aurora/event/detail/system_drag_backend.h"
#include "aurora/event/detail/system_drag_ingress.h"
#include "aurora/image/image_codec.h"
#include "aurora/window/detail/system_drag_translate.h"

namespace aurora::detail {

namespace {

// 拖出段的前展声明：`payload_from` 的自拖放直取分支需要查询在途要约与其广告格式（二者
// 定义见本文件「Win32 拖出」段）。同一翻译单元里匿名命名空间是一个实体，跨块前展与后文
// 定义同属该实体，不构成重复定义。`drag_out_in_flight_offer` 是 `DragOutEngine::
// in_flight_offer` 的薄包装：类定义在拖出段，成员访问需完整类型，故调用点只经函数面前展。
class DragOutEngine;
[[nodiscard]] auto drag_out_engine() -> DragOutEngine &;
[[nodiscard]] auto advertised_clip_formats(const DragOffer &offer) -> std::vector<CLIPFORMAT>;
[[nodiscard]] auto drag_out_in_flight_offer() -> std::optional<DragOffer>;

}  // namespace

// STGMEDIUM 的 tymed / hGlobal 是 Win32 协议固有的 tagged union 字段：本 TU 只能按协议判别后取用，
// 无 variant 可替（该结构是 OS ABI 的一部分，不得改写）。
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
namespace {

// 首批可兑现的剪贴板格式：**formats 清单声明即能兑现**，故只列本桥真正能取到字节的格式。
// `CF_DIBV5` → `image/png` 需要 DIBV5 → PNG **转码**（编码侧），本桥取不到，落地前不列——
// 声明了却取不到等于谎报。折算层的映射表已备好该档；拖出侧方向相反（PNG 解码 → DIBV5，
// stb 后端始终注册）已可兑现，见下方拖出数据对象的 `make_dibv5_global`。
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

// @brief 构造落放载荷：**跨进程一律走延迟取数**，本进程自拖放仍走内存直取。
//
// **为什么跨进程不分叉阈值**：折算层那枚 `drag::AURORA_DRAG_SYNC_PAYLOAD_LIMIT`（64 KiB）的语义是
// 「≤ 阈值走同步、> 阈值走异步」，而 Win32 上**无法在取数前预判载荷大小**——`QueryGetData` 只答
// 「给不给得出」，`GlobalSize` 只能在 `GetData` 之后问（那时阻塞已经发生）。故本桥把跨进程路径
// 整体上移到延迟通道：`Drop` 回调只登记「有哪些格式」，字节在 `system_drag::pump_reads()` 的
// 排空阶段才经 `fetch` 产出，帧循环因此跨帧推进而不是在事件派发里停一整拍。
//
// 阈值在该形态下**不参与本桥分叉**（它仍是可预判大小的路径——如自拖放直取——的单一真源），
// 本函数不另设第二个数：与其留一个永不生效的分支，不如让「一律延迟」成为唯一形态。
//
// **自拖放直取**：本进程窗口即落点（在途拖出会话的要约就是待传递的数据本身）时不经数据
// 对象，直接在 UI 线程上调 `provide` 取数。跨单元形态下经数据对象的 `GetData` 会让 UI
// 线程阻塞等拖拽线程应答、而拖拽线程的要约索取又等帧循环排空——帧循环恰恰被本落放回调
// 占住，互等到墙钟超时（见拖出段线程模型注记）。两条路径的取数判据逐条对齐：未广告格式
// 跳过、广告了却给不出按会话失败结束，不把失败伪装成格式不存在。
// @param data 数据对象（自拖放时忽略：直取路径不问数据对象）。
// @return 载荷句柄（无任何可取格式时返回空表构造的句柄，读取一律 `Ok` 空值，不谎报有数据）。

// @brief 数据对象的 RAII 引用：落放载荷跨帧存活，而 OLE 在 `Drop` 返回后即释放数据对象。
//
// `AddRef` 的必要性：延迟取数把字节的产出点从 `Drop` 回调推到之后的帧内排空，那一时刻 OLE 早已
// 收回它给出的指针——不持引用就在排空里解引用已释放对象（ASan 下必现 use-after-free）。
class DataObjectRef {
  public:
    explicit DataObjectRef(IDataObject *data) : data_(data) {
        if (data_ != nullptr) {
            data_->AddRef();
        }
    }
    ~DataObjectRef() {
        if (data_ != nullptr) {
            data_->Release();
        }
    }
    DataObjectRef(const DataObjectRef &) = delete;
    auto operator=(const DataObjectRef &) -> DataObjectRef & = delete;
    DataObjectRef(DataObjectRef &&) = delete;
    auto operator=(DataObjectRef &&) -> DataObjectRef & = delete;

    /// @brief 取被持有的数据对象（可能为空）。
    [[nodiscard]] auto get() const -> IDataObject * { return data_; }

  private:
    IDataObject *data_;  ///< 被引用的数据对象（引用由本类 AddRef / Release 成对管理，非所有权）
};

/// @brief 按剪贴板格式号取原始字节（延迟取数路径的通用档）。
///
/// 与 `read_uri_list` / `read_unicode_text` 的分工：那两档要做语义转换（`CF_HDROP` → uri-list 文本、
/// `CF_UNICODETEXT` → UTF-8），本函数只做「字节原样搬回」。
[[nodiscard]] auto read_clip_bytes(IDataObject *data, CLIPFORMAT format) -> std::vector<std::uint8_t> {
    if (data == nullptr) {
        return {};
    }
    FORMATETC requested{};
    requested.cfFormat = format;
    requested.ptd = nullptr;
    requested.dwAspect = DVASPECT_CONTENT;
    requested.lindex = -1;
    requested.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&requested, &medium)) || medium.hGlobal == nullptr) {
        release_locked_medium(medium);
        return {};
    }
    const auto *source = static_cast<const std::uint8_t *>(GlobalLock(medium.hGlobal));
    if (source == nullptr) {
        release_locked_medium(medium);
        return {};
    }
    const SIZE_T size = GlobalSize(medium.hGlobal);
    std::vector<std::uint8_t> out(size, 0U);
    if (size > 0) {
        std::memcpy(out.data(), source, size);
    }
    GlobalUnlock(medium.hGlobal);
    release_locked_medium(medium);
    return out;
}

/// @brief 延迟取数器：按 MIME 从数据对象取字节（排空阶段被调用，不在 `Drop` 回调内）。
///
/// 取不到一律按**空口径**交出（与同步路径既有的「跳过该格式」同口径）：把「广告了却取不出」升级为
/// `system-drag-read-failed` 是判据变更，会改写既有用例的期望，不在本批——本批只改变取数发生的时刻。
[[nodiscard]] auto fetch_bytes(IDataObject *data, const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    if (mime == AURORA_MIME_URI_LIST) {
        const auto text = read_uri_list(data);
        return text.has_value() ? std::vector<std::uint8_t>(text->begin(), text->end()) : std::vector<std::uint8_t>{};
    }
    if (mime == AURORA_MIME_TEXT_PLAIN) {
        const auto text = read_unicode_text(data);
        return text.has_value() ? std::vector<std::uint8_t>(text->begin(), text->end()) : std::vector<std::uint8_t>{};
    }
    const auto format = drag::win32_clip_format_from_mime(mime);
    if (!format.has_value()) {
        return std::vector<std::uint8_t>{};
    }
    return read_clip_bytes(data, static_cast<CLIPFORMAT>(*format));
}

[[nodiscard]] auto payload_from(IDataObject *data) -> std::shared_ptr<const DragPayload> {
    if (const auto self_offer = drag_out_in_flight_offer(); self_offer.has_value()) {
        std::unordered_map<std::string, std::vector<std::uint8_t>> entries;
        const auto advertised = advertised_clip_formats(*self_offer);
        const auto take = [&](const std::string &mime) -> void {
            const auto format = drag::win32_clip_format_from_mime(mime);
            if (!format.has_value() ||
                std::find(advertised.begin(), advertised.end(), static_cast<CLIPFORMAT>(*format)) == advertised.end()) {
                return;  // 要约未声明该格式：与数据对象路径的 DV_E_FORMATETC 同口径
            }
            const auto bytes = (*self_offer).provide(mime);
            if (!bytes) {
                // 广告了却给不出：与数据对象路径同罪（会话按失败结束），不把失败伪装成格式不存在。
                detail::complete_drag_out_session(SystemDragOutcome::Failed, DragAction::None);
                return;
            }
            entries.emplace(mime, bytes.value());
        };
        take(std::string{AURORA_MIME_URI_LIST});
        take(std::string{AURORA_MIME_TEXT_PLAIN});
        return DragPayload::from_bytes(std::move(entries));
    }
    if (data == nullptr) {
        return DragPayload::from_bytes({});
    }
    // 跨进程路径：只声明「有哪些格式」，取数延后到排空阶段。
    const auto formats = supported_formats_of(data);
    std::vector<std::string> mimes;
    for (const auto *mime : {&AURORA_MIME_URI_LIST, &AURORA_MIME_TEXT_PLAIN, &AURORA_MIME_IMAGE_PNG}) {
        const auto cf = drag::win32_clip_format_from_mime(*mime);
        if (cf.has_value() && std::find(formats.begin(), formats.end(), *cf) != formats.end()) {
            mimes.emplace_back(*mime);
        }
    }
    // 数据对象由 OLE 在 `Drop` 返回后释放：本句柄跨帧存活，故自持一份引用（RAII Release）。
    auto holder = std::make_shared<DataObjectRef>(data);
    return DragPayload::from_lazy(std::move(mimes),
                                  [holder](const std::string &mime) -> Result<std::vector<std::uint8_t>> {
                                      return fetch_bytes(holder->get(), mime);
                                  });
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

// ---- Win32 拖出：`IDropSource` + `IDataObject` + 库自管拖拽线程 ----
//
// 线程模型（设计理由见头文件注记）：`DoDragDrop` 在库自管的拖拽线程上跑，UI 线程帧循环不
// 被打断；会话终态经 `detail::complete_drag_out_session` 登记、帧循环投递；控件数据
// （`DragOffer::provide`）只经跨线程要约索取通道在 UI 线程应答——拖拽线程不得直接调用
// 应用回调（单线程 UI 纪律）。自拖放（本进程窗口即落点）另有直取路径：落放回调在 UI 线程
// 上经 `in_flight_offer` 拿到在途要约后直接调 `provide` 取数，不经数据对象——跨单元形态
// 下 `GetData` 会让 UI 线程与拖拽线程互等到墙钟超时（前者被落放回调占住、后者的要约索取
// 等帧循环排空），直取从根上消解该互等；同单元到达的 `GetData` 由 `fetch_from_offer` 按
// 调用线程判别同步应答，是直取之外的兜底。
//
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) STGMEDIUM 的 tymed / hGlobal 是
// Win32 协议固有的 tagged union 字段，只能按协议判别后取用，无 variant 可替。
namespace {

/// @brief 一场拖出会话的共享态（UI 线程 `begin` 时快照；拖拽线程只读 + 取消标志）。
struct DragOutSession {
    DragOffer offer;  ///< 要约拷贝（含影像 / 热点 / 缺省动作 / Move 确认）
    float scale = 1.0F;  ///< DPI 缩放快照（dp → 物理像素；仅影像热点折算）
    std::thread::id ui_thread{};  ///< UI 线程标识（`begin` 契约保证在 UI 线程被调）
    std::atomic<bool> cancel_requested{false};  ///< 程序化取消标志（`QueryContinueDrag` 轮询）
};

/// @brief `DoDragDrop` 返回值 → 折算层退出码枚举（SDK 宏 → 本地枚举的唯一映射点）。
[[nodiscard]] auto exit_of_drag_out(HRESULT dragged) -> drag::Win32DragOutExit {
    if (dragged == DRAGDROP_S_DROP) {
        return drag::Win32DragOutExit::Dropped;
    }
    if (dragged == DRAGDROP_S_CANCEL) {
        return drag::Win32DragOutExit::Cancelled;
    }
    if (dragged == DRAGDROP_S_USEDEFAULTCURSORS) {
        return drag::Win32DragOutExit::DefaultCursors;
    }
    return drag::Win32DragOutExit::Unexpected;  // 失败 HRESULT 一族
}

/// @brief 要约声明的 MIME → 可广告的剪贴板格式（去重、保序、丢弃未登记项）。
///
/// 未登记的自定义 MIME 不广告：只登记本库会消费 / 产出的格式（与拖入侧同口径），
/// 声明了却给不出等于谎报。
[[nodiscard]] auto advertised_clip_formats(const DragOffer &offer) -> std::vector<CLIPFORMAT> {
    std::vector<CLIPFORMAT> out;
    for (const auto &mime : offer.formats) {
        const auto format = drag::win32_clip_format_from_mime(mime);
        if (!format.has_value()) {
            continue;
        }
        const auto cf = static_cast<CLIPFORMAT>(*format);
        if (std::find(out.begin(), out.end(), cf) == out.end()) {
            out.push_back(cf);
        }
    }
    return out;
}

/// @brief 源侧可协商的落放动作：Copy | Link 恒可，Move 仅在提供了删除确认回调时可。
///
/// Move 需要源侧在落放后删除原数据；没有 `on_move_confirmed` 的源无法兑现该语义，
/// 故不广告（目标侧也就不会把光标切成 Move）。
[[nodiscard]] auto allowed_drop_effects(const DragOffer &offer) -> DWORD {
    DWORD effects = DROPEFFECT_COPY | DROPEFFECT_LINK;
    if (offer.on_move_confirmed) {
        effects |= DROPEFFECT_MOVE;
    }
    return effects;
}

/// @brief 复制全局内存块（`SetData` 非接管形态与 `GetData` 交出副本共用）。
[[nodiscard]] auto duplicate_hglobal(HGLOBAL source) -> HGLOBAL {
    if (source == nullptr) {
        return nullptr;
    }
    const SIZE_T size = GlobalSize(source);
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (copy == nullptr) {
        return nullptr;
    }
    void *source_locked = GlobalLock(source);
    void *copy_locked = GlobalLock(copy);
    if (source_locked != nullptr && copy_locked != nullptr) {
        std::memcpy(copy_locked, source_locked, size);
    }
    GlobalUnlock(source);
    GlobalUnlock(copy);
    return copy;
}

/// @brief UTF-8 字节 → `CF_UNICODETEXT` 全局内存（UTF-16 + 结尾 NUL）。
[[nodiscard]] auto make_unicode_text_global(const std::vector<std::uint8_t> &bytes) -> HGLOBAL {
    const std::string text(bytes.begin(), bytes.end());
    const std::wstring wide = internal::utf8_to_wstr(text);
    const SIZE_T size = (wide.size() + 1U) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size);
    if (handle == nullptr) {
        return nullptr;
    }
    void *locked = GlobalLock(handle);
    if (locked == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    std::memcpy(locked, wide.c_str(), size);
    GlobalUnlock(handle);
    return handle;
}

/// @brief `text/uri-list` 字节 → `CF_HDROP` 全局内存（DROPFILES + 双 NUL 结尾的宽路径清单）。
[[nodiscard]] auto make_hdrop_global(const std::vector<std::uint8_t> &bytes) -> HGLOBAL {
    const std::string text(bytes.begin(), bytes.end());
    const auto paths = uri_list::decode(text);
    if (paths.empty()) {
        // 无可落成文件的条目（非 file scheme 一类）：HDROP 无从构造，如实给不出。
        return nullptr;
    }
    std::vector<std::wstring> wide_paths;
    wide_paths.reserve(paths.size());
    SIZE_T payload = 0;
    for (const auto &path : paths) {
        wide_paths.push_back(internal::utf8_to_wstr(path));
        payload += (wide_paths.back().size() + 1U) * sizeof(wchar_t);
    }
    payload += sizeof(wchar_t);  // 清单结尾双 NUL 的第二个
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + payload);
    if (handle == nullptr) {
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast, cppcoreguidelines-pro-type-reinterpret-cast)
    auto *drop = static_cast<DROPFILES *>(GlobalLock(handle));
    if (drop == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);  // 路径清单紧随结构体之后
    drop->pt = POINT{.x = 0, .y = 0};
    drop->fNC = FALSE;  // 非客户区坐标（本库无屏幕坐标概念，交给目标解释）
    drop->fWide = TRUE;  // 宽字符路径
    // 结构体尾部即宽串缓冲区，且缓冲区只能按指针步进（reinterpret_cast + 指针算术同源）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto *cursor = reinterpret_cast<wchar_t *>(reinterpret_cast<std::uint8_t *>(drop) + sizeof(DROPFILES));
    for (const auto &wide : wide_paths) {
        std::wmemcpy(cursor, wide.c_str(), wide.size() + 1U);
        cursor += wide.size() + 1U;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    *cursor = L'\0';  // 双 NUL 收尾
    GlobalUnlock(handle);
    return handle;
}

/// @brief `image/png` 字节 → `CF_DIBV5` 全局内存（BITMAPV5HEADER + 自底向上 BGRA 像素）。
///
/// PNG 解码走仓内图像编解码注册表（stb 后端始终注册），故该格式在所有构建可兑现——
/// 与拖入侧（需 DIBV5 → PNG **编码**，暂不可得）方向相反，口径见文件头格式清单注记。
[[nodiscard]] auto make_dibv5_global(const std::vector<std::uint8_t> &bytes) -> HGLOBAL {
    const auto decoded = image::decode_memory(bytes);
    if (!decoded || decoded.value().width <= 0 || decoded.value().height <= 0) {
        return nullptr;
    }
    const Image &icon = decoded.value();
    const auto row_bytes = static_cast<SIZE_T>(icon.width) * 4U;
    const SIZE_T pixels_size = row_bytes * static_cast<SIZE_T>(icon.height);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixels_size);
    if (handle == nullptr) {
        return nullptr;
    }
    auto *base = static_cast<std::uint8_t *>(GlobalLock(handle));
    if (base == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) 全局内存布局即 DIB 布局
    auto *header = reinterpret_cast<BITMAPV5HEADER *>(base);
    *header = BITMAPV5HEADER{};
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = icon.width;
    header->bV5Height = icon.height;  // 正值 = 自底向上行序
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5RedMask = 0x00FF0000U;
    header->bV5GreenMask = 0x0000FF00U;
    header->bV5BlueMask = 0x000000FFU;
    header->bV5AlphaMask = 0xFF000000U;
    // 像素区只能由「全局内存基址 + 头长」定位，逐行拷贝即按行距步进；下标访问在该检查面上
    // 与指针算术同罪，故整段按契约点名豁免一次，不逐行标记。
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto *pixels = base + sizeof(BITMAPV5HEADER);
    const auto *source = icon.pixels.data();
    for (int y = 0; y < icon.height; ++y) {
        // 源为 RGBA8 自顶向下；DIB 自底向上：目标第 y 行对应源第 (height-1-y) 行。
        const auto *source_row = source + (static_cast<std::size_t>(icon.height - 1 - y) * row_bytes);
        auto *dest_row = pixels + (static_cast<std::size_t>(y) * row_bytes);
        for (int x = 0; x < icon.width; ++x) {
            dest_row[(x * 4U) + 0U] = source_row[(x * 4U) + 2U];  // B
            dest_row[(x * 4U) + 1U] = source_row[(x * 4U) + 1U];  // G
            dest_row[(x * 4U) + 2U] = source_row[(x * 4U) + 0U];  // R
            dest_row[(x * 4U) + 3U] = source_row[(x * 4U) + 3U];  // A（非预乘，helper 自行乘 alpha）
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    GlobalUnlock(handle);
    return handle;
}

/// @brief 按剪贴板格式构造 HGLOBAL 介质（三种可广告格式各一条路径）。
[[nodiscard]] auto build_medium(CLIPFORMAT format, const std::vector<std::uint8_t> &bytes, STGMEDIUM &out) -> bool {
    HGLOBAL handle = nullptr;
    switch (format) {
        case static_cast<CLIPFORMAT>(drag::Win32ClipFormat::UnicodeText):
            handle = make_unicode_text_global(bytes);
            break;
        case static_cast<CLIPFORMAT>(drag::Win32ClipFormat::Hdrop):
            handle = make_hdrop_global(bytes);
            break;
        case static_cast<CLIPFORMAT>(drag::Win32ClipFormat::DibV5):
            handle = make_dibv5_global(bytes);
            break;
        default:
            return false;
    }
    if (handle == nullptr) {
        return false;
    }
    out.tymed = TYMED_HGLOBAL;
    out.hGlobal = handle;
    out.pUnkForRelease = nullptr;  // 调用方持副本所有权，经 ReleaseStgMedium 释放
    return true;
}

/// @brief `IEnumFORMATETC` 最小实现：广告清单 + 游标（仅支持取方向）。
///
/// COM 纪律：不自删、析构非虚（同 DropTargetImpl）。
class DragOutFormatEnum final : public IEnumFORMATETC {
  public:
    explicit DragOutFormatEnum(std::vector<FORMATETC> formats) : formats_(std::move(formats)) {}

    auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) -> HRESULT override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFORMATETC)) {
            *object = static_cast<IEnumFORMATETC *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override { return ++ref_; }

    auto STDMETHODCALLTYPE Release() -> ULONG override {
        if (ref_ > 0U) {
            --ref_;
        }
        return ref_;
    }

    auto STDMETHODCALLTYPE Next(ULONG count, FORMATETC *out, ULONG *fetched) -> HRESULT override {
        if (out == nullptr) {
            return E_POINTER;
        }
        ULONG done = 0U;
        while (done < count && index_ < formats_.size()) {
            // `out` 是 COM 契约的调用方数组，只能按索引写入。
            out[done] = formats_[index_];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            ++index_;
            ++done;
        }
        if (fetched != nullptr) {
            *fetched = done;
        }
        return (done == count) ? S_OK : S_FALSE;
    }

    auto STDMETHODCALLTYPE Skip(ULONG count) -> HRESULT override {
        if (index_ + static_cast<std::size_t>(count) > formats_.size()) {
            index_ = formats_.size();
            return S_FALSE;
        }
        index_ += static_cast<std::size_t>(count);
        return S_OK;
    }

    auto STDMETHODCALLTYPE Reset() -> HRESULT override {
        index_ = 0;
        return S_OK;
    }

    auto STDMETHODCALLTYPE Clone(IEnumFORMATETC **out) -> HRESULT override {
        if (out == nullptr) {
            return E_POINTER;
        }
        // NOLINTNEXTLINE(*-owning-memory) COM 契约：所有权经 Release 交还调用方
        auto *clone = new DragOutFormatEnum(formats_);
        clone->index_ = index_;
        *out = clone;
        return S_OK;
    }

  private:
    std::vector<FORMATETC> formats_;
    std::size_t index_ = 0;
    ULONG ref_ = 1U;
};

/// @brief 拖出数据对象：广告要约可折算的剪贴板格式，取数经跨线程要约索取（自拖放除外）。
///
/// `SetData` 必须支持：`IDragSourceHelper::InitializeFromBitmap` 经它把拖拽影像存进本对象
/// （私有剪贴板格式；跨进程拖拽影像经该介质传输，故其中存的是位图副本而非 HBITMAP 句柄）。
///
/// COM 纪律：不自删、析构非虚（同 DropTargetImpl）。
class DragOutDataObject final : public IDataObject {
  public:
    explicit DragOutDataObject(DragOutSession *session) : session_(session) {}

    // COM 对象的身份与引用计数绑定，拷贝 / 移动都无意义（且会让两处各自释放同一份介质）。
    // 五个特殊成员一并声明，避免只写析构而被读成「遗漏的一半」。
    DragOutDataObject(const DragOutDataObject &) = delete;
    auto operator=(const DragOutDataObject &) -> DragOutDataObject & = delete;
    DragOutDataObject(DragOutDataObject &&) = delete;
    auto operator=(DragOutDataObject &&) -> DragOutDataObject & = delete;

    ~DragOutDataObject() {
        // 仅拖拽线程在 `DoDragDrop` 返回后触碰本对象（OLE 会话已收摊），析构释放接管介质。
        for (auto &entry : stored_) {
            if (entry.owned && entry.medium.hGlobal != nullptr) {
                GlobalFree(entry.medium.hGlobal);
                entry.medium.hGlobal = nullptr;
            }
        }
    }

    // IUnknown
    auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) -> HRESULT override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
            *object = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override { return ++ref_; }

    auto STDMETHODCALLTYPE Release() -> ULONG override {
        // **不自删**：所有权归 `run_session` 栈帧（同 DropTargetImpl 纪律）。
        if (ref_ > 0U) {
            --ref_;
        }
        return ref_;
    }

    // IDataObject
    auto STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) -> HRESULT override;
    auto STDMETHODCALLTYPE GetDataHere(FORMATETC * /*format*/, STGMEDIUM * /*medium*/) -> HRESULT override {
        return DV_E_FORMATETC;  // 不支持就地读取
    }
    auto STDMETHODCALLTYPE QueryGetData(FORMATETC *format) -> HRESULT override;
    auto STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *in, FORMATETC *out) -> HRESULT override {
        if (in == nullptr || out == nullptr) {
            return E_POINTER;
        }
        *out = *in;
        return DATA_S_SAMEFORMATETC;
    }
    auto STDMETHODCALLTYPE SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release) -> HRESULT override;
    auto STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **enum_out) -> HRESULT override;
    auto STDMETHODCALLTYPE DAdvise(FORMATETC * /*format*/, DWORD /*advise_flags*/, IAdviseSink * /*sink*/,
                                   DWORD * /*connection*/) -> HRESULT override {
        return OLE_E_ADVISENOTSUPPORTED;  // 拖放会话无变更通知需求
    }
    auto STDMETHODCALLTYPE DUnadvise(DWORD /*connection*/) -> HRESULT override { return OLE_E_ADVISENOTSUPPORTED; }
    auto STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **enum_out) -> HRESULT override {
        if (enum_out == nullptr) {
            return E_POINTER;
        }
        *enum_out = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

  private:
    /// @brief `SetData` 登记的条目（拖拽影像私有格式）。
    struct StoredEntry {
        FORMATETC format{};
        STGMEDIUM medium{};
        bool owned = false;  ///< 介质所有权是否已转移给本对象（`SetData` 的 fRelease）
    };

    /// @brief 取某个 MIME 的字节：自拖放同步应答，否则经跨线程索取 + 墙钟超时。
    [[nodiscard]] auto fetch_from_offer(const std::string &mime) -> Result<std::vector<std::uint8_t>>;

    /// @brief 找 `SetData` 登记的条目（按格式数值）。
    [[nodiscard]] auto find_stored(CLIPFORMAT cf) const -> const StoredEntry *;

    DragOutSession *session_;  ///< 非拥有；生命周期覆盖整场会话（引擎持 shared_ptr）
    std::vector<StoredEntry> stored_;  ///< `SetData` 登记的条目（仅拖拽线程触碰）
    ULONG ref_ = 1U;  ///< COM 引用计数（起 1：`run_session` 栈帧持一份）
};

auto DragOutDataObject::find_stored(const CLIPFORMAT cf) const -> const StoredEntry * {
    for (const auto &entry : stored_) {
        if (entry.format.cfFormat == cf && (entry.format.tymed & TYMED_HGLOBAL) != 0) {
            return &entry;
        }
    }
    return nullptr;
}

auto DragOutDataObject::fetch_from_offer(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    if (session_ == nullptr || !session_->offer.provide) {
        return make_error(ErrorCode::SystemDragReadFailed,
                          ErrorParams{{"mime", mime}, {"detail", "drag-out session has no provide callback"}});
    }
    if (session_->ui_thread == std::this_thread::get_id()) {
        // 自拖放（本进程窗口即落点）且 `GetData` 同单元到达：直接应答。自拖放的常规路径本是
        // 拖入侧直取（`payload_from` 的 `in_flight_offer` 分支，不经数据对象）；本分支是同
        // 单元调用的兜底。走跨线程队列会让 UI 线程等自己的帧循环排空，直到墙钟超时（自拖放
        // 5 秒冻结 + 读取失败）。
        return session_->offer.provide(mime);
    }
    auto promise = std::make_shared<std::promise<Result<std::vector<std::uint8_t>>>>();
    auto future = promise->get_future();
    // 指定初始化：`ProvideRequest` 的两个字段顺序不参与契约，具名后插入字段不会错配。
    detail::post_provide_request(detail::ProvideRequest{
        .mime = mime,
        .fulfill = [promise](Result<std::vector<std::uint8_t>> answer) { promise->set_value(std::move(answer)); }});
    // UI 线程不排空（宿主不调 `pump_reads`）时按墙钟超时自行失败：拖拽线程不得永久悬挂。
    if (future.wait_for(detail::system_drag_read_timeout()) != std::future_status::ready) {
        return make_error(ErrorCode::SystemDragReadFailed,
                          ErrorParams{{"mime", mime}, {"detail", "drag-out provide request timed out"}});
    }
    return future.get();
}

auto STDMETHODCALLTYPE DragOutDataObject::QueryGetData(FORMATETC *format) -> HRESULT {
    if (format == nullptr) {
        return E_POINTER;
    }
    if (format->dwAspect != DVASPECT_CONTENT || format->lindex != -1) {
        return DV_E_FORMATETC;
    }
    if (format->tymed != TYMED_NULL && (format->tymed & TYMED_HGLOBAL) == 0) {
        return DV_E_TYMED;
    }
    if (find_stored(format->cfFormat) != nullptr) {
        return S_OK;  // `SetData` 登记的条目（拖拽影像私有格式）
    }
    for (const auto cf : advertised_clip_formats(session_ == nullptr ? DragOffer{} : session_->offer)) {
        if (cf == format->cfFormat) {
            return S_OK;
        }
    }
    return DV_E_FORMATETC;
}

auto STDMETHODCALLTYPE DragOutDataObject::SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release) -> HRESULT {
    if (format == nullptr || medium == nullptr) {
        return E_POINTER;
    }
    // 只接受 HGLOBAL 介质且无外部释放接口（拖拽影像私有格式即此形态）；其余如实不受理。
    if (medium->tymed != TYMED_HGLOBAL || medium->hGlobal == nullptr || medium->pUnkForRelease != nullptr) {
        return DV_E_TYMED;
    }
    StoredEntry entry;
    entry.format = *format;
    entry.owned = (release != FALSE);
    if (entry.owned) {
        entry.medium = *medium;  // 所有权转移（fRelease=TRUE 语义）
    } else {
        entry.medium.tymed = TYMED_HGLOBAL;
        entry.medium.hGlobal = duplicate_hglobal(medium->hGlobal);
        entry.medium.pUnkForRelease = nullptr;
        if (entry.medium.hGlobal == nullptr) {
            return E_OUTOFMEMORY;
        }
    }
    // 同格式重复登记（helper 只会登记一次，此处为健壮性兜底）：先释放旧条目。
    for (auto &existing : stored_) {
        if (existing.format.cfFormat == entry.format.cfFormat) {
            if (existing.owned && existing.medium.hGlobal != nullptr) {
                GlobalFree(existing.medium.hGlobal);
            }
            existing = entry;  // 平凡可拷贝类型：`std::move` 对它无效果，故直接赋值
            return S_OK;
        }
    }
    stored_.push_back(entry);
    return S_OK;
}

auto STDMETHODCALLTYPE DragOutDataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC **enum_out) -> HRESULT {
    if (enum_out == nullptr) {
        return E_POINTER;
    }
    *enum_out = nullptr;
    if (direction != DATADIR_GET) {
        return E_NOTIMPL;  // 只支持取方向（本对象不接受目标侧回写）
    }
    std::vector<FORMATETC> formats;
    for (const auto cf : advertised_clip_formats(session_ == nullptr ? DragOffer{} : session_->offer)) {
        FORMATETC entry{};
        entry.cfFormat = cf;
        entry.ptd = nullptr;
        entry.dwAspect = DVASPECT_CONTENT;
        entry.lindex = -1;
        entry.tymed = TYMED_HGLOBAL;
        formats.push_back(entry);
    }
    // NOLINTNEXTLINE(*-owning-memory) COM 契约：所有权经 Release 交还调用方
    *enum_out = new DragOutFormatEnum(std::move(formats));
    return S_OK;
}

auto STDMETHODCALLTYPE DragOutDataObject::GetData(FORMATETC *format, STGMEDIUM *medium) -> HRESULT {
    if (format == nullptr || medium == nullptr) {
        return E_POINTER;
    }
    if (format->dwAspect != DVASPECT_CONTENT || format->lindex != -1) {
        return DV_E_FORMATETC;
    }
    if (format->tymed != TYMED_NULL && (format->tymed & TYMED_HGLOBAL) == 0) {
        return DV_E_TYMED;
    }
    // ① `SetData` 登记的条目（拖拽影像私有格式）：复制介质后交出（调用方持副本所有权）。
    if (const auto *entry = find_stored(format->cfFormat); entry != nullptr) {
        STGMEDIUM copy{};
        copy.tymed = TYMED_HGLOBAL;
        copy.hGlobal = duplicate_hglobal(entry->medium.hGlobal);
        copy.pUnkForRelease = nullptr;
        if (copy.hGlobal == nullptr) {
            return E_OUTOFMEMORY;
        }
        *medium = copy;
        return S_OK;
    }
    // ② 要约声明的格式：跨线程索取字节 → 按格式构造介质。
    const auto mime = drag::mime_from_win32_clip_format(static_cast<std::uint16_t>(format->cfFormat));
    if (!mime.has_value()) {
        return DV_E_FORMATETC;
    }
    bool advertised = false;
    for (const auto cf : advertised_clip_formats(session_ == nullptr ? DragOffer{} : session_->offer)) {
        if (cf == format->cfFormat) {
            advertised = true;
            break;
        }
    }
    if (!advertised) {
        return DV_E_FORMATETC;
    }
    const auto bytes = fetch_from_offer(*mime);
    if (!bytes) {
        // 要约提供失败 = 会话按失败结束（event.h `DragOffer::provide` 契约）：登记终态后
        // 如实向目标报读取失败，不把「给不出」伪装成「格式不存在」。
        detail::complete_drag_out_session(SystemDragOutcome::Failed, DragAction::None);
        return E_FAIL;
    }
    STGMEDIUM built{};
    if (!build_medium(format->cfFormat, bytes.value(), built)) {
        // 广告了却构造不出介质（PNG 字节不可解码一类）与提供失败同罪：会话按失败结束。
        detail::complete_drag_out_session(SystemDragOutcome::Failed, DragAction::None);
        return E_FAIL;
    }
    *medium = built;
    return S_OK;
}

/// @brief `IDropSource` 实现：取消检测的唯一来源是 `QueryContinueDrag`（Win32 拖放惯例）。
///
/// COM 纪律：不自删、析构非虚（同 DropTargetImpl）。
class DragOutDropSource final : public IDropSource {
  public:
    explicit DragOutDropSource(DragOutSession *session) : session_(session) {}

    auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) -> HRESULT override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropSource)) {
            *object = static_cast<IDropSource *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override { return ++ref_; }

    auto STDMETHODCALLTYPE Release() -> ULONG override {
        if (ref_ > 0U) {
            --ref_;
        }
        return ref_;
    }

    auto STDMETHODCALLTYPE QueryContinueDrag(BOOL escape_pressed, DWORD key_state) -> HRESULT override {
        if (escape_pressed != FALSE) {
            return DRAGDROP_S_CANCEL;  // Esc = 用户取消（用户意图，不是故障）
        }
        if (session_->cancel_requested.load(std::memory_order_relaxed)) {
            return DRAGDROP_S_CANCEL;  // 程序化取消（`system_drag::cancel`）
        }
        if ((key_state & MK_RBUTTON) != 0U) {
            return DRAGDROP_S_CANCEL;  // 右键取消（Windows 拖放惯例）
        }
        if ((key_state & MK_LBUTTON) == 0U) {
            return DRAGDROP_S_DROP;  // 左键释放 = 落放
        }
        return S_OK;
    }

    auto STDMETHODCALLTYPE GiveFeedback(DWORD /*effect*/) -> HRESULT override {
        return DRAGDROP_S_USEDEFAULTCURSORS;  // 光标反馈交 OS 默认，本库不自绘
    }

  private:
    DragOutSession *session_;  ///< 非拥有；生命周期覆盖整场会话
    ULONG ref_ = 1U;  ///< COM 引用计数（起 1：`run_session` 栈帧持一份）
};

/// @brief 由 `DragOffer::icon` 构造拖拽影像位图（32bpp DIB section；RGBA8 → BGRA）。
struct DragImageBitmap {
    HBITMAP bitmap = nullptr;  ///< 位图句柄（成功时非空；调用方负责 DeleteObject）
    int width = 0;  ///< 像素宽
    int height = 0;  ///< 像素高
};

[[nodiscard]] auto create_drag_image_bitmap(const Image &icon) -> DragImageBitmap {
    DragImageBitmap out;
    if (icon.width <= 0 || icon.height <= 0 ||
        icon.pixels.size() < static_cast<std::size_t>(icon.width) * static_cast<std::size_t>(icon.height) * 4U) {
        return out;  // 空图像或像素数不符：交 OS 默认影像
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = icon.width;
    info.bmiHeader.biHeight = -icon.height;  // 负值 = 自顶向下行序
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    out.bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (out.bitmap == nullptr || pixels == nullptr) {
        out.bitmap = nullptr;
        return out;
    }
    out.width = icon.width;
    out.height = icon.height;
    // 同上：`CreateDIBSection` 交回的是裸像素缓冲区，逐像素写入即按步长步进。
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto *source = icon.pixels.data();
    auto *dest = static_cast<std::uint8_t *>(pixels);
    for (int i = 0; i < icon.width * icon.height; ++i) {
        dest[(i * 4) + 0] = source[(i * 4) + 2];  // B
        dest[(i * 4) + 1] = source[(i * 4) + 1];  // G
        dest[(i * 4) + 2] = source[(i * 4) + 0];  // R
        dest[(i * 4) + 3] = source[(i * 4) + 3];  // A（非预乘：MSDN 明示 helper 计算 alpha 时自乘 RGB）
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return out;
}

/// @brief 经 `IDragSourceHelper` 把拖拽影像装进数据对象（`DragOffer::icon` 非空时）。
///
/// 空图像交 OS 默认影像：影像是装饰性提示，缺失不致语义受损（`DragOffer::icon` 契约），
/// 故 helper 缺失（旧系统）或位图构造失败都静默降级，不改变会话语义。
/// @return 装进数据对象的位图句柄（调用方在 `DoDragDrop` 返回后删除）；未设置影像时为 nullptr。
[[nodiscard]] auto setup_drag_image(const DragOutSession *session, IDataObject *data) -> HBITMAP {
    if (session->offer.icon.width <= 0 || session->offer.icon.height <= 0) {
        return nullptr;
    }
    IDragSourceHelper *helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&helper)))) {
        return nullptr;  // 旧系统无拖拽影像 helper：静默降级
    }
    const DragImageBitmap image = create_drag_image_bitmap(session->offer.icon);
    HBITMAP handed_off = nullptr;
    if (image.bitmap != nullptr) {
        SHDRAGIMAGE drag_image{};
        drag_image.sizeDragImage = SIZE{.cx = image.width, .cy = image.height};
        // 热点（逻辑 dp）→ 像素：dp × DPI 缩放快照，夹在影像尺寸内（不得出现负偏移）。
        const float scale = session->scale > 0.0F ? session->scale : 1.0F;
        const int offset_x = static_cast<int>(std::lround(session->offer.icon_hotspot.x * scale));
        const int offset_y = static_cast<int>(std::lround(session->offer.icon_hotspot.y * scale));
        drag_image.ptOffset =
            POINT{.x = std::clamp(offset_x, 0, image.width), .y = std::clamp(offset_y, 0, image.height)};
        drag_image.hbmpDragImage = image.bitmap;
        drag_image.crColorKey = CLR_NONE;  // 透明度由位图 alpha 通道承载（非预乘）
        // helper 同步把位图**副本**存进数据对象（私有格式介质）：HBITMAP 句柄无法跨进程
        // 传输，故此后位图归本线程所有，由 `run_session` 在 DoDragDrop 返回后删除。
        handed_off = SUCCEEDED(helper->InitializeFromBitmap(&drag_image, data)) ? image.bitmap : nullptr;
        if (handed_off == nullptr) {
            DeleteObject(image.bitmap);  // 装载失败：位图无人接管，就地释放（免泄漏）
        }
    }
    helper->Release();
    return handed_off;
}

/// @brief 拖出引擎（进程级单槽）：库自管拖拽线程 + 单会话串行。
class DragOutEngine {
  public:
    /// @brief 发起一场拖出（UI 线程调用；发起即返回）。
    /// @return true = 会话已搬到拖拽线程；false = 已有会话在途或线程创建失败。
    auto begin(const DragOffer &offer) -> bool;

    /// @brief 取消在途会话（程序化取消；无在途会话为 no-op）。
    auto cancel() -> void;

    /// @brief 在途会话的要约快照（UI 线程查询；无在途会话为 `std::nullopt`）。
    ///
    /// 自拖放（本进程窗口即落点）时拖入侧据此直接在 UI 线程取数、不经数据对象：跨单元
    /// 形态下 `GetData` 会把 UI 线程与拖拽线程互等到墙钟超时（UI 线程被落放回调占住、
    /// 拖拽线程的要约索取等帧循环排空，见 `fetch_from_offer` 注记）。无在途会话（含一切
    /// 跨进程拖入）时返回空，拖入侧照旧走数据对象路径。
    [[nodiscard]] auto in_flight_offer() -> std::optional<DragOffer>;

    /// @brief 登记进程级 DPI 缩放提示（宿主 `refresh_scale()` 后调用；见头文件注记）。
    auto note_scale(float scale) -> void;

  private:
    auto run_session(const std::shared_ptr<DragOutSession> &session) -> void;

    std::mutex mutex_;  ///< 保护以下成员
    bool in_flight_ = false;  ///< 是否有会话在途
    std::thread thread_;  ///< 拖拽线程（单会话串行）
    std::shared_ptr<DragOutSession> current_;  ///< 在途会话（取消路径置位用；结束后即空）
    std::atomic<float> scale_hint_{1.0F};  ///< 宿主推送的 DPI 缩放（仅 begin 快照用；不持窗口指针）
};

auto DragOutEngine::note_scale(const float scale) -> void {
    // 非有限值 / 非正值按 1.0：折算只消费有限正数（`setup_drag_image` 另有兜底，双保险）。
    scale_hint_.store((std::isfinite(scale) && scale > 0.0F) ? scale : 1.0F, std::memory_order_relaxed);
}

auto DragOutEngine::in_flight_offer() -> std::optional<DragOffer> {
    const std::scoped_lock lock{mutex_};
    if (!in_flight_ || current_ == nullptr) {
        return std::nullopt;
    }
    return current_->offer;
}

auto DragOutEngine::begin(const DragOffer &offer) -> bool {
    const std::scoped_lock lock{mutex_};
    if (in_flight_) {
        return false;  // 单线程 UI 下一次一会话（事件层已先行拒绝，这里是引擎侧第二道闸）
    }
    if (thread_.joinable()) {
        thread_.join();  // 上一场已让出单槽但句柄未收：兜底 join（此时线程必已退出）
    }
    auto session = std::make_shared<DragOutSession>();
    session->offer = offer;
    session->ui_thread = std::this_thread::get_id();  // begin 契约：UI 线程调用
    session->scale = scale_hint_.load(std::memory_order_relaxed);  // 进程级提示槽（见头文件注记）
    current_ = session;  // 取消路径按指针置位（会话态在途期间有效）
    in_flight_ = true;
    try {
        // 闭包内的 `session` 是 const（按值捕获 + 非 mutable），`std::move` 对它无效果；
        // 会话的存活由闭包自身持有到线程结束，传引用即可。
        thread_ = std::thread([this, session]() { run_session(session); });
    } catch (const std::system_error &) {
        // 线程创建失败（资源耗尽一类）：会话无从发起，如实回绝（事件层落 session-failed）。
        current_ = nullptr;
        in_flight_ = false;
        return false;
    }
    return true;
}

auto DragOutEngine::cancel() -> void {
    // 只置取消标志：真正收摊由 `QueryContinueDrag` 在 OS 拖拽循环内完成（Win32 没有
    // 从外部掐断 `DoDragDrop` 的 API）。标志在下一回 OS 回调查询时生效；鼠标完全静止
    // 时该回调稀疏，故取消以 Esc / 右键路径为主、程序化取消为辅。
    // 会话指针在锁内拷一份再置位（shared_ptr 拷贝是原子引用计数操作，置位本身无需持锁）。
    std::shared_ptr<DragOutSession> session;
    {
        const std::scoped_lock lock{mutex_};
        if (in_flight_) {
            session = current_;
        }
    }
    if (session != nullptr) {
        session->cancel_requested.store(true, std::memory_order_relaxed);
    }
}

auto DragOutEngine::run_session(const std::shared_ptr<DragOutSession> &session) -> void {
    // 拖拽线程自持一次 OLE 初始化：`DoDragDrop` 要求调用线程有 COM 单元，与 UI 线程的
    // `OleInitialize`（拖入桥）互不相干——两个 STA 各管各的会话。
    const HRESULT ole = OleInitialize(nullptr);
    const bool ole_ready = (ole == S_OK || ole == S_FALSE);  // S_FALSE = 本线程已初始化过，同样可用
    if (!ole_ready) {
        AURORA_LOG_WARN("system-drag", "drag-out OleInitialize failed: hr=0x%08lX", static_cast<unsigned long>(ole));
        detail::complete_drag_out_session(SystemDragOutcome::Failed, DragAction::None);
    } else {
        // COM 对象按 DropTargetImpl 同款纪律：栈上构造、本线程持所有权、不自删。
        DragOutDataObject data{session.get()};
        DragOutDropSource source{session.get()};
        const HBITMAP drag_image = setup_drag_image(session.get(), &data);
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT dragged = DoDragDrop(&data, &source, allowed_drop_effects(session->offer), &effect);
        const auto result = drag::result_from_win32_drag_out(exit_of_drag_out(dragged), effect);
        // 终态经事件层登记（线程安全）：「Move 确认早于 on_end」的次序由帧循环排空保证。
        // 取消路径已由 `system_drag::cancel` 先行登记终态时，本次登记被「首登记者赢」自然作废。
        detail::complete_drag_out_session(result.outcome, result.action);
        if (drag_image != nullptr) {
            DeleteObject(drag_image);  // 位图副本已进数据对象，句柄归本线程释放
        }
    }
    if (ole_ready) {
        OleUninitialize();
    }
    {
        const std::scoped_lock lock{mutex_};
        in_flight_ = false;
        current_ = nullptr;  // 让出单槽：此后取消路径不再触达本会话
    }
}

[[nodiscard]] auto drag_out_engine() -> DragOutEngine & {
    // 刻意泄漏的进程级单槽：进程退出由 OS 回收（与事件层注册面同口径）。若改为函数内
    // static 值对象，其析构会在进程退出期 join 拖拽线程——彼时跨 TU 的事件层登记表
    // （drag_payload.cpp 的函数内 static）可能已先行销毁，拖拽线程补登终态即成
    // use-after-destruction；且 `DoDragDrop` 模态循环不保证在退出期自行返回。
    static auto *const ENGINE = new DragOutEngine();
    return *ENGINE;
}

[[nodiscard]] auto drag_out_in_flight_offer() -> std::optional<DragOffer> {
    // 拖入侧落放回调（UI 线程）专用：把「有没有自拖放在途」的问句收敛到一个函数面，
    // 使拖入段不必前展完整引擎类型（前展声明见文件首）。
    return drag_out_engine().in_flight_offer();
}

}  // namespace
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

auto register_win32_drag_out_backend() -> void {
    // 重复注册由事件层「先到者优先」作废（每个窗口的 Impl 构造期都会调一次）。
    detail::register_system_drag_out_backend(detail::SystemDragOutBackend{
        .begin = [](const DragOffer &offer) -> bool { return drag_out_engine().begin(offer); },
        .cancel = []() -> void { drag_out_engine().cancel(); },
    });
}

auto win32_drag_out_note_scale(const float scale) -> void { drag_out_engine().note_scale(scale); }

}  // namespace aurora::detail

#endif  // 平台 ∧ 后端门控（与文件首同款）
