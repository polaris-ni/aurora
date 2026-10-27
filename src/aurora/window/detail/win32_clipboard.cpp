#include "win32_clipboard.h"

#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

// NOLINTBEGIN(readability-identifier-naming): Windows SDK 宏，命名不可改
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOLINTEND(readability-identifier-naming)
#include <windows.h>

#include "aurora/app/clipboard.h"
#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "clipboard_win32_translate.h"

namespace aurora::detail {
namespace {

/// @brief `CF_HTML` 的注册格式名（Windows 侧约定名，不是 MIME；MIME 与它的映射只此一处）。
constexpr const char *AURORA_WIN32_HTML_FORMAT_NAME = "HTML Format";

/// @brief 进程唯一的桥状态：剪贴板是**进程级**资源，多窗口共享同一份 owner 与要约。
struct BridgeState {
    HWND owner = nullptr;                                  ///< 当前剪贴板 owner 窗口（最近登记者）
    bool has_offer = false;                                ///< 是否持有惰性提供要约（即是否持有所有权）
    ClipboardOffer offer;                                  ///< 惰性要约（formats + provide）
    std::unordered_map<std::string, UINT> mime_to_format;  ///< MIME → 剪贴板格式号（含自注册者）
    std::unordered_map<UINT, std::string> format_to_mime;  ///< 剪贴板格式号 → MIME（反查，供枚举用）
    bool listening = false;                                ///< 是否已 `AddClipboardFormatListener`
    int refs = 0;                                          ///< 存活守卫数（归零即注销进程级后端）
};

[[nodiscard]] auto bridge_state() -> BridgeState & {
    static BridgeState state;
    return state;
}

/// @brief 把 Win32 LastError 折进失败 detail（与 `clipboard.cpp` 同款：同一句话在「别的线程正持有
/// 剪贴板」与「当前会话没有桌面」两种成因下对策不同，留下码值才有诊断价值）。
[[nodiscard]] auto win_detail(const char *what) -> std::string {
    return std::string(what) + ", GetLastError=" + std::to_string(GetLastError());
}

[[nodiscard]] auto access_failed(const char *what) -> Error {
    return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", win_detail(what)}});
}

[[nodiscard]] auto write_failed(const char *what) -> Error {
    return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", win_detail(what)}});
}

[[nodiscard]] auto utf8_to_utf16(const std::string &text) -> std::optional<std::u16string> {
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (units <= 0) {
        return std::nullopt;  // 非法 UTF-8：与 set_text 同口径，在动剪贴板之前就拒
    }
    std::u16string out(static_cast<std::size_t>(units) - 1U, u'\0');
    // 平台 API 要求 `LPWSTR`：宽字符缓冲区与 `std::u16string` 的存储同构，此处只做类型桥接。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, reinterpret_cast<LPWSTR>(out.data()), units);
    return out;
}

/// @brief 取 `std::u16string` 的宽字符只读指针（平台 API 形参要求 `LPCWCH`）。
///
/// 只做类型桥接，不重新解释字节：`char16_t` 与 Windows 侧 `wchar_t` 同为 16 位 UTF-16 码元，
/// 缓冲区布局一致。
[[nodiscard]] auto wide_ptr(const std::u16string &text) -> LPCWCH {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<LPCWCH>(text.data());
}

[[nodiscard]] auto utf16_to_utf8(const std::u16string &text) -> std::string {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_ptr(text), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide_ptr(text), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                        nullptr);
    return out;
}

/// @brief MIME → 剪贴板格式号：标准档直映，其余经 `RegisterClipboardFormat` 注册并缓存。
///
/// 缓存的理由：格式号在同一会话内稳定，但 `RegisterClipboardFormat` 每次调用都要进内核查表；
/// 剪贴板写入在复制大位图时可能连续多次命中，缓存后只剩一次。
[[nodiscard]] auto format_of(const std::string &mime) -> std::optional<UINT> {
    auto &state = bridge_state();
    if (const auto it = state.mime_to_format.find(mime); it != state.mime_to_format.end()) {
        return it->second;
    }
    UINT format = 0;
    if (mime == AURORA_MIME_TEXT_PLAIN) {
        format = CF_UNICODETEXT;
    } else if (mime == AURORA_MIME_IMAGE_PNG) {
        format = CF_DIB;  // 图像在 Win32 上的既有落点（见 `set_image`）
    } else if (mime == AURORA_MIME_TEXT_HTML) {
        format = RegisterClipboardFormatA(AURORA_WIN32_HTML_FORMAT_NAME);
    } else {
        format = RegisterClipboardFormatA(mime.c_str());
    }
    if (format == 0) {
        return std::nullopt;  // 注册失败（格式名非法或系统表满）
    }
    state.mime_to_format.emplace(mime, format);
    state.format_to_mime.emplace(format, mime);
    return format;
}

/// @brief 剪贴板格式号 → MIME（供 `available_formats` 如实枚举）。
[[nodiscard]] auto mime_of(UINT format) -> std::optional<std::string> {
    if (format == CF_UNICODETEXT || format == CF_TEXT) {
        return std::string{AURORA_MIME_TEXT_PLAIN};
    }
    if (format == CF_DIB || format == CF_DIBV5) {
        return std::string{AURORA_MIME_IMAGE_PNG};
    }
    const auto &state = bridge_state();
    if (const auto it = state.format_to_mime.find(format); it != state.format_to_mime.end()) {
        return it->second;
    }
    return std::nullopt;
}

/// @brief 把字节写进剪贴板（调用方已持有打开态的剪贴板）。
[[nodiscard]] auto put_bytes(UINT format, const std::vector<std::uint8_t> &bytes) -> std::optional<Error> {
    if (bytes.empty()) {
        return std::nullopt;  // 空载荷不入剪贴板：与「空内容为 no-op」同口径
    }
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (handle == nullptr) {
        return write_failed("GlobalAlloc");
    }
    auto *target = static_cast<std::uint8_t *>(GlobalLock(handle));
    if (target == nullptr) {
        GlobalFree(handle);
        return write_failed("GlobalLock");
    }
    std::memcpy(target, bytes.data(), bytes.size());
    GlobalUnlock(handle);
    if (SetClipboardData(format, handle) == nullptr) {
        GlobalFree(handle);  // 提交失败时所有权未转移，漏掉即每次失败积一块
        return write_failed("SetClipboardData");
    }
    return std::nullopt;
}

/// @brief 读出某格式的字节（调用方已持有打开态的剪贴板）；格式不在为 `Ok(空字节)`。
[[nodiscard]] auto take_bytes(UINT format) -> std::vector<std::uint8_t> {
    const HANDLE handle = GetClipboardData(format);
    if (handle == nullptr) {
        return {};
    }
    const auto *source = static_cast<const std::uint8_t *>(GlobalLock(handle));
    if (source == nullptr) {
        return {};
    }
    const SIZE_T size = GlobalSize(handle);
    std::vector<std::uint8_t> out(size, 0U);
    if (size > 0) {
        std::memcpy(out.data(), source, size);
    }
    GlobalUnlock(handle);
    return out;
}

// ---- 进程级后端单槽的实现面 ----

auto platform_set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes) -> Result<bool> {
    const auto format = format_of(mime);
    if (!format) {
        return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", mime}});
    }
    if (OpenClipboard(bridge_state().owner) == 0) {
        return access_failed("OpenClipboard (set_payload)");
    }
    // 平台路径的语义是「后写者接管整块剪贴板」：既有 set_text 即以 EmptyClipboard 开头，
    // 富格式沿用同一口径，不假装能与上一次写入并存（那是 memory 后端的自由，不是系统剪贴板的）。
    EmptyClipboard();
    const auto error = put_bytes(*format, bytes);
    CloseClipboard();
    return error ? Result<bool>{*error} : Result<bool>{true};
}

auto platform_get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    const auto format = format_of(mime);
    if (!format) {
        return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", mime}});
    }
    if (OpenClipboard(nullptr) == 0) {
        return access_failed("OpenClipboard (get_payload)");
    }
    auto bytes = take_bytes(*format);  // 格式不在即空字节：空内容不是失败
    CloseClipboard();
    return bytes;
}

auto platform_set_files(const std::vector<std::string> &paths) -> Result<bool> {
    std::vector<std::u16string> wide;
    wide.reserve(paths.size());
    for (const auto &path : paths) {
        const auto converted = utf8_to_utf16(path);
        if (!converted) {
            return make_error(ErrorCode::ClipboardWriteFailed,
                              ErrorParams{{"detail", "path is not valid UTF-8"}});
        }
        wide.push_back(*converted);
    }
    const auto encoded = uri_list::encode(paths);
    const std::vector<std::uint8_t> uri_list_bytes{encoded.begin(), encoded.end()};
    const auto hdrop_bytes = clipboard::detail::encode_hdrop(wide);
    if (OpenClipboard(bridge_state().owner) == 0) {
        return access_failed("OpenClipboard (set_files)");
    }
    EmptyClipboard();
    // 同一次打开态内放两档：`CF_HDROP` 供资源管理器等系统侧消费者，`uri-list` 供本库与 GTK 系
    // 应用——两档是同一份语义的两种线上形态，不是两次写入（同一次打开态内它们并存）。
    if (const auto error = put_bytes(CF_HDROP, hdrop_bytes)) {
        CloseClipboard();
        return Result<bool>{*error};
    }
    if (const auto format = format_of(std::string{AURORA_MIME_URI_LIST})) {
        if (const auto error = put_bytes(*format, uri_list_bytes)) {
            CloseClipboard();
            return Result<bool>{*error};
        }
    }
    CloseClipboard();
    return Result<bool>{true};
}

auto platform_get_files() -> Result<std::vector<std::string>> {
    if (OpenClipboard(nullptr) == 0) {
        return access_failed("OpenClipboard (get_files)");
    }
    if (IsClipboardFormatAvailable(CF_HDROP) != 0) {
        const auto decoded = clipboard::detail::decode_hdrop(take_bytes(CF_HDROP));
        CloseClipboard();
        if (!decoded) {
            return make_error(ErrorCode::ClipboardAccessFailed,
                              ErrorParams{{"detail", "CF_HDROP block is malformed"}});
        }
        std::vector<std::string> paths;
        paths.reserve(decoded->size());
        for (const auto &entry : *decoded) {
            paths.emplace_back(utf16_to_utf8(entry));
        }
        return paths;
    }
    auto raw = std::vector<std::uint8_t>{};
    if (const auto format = format_of(std::string{AURORA_MIME_URI_LIST})) {
        raw = take_bytes(*format);
    }
    CloseClipboard();
    if (raw.empty()) {
        return std::vector<std::string>{};  // 无文件清单 = Ok(空清单)
    }
    const std::string text{raw.begin(), raw.end()};  // 物化一份文本视图：uri-list 解码器只吃字符视图
    return uri_list::decode(text);
}

auto platform_set_html(const std::string &html) -> Result<bool> {
    const auto format = format_of(std::string{AURORA_MIME_TEXT_HTML});
    if (!format) {
        return make_error(ErrorCode::ClipboardFormatUnsupported,
                          ErrorParams{{"mime", std::string{AURORA_MIME_TEXT_HTML}}});
    }
    const auto framed = clipboard::detail::build_html_frame(html);
    if (OpenClipboard(bridge_state().owner) == 0) {
        return access_failed("OpenClipboard (set_html)");
    }
    EmptyClipboard();
    const auto error = put_bytes(*format, {framed.begin(), framed.end()});
    CloseClipboard();
    return error ? Result<bool>{*error} : Result<bool>{true};
}

auto platform_get_html() -> Result<std::string> {
    const auto format = format_of(std::string{AURORA_MIME_TEXT_HTML});
    if (!format) {
        return make_error(ErrorCode::ClipboardFormatUnsupported,
                          ErrorParams{{"mime", std::string{AURORA_MIME_TEXT_HTML}}});
    }
    if (OpenClipboard(nullptr) == 0) {
        return access_failed("OpenClipboard (get_html)");
    }
    const auto bytes = take_bytes(*format);
    CloseClipboard();
    if (bytes.empty()) {
        return std::string{};  // 无 HTML = Ok("")
    }
    const std::string text{bytes.begin(), bytes.end()};  // 解帧器只吃字符视图
    const auto parsed = clipboard::detail::parse_html_frame(text);
    if (!parsed) {
        // 跨进程载荷非法：与 clipboard.h 表里「跨进程载荷非法 → ClipboardAccessFailed」同档。
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", "CF_HTML block is malformed"}});
    }
    return *parsed;
}

auto platform_set_offer(const ClipboardOffer &offer) -> Result<void> {
    auto &state = bridge_state();
    if (state.owner == nullptr) {
        // 无 owner 窗口即无 `WM_RENDERFORMAT` 的投递目标：延迟渲染在平台层不成立，如实报错。
        return make_error(ErrorCode::ClipboardFormatUnsupported,
                          ErrorParams{{"mime", offer.formats.front()},
                                      {"detail", "lazy provide needs a clipboard owner window"}});
    }
    if (OpenClipboard(state.owner) == 0) {
        return access_failed("OpenClipboard (set_offer)");
    }
    EmptyClipboard();
    bool declared = false;
    for (const auto &mime : offer.formats) {
        const auto format = format_of(mime);
        if (!format) {
            continue;
        }
        // NULL 数据句柄 = 延迟渲染声明：格式公示给系统，字节等到 `WM_RENDERFORMAT` 才产出。
        if (SetClipboardData(*format, nullptr) != nullptr) {
            declared = true;
        }
    }
    CloseClipboard();
    if (!declared) {
        return make_error(ErrorCode::ClipboardFormatUnsupported,
                          ErrorParams{{"mime", offer.formats.front()},
                                      {"detail", "none of the offered formats could be declared"}});
    }
    state.offer = offer;
    state.has_offer = true;
    return Result<void>{};
}

auto platform_available_formats() -> Result<std::vector<std::string>> {
    if (OpenClipboard(nullptr) == 0) {
        return access_failed("OpenClipboard (available_formats)");
    }
    std::vector<std::string> formats;
    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0U) {
        const auto mime = mime_of(format);
        if (!mime) {
            continue;  // 本库不认识的格式不入列：清单回答的是「你能读到什么」
        }
        if (std::ranges::find(formats, *mime) == formats.end()) {
            formats.emplace_back(*mime);
        }
    }
    CloseClipboard();
    return formats;
}

auto platform_set_change_listener(const std::function<void()> &callback) -> Result<void> {
    auto &state = bridge_state();
    if (!callback) {
        if (state.listening && state.owner != nullptr) {
            RemoveClipboardFormatListener(state.owner);
        }
        state.listening = false;
        return Result<void>{};
    }
    // 监听以 owner 窗口为投递目标：无窗口即无机制，与惰性提供同一条「不谎报」口径。
    if (state.owner == nullptr) {
        return make_error(ErrorCode::ClipboardListenUnsupported,
                          ErrorParams{{"detail", "clipboard change listener needs an owner window"}});
    }
    if (!state.listening && AddClipboardFormatListener(state.owner) == 0) {
        return make_error(ErrorCode::ClipboardListenUnsupported,
                          ErrorParams{{"detail", win_detail("AddClipboardFormatListener")}});
    }
    state.listening = true;
    return Result<void>{};
}

auto install_platform_backend() -> void {
    clipboard::detail::install_backend(clipboard::detail::PlatformBackend{
        .set_payload = platform_set_payload,
        .get_payload = platform_get_payload,
        .set_files = platform_set_files,
        .get_files = platform_get_files,
        .set_html = platform_set_html,
        .get_html = platform_get_html,
        .set_offer = platform_set_offer,
        .available_formats = platform_available_formats,
        .set_change_listener = platform_set_change_listener,
    });
}

/// @brief 安全触发一次回调：应用回调可能抛异常，而本函数的调用点全在**窗口过程 / 析构路径**上。
///
/// 异常穿过窗口过程会直接 terminate（越过 Win32 的消息边界），穿过析构同样不可接受——
/// 剪贴板桥的退位是窗口销毁的收尾动作，不能反过来让销毁失败。
auto fire_safely(const std::function<void()> &fire, const char *what) -> void {
    try {
        fire();
    } catch (...) {
        AURORA_LOG_ERROR("clipboard", what);
    }
}

auto stop_listening() -> void {
    auto &state = bridge_state();
    if (state.listening && state.owner != nullptr) {
        RemoveClipboardFormatListener(state.owner);
    }
    state.listening = false;
}
}  // namespace

// ---- 守卫与消息入口 ----

Win32ClipboardBridge::Win32ClipboardBridge() = default;

Win32ClipboardBridge::~Win32ClipboardBridge() { detach(); }

auto Win32ClipboardBridge::attach(void *owner_hwnd) -> void {
    auto &state = bridge_state();
    if (!installed_) {
        installed_ = true;
        ++state.refs;
        install_platform_backend();
    }
    hwnd_ = owner_hwnd;
    state.owner = static_cast<HWND>(owner_hwnd);  // 最近登记者生效（覆盖式）
    // owner 换了窗口：监听必须跟着迁（旧窗口收不到新窗口该收到的通知）。
    if (state.listening) {
        stop_listening();
        if (state.owner != nullptr && AddClipboardFormatListener(state.owner) != 0) {
            state.listening = true;
        }
    }
}

// 豁免异常逃逸：本函数体内已捕获应用「所有权丢失」回调抛出的异常；残余抛点只来自日志宏内部的
// 字符串分配（仅 OOM 时可达），而本函数同时是 `~Win32ClipboardBridge` 与窗口销毁路径的一环，
// 析构不容许传播异常——让 OOM 走 terminate 比传播出去更符合此处的语义。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto Win32ClipboardBridge::detach() noexcept -> void {
    if (!installed_) {
        return;
    }
    installed_ = false;
    // owner 窗口没了 ⇒ `WM_RENDERFORMAT` 无从投递，本进程事实上已不持有剪贴板：如实触发一次
    // 所有权丢失，让应用释放惰性提供所依赖的缓存，而不是让它继续以为要约还活着。
    //
    // 整段包在 try 里而不只包回调：除应用回调外，`std::function` 的重新赋值同样可能抛，
    // 而本函数处在析构与窗口销毁路径上——异常穿出析构不可接受。
    try {
        auto &state = bridge_state();
        if (state.owner == static_cast<HWND>(hwnd_)) {
            state.owner = nullptr;
            if (state.has_offer) {
                state.has_offer = false;
                state.offer = ClipboardOffer{};
                clipboard::detail::fire_ownership_lost();
            }
        }
        if (--state.refs <= 0) {
            stop_listening();
            state.has_offer = false;
            state.offer = ClipboardOffer{};
            state.refs = 0;
            clipboard::detail::remove_backend();  // 回到「本进程无富格式机制」的立骨期口径
        }
    } catch (...) {
        AURORA_LOG_ERROR("clipboard", "clipboard detach callback threw");
    }
}

auto win32_clipboard_render_format(void *hwnd, unsigned format) -> bool {
    auto &state = bridge_state();
    if (!state.has_offer || state.owner != static_cast<HWND>(hwnd)) {
        return false;  // 没有要约 / 消息不是发给当前 owner：交 DefWindowProc
    }
    const auto mime = mime_of(static_cast<UINT>(format));
    if (!mime || std::ranges::find(state.offer.formats, *mime) == state.offer.formats.end()) {
        return false;  // 该格式不由本进程声明
    }
    const auto produced = state.offer.provide(*mime);
    if (!produced) {
        // 提供失败不提交数据：系统侧该次请求即失败。回调返回 Error 的细节由应用自己留痕，
        // 本层不谎报成功——延迟渲染的「没给数据」必须表现为读不到，而不是给一份空数据。
        return true;
    }
    const auto &bytes = produced.value();
    if (bytes.empty()) {
        return true;  // 空字节同样不提交（空内容不是失败，但也不是一份可交付的载荷）
    }
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (handle == nullptr) {
        return true;
    }
    auto *target = static_cast<std::uint8_t *>(GlobalLock(handle));
    if (target == nullptr) {
        GlobalFree(handle);
        return true;
    }
    std::memcpy(target, bytes.data(), bytes.size());
    GlobalUnlock(handle);
    // 处理 WM_RENDERFORMAT 期间剪贴板由请求方持有，本方**不再 OpenClipboard**：直接提交数据。
    SetClipboardData(static_cast<UINT>(format), handle);
    return true;
}

auto win32_clipboard_destroy_clipboard() -> void {
    auto &state = bridge_state();
    if (!state.has_offer) {
        return;  // 未持有要约 = 没有可失去的所有权（与门面的成对语义一致）
    }
    state.has_offer = false;
    state.offer = ClipboardOffer{};
    fire_safely(clipboard::detail::fire_ownership_lost, "clipboard ownership-lost callback threw");
}

auto win32_clipboard_clipboard_update() -> void {
    fire_safely(clipboard::detail::fire_change, "clipboard change callback threw");
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)
