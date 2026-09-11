#include "isolation.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "aurora/app/clipboard.h"
#include "aurora/core/platform.h"
#include "death_test.h"

#if defined(AURORA_PLATFORM_WINDOWS)
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

#if defined(AURORA_PLATFORM_WASM)
/// @brief 去掉 Windows 宿主的盘符前缀（`D:/x/y` → `/x/y`）。
///
/// Emscripten 的 argv[0] 是宿主给出的 .js 路径（Windows 带盘符，如
/// `D:/repo/build-wasm/aurora_test_runner.js`）。在 wasm 的 POSIX 命名空间里
/// `D:/x` **不是**绝对路径（不以 `/` 开头），`fs::absolute()` 会退化成
/// `current_path() / "D:/x"`，拼出宿主目录下多一层假路径，向上找仓库根必然失败。
/// 去盘符后才是 POSIX 视角的绝对路径，`fs::absolute` 原样返回，向上遍历得以命中。
/// Node 解析 `/x/y` 时按当前盘符展开，故去掉盘符不会改变实际指向的文件。
/// 非 Windows 宿主（Linux CI）的 argv[0] 本就无盘符，原样返回。
[[nodiscard]] auto strip_windows_drive(std::string path) -> std::string {
    if (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        return "/" + path.substr(3);
    }
    return path;
}
#endif

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
#if defined(AURORA_PLATFORM_WASM)
    // Emscripten：argv[0] 是宿主给出的 .js 路径（Windows 带盘符），须去盘符才能被
    // fs::absolute 认作绝对路径（否则会拼上 cwd 多出一层假路径，见 strip_windows_drive）。
    fs::path dir = fs::absolute(strip_windows_drive(detail::executable_path()), ec).parent_path();
#else
    fs::path dir = fs::absolute(detail::executable_path(), ec).parent_path();
#endif
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
#if defined(AURORA_PLATFORM_WINDOWS)
    (void)_putenv_s(name, value.c_str());
#else
    (void)setenv(name, value.c_str(), 1);
#endif
}

/// @brief 读取环境变量（未设置返回空串）。
[[nodiscard]] auto get_env(const char* name) -> std::string {
// MSVC CRT 家族：MSVC 与 clang-cl 共用同一套 CRT（均提供 _dupenv_s），故两者取同一分支。
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
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
///
/// 基目录约定（C4，2026-09-11 定论）：优先定为**运行路径下的 test_temp/**（即
/// `fs::current_path()/test_temp`）。这样所有用例临时文件收敛到仓库运行目录、可被
/// `.gitignore` 统一忽略，且每用例一个唯一子目录（`test_temp/<case>`，`<case>` 为
/// `aurora_test_<时间戳>_<序号>` 唯一令牌）彼此隔离。
///
/// 回退链仅在运行路径**不可写**（只读挂载 / 沙箱 / 某些 CI 文件系统）时触发，保证隔离
/// 机制永不失效；每一级回退都已在下方注释写明原因（守门脚本 `check_test_temp_hygiene`
/// 也据此放行框架内部的必要例外）。
[[nodiscard]] auto make_unique_temp_dir() -> std::string {
    // 主基目录：运行路径下的 test_temp/（cwd 已由 setup() 切到仓库根，故实际落在 <repo>/test_temp）。
    // 用例边界经此目录隔离；不可写时才向下回退。
    std::error_code cwd_ec;
    const auto cwd = fs::current_path(cwd_ec);
    auto base = cwd_ec ? fs::path{} : ensure_base_dir(cwd / "test_temp");
    if (base.empty()) {
        // 回退①：运行路径不可写（如只读挂载 / 沙箱）→ 退到系统临时目录，仍保证隔离不失效。
        std::error_code sys_ec;
        base = ensure_base_dir(fs::temp_directory_path(sys_ec));
    }
#if !defined(AURORA_PLATFORM_WINDOWS)
    if (base.empty()) {
        // 回退②：系统临时目录也不可用 → 退到 POSIX /tmp（最后兜底，正常开发/CI 不应命中）。
        base = ensure_base_dir(fs::path{"/tmp"});
    }
#endif
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
    // current_path() 失败（ec 置位）时不早退：仍要尝试切到仓库根。
    if (!ec && current.string() == root) {
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
    // 死亡测试子进程：复用父进程经 TMPDIR 继承下来的唯一临时目录，**不再自建目录**。
    // 子进程异常退出时 end_case 来不及执行，自建目录会残留在 test_temp/ 下且父进程
    // 不知其名、无法代为清理；复用父目录后由父进程 end_case 统一回收，满足 C4
    // 「跑完 test_temp/ 应为空」的验收。子进程同样禁止删除该目录（见 end_case 守卫）。
    if (detail::death_child_mode()) {
        state.temp_dir = get_env("TMPDIR");
        return;
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
        // 死亡测试子进程复用的是父进程目录，删除权归父进程 end_case；子进程自身
        // （即便走到 end_case，如 statement 未致死）不得删除，否则会误删父进程仍在用的目录。
        if (!detail::death_child_mode()) {
            std::error_code ec;
            fs::remove_all(state.temp_dir, ec);
        }
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
