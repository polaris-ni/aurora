// AUTO-GENERATED from CLDR plural rules — DO NOT EDIT.
// Source: cldr-json cldr-core/supplemental/{plurals,ordinals}.json (CLDR 48,
// Unicode 16.0.0; data NOT vendored, see THIRD_PARTY_LICENSES.md §10).
// Regenerate: python tools/gen/gen_plural_rules.py <plurals.json> <ordinals.json>
#include "aurora/i18n/detail/plural_rules.h"

#include <string_view>

namespace aurora::detail {
namespace {
constexpr std::string_view AURORA_RULE_LANGUAGES[224] = {
    "af",  "ak",    "am",  "an",  "ar",  "ars", "as",  "asa", "ast",      "az",  "bal", "be",  "bem", "bez", "bg",
    "bho", "blo",   "bm",  "bn",  "bo",  "br",  "brx", "bs",  "ca",       "ce",  "ceb", "cgg", "chr", "ckb", "cs",
    "csw", "cv",    "cy",  "da",  "de",  "doi", "dsb", "dv",  "dz",       "ee",  "el",  "en",  "eo",  "es",  "et",
    "eu",  "fa",    "ff",  "fi",  "fil", "fo",  "fr",  "fur", "fy",       "ga",  "gd",  "gl",  "gsw", "gu",  "guw",
    "gv",  "ha",    "haw", "he",  "hi",  "hnj", "hr",  "hsb", "hu",       "hy",  "ia",  "id",  "ie",  "ig",  "ii",
    "io",  "is",    "it",  "iu",  "ja",  "jbo", "jgo", "jmc", "jv",       "jw",  "ka",  "kab", "kaj", "kcg", "kde",
    "kea", "kk",    "kkj", "kl",  "km",  "kn",  "ko",  "kok", "kok-Latn", "ks",  "ksb", "ksh", "ku",  "kw",  "ky",
    "lag", "lb",    "lg",  "lij", "lkt", "lld", "ln",  "lo",  "lt",       "lv",  "mas", "mg",  "mgo", "mk",  "ml",
    "mn",  "mo",    "mr",  "ms",  "mt",  "my",  "nah", "naq", "nb",       "nd",  "ne",  "nl",  "nn",  "nnh", "no",
    "nqo", "nr",    "nso", "ny",  "nyn", "om",  "or",  "os",  "osa",      "pa",  "pap", "pcm", "pl",  "prg", "ps",
    "pt",  "pt-PT", "rm",  "ro",  "rof", "ru",  "rwk", "sah", "saq",      "sat", "sc",  "scn", "sd",  "sdh", "se",
    "seh", "ses",   "sg",  "sgs", "sh",  "shi", "si",  "sk",  "sl",       "sma", "smi", "smj", "smn", "sms", "sn",
    "so",  "sq",    "sr",  "ss",  "ssy", "st",  "su",  "sv",  "sw",       "syr", "ta",  "te",  "teo", "th",  "ti",
    "tig", "tk",    "tl",  "tn",  "to",  "tpi", "tr",  "ts",  "tzm",      "ug",  "uk",  "und", "ur",  "uz",  "ve",
    "vec", "vi",    "vo",  "vun", "wa",  "wae", "wo",  "xh",  "xog",      "yi",  "yo",  "yue", "zh",  "zu"};
constexpr std::uint32_t AURORA_CARDINAL_MASKS[224] = {
    2,  2,  2,  2, 31, 31, 2, 2,  2,  2,  2,  26, 2,  2,  2,  2,  3, 0, 2, 0,  30, 2,  10, 18, 2, 2, 2,  2,
    2,  26, 2,  3, 31, 2,  2, 2,  14, 2,  0,  2,  2,  2,  2,  18, 2, 2, 2, 2,  2,  2,  2,  18, 2, 2, 30, 14,
    2,  2,  2,  2, 30, 2,  2, 6,  2,  0,  10, 14, 2,  2,  2,  0,  2, 0, 0, 2,  2,  18, 6,  0,  0, 2, 2,  0,
    0,  2,  2,  2, 2,  0,  0, 2,  2,  2,  0,  2,  0,  2,  2,  2,  2, 3, 2, 31, 2,  3,  2,  2,  2, 0, 18, 2,
    0,  26, 3,  2, 2,  2,  2, 2,  2,  10, 2,  0,  30, 0,  2,  6,  2, 2, 2, 2,  2,  2,  2,  0,  2, 2, 2,  2,
    2,  2,  2,  0, 2,  2,  2, 26, 3,  2,  18, 18, 2,  10, 2,  26, 2, 0, 2, 6,  2,  18, 2,  2,  6, 2, 0,  0,
    30, 10, 10, 2, 26, 14, 6, 6,  6,  6,  6,  2,  2,  2,  10, 2,  2, 2, 0, 2,  2,  2,  2,  2,  2, 0, 2,  2,
    2,  2,  2,  0, 0,  2,  2, 2,  2,  26, 0,  2,  2,  2,  18, 0,  2, 2, 2, 2,  0,  2,  2,  2,  0, 0, 0,  2};
constexpr std::uint32_t AURORA_ORDINAL_MASKS[224] = {
    0,  0,  0,  0, 0, 0, 30, 0,  0, 26, 2, 8, 0,  0,  0,  0, 11, 0, 30, 0, 0, 0,  0,  14, 0, 0, 0,  0,  0, 0, 0, 0,
    31, 0,  0,  0, 0, 0, 0,  0,  0, 14, 0, 0, 0,  0,  0,  0, 0,  2, 0,  2, 0, 0,  2,  14, 0, 0, 30, 0,  0, 0, 0, 0,
    30, 0,  0,  0, 2, 2, 0,  0,  0, 0,  0, 0, 0,  16, 0,  0, 0,  0, 0,  0, 0, 18, 0,  0,  0, 0, 0,  16, 0, 0, 0, 0,
    0,  14, 14, 0, 0, 0, 0,  18, 0, 0,  0, 0, 16, 0,  16, 0, 2,  0, 0,  0, 0, 0,  22, 0,  0, 2, 14, 2,  0, 0, 0, 0,
    0,  0,  2,  0, 0, 0, 0,  0,  0, 0,  0, 0, 0,  30, 0,  0, 0,  0, 0,  0, 0, 0,  0,  0,  0, 2, 0,  0,  0, 0, 0, 0,
    16, 16, 0,  0, 0, 0, 0,  0,  0, 0,  0, 0, 0,  0,  0,  0, 0,  0, 0,  0, 0, 18, 0,  0,  0, 0, 0,  2,  0, 0, 0, 0,
    0,  0,  0,  0, 8, 2, 0,  0,  0, 0,  0, 0, 0,  8,  0,  0, 0,  0, 16, 2, 0, 0,  0,  0,  0, 0, 0,  0,  0, 0, 0, 0};
}  // namespace

auto plural_rules_language_index(std::string_view key) -> int {
    std::size_t lo = 0;
    std::size_t hi = 224;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int cmp = key.compare(AURORA_RULE_LANGUAGES[mid]);
        if (cmp == 0) {
            return static_cast<int>(mid);
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

auto plural_rules_category_mask(int index, bool ordinal) -> std::uint32_t {
    if (index < 0) {
        return 0U;
    }
    const auto idx = static_cast<std::size_t>(index);
    return ordinal ? AURORA_ORDINAL_MASKS[idx] : AURORA_CARDINAL_MASKS[idx];
}
auto plural_rules_eval_cardinal(int index, const PluralOperands &ops) -> PluralCategory {
    switch (index) {
        case 0:  // af
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 1:  // ak
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 2:  // am
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 3:  // an
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 4:  // ar
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (std::fmod(ops.n, 100.0) >= 3.0 && std::fmod(ops.n, 100.0) <= 10.0) {
                return aurora::PluralCategory::Few;
            }
            if (std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 99.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 5:  // ars
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (std::fmod(ops.n, 100.0) >= 3.0 && std::fmod(ops.n, 100.0) <= 10.0) {
                return aurora::PluralCategory::Few;
            }
            if (std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 99.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 6:  // as
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 7:  // asa
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 8:  // ast
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 9:  // az
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 10:  // bal
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 11:  // be
            if (std::fmod(ops.n, 10.0) == 1.0 && !(std::fmod(ops.n, 100.0) == 11.0)) {
                return aurora::PluralCategory::One;
            }
            if ((std::fmod(ops.n, 10.0) >= 2.0 && std::fmod(ops.n, 10.0) <= 4.0) &&
                !(std::fmod(ops.n, 100.0) >= 12.0 && std::fmod(ops.n, 100.0) <= 14.0)) {
                return aurora::PluralCategory::Few;
            }
            if ((std::fmod(ops.n, 10.0) == 0.0 || (std::fmod(ops.n, 10.0) >= 5.0 && std::fmod(ops.n, 10.0) <= 9.0)) ||
                (std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 14.0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 12:  // bem
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 13:  // bez
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 14:  // bg
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 15:  // bho
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 16:  // blo
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 17:  // bm
            return aurora::PluralCategory::Other;
        case 18:  // bn
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 19:  // bo
            return aurora::PluralCategory::Other;
        case 20:  // br
            if (std::fmod(ops.n, 10.0) == 1.0 && !(std::fmod(ops.n, 100.0) == 11.0 || std::fmod(ops.n, 100.0) == 71.0 ||
                                                   std::fmod(ops.n, 100.0) == 91.0)) {
                return aurora::PluralCategory::One;
            }
            if (std::fmod(ops.n, 10.0) == 2.0 && !(std::fmod(ops.n, 100.0) == 12.0 || std::fmod(ops.n, 100.0) == 72.0 ||
                                                   std::fmod(ops.n, 100.0) == 92.0)) {
                return aurora::PluralCategory::Two;
            }
            if (((std::fmod(ops.n, 10.0) >= 3.0 && std::fmod(ops.n, 10.0) <= 4.0) || std::fmod(ops.n, 10.0) == 9.0) &&
                !((std::fmod(ops.n, 100.0) >= 10.0 && std::fmod(ops.n, 100.0) <= 19.0) ||
                  (std::fmod(ops.n, 100.0) >= 70.0 && std::fmod(ops.n, 100.0) <= 79.0) ||
                  (std::fmod(ops.n, 100.0) >= 90.0 && std::fmod(ops.n, 100.0) <= 99.0))) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.n == 0.0) && std::fmod(ops.n, 1000000.0) == 0.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 21:  // brx
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 22:  // bs
            if (((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) ||
                ((ops.f % 10LL) == 1 && !((ops.f % 100LL) == 11))) {
                return aurora::PluralCategory::One;
            }
            if (((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                 !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) ||
                (((ops.f % 10LL) >= 2 && (ops.f % 10LL) <= 4) && !((ops.f % 100LL) >= 12 && (ops.f % 100LL) <= 14))) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 23:  // ca
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 24:  // ce
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 25:  // ceb
            if (((ops.v == 0 && (ops.i == 1 || ops.i == 2 || ops.i == 3)) ||
                 (ops.v == 0 && !((ops.i % 10LL) == 4 || (ops.i % 10LL) == 6 || (ops.i % 10LL) == 9))) ||
                (!(ops.v == 0) && !((ops.f % 10LL) == 4 || (ops.f % 10LL) == 6 || (ops.f % 10LL) == 9))) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 26:  // cgg
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 27:  // chr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 28:  // ckb
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 29:  // cs
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((ops.i >= 2 && ops.i <= 4) && ops.v == 0) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.v == 0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 30:  // csw
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 31:  // cv
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 32:  // cy
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 3.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 33:  // da
            if (ops.n == 1.0 || (!(ops.t == 0) && (ops.i == 0 || ops.i == 1))) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 34:  // de
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 35:  // doi
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 36:  // dsb
            if ((ops.v == 0 && (ops.i % 100LL) == 1) || (ops.f % 100LL) == 1) {
                return aurora::PluralCategory::One;
            }
            if ((ops.v == 0 && (ops.i % 100LL) == 2) || (ops.f % 100LL) == 2) {
                return aurora::PluralCategory::Two;
            }
            if ((ops.v == 0 && ((ops.i % 100LL) >= 3 && (ops.i % 100LL) <= 4)) ||
                ((ops.f % 100LL) >= 3 && (ops.f % 100LL) <= 4)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 37:  // dv
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 38:  // dz
            return aurora::PluralCategory::Other;
        case 39:  // ee
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 40:  // el
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 41:  // en
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 42:  // eo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 43:  // es
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 44:  // et
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 45:  // eu
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 46:  // fa
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 47:  // ff
            if (ops.i == 0 || ops.i == 1) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 48:  // fi
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 49:  // fil
            if (((ops.v == 0 && (ops.i == 1 || ops.i == 2 || ops.i == 3)) ||
                 (ops.v == 0 && !((ops.i % 10LL) == 4 || (ops.i % 10LL) == 6 || (ops.i % 10LL) == 9))) ||
                (!(ops.v == 0) && !((ops.f % 10LL) == 4 || (ops.f % 10LL) == 6 || (ops.f % 10LL) == 9))) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 50:  // fo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 51:  // fr
            if (ops.i == 0 || ops.i == 1) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 52:  // fur
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 53:  // fy
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 54:  // ga
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n >= 3.0 && ops.n <= 6.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n >= 7.0 && ops.n <= 10.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 55:  // gd
            if (ops.n == 1.0 || ops.n == 11.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 12.0) {
                return aurora::PluralCategory::Two;
            }
            if ((ops.n >= 3.0 && ops.n <= 10.0) || (ops.n >= 13.0 && ops.n <= 19.0)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 56:  // gl
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 57:  // gsw
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 58:  // gu
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 59:  // guw
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 60:  // gv
            if (ops.v == 0 && (ops.i % 10LL) == 1) {
                return aurora::PluralCategory::One;
            }
            if (ops.v == 0 && (ops.i % 10LL) == 2) {
                return aurora::PluralCategory::Two;
            }
            if (ops.v == 0 && ((ops.i % 100LL) == 0 || (ops.i % 100LL) == 20 || (ops.i % 100LL) == 40 ||
                               (ops.i % 100LL) == 60 || (ops.i % 100LL) == 80)) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.v == 0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 61:  // ha
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 62:  // haw
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 63:  // he
            if ((ops.i == 1 && ops.v == 0) || (ops.i == 0 && !(ops.v == 0))) {
                return aurora::PluralCategory::One;
            }
            if (ops.i == 2 && ops.v == 0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 64:  // hi
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 65:  // hnj
            return aurora::PluralCategory::Other;
        case 66:  // hr
            if (((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) ||
                ((ops.f % 10LL) == 1 && !((ops.f % 100LL) == 11))) {
                return aurora::PluralCategory::One;
            }
            if (((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                 !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) ||
                (((ops.f % 10LL) >= 2 && (ops.f % 10LL) <= 4) && !((ops.f % 100LL) >= 12 && (ops.f % 100LL) <= 14))) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 67:  // hsb
            if ((ops.v == 0 && (ops.i % 100LL) == 1) || (ops.f % 100LL) == 1) {
                return aurora::PluralCategory::One;
            }
            if ((ops.v == 0 && (ops.i % 100LL) == 2) || (ops.f % 100LL) == 2) {
                return aurora::PluralCategory::Two;
            }
            if ((ops.v == 0 && ((ops.i % 100LL) >= 3 && (ops.i % 100LL) <= 4)) ||
                ((ops.f % 100LL) >= 3 && (ops.f % 100LL) <= 4)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 68:  // hu
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 69:  // hy
            if (ops.i == 0 || ops.i == 1) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 70:  // ia
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 71:  // id
            return aurora::PluralCategory::Other;
        case 72:  // ie
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 73:  // ig
            return aurora::PluralCategory::Other;
        case 74:  // ii
            return aurora::PluralCategory::Other;
        case 75:  // io
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 76:  // is
            if (((ops.t == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) ||
                ((ops.t % 10LL) == 1 && !((ops.t % 100LL) == 11))) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 77:  // it
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 78:  // iu
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 79:  // ja
            return aurora::PluralCategory::Other;
        case 80:  // jbo
            return aurora::PluralCategory::Other;
        case 81:  // jgo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 82:  // jmc
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 83:  // jv
            return aurora::PluralCategory::Other;
        case 84:  // jw
            return aurora::PluralCategory::Other;
        case 85:  // ka
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 86:  // kab
            if (ops.i == 0 || ops.i == 1) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 87:  // kaj
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 88:  // kcg
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 89:  // kde
            return aurora::PluralCategory::Other;
        case 90:  // kea
            return aurora::PluralCategory::Other;
        case 91:  // kk
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 92:  // kkj
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 93:  // kl
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 94:  // km
            return aurora::PluralCategory::Other;
        case 95:  // kn
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 96:  // ko
            return aurora::PluralCategory::Other;
        case 97:  // kok
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 98:  // kok-Latn
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 99:  // ks
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 100:  // ksb
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 101:  // ksh
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 102:  // ku
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 103:  // kw
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (((std::fmod(ops.n, 100.0) == 2.0 || std::fmod(ops.n, 100.0) == 22.0 ||
                  std::fmod(ops.n, 100.0) == 42.0 || std::fmod(ops.n, 100.0) == 62.0 ||
                  std::fmod(ops.n, 100.0) == 82.0) ||
                 (std::fmod(ops.n, 1000.0) == 0.0 &&
                  ((std::fmod(ops.n, 100000.0) >= 1000.0 && std::fmod(ops.n, 100000.0) <= 20000.0) ||
                   std::fmod(ops.n, 100000.0) == 40000.0 || std::fmod(ops.n, 100000.0) == 60000.0 ||
                   std::fmod(ops.n, 100000.0) == 80000.0))) ||
                (!(ops.n == 0.0) && std::fmod(ops.n, 1000000.0) == 100000.0)) {
                return aurora::PluralCategory::Two;
            }
            if (std::fmod(ops.n, 100.0) == 3.0 || std::fmod(ops.n, 100.0) == 23.0 || std::fmod(ops.n, 100.0) == 43.0 ||
                std::fmod(ops.n, 100.0) == 63.0 || std::fmod(ops.n, 100.0) == 83.0) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.n == 1.0) &&
                (std::fmod(ops.n, 100.0) == 1.0 || std::fmod(ops.n, 100.0) == 21.0 || std::fmod(ops.n, 100.0) == 41.0 ||
                 std::fmod(ops.n, 100.0) == 61.0 || std::fmod(ops.n, 100.0) == 81.0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 104:  // ky
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 105:  // lag
            if (ops.n == 0.0) {
                return aurora::PluralCategory::Zero;
            }
            if ((ops.i == 0 || ops.i == 1) && !(ops.n == 0.0)) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 106:  // lb
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 107:  // lg
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 108:  // lij
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 109:  // lkt
            return aurora::PluralCategory::Other;
        case 110:  // lld
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 111:  // ln
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 112:  // lo
            return aurora::PluralCategory::Other;
        case 113:  // lt
            if (std::fmod(ops.n, 10.0) == 1.0 &&
                !(std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 19.0)) {
                return aurora::PluralCategory::One;
            }
            if ((std::fmod(ops.n, 10.0) >= 2.0 && std::fmod(ops.n, 10.0) <= 9.0) &&
                !(std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 19.0)) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.f == 0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 114:  // lv
            if ((std::fmod(ops.n, 10.0) == 0.0 ||
                 (std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 19.0)) ||
                (ops.v == 2 && ((ops.f % 100LL) >= 11 && (ops.f % 100LL) <= 19))) {
                return aurora::PluralCategory::Zero;
            }
            if (((std::fmod(ops.n, 10.0) == 1.0 && !(std::fmod(ops.n, 100.0) == 11.0)) ||
                 ((ops.v == 2 && (ops.f % 10LL) == 1) && !((ops.f % 100LL) == 11))) ||
                (!(ops.v == 2) && (ops.f % 10LL) == 1)) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 115:  // mas
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 116:  // mg
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 117:  // mgo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 118:  // mk
            if (((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) ||
                ((ops.f % 10LL) == 1 && !((ops.f % 100LL) == 11))) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 119:  // ml
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 120:  // mn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 121:  // mo
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((!(ops.v == 0) || ops.n == 0.0) ||
                (!(ops.n == 1.0) && (std::fmod(ops.n, 100.0) >= 1.0 && std::fmod(ops.n, 100.0) <= 19.0))) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 122:  // mr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 123:  // ms
            return aurora::PluralCategory::Other;
        case 124:  // mt
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 0.0 || (std::fmod(ops.n, 100.0) >= 3.0 && std::fmod(ops.n, 100.0) <= 10.0)) {
                return aurora::PluralCategory::Few;
            }
            if (std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 19.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 125:  // my
            return aurora::PluralCategory::Other;
        case 126:  // nah
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 127:  // naq
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 128:  // nb
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 129:  // nd
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 130:  // ne
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 131:  // nl
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 132:  // nn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 133:  // nnh
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 134:  // no
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 135:  // nqo
            return aurora::PluralCategory::Other;
        case 136:  // nr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 137:  // nso
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 138:  // ny
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 139:  // nyn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 140:  // om
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 141:  // or
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 142:  // os
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 143:  // osa
            return aurora::PluralCategory::Other;
        case 144:  // pa
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 145:  // pap
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 146:  // pcm
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 147:  // pl
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) {
                return aurora::PluralCategory::Few;
            }
            if ((((ops.v == 0 && !(ops.i == 1)) && ((ops.i % 10LL) >= 0 && (ops.i % 10LL) <= 1)) ||
                 (ops.v == 0 && ((ops.i % 10LL) >= 5 && (ops.i % 10LL) <= 9))) ||
                (ops.v == 0 && ((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14))) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 148:  // prg
            if ((std::fmod(ops.n, 10.0) == 0.0 ||
                 (std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 19.0)) ||
                (ops.v == 2 && ((ops.f % 100LL) >= 11 && (ops.f % 100LL) <= 19))) {
                return aurora::PluralCategory::Zero;
            }
            if (((std::fmod(ops.n, 10.0) == 1.0 && !(std::fmod(ops.n, 100.0) == 11.0)) ||
                 ((ops.v == 2 && (ops.f % 10LL) == 1) && !((ops.f % 100LL) == 11))) ||
                (!(ops.v == 2) && (ops.f % 10LL) == 1)) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 149:  // ps
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 150:  // pt
            if (ops.i >= 0 && ops.i <= 1) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 151:  // pt-PT
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 152:  // rm
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 153:  // ro
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((!(ops.v == 0) || ops.n == 0.0) ||
                (!(ops.n == 1.0) && (std::fmod(ops.n, 100.0) >= 1.0 && std::fmod(ops.n, 100.0) <= 19.0))) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 154:  // rof
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 155:  // ru
            if ((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) {
                return aurora::PluralCategory::One;
            }
            if ((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) {
                return aurora::PluralCategory::Few;
            }
            if (((ops.v == 0 && (ops.i % 10LL) == 0) || (ops.v == 0 && ((ops.i % 10LL) >= 5 && (ops.i % 10LL) <= 9))) ||
                (ops.v == 0 && ((ops.i % 100LL) >= 11 && (ops.i % 100LL) <= 14))) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 156:  // rwk
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 157:  // sah
            return aurora::PluralCategory::Other;
        case 158:  // saq
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 159:  // sat
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 160:  // sc
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 161:  // scn
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 162:  // sd
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 163:  // sdh
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 164:  // se
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 165:  // seh
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 166:  // ses
            return aurora::PluralCategory::Other;
        case 167:  // sg
            return aurora::PluralCategory::Other;
        case 168:  // sgs
            if (std::fmod(ops.n, 10.0) == 1.0 && !(std::fmod(ops.n, 100.0) == 11.0)) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if ((!(ops.n == 2.0) && (std::fmod(ops.n, 10.0) >= 2.0 && std::fmod(ops.n, 10.0) <= 9.0)) &&
                !(std::fmod(ops.n, 100.0) >= 11.0 && std::fmod(ops.n, 100.0) <= 19.0)) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.f == 0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 169:  // sh
            if (((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) ||
                ((ops.f % 10LL) == 1 && !((ops.f % 100LL) == 11))) {
                return aurora::PluralCategory::One;
            }
            if (((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                 !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) ||
                (((ops.f % 10LL) >= 2 && (ops.f % 10LL) <= 4) && !((ops.f % 100LL) >= 12 && (ops.f % 100LL) <= 14))) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 170:  // shi
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n >= 2.0 && ops.n <= 10.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 171:  // si
            if ((ops.n == 0.0 || ops.n == 1.0) || (ops.i == 0 && ops.f == 1)) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 172:  // sk
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((ops.i >= 2 && ops.i <= 4) && ops.v == 0) {
                return aurora::PluralCategory::Few;
            }
            if (!(ops.v == 0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 173:  // sl
            if (ops.v == 0 && (ops.i % 100LL) == 1) {
                return aurora::PluralCategory::One;
            }
            if (ops.v == 0 && (ops.i % 100LL) == 2) {
                return aurora::PluralCategory::Two;
            }
            if ((ops.v == 0 && ((ops.i % 100LL) >= 3 && (ops.i % 100LL) <= 4)) || !(ops.v == 0)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 174:  // sma
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 175:  // smi
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 176:  // smj
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 177:  // smn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 178:  // sms
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            return aurora::PluralCategory::Other;
        case 179:  // sn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 180:  // so
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 181:  // sq
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 182:  // sr
            if (((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) ||
                ((ops.f % 10LL) == 1 && !((ops.f % 100LL) == 11))) {
                return aurora::PluralCategory::One;
            }
            if (((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                 !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) ||
                (((ops.f % 10LL) >= 2 && (ops.f % 10LL) <= 4) && !((ops.f % 100LL) >= 12 && (ops.f % 100LL) <= 14))) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 183:  // ss
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 184:  // ssy
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 185:  // st
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 186:  // su
            return aurora::PluralCategory::Other;
        case 187:  // sv
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 188:  // sw
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 189:  // syr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 190:  // ta
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 191:  // te
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 192:  // teo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 193:  // th
            return aurora::PluralCategory::Other;
        case 194:  // ti
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 195:  // tig
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 196:  // tk
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 197:  // tl
            if (((ops.v == 0 && (ops.i == 1 || ops.i == 2 || ops.i == 3)) ||
                 (ops.v == 0 && !((ops.i % 10LL) == 4 || (ops.i % 10LL) == 6 || (ops.i % 10LL) == 9))) ||
                (!(ops.v == 0) && !((ops.f % 10LL) == 4 || (ops.f % 10LL) == 6 || (ops.f % 10LL) == 9))) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 198:  // tn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 199:  // to
            return aurora::PluralCategory::Other;
        case 200:  // tpi
            return aurora::PluralCategory::Other;
        case 201:  // tr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 202:  // ts
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 203:  // tzm
            if ((ops.n >= 0.0 && ops.n <= 1.0) || (ops.n >= 11.0 && ops.n <= 99.0)) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 204:  // ug
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 205:  // uk
            if ((ops.v == 0 && (ops.i % 10LL) == 1) && !((ops.i % 100LL) == 11)) {
                return aurora::PluralCategory::One;
            }
            if ((ops.v == 0 && ((ops.i % 10LL) >= 2 && (ops.i % 10LL) <= 4)) &&
                !((ops.i % 100LL) >= 12 && (ops.i % 100LL) <= 14)) {
                return aurora::PluralCategory::Few;
            }
            if (((ops.v == 0 && (ops.i % 10LL) == 0) || (ops.v == 0 && ((ops.i % 10LL) >= 5 && (ops.i % 10LL) <= 9))) ||
                (ops.v == 0 && ((ops.i % 100LL) >= 11 && (ops.i % 100LL) <= 14))) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 206:  // und
            return aurora::PluralCategory::Other;
        case 207:  // ur
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 208:  // uz
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 209:  // ve
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 210:  // vec
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            if ((((0LL == 0 && !(ops.i == 0)) && (ops.i % 1000000LL) == 0) && ops.v == 0) || !(0LL >= 0 && 0LL <= 5)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 211:  // vi
            return aurora::PluralCategory::Other;
        case 212:  // vo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 213:  // vun
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 214:  // wa
            if (ops.n >= 0.0 && ops.n <= 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 215:  // wae
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 216:  // wo
            return aurora::PluralCategory::Other;
        case 217:  // xh
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 218:  // xog
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 219:  // yi
            if (ops.i == 1 && ops.v == 0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 220:  // yo
            return aurora::PluralCategory::Other;
        case 221:  // yue
            return aurora::PluralCategory::Other;
        case 222:  // zh
            return aurora::PluralCategory::Other;
        case 223:  // zu
            if (ops.i == 0 || ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        default:
            return aurora::PluralCategory::Other;
    }
}

auto plural_rules_eval_ordinal(int index, const PluralOperands &ops) -> PluralCategory {
    switch (index) {
        case 0:  // af
            return aurora::PluralCategory::Other;
        case 1:  // ak
            return aurora::PluralCategory::Other;
        case 2:  // am
            return aurora::PluralCategory::Other;
        case 3:  // an
            return aurora::PluralCategory::Other;
        case 4:  // ar
            return aurora::PluralCategory::Other;
        case 5:  // ars
            return aurora::PluralCategory::Other;
        case 6:  // as
            if (ops.n == 1.0 || ops.n == 5.0 || ops.n == 7.0 || ops.n == 8.0 || ops.n == 9.0 || ops.n == 10.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 7:  // asa
            return aurora::PluralCategory::Other;
        case 8:  // ast
            return aurora::PluralCategory::Other;
        case 9:  // az
            if (((ops.i % 10LL) == 1 || (ops.i % 10LL) == 2 || (ops.i % 10LL) == 5 || (ops.i % 10LL) == 7 ||
                 (ops.i % 10LL) == 8) ||
                ((ops.i % 100LL) == 20 || (ops.i % 100LL) == 50 || (ops.i % 100LL) == 70 || (ops.i % 100LL) == 80)) {
                return aurora::PluralCategory::One;
            }
            if (((ops.i % 10LL) == 3 || (ops.i % 10LL) == 4) ||
                ((ops.i % 1000LL) == 100 || (ops.i % 1000LL) == 200 || (ops.i % 1000LL) == 300 ||
                 (ops.i % 1000LL) == 400 || (ops.i % 1000LL) == 500 || (ops.i % 1000LL) == 600 ||
                 (ops.i % 1000LL) == 700 || (ops.i % 1000LL) == 800 || (ops.i % 1000LL) == 900)) {
                return aurora::PluralCategory::Few;
            }
            if ((ops.i == 0 || (ops.i % 10LL) == 6) ||
                ((ops.i % 100LL) == 40 || (ops.i % 100LL) == 60 || (ops.i % 100LL) == 90)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 10:  // bal
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 11:  // be
            if ((std::fmod(ops.n, 10.0) == 2.0 || std::fmod(ops.n, 10.0) == 3.0) &&
                !(std::fmod(ops.n, 100.0) == 12.0 || std::fmod(ops.n, 100.0) == 13.0)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 12:  // bem
            return aurora::PluralCategory::Other;
        case 13:  // bez
            return aurora::PluralCategory::Other;
        case 14:  // bg
            return aurora::PluralCategory::Other;
        case 15:  // bho
            return aurora::PluralCategory::Other;
        case 16:  // blo
            if (ops.i == 0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.i == 1) {
                return aurora::PluralCategory::One;
            }
            if (ops.i == 2 || ops.i == 3 || ops.i == 4 || ops.i == 5 || ops.i == 6) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 17:  // bm
            return aurora::PluralCategory::Other;
        case 18:  // bn
            if (ops.n == 1.0 || ops.n == 5.0 || ops.n == 7.0 || ops.n == 8.0 || ops.n == 9.0 || ops.n == 10.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 19:  // bo
            return aurora::PluralCategory::Other;
        case 20:  // br
            return aurora::PluralCategory::Other;
        case 21:  // brx
            return aurora::PluralCategory::Other;
        case 22:  // bs
            return aurora::PluralCategory::Other;
        case 23:  // ca
            if (ops.n == 1.0 || ops.n == 3.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 24:  // ce
            return aurora::PluralCategory::Other;
        case 25:  // ceb
            return aurora::PluralCategory::Other;
        case 26:  // cgg
            return aurora::PluralCategory::Other;
        case 27:  // chr
            return aurora::PluralCategory::Other;
        case 28:  // ckb
            return aurora::PluralCategory::Other;
        case 29:  // cs
            return aurora::PluralCategory::Other;
        case 30:  // csw
            return aurora::PluralCategory::Other;
        case 31:  // cv
            return aurora::PluralCategory::Other;
        case 32:  // cy
            if (ops.n == 0.0 || ops.n == 7.0 || ops.n == 8.0 || ops.n == 9.0) {
                return aurora::PluralCategory::Zero;
            }
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 3.0 || ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 5.0 || ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 33:  // da
            return aurora::PluralCategory::Other;
        case 34:  // de
            return aurora::PluralCategory::Other;
        case 35:  // doi
            return aurora::PluralCategory::Other;
        case 36:  // dsb
            return aurora::PluralCategory::Other;
        case 37:  // dv
            return aurora::PluralCategory::Other;
        case 38:  // dz
            return aurora::PluralCategory::Other;
        case 39:  // ee
            return aurora::PluralCategory::Other;
        case 40:  // el
            return aurora::PluralCategory::Other;
        case 41:  // en
            if (std::fmod(ops.n, 10.0) == 1.0 && !(std::fmod(ops.n, 100.0) == 11.0)) {
                return aurora::PluralCategory::One;
            }
            if (std::fmod(ops.n, 10.0) == 2.0 && !(std::fmod(ops.n, 100.0) == 12.0)) {
                return aurora::PluralCategory::Two;
            }
            if (std::fmod(ops.n, 10.0) == 3.0 && !(std::fmod(ops.n, 100.0) == 13.0)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 42:  // eo
            return aurora::PluralCategory::Other;
        case 43:  // es
            return aurora::PluralCategory::Other;
        case 44:  // et
            return aurora::PluralCategory::Other;
        case 45:  // eu
            return aurora::PluralCategory::Other;
        case 46:  // fa
            return aurora::PluralCategory::Other;
        case 47:  // ff
            return aurora::PluralCategory::Other;
        case 48:  // fi
            return aurora::PluralCategory::Other;
        case 49:  // fil
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 50:  // fo
            return aurora::PluralCategory::Other;
        case 51:  // fr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 52:  // fur
            return aurora::PluralCategory::Other;
        case 53:  // fy
            return aurora::PluralCategory::Other;
        case 54:  // ga
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 55:  // gd
            if (ops.n == 1.0 || ops.n == 11.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 12.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 3.0 || ops.n == 13.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 56:  // gl
            return aurora::PluralCategory::Other;
        case 57:  // gsw
            return aurora::PluralCategory::Other;
        case 58:  // gu
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 59:  // guw
            return aurora::PluralCategory::Other;
        case 60:  // gv
            return aurora::PluralCategory::Other;
        case 61:  // ha
            return aurora::PluralCategory::Other;
        case 62:  // haw
            return aurora::PluralCategory::Other;
        case 63:  // he
            return aurora::PluralCategory::Other;
        case 64:  // hi
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 65:  // hnj
            return aurora::PluralCategory::Other;
        case 66:  // hr
            return aurora::PluralCategory::Other;
        case 67:  // hsb
            return aurora::PluralCategory::Other;
        case 68:  // hu
            if (ops.n == 1.0 || ops.n == 5.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 69:  // hy
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 70:  // ia
            return aurora::PluralCategory::Other;
        case 71:  // id
            return aurora::PluralCategory::Other;
        case 72:  // ie
            return aurora::PluralCategory::Other;
        case 73:  // ig
            return aurora::PluralCategory::Other;
        case 74:  // ii
            return aurora::PluralCategory::Other;
        case 75:  // io
            return aurora::PluralCategory::Other;
        case 76:  // is
            return aurora::PluralCategory::Other;
        case 77:  // it
            if (ops.n == 11.0 || ops.n == 8.0 || ops.n == 80.0 || ops.n == 800.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 78:  // iu
            return aurora::PluralCategory::Other;
        case 79:  // ja
            return aurora::PluralCategory::Other;
        case 80:  // jbo
            return aurora::PluralCategory::Other;
        case 81:  // jgo
            return aurora::PluralCategory::Other;
        case 82:  // jmc
            return aurora::PluralCategory::Other;
        case 83:  // jv
            return aurora::PluralCategory::Other;
        case 84:  // jw
            return aurora::PluralCategory::Other;
        case 85:  // ka
            if (ops.i == 1) {
                return aurora::PluralCategory::One;
            }
            if (ops.i == 0 || (((ops.i % 100LL) >= 2 && (ops.i % 100LL) <= 20) || (ops.i % 100LL) == 40 ||
                               (ops.i % 100LL) == 60 || (ops.i % 100LL) == 80)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 86:  // kab
            return aurora::PluralCategory::Other;
        case 87:  // kaj
            return aurora::PluralCategory::Other;
        case 88:  // kcg
            return aurora::PluralCategory::Other;
        case 89:  // kde
            return aurora::PluralCategory::Other;
        case 90:  // kea
            return aurora::PluralCategory::Other;
        case 91:  // kk
            if ((std::fmod(ops.n, 10.0) == 6.0 || std::fmod(ops.n, 10.0) == 9.0) ||
                (std::fmod(ops.n, 10.0) == 0.0 && !(ops.n == 0.0))) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 92:  // kkj
            return aurora::PluralCategory::Other;
        case 93:  // kl
            return aurora::PluralCategory::Other;
        case 94:  // km
            return aurora::PluralCategory::Other;
        case 95:  // kn
            return aurora::PluralCategory::Other;
        case 96:  // ko
            return aurora::PluralCategory::Other;
        case 97:  // kok
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 98:  // kok-Latn
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 99:  // ks
            return aurora::PluralCategory::Other;
        case 100:  // ksb
            return aurora::PluralCategory::Other;
        case 101:  // ksh
            return aurora::PluralCategory::Other;
        case 102:  // ku
            return aurora::PluralCategory::Other;
        case 103:  // kw
            if ((ops.n >= 1.0 && ops.n <= 4.0) ||
                ((std::fmod(ops.n, 100.0) >= 1.0 && std::fmod(ops.n, 100.0) <= 4.0) ||
                 (std::fmod(ops.n, 100.0) >= 21.0 && std::fmod(ops.n, 100.0) <= 24.0) ||
                 (std::fmod(ops.n, 100.0) >= 41.0 && std::fmod(ops.n, 100.0) <= 44.0) ||
                 (std::fmod(ops.n, 100.0) >= 61.0 && std::fmod(ops.n, 100.0) <= 64.0) ||
                 (std::fmod(ops.n, 100.0) >= 81.0 && std::fmod(ops.n, 100.0) <= 84.0))) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 5.0 || std::fmod(ops.n, 100.0) == 5.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 104:  // ky
            return aurora::PluralCategory::Other;
        case 105:  // lag
            return aurora::PluralCategory::Other;
        case 106:  // lb
            return aurora::PluralCategory::Other;
        case 107:  // lg
            return aurora::PluralCategory::Other;
        case 108:  // lij
            if (ops.n == 11.0 || ops.n == 8.0 || (ops.n >= 80.0 && ops.n <= 89.0) ||
                (ops.n >= 800.0 && ops.n <= 899.0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 109:  // lkt
            return aurora::PluralCategory::Other;
        case 110:  // lld
            if (ops.n == 11.0 || ops.n == 8.0 || ops.n == 80.0 || ops.n == 800.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 111:  // ln
            return aurora::PluralCategory::Other;
        case 112:  // lo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 113:  // lt
            return aurora::PluralCategory::Other;
        case 114:  // lv
            return aurora::PluralCategory::Other;
        case 115:  // mas
            return aurora::PluralCategory::Other;
        case 116:  // mg
            return aurora::PluralCategory::Other;
        case 117:  // mgo
            return aurora::PluralCategory::Other;
        case 118:  // mk
            if ((ops.i % 10LL) == 1 && !((ops.i % 100LL) == 11)) {
                return aurora::PluralCategory::One;
            }
            if ((ops.i % 10LL) == 2 && !((ops.i % 100LL) == 12)) {
                return aurora::PluralCategory::Two;
            }
            if (((ops.i % 10LL) == 7 || (ops.i % 10LL) == 8) && !((ops.i % 100LL) == 17 || (ops.i % 100LL) == 18)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 119:  // ml
            return aurora::PluralCategory::Other;
        case 120:  // mn
            return aurora::PluralCategory::Other;
        case 121:  // mo
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 122:  // mr
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 123:  // ms
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 124:  // mt
            return aurora::PluralCategory::Other;
        case 125:  // my
            return aurora::PluralCategory::Other;
        case 126:  // nah
            return aurora::PluralCategory::Other;
        case 127:  // naq
            return aurora::PluralCategory::Other;
        case 128:  // nb
            return aurora::PluralCategory::Other;
        case 129:  // nd
            return aurora::PluralCategory::Other;
        case 130:  // ne
            if (ops.n >= 1.0 && ops.n <= 4.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 131:  // nl
            return aurora::PluralCategory::Other;
        case 132:  // nn
            return aurora::PluralCategory::Other;
        case 133:  // nnh
            return aurora::PluralCategory::Other;
        case 134:  // no
            return aurora::PluralCategory::Other;
        case 135:  // nqo
            return aurora::PluralCategory::Other;
        case 136:  // nr
            return aurora::PluralCategory::Other;
        case 137:  // nso
            return aurora::PluralCategory::Other;
        case 138:  // ny
            return aurora::PluralCategory::Other;
        case 139:  // nyn
            return aurora::PluralCategory::Other;
        case 140:  // om
            return aurora::PluralCategory::Other;
        case 141:  // or
            if (ops.n == 1.0 || ops.n == 5.0 || (ops.n >= 7.0 && ops.n <= 9.0)) {
                return aurora::PluralCategory::One;
            }
            if (ops.n == 2.0 || ops.n == 3.0) {
                return aurora::PluralCategory::Two;
            }
            if (ops.n == 4.0) {
                return aurora::PluralCategory::Few;
            }
            if (ops.n == 6.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 142:  // os
            return aurora::PluralCategory::Other;
        case 143:  // osa
            return aurora::PluralCategory::Other;
        case 144:  // pa
            return aurora::PluralCategory::Other;
        case 145:  // pap
            return aurora::PluralCategory::Other;
        case 146:  // pcm
            return aurora::PluralCategory::Other;
        case 147:  // pl
            return aurora::PluralCategory::Other;
        case 148:  // prg
            return aurora::PluralCategory::Other;
        case 149:  // ps
            return aurora::PluralCategory::Other;
        case 150:  // pt
            return aurora::PluralCategory::Other;
        case 151:  // pt-PT
            return aurora::PluralCategory::Other;
        case 152:  // rm
            return aurora::PluralCategory::Other;
        case 153:  // ro
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 154:  // rof
            return aurora::PluralCategory::Other;
        case 155:  // ru
            return aurora::PluralCategory::Other;
        case 156:  // rwk
            return aurora::PluralCategory::Other;
        case 157:  // sah
            return aurora::PluralCategory::Other;
        case 158:  // saq
            return aurora::PluralCategory::Other;
        case 159:  // sat
            return aurora::PluralCategory::Other;
        case 160:  // sc
            if (ops.n == 11.0 || ops.n == 8.0 || ops.n == 80.0 || ops.n == 800.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 161:  // scn
            if (ops.n == 11.0 || ops.n == 8.0 || (ops.n >= 80.0 && ops.n <= 89.0) ||
                (ops.n >= 800.0 && ops.n <= 899.0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 162:  // sd
            return aurora::PluralCategory::Other;
        case 163:  // sdh
            return aurora::PluralCategory::Other;
        case 164:  // se
            return aurora::PluralCategory::Other;
        case 165:  // seh
            return aurora::PluralCategory::Other;
        case 166:  // ses
            return aurora::PluralCategory::Other;
        case 167:  // sg
            return aurora::PluralCategory::Other;
        case 168:  // sgs
            return aurora::PluralCategory::Other;
        case 169:  // sh
            return aurora::PluralCategory::Other;
        case 170:  // shi
            return aurora::PluralCategory::Other;
        case 171:  // si
            return aurora::PluralCategory::Other;
        case 172:  // sk
            return aurora::PluralCategory::Other;
        case 173:  // sl
            return aurora::PluralCategory::Other;
        case 174:  // sma
            return aurora::PluralCategory::Other;
        case 175:  // smi
            return aurora::PluralCategory::Other;
        case 176:  // smj
            return aurora::PluralCategory::Other;
        case 177:  // smn
            return aurora::PluralCategory::Other;
        case 178:  // sms
            return aurora::PluralCategory::Other;
        case 179:  // sn
            return aurora::PluralCategory::Other;
        case 180:  // so
            return aurora::PluralCategory::Other;
        case 181:  // sq
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            if (std::fmod(ops.n, 10.0) == 4.0 && !(std::fmod(ops.n, 100.0) == 14.0)) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 182:  // sr
            return aurora::PluralCategory::Other;
        case 183:  // ss
            return aurora::PluralCategory::Other;
        case 184:  // ssy
            return aurora::PluralCategory::Other;
        case 185:  // st
            return aurora::PluralCategory::Other;
        case 186:  // su
            return aurora::PluralCategory::Other;
        case 187:  // sv
            if ((std::fmod(ops.n, 10.0) == 1.0 || std::fmod(ops.n, 10.0) == 2.0) &&
                !(std::fmod(ops.n, 100.0) == 11.0 || std::fmod(ops.n, 100.0) == 12.0)) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 188:  // sw
            return aurora::PluralCategory::Other;
        case 189:  // syr
            return aurora::PluralCategory::Other;
        case 190:  // ta
            return aurora::PluralCategory::Other;
        case 191:  // te
            return aurora::PluralCategory::Other;
        case 192:  // teo
            return aurora::PluralCategory::Other;
        case 193:  // th
            return aurora::PluralCategory::Other;
        case 194:  // ti
            return aurora::PluralCategory::Other;
        case 195:  // tig
            return aurora::PluralCategory::Other;
        case 196:  // tk
            if ((std::fmod(ops.n, 10.0) == 6.0 || std::fmod(ops.n, 10.0) == 9.0) || ops.n == 10.0) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 197:  // tl
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 198:  // tn
            return aurora::PluralCategory::Other;
        case 199:  // to
            return aurora::PluralCategory::Other;
        case 200:  // tpi
            return aurora::PluralCategory::Other;
        case 201:  // tr
            return aurora::PluralCategory::Other;
        case 202:  // ts
            return aurora::PluralCategory::Other;
        case 203:  // tzm
            return aurora::PluralCategory::Other;
        case 204:  // ug
            return aurora::PluralCategory::Other;
        case 205:  // uk
            if (std::fmod(ops.n, 10.0) == 3.0 && !(std::fmod(ops.n, 100.0) == 13.0)) {
                return aurora::PluralCategory::Few;
            }
            return aurora::PluralCategory::Other;
        case 206:  // und
            return aurora::PluralCategory::Other;
        case 207:  // ur
            return aurora::PluralCategory::Other;
        case 208:  // uz
            return aurora::PluralCategory::Other;
        case 209:  // ve
            return aurora::PluralCategory::Other;
        case 210:  // vec
            if (ops.n == 11.0 || ops.n == 8.0 || ops.n == 80.0 || ops.n == 800.0) {
                return aurora::PluralCategory::Many;
            }
            return aurora::PluralCategory::Other;
        case 211:  // vi
            if (ops.n == 1.0) {
                return aurora::PluralCategory::One;
            }
            return aurora::PluralCategory::Other;
        case 212:  // vo
            return aurora::PluralCategory::Other;
        case 213:  // vun
            return aurora::PluralCategory::Other;
        case 214:  // wa
            return aurora::PluralCategory::Other;
        case 215:  // wae
            return aurora::PluralCategory::Other;
        case 216:  // wo
            return aurora::PluralCategory::Other;
        case 217:  // xh
            return aurora::PluralCategory::Other;
        case 218:  // xog
            return aurora::PluralCategory::Other;
        case 219:  // yi
            return aurora::PluralCategory::Other;
        case 220:  // yo
            return aurora::PluralCategory::Other;
        case 221:  // yue
            return aurora::PluralCategory::Other;
        case 222:  // zh
            return aurora::PluralCategory::Other;
        case 223:  // zu
            return aurora::PluralCategory::Other;
        default:
            return aurora::PluralCategory::Other;
    }
}

}  // namespace aurora::detail
