#pragma once

// tools/verify/ 桌面外壳集成四枚真机探针的共用小工具面。
//
// 与 verify_print.h / verify_args.h 同定位：只服务 tools/verify/ 下的验收探针，不进 aurora 库、
// 不进 CTest。功能输出一律走 `AURORA_LOG_RAW`（AGENTS.md §5 规则 8：tools/ 下不新增直接触达
// 标准输出的 `printf` / `std::cout`）。
//
// 四枚探针分别在独立进程里跑，故这里的进程级静态量（失败计数 / 环境不可用标记）互不串扰。

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "aurora/core/log.h"

namespace aurora_verify {

/// @brief 本次运行的断言失败条数（决定退出码 1）。
inline auto failures() -> int & {
    static int count = 0;
    return count;
}

/// @brief 本次运行的「环境不可用」标记（置位后主流程按退出码 2 收尾）。
inline auto env_unavailable() -> bool & {
    static bool flag = false;
    return flag;
}

/// @brief 输出一行探针结论（无日志前缀、不过级别过滤、恒写 stdout）。
/// @param text 待输出文本（调用方不传行尾换行）。
inline auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记录一条断言结论：打印 PASS / FAIL 行并累计失败数。
/// @param passed 断言是否成立。
/// @param label 断言描述（ASCII）。
inline auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        ++failures();
    }
}

/// @brief 记录一条信息行（不参与判定）。
/// @param label 信息描述（ASCII）。
inline auto note(const std::string &label) -> void { emit("[INFO] " + label); }

/// @brief 记一条环境不可用并说明补救方式，随即由主流程按退出码 2 收尾。
/// @param label 判据描述。
/// @param hint 用户侧补救方式（ASCII）。
inline auto env_fail(const std::string &label, const std::string &hint) -> void {
    emit(std::string("[ENV ] ") + label);
    emit("Hint: " + hint);
    env_unavailable() = true;
}

/// @brief ASCII 小写副本（Windows 路径与注册表命令模板按大小写不敏感比较）。
/// @param text 原串。
/// @return 小写副本。
inline auto ascii_lower(std::string text) -> std::string {
    for (char &ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

/// @brief 大小写不敏感的子串判定（只对 ASCII 字母折叠；路径比较够用）。
/// @param haystack 被搜索串。
/// @param needle 待查子串。
/// @return 命中为 true。
inline auto contains_ascii_ci(std::string_view haystack, std::string_view needle) -> bool {
    const std::string hay = ascii_lower(std::string(haystack));
    const std::string want = ascii_lower(std::string(needle));
    return !want.empty() && hay.find(want) != std::string::npos;
}

/// @brief argv[0] → 绝对路径（进 OS 命令模板用；不引任何平台头）。
///
/// 探针只在真机上跑，argv[0] 由调用方给出；`std::filesystem::absolute` 以当前工作目录解析
/// 相对形式，`weakly_canonical` 再消解 `.` / `..`（不去解析不存在的尾段）。
/// @param argv0 命令行第 0 项。
/// @return 绝对路径；解析失败时原样返回 argv0。
inline auto executable_path(const char *argv0) -> std::string {
    std::string raw = argv0 == nullptr ? std::string{} : std::string(argv0);
    if (raw.empty()) {
        return raw;
    }
    std::error_code ignored;
    const auto absolute = std::filesystem::absolute(raw, ignored);
    if (ignored || absolute.empty()) {
        return raw;
    }
    const auto canonical = std::filesystem::weakly_canonical(absolute, ignored);
    return (ignored ? absolute : canonical).string();
}

/// @brief 本次运行的唯一后缀（方案名 / 扩展名 / 临时目录共用）。
/// @return 十进制数字串（只含 ASCII 数字，可拼进 RFC 3986 方案名与扩展名）。
inline auto unique_suffix() -> std::string {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<unsigned long long>(ticks) & 0xFFFFFFFFULL);
}

/// @brief 建一个本次运行独占的探针目录。
/// @param tag 目录名前缀（ASCII）。
/// @return 目录路径；创建失败返回空串。
inline auto make_probe_directory(const std::string &tag) -> std::string {
    std::error_code ignored;
    const auto path = std::filesystem::temp_directory_path(ignored) / (tag + "-" + unique_suffix());
    if (ignored) {
        return {};
    }
    std::filesystem::create_directories(path, ignored);
    return ignored ? std::string{} : path.string();
}

}  // namespace aurora_verify
