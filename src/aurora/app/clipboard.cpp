#include "aurora/app/clipboard.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/core/image.h"
#include "aurora/core/platform.h"
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
#include "aurora/app/detail/macos_clipboard.h"  // NSPasteboard 富格式后端（自举登记；平台宏须先就绪）
#endif
#include "aurora/core/result.h"
#include "aurora/core/time.h"
#include "aurora/widget/widget.h"

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

namespace {
/// @brief 单条监听注册（纯多播：每次注册各占一条）。
struct ClipboardListenerEntry {
    ClipboardEventKind kinds = ClipboardEventKind::None;  ///< 订阅的类别位掩码
    std::function<void(ClipboardEventKind)> callback;  ///< 回调（形参为命中类别）
    std::optional<std::weak_ptr<Widget>> guard;  ///< owner 弱守卫（空 = 句柄形态）
    bool cancelled = false;  ///< 是否已注销（快照派发据此立即止损）
};

/// @brief 进程内监听注册表（与后端无关：平台事件驱动，测试经 `emit_*` 模拟触发）。
///
/// 单独成体而非塞进后端：监听注册是应用级状态、不是内容——`clear()` 与后端装卸都不改动它。
struct ClipboardListeners {
    std::mutex mutex;  ///< 保护以下成员
    std::vector<std::shared_ptr<ClipboardListenerEntry>> entries;  ///< 注册序（FIFO 派发）
    std::atomic<std::size_t> guarded{0};  ///< 带 owner 守卫的条目数（为 0 时任何入口都不清扫）
    std::uint64_t last_sweep_ms = 0;  ///< 上次整表清扫时间（0 = 尚未清扫）
};

/// @brief 进程唯一监听状态（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto clipboard_listeners() -> ClipboardListeners & {
    static ClipboardListeners listeners;
    return listeners;
}

/// @brief 入口侧的惰性清扫：守卫条目为 0 时零锁零开销；否则超过节流窗口才整表回收。
///
/// 「本轮新增下一轮生效、本轮取消立即生效」由快照派发保证；清扫只回收 owner 已销毁的条目内存。
auto maybe_sweep_listeners() -> void {
    auto &listeners = clipboard_listeners();
    if (listeners.guarded.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const std::scoped_lock lock{listeners.mutex};
    if (listeners.guarded.load(std::memory_order_relaxed) == 0) {
        return;
    }
    constexpr std::uint64_t sweep_interval_ms = 5000;  // 守卫条目存在时的整表清扫节流窗口
    const auto now = current_timestamp();
    if (listeners.last_sweep_ms != 0 && now - listeners.last_sweep_ms < sweep_interval_ms) {
        return;
    }
    listeners.last_sweep_ms = now;
    std::size_t guarded = 0;
    std::erase_if(listeners.entries, [&guarded](const std::shared_ptr<ClipboardListenerEntry> &entry) {
        if (entry->cancelled || (entry->guard.has_value() && entry->guard->expired())) {
            return true;
        }
        if (entry->guard.has_value()) {
            ++guarded;
        }
        return false;
    });
    listeners.guarded.store(guarded, std::memory_order_relaxed);
}

/// @brief 按命中类别派发一轮：先持锁取快照，再锁外逐条调用。
///
/// 逐条检查注销位与 owner 存活：回调内取消的条目立即止损（含本轮尚未轮到的），回调内新注册的
/// 条目不在快照内，留到下一轮。
auto dispatch_clipboard_event(ClipboardEventKind kind) -> void {
    std::vector<std::shared_ptr<ClipboardListenerEntry>> snapshot;
    {
        auto &listeners = clipboard_listeners();
        const std::scoped_lock lock{listeners.mutex};
        snapshot.reserve(listeners.entries.size());
        for (const auto &entry : listeners.entries) {
            if (clipboard_event_has(entry->kinds, kind)) {
                snapshot.push_back(entry);
            }
        }
    }
    for (const auto &entry : snapshot) {
        if (entry->cancelled) {
            continue;  // 本轮中途被取消
        }
        if (entry->guard.has_value() && entry->guard->expired()) {
            continue;  // owner 已销毁：派发永远逐条判活，清扫只负责回收内存
        }
        entry->callback(kind);
    }
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
    bool populated = false;  ///< 是否已登记过（macOS 自举登记只在从未登记时补位，不覆盖测试后端）
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
    slot.populated = true;
}

auto remove_backend() -> void {
    auto &slot = backend_slot();
    const std::scoped_lock lock{slot.mutex};
    slot.backend = PlatformBackend{};  // 归零即回到「本进程无富格式机制」的立骨期口径
    slot.populated = false;
}

auto active_backend() -> const PlatformBackend * {
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    // NSPasteboard 是应用级资源、不依赖窗口宿主：首次问后端即自举登记，无窗口场景同样可用。
    // 其余平台由宿主守卫登记（Win32 需要 owner 窗口承载延迟渲染与监听）。
    // 只在从未登记时补位：测试后端先于首次问讯安装时不得被平台后端覆盖。
    bool need_bootstrap = false;
    {
        auto &slot = backend_slot();
        const std::scoped_lock lock{slot.mutex};
        need_bootstrap = !slot.populated;
    }
    if (need_bootstrap) {
        static_cast<void>(aurora::detail::macos_clipboard_ensure_registered());
    }
#endif
    auto &slot = backend_slot();
    const std::scoped_lock lock{slot.mutex};
    return &slot.backend;
}

auto backend_has_listener() -> bool { return active_backend()->set_change_listener != nullptr; }

auto fire_ownership_lost() -> void {
    // 锁外派发：应用回调可能再次进入 Clipboard 入口，持锁调用会自死锁。
    dispatch_clipboard_event(ClipboardEventKind::OwnershipLost);
}

auto fire_change() -> void { dispatch_clipboard_event(ClipboardEventKind::ContentChanged); }
}  // namespace clipboard::detail

namespace {
/// @brief 两形态共用的注册实现：校验 → 能力判据 → 入表 → 返回注销凭证。
/// @param kinds 订阅类别位掩码（至少一类）
/// @param callback 通知回调（不得为空）
/// @param guard owner 弱守卫（句柄形态为空）
/// @return 成功为注销凭证；参数错为 `GeneralInvalidArgument`；请求内容变化但无机制为
/// `ClipboardListenUnsupported`
auto add_clipboard_listener(ClipboardEventKind kinds, std::function<void(ClipboardEventKind)> callback,
                            std::optional<std::weak_ptr<Widget>> guard) -> Result<Subscription> {
    if (!callback) {
        return make_error(ErrorCode::GeneralInvalidArgument, "on_event: callback must not be empty");
    }
    if (static_cast<std::uint8_t>(kinds) == 0U) {
        return make_error(ErrorCode::GeneralInvalidArgument, "on_event: kinds must not be empty");
    }
    // 只有内容变化依赖监听机制；所有权丢失来自既有平台通知，不做能力校验。
    if (clipboard_event_has(kinds, ClipboardEventKind::ContentChanged) && !clipboard::detail::backend_has_listener()) {
        return make_error(ErrorCode::ClipboardListenUnsupported,
                          ErrorParams{{"detail", "this platform has no clipboard change listener mechanism"}});
    }
    maybe_sweep_listeners();
    auto entry = std::make_shared<ClipboardListenerEntry>();
    entry->kinds = kinds;
    entry->callback = std::move(callback);
    entry->guard = std::move(guard);
    auto &listeners = clipboard_listeners();
    {
        const std::scoped_lock lock{listeners.mutex};
        listeners.entries.push_back(entry);
        if (entry->guard.has_value()) {
            listeners.guarded.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return Subscription{[entry, &listeners]() {
        const std::scoped_lock lock{listeners.mutex};
        if (entry->cancelled) {
            return;  // 幂等：重复 reset 不二次计数
        }
        entry->cancelled = true;
        if (entry->guard.has_value()) {
            listeners.guarded.fetch_sub(1, std::memory_order_relaxed);
        }
        std::erase(listeners.entries, entry);
    }};
}
}  // namespace

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
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_text) {
        return platform->set_text(text);
    }
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
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_text) {
        return platform->get_text();
    }
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
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_image) {
        return platform->set_image(img);
    }
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
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_image) {
        return platform->get_image();
    }
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
    maybe_sweep_listeners();
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
    maybe_sweep_listeners();
    if (mime.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument, "set_payload: mime must not be empty");
    }
    // 平台后端登记时走平台实现；未登记（或该子能力缺席）沿用立骨期的「如实报无机制」。
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_payload) {
        return platform->set_payload(mime, bytes);
    }
    return format_unsupported(mime);
}

auto Clipboard::get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_payload) {
        return platform->get_payload(mime);
    }
    return format_unsupported(mime);
}

auto Clipboard::set_files(const std::vector<std::string> &paths) -> Result<bool> {
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_files) {
        return platform->set_files(paths);
    }
    return format_unsupported(AURORA_MIME_URI_LIST);
}

auto Clipboard::get_files() -> Result<std::vector<std::string>> {
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_files) {
        return platform->get_files();
    }
    return format_unsupported(AURORA_MIME_URI_LIST);
}

auto Clipboard::set_html(const std::string &html) -> Result<bool> {
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_html) {
        return platform->set_html(html);
    }
    return format_unsupported(AURORA_MIME_TEXT_HTML);
}

auto Clipboard::get_html() -> Result<std::string> {
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->get_html) {
        return platform->get_html();
    }
    return format_unsupported(AURORA_MIME_TEXT_HTML);
}

auto Clipboard::set_offer(const ClipboardOffer &offer) -> Result<void> {
    maybe_sweep_listeners();
    // 要约不合法是调用方参数问题，与「平台没有机制」是两类失败，分开报（与 system_drag::begin 同口径）。
    if (offer.formats.empty() || !offer.provide) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "set_offer: formats must not be empty and provide must be a valid callback");
    }
    // 平台路径：惰性提供的承载是剪贴板 owner 窗口（Win32 `WM_RENDERFORMAT` 只发给 owner），
    // 无平台后端登记即无 owner 窗口，此时如实报错——不因「新 API 已声明」就谎报可用。
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->set_offer) {
        return platform->set_offer(offer);
    }
    return make_error(
        ErrorCode::ClipboardFormatUnsupported,
        ErrorParams{{"mime", offer.formats.front()}, {"detail", "lazy provide is not wired on this platform yet"}});
}

auto Clipboard::on_event(ClipboardEventKind kinds, std::function<void(ClipboardEventKind)> callback)
    -> Result<Subscription> {
    return add_clipboard_listener(kinds, std::move(callback), std::nullopt);
}

auto Clipboard::on_event(Widget &owner, ClipboardEventKind kinds, std::function<void(ClipboardEventKind)> callback)
    -> Result<Subscription> {
    const std::weak_ptr<Widget> guard = owner.weak_from_this();
    if (guard.expired()) {
        return make_error(
            ErrorCode::GeneralInvalidArgument,
            ErrorParams{{"detail", "owner must be managed by std::shared_ptr (weak_from_this() is empty)"}});
    }
    return add_clipboard_listener(kinds, std::move(callback), guard);
}

auto Clipboard::clear() -> Result<void> {
    maybe_sweep_listeners();
    if (const auto *platform = clipboard::detail::active_backend(); platform != nullptr && platform->clear) {
        return platform->clear();
    }
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
    // pbcopy 以空输入重写 pasteboard：旧内容（含非文本格式）整体被替换，读回为空串，
    // 与 Win32 `EmptyClipboard` / Linux `xsel --clear` 同语义。工具缺失按 access-failed 如实报。
    const auto cleared = pipe_to_command("pbcopy", std::string{});
    if (cleared) {
        return Result<void>{};
    }
    return make_error(ErrorCode::ClipboardAccessFailed, ErrorParams{{"detail", cleared.error().message}});
#else
    return make_error(ErrorCode::GeneralNotSupported, "clear: this platform has no clipboard implementation");
#endif
}

}  // namespace aurora
