#include "aurora/event/event.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/image/image_codec.h"

#if defined(AURORA_ENABLE_DEBUG) && defined(AURORA_ENABLE_TEST_HOOKS)
#define AURORA_SYSTEM_DRAG_TEST_BACKEND 1  // NOLINT(*-macro-usage)
#endif

namespace aurora {

namespace {

/// @brief 会话载荷的 MIME 键：三条便利读取各自认的常量，取自 MIME 词汇表单一真源。
constexpr std::string_view AURORA_DRAG_MIME_URI_LIST = AURORA_MIME_URI_LIST;
constexpr std::string_view AURORA_DRAG_MIME_TEXT = AURORA_MIME_TEXT_PLAIN;
constexpr std::string_view AURORA_DRAG_MIME_PNG = AURORA_MIME_IMAGE_PNG;

/// @brief 把 `string_view` 形式的 MIME 常量折成 `unordered_map` 查找所需的 `std::string` 键。
[[nodiscard]] auto mime_key(std::string_view mime) -> std::string { return std::string{mime}; }

}  // namespace

// ---- DragPayload（pimpl）----

struct DragPayload::Impl {
    std::unordered_map<std::string, std::vector<std::uint8_t>> data;  ///< MIME → 原始字节
};

DragPayload::DragPayload() : impl_(std::make_unique<Impl>()) {}

DragPayload::~DragPayload() = default;

auto DragPayload::from_bytes(std::unordered_map<std::string, std::vector<std::uint8_t>> data)
    -> std::shared_ptr<const DragPayload> {
    // 私有构造仅供本工厂使用：实例一律以「只读句柄」形态交出，调用方无法再改其内容。
    auto *raw = new DragPayload();  // NOLINT(*-owning-memory): 随即交 shared_ptr 接管
    raw->impl_->data = std::move(data);
    raw->impl_->data.erase(std::string{});  // 空 MIME 键无意义，直接丢弃
    return std::shared_ptr<const DragPayload>(raw);
}

auto DragPayload::has(const std::string &mime) const -> bool { return impl_->data.contains(mime); }

auto DragPayload::bytes(const std::string &mime) const -> Result<std::vector<std::uint8_t>> {
    const auto it = impl_->data.find(mime);
    if (it == impl_->data.end()) {
        return std::vector<std::uint8_t>{};  // 格式不存在 = Ok(空)：空内容不是失败
    }
    return it->second;
}

auto DragPayload::files() const -> Result<std::vector<std::string>> {
    const auto raw = bytes(mime_key(AURORA_DRAG_MIME_URI_LIST));
    if (!raw) {
        return raw.error();
    }
    // 解码只留可映射为本地路径的条目：非 file scheme 的行（http:// 等）静默跳过，
    // 完整原始行仍可经 bytes(uri-list) 取得。
    return uri_list::decode(std::string_view(
        reinterpret_cast<const char *>(raw.value().data()),  // NOLINT(*-pro-type-reinterpret-cast)
        raw.value().size()));
}

auto DragPayload::text() const -> Result<std::string> {
    const auto raw = bytes(mime_key(AURORA_DRAG_MIME_TEXT));
    if (!raw) {
        return raw.error();
    }
    const auto &buf = raw.value();
    return std::string(buf.begin(), buf.end());
}

auto DragPayload::image() const -> Result<Image> {
    const auto raw = bytes(mime_key(AURORA_DRAG_MIME_PNG));
    if (!raw) {
        return raw.error();
    }
    if (raw.value().empty()) {
        return Image{};  // 无该格式 = Ok(空 Image)
    }
    return image::decode_memory(raw.value());
}

// ---- SystemDropEvent 的便利读取 ----
// 无载荷（enter / over / leave 态的常态）一律按空口径返回：不崩溃、不报错。

auto SystemDropEvent::files() const -> Result<std::vector<std::string>> {
    if (payload == nullptr) {
        return std::vector<std::string>{};
    }
    return payload->files();
}

auto SystemDropEvent::text() const -> Result<std::string> {
    if (payload == nullptr) {
        return std::string{};
    }
    return payload->text();
}

auto SystemDropEvent::image() const -> Result<Image> {
    if (payload == nullptr) {
        return Image{};
    }
    return payload->image();
}

auto SystemDropEvent::bytes(const std::string &mime) const -> Result<std::vector<std::uint8_t>> {
    if (payload == nullptr) {
        return std::vector<std::uint8_t>{};
    }
    return payload->bytes(mime);
}

// ---- 系统拖出会话 ----
// 立骨期各后端均未接线：无测试后端时一律「机制缺失」。这里刻意不引入任何嵌套消息泵
// （Win32 DoDragDrop 的嵌套泵与重入 guard 属接线程设计项）。

#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
namespace {
/// @brief 测试注入点（test-only）：进程内 inert 后端，安装后 `begin` 不触碰平台。
struct SystemDragTestBackend {
    std::mutex mutex;  ///< 保护 active（死亡测试子进程与主进程各自持有独立实例）
    bool active = false;  ///< 后端是否已安装
};

[[nodiscard]] auto system_drag_test_backend() -> SystemDragTestBackend & {
    static SystemDragTestBackend backend;
    return backend;
}
}  // namespace
#endif

auto system_drag::begin(const DragOffer &offer) -> Result<DragAction> {
    // 要约不合法是调用方参数问题，与「平台没有机制」是两类失败，分开报。
    if (offer.formats.empty() || !offer.provide) {
        return make_error(ErrorCode::GeneralInvalidArgument, "system_drag::begin: empty formats or no provide callback");
    }
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    {
        auto &backend = system_drag_test_backend();
        const std::scoped_lock lock{backend.mutex};
        if (backend.active) {
            return DragAction::Copy;  // inert 后端的成功动作（机制存在时的常态协商结果）
        }
    }
#endif
    return make_error(ErrorCode::SystemDragNotSupported,
                      "system_drag::begin: no system drag-and-drop backend is wired on this platform yet");
}

auto system_drag::install_test_backend() -> bool {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &backend = system_drag_test_backend();
    const std::scoped_lock lock{backend.mutex};
    backend.active = true;
    return true;
#else
    return false;
#endif
}

auto system_drag::reset_test_backend() -> void {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &backend = system_drag_test_backend();
    const std::scoped_lock lock{backend.mutex};
    backend.active = false;
#endif
}

auto system_drag::remove_test_backend() -> bool {
#ifdef AURORA_SYSTEM_DRAG_TEST_BACKEND
    auto &backend = system_drag_test_backend();
    const std::scoped_lock lock{backend.mutex};
    const bool was_active = backend.active;
    backend.active = false;
    return was_active;
#else
    return false;
#endif
}

#undef AURORA_SYSTEM_DRAG_TEST_BACKEND

}  // namespace aurora
