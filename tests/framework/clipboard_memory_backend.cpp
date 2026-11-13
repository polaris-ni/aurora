#include "framework/clipboard_memory_backend.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/detail/clipboard_backend.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/image/image_codec.h"

namespace aurora::testing::clipboard_memory {
namespace {

/// @brief 进程内 memory 后端状态（函数内 static，规避静态初始化顺序问题）。
struct MemoryBackend {
    std::mutex mutex;  ///< 保护以下成员
    std::string text;  ///< 文本载荷（UTF-8）
    Image image;  ///< 图像载荷（RGBA8）
    std::unordered_map<std::string, std::vector<std::uint8_t>> payloads;  ///< 富格式载荷（MIME → 原始字节）
    std::optional<ClipboardOffer> offer;  ///< 惰性提供要约（持有即代表本进程持有剪贴板所有权）
    std::unordered_map<std::string, std::vector<std::uint8_t>> rendered;  ///< 要约已渲染格式的缓存
    bool active = false;  ///< 后端是否已安装
    bool listen_supported = true;  ///< 是否模拟「有变更监听机制」
};

/// @brief 进程唯一后端实例。
[[nodiscard]] auto memory_backend() -> MemoryBackend & {
    static MemoryBackend backend;
    return backend;
}

/// @brief 文本 → 字节载荷（富格式通道一律按字节存）。
[[nodiscard]] auto to_bytes(const std::string &text) -> std::vector<std::uint8_t> { return {text.begin(), text.end()}; }

/// @brief 字节载荷 → 文本（uri-list / html 读取侧的还原）。
[[nodiscard]] auto to_text(const std::vector<std::uint8_t> &bytes) -> std::string {
    return {bytes.begin(), bytes.end()};
}

/// @brief PNG 字节 → RGBA8 图像；空字节按空口径返回空 Image，解码失败原样上报。
[[nodiscard]] auto decode_or_empty(const std::vector<std::uint8_t> &bytes) -> Result<Image> {
    if (bytes.empty()) {
        return Image{};
    }
    return image::decode_memory(bytes);
}

/// @brief 该 MIME 是否由惰性要约声明（持锁判定）。
[[nodiscard]] auto offer_declares(const MemoryBackend &backend, const std::string &mime) -> bool {
    return backend.offer.has_value() && std::ranges::find(backend.offer->formats, mime) != backend.offer->formats.end();
}

/// @brief 非空立即写把该 MIME 自惰性要约摘走（与平台「后写者接管该格式」同款）。
auto take_over_from_offer(MemoryBackend &backend, const std::string &mime) -> void {
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
auto cache_rendered(MemoryBackend &backend, const std::string &mime, const std::vector<std::uint8_t> &bytes) -> void {
    const std::scoped_lock lock{backend.mutex};
    backend.rendered[mime] = bytes;
}

/// @brief 提供失败的结构化错误（惰性回调返回 `Error`）。
[[nodiscard]] auto provider_failed(const std::string &mime, const std::string &detail) -> Error {
    return make_error(ErrorCode::ClipboardProviderFailed, ErrorParams{{"mime", mime}, {"detail", detail}});
}

/// @brief 清空内容（不含安装态）：文本 / 图像 / 富格式载荷 / 要约及其渲染缓存。
/// @note 调用方须已持有 `backend.mutex`。
auto clear_content_locked(MemoryBackend &backend) -> void {
    backend.text.clear();
    backend.image = Image{};
    backend.payloads.clear();
    backend.offer.reset();
    backend.rendered.clear();
}

/// @brief 按 MIME 读字节：已渲染缓存 → 惰性要约 → 立即写载荷。
///
/// 三段式持锁：持锁判定通道 → **锁外**调 `provide`（应用回调可能再次进入 Clipboard 入口）→
/// 持锁回填缓存。格式不存在时返回 `Ok(空字节)`（空内容不是失败）。
[[nodiscard]] auto read_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> {
    auto &backend = memory_backend();
    std::function<Result<std::vector<std::uint8_t>>(const std::string &)> provide;
    {
        const std::scoped_lock lock{backend.mutex};
        if (const auto it = backend.rendered.find(mime); it != backend.rendered.end()) {
            return it->second;
        }
        if (offer_declares(backend, mime)) {
            if (backend.offer) {
                provide = backend.offer->provide;  // 锁外调用：拷贝回调而非持锁调用
            }
        } else {
            const auto it = backend.payloads.find(mime);
            return it == backend.payloads.end() ? std::vector<std::uint8_t>{} : it->second;
        }
    }
    const auto rendered = provide(mime);
    if (!rendered) {
        return provider_failed(mime, rendered.error().message);
    }
    cache_rendered(backend, mime, rendered.value());
    return rendered.value();
}

// ---- 后端表字段实现（字段存在即「本后端有该通道」，缺席即回落平台路径）----

auto memory_set_text(const std::string &text) -> Result<bool> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    if (text.empty()) {
        return Result<bool>{true};  // 空串 no-op，保留既有内容
    }
    take_over_from_offer(backend, std::string{AURORA_MIME_TEXT_PLAIN});
    backend.text = text;
    return Result<bool>{true};
}

auto memory_get_text() -> Result<std::string> {
    const std::string mime{AURORA_MIME_TEXT_PLAIN};
    auto &backend = memory_backend();
    std::function<Result<std::vector<std::uint8_t>>(const std::string &)> provide;
    std::string immediate;
    {
        const std::scoped_lock lock{backend.mutex};
        if (const auto it = backend.rendered.find(mime); it != backend.rendered.end()) {
            return to_text(it->second);
        }
        if (offer_declares(backend, mime)) {
            if (backend.offer) {
                provide = backend.offer->provide;
            }
        } else {
            immediate = backend.text;
        }
    }
    if (!provide) {
        return immediate;  // 无系统 / 无要约 / 要约未声明该格式 → 立即写通道（可为空）
    }
    const auto rendered = provide(mime);
    if (!rendered) {
        return provider_failed(mime, rendered.error().message);
    }
    cache_rendered(backend, mime, rendered.value());
    return to_text(rendered.value());
}

auto memory_set_image(const Image &img) -> Result<bool> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) {
        return Result<bool>{true};  // 空图像 no-op，保留既有内容
    }
    take_over_from_offer(backend, std::string{AURORA_MIME_IMAGE_PNG});
    backend.image = img;
    return Result<bool>{true};
}

auto memory_get_image() -> Result<Image> {
    const std::string mime{AURORA_MIME_IMAGE_PNG};
    auto &backend = memory_backend();
    std::function<Result<std::vector<std::uint8_t>>(const std::string &)> provide;
    Image immediate;
    {
        const std::scoped_lock lock{backend.mutex};
        if (const auto it = backend.rendered.find(mime); it != backend.rendered.end()) {
            return decode_or_empty(it->second);
        }
        if (offer_declares(backend, mime)) {
            if (backend.offer) {
                provide = backend.offer->provide;
            }
        } else {
            immediate = backend.image;
        }
    }
    if (!provide) {
        return immediate;
    }
    const auto rendered = provide(mime);
    if (!rendered) {
        return provider_failed(mime, rendered.error().message);
    }
    cache_rendered(backend, mime, rendered.value());
    return decode_or_empty(rendered.value());
}

auto memory_set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes) -> Result<bool> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    if (bytes.empty()) {
        return Result<bool>{true};  // 空载荷 no-op
    }
    take_over_from_offer(backend, mime);
    backend.payloads[mime] = bytes;
    return Result<bool>{true};
}

auto memory_get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>> { return read_payload(mime); }

auto memory_set_files(const std::vector<std::string> &paths) -> Result<bool> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    if (paths.empty()) {
        return Result<bool>{true};  // 空清单 no-op
    }
    take_over_from_offer(backend, std::string{AURORA_MIME_URI_LIST});
    backend.payloads[std::string{AURORA_MIME_URI_LIST}] = to_bytes(uri_list::encode(paths));
    return Result<bool>{true};
}

auto memory_get_files() -> Result<std::vector<std::string>> {
    const auto consumed = read_payload(std::string{AURORA_MIME_URI_LIST});
    if (!consumed) {
        return consumed.error();
    }
    if (consumed.value().empty()) {
        return std::vector<std::string>{};  // 无该格式 = Ok(空清单)
    }
    return uri_list::decode(to_text(consumed.value()));
}

auto memory_set_html(const std::string &html) -> Result<bool> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    if (html.empty()) {
        return Result<bool>{true};  // 空串 no-op
    }
    take_over_from_offer(backend, std::string{AURORA_MIME_TEXT_HTML});
    backend.payloads[std::string{AURORA_MIME_TEXT_HTML}] = to_bytes(html);
    return Result<bool>{true};
}

auto memory_get_html() -> Result<std::string> {
    const auto consumed = read_payload(std::string{AURORA_MIME_TEXT_HTML});
    if (!consumed) {
        return consumed.error();
    }
    return to_text(consumed.value());  // 无该格式 = Ok("")
}

auto memory_set_offer(const ClipboardOffer &offer) -> Result<void> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    backend.offer = offer;  // 整体替换旧要约（含其已渲染缓存）
    backend.rendered.clear();
    // 对称的「后写者接管」：要约声明的格式自立即写通道摘走（延迟渲染声明接管该格式）。
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

auto memory_available_formats() -> Result<std::vector<std::string>> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    std::vector<std::string> formats;
    // 惰性要约格式按声明序入列（顺序即优先级；要约即所有权声明）。
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
    // 排序只为给出稳定清单顺序：unordered_map 迭代序不确定，而调用方按序比对。
    std::ranges::sort(keys);
    for (auto &key : keys) {
        if (std::ranges::find(formats, key) == formats.end()) {
            formats.push_back(std::move(key));
        }
    }
    return formats;
}

auto memory_clear() -> Result<void> {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    clear_content_locked(backend);
    return Result<void>{};
}

// 能力探针字段：门面只判断它是否存在（从不调用），故实现体只需是成功空操作。
// NOLINTNEXTLINE(performance-unnecessary-value-param) 字段签名由 PlatformBackend 固定为按值
auto memory_set_change_listener(std::function<void()> /*callback*/) -> Result<void> { return Result<void>{}; }

/// @brief 组装后端表；`with_listener` 决定是否填能力探针字段。
[[nodiscard]] auto make_table(bool with_listener) -> clipboard::detail::PlatformBackend {
    clipboard::detail::PlatformBackend table;
    table.set_text = memory_set_text;
    table.get_text = memory_get_text;
    table.set_image = memory_set_image;
    table.get_image = memory_get_image;
    table.clear = memory_clear;
    table.set_payload = memory_set_payload;
    table.get_payload = memory_get_payload;
    table.set_files = memory_set_files;
    table.get_files = memory_get_files;
    table.set_html = memory_set_html;
    table.get_html = memory_get_html;
    table.set_offer = memory_set_offer;
    table.available_formats = memory_available_formats;
    if (with_listener) {
        table.set_change_listener = memory_set_change_listener;
    }
    return table;
}

}  // namespace

auto install() -> bool {
    auto &backend = memory_backend();
    {
        const std::scoped_lock lock{backend.mutex};
        clear_content_locked(backend);
        backend.active = true;
        backend.listen_supported = true;  // 安装即回到「模拟有监听机制」初态
    }
    clipboard::detail::install_backend(make_table(true));
    return true;
}

auto reset() -> void {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    clear_content_locked(backend);
}

auto remove() -> bool {
    auto &backend = memory_backend();
    bool was_active = false;
    {
        const std::scoped_lock lock{backend.mutex};
        was_active = backend.active;
        backend.active = false;
        clear_content_locked(backend);
    }
    if (was_active) {
        clipboard::detail::remove_backend();
    }
    return was_active;
}

auto installed() -> bool {
    auto &backend = memory_backend();
    const std::scoped_lock lock{backend.mutex};
    return backend.active;
}

auto set_listen_supported(bool supported) -> void {
    auto &backend = memory_backend();
    bool active = false;
    {
        const std::scoped_lock lock{backend.mutex};
        backend.listen_supported = supported;
        active = backend.active;
    }
    if (active) {
        clipboard::detail::install_backend(make_table(supported));
    }
}

auto emit_change() -> void { clipboard::detail::fire_change(); }

auto emit_ownership_lost() -> void {
    auto &backend = memory_backend();
    {
        const std::scoped_lock lock{backend.mutex};
        // 未持有惰性要约 = 没有可失去的所有权，整体 no-op。
        if (!backend.active || !backend.offer.has_value()) {
            return;
        }
        backend.offer.reset();  // 要约随所有权一并失效
        backend.rendered.clear();
    }
    // 与平台桥共用同一条触发路径（`WM_DESTROYCLIPBOARD` 到达时走的就是这条）。
    clipboard::detail::fire_ownership_lost();
}

}  // namespace aurora::testing::clipboard_memory
