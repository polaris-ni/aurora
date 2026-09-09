/// 测试类型: unit
/// 目标单元: include/aurora/render/font_discovery.h
/// 测试说明: 覆盖字体发现的初始化与 family 解析：内嵌默认字体可用、未知 family 回退默认链、
/// 内存字体注册后可按 family 解析出可用 FT_Face、face id 稳定，以及 shutdown 后重新 init 可恢复

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/render/font_discovery.h"
#include "aurora/render/noto_font_data.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_font_discovery {

AURORA_TEST_CASE(init_registers_default_faces) {
    render::init_font_discovery();
    const auto& faces = render::resolve_faces("");
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
}

AURORA_TEST_CASE(unknown_family_falls_back_to_default_chain) {
    // 未注册 family 不得返回空表：缺字回退依赖默认链兜底。
    render::init_font_discovery();
    const auto& faces = render::resolve_faces("no-such-family-xyz");
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
}

AURORA_TEST_CASE(sans_serif_alias_resolves_like_default) {
    render::init_font_discovery();
    AURORA_TEST_CHECK_EQ(render::resolve_faces("sans-serif").size(), render::resolve_faces("").size());
    AURORA_TEST_CHECK_EQ(render::resolve_faces("sans-serif").front(), render::resolve_faces("").front());
}

AURORA_TEST_CASE(register_font_memory_makes_family_resolvable) {
    render::init_font_discovery();
    const auto data = render::noto_sans_ttf();
    render::register_font_memory("utest-custom-family", std::vector<std::uint8_t>{data.begin(), data.end()});

    const auto& faces = render::resolve_faces("utest-custom-family");
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
    // 内存字节须由 FontFace 持有（否则 face 释放后字形数据悬空）。
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->mem);
    AURORA_TEST_CHECK_FALSE(faces.front()->mem->empty());
}

AURORA_TEST_CASE(face_ids_are_stable_across_lookups) {
    render::init_font_discovery();
    const int first_id = render::resolve_faces("").front()->id;
    const int second_id = render::resolve_faces("").front()->id;
    AURORA_TEST_CHECK_EQ(first_id, second_id);
}

AURORA_TEST_CASE(reinit_after_shutdown_restores_discovery) {
    render::init_font_discovery();
    AURORA_TEST_REQUIRE_FALSE(render::resolve_faces("").empty());

    render::shutdown_font_discovery();
    render::init_font_discovery();

    const auto& faces = render::resolve_faces("");
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
}

}  // namespace aurora::test_cases::utest_font_discovery
