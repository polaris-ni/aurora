#include "aurora/app/clipboard.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/image.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/image/image_codec.h"

#ifdef AURORA_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
#include <array>
#include <cstdio>
#endif

#ifdef AURORA_PLATFORM_MACOS
#include <array>
#include <cstdio>
#endif

#if defined(AURORA_ENABLE_DEBUG) && defined(AURORA_ENABLE_TEST_HOOKS)
#define AURORA_CLIPBOARD_TEST_BACKEND 1  // NOLINT(*-macro-usage)
#include <mutex>
#endif

namespace aurora {

namespace {
/// @brief 本平台缺少指定 MIME 的读写机制（立骨期富格式平台路径的统一失败）。
///
/// 机制缺失是**稳态**：`clipboard-format-unsupported` 的 `retryable` 为 false，调用方应改走
/// `available_formats` 已列的通道，而不是退避重试。
[[nodiscard]] auto format_unsupported(std::string_view mime) -> Error {
    return make_error(ErrorCode::ClipboardFormatUnsupported, ErrorParams{{"mime", std::string{mime}}});
}
}  // namespace

#ifdef AURORA_CLIPBOARD_TEST_BACKEND
namespace {
/// @brief 测试注入点（test-only）：进程内 memory 后端，安装后 set/get 全部改走内存。
///
/// 供测试框架在并行（CTest 进程隔离 + 用例边界复位）下隔离系统剪贴板；
/// 进程全局单例 + 互斥保护。未安装（active 为假）时一切走平台实现，零行为差异。
struct TestBackend {
    std::mutex mutex;  ///< 保护以下成员（死亡测试子进程与主进程各自持有独立实例）
    std::string text;  ///< memory 后端的文本载荷（UTF-8）
    Image image;  ///< memory 后端的图像载荷（RGBA8）
    std::unordered_map<std::string, std::vector<std::uint8_t>> payloads;  ///< 富格式载荷（MIME → 原始字节）
    std::optional<ClipboardOffer> offer;  ///< 惰性提供要约（无 = 未持有；持有即代表本进程持有剪贴板所有权）
    std::unordered_map<std::string, std::vector<std::uint8_t>> rendered;  ///< 要约已渲染格式的缓存（渲染一次即定型）
    bool active = false;  ///< 后端是否已安装
};

/// @brief 进程唯一后端实例（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto test_backend() -> TestBackend & {
    static TestBackend backend;
    return backend;
}

/// @brief 文本 → 字节载荷（富格式通道一律按字节存，与平台剪贴板的跨进程字节口径一致）。
[[nodiscard]] auto to_bytes(const std::string &text) -> std::vector<std::uint8_t> { return {text.begin(), text.end()}; }

/// @brief 字节载荷 → 文本（uri-list / html 读取侧的还原；字节本身由本进程写入，无非法编码风险）。
[[nodiscard]] auto to_text(const std::vector<std::uint8_t> &bytes) -> std::string {
    return {bytes.begin(), bytes.end()};
}

/// @brief PNG 字节 → RGBA8 图像（惰性要约的图像通道）；空字节按空口径返回空 Image
/// （空内容不是失败），解码失败原样上报图像编解码器的结构化错误。
[[nodiscard]] auto decode_or_empty(const std::vector<std::uint8_t> &bytes) -> Result<Image> {
    if (bytes.empty()) {
        return Image{};
    }
    return image::decode_memory(bytes);
}

/// @brief 该 MIME 是否由惰性要约声明（持锁判定）。
[[nodiscard]] auto offer_declares(const TestBackend &backend, const std::string &mime) -> bool {
    return backend.offer.has_value() && std::ranges::find(backend.offer->formats, mime) != backend.offer->formats.end();
}

/// @brief 立即写接管：非空立即写把该 MIME 自惰性要约摘走（与平台「后写者接管该格式」同款）。
auto take_over_from_offer(TestBackend &backend, const std::string &mime) -> void {
    if (!backend.offer.has_value()) {
        return;
    }
    const auto it = std::ranges::find(backend.offer->formats, mime);
    if (it == backend.offer->formats.end()) {
        return;
    }
    backend.offer->formats.erase(it);
    backend.rendered.erase(mime);
}

/// @brief 渲染结果回填缓存（渲染一次即定型，对标平台 `WM_RENDERFORMAT` 后由系统托管）。
auto cache_rendered(TestBackend &backend, const std::string &mime, const std::vector<std::uint8_t> &bytes) -> void {
    const std::scoped_lock lock{backend.mutex};
    backend.rendered[mime] = bytes;
}

/// @brief 提供失败的结构化错误（惰性回调返回 `Error`，或回调为空而格式被真正请求）。
[[nodiscard]] auto provider_failed(const std::string &mime, const std::string &detail) -> Error {
    return make_error(ErrorCode::ClipboardProviderFailed, ErrorParams{{"mime", mime}, {"detail", detail}});
}

/// @brief memory 后端按 MIME 读字节（富格式便利封装的公共路径）：惰性要约优先，其次立即写载荷。
///
/// 三段式持锁：持锁判定通道（已渲染缓存 → 要约 → 载荷）→ **锁外**调 `provide`（应用回调可能
/// 再次进入 Clipboard 入口，持锁调用会自死锁）→ 持锁回填缓存。
/// @return 后端**已安装**时为消费结果；未安装时为 `std::nullopt`，调用方据此回落到平台后端路径
/// （未安装后端 = 走平台实现，不是「平台也没有机制」，两者不可混为一谈）。
[[nodiscard]] auto backend_read_payload(TestBackend &backend, const std::string &mime)
    -> std::optional<Result<std::vector<std::uint8_t>>> {
    std::function<Result<std::vector<std::uint8_t>>(const std::string &)> provide;
    {
        const std::scoped_lock lock{backend.mutex};
        if (!backend.active) {
            return std::nullopt;
        }
        if (const auto it = backend.rendered.find(mime); it != backend.rendered.end()) {
            return it->second;
        }
        if (offer_declares(backend, mime)) {
            if (backend.offer) {
                provide = backend.offer->provide;  // 锁外调用：拷贝回调而非持锁调用
            }
        } else {
            const auto it = backend.payloads.find(mime);
            return it == backend.payloads.end() ? std::vector<std::uint8_t>{} : it->second;  // 格式不存在 = Ok(空)
        }
    }
    const auto rendered = provide(mime);
    if (!rendered) {
        return provider_failed(mime, rendered.error().message);
    }
    cache_rendered(backend, mime, rendered.value());
    return rendered.value();
}
}  // namespace
#endif

namespace {
/// @brief 进程内监听注册与监听能力标记（与后端无关：真实后端接线后由平台事件驱动，memory
/// 后端由 `emit_*` 模拟触发驱动）。
///
/// 单独成体而非塞进 TestBackend：监听注册是应用级状态、不是内容——`clear()` 与后端卸载都不该
/// 顺手清掉它；而「本平台有无监听机制」在平台路径上恒为假（本轮未接），只有 memory 后端
/// 安装时才模拟为真。
struct ClipboardListeners {
    std::mutex mutex;  ///< 保护以下成员
    std::function<void()> on_ownership_lost;  ///< 所有权丢失回调（空 = 未注册）
    std::function<void()> on_change;  ///< 内容变化回调（空 = 未注册）
    bool listen_supported = false;  ///< 当前后端 / 平台有无变更监听机制
};

/// @brief 进程唯一监听状态（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto clipboard_listeners() -> ClipboardListeners & {
    static ClipboardListeners listeners;
    return listeners;
}
}  // namespace

namespace clipboard::detail {
namespace {
/// @brief 进程唯一的平台后端槽（函数内 static，规避静态初始化顺序问题）。
///
/// 与 `ClipboardListeners` 分开成体的理由相同：后端是**能力声明**而不是内容，`clear()` 与测试后端
/// 装卸都不该顺手把它拆掉——它由平台桥自己的引用计数守卫（`Win32Host` 持有）决定存亡。
struct BackendSlot {
    std::mutex mutex;  ///< 保护以下成员（桥的登记 / 注销在宿主构造与销毁线程上发生）
    PlatformBackend backend;  ///< 当前生效的后端（字段可缺席；整表空 = 未登记）
};

[[nodiscard]] auto backend_slot() -> BackendSlot & {
    static BackendSlot slot;
    return slot;
}
}  // namespace

auto install_backend(PlatformBackend backend) -> void {
    auto &slot = backend_slot();
    const std::scoped_lock lock{slot.mutex};
    slot.backend = std::move(backend);
}

auto remove_backend() -> void {
    auto &slot = backend_slot();
    const std::scoped_lock lock{slot.mutex};
    slot.backend = PlatformBackend{};  // 归零即回到「本进程无富格式机制」的立骨期口径
}

auto active_backend() -> const PlatformBackend * {
    auto &slot = backend_slot();
    const std::scoped_lock lock{slot.mutex};
    return &slot.backend;
}

auto backend_has_listener() -> bool { return active_backend()->set_change_listener != nullptr; }

auto fire_ownership_lost() -> void {
    std::function<void()> callback;
    {
        auto &listeners = clipboard_listeners();
        const std::scoped_lock lock{listeners.mutex};
        callback = listeners.on_ownership_lost;
    }
    if (callback) {
        callback();  // 锁外触发：应用回调可能再次进入 Clipboard 入口，持锁调用会自死锁
    }
}

auto fire_change() -> void {
    std::function<void()> callback;
    {
        auto &listeners = clipboard_listeners();
        const std::scoped_lock lock{listeners.mutex};
        callback = listeners.on_change;
    }
    if (callback) {
        callback();
    }
}
}  // namespace clipboard::detail

#ifdef AURORA_PLATFORM_WINDOWS
namespace {
/// @brief 把 Win32 LastError 折进失败 detail：同一句「OpenClipboard 失败」在「别的线程正持有
/// 剪贴板」（可重试）与「当前会话没有桌面」（不可重试）两种成因下对策不同，留下码值才有诊断价值。
[[nodiscard]] auto win_detail(const char *what) -> std::string {
    return std::string(what) + ", GetLastError=" + std::to_string(GetLastError());
}
}  // namespace
#endif

#if (defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)) || defined(AURORA_PLATFORM_MACOS)
namespace {

/// @brief 作用域内将 SIGPIPE 置为忽略：Linux/macOS 无标准剪贴板 API，须借
/// xclip/xsel/pbcopy 外壳进程中转。当这些工具缺失（或没有 X display）时子进程会立即退出，
/// 导致后续 fwrite/pclose 向已关闭的管道写数据触发 SIGPIPE 直接杀死本进程（headless CI 等
/// 无剪贴板环境常见，且 ASan 下因执行更慢更易命中风写竞态）。置 SIG_IGN 后写操作返回 EPIPE
/// （fwrite 截断、pclose 返回非零），调用方据此把「剪贴板不可用」折成 ClipboardAccessFailed 报给
/// 调用方，而非崩溃。作用域结束即
/// 恢复原处置，不影响进程其余部分。
struct ScopedSigpipeIgnore {
    using Handler = void (*)(int);
    Handler prev;
    ScopedSigpipeIgnore() : prev(std::signal(SIGPIPE, SIG_IGN)) {}
    ~ScopedSigpipeIgnore() {
        if (prev != SIG_ERR) {
            std::signal(SIGPIPE, prev);
        }
    }
    ScopedSigpipeIgnore(const ScopedSigpipeIgnore &) = delete;
    ScopedSigpipeIgnore &operator=(const ScopedSigpipeIgnore &) = delete;
    // 移动同样禁掉：这个守卫的语义就是「构造时接管信号处置、析构时还原」，搬到别处即还原时机错位。
    // 上一行删掉拷贝后移动本就不会隐式生成，此处补齐是把它写进声明（五法则自查要求显式表态）。
    ScopedSigpipeIgnore(ScopedSigpipeIgnore &&) = delete;
    ScopedSigpipeIgnore &operator=(ScopedSigpipeIgnore &&) = delete;
};

/// @brief 执行命令并向其 stdin 写入数据（用于 xclip/pbcopy 写入剪贴板）。
/// @return 命令退出码为 0 时 Ok；否则 ClipboardAccessFailed，detail 指明是 popen 起不来还是退出码非零。
auto pipe_to_command(const std::string &cmd, const std::string &data) -> Result<bool> {
    // Linux/macOS 无标准剪贴板 API，必须借 xclip/xsel/pbcopy 外壳；命令为硬编码常量、无外部输入拼接，无命令注入风险。
    const ScopedSigpipeIgnore ignore_sigpipe;  // 防缺失/无显示的工具导致 SIGPIPE 杀死进程
    FILE *pipe = popen(cmd.c_str(), "w");  // NOLINT(bugprone-command-processor)
    if (pipe == nullptr) {
        return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", "popen(" + cmd + ") failed"}});
    }
    if (!data.empty()) {
        std::fwrite(data.data(), 1, data.size(), pipe);
    }
    const int ret = pclose(pipe);
    if (ret != 0) {
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", cmd + " exited with code " + std::to_string(ret)}});
    }
    return Result<bool>{true};
}

/// @brief 执行命令并读取其 stdout（用于 xsel/pbpaste 读取剪贴板）。
/// @return Ok(命令输出，UTF-8；空输出 = 剪贴板确实为空)；起不来或退出码非零为 ClipboardAccessFailed。
auto read_from_command(const std::string &cmd) -> Result<std::string> {
    // 读取 pbpaste/xsel -o/xclip -o 输出为硬编码，无外部输入拼接，无命令注入风险
    FILE *pipe = popen(cmd.c_str(), "r");  // NOLINT(bugprone-command-processor)
    if (pipe == nullptr) {
        return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", "popen(" + cmd + ") failed"}});
    }
    std::string result;
    std::array<char, 4096> buf{};
    while (auto *s = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        result += s;
    }
    const int ret = pclose(pipe);
    if (ret != 0) {
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", cmd + " exited with code " + std::to_string(ret)}});
    }
    return result;
}
}  // namespace
#endif

auto Clipboard::set_text(const std::string &text) -> Result<bool> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            if (text.empty()) {
                return Result<bool>{true};  // 契约：空串 no-op，保留既有内容（平台分支同口径）
            }
            take_over_from_offer(backend, std::string{AURORA_MIME_TEXT_PLAIN});  // 非空立即写接管该格式
            backend.text = text;
            return Result<bool>{true};
        }
    }
#endif
#ifdef AURORA_PLATFORM_WINDOWS
    if (text.empty()) {
        return Result<bool>{true};  // 契约：空串 no-op，不触碰剪贴板、保留既有内容
    }
    // 先转码再开剪贴板：MB_ERR_INVALID_CHARS 让非法 UTF-8 在**动任何既有内容之前**就被拒，
    // 失败的那次写入不会把用户原本复制的东西清掉。（默认宽松模式下非法字节会被替换成 U+FFFD，
    // 等于把乱码悄悄塞进剪贴板——那正是本次改签名要消灭的静默失败。）
    const int wchar_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (wchar_len <= 0) {
        return make_error(ErrorCode::ClipboardWriteFailed,
                          ErrorParams{{"detail", win_detail("MultiByteToWideChar(CP_UTF8) rejected the payload")}});
    }
    if (OpenClipboard(nullptr) == 0) {
        return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", win_detail("OpenClipboard")}});
    }
    EmptyClipboard();
    const HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wchar_len) * sizeof(wchar_t));
    if (h == nullptr) {
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", win_detail("GlobalAlloc")}});
    }
    auto *p = static_cast<wchar_t *>(GlobalLock(h));
    if (p == nullptr) {
        GlobalFree(h);  // 句柄尚未交给剪贴板，归本进程释放
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", win_detail("GlobalLock")}});
    }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wchar_len);
    GlobalUnlock(h);
    if (SetClipboardData(CF_UNICODETEXT, h) == nullptr) {
        GlobalFree(h);  // 提交失败时所有权未转移，漏掉即每次失败积一块
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed,
                          ErrorParams{{"detail", win_detail("SetClipboardData(CF_UNICODETEXT)")}});
    }
    CloseClipboard();
    return Result<bool>{true};
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    if (text.empty()) {
        return Result<bool>{true};  // 契约：空串 no-op，保留既有内容
    }
    // 优先 xsel：xsel 写入后 fork 守护进程持有 X selection 并立即返回，不会阻塞调用方；
    // 回退 xclip。注意 xclip 复制后前台常驻（等待 selection 被读取才退出），在 headless
    // 无剪贴板管理器的环境（CI）下 pclose 会永久等待 → 测试挂死，故不作为首选。
    auto primary = pipe_to_command("xsel --clipboard --input 2>/dev/null", text);
    if (primary) {
        return primary;
    }
    auto fallback = pipe_to_command("xclip -selection clipboard 2>/dev/null", text);
    if (fallback) {
        return fallback;
    }
    return make_error(ErrorCode::ClipboardAccessFailed,
                      ErrorParams{{"detail", primary.error().message + "; " + fallback.error().message}});
#elif defined(AURORA_PLATFORM_MACOS)
    if (text.empty()) {
        return Result<bool>{true};  // 契约：空串 no-op，保留既有内容
    }
    return pipe_to_command("pbcopy", text);
#else
    (void)text;
    return make_error(ErrorCode::GeneralNotSupported, "set_text: this platform has no clipboard implementation");
#endif
}

auto Clipboard::get_text() -> Result<std::string> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::string mime{AURORA_MIME_TEXT_PLAIN};
        std::function<Result<std::vector<std::uint8_t>>(const std::string &)> provide;
        bool handled = false;  // 后端活动态 = 本入口已消费（含空文本 Ok("")），不再落平台路径
        std::string immediate;
        {
            const std::scoped_lock lock{backend.mutex};
            if (backend.active) {
                handled = true;
                if (const auto it = backend.rendered.find(mime); it != backend.rendered.end()) {
                    return to_text(it->second);  // 已渲染：缓存优先（渲染一次即定型）
                }
                if (offer_declares(backend, mime)) {
                    if (backend.offer) {
                        provide = backend.offer->provide;  // 锁外调用：拷贝回调而非持锁调用
                    }
                } else {
                    immediate = backend.text;  // 立即写文本通道
                }
            }
        }
        if (handled && !provide) {
            return immediate;
        }
        if (provide) {
            const auto rendered = provide(mime);
            if (!rendered) {
                return provider_failed(mime, rendered.error().message);
            }
            cache_rendered(backend, mime, rendered.value());
            return to_text(rendered.value());
        }
    }
#endif
#ifdef AURORA_PLATFORM_WINDOWS
    if (OpenClipboard(nullptr) == 0) {
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", win_detail("OpenClipboard (get_text)")}});
    }
    std::string result;
    if (const HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        const auto *w = static_cast<const wchar_t *>(GlobalLock(h));
        if (w == nullptr) {
            CloseClipboard();
            return make_error(ErrorCode::ClipboardAccessFailed,
                              ErrorParams{{"detail", win_detail("GlobalLock (get_text)")}});
        }
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            result.resize(static_cast<size_t>(len) - 1);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, result.data(), len, nullptr, nullptr);
        }
        GlobalUnlock(h);
    }  // 无 CF_UNICODETEXT = 剪贴板可读但没有文本，Ok("") 而非失败
    CloseClipboard();
    return result;
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    // xclip 输出可能含尾随换行；xsel --clipboard 更干净。xsel 成功即以其结果为准（空输出
    // = 剪贴板确实没有文本），只有它本身起不来/非零退出才回退 xclip。
    auto primary = read_from_command("xsel --clipboard --output 2>/dev/null");
    if (primary) {
        return primary;
    }
    auto fallback = read_from_command("xclip -selection clipboard -o 2>/dev/null");
    if (fallback) {
        return fallback;
    }
    return make_error(ErrorCode::ClipboardAccessFailed,
                      ErrorParams{{"detail", primary.error().message + "; " + fallback.error().message}});
#elif defined(AURORA_PLATFORM_MACOS)
    return read_from_command("pbpaste");
#else
    return make_error(ErrorCode::GeneralNotSupported, "get_text: this platform has no clipboard implementation");
#endif
}

auto Clipboard::set_image(const Image &img) -> Result<bool> {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast): Win32
    // HGLOBAL 字节搬运不可避免
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) {
                return Result<bool>{true};  // 契约：空图像 no-op，保留既有内容（平台分支同口径）
            }
            take_over_from_offer(backend, std::string{AURORA_MIME_IMAGE_PNG});  // 非空立即写接管该格式
            backend.image = img;
            return Result<bool>{true};
        }
    }
#endif

#ifdef AURORA_PLATFORM_WINDOWS
    if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) {
        return Result<bool>{true};  // 空图像早退（不清除已有内容）
    }
    // 剪贴板载荷上限 + 64 位尺寸算术：stride*h 用 32 位在 w/h ≥ 2^15 量级时回绕，
    // 分配出过小的堆块，后续逐像素拷贝即越界写。同时校验像素缓冲与维度一致，
    // 防止构造不一致的 Image 导致越界读。
    constexpr std::int64_t k_max_clipboard_dim = 16384;
    if (img.width > k_max_clipboard_dim || img.height > k_max_clipboard_dim ||
        img.pixels.size() != static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4U) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "set_image: image too large or pixel buffer size mismatch");  // 早退，不清除已有内容
    }
    if (OpenClipboard(nullptr) == 0) {
        return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", win_detail("OpenClipboard")}});
    }
    EmptyClipboard();
    const int w = img.width;
    const int h = img.height;
    const std::uint64_t stride64 = static_cast<std::uint64_t>(w) * 4U;  // 32bpp
    const std::uint64_t dib_size64 = sizeof(BITMAPINFOHEADER) + (stride64 * static_cast<std::uint64_t>(h));
    const HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(dib_size64));
    if (hg == nullptr) {
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", win_detail("GlobalAlloc")}});
    }
    auto *p = static_cast<std::uint8_t *>(GlobalLock(hg));
    if (p == nullptr) {
        GlobalFree(hg);
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", win_detail("GlobalLock")}});
    }
    auto *bi = reinterpret_cast<BITMAPINFOHEADER *>(p);
    *bi = {};
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = w;
    bi->biHeight = -h;  // 负值 = 自顶向下（origin 左上），避免翻转
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = static_cast<DWORD>(stride64 * static_cast<std::uint64_t>(h));  // 上限内必 < 2^31，不回绕
    std::uint8_t *dst = p + sizeof(BITMAPINFOHEADER);
    const std::uint8_t *src = img.pixels.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t *px = src + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + x) * 4U);
            // RGBA8 → BGRA（CF_DIB 32bpp BI_RGB 期望 BGRX/BGRA）。
            *dst++ = px[2];
            *dst++ = px[1];
            *dst++ = px[0];
            *dst++ = px[3];
        }
    }
    GlobalUnlock(hg);
    if (SetClipboardData(CF_DIB, hg) == nullptr) {
        GlobalFree(hg);  // 提交失败时所有权未转移，漏掉即每次失败积一块
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed,
                          ErrorParams{{"detail", win_detail("SetClipboardData(CF_DIB)")}});
    }
    CloseClipboard();
    return Result<bool>{true};
#else
    (void)img;
    return make_error(ErrorCode::GeneralNotSupported, "set_image: this platform has no clipboard implementation");
#endif
}  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast)

auto Clipboard::get_image() -> Result<Image> {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast): Win32
    // HGLOBAL 字节搬运不可避免
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::string mime{AURORA_MIME_IMAGE_PNG};
        std::function<Result<std::vector<std::uint8_t>>(const std::string &)> provide;
        bool handled = false;  // 后端活动态 = 本入口已消费，不再落平台路径
        Image immediate;
        {
            const std::scoped_lock lock{backend.mutex};
            if (backend.active) {
                handled = true;
                if (const auto it = backend.rendered.find(mime); it != backend.rendered.end()) {
                    return decode_or_empty(it->second);  // 已渲染：缓存优先（渲染一次即定型）
                }
                if (offer_declares(backend, mime)) {
                    if (backend.offer) {
                        provide = backend.offer->provide;  // 锁外调用：拷贝回调而非持锁调用
                    }
                } else {
                    immediate = backend.image;  // 立即写图像通道
                }
            }
        }
        if (handled && !provide) {
            return immediate;
        }
        if (provide) {
            const auto rendered = provide(mime);
            if (!rendered) {
                return provider_failed(mime, rendered.error().message);
            }
            cache_rendered(backend, mime, rendered.value());
            return decode_or_empty(rendered.value());
        }
    }
#endif
#ifdef AURORA_PLATFORM_WINDOWS
    Image out;
    if (OpenClipboard(nullptr) == 0) {
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", win_detail("OpenClipboard (get_image)")}});
    }
    if (IsClipboardFormatAvailable(CF_DIB) == 0) {
        CloseClipboard();
        return out;  // 没有 CF_DIB 格式 = 剪贴板确实无图像，Ok(空 Image) 而非失败
    }
    const HANDLE hg = GetClipboardData(CF_DIB);
    if (hg == nullptr) {
        CloseClipboard();
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", "CF_DIB is advertised but GetClipboardData returned null"}});
    }
    const auto *p = static_cast<const std::uint8_t *>(GlobalLock(hg));
    if (p == nullptr) {
        CloseClipboard();
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", win_detail("GlobalLock (get_image)")}});
    }
    // 剪贴板是跨进程信道：同一桌面上的任意进程都能放入任意 CF_DIB 字节块，故头部字段
    // （biWidth/biHeight/biBitCount/biSize/biClrUsed）全部是不可信输入，必须逐项校验后
    // 才能用于指针运算，否则可越界读出相邻堆内存并随「粘贴的图片」泄露出去。
    const SIZE_T avail = GlobalSize(hg);
    if (avail < sizeof(BITMAPINFOHEADER)) {
        GlobalUnlock(hg);
        CloseClipboard();
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", "CF_DIB block is smaller than its BITMAPINFOHEADER"}});
    }
    const auto *bi = reinterpret_cast<const BITMAPINFOHEADER *>(p);
    const int w = bi->biWidth;
    int hgt = bi->biHeight;
    const bool top_down = (hgt < 0);
    hgt = (hgt == INT_MIN) ? 0 : std::abs(hgt);  // std::abs(INT_MIN) 为 UB
    const int bpp = bi->biBitCount;
    if (w <= 0 || hgt <= 0 || (bpp != 24 && bpp != 32)) {
        GlobalUnlock(hg);
        CloseClipboard();
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", "malformed CF_DIB header (non-positive size or unsupported bpp)"}});
    }
    // 全部用 64 位算术：biSize/biClrUsed 可被构造成 0xFFFFFFF0 之类，32 位下会回绕，
    // 使 off/src_stride 变成看似合法的小值而绕过下面的容量检查。
    const std::uint64_t off = static_cast<std::uint64_t>(bi->biSize) + (static_cast<std::uint64_t>(bi->biClrUsed) * 4U);
    const std::uint64_t src_stride =
        (((static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(bpp)) + 31U) / 32U) * 4U;
    // 像素区必须完整落在剪贴板块内；off 还须至少跳过它自称的头部。
    if (off < sizeof(BITMAPINFOHEADER) || off > avail ||
        src_stride * static_cast<std::uint64_t>(hgt) > static_cast<std::uint64_t>(avail) - off) {
        GlobalUnlock(hg);
        CloseClipboard();
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", "CF_DIB pixel area does not fit its block (rejected as malformed)"}});
    }
    out.width = w;
    out.height = hgt;
    out.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(hgt) * 4U, 0U);
    const std::uint8_t *src = p + off;
    for (int y = 0; y < hgt; ++y) {
        // 自底向上（biHeight>0）时 DIB 首行是图像底部；top-down 时首行是顶部。
        const int sy = top_down ? y : (hgt - 1 - y);
        const std::uint8_t *row = src + (static_cast<std::size_t>(sy) * src_stride);
        for (int x = 0; x < w; ++x) {
            std::uint8_t *px =
                out.pixels.data() + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + x) * 4U);
            if (bpp == 32) {
                const std::uint8_t *s = row + (static_cast<std::size_t>(x) * 4U);
                px[0] = s[2];
                px[1] = s[1];
                px[2] = s[0];
                px[3] = s[3];  // BGRA → RGBA
            } else if (bpp == 24) {
                const std::uint8_t *s = row + (static_cast<std::size_t>(x) * 3U);
                px[0] = s[2];
                px[1] = s[1];
                px[2] = s[0];
                px[3] = 255;
            } else {
                px[0] = px[1] = px[2] = 0;
                px[3] = 255;
            }
        }
    }
    GlobalUnlock(hg);
    CloseClipboard();
    return out;
#else
    return make_error(ErrorCode::GeneralNotSupported, "get_image: this platform has no clipboard implementation");
#endif
}  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast)

auto Clipboard::available_formats() -> Result<std::vector<std::string>> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            std::vector<std::string> formats;
            // 惰性要约格式按声明序入列（顺序即优先级；要约即所有权声明，系统侧同样公示这些格式）。
            if (backend.offer.has_value()) {
                for (const auto &mime : backend.offer->formats) {
                    if (std::ranges::find(formats, mime) == formats.end()) {
                        formats.emplace_back(mime);
                    }
                }
            }
            if (!backend.text.empty()) {
                formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
            }
            if (backend.image.width > 0 && backend.image.height > 0) {
                formats.emplace_back(AURORA_MIME_IMAGE_PNG);
            }
            std::vector<std::string> keys;
            keys.reserve(backend.payloads.size());
            for (const auto &key : backend.payloads | std::views::keys) {
                keys.emplace_back(key);
            }
            // 排序只为给出稳定的清单顺序：unordered_map 的迭代序不确定，而调用方（含单测）会按序比对。
            std::ranges::sort(keys);
            for (auto &key : keys) {
                if (std::ranges::find(formats, key) == formats.end()) {
                    formats.push_back(std::move(key));
                }
            }
            return formats;
        }
    }
#endif
    // 平台后端登记时由其**如实**枚举当前剪贴板持有的格式（含它自己接的富格式与惰性要约声明的
    // 格式）；未登记时退回立骨期口径：只列既有机制覆盖的两档，新 API 已声明不等于格式已可用。
    if (const auto *platform = clipboard::detail::active_backend();
        platform != nullptr && platform->available_formats) {
        return platform->available_formats();
    }
#ifdef AURORA_PLATFORM_WINDOWS
    return std::vector<std::string>{std::string{AURORA_MIME_TEXT_PLAIN}, std::string{AURORA_MIME_IMAGE_PNG}};
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    return std::vector<std::string>{std::string{AURORA_MIME_TEXT_PLAIN}};
#elif defined(AURORA_PLATFORM_MACOS)
    return std::vector<std::string>{std::string{AURORA_MIME_TEXT_PLAIN}};
#else
    return make_error(ErrorCode::GeneralNotSupported,
                      "available_formats: this platform has no clipboard implementation");
#endif
}

auto Clipboard::set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes) -> Result<bool> {
    if (mime.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, "set_payload: mime must not be empty");
    }
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            if (bytes.empty()) {
                return Result<bool>{true};  // 契约：空载荷 no-op，保留既有内容
            }
            take_over_from_offer(backend, mime);  // 非空立即写接管该格式
            backend.payloads[mime] = bytes;
            return Result<bool>{true};
        }
    }
#endif
    // 平台后端登记时走平台实现；未登记（或该子能力缺席）沿用立骨期的「如实报无机制」。
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_payload) {
        return platform->set_payload(mime, bytes);
    }
    return format_unsupported(mime);
}

auto Clipboard::get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    if (const auto consumed = backend_read_payload(test_backend(), mime)) {
        return *consumed;  // 后端已安装：本入口已消费，不再落平台路径
    }
#endif
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_payload) {
        return platform->get_payload(mime);
    }
    return format_unsupported(mime);
}

auto Clipboard::set_files(const std::vector<std::string> &paths) -> Result<bool> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            if (paths.empty()) {
                return Result<bool>{true};  // 契约：空清单 no-op，保留既有内容
            }
            take_over_from_offer(backend, std::string{AURORA_MIME_URI_LIST});  // 非空立即写接管该格式
            // 编码只认本地路径；URL 类内容由调用方改走 set_payload 给原始字节。
            backend.payloads[std::string{AURORA_MIME_URI_LIST}] = to_bytes(uri_list::encode(paths));
            return Result<bool>{true};
        }
    }
#endif
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_files) {
        return platform->set_files(paths);
    }
    return format_unsupported(AURORA_MIME_URI_LIST);
}

auto Clipboard::get_files() -> Result<std::vector<std::string>> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    if (const auto consumed = backend_read_payload(test_backend(), std::string{AURORA_MIME_URI_LIST})) {
        if (!*consumed) {
            return consumed->error();
        }
        if (consumed->value().empty()) {
            return std::vector<std::string>{};  // 无该格式 = Ok(空清单)
        }
        // 解码只留可映射为本地路径的条目：非 file scheme 的行静默跳过，全被跳过即空清单。
        return uri_list::decode(to_text(consumed->value()));
    }
#endif
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_files) {
        return platform->get_files();
    }
    return format_unsupported(AURORA_MIME_URI_LIST);
}

auto Clipboard::set_html(const std::string &html) -> Result<bool> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            if (html.empty()) {
                return Result<bool>{true};  // 契约：空串 no-op，保留既有内容
            }
            take_over_from_offer(backend, std::string{AURORA_MIME_TEXT_HTML});  // 非空立即写接管该格式
            backend.payloads[std::string{AURORA_MIME_TEXT_HTML}] = to_bytes(html);
            return Result<bool>{true};
        }
    }
#endif
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_html) {
        return platform->set_html(html);
    }
    return format_unsupported(AURORA_MIME_TEXT_HTML);
}

auto Clipboard::get_html() -> Result<std::string> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    if (const auto consumed = backend_read_payload(test_backend(), std::string{AURORA_MIME_TEXT_HTML})) {
        if (!*consumed) {
            return consumed->error();
        }
        return to_text(consumed->value());  // 无该格式 = Ok("")：空内容不是失败
    }
#endif
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_html) {
        return platform->get_html();
    }
    return format_unsupported(AURORA_MIME_TEXT_HTML);
}

auto Clipboard::set_offer(const ClipboardOffer &offer) -> Result<void> {
    // 要约不合法是调用方参数问题，与「平台没有机制」是两类失败，分开报（与 system_drag::begin 同口径）。
    if (offer.formats.empty() || !offer.provide) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "set_offer: formats must not be empty and provide must be a valid callback");
    }
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            backend.offer = offer;  // 整体替换旧要约（含其已渲染缓存）
            backend.rendered.clear();
            // 对称的「后写者接管」：要约声明的格式自立即写通道摘走（平台同款：延迟渲染声明接管该格式）。
            for (const auto &mime : offer.formats) {
                if (mime == AURORA_MIME_TEXT_PLAIN) {
                    backend.text.clear();
                } else if (mime == AURORA_MIME_IMAGE_PNG) {
                    backend.image = Image{};
                } else {
                    backend.payloads.erase(mime);
                }
            }
            return Result<void>{};
        }
    }
#endif
    // 平台路径：惰性提供的承载是剪贴板 owner 窗口（Win32 `WM_RENDERFORMAT` 只发给 owner），
    // 无平台后端登记即无 owner 窗口，此时如实报错——不因「新 API 已声明」就谎报可用。
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_offer) {
        return platform->set_offer(offer);
    }
    return make_error(
        ErrorCode::ClipboardFormatUnsupported,
        ErrorParams{{"mime", offer.formats.front()}, {"detail", "lazy provide is not wired on this platform yet"}});
}

auto Clipboard::on_ownership_lost(std::function<void()> callback) -> void {
    auto &listeners = clipboard_listeners();
    const std::scoped_lock lock{listeners.mutex};
    listeners.on_ownership_lost = std::move(callback);  // 替换不叠加；空回调即注销
}

auto Clipboard::on_change(std::function<void()> callback) -> Result<void> {
    auto &listeners = clipboard_listeners();
    const std::scoped_lock lock{listeners.mutex};
    if (!callback) {
        listeners.on_change = nullptr;  // 注销不需要监听机制
        return Result<void>{};
    }
    // 监听机制的来源有二：memory 测试后端（模拟有 / 无）与平台后端（真实机制）。二者任一提供即
    // 注册成功；都未提供时**注册即失败**——不谎报：注册了却收不到通知会让调用方逻辑静默失效。
    if (!listeners.listen_supported && !clipboard::detail::backend_has_listener()) {
        return make_error(ErrorCode::ClipboardListenUnsupported,
                          ErrorParams{{"detail", "this platform has no clipboard change listener mechanism"}});
    }
    listeners.on_change = std::move(callback);
    return Result<void>{};
}

auto Clipboard::clear() -> Result<void> {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            backend.text.clear();
            backend.image = Image{};
            backend.payloads.clear();
            backend.offer.reset();  // 要约即所有权声明，随内容一并清空
            backend.rendered.clear();
            return Result<void>{};
        }
    }
#endif
#ifdef AURORA_PLATFORM_WINDOWS
    if (OpenClipboard(nullptr) == 0) {
        return make_error(ErrorCode::ClipboardAccessFailed,
                          ErrorParams{{"detail", win_detail("OpenClipboard (clear)")}});
    }
    if (EmptyClipboard() == 0) {
        const auto detail = win_detail("EmptyClipboard");
        CloseClipboard();
        return make_error(ErrorCode::ClipboardWriteFailed, ErrorParams{{"detail", detail}});
    }
    CloseClipboard();
    return Result<void>{};
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    // xsel 的 --clear 清空 selection 后即退出，不前台驻留；xclip 无等价的「清空全部格式」入口
    // （写空输入只是置空文本，不等于清空），故不回退——工具缺失按 access-failed 如实报。
    const auto cleared = pipe_to_command("xsel --clipboard --clear 2>/dev/null", std::string{});
    if (cleared) {
        return Result<void>{};
    }
    return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", cleared.error().message}});
#elif defined(AURORA_PLATFORM_MACOS)
    return make_error(ErrorCode::GeneralNotSupported, "clear: this platform has no clipboard clear implementation");
#else
    return make_error(ErrorCode::GeneralNotSupported, "clear: this platform has no clipboard implementation");
#endif
}

// ---- 测试注入点（test-only）------------------------------------------------
// 双宏（AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS）齐备时操作进程内 memory
// 后端；任一关闭时返回 false / no-op（消费端调用始终可编译，平台行为不变）。

auto Clipboard::install_test_backend() -> bool {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        backend.text.clear();
        backend.image = Image{};
        backend.payloads.clear();
        backend.offer.reset();
        backend.rendered.clear();
        backend.active = true;
    }
    {
        // 安装即全量复位：监听注册不是内容，但跨用例隔离要求重装时回到「未注册 + 模拟有监听」初态。
        auto &listeners = clipboard_listeners();
        const std::scoped_lock lock{listeners.mutex};
        listeners.on_ownership_lost = nullptr;
        listeners.on_change = nullptr;
        listeners.listen_supported = true;  // memory 后端模拟「有监听机制」的平台
    }
    return true;
#else
    return false;
#endif
}

auto Clipboard::reset_test_backend() -> void {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    auto &backend = test_backend();
    const std::scoped_lock lock{backend.mutex};
    backend.text.clear();
    backend.image = Image{};
    backend.payloads.clear();
    backend.offer.reset();
    backend.rendered.clear();
#endif
}

auto Clipboard::remove_test_backend() -> bool {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    bool was_active = false;
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        was_active = backend.active;
        backend.active = false;
        backend.text.clear();
        backend.image = Image{};
        backend.payloads.clear();
        backend.offer.reset();
        backend.rendered.clear();
    }
    {
        auto &listeners = clipboard_listeners();
        const std::scoped_lock lock{listeners.mutex};
        listeners.on_ownership_lost = nullptr;
        listeners.on_change = nullptr;
        listeners.listen_supported = false;  // 恢复「平台路径本就无监听机制」的口径
    }
    return was_active;
#else
    return false;
#endif
}

auto Clipboard::emit_ownership_lost() -> void {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    {
        auto &backend = test_backend();
        const std::scoped_lock lock{backend.mutex};
        // 未持有惰性要约 = 没有可失去的所有权（与 on_ownership_lost 的成对语义一致），整体 no-op。
        if (!backend.active || !backend.offer.has_value()) {
            return;
        }
        backend.offer.reset();  // 要约随所有权一并失效（新所有者的内容取代本方）
        backend.rendered.clear();
    }
    // 与平台桥共用同一条触发路径（`WM_DESTROYCLIPBOARD` 到达时走的就是这一条），两条来源的
    // 派发语义因此不可能分岔。
    clipboard::detail::fire_ownership_lost();
#endif
}

auto Clipboard::emit_change() -> void {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    // 同上：与平台桥的 `WM_CLIPBOARDUPDATE` 共用同一条触发路径；不改变内容本身。
    clipboard::detail::fire_change();
#endif
}

auto Clipboard::set_listen_supported(bool supported) -> void {
#ifdef AURORA_CLIPBOARD_TEST_BACKEND
    auto &listeners = clipboard_listeners();
    const std::scoped_lock lock{listeners.mutex};
    listeners.listen_supported = supported;
#else
    (void)supported;
#endif
}

#undef AURORA_CLIPBOARD_TEST_BACKEND

}  // namespace aurora
