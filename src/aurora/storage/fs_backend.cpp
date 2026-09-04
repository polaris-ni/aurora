// ============================================================================
// fs_backend.cpp — 文件系统后端实现（默认，零依赖，始终编译）
// ----------------------------------------------------------------------------
// 每记录一个 JSON 信封文件；二进制载荷走 sidecar `<id>.bin` + 信封内 `blob_ref`
// （对齐 Core Data「>1MB 二进制落盘 + 存路径」策略，零 base64 膨胀）。原子写
// （临时文件 + rename），可选跨进程 advisory 锁。见 ARCHITECTURE.md §4.8。
// ============================================================================

#include "aurora/storage/fs_backend.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/preferences/preferences.h"

#ifdef AURORA_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN  // NOLINT(*-identifier-naming)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace aurora::storage {

namespace {

constexpr char AURORA_B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// id → 文件名安全编码（base64url，无填充）。
[[nodiscard]] auto b64url_encode(std::string_view in) -> std::string {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    // NOLINTBEGIN(*-pro-bounds-constant-array-index,*-signed-bitwise,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    while (i + 2 < in.size()) {
        const std::uint32_t n = (static_cast<std::uint8_t>(in[i]) << 16U) |
                                (static_cast<std::uint8_t>(in[i + 1]) << 8U) | static_cast<std::uint8_t>(in[i + 2]);
        out.push_back(AURORA_B64_URL[(n >> 18U) & 63U]);
        out.push_back(AURORA_B64_URL[(n >> 12U) & 63U]);
        out.push_back(AURORA_B64_URL[(n >> 6U) & 63U]);
        out.push_back(AURORA_B64_URL[n & 63U]);
        i += 3;
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint8_t>(in[i]) << 16U;
        out.push_back(AURORA_B64_URL[(n >> 18U) & 63U]);
        out.push_back(AURORA_B64_URL[(n >> 12U) & 63U]);
    } else if (rem == 2) {
        const std::uint32_t n =
            (static_cast<std::uint8_t>(in[i]) << 16U) | (static_cast<std::uint8_t>(in[i + 1]) << 8U);
        out.push_back(AURORA_B64_URL[(n >> 18U) & 63U]);
        out.push_back(AURORA_B64_URL[(n >> 12U) & 63U]);
        out.push_back(AURORA_B64_URL[(n >> 6U) & 63U]);
    }
    // NOLINTEND(*-pro-bounds-constant-array-index,*-signed-bitwise,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return out;
}

[[nodiscard]] auto b64url_val(char c) -> int {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

// 文件名安全编码 → id（base64url 解码，忽略非法字符）。
[[nodiscard]] auto b64url_decode(std::string_view in) -> std::string {
    std::string out;
    std::uint32_t acc = 0;
    std::uint8_t bits = 0;
    for (const char c : in) {
        const int v = b64url_val(c);
        if (v < 0) {
            continue;
        }
        acc = (acc << 6U) | v;  // NOLINT(*-signed-bitwise)
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFFU));
        }
    }
    return out;
}

[[nodiscard]] auto mtime_to_ms(const std::chrono::system_clock::time_point &tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] auto ms_to_mtime(std::int64_t ms) -> std::chrono::system_clock::time_point {
    if (ms <= 0) {
        return {};  // 缺失/未知 → epoch
    }
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// 目录是否可写：写删一个探针文件验证。
[[nodiscard]] auto dir_is_writable(const std::filesystem::path &dir) -> bool {
    const auto probe = dir / ".aurora_write_probe";
    std::error_code ec;
    std::ofstream f(probe, std::ios::binary | std::ios::trunc);
    const bool ok = static_cast<bool>(f);
    f.close();
    std::filesystem::remove(probe, ec);
    return ok;
}

// 原子写文本：临时文件 + rename（失败回退为删目标再 rename）。
[[nodiscard]] auto atomic_write_text(const std::filesystem::path &final_path, const std::string &content,
                                     std::error_code &ec) -> bool {
    auto tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        f << content;
        f.close();
        if (!f) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    std::filesystem::rename(tmp, final_path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(final_path, ec2);
        std::filesystem::rename(tmp, final_path, ec);
        if (ec) {
            return false;
        }
    }
    return true;
}

// 原子写二进制。
[[nodiscard]] auto atomic_write_bytes(const std::filesystem::path &final_path, const StorageBytes &bytes,
                                      std::error_code &ec) -> bool {
    auto tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        if (!bytes.empty()) {
            // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
            f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        f.close();
        if (!f) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    std::filesystem::rename(tmp, final_path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(final_path, ec2);
        std::filesystem::rename(tmp, final_path, ec);
        if (ec) {
            return false;
        }
    }
    return true;
}

}  // namespace

FilesystemBackend::FilesystemBackend(FilesystemOptions opts) : opts_(std::move(opts)) {
    if (opts_.root.empty()) {
        root_ = aurora::preferences::Preferences::default_config_dir() / "aurora_storage";
    } else {
        root_ = opts_.root;
    }

    std::error_code ec;
    if (opts_.auto_create_dir) {
        std::filesystem::create_directories(root_, ec);
        if (ec) {
            return;  // m_open 保持 false
        }
    } else if (!std::filesystem::is_directory(root_, ec)) {
        return;
    }

    if (!dir_is_writable(root_)) {
        return;
    }

    if (opts_.cross_process_lock && !acquire_lock()) {
        return;
    }

    open_ = true;
}

[[nodiscard]] auto FilesystemBackend::acquire_lock() -> bool {
    const auto lock_path = root_ / "aurora_storage.lock";
#ifdef AURORA_PLATFORM_WINDOWS
    const auto wpath = lock_path.wstring();
    const HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    OVERLAPPED ov{};
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) == 0) {
        CloseHandle(h);
        return false;
    }
    lock_ = std::shared_ptr<void>(h, [](void *p) -> void { CloseHandle(p); });
    return true;
#else
    const int fd = ::open(lock_path.string().c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    lock_ = std::shared_ptr<void>(reinterpret_cast<void *>(static_cast<intptr_t>(fd)),
                                  [](void *p) { ::close(static_cast<int>(reinterpret_cast<intptr_t>(p))); });
    return true;
#endif
}

auto FilesystemBackend::put_record(const std::string &id, const StorageRecord &rec) -> Result<void> {
    if (!open_) {
        return Result<void>{make_error(ErrorCode::StorageBackendUnavailable, "Filesystem backend not opened: " + id)};
    }
    const auto enc = b64url_encode(id);
    const auto json_path = root_ / (enc + ".json");

    Json env = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    env["id"] = rec.id.empty() ? id : rec.id;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    env["type"] = rec.type;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    env["version"] = rec.version;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    env["encoding"] = (rec.encoding == StorageEncoding::Binary) ? "binary" : "json";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    env["mtime"] = mtime_to_ms(rec.mtime);

    if (rec.encoding == StorageEncoding::Binary) {
        const auto bin_path = root_ / (enc + ".bin");
        const auto &bytes = std::get<StorageBytes>(rec.payload);
        std::error_code ec;
        if (!atomic_write_bytes(bin_path, bytes, ec)) {
            return Result<void>{
                make_error(ErrorCode::StorageIoError, "Failed to write binary sidecar: " + ec.message())};
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        env["blob_ref"] = enc + ".bin";
    } else {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        env["payload"] = std::get<Json>(rec.payload);
        // 记录由 Binary 改写为 Json 时清理旧 sidecar：残留的 <id>.bin 既是磁盘泄漏，
        // 也让已删除的二进制载荷继续躺在盘上（泄露面）。删除失败不阻断主流程。
        std::error_code rm_ec;
        std::filesystem::remove(root_ / (enc + ".bin"), rm_ec);
    }

    std::error_code ec;
    if (!atomic_write_text(json_path, env.dump(2), ec)) {
        return Result<void>{make_error(ErrorCode::StorageIoError, "Failed to write record file: " + ec.message())};
    }
    return Result<void>{};
}

auto FilesystemBackend::get_record(const std::string &id) -> Result<StorageRecord> {
    if (!open_) {
        return Result<StorageRecord>{
            make_error(ErrorCode::StorageBackendUnavailable, "Filesystem backend not opened: " + id)};
    }
    const auto enc = b64url_encode(id);
    const auto json_path = root_ / (enc + ".json");

    std::error_code ec;
    if (!std::filesystem::exists(json_path, ec)) {
        return Result<StorageRecord>{make_error(ErrorCode::StorageRecordNotFound, "Record file does not exist: " + id)};
    }

    std::ifstream f(json_path, std::ios::binary);
    if (!f) {
        return Result<StorageRecord>{make_error(ErrorCode::StorageIoError, "Failed to open record file: " + id)};
    }
    std::stringstream ss;
    ss << f.rdbuf();

    Json env;
    try {
        env = Json::parse(ss.str());
    } catch (...) {
        return Result<StorageRecord>{make_error(ErrorCode::StorageRecordCorrupt, "Record JSON parse failed: " + id)};
    }
    if (!env.is_object()) {
        return Result<StorageRecord>{make_error(ErrorCode::StorageRecordCorrupt, "Record structure invalid: " + id)};
    }

    StorageRecord rec;
    rec.id = env.value("id", id);
    rec.type = env.value("type", "");
    rec.version = env.value("version", 1U);
    const std::string enc_str = env.value("encoding", "json");
    rec.encoding = enc_str == "binary" ? StorageEncoding::Binary : StorageEncoding::Json;
    rec.mtime = ms_to_mtime(env.value("mtime", std::int64_t{0}));

    if (rec.encoding == StorageEncoding::Binary) {
        const auto bin_path = root_ / (enc + ".bin");
        if (!std::filesystem::exists(bin_path, ec)) {
            return Result<StorageRecord>{make_error(ErrorCode::StorageRecordCorrupt, "Binary sidecar missing: " + id)};
        }
        std::ifstream bf(bin_path, std::ios::binary);
        if (!bf) {
            return Result<StorageRecord>{make_error(ErrorCode::StorageIoError, "Failed to open binary sidecar: " + id)};
        }
        const std::string content(std::istreambuf_iterator<char>(bf), std::istreambuf_iterator<char>{});
        std::vector<std::byte> bytes(content.size());
        for (std::size_t i = 0; i < content.size(); ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            bytes[i] = static_cast<std::byte>(content[i]);
        }
        rec.payload = std::move(bytes);
        rec.blob_ref = enc + ".bin";
    } else {
        if (!env.contains("payload")) {
            return Result<StorageRecord>{
                make_error(ErrorCode::StorageRecordCorrupt, "JSON record missing payload: " + id)};
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        rec.payload = env["payload"];
    }
    return Result<StorageRecord>{std::move(rec)};
}

auto FilesystemBackend::remove(const std::string &id) -> Result<void> {
    if (!open_) {
        return Result<void>{make_error(ErrorCode::StorageBackendUnavailable, "Filesystem backend not opened: " + id)};
    }
    const auto enc = b64url_encode(id);
    std::error_code ec;
    std::filesystem::remove(root_ / (enc + ".json"), ec);
    std::filesystem::remove(root_ / (enc + ".bin"), ec);
    return Result<void>{};  // 幂等
}

auto FilesystemBackend::list() -> Result<std::vector<std::string>> {
    if (!open_) {
        return Result<std::vector<std::string>>{
            make_error(ErrorCode::StorageBackendUnavailable, "Filesystem backend not opened")};
    }
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto &p : std::filesystem::directory_iterator(root_, ec)) {
        if (!p.is_regular_file()) {
            continue;
        }
        const auto name = p.path().filename().string();
        if (name.size() > 5 && name.ends_with(".json")) {
            const std::string enc = name.substr(0, name.size() - 5);
            ids.push_back(b64url_decode(enc));
        }
    }
    if (ec) {
        return Result<std::vector<std::string>>{
            make_error(ErrorCode::StorageIoError, "Failed to enumerate directory: " + ec.message())};
    }
    return Result<std::vector<std::string>>{std::move(ids)};
}

}  // namespace aurora::storage