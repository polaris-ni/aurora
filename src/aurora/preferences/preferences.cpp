#include "aurora/preferences/preferences.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#endif

namespace aurora::preferences {

namespace {

/// @brief 保留给元数据的顶级键名（用户数据不应使用此名，否则会被剥离）。
constexpr auto AURORA_PREFERENCE_META_KEY = "__aurora_preference_meta__";

/// @brief 把 `unordered_map<string,double>` 序列化为 JSON 对象（跳过值为 0 的项）。
auto to_json_map(const std::unordered_map<std::string, double> &m) -> Json {
    Json out = Json::object();
    for (const auto &kv : m) {
        if (kv.second != 0.0) {
            out[kv.first] = kv.second;
        }
    }
    return out;
}

/// @brief 从 JSON 对象解析版本/墓碑表。
auto from_json_map(const Json &j) -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> out;
    if (j.is_object()) {
        for (const auto &it : j.items()) {
            out[it.key()] = it.value().is_number() ? it.value().get<double>() : 0.0;
        }
    }
    return out;
}

/**
 * @brief 跨进程 advisory 文件锁（RAII）。
 *
 * 锁定 `<data_file>.lock`，保证多个进程对同一个配置文件的 `flush`/`reload` 互斥、
 * 且读时能读到完整内容。读写均通过锁序列化，避免半写损坏与互相覆盖。
 *
 * - Windows：`CreateFile` 打开锁文件 + `LockFileEx`（独占/共享），析构时 `UnlockFileEx`。
 * - POSIX：`open` 打开锁文件 + `flock(LOCK_EX | LOCK_SH)`。
 */
class FileLock {
  public:
    explicit FileLock(const std::filesystem::path &data_file)
#ifdef AURORA_PLATFORM_WINDOWS
        : m_lock_path(std::filesystem::path(data_file.wstring() + L".lock")){}
#else
        : m_lock_path(std::filesystem::path(data_file.string() + ".lock")) {
    }
#endif
          ~FileLock() {
        unlock();
    }

    FileLock(const FileLock &) = delete;
    auto operator=(const FileLock &) -> FileLock & = delete;
    FileLock(FileLock &&) = delete;
    auto operator=(FileLock &&) -> FileLock & = delete;

    /// @brief 获取锁；`exclusive` 为 true 时独占（写），否则共享（读）。阻塞直到获取成功。
    auto lock(bool exclusive) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
        m_handle = ::CreateFileW(m_lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        OVERLAPPED ov{};
        const DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0u;
        return ::LockFileEx(m_handle, flags, 0, 1, 0, &ov) != 0;
#else
        m_fd = ::open(m_lock_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (m_fd < 0) {
            return false;
        }
        return ::flock(m_fd, exclusive ? LOCK_EX : LOCK_SH) == 0;
#endif
    }

    auto unlock() -> void {
#ifdef AURORA_PLATFORM_WINDOWS
        if (m_handle != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov{};
            ::UnlockFileEx(m_handle, 0, 1, 0, &ov);
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

  private:
#ifdef AURORA_PLATFORM_WINDOWS
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
    std::filesystem::path m_lock_path;
};

/**
 * @brief 从整份磁盘 JSON 中拆出「用户数据」与「meta（versions/tombstones/cleared_at）」。
 * 旧格式（无 meta 键）也能兼容：data 为整个对象，meta 为空。
 */
auto split_meta(const Json &whole, Json &data, std::unordered_map<std::string, double> &versions,
                std::unordered_map<std::string, double> &tombstones, double &cleared_at) -> void {
    data = Json::object();
    versions.clear();
    tombstones.clear();
    cleared_at = 0.0;
    if (!whole.is_object()) {
        return;
    }
    data = whole;
    data.erase(AURORA_PREFERENCE_META_KEY);
    const Json meta = (whole.contains(AURORA_PREFERENCE_META_KEY) && whole[AURORA_PREFERENCE_META_KEY].is_object())
                          ? whole[AURORA_PREFERENCE_META_KEY]
                          : Json::object();
    cleared_at = meta.contains("cleared_at") && meta["cleared_at"].is_number() ? meta["cleared_at"].get<double>() : 0.0;
    if (meta.contains("versions")) {
        versions = from_json_map(meta["versions"]);
    }
    if (meta.contains("tombstones")) {
        tombstones = from_json_map(meta["tombstones"]);
    }
}

} // namespace

// ----- 嵌套 JSON 路径助手（复合点号键） -----

/// @brief 按复合点号键取嵌套值；缺失或路径中断返回 Json{}。
auto resolve_get(const Json &root, const std::string &composite) -> Json {
    const Json *cur = &root;
    std::string_view rem(composite);
    while (true) {
        const auto dot = rem.find('.');
        const std::string seg(rem.substr(0, dot));
        if (!cur->is_object() || !cur->contains(seg)) {
            return Json{};
        }
        cur = &(*cur)[seg];
        if (dot == std::string_view::npos) {
            break;
        }
        rem = rem.substr(dot + 1);
    }
    return *cur;
}

/// @brief 按复合点号键写入嵌套值（中间段自动建对象容器）。
auto resolve_set(Json &root, const std::string &composite, Json value) -> void {
    Json *cur = &root;
    std::string_view rem(composite);
    while (true) {
        const auto dot = rem.find('.');
        const std::string seg(rem.substr(0, dot));
        if (!cur->is_object()) {
            *cur = Json::object();
        }
        if (dot == std::string_view::npos) {
            (*cur)[seg] = std::move(value);
            return;
        }
        if (!cur->contains(seg) || !(*cur)[seg].is_object()) {
            (*cur)[seg] = Json::object();
        }
        cur = &(*cur)[seg];
        rem = rem.substr(dot + 1);
    }
}

/// @brief 按复合点号键删除嵌套值（路径中断则无操作）。
auto resolve_erase(Json &root, const std::string &composite) -> void {
    Json *cur = &root;
    std::string_view rem(composite);
    while (true) {
        const auto dot = rem.find('.');
        const std::string seg(rem.substr(0, dot));
        if (!cur->is_object() || !cur->contains(seg)) {
            return;
        }
        if (dot == std::string_view::npos) {
            cur->erase(seg);
            return;
        }
        cur = &(*cur)[seg];
        rem = rem.substr(dot + 1);
    }
}

/// @brief 把嵌套 JSON 拍平为复合点号键 → 叶子值的平面表（递归展开所有对象）。
auto flatten(const Json &root) -> std::unordered_map<std::string, Json> {
    std::unordered_map<std::string, Json> out;
    struct Frame {
        const Json *node;
        std::string prefix;
    };
    std::vector<Frame> stack{ { .node = &root, .prefix = "" } };
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        if (!f.node->is_object()) {
            continue;
        }
        for (const auto &it : f.node->items()) {
            const std::string k = f.prefix.empty() ? it.key() : (f.prefix + "." + it.key());
            if (it.value().is_object()) {
                stack.push_back({ .node = &it.value(), .prefix = k });
            } else {
                out[k] = it.value();
            }
        }
    }
    return out;
}

auto Preferences::default_config_dir() -> std::filesystem::path {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv 在 MSVC/clang-cl 下被标为"不安全"，但它是标准可移植接口
#endif
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); (xdg != nullptr) && ((*xdg) != 0)) {
        return { xdg };
    }
#ifdef AURORA_PLATFORM_WINDOWS
    if (const char *local = std::getenv("LOCALAPPDATA"); (local != nullptr) && ((*local) != 0)) {
        return { local };
    }
#else
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config";
    }
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return std::filesystem::current_path();
}

auto Preferences::registry() -> std::unordered_map<std::string, std::unique_ptr<Preferences>> & {
    static std::unordered_map<std::string, std::unique_ptr<Preferences>> r;
    return r;
}

auto Preferences::registry_mutex() -> std::mutex & {
    static std::mutex m;
    return m;
}

auto Preferences::instance(const std::string &name) -> Preferences & { return instance(name, default_config_dir()); }

auto Preferences::instance(const std::string &name, const std::filesystem::path &dir) -> Preferences & {
    std::filesystem::path file = dir / name;
    if (file.extension().empty()) {
        file += ".json";
    }
    return instance_at(name, std::move(file));
}

auto Preferences::instance_at(const std::string &name, std::filesystem::path file) -> Preferences & {
    std::unique_lock lock(registry_mutex());
    auto &reg = registry();
    const auto it = reg.find(name);
    if (it != reg.end() && it->second) {
        return *it->second;
    }
    auto p = std::make_unique<Preferences>(std::move(file));
    auto &ref = *p;
    reg[name] = std::move(p);
    return ref;
}

auto Preferences::load_from_file() -> void {
    m_load_error.reset();
    m_versions.clear();
    m_tombstones.clear();
    m_cleared_at = 0.0;
    if (m_file.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_file, ec)) {
        m_root = Json::object(); // 文件不存在 → 空配置（构造后由 flush 创建）
        return;
    }
    std::ifstream in(m_file, std::ios::binary);
    if (!in) {
        m_load_error = make_error(ErrorCode::PrefsOpenFailed, "Failed to open config file: " + m_file.string(),
                                  "Check file path and read permission", "", m_file.string());
        m_root = Json::object();
        return;
    }
    try {
        Json whole;
        in >> whole;
        Json data;
        std::unordered_map<std::string, double> versions;
        std::unordered_map<std::string, double> tombstones;
        double cleared_at = 0.0;
        split_meta(whole, data, versions, tombstones, cleared_at);
        m_root = std::move(data);
        m_versions = std::move(versions);
        m_tombstones = std::move(tombstones);
        m_cleared_at = cleared_at;
        // 应用持久化的墓碑/清空纪元，得到初始内存视图（不复活已删除键）。
        reconcile(m_root, m_versions);
    } catch (const std::exception &e) {
        m_load_error =
            make_error(ErrorCode::PrefsParseFailed, std::string("Config file JSON parse failed: ") + e.what(),
                       "Check whether file is valid JSON", "", m_file.string());
        m_root = Json::object();
    }
}

// NOLINTNEXTLINE(*-function-cognitive-complexity)
auto Preferences::reconcile(const Json &on_disk, const std::unordered_map<std::string, double> &disk_versions) -> void {
    // 把嵌套 m_root / on_disk 拍平为复合点号键平面视图，统一在复合键空间做 LWW/墓碑/清空纪元。
    const auto mem = flatten(m_root);
    const auto disk = flatten(on_disk);

    // 收集所有候选复合键（内存、磁盘、版本表、墓碑表）。
    std::set<std::string> all;
    for (const auto &key : mem | std::views::keys) {
        all.insert(key);
    }
    for (const auto &key : disk | std::views::keys) {
        all.insert(key);
    }
    for (const auto &key : m_versions | std::views::keys) {
        all.insert(key);
    }
    for (const auto &key : m_tombstones | std::views::keys) {
        all.insert(key);
    }

    std::unordered_map<std::string, Json> merged;
    std::unordered_map<std::string, double> merged_ver;
    for (const auto &k : all) {
        const double tomb = m_tombstones.contains(k) ? m_tombstones[k] : 0.0;
        const double ver = m_versions.contains(k) ? m_versions[k] : 0.0;
        // 1) 全局清空纪元命中：版本与墓碑都早于纪元 → 删除。
        if (m_cleared_at > 0.0 && ver < m_cleared_at && tomb < m_cleared_at) {
            continue;
        }
        // 2) 墓碑胜出（LWW：删除时间戳晚于写入版本）→ 删除。墓碑持续保留以阻止旧副本复活。
        if (tomb > ver) {
            continue;
        }
        // 3) 存活：按版本决定取值（仅本进程显式 set 的版本参与 LWW；仅加载的键让位于磁盘新值）。
        const double d_ver = disk_versions.contains(k) ? disk_versions.at(k) : 0.0;
        Json val{};
        bool have_val = false;
        if (mem.contains(k)) {
            if (ver < d_ver) {
                val = disk.contains(k) ? disk.at(k) : Json{};
                merged_ver[k] = d_ver;
            } else {
                val = mem.at(k);
                if (!merged_ver.contains(k)) {
                    merged_ver[k] = ver;
                }
            }
            have_val = true;
        } else if (disk.contains(k)) {
            val = disk.at(k);
            if (!merged_ver.contains(k)) {
                merged_ver[k] = d_ver;
            }
            have_val = true;
        }
        if (have_val) {
            merged[k] = std::move(val);
        }
        // 否则既无内存值也无磁盘值（仅墓碑/版本占位）→ 不创建值。
    }

    // 由合并后的复合键平面表重建嵌套 m_root。
    m_root = Json::object();
    for (const auto &kv : merged) {
        resolve_set(m_root, kv.first, kv.second);
    }
    m_versions = std::move(merged_ver);
}

auto Preferences::contains_impl(const std::string &scope, const std::string &key) const -> bool {
    std::unique_lock lock(m_mutex);
    const std::string composite = scope.empty() ? key : scope + "." + key;
    return !resolve_get(m_root, composite).is_null();
}

auto Preferences::keys_impl(const std::string &scope) const -> std::vector<std::string> {
    std::unique_lock lock(m_mutex);
    std::vector<std::string> out;
    if (scope.empty()) {
        for (const auto &item : m_root.items()) {
            if (!item.value().is_null()) {
                out.push_back(item.key());
            }
        }
        return out;
    }
    const Json sub = resolve_get(m_root, scope);
    if (!sub.is_object()) {
        return out;
    }
    for (const auto &item : sub.items()) {
        out.push_back(item.key());
    }
    return out;
}

auto Preferences::remove_impl(const std::string &scope, const std::string &key) -> void {
    const std::string composite = scope.empty() ? key : scope + "." + key;
    std::unique_lock lock(m_mutex);
    resolve_erase(m_root, composite);
    m_states.erase(composite);
    m_tombstones[composite] = now_ts(); // 标记删除（pending，直到 flush 持久化）
    m_versions.erase(composite);
}

auto Preferences::clear_impl(const std::string &scope) -> void {
    std::unique_lock lock(m_mutex);
    if (scope.empty()) {
        // 全局清空（现有行为）：全局清空纪元 + 已知键墓碑。
        std::vector<std::string> held;
        for (const auto &item : m_root.items()) {
            held.push_back(item.key());
        }
        m_cleared_at = std::max(m_cleared_at, now_ts()); // 全局清空纪元
        for (const auto &k : held) {
            m_tombstones[k] = now_ts(); // 已知键打墓碑，确保本地持有的键被清掉
        }
        m_root = Json::object();
        m_states.clear();
        m_versions.clear();
        return;
    }
    // 分组清空：对该子树所有已知复合键打墓碑（等效逐键可靠删除，跨进程一致）。
    const std::string prefix = scope + ".";
    const auto flat = flatten(m_root);
    std::vector<std::string> to_erase;
    for (const auto &key : flat | std::views::keys) {
        if (key.starts_with(prefix)) {
            to_erase.push_back(key);
        }
    }
    for (const auto &k : to_erase) {
        resolve_erase(m_root, k);
        m_states.erase(k);
        m_tombstones[k] = now_ts();
        m_versions.erase(k);
    }
}

auto Preferences::flush() -> Result<void> {
    if (m_file.empty()) {
        return make_error(
            ErrorCode::PrefsNotPersistent, "Preferences is in memory mode, cannot flush",
            "Specify file path at construction (Preferences(path) / at(path) / with_location(name)) or use "
            "Preferences::instance(name)",
            "", "");
    }
    std::unique_lock lock(m_mutex); // 线程安全：串行化与其他读写
    if (m_opts.auto_create_dir) {
        std::error_code ec;
        std::filesystem::create_directories(m_file.parent_path(), ec);
    }
    FileLock flock(m_file); // 进程安全：跨进程互斥写
    if (!flock.lock(true)) {
        return make_error(ErrorCode::IOFileNotFound, "Failed to acquire config file lock: " + m_file.string(),
                          "Another process may be writing, retry later", "", m_file.string());
    }
    // 读取磁盘现状（其他进程可能已写入或删除键）。
    Json on_disk = Json::object();
    std::unordered_map<std::string, double> disk_versions;
    std::unordered_map<std::string, double> disk_tombstones;
    double disk_cleared_at = 0.0;
    {
        std::error_code ec_disk;
        if (std::filesystem::exists(m_file, ec_disk)) {
            std::ifstream in(m_file, std::ios::binary);
            if (in) {
                try {
                    Json whole;
                    in >> whole;
                    split_meta(whole, on_disk, disk_versions, disk_tombstones, disk_cleared_at);
                } catch (const nlohmann::json::exception &) {
                    // 损坏的临时/残留内容：忽略，以本进程内存为准覆盖。
                    on_disk = Json::object();
                }
            }
        }
    }
    // 合并跨进程知识：清空纪元与墓碑取 max（传播删除）；版本不合并（仅本进程显式 set 的版本参与 LWW）。
    m_cleared_at = std::max(m_cleared_at, disk_cleared_at);
    for (const auto &kv : disk_tombstones) {
        auto &slot = m_tombstones[kv.first];
        slot = std::max(slot, kv.second);
    }
    // 重算内存视图：合并远端新增键、应用墓碑与清空纪元。
    reconcile(on_disk, disk_versions);

    // 序列化：用户数据 + meta（versions / tombstones / cleared_at）。
    Json out = m_root;
    Json meta = Json::object();
    meta["versions"] = to_json_map(m_versions);
    meta["tombstones"] = to_json_map(m_tombstones);
    if (m_cleared_at > 0.0) {
        meta["cleared_at"] = m_cleared_at;
    }
    out[AURORA_PREFERENCE_META_KEY] = meta;
    const std::string content = out.dump(2); // 人类可读、UTF-8（无 BOM）

    // 临时文件名须进程唯一（含 PID），避免多进程共用同一临时文件互相覆盖。
#ifdef AURORA_PLATFORM_WINDOWS
    const auto pid = static_cast<unsigned long>(::GetCurrentProcessId());
    auto tmp = std::filesystem::path(std::wstring(m_file.wstring()) + L"." + std::to_wstring(pid) + L".tmp");
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
    auto tmp = std::filesystem::path(std::string(m_file.string()) + "." + std::to_string(pid) + ".tmp");
#endif
    {
        std::ofstream out_f(tmp, std::ios::binary | std::ios::trunc);
        if (!out_f) {
            return make_error(ErrorCode::PrefsWriteFailed, "Failed to write temp file: " + tmp.string(),
                              "Check whether directory exists and write permission", "", m_file.string());
        }
        out_f << content;
        if (!out_f) {
            return make_error(ErrorCode::PrefsWriteFailed, "Failed to write temp file: " + tmp.string(), "", "",
                              m_file.string());
        }
    }
    // 原子替换：rename 在同文件系统上为原子操作，避免读到半写文件。
    std::error_code ec;
    std::filesystem::rename(tmp, m_file, ec);
    if (ec) {
        return make_error(ErrorCode::PrefsWriteFailed, "Failed to rename temp file: " + ec.message(),
                          "Check disk space and target file permissions", "", m_file.string());
    }
    return {};
}

auto Preferences::reload() -> Result<void> {
    if (m_file.empty()) {
        return make_error(ErrorCode::PrefsNotPersistent, "Preferences is in memory mode, cannot reload",
                          "Specify file path at construction or use Preferences::instance(name)", "", "");
    }
    std::unique_lock lock(m_mutex); // 线程安全
    FileLock flock(m_file);         // 进程安全：读时加共享锁，保证读到完整文件
    if (!flock.lock(false)) {
        return make_error(ErrorCode::IOFileNotFound, "Failed to acquire config file lock: " + m_file.string(),
                          "Another process may be writing, retry later", "", m_file.string());
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_file, ec)) {
        m_root = Json::object();
        m_versions.clear();
        m_tombstones.clear();
        m_cleared_at = 0.0;
        std::vector<std::pair<std::shared_ptr<IStateHolder>, Json>> to_push;
        for (auto &[k, h] : m_states) {
            (void)k;
            to_push.emplace_back(h, Json{});
        }
        for (auto &[h, j] : to_push) {
            h->push(j);
        }
        return {};
    }
    Json whole;
    {
        std::ifstream in(m_file, std::ios::binary);
        if (!in) {
            return make_error(ErrorCode::PrefsOpenFailed, "Failed to open config file: " + m_file.string(), "", "",
                              m_file.string());
        }
        try {
            in >> whole;
        } catch (const std::exception &e) {
            return make_error(ErrorCode::PrefsParseFailed, std::string("Config file JSON parse failed: ") + e.what(),
                              "", "", m_file.string());
        }
    }
    Json on_disk;
    std::unordered_map<std::string, double> disk_versions;
    std::unordered_map<std::string, double> disk_tombstones;
    double disk_cleared_at = 0.0;
    split_meta(whole, on_disk, disk_versions, disk_tombstones, disk_cleared_at);

    // reload 契约：丢弃本地未落盘修改，完全以磁盘为准（reload 即「从文件重载」）。
    m_cleared_at = std::max(m_cleared_at, disk_cleared_at);
    m_versions = disk_versions;
    m_tombstones = disk_tombstones;
    m_root = std::move(on_disk);
    reconcile(m_root, m_versions); // 应用持久化的墓碑/清空纪元

    std::vector<std::pair<std::shared_ptr<IStateHolder>, Json>> to_push;
    for (auto &[k, h] : m_states) {
        const Json j = resolve_get(m_root, k); // k 为复合键，须按嵌套路径寻址
        to_push.emplace_back(h, j.is_null() ? Json{} : j);
    }
    for (auto &[h, j] : to_push) {
        h->push(j);
    }
    return {};
}

} // namespace aurora::preferences
