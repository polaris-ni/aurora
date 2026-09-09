/// 测试类型: unit
/// 目标单元: include/aurora/debug/feature_flags.h
/// 测试说明: 覆盖 feature 宏运行时查询门面——值初始化默认全 false、结构体快照与 JSON
/// 导出逐键一致、JSON 键 = 完整宏名且数量收敛（守护 to_json 完整性）、便捷封装等价、
/// 编译期常量快照跨调用稳定。feature_flags 始终可用（gated = none），无需能力探测。

#include <string>
#include <vector>

#include "aurora/debug/feature_flags.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_feature_flags {

using aurora::debug::feature_flags;
using aurora::debug::feature_flags_json;
using aurora::debug::FeatureFlags;

namespace {

/// 宏名 ↔ 结构体字段的对照表（键 = 完整宏名，值 = FeatureFlags 成员地址）。
struct FlagKeyPair {
    const char* key;
    bool FeatureFlags::* field;
};

/// 全部 18 个归一化镜像字段（与 BUILD_OPTIONS.md 三层命名分组一一对应）。
[[nodiscard]] auto flag_key_table() -> const std::vector<FlagKeyPair>& {
    static const std::vector<FlagKeyPair> AURORA_FLAG_KEY_TABLE = {
        {.key = "AURORA_BACKEND_HEADLESS", .field = &FeatureFlags::backend_headless},
        {.key = "AURORA_BACKEND_WIN32", .field = &FeatureFlags::backend_win32},
        {.key = "AURORA_BACKEND_D3D11", .field = &FeatureFlags::backend_d3d11},
        {.key = "AURORA_BACKEND_GLFW", .field = &FeatureFlags::backend_glfw},
        {.key = "AURORA_BACKEND_X11", .field = &FeatureFlags::backend_x11},
        {.key = "AURORA_BACKEND_WAYLAND", .field = &FeatureFlags::backend_wayland},
        {.key = "AURORA_BACKEND_MACOS", .field = &FeatureFlags::backend_macos},
        {.key = "AURORA_BACKEND_WASM", .field = &FeatureFlags::backend_wasm},
        {.key = "AURORA_ENABLE_LAYOUT_CACHE", .field = &FeatureFlags::layout_cache},
        {.key = "AURORA_ENABLE_OCCLUSION_CULLING", .field = &FeatureFlags::occlusion_culling},
        {.key = "AURORA_ENABLE_DISPLAY_LIST", .field = &FeatureFlags::display_list},
        {.key = "AURORA_ENABLE_SIMD", .field = &FeatureFlags::simd},
        {.key = "AURORA_ENABLE_PROFILING", .field = &FeatureFlags::profiling},
        {.key = "AURORA_ENABLE_TRACING", .field = &FeatureFlags::tracing},
        {.key = "AURORA_ENABLE_DEBUG", .field = &FeatureFlags::debug},
        {.key = "AURORA_ENABLE_IMAGE_JPEG", .field = &FeatureFlags::image_jpeg},
        {.key = "AURORA_ENABLE_IMAGE_WEBP", .field = &FeatureFlags::image_webp},
        {.key = "AURORA_ENABLE_IMAGE_PNG", .field = &FeatureFlags::image_png},
    };
    return AURORA_FLAG_KEY_TABLE;
}

}  // namespace

AURORA_TEST_CASE(value_initialized_flags_are_all_false) {
    // 契约：FeatureFlags 值初始化默认全 false（真实取值由 feature_flags() 按宏填充）。
    constexpr FeatureFlags defaults{};
    AURORA_TEST_CHECK_FALSE(defaults.backend_headless);
    AURORA_TEST_CHECK_FALSE(defaults.backend_win32);
    AURORA_TEST_CHECK_FALSE(defaults.backend_d3d11);
    AURORA_TEST_CHECK_FALSE(defaults.backend_glfw);
    AURORA_TEST_CHECK_FALSE(defaults.backend_x11);
    AURORA_TEST_CHECK_FALSE(defaults.backend_wayland);
    AURORA_TEST_CHECK_FALSE(defaults.backend_macos);
    AURORA_TEST_CHECK_FALSE(defaults.backend_wasm);
    AURORA_TEST_CHECK_FALSE(defaults.layout_cache);
    AURORA_TEST_CHECK_FALSE(defaults.occlusion_culling);
    AURORA_TEST_CHECK_FALSE(defaults.display_list);
    AURORA_TEST_CHECK_FALSE(defaults.simd);
    AURORA_TEST_CHECK_FALSE(defaults.profiling);
    AURORA_TEST_CHECK_FALSE(defaults.tracing);
    AURORA_TEST_CHECK_FALSE(defaults.debug);
    AURORA_TEST_CHECK_FALSE(defaults.image_jpeg);
    AURORA_TEST_CHECK_FALSE(defaults.image_webp);
    AURORA_TEST_CHECK_FALSE(defaults.image_png);
}

AURORA_TEST_CASE(snapshot_matches_json_per_macro_key) {
    // JSON 键 = 完整宏名，值必须与结构体字段逐一相等（自描述契约：Inspector/CLI 直读）。
    const FeatureFlags f = feature_flags();
    const Json j = f.to_json();
    AURORA_TEST_CHECK_TRUE(j.is_object());
    for (const FlagKeyPair& pair : flag_key_table()) {
        AURORA_TEST_CHECK_MSG(j.contains(pair.key), std::string("to_json 缺少宏键: ") + pair.key);
        AURORA_TEST_CHECK_EQ(j[pair.key], f.*(pair.field));
    }
}

AURORA_TEST_CASE(json_contains_exactly_the_documented_macro_keys) {
    // 数量收敛守护：结构体新增字段而 to_json 漏写时，此处红灯（镜像三处同步纪律）。
    const Json j = feature_flags().to_json();
    AURORA_TEST_CHECK_EQ(j.size(), flag_key_table().size());
    // 全部值必须是布尔（工具直读依赖）。
    for (auto it = j.begin(); it != j.end(); ++it) {
        AURORA_TEST_CHECK_MSG(it.value().is_boolean(), "宏键 " + it.key() + " 的 JSON 值应为 boolean");
    }
}

AURORA_TEST_CASE(convenience_json_matches_to_json) {
    // feature_flags_json() 是 feature_flags().to_json() 的便捷封装，二者必须等价。
    const Json via_convenience = feature_flags_json();
    const Json via_direct = feature_flags().to_json();
    AURORA_TEST_CHECK_EQ(via_convenience.dump(), via_direct.dump());
}

AURORA_TEST_CASE(snapshot_is_stable_across_calls) {
    // 结果为编译期常量快照：与运行环境无关，重复调用取值恒定。
    const FeatureFlags first = feature_flags();
    const FeatureFlags second = feature_flags();
    for (const FlagKeyPair& pair : flag_key_table()) {
        AURORA_TEST_CHECK_EQ(second.*(pair.field), first.*(pair.field));
    }
}

}  // namespace aurora::test_cases::utest_feature_flags
