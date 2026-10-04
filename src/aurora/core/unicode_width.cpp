// ============================================================
// Unicode 码点宽度判定（src/aurora/core/unicode_width.cpp）
// ------------------------------------------------------------
// 声明侧的契约与判定次序见 include/aurora/core/unicode_width.h，本文件只有数据与查表。
//
// 三张表由 Unicode CDATA 的 EastAsianWidth-18.0.0.txt 与 DerivedGeneralCategory-18.0.0.txt
// （均 2026-06-29）合并相邻区间生成，许可与再生成口径见 THIRD_PARTY_LICENSES.md。
// 表须保持「按 first 升序且互不相交」——upper_bound 的正确性全靠它，改动数据后跑
// utest_unicode_width 的序性用例。
// ============================================================

#include "aurora/core/unicode_width.h"

#include <algorithm>
#include <array>
#include <span>

namespace aurora {
namespace {

// 一段连续的码点区间，含首含尾。
struct CodePointRange {
    char32_t first;
    char32_t last;
};

// General_Category ∈ {Mn, Me, Cf}：组合符号与格式字符，不独立占格。
constexpr std::array<CodePointRange, 379> AURORA_ZERO_WIDTH_RANGES{{
    {.first = 0x00ADU, .last = 0x00ADU},   {.first = 0x0300U, .last = 0x036FU},   {.first = 0x0483U, .last = 0x0489U},
    {.first = 0x0591U, .last = 0x05BDU},   {.first = 0x05BFU, .last = 0x05BFU},   {.first = 0x05C1U, .last = 0x05C2U},
    {.first = 0x05C4U, .last = 0x05C5U},   {.first = 0x05C7U, .last = 0x05C9U},   {.first = 0x0600U, .last = 0x0605U},
    {.first = 0x0610U, .last = 0x061AU},   {.first = 0x061CU, .last = 0x061CU},   {.first = 0x064BU, .last = 0x065FU},
    {.first = 0x0670U, .last = 0x0670U},   {.first = 0x06D6U, .last = 0x06DDU},   {.first = 0x06DFU, .last = 0x06E4U},
    {.first = 0x06E7U, .last = 0x06E8U},   {.first = 0x06EAU, .last = 0x06EDU},   {.first = 0x070FU, .last = 0x070FU},
    {.first = 0x0711U, .last = 0x0711U},   {.first = 0x0730U, .last = 0x074AU},   {.first = 0x07A6U, .last = 0x07B0U},
    {.first = 0x07EBU, .last = 0x07F3U},   {.first = 0x07FDU, .last = 0x07FDU},   {.first = 0x0816U, .last = 0x0819U},
    {.first = 0x081BU, .last = 0x0823U},   {.first = 0x0825U, .last = 0x0827U},   {.first = 0x0829U, .last = 0x082DU},
    {.first = 0x0859U, .last = 0x085BU},   {.first = 0x0890U, .last = 0x0891U},   {.first = 0x0897U, .last = 0x089FU},
    {.first = 0x08CAU, .last = 0x0902U},   {.first = 0x093AU, .last = 0x093AU},   {.first = 0x093CU, .last = 0x093CU},
    {.first = 0x0941U, .last = 0x0948U},   {.first = 0x094DU, .last = 0x094DU},   {.first = 0x0951U, .last = 0x0957U},
    {.first = 0x0962U, .last = 0x0963U},   {.first = 0x0981U, .last = 0x0981U},   {.first = 0x09BCU, .last = 0x09BCU},
    {.first = 0x09C1U, .last = 0x09C4U},   {.first = 0x09CDU, .last = 0x09CDU},   {.first = 0x09E2U, .last = 0x09E3U},
    {.first = 0x09FEU, .last = 0x09FEU},   {.first = 0x0A01U, .last = 0x0A02U},   {.first = 0x0A3CU, .last = 0x0A3CU},
    {.first = 0x0A41U, .last = 0x0A42U},   {.first = 0x0A47U, .last = 0x0A48U},   {.first = 0x0A4BU, .last = 0x0A4DU},
    {.first = 0x0A51U, .last = 0x0A51U},   {.first = 0x0A70U, .last = 0x0A71U},   {.first = 0x0A75U, .last = 0x0A75U},
    {.first = 0x0A81U, .last = 0x0A82U},   {.first = 0x0ABCU, .last = 0x0ABCU},   {.first = 0x0AC1U, .last = 0x0AC5U},
    {.first = 0x0AC7U, .last = 0x0AC8U},   {.first = 0x0ACDU, .last = 0x0ACDU},   {.first = 0x0AE2U, .last = 0x0AE3U},
    {.first = 0x0AFAU, .last = 0x0AFFU},   {.first = 0x0B01U, .last = 0x0B01U},   {.first = 0x0B3CU, .last = 0x0B3CU},
    {.first = 0x0B3FU, .last = 0x0B3FU},   {.first = 0x0B41U, .last = 0x0B44U},   {.first = 0x0B4DU, .last = 0x0B4DU},
    {.first = 0x0B53U, .last = 0x0B56U},   {.first = 0x0B62U, .last = 0x0B63U},   {.first = 0x0B82U, .last = 0x0B82U},
    {.first = 0x0BC0U, .last = 0x0BC0U},   {.first = 0x0BCDU, .last = 0x0BCDU},   {.first = 0x0C00U, .last = 0x0C00U},
    {.first = 0x0C04U, .last = 0x0C04U},   {.first = 0x0C3CU, .last = 0x0C3CU},   {.first = 0x0C3EU, .last = 0x0C40U},
    {.first = 0x0C46U, .last = 0x0C48U},   {.first = 0x0C4AU, .last = 0x0C4DU},   {.first = 0x0C55U, .last = 0x0C56U},
    {.first = 0x0C62U, .last = 0x0C63U},   {.first = 0x0C81U, .last = 0x0C81U},   {.first = 0x0CBCU, .last = 0x0CBCU},
    {.first = 0x0CBFU, .last = 0x0CBFU},   {.first = 0x0CC6U, .last = 0x0CC6U},   {.first = 0x0CCCU, .last = 0x0CCDU},
    {.first = 0x0CE2U, .last = 0x0CE3U},   {.first = 0x0D00U, .last = 0x0D01U},   {.first = 0x0D3BU, .last = 0x0D3CU},
    {.first = 0x0D41U, .last = 0x0D44U},   {.first = 0x0D4DU, .last = 0x0D4DU},   {.first = 0x0D62U, .last = 0x0D63U},
    {.first = 0x0D81U, .last = 0x0D81U},   {.first = 0x0DCAU, .last = 0x0DCAU},   {.first = 0x0DD2U, .last = 0x0DD4U},
    {.first = 0x0DD6U, .last = 0x0DD6U},   {.first = 0x0E31U, .last = 0x0E31U},   {.first = 0x0E34U, .last = 0x0E3AU},
    {.first = 0x0E47U, .last = 0x0E4EU},   {.first = 0x0EB1U, .last = 0x0EB1U},   {.first = 0x0EB4U, .last = 0x0EBCU},
    {.first = 0x0EC8U, .last = 0x0ECEU},   {.first = 0x0F18U, .last = 0x0F19U},   {.first = 0x0F35U, .last = 0x0F35U},
    {.first = 0x0F37U, .last = 0x0F37U},   {.first = 0x0F39U, .last = 0x0F39U},   {.first = 0x0F71U, .last = 0x0F7EU},
    {.first = 0x0F80U, .last = 0x0F84U},   {.first = 0x0F86U, .last = 0x0F87U},   {.first = 0x0F8DU, .last = 0x0F97U},
    {.first = 0x0F99U, .last = 0x0FBCU},   {.first = 0x0FC6U, .last = 0x0FC6U},   {.first = 0x102DU, .last = 0x1030U},
    {.first = 0x1032U, .last = 0x1037U},   {.first = 0x1039U, .last = 0x103AU},   {.first = 0x103DU, .last = 0x103EU},
    {.first = 0x1058U, .last = 0x1059U},   {.first = 0x105EU, .last = 0x1060U},   {.first = 0x1071U, .last = 0x1074U},
    {.first = 0x1082U, .last = 0x1082U},   {.first = 0x1085U, .last = 0x1086U},   {.first = 0x108DU, .last = 0x108DU},
    {.first = 0x109DU, .last = 0x109DU},   {.first = 0x135DU, .last = 0x135FU},   {.first = 0x1712U, .last = 0x1714U},
    {.first = 0x1732U, .last = 0x1733U},   {.first = 0x1752U, .last = 0x1753U},   {.first = 0x1772U, .last = 0x1773U},
    {.first = 0x17B4U, .last = 0x17B5U},   {.first = 0x17B7U, .last = 0x17BDU},   {.first = 0x17C6U, .last = 0x17C6U},
    {.first = 0x17C9U, .last = 0x17D3U},   {.first = 0x17DDU, .last = 0x17DDU},   {.first = 0x180BU, .last = 0x180FU},
    {.first = 0x1885U, .last = 0x1886U},   {.first = 0x18A9U, .last = 0x18A9U},   {.first = 0x1920U, .last = 0x1922U},
    {.first = 0x1927U, .last = 0x1928U},   {.first = 0x1932U, .last = 0x1932U},   {.first = 0x1939U, .last = 0x193BU},
    {.first = 0x1A17U, .last = 0x1A18U},   {.first = 0x1A1BU, .last = 0x1A1BU},   {.first = 0x1A56U, .last = 0x1A56U},
    {.first = 0x1A58U, .last = 0x1A5EU},   {.first = 0x1A60U, .last = 0x1A60U},   {.first = 0x1A62U, .last = 0x1A62U},
    {.first = 0x1A65U, .last = 0x1A6CU},   {.first = 0x1A73U, .last = 0x1A7CU},   {.first = 0x1A7FU, .last = 0x1A7FU},
    {.first = 0x1AB0U, .last = 0x1AF0U},   {.first = 0x1B00U, .last = 0x1B03U},   {.first = 0x1B34U, .last = 0x1B34U},
    {.first = 0x1B36U, .last = 0x1B3AU},   {.first = 0x1B3CU, .last = 0x1B3CU},   {.first = 0x1B42U, .last = 0x1B42U},
    {.first = 0x1B6BU, .last = 0x1B73U},   {.first = 0x1B80U, .last = 0x1B81U},   {.first = 0x1BA2U, .last = 0x1BA5U},
    {.first = 0x1BA8U, .last = 0x1BA9U},   {.first = 0x1BABU, .last = 0x1BADU},   {.first = 0x1BE6U, .last = 0x1BE6U},
    {.first = 0x1BE8U, .last = 0x1BE9U},   {.first = 0x1BEDU, .last = 0x1BEDU},   {.first = 0x1BEFU, .last = 0x1BF1U},
    {.first = 0x1C2CU, .last = 0x1C33U},   {.first = 0x1C36U, .last = 0x1C37U},   {.first = 0x1CD0U, .last = 0x1CD2U},
    {.first = 0x1CD4U, .last = 0x1CE0U},   {.first = 0x1CE2U, .last = 0x1CE8U},   {.first = 0x1CEDU, .last = 0x1CEDU},
    {.first = 0x1CF4U, .last = 0x1CF4U},   {.first = 0x1CF8U, .last = 0x1CF9U},   {.first = 0x1DC0U, .last = 0x1DFFU},
    {.first = 0x200BU, .last = 0x200FU},   {.first = 0x202AU, .last = 0x202EU},   {.first = 0x2060U, .last = 0x2064U},
    {.first = 0x2066U, .last = 0x206FU},   {.first = 0x20D0U, .last = 0x20F0U},   {.first = 0x2CEFU, .last = 0x2CF1U},
    {.first = 0x2D7FU, .last = 0x2D7FU},   {.first = 0x2DE0U, .last = 0x2DFFU},   {.first = 0x302AU, .last = 0x302DU},
    {.first = 0x3099U, .last = 0x309AU},   {.first = 0xA66FU, .last = 0xA672U},   {.first = 0xA674U, .last = 0xA67DU},
    {.first = 0xA69EU, .last = 0xA69FU},   {.first = 0xA6F0U, .last = 0xA6F1U},   {.first = 0xA802U, .last = 0xA802U},
    {.first = 0xA806U, .last = 0xA806U},   {.first = 0xA80BU, .last = 0xA80BU},   {.first = 0xA825U, .last = 0xA826U},
    {.first = 0xA82CU, .last = 0xA82CU},   {.first = 0xA8C4U, .last = 0xA8C5U},   {.first = 0xA8E0U, .last = 0xA8F1U},
    {.first = 0xA8FFU, .last = 0xA8FFU},   {.first = 0xA926U, .last = 0xA92DU},   {.first = 0xA947U, .last = 0xA951U},
    {.first = 0xA980U, .last = 0xA982U},   {.first = 0xA9B3U, .last = 0xA9B3U},   {.first = 0xA9B6U, .last = 0xA9B9U},
    {.first = 0xA9BCU, .last = 0xA9BDU},   {.first = 0xA9E5U, .last = 0xA9E5U},   {.first = 0xAA29U, .last = 0xAA2EU},
    {.first = 0xAA31U, .last = 0xAA32U},   {.first = 0xAA35U, .last = 0xAA36U},   {.first = 0xAA43U, .last = 0xAA43U},
    {.first = 0xAA4CU, .last = 0xAA4CU},   {.first = 0xAA7CU, .last = 0xAA7CU},   {.first = 0xAAB0U, .last = 0xAAB0U},
    {.first = 0xAAB2U, .last = 0xAAB4U},   {.first = 0xAAB7U, .last = 0xAAB8U},   {.first = 0xAABEU, .last = 0xAABFU},
    {.first = 0xAAC1U, .last = 0xAAC1U},   {.first = 0xAAECU, .last = 0xAAEDU},   {.first = 0xAAF6U, .last = 0xAAF6U},
    {.first = 0xABE5U, .last = 0xABE5U},   {.first = 0xABE8U, .last = 0xABE8U},   {.first = 0xABEDU, .last = 0xABEDU},
    {.first = 0xFB1EU, .last = 0xFB1EU},   {.first = 0xFE00U, .last = 0xFE0FU},   {.first = 0xFE20U, .last = 0xFE2FU},
    {.first = 0xFEFFU, .last = 0xFEFFU},   {.first = 0xFFF9U, .last = 0xFFFBU},   {.first = 0x101FDU, .last = 0x101FDU},
    {.first = 0x102E0U, .last = 0x102E0U}, {.first = 0x10376U, .last = 0x1037AU}, {.first = 0x10A01U, .last = 0x10A03U},
    {.first = 0x10A05U, .last = 0x10A06U}, {.first = 0x10A0CU, .last = 0x10A0FU}, {.first = 0x10A38U, .last = 0x10A3AU},
    {.first = 0x10A3FU, .last = 0x10A3FU}, {.first = 0x10AE5U, .last = 0x10AE6U}, {.first = 0x10D24U, .last = 0x10D27U},
    {.first = 0x10D69U, .last = 0x10D6DU}, {.first = 0x10EABU, .last = 0x10EACU}, {.first = 0x10ECBU, .last = 0x10ECFU},
    {.first = 0x10EF0U, .last = 0x10EFFU}, {.first = 0x10F46U, .last = 0x10F50U}, {.first = 0x10F82U, .last = 0x10F85U},
    {.first = 0x11001U, .last = 0x11001U}, {.first = 0x11038U, .last = 0x11046U}, {.first = 0x11070U, .last = 0x11070U},
    {.first = 0x11073U, .last = 0x11074U}, {.first = 0x1107FU, .last = 0x11081U}, {.first = 0x110B3U, .last = 0x110B6U},
    {.first = 0x110B9U, .last = 0x110BAU}, {.first = 0x110BDU, .last = 0x110BDU}, {.first = 0x110C2U, .last = 0x110C2U},
    {.first = 0x110CDU, .last = 0x110CDU}, {.first = 0x11100U, .last = 0x11102U}, {.first = 0x11127U, .last = 0x1112BU},
    {.first = 0x1112DU, .last = 0x11134U}, {.first = 0x11173U, .last = 0x11173U}, {.first = 0x11180U, .last = 0x11181U},
    {.first = 0x111B6U, .last = 0x111BEU}, {.first = 0x111C9U, .last = 0x111CCU}, {.first = 0x111CFU, .last = 0x111CFU},
    {.first = 0x1122FU, .last = 0x11231U}, {.first = 0x11234U, .last = 0x11234U}, {.first = 0x11236U, .last = 0x11237U},
    {.first = 0x1123EU, .last = 0x1123EU}, {.first = 0x11241U, .last = 0x11241U}, {.first = 0x112DFU, .last = 0x112DFU},
    {.first = 0x112E3U, .last = 0x112EAU}, {.first = 0x11300U, .last = 0x11301U}, {.first = 0x1133BU, .last = 0x1133CU},
    {.first = 0x11340U, .last = 0x11340U}, {.first = 0x11366U, .last = 0x1136CU}, {.first = 0x11370U, .last = 0x11374U},
    {.first = 0x113BBU, .last = 0x113C0U}, {.first = 0x113CEU, .last = 0x113CEU}, {.first = 0x113D0U, .last = 0x113D0U},
    {.first = 0x113D2U, .last = 0x113D2U}, {.first = 0x113E1U, .last = 0x113E2U}, {.first = 0x11438U, .last = 0x1143FU},
    {.first = 0x11442U, .last = 0x11444U}, {.first = 0x11446U, .last = 0x11446U}, {.first = 0x1145EU, .last = 0x1145EU},
    {.first = 0x114B3U, .last = 0x114B8U}, {.first = 0x114BAU, .last = 0x114BAU}, {.first = 0x114BFU, .last = 0x114C0U},
    {.first = 0x114C2U, .last = 0x114C3U}, {.first = 0x115B2U, .last = 0x115B5U}, {.first = 0x115BCU, .last = 0x115BDU},
    {.first = 0x115BFU, .last = 0x115C0U}, {.first = 0x115DCU, .last = 0x115DDU}, {.first = 0x11633U, .last = 0x1163AU},
    {.first = 0x1163DU, .last = 0x1163DU}, {.first = 0x1163FU, .last = 0x11640U}, {.first = 0x116ABU, .last = 0x116ABU},
    {.first = 0x116ADU, .last = 0x116ADU}, {.first = 0x116B0U, .last = 0x116B5U}, {.first = 0x116B7U, .last = 0x116B7U},
    {.first = 0x1171DU, .last = 0x1171DU}, {.first = 0x1171FU, .last = 0x1171FU}, {.first = 0x11722U, .last = 0x11725U},
    {.first = 0x11727U, .last = 0x1172BU}, {.first = 0x1182FU, .last = 0x11837U}, {.first = 0x11839U, .last = 0x1183AU},
    {.first = 0x1193BU, .last = 0x1193CU}, {.first = 0x1193EU, .last = 0x1193EU}, {.first = 0x11943U, .last = 0x11943U},
    {.first = 0x119D4U, .last = 0x119D7U}, {.first = 0x119DAU, .last = 0x119DBU}, {.first = 0x119E0U, .last = 0x119E0U},
    {.first = 0x11A01U, .last = 0x11A0AU}, {.first = 0x11A33U, .last = 0x11A38U}, {.first = 0x11A3BU, .last = 0x11A3EU},
    {.first = 0x11A47U, .last = 0x11A47U}, {.first = 0x11A51U, .last = 0x11A56U}, {.first = 0x11A59U, .last = 0x11A5BU},
    {.first = 0x11A8AU, .last = 0x11A96U}, {.first = 0x11A98U, .last = 0x11A99U}, {.first = 0x11B60U, .last = 0x11B60U},
    {.first = 0x11B62U, .last = 0x11B64U}, {.first = 0x11B66U, .last = 0x11B66U}, {.first = 0x11C30U, .last = 0x11C36U},
    {.first = 0x11C38U, .last = 0x11C3DU}, {.first = 0x11C3FU, .last = 0x11C3FU}, {.first = 0x11C92U, .last = 0x11CA7U},
    {.first = 0x11CAAU, .last = 0x11CB0U}, {.first = 0x11CB2U, .last = 0x11CB3U}, {.first = 0x11CB5U, .last = 0x11CB6U},
    {.first = 0x11D31U, .last = 0x11D36U}, {.first = 0x11D3AU, .last = 0x11D3AU}, {.first = 0x11D3CU, .last = 0x11D3DU},
    {.first = 0x11D3FU, .last = 0x11D45U}, {.first = 0x11D47U, .last = 0x11D47U}, {.first = 0x11D90U, .last = 0x11D91U},
    {.first = 0x11D95U, .last = 0x11D95U}, {.first = 0x11D97U, .last = 0x11D97U}, {.first = 0x11DF0U, .last = 0x11DF0U},
    {.first = 0x11EF3U, .last = 0x11EF4U}, {.first = 0x11F00U, .last = 0x11F01U}, {.first = 0x11F36U, .last = 0x11F3AU},
    {.first = 0x11F40U, .last = 0x11F40U}, {.first = 0x11F42U, .last = 0x11F42U}, {.first = 0x11F5AU, .last = 0x11F5AU},
    {.first = 0x13430U, .last = 0x13440U}, {.first = 0x13447U, .last = 0x13455U}, {.first = 0x1611EU, .last = 0x16129U},
    {.first = 0x1612DU, .last = 0x1612FU}, {.first = 0x16AF0U, .last = 0x16AF4U}, {.first = 0x16B30U, .last = 0x16B36U},
    {.first = 0x16F4FU, .last = 0x16F4FU}, {.first = 0x16F8FU, .last = 0x16F92U}, {.first = 0x16FE4U, .last = 0x16FE4U},
    {.first = 0x1BC9DU, .last = 0x1BC9EU}, {.first = 0x1BCA0U, .last = 0x1BCA3U}, {.first = 0x1CF00U, .last = 0x1CF2DU},
    {.first = 0x1CF30U, .last = 0x1CF46U}, {.first = 0x1D127U, .last = 0x1D128U}, {.first = 0x1D167U, .last = 0x1D169U},
    {.first = 0x1D173U, .last = 0x1D182U}, {.first = 0x1D185U, .last = 0x1D18BU}, {.first = 0x1D1AAU, .last = 0x1D1ADU},
    {.first = 0x1D242U, .last = 0x1D244U}, {.first = 0x1D25BU, .last = 0x1D25CU}, {.first = 0x1DA00U, .last = 0x1DA36U},
    {.first = 0x1DA3BU, .last = 0x1DA6CU}, {.first = 0x1DA75U, .last = 0x1DA75U}, {.first = 0x1DA84U, .last = 0x1DA84U},
    {.first = 0x1DA9BU, .last = 0x1DA9FU}, {.first = 0x1DAA1U, .last = 0x1DAAFU}, {.first = 0x1E000U, .last = 0x1E006U},
    {.first = 0x1E008U, .last = 0x1E018U}, {.first = 0x1E01BU, .last = 0x1E021U}, {.first = 0x1E023U, .last = 0x1E024U},
    {.first = 0x1E026U, .last = 0x1E02AU}, {.first = 0x1E08FU, .last = 0x1E08FU}, {.first = 0x1E130U, .last = 0x1E136U},
    {.first = 0x1E2AEU, .last = 0x1E2AEU}, {.first = 0x1E2ECU, .last = 0x1E2EFU}, {.first = 0x1E4ECU, .last = 0x1E4EFU},
    {.first = 0x1E5EEU, .last = 0x1E5EFU}, {.first = 0x1E6E3U, .last = 0x1E6E3U}, {.first = 0x1E6E6U, .last = 0x1E6E6U},
    {.first = 0x1E6EEU, .last = 0x1E6EFU}, {.first = 0x1E6F5U, .last = 0x1E6F5U}, {.first = 0x1E8D0U, .last = 0x1E8D6U},
    {.first = 0x1E944U, .last = 0x1E94AU}, {.first = 0xE0001U, .last = 0xE0001U}, {.first = 0xE0020U, .last = 0xE007FU},
    {.first = 0xE0100U, .last = 0xE01EFU},
}};

// East Asian Width = W 与 F：恒占 2 格。
constexpr std::array<CodePointRange, 126> AURORA_WIDE_RANGES{{
    {.first = 0x1100U, .last = 0x115FU},   {.first = 0x231AU, .last = 0x231BU},   {.first = 0x2329U, .last = 0x232AU},
    {.first = 0x23E9U, .last = 0x23ECU},   {.first = 0x23F0U, .last = 0x23F0U},   {.first = 0x23F3U, .last = 0x23F3U},
    {.first = 0x25FDU, .last = 0x25FEU},   {.first = 0x2614U, .last = 0x2615U},   {.first = 0x2630U, .last = 0x2637U},
    {.first = 0x2648U, .last = 0x2653U},   {.first = 0x267FU, .last = 0x267FU},   {.first = 0x268AU, .last = 0x268FU},
    {.first = 0x2693U, .last = 0x2693U},   {.first = 0x26A1U, .last = 0x26A1U},   {.first = 0x26AAU, .last = 0x26ABU},
    {.first = 0x26BDU, .last = 0x26BEU},   {.first = 0x26C4U, .last = 0x26C5U},   {.first = 0x26CEU, .last = 0x26CEU},
    {.first = 0x26D4U, .last = 0x26D4U},   {.first = 0x26EAU, .last = 0x26EAU},   {.first = 0x26F2U, .last = 0x26F3U},
    {.first = 0x26F5U, .last = 0x26F5U},   {.first = 0x26FAU, .last = 0x26FAU},   {.first = 0x26FDU, .last = 0x26FDU},
    {.first = 0x2705U, .last = 0x2705U},   {.first = 0x270AU, .last = 0x270BU},   {.first = 0x2728U, .last = 0x2728U},
    {.first = 0x274CU, .last = 0x274CU},   {.first = 0x274EU, .last = 0x274EU},   {.first = 0x2753U, .last = 0x2755U},
    {.first = 0x2757U, .last = 0x2757U},   {.first = 0x2795U, .last = 0x2797U},   {.first = 0x27B0U, .last = 0x27B0U},
    {.first = 0x27BFU, .last = 0x27BFU},   {.first = 0x2B1BU, .last = 0x2B1CU},   {.first = 0x2B50U, .last = 0x2B50U},
    {.first = 0x2B55U, .last = 0x2B55U},   {.first = 0x2E80U, .last = 0x2E99U},   {.first = 0x2E9BU, .last = 0x2EF3U},
    {.first = 0x2F00U, .last = 0x2FD5U},   {.first = 0x2FF0U, .last = 0x303EU},   {.first = 0x3041U, .last = 0x3096U},
    {.first = 0x3099U, .last = 0x30FFU},   {.first = 0x3105U, .last = 0x312FU},   {.first = 0x3131U, .last = 0x318EU},
    {.first = 0x3190U, .last = 0x31E5U},   {.first = 0x31EFU, .last = 0x321EU},   {.first = 0x3220U, .last = 0x3247U},
    {.first = 0x3250U, .last = 0xA48CU},   {.first = 0xA490U, .last = 0xA4C6U},   {.first = 0xA960U, .last = 0xA97CU},
    {.first = 0xAC00U, .last = 0xD7A3U},   {.first = 0xF900U, .last = 0xFAFFU},   {.first = 0xFE10U, .last = 0xFE19U},
    {.first = 0xFE30U, .last = 0xFE52U},   {.first = 0xFE54U, .last = 0xFE66U},   {.first = 0xFE68U, .last = 0xFE6BU},
    {.first = 0xFF01U, .last = 0xFF60U},   {.first = 0xFFE0U, .last = 0xFFE6U},   {.first = 0x16FE0U, .last = 0x16FE4U},
    {.first = 0x16FF0U, .last = 0x16FF6U}, {.first = 0x17000U, .last = 0x18CDAU}, {.first = 0x18CFFU, .last = 0x18D20U},
    {.first = 0x18D80U, .last = 0x18DF2U}, {.first = 0x18E00U, .last = 0x19191U}, {.first = 0x191A0U, .last = 0x191D2U},
    {.first = 0x1AFF0U, .last = 0x1AFF3U}, {.first = 0x1AFF5U, .last = 0x1AFFBU}, {.first = 0x1AFFDU, .last = 0x1AFFEU},
    {.first = 0x1B000U, .last = 0x1B128U}, {.first = 0x1B132U, .last = 0x1B132U}, {.first = 0x1B150U, .last = 0x1B152U},
    {.first = 0x1B155U, .last = 0x1B155U}, {.first = 0x1B164U, .last = 0x1B168U}, {.first = 0x1B170U, .last = 0x1B2FBU},
    {.first = 0x1D300U, .last = 0x1D356U}, {.first = 0x1D360U, .last = 0x1D376U}, {.first = 0x1F004U, .last = 0x1F004U},
    {.first = 0x1F0CFU, .last = 0x1F0CFU}, {.first = 0x1F18EU, .last = 0x1F18EU}, {.first = 0x1F191U, .last = 0x1F19AU},
    {.first = 0x1F1AEU, .last = 0x1F1AEU}, {.first = 0x1F200U, .last = 0x1F202U}, {.first = 0x1F210U, .last = 0x1F23BU},
    {.first = 0x1F240U, .last = 0x1F248U}, {.first = 0x1F250U, .last = 0x1F251U}, {.first = 0x1F260U, .last = 0x1F265U},
    {.first = 0x1F300U, .last = 0x1F320U}, {.first = 0x1F32DU, .last = 0x1F335U}, {.first = 0x1F337U, .last = 0x1F37CU},
    {.first = 0x1F37EU, .last = 0x1F393U}, {.first = 0x1F3A0U, .last = 0x1F3CAU}, {.first = 0x1F3CFU, .last = 0x1F3D3U},
    {.first = 0x1F3E0U, .last = 0x1F3F0U}, {.first = 0x1F3F4U, .last = 0x1F3F4U}, {.first = 0x1F3F8U, .last = 0x1F43EU},
    {.first = 0x1F440U, .last = 0x1F440U}, {.first = 0x1F442U, .last = 0x1F4FCU}, {.first = 0x1F4FFU, .last = 0x1F53DU},
    {.first = 0x1F54BU, .last = 0x1F54EU}, {.first = 0x1F550U, .last = 0x1F567U}, {.first = 0x1F57AU, .last = 0x1F57AU},
    {.first = 0x1F595U, .last = 0x1F596U}, {.first = 0x1F5A4U, .last = 0x1F5A4U}, {.first = 0x1F5FBU, .last = 0x1F64FU},
    {.first = 0x1F680U, .last = 0x1F6C5U}, {.first = 0x1F6CCU, .last = 0x1F6CCU}, {.first = 0x1F6D0U, .last = 0x1F6D2U},
    {.first = 0x1F6D5U, .last = 0x1F6D9U}, {.first = 0x1F6DCU, .last = 0x1F6DFU}, {.first = 0x1F6EBU, .last = 0x1F6ECU},
    {.first = 0x1F6F4U, .last = 0x1F6FCU}, {.first = 0x1F7DAU, .last = 0x1F7DAU}, {.first = 0x1F7E0U, .last = 0x1F7EBU},
    {.first = 0x1F7F0U, .last = 0x1F7F0U}, {.first = 0x1F90CU, .last = 0x1F93AU}, {.first = 0x1F93CU, .last = 0x1F945U},
    {.first = 0x1F947U, .last = 0x1F9FFU}, {.first = 0x1FA70U, .last = 0x1FA7CU}, {.first = 0x1FA80U, .last = 0x1FAC6U},
    {.first = 0x1FAC8U, .last = 0x1FAC8U}, {.first = 0x1FACCU, .last = 0x1FADDU}, {.first = 0x1FADFU, .last = 0x1FAEBU},
    {.first = 0x1FAEFU, .last = 0x1FAFAU}, {.first = 0x20000U, .last = 0x2FFFDU}, {.first = 0x30000U, .last = 0x3FFFDU},
}};

// East Asian Width = A：占几格由 AmbiguousWidthMode 决定。
constexpr std::array<CodePointRange, 179> AURORA_AMBIGUOUS_RANGES{{
    {.first = 0x00A1U, .last = 0x00A1U},     {.first = 0x00A4U, .last = 0x00A4U},
    {.first = 0x00A7U, .last = 0x00A8U},     {.first = 0x00AAU, .last = 0x00AAU},
    {.first = 0x00ADU, .last = 0x00AEU},     {.first = 0x00B0U, .last = 0x00B4U},
    {.first = 0x00B6U, .last = 0x00BAU},     {.first = 0x00BCU, .last = 0x00BFU},
    {.first = 0x00C6U, .last = 0x00C6U},     {.first = 0x00D0U, .last = 0x00D0U},
    {.first = 0x00D7U, .last = 0x00D8U},     {.first = 0x00DEU, .last = 0x00E1U},
    {.first = 0x00E6U, .last = 0x00E6U},     {.first = 0x00E8U, .last = 0x00EAU},
    {.first = 0x00ECU, .last = 0x00EDU},     {.first = 0x00F0U, .last = 0x00F0U},
    {.first = 0x00F2U, .last = 0x00F3U},     {.first = 0x00F7U, .last = 0x00FAU},
    {.first = 0x00FCU, .last = 0x00FCU},     {.first = 0x00FEU, .last = 0x00FEU},
    {.first = 0x0101U, .last = 0x0101U},     {.first = 0x0111U, .last = 0x0111U},
    {.first = 0x0113U, .last = 0x0113U},     {.first = 0x011BU, .last = 0x011BU},
    {.first = 0x0126U, .last = 0x0127U},     {.first = 0x012BU, .last = 0x012BU},
    {.first = 0x0131U, .last = 0x0133U},     {.first = 0x0138U, .last = 0x0138U},
    {.first = 0x013FU, .last = 0x0142U},     {.first = 0x0144U, .last = 0x0144U},
    {.first = 0x0148U, .last = 0x014BU},     {.first = 0x014DU, .last = 0x014DU},
    {.first = 0x0152U, .last = 0x0153U},     {.first = 0x0166U, .last = 0x0167U},
    {.first = 0x016BU, .last = 0x016BU},     {.first = 0x01CEU, .last = 0x01CEU},
    {.first = 0x01D0U, .last = 0x01D0U},     {.first = 0x01D2U, .last = 0x01D2U},
    {.first = 0x01D4U, .last = 0x01D4U},     {.first = 0x01D6U, .last = 0x01D6U},
    {.first = 0x01D8U, .last = 0x01D8U},     {.first = 0x01DAU, .last = 0x01DAU},
    {.first = 0x01DCU, .last = 0x01DCU},     {.first = 0x0251U, .last = 0x0251U},
    {.first = 0x0261U, .last = 0x0261U},     {.first = 0x02C4U, .last = 0x02C4U},
    {.first = 0x02C7U, .last = 0x02C7U},     {.first = 0x02C9U, .last = 0x02CBU},
    {.first = 0x02CDU, .last = 0x02CDU},     {.first = 0x02D0U, .last = 0x02D0U},
    {.first = 0x02D8U, .last = 0x02DBU},     {.first = 0x02DDU, .last = 0x02DDU},
    {.first = 0x02DFU, .last = 0x02DFU},     {.first = 0x0300U, .last = 0x036FU},
    {.first = 0x0391U, .last = 0x03A1U},     {.first = 0x03A3U, .last = 0x03A9U},
    {.first = 0x03B1U, .last = 0x03C1U},     {.first = 0x03C3U, .last = 0x03C9U},
    {.first = 0x0401U, .last = 0x0401U},     {.first = 0x0410U, .last = 0x044FU},
    {.first = 0x0451U, .last = 0x0451U},     {.first = 0x2010U, .last = 0x2010U},
    {.first = 0x2013U, .last = 0x2016U},     {.first = 0x2018U, .last = 0x2019U},
    {.first = 0x201CU, .last = 0x201DU},     {.first = 0x2020U, .last = 0x2022U},
    {.first = 0x2024U, .last = 0x2027U},     {.first = 0x2030U, .last = 0x2030U},
    {.first = 0x2032U, .last = 0x2033U},     {.first = 0x2035U, .last = 0x2035U},
    {.first = 0x203BU, .last = 0x203BU},     {.first = 0x203EU, .last = 0x203EU},
    {.first = 0x2074U, .last = 0x2074U},     {.first = 0x207FU, .last = 0x207FU},
    {.first = 0x2081U, .last = 0x2084U},     {.first = 0x20ACU, .last = 0x20ACU},
    {.first = 0x2103U, .last = 0x2103U},     {.first = 0x2105U, .last = 0x2105U},
    {.first = 0x2109U, .last = 0x2109U},     {.first = 0x2113U, .last = 0x2113U},
    {.first = 0x2116U, .last = 0x2116U},     {.first = 0x2121U, .last = 0x2122U},
    {.first = 0x2126U, .last = 0x2126U},     {.first = 0x212BU, .last = 0x212BU},
    {.first = 0x2153U, .last = 0x2154U},     {.first = 0x215BU, .last = 0x215EU},
    {.first = 0x2160U, .last = 0x216BU},     {.first = 0x2170U, .last = 0x2179U},
    {.first = 0x2189U, .last = 0x2189U},     {.first = 0x2190U, .last = 0x2199U},
    {.first = 0x21B8U, .last = 0x21B9U},     {.first = 0x21D2U, .last = 0x21D2U},
    {.first = 0x21D4U, .last = 0x21D4U},     {.first = 0x21E7U, .last = 0x21E7U},
    {.first = 0x2200U, .last = 0x2200U},     {.first = 0x2202U, .last = 0x2203U},
    {.first = 0x2207U, .last = 0x2208U},     {.first = 0x220BU, .last = 0x220BU},
    {.first = 0x220FU, .last = 0x220FU},     {.first = 0x2211U, .last = 0x2211U},
    {.first = 0x2215U, .last = 0x2215U},     {.first = 0x221AU, .last = 0x221AU},
    {.first = 0x221DU, .last = 0x2220U},     {.first = 0x2223U, .last = 0x2223U},
    {.first = 0x2225U, .last = 0x2225U},     {.first = 0x2227U, .last = 0x222CU},
    {.first = 0x222EU, .last = 0x222EU},     {.first = 0x2234U, .last = 0x2237U},
    {.first = 0x223CU, .last = 0x223DU},     {.first = 0x2248U, .last = 0x2248U},
    {.first = 0x224CU, .last = 0x224CU},     {.first = 0x2252U, .last = 0x2252U},
    {.first = 0x2260U, .last = 0x2261U},     {.first = 0x2264U, .last = 0x2267U},
    {.first = 0x226AU, .last = 0x226BU},     {.first = 0x226EU, .last = 0x226FU},
    {.first = 0x2282U, .last = 0x2283U},     {.first = 0x2286U, .last = 0x2287U},
    {.first = 0x2295U, .last = 0x2295U},     {.first = 0x2299U, .last = 0x2299U},
    {.first = 0x22A5U, .last = 0x22A5U},     {.first = 0x22BFU, .last = 0x22BFU},
    {.first = 0x2312U, .last = 0x2312U},     {.first = 0x2460U, .last = 0x24E9U},
    {.first = 0x24EBU, .last = 0x254BU},     {.first = 0x2550U, .last = 0x2573U},
    {.first = 0x2580U, .last = 0x258FU},     {.first = 0x2592U, .last = 0x2595U},
    {.first = 0x25A0U, .last = 0x25A1U},     {.first = 0x25A3U, .last = 0x25A9U},
    {.first = 0x25B2U, .last = 0x25B3U},     {.first = 0x25B6U, .last = 0x25B7U},
    {.first = 0x25BCU, .last = 0x25BDU},     {.first = 0x25C0U, .last = 0x25C1U},
    {.first = 0x25C6U, .last = 0x25C8U},     {.first = 0x25CBU, .last = 0x25CBU},
    {.first = 0x25CEU, .last = 0x25D1U},     {.first = 0x25E2U, .last = 0x25E5U},
    {.first = 0x25EFU, .last = 0x25EFU},     {.first = 0x2605U, .last = 0x2606U},
    {.first = 0x2609U, .last = 0x2609U},     {.first = 0x260EU, .last = 0x260FU},
    {.first = 0x261CU, .last = 0x261CU},     {.first = 0x261EU, .last = 0x261EU},
    {.first = 0x2640U, .last = 0x2640U},     {.first = 0x2642U, .last = 0x2642U},
    {.first = 0x2660U, .last = 0x2661U},     {.first = 0x2663U, .last = 0x2665U},
    {.first = 0x2667U, .last = 0x266AU},     {.first = 0x266CU, .last = 0x266DU},
    {.first = 0x266FU, .last = 0x266FU},     {.first = 0x269EU, .last = 0x269FU},
    {.first = 0x26BFU, .last = 0x26BFU},     {.first = 0x26C6U, .last = 0x26CDU},
    {.first = 0x26CFU, .last = 0x26D3U},     {.first = 0x26D5U, .last = 0x26E1U},
    {.first = 0x26E3U, .last = 0x26E3U},     {.first = 0x26E8U, .last = 0x26E9U},
    {.first = 0x26EBU, .last = 0x26F1U},     {.first = 0x26F4U, .last = 0x26F4U},
    {.first = 0x26F6U, .last = 0x26F9U},     {.first = 0x26FBU, .last = 0x26FCU},
    {.first = 0x26FEU, .last = 0x26FFU},     {.first = 0x273DU, .last = 0x273DU},
    {.first = 0x2776U, .last = 0x277FU},     {.first = 0x2B56U, .last = 0x2B59U},
    {.first = 0x3248U, .last = 0x324FU},     {.first = 0xE000U, .last = 0xF8FFU},
    {.first = 0xFE00U, .last = 0xFE0FU},     {.first = 0xFFFDU, .last = 0xFFFDU},
    {.first = 0x1F100U, .last = 0x1F10AU},   {.first = 0x1F110U, .last = 0x1F12DU},
    {.first = 0x1F130U, .last = 0x1F169U},   {.first = 0x1F170U, .last = 0x1F18DU},
    {.first = 0x1F18FU, .last = 0x1F190U},   {.first = 0x1F19BU, .last = 0x1F1ACU},
    {.first = 0xE0100U, .last = 0xE01EFU},   {.first = 0xF0000U, .last = 0xFFFFDU},
    {.first = 0x100000U, .last = 0x10FFFDU},
}};

// 表按 first 升序且互不相交，故取「first 严格大于码点的第一个区间」的前一段即可判定。
constexpr auto in_ranges(std::span<const CodePointRange> table, char32_t code_point) noexcept -> bool {
    const auto next = std::ranges::upper_bound(table, code_point, {}, &CodePointRange::first);
    return next != table.begin() && std::prev(next)->last >= code_point;
}

// WHY 可以是单点比较：判定的三次查表各自独立，而每张表都按 first 升序且互不相交（本文件头部的不变量），
// 故小于某张表首项的码点必然落在该表之外。取三表首项的最小值即得一个同时躲开三次二分的下界。
// 这是可证明等价而非近似——落在界内的码点按既有次序的结果恒为 1，与 ambiguous_mode 无关。
// 界由表导出而非写死字面量：数据表再生成后若首项下移，本常量随之收紧，无需人工同步阈值。
constexpr std::uint32_t AURORA_UNICODE_SINGLE_WIDTH_BELOW = std::min(
    {AURORA_ZERO_WIDTH_RANGES.front().first, AURORA_WIDE_RANGES.front().first, AURORA_AMBIGUOUS_RANGES.front().first});

}  // namespace

auto unicode_cell_width(char32_t code_point, AmbiguousWidthMode ambiguous_mode) noexcept -> std::uint8_t {
    if (static_cast<std::uint32_t>(code_point) < AURORA_UNICODE_SINGLE_WIDTH_BELOW) {
        return 1U;
    }
    if (in_ranges(AURORA_ZERO_WIDTH_RANGES, code_point)) {
        return 0U;
    }
    if (in_ranges(AURORA_WIDE_RANGES, code_point)) {
        return 2U;
    }
    if (in_ranges(AURORA_AMBIGUOUS_RANGES, code_point)) {
        return ambiguous_mode == AmbiguousWidthMode::Wide ? 2U : 1U;
    }
    return 1U;
}

}  // namespace aurora
