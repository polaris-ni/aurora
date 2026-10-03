/// 测试类型: unit
/// 目标单元: include/aurora/render/font_discovery.h
/// 测试说明: 覆盖字体发现的初始化与 family 解析：内嵌默认字体可用、未知 family 回退默认链、
/// 内存字体注册后可按 family 解析出可用 FT_Face、face id 稳定，以及 shutdown 后重新 init 可恢复。
///           另覆盖字体族枚举 `list_font_families()`：与 `resolve_faces` 的同源性（对拍判据）、
///           稳定序去重、等宽判定以度量为准、过滤腿、内置等宽族可见

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/render/font_discovery.h"
#include "aurora/render/noto_font_data.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_font_discovery {

AURORA_TEST_CASE(init_registers_default_faces) {
    render::init_font_discovery();
    const auto faces = render::resolve_faces("");
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
}

AURORA_TEST_CASE(unknown_family_falls_back_to_default_chain) {
    // 未注册 family 不得返回空表：缺字回退依赖默认链兜底。
    render::init_font_discovery();
    const auto faces = render::resolve_faces("no-such-family-xyz");
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

    const auto faces = render::resolve_faces("utest-custom-family");
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

    const auto faces = render::resolve_faces("");
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
}

// ---- 字体族枚举 `list_font_families()` ----

namespace {

/// @brief 一个绝不会命中任何族的哨兵族名。
constexpr const char *kSentinelFamily = "utest-no-such-family-sentinel-zzz";

/// @brief 默认链的面集合（哨兵族解析到的就是它）。
/// @return 哨兵族的解析结果（按 FontFace 裸指针）。
[[nodiscard]] auto default_chain_faces() -> const std::vector<render::FontFace *> & {
    return render::resolve_faces(kSentinelFamily, 400);
}

/// @brief 该族的解析结果是否**不同于**纯默认链（即它真的贡献了自己的面）。
///
/// 为什么比**序列**而不是比集合：系统回退族（Segoe UI / Arial / 微软雅黑 …）的 face 本身就
/// 被 `add_default_face()` 挂进了默认链，按「集合差」判它们会误报——它们的面确实在默认链里
/// 出现过一次。但 `resolve_faces` 的 emit 顺序是「先族内、后默认链」，故这些族的结果**顺序**
/// 与哨兵（纯默认链）不同，序列比较能正确判出。
/// @param family 族名。
/// @return 该族真的解析出了属于自己的面时 true。
///
/// 兜底分支为什么不能直接放行：若「序列与哨兵相同」就无条件算通过，那么**物化失败**（目录里
/// 扫到的族没被加载进注册表）也会被判成同源——那正是本条要拦的缺陷，放行等于把判据作废。
/// 故只在「首面的 FT family_name 就等于族名」时才接受：目录扫描出来的族名本就取自
/// `FT_Face::family_name`，它一旦没能物化出自己的面，首面就会是默认链的 Noto Sans，名字对不上。
[[nodiscard]] auto family_contributes_real_faces(const std::string &family) -> bool {
    const auto &own = render::resolve_faces(family, 400);
    const auto &fallback = default_chain_faces();
    if (own != fallback) {
        return true;
    }
    return (own.front()->face != nullptr) && (own.front()->face->family_name != nullptr) &&
           (family == own.front()->face->family_name);
}

}  // namespace

// 同源性（A-1）。为什么是**对拍**而不是「断言 resolve_faces 非空」：
// `resolve_faces()` 对任何未命中的 family 都会 emit 默认链（`""` / `"sans-serif"` / `"default"`），
// 故它**恒非空**——「逐个断言非空」是一条空转判据，无论枚举实现多烂都全绿。真正的契约是
// 「枚举出来的族必须贡献**属于它自己**的面」，故拿哨兵族（必然落到纯默认链）做对照：
// 每个枚举出的族都必须至少有一个面不在默认链里。
AURORA_TEST_CASE(every_enumerated_family_resolves_to_its_own_faces) {
    render::init_font_discovery();
    const auto families = render::list_font_families();
    AURORA_TEST_REQUIRE_FALSE(families.empty());
    std::size_t checked = 0;
    for (const auto &info : families) {
        // 任何族都不得「解析不到面」：face_count 恒 >= 1 是枚举入口的自身契约。
        AURORA_TEST_CHECK(info.face_count >= 1);
        const auto &faces = render::resolve_faces(info.family, 400);
        AURORA_TEST_CHECK_FALSE(faces.empty());
        AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
        AURORA_TEST_CHECK(family_contributes_real_faces(info.family));
        ++checked;
    }
    AURORA_TEST_CHECK_EQ(checked, families.size());
}

// 稳定序（A-2）：两次调用逐位相等、升序、无重复。下拉数据源要求稳定序，不能让 UI 侧自己排序。
AURORA_TEST_CASE(enumerated_families_are_sorted_deduplicated_and_stable) {
    render::init_font_discovery();
    const auto a = render::list_font_families();
    const auto b = render::list_font_families();
    AURORA_TEST_REQUIRE_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        AURORA_TEST_CHECK(a[i].family == b[i].family);
        AURORA_TEST_CHECK_EQ(a[i].monospace ? 1 : 0, b[i].monospace ? 1 : 0);
    }
    std::vector<std::string> names;
    for (const auto &info : a) {
        names.push_back(info.family);
    }
    AURORA_TEST_CHECK(std::ranges::is_sorted(names));
    AURORA_TEST_CHECK_EQ(std::ranges::adjacent_find(names), names.end());
    // 伪族名（默认链的键）不得出现：空串会破坏下拉，三者又指向同一个 Noto Sans 面。
    AURORA_TEST_CHECK_EQ(std::ranges::find(names, std::string{}), names.end());
    AURORA_TEST_CHECK_EQ(std::ranges::find(names, std::string{"sans-serif"}), names.end());
    AURORA_TEST_CHECK_EQ(std::ranges::find(names, std::string{"default"}), names.end());
}

// 等宽判定以**度量**为准（A-3）：注册一个「名字像等宽、字体非等宽」的族（名字含 Mono，字节是
// 比例字体的 Noto Sans），必须判 false。只测名字表的话这条测不到——那正是既有 `monospace` / `mono`
// 特判的盲区。
AURORA_TEST_CASE(monospace_flag_follows_metrics_not_the_family_name) {
    render::init_font_discovery();
    const auto data = render::noto_sans_ttf();
    render::register_font_memory("utest-Mono-lookalike", std::vector<std::uint8_t>{data.begin(), data.end()});

    const auto families = render::list_font_families();
    const auto it = std::ranges::find(families, std::string{"utest-Mono-lookalike"}, &render::FontFamilyInfo::family);
    AURORA_TEST_REQUIRE(it != families.end());
    // 名字里带 mono，但 Noto Sans 是比例字体 ⇒ 度量判 false。
    AURORA_TEST_CHECK_FALSE(it->monospace);
    // 它确实被枚举出来了（注册即入枚举，这是 A-5 的另一半）。
    AURORA_TEST_CHECK(it->face_count >= 1);
}

// 过滤腿（A-4）：monospace_only 的返回集是全集的子集，且每个元素 monospace == true。
AURORA_TEST_CASE(monospace_only_returns_a_subset_flagged_monospace) {
    render::init_font_discovery();
    const auto all = render::list_font_families();
    const auto mono = render::list_font_families(true);
    AURORA_TEST_CHECK(mono.size() <= all.size());
    for (const auto &info : mono) {
        AURORA_TEST_CHECK(info.monospace);
        const auto it = std::ranges::find(all, info.family, &render::FontFamilyInfo::family);
        AURORA_TEST_CHECK(it != all.end());
    }
}

// 内置等宽族可见（A-5）：框架此前没有任何内置等宽族（内置族只有非等宽的 Noto Sans），
// 故本轮注册了内置 Cascadia Code（OFL）。它必须出现在枚举里且 monospace == true；
// 内置 Noto Sans 也必须出现且 monospace == false。
AURORA_TEST_CASE(builtin_cascadia_code_is_enumerated_as_monospace) {
    render::init_font_discovery();
    const auto families = render::list_font_families();
    const auto cascadia = std::ranges::find(families, std::string{"Cascadia Code"}, &render::FontFamilyInfo::family);
    AURORA_TEST_REQUIRE(cascadia != families.end());
    AURORA_TEST_CHECK(cascadia->monospace);
    AURORA_TEST_CHECK(cascadia->face_count >= 1);
    // 真等宽族必须能过过滤腿。
    const auto mono = render::list_font_families(true);
    AURORA_TEST_CHECK(std::ranges::find(mono, std::string{"Cascadia Code"}, &render::FontFamilyInfo::family) !=
                      mono.end());
    // 内置比例字体 Noto Sans 也在枚举里，且**不**被判等宽（防「只要是内置族就判 true」）。
    const auto noto = std::ranges::find(families, std::string{"Noto Sans"}, &render::FontFamilyInfo::family);
    AURORA_TEST_REQUIRE(noto != families.end());
    AURORA_TEST_CHECK_FALSE(noto->monospace);
    // 默认链行为不变：内置等宽族不得被挂进默认链（A-7）。
    AURORA_TEST_CHECK_EQ(render::resolve_faces("").front(), render::resolve_faces("sans-serif").front());
}

// 既有面不破坏（A-7）：枚举不得改变 `resolve_faces` 对默认链与已注册族的行为。
AURORA_TEST_CASE(enumerating_does_not_disturb_existing_resolution) {
    render::init_font_discovery();
    const auto before = render::resolve_faces("", 400);
    const auto before_size = before.size();
    const auto before_front = before.front();
    const auto all = render::list_font_families();
    AURORA_TEST_CHECK_FALSE(all.empty());
    const auto after = render::resolve_faces("", 400);
    AURORA_TEST_CHECK_EQ(after.size(), before_size);
    AURORA_TEST_CHECK_EQ(after.front(), before_front);
}

}  // namespace aurora::test_cases::utest_font_discovery
