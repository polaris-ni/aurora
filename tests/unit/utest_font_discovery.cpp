/// 测试类型: unit
/// 目标单元: include/aurora/render/font_discovery.h
/// 测试说明: 覆盖字体发现的初始化与 family 解析：内嵌默认字体可用、未知 family 回退默认链、
/// 内存字体注册后可按 family 解析出可用 FT_Face、face id 稳定，以及 shutdown 后重新 init 可恢复。
///           另覆盖字体族枚举 `list_font_families()`：与 `resolve_faces` 的同源性（对拍判据）、
///           稳定序去重、等宽判定以度量为准、过滤分支、内置等宽族可见

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/render/cascadia_font_data.h"
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
constexpr const char *AURORA_UTEST_SENTINEL_FAMILY = "utest-no-such-family-sentinel-zzz";

/// @brief 默认链的面集合（哨兵族解析到的就是它）。
/// @return 哨兵族的解析结果（按 FontFace 裸指针）。
[[nodiscard]] auto default_chain_faces() -> const std::vector<render::FontFace *> & {
    return render::resolve_faces(AURORA_UTEST_SENTINEL_FAMILY, 400);
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
           family == own.front()->face->family_name;
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
    names.reserve(a.size());
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

// 过滤分支（A-4）：monospace_only 的返回集是全集的子集，且每个元素 monospace == true。
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
    // 真等宽族必须能过过滤分支。
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
    const auto *const before_front = before.front();
    const auto all = render::list_font_families();
    AURORA_TEST_CHECK_FALSE(all.empty());
    const auto after = render::resolve_faces("", 400);
    AURORA_TEST_CHECK_EQ(after.size(), before_size);
    AURORA_TEST_CHECK_EQ(after.front(), before_front);
}

// ============================ 按族缺字回退链 ============================
//
// 判据设计（避免「空转判据」）：本组用例不只看「链后面多了几个面」——全局默认链本来就贡献
// 多个面，那样的断言删掉整条链也照样全绿。真正的判据是**位置与归属**：
//   ① 链上族的面必须出现在「主族之后、默认链之前」这一段；
//   ② 同一组面在两条不同顺序的链下必须给出**不同**的相对次序（顺序即语义、可被观察）；
//   ③ 链上族名解析不到时不抛、不中断，且尾部回落仍有效。
//
// 用例自备两个测试族，避免依赖宿主装了哪些系统字体：Cascadia Code（内置等宽）与
// Noto Sans（内置比例）注册到私有名下 —— 两者度量差异明确，便于断言「选到了哪一个」。

namespace {

/// @brief 注册一个测试族（幂等名：重复注册会追加面，故用唯一名）。
auto register_probe_family(const std::string &family, bool monospace) -> void {
    if (monospace) {
        const auto data = render::cascadia_code_ttf();
        render::register_font_memory(family, std::vector<std::uint8_t>{data.begin(), data.end()});
    } else {
        const auto data = render::noto_sans_ttf();
        render::register_font_memory(family, std::vector<std::uint8_t>{data.begin(), data.end()});
    }
}

/// @brief 取某族自身的首个面（用于「这一段里应当只含该族」的对拍）。
auto first_face_of(const std::string &family) -> const render::FontFace * {
    const auto &faces = render::resolve_faces(family, 400);
    return faces.empty() ? nullptr : faces.front();
}

}  // namespace

AURORA_TEST_CASE(fallback_chain_places_family_between_main_and_default) {
    render::init_font_discovery();
    register_probe_family("utest-main-family", false);
    register_probe_family("utest-chain-mono", true);

    const std::vector<std::string> chain{"utest-chain-mono"};
    const auto &with_chain = render::resolve_faces("utest-main-family", 400, chain);

    // 主族自身必须仍在首位（链是「本族缺字之后」的追加段，不是替换）。
    AURORA_TEST_REQUIRE_FALSE(with_chain.empty());
    AURORA_TEST_CHECK_EQ(with_chain.front(), first_face_of("utest-main-family"));

    // 链上族的面必须真的在序列里（否则整条链是空转的）。
    const auto *chain_face = first_face_of("utest-chain-mono");
    AURORA_TEST_REQUIRE_NOT_NULL(chain_face);
    const bool chain_present = std::ranges::find(with_chain, chain_face) != with_chain.end();
    AURORA_TEST_CHECK(chain_present);
}

AURORA_TEST_CASE(fallback_chain_order_survives_a_cross_family_weight_resort) {
    // 本用例专钉「段内排序**不跨族**」这条不变量，且是上一条用例的**加强版**：
    // 上一条只在「所有面字重相同」的探针上比对序列，而等权元素经 stable_sort 保持原序，
    // 于是「跨族按字重距离重排」这个变异在该环境里是**恒等变换**、判据结构上无法观测
    // （变异自证实测：不转红）。要让它可观测，必须让链上两族**字重不同**。
    //
    // 独立基准：不自行构造 700 字重的面（那会变成「用被测实现自己的规则造输入」），而是
    // 从 `list_font_families()` 的枚举结果里找一个**真实存在的粗体面**所属的族 ——
    // 系统字体目录扫描会扫到 bold 文件（如 arialbd.ttf），其 OS/2 style_flags 带 BOLD
    // ⇒ FontFace::weight == 700。这条基准独立于 `resolve_faces` 的排序逻辑。
    render::init_font_discovery();

    // 找粗体族：枚举出的族里，第一个解析出 weight >= 600 的面所属的族。
    std::string bold_family;
    const render::FontFace *bold_face = nullptr;
    for (const auto &info : render::list_font_families()) {
        const auto &faces = render::resolve_faces(info.family, 400);
        for (const auto *fc : faces) {
            if ((fc != nullptr) && (fc->weight >= 600)) {
                bold_family = info.family;
                bold_face = fc;
                break;
            }
        }
        if (bold_face != nullptr) {
            break;
        }
    }
    // 宿主确实一个粗体系统字体都没装时，本用例的前提不成立 —— 如实 SKIP，
    // 不能给假绿（SKIP 与「判据空转」是两回事，前者是环境缺前提，后者是判据无效）。
    if (bold_face == nullptr) {
        AURORA_TEST_SKIP("no system family with a bold face: cross-family weight resort is unobservable");
        return;
    }
    AURORA_TEST_REQUIRE_EQ(bold_face->weight, 700);

    // 链首族是本仓自备的等宽族（weight 400），链次族是上面找到的粗体族（weight 700）。
    // 链序声明「400 族在前、700 族在后」。
    //
    // 主族**必须也注册一个探针族**：未注册且不在 catalog 里的族不贡献任何面，序列会从链首族
    // 开始（at(0)=mono、at(1)=bold），下标断言整体错位一格。段数是可数的，别让它含糊。
    register_probe_family("utest-mix-mono", true);
    register_probe_family("utest-mix-main", false);
    const auto *main_face = first_face_of("utest-mix-main");
    const auto *mono_face = first_face_of("utest-mix-mono");
    AURORA_TEST_REQUIRE_NOT_NULL(main_face);
    AURORA_TEST_REQUIRE_NOT_NULL(mono_face);
    AURORA_TEST_REQUIRE_EQ(main_face->weight, 400);
    AURORA_TEST_REQUIRE_EQ(mono_face->weight, 400);

    const std::vector<std::string> chain{"utest-mix-mono", bold_family};
    const auto &faces = render::resolve_faces("utest-mix-main", 400, chain);
    AURORA_TEST_REQUIRE(faces.size() >= 4U);

    // 段序断言用**相对位置**而非硬编码下标：粗体族通常同时有 400 与 700 的面，段内按请求
    // 字重 400 排序时其 400 面在前、700 面在后，故「链次族那一段」的下标并非 2。
    // 真正要钉的不变量是「700 面属于链段、排在默认链之前」。
    //
    // 取默认链首面（Noto Sans）作基准 —— 跨族 weight 重排会把 700 面（距离 300）推到所有
    // 400 面之后，也就是挪到默认链**后面** ⇒ 下标关系反转 ⇒ 转红。
    const auto *default_front = render::resolve_faces("", 400).front();
    AURORA_TEST_REQUIRE_NOT_NULL(default_front);
    const auto idx_of = [&faces](const render::FontFace *needle) -> std::size_t {
        const auto it = std::ranges::find(faces, needle);
        return it == faces.end() ? faces.size() : static_cast<std::size_t>(it - faces.begin());
    };
    const std::size_t idx_bold = idx_of(bold_face);
    const std::size_t idx_default = idx_of(default_front);
    AURORA_TEST_REQUIRE(idx_bold != faces.size());
    AURORA_TEST_REQUIRE(idx_default != faces.size());
    AURORA_TEST_CHECK_LT(idx_bold, idx_default);
    // 段序第一段必须是主族自身：链是「本族缺字之后」的追加段，不是替换。
    AURORA_TEST_CHECK_EQ(faces.front(), main_face);

    // 反向链序：粗体族在前、常规族在后。**同一个 700 面**的下标必须整体前移，
    // 否则「顺序即语义」这条承诺在实现里并不成立。
    const std::vector<std::string> reversed{bold_family, "utest-mix-mono"};
    const auto &rev = render::resolve_faces("utest-mix-main", 400, reversed);
    AURORA_TEST_REQUIRE(rev.size() >= 4U);
    const auto rev_idx_of = [&rev](const render::FontFace *needle) -> std::size_t {
        const auto it = std::ranges::find(rev, needle);
        return it == rev.end() ? rev.size() : static_cast<std::size_t>(it - rev.begin());
    };
    const std::size_t rev_idx_bold = rev_idx_of(bold_face);
    const std::size_t rev_idx_default = rev_idx_of(default_front);
    AURORA_TEST_REQUIRE(rev_idx_bold != rev.size());
    AURORA_TEST_REQUIRE(rev_idx_default != rev.size());
    AURORA_TEST_CHECK_LT(rev_idx_bold, rev_idx_default);
    // 声明顺序变了，700 面的位置必须**真的**变（否则两条链等价，顺序语义没生效）。
    AURORA_TEST_CHECK_NE(rev_idx_bold, idx_bold);
}

AURORA_TEST_CASE(fallback_chain_unknown_family_falls_through_to_default) {
    render::init_font_discovery();
    register_probe_family("utest-fallthrough-main", false);

    // 链首是**不存在的族名**：必须跳过并继续，且整条链不得因此解析为空 ——
    // 尾部默认链仍须兜底（缺字回退依赖这一点）。
    const std::vector<std::string> chain{"utest-no-such-family-aaa", "utest-also-missing-bbb"};
    const auto &faces = render::resolve_faces("utest-fallthrough-main", 400, chain);
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);

    // 不可解析的链必须与「无链」给出同一结果：既然两族都没解析到面，追加它们就是空操作。
    const auto &plain = render::resolve_faces("utest-fallthrough-main", 400);
    AURORA_TEST_CHECK_EQ(faces.size(), plain.size());
    AURORA_TEST_CHECK_EQ(faces.front(), plain.front());
}

AURORA_TEST_CASE(fallback_chain_entry_skips_pseudo_family_and_empty_names) {
    render::init_font_discovery();
    register_probe_family("utest-pseudo-mono", true);

    // 伪族名（空串 / sans-serif / default）是默认链的键而非族名：链上出现它们若被接受，等于
    // 把尾部回落提前到首位，与「链在默认链之前」的语义相悖。此处只验「不抛且结果合法」。
    const std::vector<std::string> chain{"", "sans-serif"};
    const auto &faces = render::resolve_faces("utest-pseudo-main", 400, chain);
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    AURORA_TEST_CHECK_NOT_NULL(faces.front()->face);
}

AURORA_TEST_CASE(empty_chain_is_byte_identical_to_no_chain_overload) {
    render::init_font_discovery();
    register_probe_family("utest-equiv-main", false);

    // 「不指定时与现状逐字节一致」：空链重载须与无链重载给出**完全相同**的指针序列
    // （逐元素对拍，而非只比 size —— 只比 size 时「换了顺序」会漏判）。
    constexpr std::vector<std::string> empty_chain{};
    const auto &with_empty = render::resolve_faces("utest-equiv-main", 400, empty_chain);
    const auto &plain = render::resolve_faces("utest-equiv-main", 400);
    AURORA_TEST_REQUIRE_EQ(with_empty.size(), plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        AURORA_TEST_CHECK_EQ(with_empty.at(i), plain.at(i));
    }
    // 默认链路径同样验一遍（既有消费者最关心的那条）。
    const auto &def_with_empty = render::resolve_faces("", 400, empty_chain);
    const auto &def_plain = render::resolve_faces("", 400);
    AURORA_TEST_REQUIRE_EQ(def_with_empty.size(), def_plain.size());
    for (std::size_t i = 0; i < def_plain.size(); ++i) {
        AURORA_TEST_CHECK_EQ(def_with_empty.at(i), def_plain.at(i));
    }
}

AURORA_TEST_CASE(fallback_chain_names_are_resolvable_like_listed_families) {
    render::init_font_discovery();
    register_probe_family("utest-source-mono", true);

    // 「链上族名口径与 list_font_families() 完全同源」：取自枚举结果的族名，
    // 逐个喂给带链重载都必须解析得到**属于该族**的面（枚举却解析不到，比不枚举更糟）。
    const auto listed = render::list_font_families();
    AURORA_TEST_REQUIRE_FALSE(listed.empty());
    std::vector<std::string> chain;
    chain.reserve(listed.size());  // 逐个 push 前先备容量：整族枚举可达数百项
    for (const auto &info : listed) {
        chain.push_back(info.family);
    }
    const auto &faces = render::resolve_faces("utest-source-main", 400, chain);
    AURORA_TEST_REQUIRE_FALSE(faces.empty());
    // 枚举出的族至少有一个能在链里解析到面：否则「同源」承诺落空。
    bool any_listed_family_resolved = false;
    for (const auto &info : listed) {
        const auto &f2 = render::resolve_faces(info.family, 400);
        if (!f2.empty() && f2.front()->mem != nullptr) {
            any_listed_family_resolved = true;
            break;
        }
    }
    AURORA_TEST_CHECK(any_listed_family_resolved);
}

}  // namespace aurora::test_cases::utest_font_discovery
