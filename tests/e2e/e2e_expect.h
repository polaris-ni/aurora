#pragma once

// ============================================================
// E2E 期望集适配层（tests/e2e/e2e_expect.h）
// ------------------------------------------------------------
// 把「后端不可用」的裁决权从用例交给**编排方**：编排方用环境变量声明「在本运行环境里，
// 这些后端**必须**可用」，用例据此二分——期望集内不可用 = **失败**（红灯），集外 = **跳过**。
//
//   AURORA_E2E_EXPECT=win32,glfw   # 本环境下期望可用的后端（逗号分隔短名，见 backend_name）
//   （不设置）                      # 空集：一律按「跳过」处理，本地开发行为不变
//
// 存在的唯一理由：真实后端用例在无显示环境下建窗必然失败，若一律 `AURORA_TEST_SKIP`，
// 则后端真坏了、CI 镜像换了、xvfb 没起来，结果永远还是「跳过 = 绿」——**永久跳过绿灯**。
// 声明式期望集把「环境没兑现」与「设计上不适用」重新分开：只有后者可以跳过。
//
// 本层只存在于测试侧：内核（tools/include/e2e/harness.h）不含测试框架宏，只回答「这个后端
// 在本构建 / 本环境下能不能用」，**不决定**用例该跳过还是失败。策略必须落在这里，才能既被
// etest_ 用例复用，又不污染 tools/verify/ 的真机验收探针。
// ============================================================

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

#include "e2e/harness.h"
#include "framework/aurora_test.h"

namespace aurora::e2e {

/// @brief 期望集环境变量名（编排方声明「本环境下这些后端必须可用」）。
inline constexpr auto AURORA_EXPECT_ENV_VAR = "AURORA_E2E_EXPECT";

namespace detail {

/// @brief 归一化后端短名：去首尾空白 + 转小写（容忍 `GLFW` / `glfw ` 一类写法）。
[[nodiscard]] inline auto trim_and_lower(std::string_view raw) -> std::string {
    const auto is_space = [](unsigned char c) -> bool { return std::isspace(c) != 0; };
    const char *first = raw.data();
    // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
    const char *last = raw.data() + raw.size();
    while (first != last && is_space(static_cast<unsigned char>(*first))) {
        ++first;
    }
    while (first != last && is_space(static_cast<unsigned char>(*(last - 1)))) {
        --last;
    }
    // NOLINTEND(*-pro-bounds-pointer-arithmetic)
    std::string out{first, last};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// @brief 解析逗号分隔的期望集；未设置（`raw == nullptr`）得空集 = 本地开发默认（全部跳过）。
[[nodiscard]] inline auto parse_expected(const char *raw) -> std::set<std::string> {
    std::set<std::string> expected;
    if (raw == nullptr) {
        return expected;
    }
    std::string_view rest{raw};
    while (true) {
        const auto comma = rest.find(',');
        auto name = trim_and_lower(rest.substr(0, comma));
        if (!name.empty()) {
            expected.insert(std::move(name));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1U);
    }
    return expected;
}

}  // namespace detail

/// @brief 本次运行的期望后端集（首次调用时读环境变量并缓存，进程内只解析一次）。
///
/// 期望集由编排方在进程启动前设定、用例运行期不会变更，故缓存安全，且避免每例重解析。
[[nodiscard]] inline auto expected_backends() -> const std::set<std::string> & {
    static const std::set<std::string> CACHED = detail::parse_expected(std::getenv(AURORA_EXPECT_ENV_VAR));
    return CACHED;
}

/// @brief 该后端是否被编排方声明为「本环境下期望可用」。
[[nodiscard]] inline auto is_expected(Backend backend) -> bool {
    return expected_backends().contains(backend_name(backend));
}

/// @brief 后端不可用时的统一裁决：期望集内 → **失败**，集外 → **跳过**。
///
/// 建窗失败与读回能力缺失都走这里，语义边界：
///   - 期望集内不可用 = 编排方声明了期望、环境却没兑现（镜像缺组件、驱动退化、构建配置
///     未开读回能力），属**必须修的红灯**，不得降级为跳过；
///   - 期望集外不可用 = 设计上的不适用，计入 Skipped，不伪装成通过。
///
/// 两条路径都以异常终止用例（`AURORA_TEST_FAIL_FATAL` → `CaseAbort`；`AURORA_TEST_SKIP`
/// → `CaseSkipped`），故本函数 `[[noreturn]]`，调用处无需再写 `return`。
[[noreturn]] inline auto account_unavailable(Backend backend, const std::string &reason) -> void {
    const std::string name{backend_name(backend)};
    if (is_expected(backend)) {
        AURORA_TEST_FAIL_FATAL(std::string{"backend '"} + name + "' is declared in " + AURORA_EXPECT_ENV_VAR +
                               " but unavailable in this environment: " + reason);
        // 不可达：report() 的 Fatal 分支恒抛 CaseAbort（tests/framework/test_assert.cpp）。
        // 刻意不写 else —— 保持末尾是 noreturn 调用，编译器据此判定本函数无返回路径。
    }
    AURORA_TEST_SKIP(std::string{"backend '"} + name + "' is not declared in " + AURORA_EXPECT_ENV_VAR +
                     " (skipped by policy): " + reason);
}

}  // namespace aurora::e2e
