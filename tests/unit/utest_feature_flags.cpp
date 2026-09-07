/// 测试类型: unit
/// 目标单元: include/aurora/debug/feature_flags.h
/// 测试说明: 运行时 feature 宏开关查询（结构体字段与编译期宏镜像一致、JSON 键完整且为布尔）单元测试

#include <string>

#include "aurora/debug/feature_flags.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_feature_flags {

AURORA_TEST() {
    // 编译期参照：测试 TU 与 aurora 库共享 PUBLIC feature 宏（同一宏注入面），
    // 用 #ifdef 镜像逐字段比对，防止 feature_flags.cpp 的单点收口与真实注入漂移。
    // （局部 constexpr：命名规则对 namespace 级全局常量要求 AURORA_ 前缀，放函数内更轻。）
#ifdef AURORA_BACKEND_HEADLESS
    constexpr bool headless_on = true;
#else
    constexpr bool headless_on = false;
#endif
#ifdef AURORA_ENABLE_SIMD
    constexpr bool simd_on = true;
#else
    constexpr bool simd_on = false;
#endif
#ifdef AURORA_ENABLE_LAYOUT_CACHE
    constexpr bool layout_cache_on = true;
#else
    constexpr bool layout_cache_on = false;
#endif

    // ---- 1. 编译期镜像一致性（抽样：后端 / 内部能力 / 架构优化三类各一） ----
    {
        const auto flags = aurora::debug::feature_flags();
        AURORA_TEST_CHECK_EQ(flags.backend_headless, headless_on);
        AURORA_TEST_CHECK_EQ(flags.simd, simd_on);
        AURORA_TEST_CHECK_EQ(flags.layout_cache, layout_cache_on);
    }

    // ---- 2. 默认开启的不变量：Headless 后端恒可用（构造期必开，BUILD_OPTIONS §3） ----
    {
        const auto flags = aurora::debug::feature_flags();
        AURORA_TEST_CHECK(flags.backend_headless);
    }

    // ---- 3. JSON 导出：键 = 完整宏名，值 = 布尔，且与结构体逐字段一致 ----
    {
        const auto json = aurora::debug::feature_flags_json();
        AURORA_TEST_CHECK(json.is_object());

        const auto flags = aurora::debug::feature_flags();
        const char* keys[] = {
            "AURORA_BACKEND_HEADLESS",    "AURORA_BACKEND_WIN32",
            "AURORA_BACKEND_D3D11",       "AURORA_BACKEND_GLFW",
            "AURORA_BACKEND_X11",         "AURORA_BACKEND_WAYLAND",
            "AURORA_BACKEND_MACOS",       "AURORA_BACKEND_WASM",
            "AURORA_ENABLE_LAYOUT_CACHE", "AURORA_ENABLE_OCCLUSION_CULLING",
            "AURORA_ENABLE_DISPLAY_LIST", "AURORA_ENABLE_SIMD",
            "AURORA_ENABLE_PROFILING",    "AURORA_ENABLE_TRACING",
            "AURORA_ENABLE_DEBUG",        "AURORA_ENABLE_IMAGE_JPEG",
            "AURORA_ENABLE_IMAGE_WEBP",   "AURORA_ENABLE_IMAGE_PNG",
        };
        for (const char* k : keys) {
            AURORA_TEST_CHECK(json.contains(k));
            AURORA_TEST_CHECK(json[k].is_boolean());
        }
        AURORA_TEST_CHECK_EQ(json["AURORA_BACKEND_HEADLESS"].get<bool>(), flags.backend_headless);
        AURORA_TEST_CHECK_EQ(json["AURORA_ENABLE_DEBUG"].get<bool>(), flags.debug);
        AURORA_TEST_CHECK_EQ(json["AURORA_ENABLE_IMAGE_JPEG"].get<bool>(), flags.image_jpeg);
    }

    // ---- 4. to_json 与便捷封装等价 ----
    {
        AURORA_TEST_CHECK(aurora::debug::feature_flags().to_json() == aurora::debug::feature_flags_json());
    }
}

}  // namespace aurora::test_cases::utest_feature_flags
