#include "aurora/preferences/preferences.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace aurora::preferences {

namespace {

/// @brief 保留给元数据的顶级键名（用户数据不应使用此名，否则会被剥离）。
constexpr auto AURORA_PREFERENCE_META_KEY = "__aurora_preference_meta__";

/// @brief 把 `unordered_map<string,double>` 序列化为 JSON 对象（跳过值为 0 的项）。
auto to_json_map(const std::unordered_map<std::string, double> &m) -> json::Value {
    auto out = json::Value::object();
    for (const auto &kv : m) {
        if (kv.second != 0.0) {
            out.set(kv.first, kv.second);
        }
    }
    return out;
}

/// @brief 从 JSON 对象解析版本/墓碑表。
auto from_json_map(const json::Value &j) -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> out;
    if (j.is_object()) {
        for (const auto &entry : j.entries()) {
            out[std::string(entry.key)] = entry.value.is_number() ? entry.value.as_or<double>(0.0) : 0.0;
        }
    }
    return out;
}

///
/// @brief 跨进程 advisory 文件锁（RAII）。
///
/// 锁定 `<data_file>.lock`，保证多个进程对同一个配置文件的 `flush`/`reload` 互斥、
/// 且读时能读到完整内容。读写均通过锁序列化，避免半写损坏与互相覆盖。
///
/// - Windows：`CreateFile` 打开锁文件 + `LockFileEx`（独占/共享），析构时 `UnlockFileEx`。
/// - POSIX：`open` 打开锁文件 + `flock(LOCK_EX | LOCK_SH)`。
///
class FileLock {
  public:
    explicit FileLock(const std::filesystem::path &data_file)
#ifdef AURORA_PLATFORM_WINDOWS
        : lock_path_(std::filesystem::path(data_file.wstring() + L".lock")){}
#else
        : lock_path_(std::filesystem::path(data_file.string() + ".lock")) {
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
        handle_ = ::CreateFileW(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        OVERLAPPED ov{};
        const DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0U;
        return ::LockFileEx(handle_, flags, 0, 1, 0, &ov) != 0;
#else
        fd_ = ::open(lock_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            return false;
        }
        return ::flock(fd_, exclusive ? LOCK_EX : LOCK_SH) == 0;
#endif
    }

    auto unlock() -> void {
#ifdef AURORA_PLATFORM_WINDOWS
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov{};
            ::UnlockFileEx(handle_, 0, 1, 0, &ov);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

  private:
#ifdef AURORA_PLATFORM_WINDOWS
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    std::filesystem::path lock_path_;
};

/// @brief 从整份磁盘 JSON 中拆出「用户数据」与「meta（versions/tombstones/cleared_at）」。
/// 旧格式（无 meta 键）也能兼容：data 为整个对象，meta 为空。
auto split_meta(const json::Value &whole, json::Value &data, std::unordered_map<std::string, double> &versions,
                std::unordered_map<std::string, double> &tombstones, double &cleared_at) -> void {
    data = json::Value::object();
    versions.clear();
    tombstones.clear();
    cleared_at = 0.0;
    if (!whole.is_object()) {
        return;
    }
    data = whole;
    data.erase(AURORA_PREFERENCE_META_KEY);
    // 指针路径：meta 缺失或不是对象即视为旧格式（无元数据），三类字段一律取空/0。
    const json::Value *meta = whole.at(AURORA_PREFERENCE_META_KEY);
    if ((meta == nullptr) || !meta->is_object()) {
        return;
    }
    cleared_at = meta->as_or<double>("cleared_at", 0.0);
    if (const json::Value *versions_node = meta->at("versions"); versions_node != nullptr) {
        versions = from_json_map(*versions_node);
    }
    if (const json::Value *tombstones_node = meta->at("tombstones"); tombstones_node != nullptr) {
        tombstones = from_json_map(*tombstones_node);
    }
}

}  // namespace

// ----- 嵌套 JSON 路径助手（复合点号键） -----

/// @brief 按复合点号键取嵌套值；缺失或路径中断返回空值。
auto resolve_get(const json::Value &root, const std::string &composite) -> json::Value {
    const json::Value *cur = &root;
    std::string_view rem(composite);
    while (true) {
        const auto dot = rem.find('.');
        const std::string seg(rem.substr(0, dot));
        // at() 在非对象上同样返回 nullptr，故「路径中断」与「键缺失」合并为一次判空。
        const json::Value *next = cur->at(seg);
        if (next == nullptr) {
            return json::Value{};
        }
        cur = next;
        if (dot == std::string_view::npos) {
            break;
        }
        rem = rem.substr(dot + 1);
    }
    return *cur;
}

/// @brief 按复合点号键写入嵌套值（中间段自动建对象容器）。
auto resolve_set(json::Value &root, const std::string &composite, json::Value value) -> void {
    json::Value *cur = &root;
    std::string_view rem(composite);
    while (true) {
        const auto dot = rem.find('.');
        const std::string seg(rem.substr(0, dot));
        if (!cur->is_object()) {
            *cur = json::Value::object();
        }
        if (dot == std::string_view::npos) {
            cur->set(seg, std::move(value));
            return;
        }
        // 写接口会使既有引用失效，故每次插入后重新取址，不复用插入前的指针。
        json::Value *next = cur->at(seg);
        if ((next == nullptr) || !next->is_object()) {
            cur->set(seg, json::Value::object());
            next = cur->at(seg);
        }
        cur = next;
        rem = rem.substr(dot + 1);
    }
}

/// @brief 按复合点号键删除嵌套值（路径中断则无操作）。
auto resolve_erase(json::Value &root, const std::string &composite) -> void {
    json::Value *cur = &root;
    std::string_view rem(composite);
    while (true) {
        const auto dot = rem.find('.');
        const std::string seg(rem.substr(0, dot));
        json::Value *next = cur->at(seg);
        if (next == nullptr) {
            return;
        }
        if (dot == std::string_view::npos) {
            cur->erase(seg);
            return;
        }
        cur = next;
        rem = rem.substr(dot + 1);
    }
}

/// @brief 把嵌套 JSON 拍平为复合点号键 → 叶子值的平面表（递归展开所有对象）。
auto flatten(const json::Value &root) -> std::unordered_map<std::string, json::Value> {
    std::unordered_map<std::string, json::Value> out;
    struct Frame {
        const json::Value *node;
        std::string prefix;
    };
    std::vector<Frame> stack{{.node = &root, .prefix = ""}};
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        if (!f.node->is_object()) {
            continue;
        }
        for (const auto &entry : f.node->entries()) {
            const std::string key(entry.key);
            const std::string k = f.prefix.empty() ? key : (f.prefix + "." + key);
            if (entry.value.is_object()) {
                stack.push_back({.node = &entry.value, .prefix = k});
            } else {
                out[k] = entry.value;
            }
        }
    }
    return out;
}

auto Preferences::default_config_dir() -> std::filesystem::path {
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv 在 MSVC/clang-cl 下被标为"不安全"，但它是标准可移植接口
#endif
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); (xdg != nullptr) && ((*xdg) != 0)) {
        return {xdg};
    }
#ifdef AURORA_PLATFORM_WINDOWS
    if (const char *local = std::getenv("LOCALAPPDATA"); (local != nullptr) && ((*local) != 0)) {
        return {local};
    }
#else
    if (const char *home = std::getenv("HOME"); (home != nullptr) && ((*home) != 0)) {
        return std::filesystem::path(home) / ".config";
    }
#endif
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
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
    auto &slot = reg[name];
    slot = std::make_unique<Preferences>(std::move(file));
    return *slot;
}

auto Preferences::load_from_file() -> void {
    load_error_.reset();
    versions_.clear();
    tombstones_.clear();
    cleared_at_ = 0.0;
    if (file_.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        root_ = json::Value::object();  // 文件不存在 → 空配置（构造后由 flush 创建）
        return;
    }
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        load_error_ = make_error(ErrorCode::PrefsOpenFailed, "Failed to open config file: " + file_.string(),
                                 "Check file path and read permission", "", file_.string());
        root_ = json::Value::object();
        return;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto whole = json::parse(text);
    if (!whole.ok()) {
        // 解析失败语义不变：记入 last_load_error 并以空存储启动（异常改为显式判 Result）。
        load_error_ = make_error(ErrorCode::PrefsParseFailed,
                                 std::string("Config file JSON parse failed: ") + whole.error().message,
                                 "Check whether file is valid JSON", "", file_.string());
        root_ = json::Value::object();
        return;
    }
    json::Value data;
    std::unordered_map<std::string, double> versions;
    std::unordered_map<std::string, double> tombstones;
    double cleared_at = 0.0;
    split_meta(whole.value(), data, versions, tombstones, cleared_at);
    root_ = std::move(data);
    versions_ = std::move(versions);
    tombstones_ = std::move(tombstones);
    cleared_at_ = cleared_at;
    // 应用持久化的墓碑/清空纪元，得到初始内存视图（不复活已删除键）。
    reconcile(root_, versions_);
}

auto Preferences::reconcile(const json::Value &on_disk, const std::unordered_map<std::string, double> &disk_versions)
    -> void {
    // 把嵌套 root_ / on_disk 拍平为复合点号键平面视图，统一在复合键空间做 LWW/墓碑/清空纪元。
    const auto mem = flatten(root_);
    const auto disk = flatten(on_disk);

    // 收集所有候选复合键（内存、磁盘、版本表、墓碑表）。
    std::set<std::string> all;
    for (const auto &key : mem | std::views::keys) {
        all.insert(key);
    }
    for (const auto &key : disk | std::views::keys) {
        all.insert(key);
    }
    for (const auto &key : versions_ | std::views::keys) {
        all.insert(key);
    }
    for (const auto &key : tombstones_ | std::views::keys) {
        all.insert(key);
    }

    std::unordered_map<std::string, json::Value> merged;
    std::unordered_map<std::string, double> merged_ver;
    for (const auto &k : all) {
        const double tomb = tombstones_.contains(k) ? tombstones_[k] : 0.0;
        const double ver = versions_.contains(k) ? versions_[k] : 0.0;
        // 1) 全局清空纪元命中：版本与墓碑都早于纪元 → 删除。
        if (cleared_at_ > 0.0 && ver < cleared_at_ && tomb < cleared_at_) {
            continue;
        }
        // 2) 墓碑胜出（LWW：删除时间戳晚于写入版本）→ 删除。墓碑持续保留以阻止旧副本复活。
        if (tomb > ver) {
            continue;
        }
        // 3) 存活：按版本决定取值（仅本进程显式 set 的版本参与 LWW；仅加载的键让位于磁盘新值）。
        const double d_ver = disk_versions.contains(k) ? disk_versions.at(k) : 0.0;
        json::Value val{};
        bool have_val = false;
        if (mem.contains(k)) {
            if (ver < d_ver) {
                val = disk.contains(k) ? disk.at(k) : json::Value{};
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

    // 由合并后的复合键平面表重建嵌套 root_。
    root_ = json::Value::object();
    for (const auto &kv : merged) {
        resolve_set(root_, kv.first, kv.second);
    }
    versions_ = std::move(merged_ver);
}

auto Preferences::contains_impl(const std::string &scope, const std::string &key) const -> bool {
    std::unique_lock lock(mutex_);
    const std::string composite = scope.empty() ? key : scope + "." + key;
    return !resolve_get(root_, composite).is_null();
}

auto Preferences::keys_impl(const std::string &scope) const -> std::vector<std::string> {
    std::unique_lock lock(mutex_);
    std::vector<std::string> out;
    if (scope.empty()) {
        for (const auto &entry : root_.entries()) {
            if (!entry.value.is_null()) {
                out.emplace_back(entry.key);
            }
        }
        return out;
    }
    const json::Value sub = resolve_get(root_, scope);
    if (!sub.is_object()) {
        return out;
    }
    for (const auto &entry : sub.entries()) {
        out.emplace_back(entry.key);
    }
    return out;
}

auto Preferences::remove_impl(const std::string &scope, const std::string &key) -> void {
    const std::string composite = scope.empty() ? key : scope + "." + key;
    std::unique_lock lock(mutex_);
    resolve_erase(root_, composite);
    states_.erase(composite);
    tombstones_[composite] = now_ts();  // 标记删除（pending，直到 flush 持久化）
    versions_.erase(composite);
}

auto Preferences::clear_impl(const std::string &scope) -> void {
    std::unique_lock lock(mutex_);
    if (scope.empty()) {
        // 全局清空（现有行为）：全局清空纪元 + 已知键墓碑。
        std::vector<std::string> held;
        for (const auto &entry : root_.entries()) {
            held.emplace_back(entry.key);
        }
        cleared_at_ = std::max(cleared_at_, now_ts());  // 全局清空纪元
        for (const auto &k : held) {
            tombstones_[k] = now_ts();  // 已知键打墓碑，确保本地持有的键被清掉
        }
        root_ = json::Value::object();
        states_.clear();
        versions_.clear();
        return;
    }
    // 分组清空：对该子树所有已知复合键打墓碑（等效逐键可靠删除，跨进程一致）。
    const std::string prefix = scope + ".";
    const auto flat = flatten(root_);
    std::vector<std::string> to_erase;
    for (const auto &key : flat | std::views::keys) {
        if (key.starts_with(prefix)) {
            to_erase.push_back(key);
        }
    }
    for (const auto &k : to_erase) {
        resolve_erase(root_, k);
        states_.erase(k);
        tombstones_[k] = now_ts();
        versions_.erase(k);
    }
}

auto Preferences::flush() -> Result<void> {
    if (file_.empty()) {
        return make_error(
            ErrorCode::PrefsNotPersistent, "Preferences is in memory mode, cannot flush",
            "Specify file path at construction (Preferences(path) / at(path) / with_location(name)) or use "
            "Preferences::instance(name)",
            "", "");
    }
    std::unique_lock lock(mutex_);  // 线程安全：串行化与其他读写
    if (opts_.auto_create_dir) {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
    }
    FileLock flock(file_);  // 进程安全：跨进程互斥写
    if (!flock.lock(true)) {
        return make_error(ErrorCode::IOFileNotFound, "Failed to acquire config file lock: " + file_.string(),
                          "Another process may be writing, retry later", "", file_.string());
    }
    // 读取磁盘现状（其他进程可能已写入或删除键）。
    json::Value on_disk = json::Value::object();
    std::unordered_map<std::string, double> disk_versions;
    std::unordered_map<std::string, double> disk_tombstones;
    double disk_cleared_at = 0.0;
    {
        std::error_code ec_disk;
        if (std::filesystem::exists(file_, ec_disk)) {
            std::ifstream in(file_, std::ios::binary);
            if (in) {
                const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                auto whole = json::parse(text);
                if (whole.ok()) {
                    split_meta(whole.value(), on_disk, disk_versions, disk_tombstones, disk_cleared_at);
                } else {
                    // 损坏的临时/残留内容：忽略，以本进程内存为准覆盖。
                    on_disk = json::Value::object();
                }
            }
        }
    }
    // 合并跨进程知识：清空纪元与墓碑取 max（传播删除）；版本不合并（仅本进程显式 set 的版本参与 LWW）。
    cleared_at_ = std::max(cleared_at_, disk_cleared_at);
    for (const auto &kv : disk_tombstones) {
        auto &slot = tombstones_[kv.first];
        slot = std::max(slot, kv.second);
    }
    // 重算内存视图：合并远端新增键、应用墓碑与清空纪元。
    reconcile(on_disk, disk_versions);

    // 序列化：用户数据 + meta（versions / tombstones / cleared_at）。
    json::Value out = root_;
    auto meta = json::Value::object();
    meta.set("versions", to_json_map(versions_));
    meta.set("tombstones", to_json_map(tombstones_));
    if (cleared_at_ > 0.0) {
        meta.set("cleared_at", cleared_at_);
    }
    out.set(AURORA_PREFERENCE_META_KEY, std::move(meta));
    auto dumped = json::dump(out, {.indent = 2});  // 人类可读、UTF-8（无 BOM）
    if (!dumped.ok()) {
        return make_error(ErrorCode::PrefsWriteFailed, "Failed to serialize preferences: " + dumped.error().message,
                          "Check whether stored values contain non-finite numbers", "", file_.string());
    }
    const std::string content = std::move(dumped.value());

    // 临时文件名须进程唯一（含 PID），避免多进程共用同一临时文件互相覆盖。
#ifdef AURORA_PLATFORM_WINDOWS
    const auto pid = static_cast<unsigned long>(::GetCurrentProcessId());
    auto tmp = std::filesystem::path(std::wstring(file_.wstring()) + L"." + std::to_wstring(pid) + L".tmp");
#else
    const auto pid = static_cast<unsigned long>(::getpid());
    auto tmp = std::filesystem::path(std::string(file_.string()) + "." + std::to_string(pid) + ".tmp");
#endif
    {
        std::ofstream out_f(tmp, std::ios::binary | std::ios::trunc);
        if (!out_f) {
            return make_error(ErrorCode::PrefsWriteFailed, "Failed to write temp file: " + tmp.string(),
                              "Check whether directory exists and write permission", "", file_.string());
        }
        out_f << content;
        if (!out_f) {
            return make_error(ErrorCode::PrefsWriteFailed, "Failed to write temp file: " + tmp.string(), "", "",
                              file_.string());
        }
    }
    // 原子替换：rename 在同文件系统上为原子操作，避免读到半写文件。
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        return make_error(ErrorCode::PrefsWriteFailed, "Failed to rename temp file: " + ec.message(),
                          "Check disk space and target file permissions", "", file_.string());
    }
    return {};
}

auto Preferences::reload() -> Result<void> {
    if (file_.empty()) {
        return make_error(ErrorCode::PrefsNotPersistent, "Preferences is in memory mode, cannot reload",
                          "Specify file path at construction or use Preferences::instance(name)", "", "");
    }
    std::unique_lock lock(mutex_);  // 线程安全
    FileLock flock(file_);  // 进程安全：读时加共享锁，保证读到完整文件
    if (!flock.lock(false)) {
        return make_error(ErrorCode::IOFileNotFound, "Failed to acquire config file lock: " + file_.string(),
                          "Another process may be writing, retry later", "", file_.string());
    }
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        root_ = json::Value::object();
        versions_.clear();
        tombstones_.clear();
        cleared_at_ = 0.0;
        std::vector<std::pair<std::shared_ptr<IStateHolder>, json::Value>> to_push;
        for (auto &[k, h] : states_) {
            (void)k;
            to_push.emplace_back(h, json::Value{});
        }
        for (auto &[h, j] : to_push) {
            h->push(j);
        }
        return {};
    }
    json::Value whole;
    {
        std::ifstream in(file_, std::ios::binary);
        if (!in) {
            return make_error(ErrorCode::PrefsOpenFailed, "Failed to open config file: " + file_.string(), "", "",
                              file_.string());
        }
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        auto parsed = json::parse(text);
        if (!parsed.ok()) {
            return make_error(ErrorCode::PrefsParseFailed,
                              std::string("Config file JSON parse failed: ") + parsed.error().message, "", "",
                              file_.string());
        }
        whole = std::move(parsed.value());
    }
    json::Value on_disk;
    std::unordered_map<std::string, double> disk_versions;
    std::unordered_map<std::string, double> disk_tombstones;
    double disk_cleared_at = 0.0;
    split_meta(whole, on_disk, disk_versions, disk_tombstones, disk_cleared_at);

    // reload 契约：丢弃本地未落盘修改，完全以磁盘为准（reload 即「从文件重载」）。
    cleared_at_ = std::max(cleared_at_, disk_cleared_at);
    versions_ = disk_versions;
    tombstones_ = disk_tombstones;
    root_ = std::move(on_disk);
    reconcile(root_, versions_);  // 应用持久化的墓碑/清空纪元

    std::vector<std::pair<std::shared_ptr<IStateHolder>, json::Value>> to_push;
    for (auto &[k, h] : states_) {
        const json::Value j = resolve_get(root_, k);  // k 为复合键，须按嵌套路径寻址
        to_push.emplace_back(h, j.is_null() ? json::Value{} : j);
    }
    for (auto &[h, j] : to_push) {
        h->push(j);
    }
    return {};
}

}  // namespace aurora::preferences