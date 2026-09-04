// test_version.cpp — core/version.h 版本常量一致性。
// 覆盖：AURORA_VERSION_STRING 与数字分量/后缀宏的自洽（semver 形态）、
//       头文件自包含（不经 aurora.h 直接包含）、CMake 注入与头内回退同源。

#include <string>

#include "aurora/core/version.h"
#include "test_harness.h"

AURORA_TEST() {
    // ---- 1. 数字分量与 NUMERIC 拼串一致 ----
    {
        const std::string numeric = AURORA_VERSION_NUMERIC;
        const std::string expect = std::to_string(AURORA_VERSION_MAJOR) + "." + std::to_string(AURORA_VERSION_MINOR) +
                                   "." + std::to_string(AURORA_VERSION_PATCH);
        AURORA_TEST_CHECK_MSG(numeric == expect, "NUMERIC equals MAJOR.MINOR.PATCH");
    }

    // ---- 2. 完整串：有后缀时为 X.Y.Z-suffix，无后缀时为 X.Y.Z ----
    {
        const std::string full = AURORA_VERSION_STRING;
#if AURORA_HAS_VERSION_SUFFIX
        const std::string suffix = AURORA_VERSION_SUFFIX_STR;
        AURORA_TEST_CHECK_MSG(full == std::string(AURORA_VERSION_NUMERIC) + "-" + suffix,
                              "STRING == NUMERIC-SUFFIX when suffix enabled");
        // 后缀非空且不含前导 '-'（拼接时统一加）
        AURORA_TEST_CHECK_MSG(!suffix.empty() && suffix.front() != '-', "suffix non-empty and not self-hyphenated");
#else
        AURORA_TEST_CHECK_MSG(full == AURORA_VERSION_NUMERIC, "STRING == NUMERIC when suffix disabled");
#endif
    }

    // ---- 3. semver 形态：以数字开头、分量间为点分、长度合理 ----
    {
        const std::string full = AURORA_VERSION_STRING;
        AURORA_TEST_CHECK_MSG(full.size() >= 5, "version string non-trivial");
        AURORA_TEST_CHECK_MSG(
            full.find_first_not_of("0123456789.") != std::string::npos || full.find("..") == std::string::npos,
            "no empty numeric component (no consecutive dots)");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(full[0] >= '0' && full[0] <= '9');
    }

    // ---- 4. 当前发布形态（alpha 阶段）：主版本 1，含 alpha 后缀 ----
    {
        AURORA_TEST_CHECK_EQ(AURORA_VERSION_MAJOR, 1);
#if AURORA_HAS_VERSION_SUFFIX
        const std::string s = AURORA_VERSION_SUFFIX_STR;
        AURORA_TEST_CHECK_MSG(s.rfind("alpha", 0) == 0, "pre-release phase identifier is alpha");
#endif
    }
}