// ============================================================================
// theming/color_roles_high_contrast.cpp - 高对比派生：按判据登记表整族调 tone
// ----------------------------------------------------------------------------
// 本文件是 `ColorRoles::with_high_contrast()` 的实现体，也是全库唯一回答「无障碍开关置位后，
// 这套配色该变成什么」的地方。
//
// 三条纪律：
//   1. **判据表是唯一规则来源**。配对关系、档位下限、可动性全部读
//      `detail::AURORA_CONTRAST_AUDITS` / `AURORA_CONTRAST_TIERS`——与 `check_theme_contrast` 门禁、
//      `utest_theme_roles` 同一张表。表里加一行，这里就自动多判一行，不存在第二本规则书。
//   2. **判定算术只用公共件**：比值经 `aurora::contrast_ratio`、亮度经 `aurora::relative_luminance`
//      求得，本文件不复写公式。仅通道级 sRGB <-> 线性两枚换算就地私有——公共件只给整色亮度，
//      而这里要按通道求逆。
//   3. **派生不重新配色**：沿线性 RGB 射线把亮度拉到解出的目标值，彩度比例原样保留。向白 / 向黑
//      交替混合会把颜色洗成灰（实测否决过该形态），故只在射线越出色域壳时才向白点补最后一段。
//
// 求解形态（两轮，每轮逐行扫描至多 AURORA_HIGH_CONTRAST_PASSES 遍，本轮无改动即收敛）：
//   * 严格轮——只接受「修好本行且不弄坏任何其他已达标行」的候选；
//   * 放宽轮——对确实无解的冲突行，取「弄坏文本档最少」的候选。冲突是真的：Aqua 暗档官方配对里
//     白字压强调蓝只有 4.07:1，而同一支蓝作为非文本元素还必须压住页面底色的 3.0:1，两端不可能同时
//     满足，于是让前景标签下沉，而不是把强调色压暗。
//   候选端次序恒为「先底色、后前景」，故能不动标签就不动标签；纯底色（只作背景出现的 `surface` 族
//   等）恒不动——页面地在高对比语义下不得移位。
//
// 目标值就地留余量（AURORA_HIGH_CONTRAST_HEADROOM），因为角色按 8 位量化存储：恰好落在 4.500 上的
// 解经四舍五入后会掉到线下。
// ============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/theming/color_roles.h"
#include "aurora/theming/detail/contrast_audit.h"

namespace aurora {
namespace {

using aurora::detail::AURORA_CONTRAST_AUDITS;
using aurora::detail::AURORA_CONTRAST_TIERS;
using aurora::detail::ContrastPairAudit;
using aurora::detail::ContrastTier;

using RoleMember = Color ColorRoles::*;

/// @brief 目标比值相对档位下限的余量：8 位量化后仍压得住线（4.5 × 1.03 = 4.635）。
constexpr double AURORA_HIGH_CONTRAST_HEADROOM = 1.03;
/// @brief 单轮扫描的最大遍数：每修一行都可能让另一行重新掉线，收敛即停。
constexpr int AURORA_HIGH_CONTRAST_PASSES = 8;
/// @brief 不适用档的下限值：等于它即本行不参与派生。
constexpr double AURORA_HIGH_CONTRAST_INAPPLICABLE = 0.0;
/// @brief 色域壳外补最后一段时的二分轮数：2^-40 远小于 8 位量化步长。
constexpr int AURORA_HIGH_CONTRAST_BISECTIONS = 40;
/// @brief 目标亮度与现值之差的下限：小于它即本行已由量化解决，不该再动颜色。
constexpr double AURORA_HIGH_CONTRAST_EPSILON = 1e-4;
/// @brief 近黑亮度阈值：低于它就没有可保留的彩度方向（纯黑只能整体提亮）。
constexpr double AURORA_HIGH_CONTRAST_NEUTRAL_FLOOR = 1e-9;
/// @brief 线性通道成数：与 `relative_luminance` 同一组 WCAG 权重。
constexpr double AURORA_LUMA_RED = 0.2126;
constexpr double AURORA_LUMA_GREEN = 0.7152;
constexpr double AURORA_LUMA_BLUE = 0.0722;
/// @brief sRGB 分段点与斜率（IEC 61966-2-1 的固定常数）。
constexpr double AURORA_SRGB_OFFSET = 0.055;
constexpr double AURORA_SRGB_SLOPE = 1.055;
constexpr double AURORA_SRGB_POWER = 2.4;
constexpr double AURORA_SRGB_TOE_MAX = 0.04045;
constexpr double AURORA_SRGB_TOE_SLOPE = 12.92;
constexpr double AURORA_SRGB_BREAK_LOW = 0.0031308;
constexpr double AURORA_CHANNEL_MAX = 255.0;
constexpr double AURORA_LUMA_BASE = 0.05;

/// @brief 一条待判配对在求解时的最小视图（成员指针 + 该档下限 + 档位归属）。
struct AuditRow {
    RoleMember fg = &ColorRoles::primary;  ///< 前景角色。
    RoleMember bg = &ColorRoles::primary;  ///< 底色角色。
    double min_ratio = 0.0;  ///< 档位下限（未乘余量）。
    bool text = false;  ///< 是否文本档（放宽轮的取舍权重）。
};

/// @brief 一个候选解：颜色值 + 它的排序键（破坏面、端别、改动幅度）。
struct Candidate {
    bool valid = false;  ///< 是否构成可用候选。
    int broken_text = 0;  ///< 被弄坏的文本档行数。
    int broken_total = 0;  ///< 被弄坏的总行数。
    bool background_end = false;  ///< 动的是底色端（同分时偏爱它，标签因此保持不动）。
    double shift = 0.0;  ///< 亮度改动幅度。
    RoleMember member = &ColorRoles::primary;  ///< 待写的角色。
    Color value;  ///< 该候选给出的颜色。
};

/// @brief 排序键：破坏文本档最少 → 破坏总数最少 → 偏爱动底色 → 改动幅度最小。
/// @param cand 候选。
/// @return 可比较的元组（字典序越小越好）。
[[nodiscard]] constexpr auto candidate_rank(const Candidate &cand) noexcept -> std::tuple<int, int, int, double> {
    return {cand.broken_text, cand.broken_total, cand.background_end ? 0 : 1, cand.shift};
}

/// @brief 单通道 sRGB -> 线性。
/// @param v 8 位通道值。
/// @return [0, 1] 的线性分量。
[[nodiscard]] auto channel_to_linear(const std::uint8_t v) noexcept -> double {
    const double s = static_cast<double>(v) / AURORA_CHANNEL_MAX;
    return s <= AURORA_SRGB_TOE_MAX ? s / AURORA_SRGB_TOE_SLOPE
                                    : std::pow((s + AURORA_SRGB_OFFSET) / AURORA_SRGB_SLOPE, AURORA_SRGB_POWER);
}

/// @brief 单通道线性 -> sRGB（先夹回 [0,1] 再编码）。
/// @param x 线性分量。
/// @return 四舍五入后的 8 位通道值。
[[nodiscard]] auto channel_to_srgb(const double x) noexcept -> std::uint8_t {
    const double clamped = std::clamp(x, 0.0, 1.0);
    const double s = clamped <= AURORA_SRGB_BREAK_LOW
                         ? clamped * AURORA_SRGB_TOE_SLOPE
                         : (AURORA_SRGB_SLOPE * std::pow(clamped, 1.0 / AURORA_SRGB_POWER)) - AURORA_SRGB_OFFSET;
    return static_cast<std::uint8_t>(std::lround(s * AURORA_CHANNEL_MAX));
}

/// @brief 线性三元组的加权和（与 `relative_luminance` 同权重，供求逆过程内联使用）。
/// @param rgb 线性 R/G/B。
/// @return [0, 1] 的相对亮度。
[[nodiscard]] auto linear_luminance(const std::array<double, 3> &rgb) noexcept -> double {
    return (AURORA_LUMA_RED * rgb[0]) + (AURORA_LUMA_GREEN * rgb[1]) + (AURORA_LUMA_BLUE * rgb[2]);
}

/// @brief 线性三元组编码成色（alpha 由调用方原样带回）。
/// @param rgb 线性 R/G/B。
/// @param alpha 不透明度。
/// @return 编码后的颜色。
[[nodiscard]] auto encode_linear(const std::array<double, 3> &rgb, const std::uint8_t alpha) noexcept -> Color {
    return Color{channel_to_srgb(rgb[0]), channel_to_srgb(rgb[1]), channel_to_srgb(rgb[2]), alpha};
}

/// @brief 沿线性 RGB 射线把亮度拉到 target。
///
/// 射线整体在色域内时等比缩放即可；越界时先把方向贴到色域壳（最大通道归一），壳上亮度已够就到此
/// 为止，还差最后一段才向白点二分补足——这是唯一允许降彩度的路径。
///
/// @param color 原色。
/// @param target 目标相对亮度，须落在 [0, 1]。
/// @return 同彩度比例、亮度为 target 的颜色；近黑色无方向可依，退化为该亮度的中性灰。
[[nodiscard]] auto set_luminance(const Color &color, const double target) noexcept -> Color {
    std::array<double, 3> linear = {channel_to_linear(color.r), channel_to_linear(color.g), channel_to_linear(color.b)};
    const double current = linear_luminance(linear);
    if (current <= AURORA_HIGH_CONTRAST_NEUTRAL_FLOOR) {
        const double flat = std::clamp(target, 0.0, 1.0) / 3.0;
        return encode_linear({flat, flat, flat}, color.a);
    }
    const double scale = target / current;
    for (double &v : linear) {
        v *= scale;
    }
    const double peak = *std::max_element(linear.begin(), linear.end());
    if (peak <= 1.0) {
        return encode_linear(linear, color.a);
    }
    for (double &v : linear) {
        v /= peak;
    }
    if (linear_luminance(linear) >= target) {
        return encode_linear(linear, color.a);
    }
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < AURORA_HIGH_CONTRAST_BISECTIONS; ++i) {
        const double mid = (lo + hi) / 2.0;
        std::array<double, 3> mixed{};
        for (std::size_t c = 0; c < mixed.size(); ++c) {
            mixed.at(c) = linear.at(c) + ((1.0 - linear.at(c)) * mid);
        }
        if (linear_luminance(mixed) > target) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const double t = (lo + hi) / 2.0;
    for (double &v : linear) {
        v += (1.0 - v) * t;
    }
    return encode_linear(linear, color.a);
}

/// @brief 档位查表。
/// @param tier 登记表里的档位。
/// @return 该档下限；不适用档与查不到时一律 0。
[[nodiscard]] auto tier_min_ratio(const ContrastTier tier) noexcept -> double {
    if (tier == ContrastTier::NotApplicable) {
        return AURORA_HIGH_CONTRAST_INAPPLICABLE;
    }
    for (const auto &row : AURORA_CONTRAST_TIERS) {
        if (row.tier == tier) {
            return row.min_ratio;
        }
    }
    return AURORA_HIGH_CONTRAST_INAPPLICABLE;
}

/// @brief 待判行集合（剔除不适用行，顺序与登记表一致）。
/// @return 求解用的行视图。
[[nodiscard]] auto audit_rows() -> std::vector<AuditRow> {
    std::vector<AuditRow> rows;
    rows.reserve(AURORA_CONTRAST_AUDITS.size());
    for (const ContrastPairAudit &audit : AURORA_CONTRAST_AUDITS) {
        const double min_ratio = tier_min_ratio(audit.tier);
        if (min_ratio <= AURORA_HIGH_CONTRAST_INAPPLICABLE) {
            continue;
        }
        rows.push_back(AuditRow{
            .fg = audit.fg,
            .bg = audit.bg,
            .min_ratio = min_ratio,
            .text = (audit.tier == ContrastTier::Text),
        });
    }
    return rows;
}

/// @brief 纯底色集合：只作背景出现、从不作前景的角色，派生中恒不动。
/// @param rows 待判行集合。
/// @return 成员指针列表（规模至多几十枚，线性查足够）。
[[nodiscard]] auto ground_roles(const std::vector<AuditRow> &rows) -> std::vector<RoleMember> {
    std::vector<RoleMember> grounds;
    for (const AuditRow &row : rows) {
        if (std::find(grounds.begin(), grounds.end(), row.bg) != grounds.end()) {
            continue;
        }
        const bool also_foreground = std::any_of(
            rows.begin(), rows.end(), [member = row.bg](const AuditRow &other) { return other.fg == member; });
        if (!also_foreground) {
            grounds.push_back(row.bg);
        }
    }
    return grounds;
}

/// @brief 当前状态下逐行达标位（按待判行顺序）。
/// @param roles 待判角色组。
/// @param rows 待判行集合。
/// @return 达标位列表。
[[nodiscard]] auto satisfied_map(const ColorRoles &roles, const std::vector<AuditRow> &rows) -> std::vector<bool> {
    std::vector<bool> met;
    met.reserve(rows.size());
    for (const AuditRow &row : rows) {
        met.push_back(contrast_ratio(roles.*(row.fg), roles.*(row.bg)) >= row.min_ratio);
    }
    return met;
}

/// @brief 把某端角色拉到「本行需要」的亮度：另一端视为定住，比它更暗 / 更亮各一个解。
/// @param color 该端现值。
/// @param other_l 另一端亮度。
/// @param want 本行目标比值（已含余量）。
/// @return 有效目标亮度列表（落在 [0,1] 且与现值有实际差距者）。
[[nodiscard]] auto luminance_solutions(const Color &color, const double other_l, const double want)
    -> std::vector<double> {
    const double current = relative_luminance(color);
    std::vector<double> out;
    out.reserve(2);
    for (const double target : {((other_l + AURORA_LUMA_BASE) / want) - AURORA_LUMA_BASE,
                                (want * (other_l + AURORA_LUMA_BASE)) - AURORA_LUMA_BASE}) {
        if (target < 0.0 || target > 1.0 || std::abs(target - current) <= AURORA_HIGH_CONTRAST_EPSILON) {
            continue;
        }
        out.push_back(target);
    }
    return out;
}

/// @brief 试写一个候选并数它弄坏了哪些已达标行（本行自身除外）。
/// @param probe 可写的角色组副本（调用方负责还原）。
/// @param cand 待试候选（写入后回填计数）。
/// @param rows 待判行集合。
/// @param index 本行下标。
/// @param before 试写前的达标位。
/// @return 试写后本行是否达标。
[[nodiscard]] auto score_candidate(ColorRoles &probe, Candidate &cand, const std::vector<AuditRow> &rows,
                                   const std::size_t index, const std::vector<bool> &before) -> bool {
    probe.*cand.member = cand.value;
    if (contrast_ratio(probe.*(rows[index].fg), probe.*(rows[index].bg)) < rows[index].min_ratio) {
        return false;
    }
    const std::vector<bool> after = satisfied_map(probe, rows);
    for (std::size_t j = 0; j < rows.size(); ++j) {
        if (j == index || !before[j] || after[j]) {
            continue;
        }
        ++cand.broken_total;
        if (rows[j].text) {
            ++cand.broken_text;
        }
    }
    return true;
}

}  // namespace

auto ColorRoles::with_high_contrast() const -> ColorRoles {
    const std::vector<AuditRow> rows = audit_rows();
    const std::vector<RoleMember> grounds = ground_roles(rows);
    ColorRoles out = *this;

    // 两轮：严格轮只接受不伤及他行的候选；放宽轮才允许为真冲突让路。
    for (const bool relaxed : {false, true}) {
        for (int pass = 0; pass < AURORA_HIGH_CONTRAST_PASSES; ++pass) {
            bool changed = false;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const AuditRow &row = rows[index];
                if (contrast_ratio(out.*(row.fg), out.*(row.bg)) >= row.min_ratio) {
                    continue;
                }
                const double want = row.min_ratio * AURORA_HIGH_CONTRAST_HEADROOM;
                const std::vector<bool> before = satisfied_map(out, rows);
                const ColorRoles snapshot = out;
                Candidate best;
                // 端次序：先底色（标签因此不动），后前景；纯底色直接跳过。
                for (const RoleMember member : {row.bg, row.fg}) {
                    if (std::find(grounds.begin(), grounds.end(), member) != grounds.end()) {
                        continue;
                    }
                    const RoleMember other = (member == row.bg) ? row.fg : row.bg;
                    const Color current = out.*member;
                    const double other_l = relative_luminance(out.*other);
                    for (const double target : luminance_solutions(current, other_l, want)) {
                        Candidate cand{
                            .valid = true,
                            .background_end = (member == row.bg),
                            .shift = std::abs(target - relative_luminance(current)),
                            .member = member,
                            .value = set_luminance(current, target),
                        };
                        if (!score_candidate(out, cand, rows, index, before)) {
                            out = snapshot;
                            continue;
                        }
                        out = snapshot;
                        if (best.valid ? candidate_rank(cand) < candidate_rank(best) : true) {
                            best = cand;
                        }
                    }
                }
                if (!best.valid) {
                    continue;
                }
                if (relaxed || (best.broken_text == 0 && best.broken_total == 0)) {
                    out.*best.member = best.value;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
    }
    return out;
}

}  // namespace aurora
