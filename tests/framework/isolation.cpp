#include "isolation.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "aurora/app/clipboard.h"
#include "death_test.h"

#ifdef _WIN32
// 只需要进程与环境变量 API（同 test_death.cpp 的取舍，不自定义 WIN32_LEAN_AND_MEAN）。
#include <windows.h>
#else
#include <unistd.h>
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
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    (void)_dupenv_s(&value, &length, name);  // 返回 malloc 副本，用后须 free
    const std::string result = value == nullptr ? std::string{} : std::string{value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

/// @brief 创建本轮唯一临时目录（时间戳 + 序号 + create_directory 原生排他，重试上限兜底）。
[[nodiscard]] auto make_unique_temp_dir() -> std::string {
    std::error_code base_ec;
    const auto base = fs::temp_directory_path(base_ec);
    // 基目录兜底：temp_directory_path 可能读到已被上一用例删除的 TMP（外部注入态），
    // 此时重建基目录本身（幂等、低廉），失败则放弃本用例的 tmpdir 隔离。
    if (!base_ec && !base.empty()) {
        (void)fs::create_directories(base, base_ec);
    }
    if (base_ec || base.empty()) {
        return {};
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto& state = case_state();
    for (int attempt = 0; attempt < 64; ++attempt) {
        const fs::path candidate =
            base / ("aurora_test_" + std::to_string(stamp) + "_" + std::to_string(state.seq));
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
    static const std::string root = locate_repo_root();
    return root;
}

}  // namespace aurora::testing::isolation
