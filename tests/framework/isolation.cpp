#include "isolation.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "aurora/app/clipboard.h"
#include "death_test.h"

#ifdef _WIN32
// 只需要进程与环境变量 API（同 test_death.cpp 的取舍，不自定义 WIN32_LEAN_AND_MEAN）。
#include <windows.h>
#endif

namespace aurora::testing::isolation {

namespace {

namespace fs = std::filesystem;

/// @brief 当前用例的隔离状态（仅 main 线程触达 begin/end，无需加锁）。
struct CaseState {
    std::string temp_dir;  ///< 本轮唯一临时目录（用例结束后清理）
    int seq = 0;  ///< 目录名序号（与时间戳一起保证跨进程唯一）
    bool env_saved = false;  ///< 进程原始 TMP/TMPDIR/TEMP 是否已快照
    std::string saved_tmpdir;  ///< 原始 TMPDIR（end_case 还原用）
    std::string saved_tmp;  ///< 原始 TMP
    std::string saved_temp;  ///< 原始 TEMP
};

[[nodiscard]] auto case_state() -> CaseState& {
    static CaseState state;
    return state;
}

/// @brief 目录是否形如仓库根（codespec/ 与 CMakeLists.txt 同在，均为仓库根独有标志）。
[[nodiscard]] auto looks_like_repo_root(const fs::path& dir) -> bool {
    std::error_code ec;
    return fs::is_directory(dir / "codespec", ec) && !ec && fs::exists(dir / "CMakeLists.txt", ec);
}

/// @brief 从可执行文件位置向上定位仓库根；失败回退从 cwd 向上找。
///
/// CTest 以绝对路径调用 runner（<repo>/build[/x]/aurora_test_runner），向上最多 6 层
/// 足以覆盖任意构建目录布局；安装到仓库外的 runner 定位失败，返回空串（cwd 不动）。
[[nodiscard]] auto locate_repo_root() -> std::string {
    std::error_code ec;
    fs::path dir = fs::absolute(detail::executable_path(), ec).parent_path();
    if (ec || dir.empty()) {
        dir = fs::current_path(ec);
        if (ec) {
            return {};
        }
    }
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        if (looks_like_repo_root(dir)) {
            return dir.string();
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

/// @brief 跨进程安全的进程内环境变量写入（TMPDIR/TMP/TEMP 三处同步接管）。
auto set_env(const char* name, const std::string& value) -> void {
#ifdef _WIN32
    (void)_putenv_s(name, value.c_str());
#else
    (void)setenv(name, value.c_str(), 1);
#endif
}

/// @brief 读取环境变量（未设置返回空串）。
[[nodiscard]] auto get_env(const char* name) -> std::string {
#ifdef _MSC_VER
    char* raw = nullptr;
    std::size_t length = 0;
    (void)_dupenv_s(&raw, &length, name);  // 返回 malloc 副本：包进 unique_ptr（free 作 deleter）RAII 释放
    // deleter 类型显式写为 void(*)(void*)：&std::free 存在 nullptr_t 删除重载，须靠目标类型消歧。
    const std::unique_ptr<char, void (*)(void*)> value{raw, std::free};
    return value ? std::string{value.get()} : std::string{};
#else
    // MinGW 等 CRT 不提供 _dupenv_s（MSVC 专有），getenv 在本框架的进程隔离模型下同样安全。
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

/// @brief 校验（必要时创建）一个可用基目录；不可用返回空路径。
[[nodiscard]] auto ensure_base_dir(const fs::path &candidate) -> fs::path {
    if (candidate.empty()) {
        return {};
    }
    std::error_code ec;
    (void)fs::create_directories(candidate, ec);
    // 基目录也可能是被上一用例删除的 TMP（外部注入态），故重建后再判一次。
    if (ec || !fs::is_directory(candidate, ec)) {
        return {};
    }
    return candidate;
}

/// @brief 创建本轮唯一临时目录（时间戳 + 序号 + create_directory 原生排他，重试上限兜底）。
[[nodiscard]] auto make_unique_temp_dir() -> std::string {
    // 基目录按优先级回退：系统临时目录 → POSIX /tmp → 工作目录下的隐藏目录。
    // 必须回退到一个**可用目录**而非返回空串：空串等于放弃接管 TMPDIR/TMP/TEMP，
    // 用例里抛异常的 temp_directory_path() 会直接失败，temp_dir() 拼出的路径也会退化成根路径
    // （如 WSL 继承了 Windows 的 TMP/TEMP，libstdc++ 的 temp_directory_path 直接报 ENOENT）。
    std::error_code base_ec;
    auto base = ensure_base_dir(fs::temp_directory_path(base_ec));
#ifndef _WIN32
    if (base.empty()) {
        base = ensure_base_dir(fs::path{"/tmp"});
    }
#endif
    if (base.empty()) {
        std::error_code cwd_ec;
        const auto cwd = fs::current_path(cwd_ec);
        if (!cwd_ec) {
            base = ensure_base_dir(cwd / ".aurora_test_tmp");
        }
    }
    if (base.empty()) {
        return {};
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto& state = case_state();
    for (int attempt = 0; attempt < 64; ++attempt) {
        const fs::path candidate = base / ("aurora_test_" + std::to_string(stamp) + "_" + std::to_string(state.seq));
        ++state.seq;
        std::error_code ec;
        if (fs::create_directory(candidate, ec) && !ec) {
            return candidate.string();
        }
    }
    return {};
}

}  // namespace

auto setup() -> void {
    const auto& root = repo_root();
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    const auto current = fs::current_path(ec);
    if (ec || current.string() == root) {
        return;
    }
    fs::current_path(root, ec);  // 切换失败保持原 cwd（用例内可用 paths::under_repo 兜底）
}

auto begin_case() -> void {
    auto& state = case_state();
    // 进程原始 TMP/TMPDIR/TEMP 只快照一次（首个用例前），供 end_case 还原——
    // 否则上一用例删除临时目录后，残留的 env 会让下一用例的 temp_directory_path
    // 解析到不存在的基目录，隔离机制自毁。
    if (!state.env_saved) {
        state.saved_tmpdir = get_env("TMPDIR");
        state.saved_tmp = get_env("TMP");
        state.saved_temp = get_env("TEMP");
        state.env_saved = true;
    }
    // 兜底清理上一轮残留（end_case 正常已清；容忍异常路径跳过 end 的极端情况）。
    if (!state.temp_dir.empty()) {
        std::error_code ec;
        fs::remove_all(state.temp_dir, ec);
        state.temp_dir.clear();
    }
    // 防御性卸载上一用例可能遗留的剪贴板注入（end_case 正常已卸）。
    (void)aurora::Clipboard::remove_test_backend();

    state.temp_dir = make_unique_temp_dir();
    if (!state.temp_dir.empty()) {
        set_env("TMPDIR", state.temp_dir);
        set_env("TMP", state.temp_dir);
        set_env("TEMP", state.temp_dir);
    }
}

auto end_case() -> void {
    auto& state = case_state();
    if (!state.temp_dir.empty()) {
        std::error_code ec;
        fs::remove_all(state.temp_dir, ec);
        state.temp_dir.clear();
    }
    (void)aurora::Clipboard::remove_test_backend();
    // 还原进程原始临时目录 env：临时目录已被删除，env 不得继续指向它。
    set_env("TMPDIR", state.saved_tmpdir);
    set_env("TMP", state.saved_tmp);
    set_env("TEMP", state.saved_temp);
}

auto temp_dir() -> const std::string& { return case_state().temp_dir; }

auto repo_root() -> const std::string& {
    // 函数内静态常量按 StaticConstantCase 要求 UPPER_CASE 命名（已是最近作用域，无需再外移）。
    static const std::string REPO_ROOT = locate_repo_root();
    return REPO_ROOT;
}

}  // namespace aurora::testing::isolation
