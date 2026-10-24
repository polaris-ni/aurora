/// 测试类型: integration
/// 目标单元: tools/gen/gen_i18n.cpp
/// 测试说明: 以仓库内 fixture 词表（tests/fixtures/i18n/）的**已生成产物**为输入，验证
///           gen_i18n 的输出两件能被运行期消费：生成头独立编译并给出具名参数的
///           LocalizedString，AUM1 bin 经 MessageCatalog::load_binary 载入后按 locale 逐条命中
///           （含六档回退、内置保留区合并、缺译回退到缺省语言）。
/// 覆盖说明: 对应 CTest 门禁 check_i18n_gen_golden 的 C++ 腿；fixture JSON 变更后须先重跑
///           gen_i18n 再更新 tests/fixtures/i18n/gen/ 下的产物，否则本用例与 golden 双双转红

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/i18n/builtin_messages.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/message_catalog.h"
#include "fixtures/i18n/gen/messages_base.hpp"
#include "framework/aurora_test.h"
#include "paths.h"

namespace aurora::test_cases::itest_i18n_pipeline {

namespace imsg = itest_i18n::msg;  // 生成 API 的落点命名空间（imsg::demo::* / imsg::id::*）

namespace {

/// @brief 读仓库根下的 fixture 文件原始字节。
/// @param relative 仓库相对路径。
/// @return 文件字节；读不到时为空（用例以 REQUIRE 先拦）。
auto read_fixture_bytes(std::string_view relative) -> std::vector<std::byte> {
    std::ifstream in(testing::paths::under_repo(relative), std::ios::binary);
    AURORA_TEST_REQUIRE_MSG(in.good(), "fixture must be readable: " + std::string(relative));
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<std::byte> bytes(text.size());
    for (std::size_t k = 0; k < text.size(); ++k) {
        bytes[k] = static_cast<std::byte>(static_cast<unsigned char>(text[k]));
    }
    return bytes;
}

/// @brief 载入 fixture bin 到独立目录（不碰进程默认目录，免得跨用例污染）。
/// @return 已载入词条的目录。
auto loaded_catalog() -> aurora::MessageCatalog {
    aurora::MessageCatalog catalog;
    const std::vector<std::byte> bytes = read_fixture_bytes("tests/fixtures/i18n/gen/messages_base.bin");
    AURORA_TEST_REQUIRE(catalog.load_binary(std::span<const std::byte>{bytes}).ok());
    catalog.set_default_locale(aurora::Locale{.language = "en"});
    return catalog;
}

/// @brief 构造一条内置词条的待解析串，并带上调用侧的英文兜底文本。
///
/// 「库不预置译文」：内置词条的英文模板由调用点（控件）随 text 携带，bin 只在被 locale 覆盖时
/// 才有该槽位，所以本 helper 同时承担「命中覆盖译文」与「回退英文 text」两条口径的验证。
/// @param id 内置词条 Id。
/// @param fallback 调用侧英文兜底文本。
/// @param args 具名参数，缺省为空。
/// @return localize 为 true、text 为 fallback 的实例。
auto builtin_with_fallback(aurora::MessageId id, std::string_view fallback, std::vector<aurora::MessageArg> args = {})
    -> aurora::LocalizedString {
    aurora::LocalizedString ls = aurora::LocalizedString::tr(id, std::move(args));
    ls.text = std::string(fallback);
    return ls;
}

/// @brief 位置/总数两个具名参数的便捷构造（内置 reorder 词条的参数面）。
/// @param position 位置值。
/// @param total 总数值。
/// @return 名为 position / total 的两参数。
auto position_args(std::int64_t position, std::int64_t total) -> std::vector<aurora::MessageArg> {
    std::vector<aurora::MessageArg> args;
    args.emplace_back("position", position);
    args.emplace_back("total", total);
    return args;
}

// 静态初始化期构造 3 枚短字符串，唯一抛出路径是 bad_alloc；测试进程对内存耗尽没有恢复语义
// （既有同类件按同口径豁免，见 src/aurora/render/font_discovery.cpp）。
// NOLINTBEGIN(bugprone-throwing-static-initialization)
const aurora::Locale AURORA_EN{.language = "en"};
const aurora::Locale AURORA_ZH{.language = "zh", .script = "Hans", .region = "CN"};
const aurora::Locale AURORA_RU{.language = "ru"};
// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace

AURORA_TEST_CASE(generated_api_resolves_base_locale_templates) {
    const aurora::MessageCatalog catalog = loaded_catalog();
    const auto at = [&](const aurora::LocalizedString &ls) { return ls.resolve(&catalog, AURORA_EN); };
    AURORA_TEST_CHECK_EQ(at(imsg::demo::welcome(1, "Ada")), std::string("Hi Ada, 1 file."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::welcome(1234, "Ada")), std::string("Hi Ada, 1,234 files."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::due(1234.56, aurora::CivilDate{2025, 10, 25})),
                         std::string("Due October 25, 2025 for \u20AC1,234.56."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::role("admin")), std::string("Administrator"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::role("guest")), std::string("Guest"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::role("nobody")), std::string("User"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::plain()), std::string("Nothing to substitute here"));
    // ordinal 表（en）：1st / 2nd / 3rd，11 落 other。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::rank(1)), std::string("You are 1st."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::rank(2)), std::string("You are 2nd."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::rank(3)), std::string("You are 3rd."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::rank(11)), std::string("You are 11th."));
}

AURORA_TEST_CASE(generated_api_covers_offset_exact_and_quoting) {
    const aurora::MessageCatalog catalog = loaded_catalog();
    const auto at = [&](const aurora::LocalizedString &ls) { return ls.resolve(&catalog, AURORA_EN); };
    // offset:1 先进入判定：n=1 → 生效 0 → =0；n=2 → 生效 1 → =1；n=3 → 生效 2 → one；n=5 → other。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::offset(1)), std::string("none"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::offset(2)), std::string("one more"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::offset(3)), std::string("2 more"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::offset(5)), std::string("4 more"));
    // MF1 引号子集：'{ → 字面 {；'' → 字面 '；'# → 字面 #
    AURORA_TEST_CHECK_EQ(at(imsg::demo::quoted()), std::string("{ brace, 'quoted' and # hash stay literal"));
    // 嵌套：select 套在 plural 分支里，分支内 `#` 取外层复数的生效值。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::nested(1, "file")), std::string("1 file"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::nested(1, "box")), std::string("1 item"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::nested(1234, "file")), std::string("1,234 files"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::nested(1234, "box")), std::string("1,234 items"));
}

AURORA_TEST_CASE(translated_locale_hits_its_own_entries) {
    const aurora::MessageCatalog catalog = loaded_catalog();
    const auto at = [&](const aurora::LocalizedString &ls) { return ls.resolve(&catalog, AURORA_ZH); };
    // zh-Hans-CN 只有复数 other 档；`#` 取分组后的生效值。期望串以转义写就（字面量不入 CJK）。
    // 你好 Ada，5 个文件。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::welcome(5, "Ada")),
                         std::string("\u4F60\u597D Ada\uFF0C5 \u4E2A\u6587\u4EF6\u3002"));
    // { 花括号、'引号' 与 # 井号保持字面
    AURORA_TEST_CHECK_EQ(
        at(imsg::demo::quoted()),
        std::string("{ \u82B1\u62EC\u53F7\u3001'\u5F15\u53F7' \u4E0E # \u4E95\u53F7\u4FDD\u6301\u5B57\u9762"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::offset(5)), std::string("4 \u4E2A"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::nested(1234, "file")), std::string("1,234 \u4E2A\u6587\u4EF6"));
    // 序数在中文里没有词形变化：other 档直接给数。你排在第 7 位。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::rank(7)), std::string("\u4F60\u6392\u5728\u7B2C 7 \u4F4D\u3002"));
    // zh 自带 demo.due：日期与货币按 zh 口径格式化，而不是照抄基准语言英文。
    // 2025年10月25日 到期，共 €1,234.56。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::due(1234.56, aurora::CivilDate{2025, 10, 25})),
                         std::string("2025\u5E7410\u670825\u65E5 \u5230\u671F\uFF0C\u5171 \u20AC1,234.56\u3002"));
    // 内置保留区的英文兜底不入 bin（「库不预置译文」）：bin 没有 zh 模板时回退调用点自带的英文 text。
    AURORA_TEST_CHECK_EQ(
        builtin_with_fallback(aurora::msg::AURORA_REORDER_POSITION, "Position 2 of 5.", position_args(2, 5))
            .resolve(&catalog, AURORA_ZH),
        std::string("Position 2 of 5."));
}

AURORA_TEST_CASE(cldr_branch_sets_and_select_gaps_follow_the_base) {
    const aurora::MessageCatalog catalog = loaded_catalog();
    const auto at = [&](const aurora::LocalizedString &ls) { return ls.resolve(&catalog, AURORA_RU); };
    // ru 补齐 one/few/many（CLDR 可达档），选分支靠规则表而不是基准分支集。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::welcome(1, "Ada")), std::string("Привет Ada, 1 файл."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::welcome(2, "Ada")), std::string("Привет Ada, 2 файла."));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::welcome(5, "Ada")), std::string("Привет Ada, 5 файлов."));
    // ru 自己的 demo.plain 命中，不落到基准语言。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::plain()), std::string("Здесь нечего подставлять"));
    // ru 的 select 少给 guest 档：该值落 other（生成器只报 warn，不是结构错误）。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::role("guest")), std::string("User"));
    AURORA_TEST_CHECK_EQ(at(imsg::demo::role("admin")), std::string("Administrator"));
    // ru 未译 demo.quoted：整条回退缺省语言 en 的模板。
    AURORA_TEST_CHECK_EQ(at(imsg::demo::quoted()), std::string("{ brace, 'quoted' and # hash stay literal"));
    // 模板整条回退，但数字 / 货币 / 日期仍按请求的 ru 口径格式化（译文与格式化口径解耦）。
    // Due 2025-10-25 for 9,50 €.
    AURORA_TEST_CHECK_EQ(at(imsg::demo::due(9.5, aurora::CivilDate{2025, 10, 25})),
                         std::string("Due 2025-10-25 for 9,50 \u20AC."));
}

AURORA_TEST_CASE(builtin_reserved_region_merges_into_generated_bin) {
    // 生成器把内置保留区并进同一张名字表与 bin 区间：应用译文覆盖内置 key 走同一通道，Id 不冲突。
    const aurora::MessageCatalog catalog = loaded_catalog();
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(aurora::msg::AURORA_COMMAND_PALETTE_PLACEHOLDER),
                         std::string_view{"command_palette.placeholder"});
    AURORA_TEST_CHECK_EQ(catalog.find_by_name("aurora.reorder.position"), aurora::msg::AURORA_REORDER_POSITION);
    // ru 侧是 fixture 里的覆盖译文（bin 的保留区槽位）；en 侧 bin 里没有模板，回退调用点英文 text。
    AURORA_TEST_CHECK_EQ(
        builtin_with_fallback(aurora::msg::AURORA_REORDER_POSITION, "Position 2 of 5.", position_args(2, 5))
            .resolve(&catalog, AURORA_RU),
        std::string("Позиция 2 из 5."));
    AURORA_TEST_CHECK_EQ(
        builtin_with_fallback(aurora::msg::AURORA_REORDER_POSITION, "Position 2 of 5.", position_args(2, 5))
            .resolve(&catalog, AURORA_EN),
        std::string("Position 2 of 5."));
    // 未被任何 locale 覆盖的内置词条在 bin 里没有槽位：取字面回退，不脏化保留区。
    AURORA_TEST_CHECK_EQ(builtin_with_fallback(aurora::msg::AURORA_COMMAND_PALETTE_PLACEHOLDER, "Type a command...")
                             .resolve(&catalog, AURORA_RU),
                         std::string("Type a command..."));
}

AURORA_TEST_CASE(generated_ids_map_back_to_canonical_names) {
    const aurora::MessageCatalog catalog = loaded_catalog();
    // 生成头的 Id 与 bin 的名字表同源：规范名反查逐条命中，应用词表起点按 --id-base 落位。
    AURORA_TEST_CHECK_EQ(imsg::id::AURORA_DEMO_WELCOME.value, std::uint32_t{1024 + 7});
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(imsg::id::AURORA_DEMO_WELCOME), std::string_view{"demo.welcome"});
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(imsg::id::AURORA_DEMO_DUE), std::string_view{"demo.due"});
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(imsg::id::AURORA_DEMO_QUOTED), std::string_view{"demo.quoted"});
    AURORA_TEST_CHECK_EQ(catalog.find_by_name("demo.nested"), imsg::id::AURORA_DEMO_NESTED);
    // 保留区之外的空洞（8..1023）没有规范名：反查给空视图而不是脏数据。
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(aurora::MessageId{512}), std::string_view{});
}

AURORA_TEST_CASE(missing_translation_count_matches_the_fixture) {
    const aurora::MessageCatalog catalog = loaded_catalog();
    // 缺译清单覆盖整个 Id 空间（内置保留区 + 应用区），按 Id 升序；本用例按规范名前缀分桶核对。
    // 内置保留区的英文兜不住进 bin，所以只有「别的 locale 有译文、该 locale 没有」才算缺译。
    const std::vector<aurora::MessageId> ru_missing = catalog.missing_translations(AURORA_RU);
    std::vector<std::string> ru_app_missing;
    std::size_t ru_builtin_missing = 0;
    for (const aurora::MessageId id : ru_missing) {
        const std::string_view name = catalog.canonical_name(id);
        AURORA_TEST_REQUIRE(!name.empty());  // 缺译只可能落在有规范名的槽位
        if (name.starts_with("demo.")) {
            ru_app_missing.emplace_back(name);
        } else {
            ++ru_builtin_missing;
            AURORA_TEST_CHECK(id.value < 1024);
        }
    }
    // ru 译了 welcome / rank / role / plain，另四条应用词条缺译。
    AURORA_TEST_CHECK_EQ(ru_app_missing,
                         std::vector<std::string>{"demo.due", "demo.nested", "demo.offset", "demo.quoted"});
    // 内置区唯一被覆盖的 aurora.reorder.position 正是 ru 自己给的：ru 无内置缺译。
    AURORA_TEST_CHECK_EQ(ru_builtin_missing, std::size_t{0});
    // zh 覆盖全部应用词条：缺译只剩 ru 独有的那条内置覆盖。
    const std::vector<aurora::MessageId> zh_missing = catalog.missing_translations(AURORA_ZH);
    AURORA_TEST_REQUIRE_EQ(zh_missing.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(zh_missing[0]), std::string_view{"aurora.reorder.position"});
}

}  // namespace aurora::test_cases::itest_i18n_pipeline
