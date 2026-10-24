/// 测试类型: unit
/// 目标单元: include/aurora/i18n/message_catalog.h
/// 测试说明: MessageCatalog——词表登记/查名、AUM1 二进制载入（含 magic/version/字节序/截断
///           结构化错误）、六档回退物化、缺译诊断、消息引擎（具名参数/选择器/函数化占位/转义）

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/i18n/builtin_messages.gen.h"
#include "aurora/i18n/message_catalog.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_message_catalog {
namespace {

/// @brief 小端写入助手（测试内手工拼 AUM1 字节流）。
auto put_u16(std::vector<std::byte> &out, std::uint32_t v) -> void {
    out.push_back(static_cast<std::byte>((static_cast<unsigned>(v) >> 0U) & 0xFFU));
    out.push_back(static_cast<std::byte>((static_cast<unsigned>(v) >> 8U) & 0xFFU));
}

auto put_u32(std::vector<std::byte> &out, std::uint32_t v) -> void {
    out.push_back(static_cast<std::byte>((static_cast<unsigned>(v) >> 0U) & 0xFFU));
    out.push_back(static_cast<std::byte>((static_cast<unsigned>(v) >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>((static_cast<unsigned>(v) >> 16U) & 0xFFU));
    out.push_back(static_cast<std::byte>((static_cast<unsigned>(v) >> 24U) & 0xFFU));
}

auto put_str(std::vector<std::byte> &out, std::string_view text) -> void {
    for (const char ch : text) {
        out.push_back(static_cast<std::byte>(ch));
    }
}

/// @brief 一条 locale 段：tag + 与名字表等长的模板（空 optional = 缺译哨兵槽）。
struct LocaleSpec {
    std::string tag;
    std::vector<std::optional<std::string>> texts;
};

/// @brief 通用 AUM1 拼装：基址 / 名字表 / 各 locale 模板可自由组合（覆盖与冲突用例复用）。
auto build_bin(std::uint32_t id_base, const std::vector<std::string> &names, const std::vector<LocaleSpec> &locales)
    -> std::vector<std::byte> {
    constexpr std::uint32_t missing = 0xFFFFFFFFU;
    std::string pool;
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> slots;
    for (const LocaleSpec &spec : locales) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> per;
        for (const std::optional<std::string> &text : spec.texts) {
            if (!text.has_value()) {
                per.emplace_back(missing, 0U);
            } else {
                per.emplace_back(static_cast<std::uint32_t>(pool.size()), static_cast<std::uint32_t>(text->size()));
                pool += *text;
            }
        }
        slots.push_back(std::move(per));
    }
    std::vector<std::byte> out;
    put_str(out, "AUM1");
    put_u32(out, 1);
    put_u32(out, id_base);
    put_u32(out, static_cast<std::uint32_t>(names.size()));
    put_u32(out, static_cast<std::uint32_t>(locales.size()));
    for (const std::string &name : names) {
        put_u16(out, static_cast<std::uint32_t>(name.size()));
        put_str(out, name);
    }
    for (std::size_t l = 0; l < locales.size(); ++l) {
        out.push_back(static_cast<std::byte>(locales[l].tag.size()));
        put_str(out, locales[l].tag);
        for (const auto &[offset, length] : slots[l]) {
            put_u32(out, offset);
            put_u32(out, length);
        }
    }
    put_str(out, pool);
    return out;
}

/// @brief 双 locale 样例词表：id 1024=app.welcome、1025=app.files；en 全量、zh 仅 files。
auto sample_bin() -> std::vector<std::byte> {
    return build_bin(1024, {"app.welcome", "app.files"},
                     {
                         LocaleSpec{.tag = "en",
                                    .texts =
                                        {
                                            std::string{"Welcome {name}, {count, number}."},
                                            std::string{"{count, plural, =0 {No files} one {# file} other {# files}}"},
                                        }},
                         LocaleSpec{.tag = "zh",
                                    .texts =
                                        {
                                            std::nullopt,
                                            // CJK-LITERAL: cjk-fixture - zh plural template under test
                                            std::string{"{count, plural, other {# 个文件}}"},
                                        }},
                     });
}

}  // namespace

AURORA_TEST_CASE(vocabulary_registration_and_name_lookup) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"app.welcome", "app.files"}).ok());
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(aurora::MessageId{1024}), std::string_view("app.welcome"));
    AURORA_TEST_CHECK_EQ(catalog.canonical_name(aurora::MessageId{1025}), std::string_view("app.files"));
    AURORA_TEST_CHECK(catalog.canonical_name(aurora::MessageId{1026}).empty());
    AURORA_TEST_CHECK_EQ(catalog.find_by_name("app.files"), aurora::MessageId{1025});
    AURORA_TEST_CHECK(catalog.find_by_name("no.such") == aurora::AURORA_INVALID_MESSAGE_ID);
}

AURORA_TEST_CASE(vocabulary_registration_structured_errors) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"a", "b"}).ok());
    auto overlapped = catalog.register_vocabulary(1025, {"c"});
    AURORA_TEST_REQUIRE(!overlapped);
    AURORA_TEST_CHECK(overlapped.error().code_enum == aurora::ErrorCode::I18nVocabularyOverlap);
    auto duplicated = catalog.register_vocabulary(2048, {"b"});
    AURORA_TEST_REQUIRE(!duplicated);
    AURORA_TEST_CHECK(duplicated.error().code_enum == aurora::ErrorCode::I18nNameDuplicate);
}

AURORA_TEST_CASE(add_resolves_by_name_and_reports_unknown) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"greet"}).ok());
    AURORA_TEST_CHECK(catalog.add(aurora::Locale{.language = "en"}, "greet", "Hello").ok());
    auto unknown = catalog.add(aurora::Locale{.language = "en"}, "missing", "x");
    AURORA_TEST_REQUIRE(!unknown);
    AURORA_TEST_CHECK(unknown.error().code_enum == aurora::ErrorCode::I18nUnknownName);
    const aurora::LocalizedString localized = aurora::LocalizedString::tr(aurora::MessageId{1024});
    AURORA_TEST_CHECK_EQ(localized.resolve(&catalog, aurora::Locale{.language = "en"}), std::string("Hello"));
}

AURORA_TEST_CASE(binary_load_and_lookup_roundtrip) {
    aurora::MessageCatalog catalog;
    const auto bin = sample_bin();
    const auto loaded = catalog.load_binary(bin);
    AURORA_TEST_REQUIRE(loaded);
    const aurora::Locale en{.language = "en"};
    const aurora::Locale zh{.language = "zh"};
    const auto welcome = aurora::LocalizedString::tr(aurora::MessageId{1024},
                                                     {{"name", std::string{"Ada"}}, {"count", std::int64_t{3}}});
    AURORA_TEST_CHECK_EQ(welcome.resolve(&catalog, en), std::string("Welcome Ada, 3."));
    const auto files_zero = aurora::LocalizedString::tr(aurora::MessageId{1025}, {{"count", std::int64_t{0}}});
    AURORA_TEST_CHECK_EQ(files_zero.resolve(&catalog, en), std::string("No files"));
    const auto files_five = aurora::LocalizedString::tr(aurora::MessageId{1025}, {{"count", std::int64_t{5}}});
    AURORA_TEST_CHECK_EQ(files_five.resolve(&catalog, en), std::string("5 files"));
    // zh 缺 welcome（哨兵槽）→ 六档链落到缺省档（缺省 Locale{} 即 en）；files 走 zh 的 other 分支
    AURORA_TEST_CHECK_EQ(welcome.resolve(&catalog, zh), std::string("Welcome Ada, 3."));
    // CJK-LITERAL: cjk-fixture - zh 档 other 分支的展示串即本用例判据
    AURORA_TEST_CHECK_EQ(files_five.resolve(&catalog, zh), std::string("5 个文件"));
    // 覆盖语义：后到覆盖同 locale 同 Id
    AURORA_TEST_CHECK(catalog.add(en, "app.files", "{count, plural, other {OVERWRITTEN}}").ok());
    AURORA_TEST_CHECK_EQ(files_five.resolve(&catalog, en), std::string("OVERWRITTEN"));
}

AURORA_TEST_CASE(binary_structured_errors) {
    aurora::MessageCatalog catalog;
    auto bin = sample_bin();
    bin[0] = std::byte{'X'};
    auto bad_magic = catalog.load_binary(bin);
    AURORA_TEST_REQUIRE(!bad_magic);
    AURORA_TEST_CHECK(bad_magic.error().code_enum == aurora::ErrorCode::I18nBinMagicFailed);

    bin = sample_bin();
    bin[4] = std::byte{7U};  // version = 7
    auto bad_version = catalog.load_binary(bin);
    AURORA_TEST_REQUIRE(!bad_version);
    AURORA_TEST_CHECK(bad_version.error().code_enum == aurora::ErrorCode::I18nBinVersionUnsupported);

    bin = sample_bin();
    bin[4] = std::byte{0U};
    bin[5] = std::byte{0U};
    bin[6] = std::byte{0U};
    bin[7] = std::byte{1U};  // version 字段大端解读的 1
    auto bad_endian = catalog.load_binary(bin);
    AURORA_TEST_REQUIRE(!bad_endian);
    AURORA_TEST_CHECK(bad_endian.error().code_enum == aurora::ErrorCode::I18nBinEndiannessInvalid);

    bin = sample_bin();
    bin.resize(30);  // 截断在名字表内
    auto truncated = catalog.load_binary(bin);
    AURORA_TEST_REQUIRE(!truncated);
    AURORA_TEST_CHECK(truncated.error().code_enum == aurora::ErrorCode::I18nBinTruncated);
}

AURORA_TEST_CASE(binary_reload_overwrites_sparse_and_rejects_conflicts) {
    aurora::MessageCatalog catalog;
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_REQUIRE(catalog.load_binary(sample_bin()));
    // 稀疏 add 先落，同词表 bin 后到即覆盖（load_binary 的「后到覆盖」契约）。
    AURORA_TEST_CHECK(catalog.add(en, "app.welcome", "SPARSE").ok());
    const auto probe = aurora::LocalizedString::tr(aurora::MessageId{1024});
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, en), std::string("SPARSE"));
    const auto updated = build_bin(1024, {"app.welcome", "app.files"},
                                   {LocaleSpec{.tag = "en", .texts = {std::string{"BIN WELCOME"}, std::nullopt}}});
    AURORA_TEST_REQUIRE(catalog.load_binary(updated));
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, en), std::string("BIN WELCOME"));
    // 缺译哨兵槽不动既有值：files 的 en 词条仍是 sample_bin 那条。
    const auto files = aurora::LocalizedString::tr(aurora::MessageId{1025}, {{"count", std::int64_t{2}}});
    AURORA_TEST_CHECK_EQ(files.resolve(&catalog, en), std::string("2 files"));
    // 同基址缩减名单 → 拒绝（词表只增不减）；不同基址区间交叠 → I18nVocabularyOverlap。
    const auto shrunk = build_bin(1024, {"app.welcome"}, {LocaleSpec{.tag = "en", .texts = {std::string{"X"}}}});
    auto shrunk_result = catalog.load_binary(shrunk);
    AURORA_TEST_REQUIRE(!shrunk_result);
    AURORA_TEST_CHECK(shrunk_result.error().code_enum == aurora::ErrorCode::I18nVocabularyOverlap);
    const auto clashing = build_bin(1025, {"other"}, {LocaleSpec{.tag = "en", .texts = {std::string{"Y"}}}});
    auto clash = catalog.load_binary(clashing);
    AURORA_TEST_REQUIRE(!clash);
    AURORA_TEST_CHECK(clash.error().code_enum == aurora::ErrorCode::I18nVocabularyOverlap);
    // 被拒的两次载入不落库：词条与名字表保持覆盖后的状态。
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, en), std::string("BIN WELCOME"));
    AURORA_TEST_CHECK(catalog.canonical_name(aurora::MessageId{1026}).empty());
}

AURORA_TEST_CASE(id_out_of_range_rejects_and_keeps_catalog_untouched) {
    // Id 越界：区间跨过无效 Id 哨兵 → I18nIdOutOfRange，且失败登记不改动目录（原子性）。
    aurora::MessageCatalog catalog;
    auto out_of_range = catalog.register_vocabulary(aurora::AURORA_INVALID_MESSAGE_ID_VALUE - 1U, {"a", "b", "c"});
    AURORA_TEST_REQUIRE(!out_of_range);
    AURORA_TEST_CHECK(out_of_range.error().code_enum == aurora::ErrorCode::I18nIdOutOfRange);
    AURORA_TEST_CHECK(catalog.canonical_name(aurora::MessageId{0xFFFFFFFEU}).empty());
    AURORA_TEST_CHECK(catalog.find_by_name("a") == aurora::AURORA_INVALID_MESSAGE_ID);
}

AURORA_TEST_CASE(missing_translations_spans_unloaded_locale) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_REQUIRE(catalog.load_binary(sample_bin()));
    // en 全量 → 无缺译；zh 缺 welcome → 明细一条；fr 整档未载 → 全域缺译。
    AURORA_TEST_CHECK(catalog.missing_translations(aurora::Locale{.language = "en"}).empty());
    const auto zh_missing = catalog.missing_translations(aurora::Locale{.language = "zh"});
    AURORA_TEST_REQUIRE(zh_missing.size() == 1);
    AURORA_TEST_CHECK_EQ(zh_missing[0], aurora::MessageId{1024});
    const auto fr_missing = catalog.missing_translations(aurora::Locale{.language = "fr"});
    AURORA_TEST_REQUIRE(fr_missing.size() == 2);
    AURORA_TEST_CHECK_EQ(fr_missing[0], aurora::MessageId{1024});
    AURORA_TEST_CHECK_EQ(fr_missing[1], aurora::MessageId{1025});
}

AURORA_TEST_CASE(engine_hash_uses_grouped_value_and_date_style_tokens) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"files", "due"}).ok());
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_CHECK(catalog.add(en, "files", "{n, plural, one {# file} other {# files}}").ok());
    // 分支内 # 走已分组展示值（不是裸值）。
    AURORA_TEST_CHECK_EQ(
        aurora::LocalizedString::tr(aurora::MessageId{1024}, {{"n", std::int64_t{1234}}}).resolve(&catalog, en),
        std::string("1,234 files"));
    // 日期函数 style token：short / 缺省 medium / long 三档与 format_date 逐位对齐。
    AURORA_TEST_CHECK(catalog.add(en, "due", "{d, date, short}|{d, date}|{d, date, long}").ok());
    AURORA_TEST_CHECK_EQ(aurora::LocalizedString::tr(aurora::MessageId{1025},
                                                     {{"d", aurora::CivilDate{.year = 2025, .month = 10, .day = 25}}})
                             .resolve(&catalog, en),
                         std::string("10/25/25|2025-10-25|October 25, 2025"));
}

AURORA_TEST_CASE(fallback_chain_materializes_six_tiers) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"greet"}).ok());
    const aurora::Locale hans{.language = "zh", .script = "Hans"};
    const aurora::Locale plain{.language = "zh"};
    AURORA_TEST_CHECK(catalog.add(hans, "greet", "SCRIPT").ok());
    AURORA_TEST_CHECK(catalog.add(plain, "greet", "LANG").ok());
    const auto probe = aurora::LocalizedString::tr(aurora::MessageId{1024});
    // zh-Hans-CN：full 缺 → zh-Hans 命中（script 档先于 region/language 档）
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, aurora::Locale{.language = "zh", .script = "Hans", .region = "CN"}),
                         std::string("SCRIPT"));
    // zh-CN：tiers = [zh-CN, zh]（无 script 档），lang 档命中
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, aurora::Locale{.language = "zh", .region = "CN"}),
                         std::string("LANG"));
    // 纯 zh 命中本档
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, plain), std::string("LANG"));
    // 缺省档兜底：fr 请求 → default
    catalog.set_default_locale(hans);
    AURORA_TEST_CHECK_EQ(probe.resolve(&catalog, aurora::Locale{.language = "fr"}), std::string("SCRIPT"));
}

AURORA_TEST_CASE(missing_translations_reports_ids_sorted) {
    aurora::MessageCatalog catalog;
    const auto bin = sample_bin();
    AURORA_TEST_REQUIRE(catalog.load_binary(bin));
    const auto missing = catalog.missing_translations(aurora::Locale{.language = "zh"});
    AURORA_TEST_REQUIRE(missing.size() == 1);
    AURORA_TEST_CHECK_EQ(missing[0], aurora::MessageId{1024});  // welcome 在 zh 无自有词条
    AURORA_TEST_CHECK(catalog.missing_translations(aurora::Locale{.language = "en"}).empty());
}

AURORA_TEST_CASE(engine_named_args_number_and_plural) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"m"}).ok());
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_CHECK(catalog.add(en, "m", "Hello {name}, {count, number} new messages.").ok());
    const auto out = aurora::LocalizedString::tr(aurora::MessageId{1024},
                                                 {{"name", std::string{"Ada"}}, {"count", std::int64_t{1234}}})
                         .resolve(&catalog, en);
    AURORA_TEST_CHECK_EQ(out, std::string("Hello Ada, 1,234 new messages."));
}

AURORA_TEST_CASE(engine_plural_exact_offset_and_hash) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"m"}).ok());
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_CHECK(catalog.add(en, "m", "{n, plural, offset:1 =0 {none} =1 {one more} other {# more}}").ok());
    const auto resolve = [&](std::int64_t v) {
        return aurora::LocalizedString::tr(aurora::MessageId{1024}, {{"n", v}}).resolve(&catalog, en);
    };
    AURORA_TEST_CHECK_EQ(resolve(1), std::string("none"));  // 1 - offset 1 = 0 → =0 精确
    AURORA_TEST_CHECK_EQ(resolve(2), std::string("one more"));  // 2 - 1 = 1 → =1 精确
    AURORA_TEST_CHECK_EQ(resolve(5), std::string("4 more"));  // other：# 显示 offset 后的生效值
}

AURORA_TEST_CASE(engine_select_ordinal_and_select) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"rank", "role"}).ok());
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_CHECK(catalog.add(en, "rank", "{pos, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}").ok());
    const auto rank = [&](std::int64_t pos) {
        return aurora::LocalizedString::tr(aurora::MessageId{1024}, {{"pos", pos}}).resolve(&catalog, en);
    };
    AURORA_TEST_CHECK_EQ(rank(1), std::string("1st"));
    AURORA_TEST_CHECK_EQ(rank(2), std::string("2nd"));
    AURORA_TEST_CHECK_EQ(rank(3), std::string("3rd"));
    AURORA_TEST_CHECK_EQ(rank(11), std::string("11th"));
    AURORA_TEST_CHECK(catalog.add(en, "role", "{role, select, admin {Administrator} other {User}}").ok());
    const auto role = [&](const std::string &value) {
        return aurora::LocalizedString::tr(aurora::MessageId{1025}, {{"role", value}}).resolve(&catalog, en);
    };
    AURORA_TEST_CHECK_EQ(role("admin"), std::string("Administrator"));
    AURORA_TEST_CHECK_EQ(role("guest"), std::string("User"));
}

AURORA_TEST_CASE(engine_date_currency_and_escaping) {
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"when", "price", "quoted"}).ok());
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_CHECK(catalog.add(en, "when", "Due {when, date, long}").ok());
    const auto when = aurora::LocalizedString::tr(aurora::MessageId{1024},
                                                  {{"when", aurora::CivilDate{.year = 2025, .month = 10, .day = 25}}})
                          .resolve(&catalog, en);
    AURORA_TEST_CHECK_EQ(when, std::string("Due October 25, 2025"));
    AURORA_TEST_CHECK(catalog.add(en, "price", "{total, currency, EUR} due").ok());
    const auto price = aurora::LocalizedString::tr(aurora::MessageId{1025}, {{"total", 1234.56}}).resolve(&catalog, en);
    AURORA_TEST_CHECK_EQ(price, std::string("\u20AC1,234.56 due"));
    // MF1 引号子集：'{ → 字面 {；'' → 字面 '
    AURORA_TEST_CHECK(catalog.add(en, "quoted", "'{' literal and ''quoted''").ok());
    const auto quoted = aurora::LocalizedString::tr(aurora::MessageId{1026}).resolve(&catalog, en);
    AURORA_TEST_CHECK_EQ(quoted, std::string("{ literal and 'quoted'"));
}

AURORA_TEST_CASE(hash_scope_follows_icu_numeric_context) {
    // `#` 只在复数上下文里被替换：顶层是字面量，select 套在 plural 里穿透取外层生效值，
    // select 单独出现时同样落回字面（与 ICU MessageFormat 同口径，生成器只报 warn）。
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"top", "nested", "bare"}).ok());
    const aurora::Locale en{.language = "en"};
    AURORA_TEST_CHECK(catalog.add(en, "top", "# files").ok());
    AURORA_TEST_CHECK_EQ(aurora::LocalizedString::tr(aurora::MessageId{1024}).resolve(&catalog, en),
                         std::string("# files"));
    AURORA_TEST_CHECK(catalog
                          .add(en, "nested",
                               "{count, plural, one {{kind, select, file {# file} other {# item}}} "
                               "other {{kind, select, file {# files} other {# items}}}}")
                          .ok());
    const auto nested = [&](std::int64_t count, const std::string &kind) {
        return aurora::LocalizedString::tr(aurora::MessageId{1025}, {{"count", count}, {"kind", kind}})
            .resolve(&catalog, en);
    };
    AURORA_TEST_CHECK_EQ(nested(1, "file"), std::string("1 file"));
    AURORA_TEST_CHECK_EQ(nested(1234, "file"), std::string("1,234 files"));
    AURORA_TEST_CHECK_EQ(nested(1234, "box"), std::string("1,234 items"));
    AURORA_TEST_CHECK(catalog.add(en, "bare", "{kind, select, file {#} other {other}}").ok());
    AURORA_TEST_CHECK_EQ(
        aurora::LocalizedString::tr(aurora::MessageId{1026}, {{"kind", std::string{"file"}}}).resolve(&catalog, en),
        std::string("#"));
}

AURORA_TEST_CASE(unparseable_template_falls_back_to_raw_text) {
    // 数据面结构错误（生成器已拦）运行期降级：回退原文，不抛不崩。
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK(catalog.register_vocabulary(1024, {"broken"}).ok());
    AURORA_TEST_CHECK(catalog.add(aurora::Locale{.language = "en"}, "broken", "{unclosed").ok());
    const auto out =
        aurora::LocalizedString::tr(aurora::MessageId{1024}).resolve(&catalog, aurora::Locale{.language = "en"});
    AURORA_TEST_CHECK_EQ(out, std::string("{unclosed"));
}

AURORA_TEST_CASE(builtin_vocabulary_is_seeded_in_default_catalog) {
    // 进程默认目录预登记内置词表（只读探测；写入用例一律用本地目录，避免跨用例污染）。
    AURORA_TEST_CHECK_EQ(aurora::default_catalog().find_by_name("command_palette.placeholder"),
                         aurora::msg::AURORA_COMMAND_PALETTE_PLACEHOLDER);
    AURORA_TEST_CHECK_EQ(aurora::default_catalog().canonical_name(aurora::msg::AURORA_COMMAND_PALETTE_NO_RESULTS),
                         std::string_view("command_palette.no_results"));
    // 内置词条英文兜底模板作为 bin 播种与基准数据
    AURORA_TEST_CHECK_EQ(aurora::detail::AURORA_BUILTIN_MESSAGE_TEMPLATES[5], std::string_view("No matching commands"));
}

}  // namespace aurora::test_cases::utest_message_catalog
