// Emits the machine-readable color-role contrast audit matrix that tools/check/check_theme_contrast.py
// judges. Every ratio goes through the library's own aurora::contrast_ratio: a second WCAG
// implementation here (or in the gate, or in a test) is exactly the three-way drift this tool exists
// to prevent, so this file must stay free of any color math of its own.
//
// Columns are the preset combinations the library ships (5 style values x light/dark) in both contrast
// levels: the normal column comes straight from Theme::from_style -- the same function the window host uses
// to build the root theme, so the audit reads the values that actually reach the screen -- and the
// high_contrast column is the same call with AccessibilitySettings::high_contrast set, which is the only
// way the gate can judge the switch instead of trusting it. That is why the derived columns carry no
// exemption: the switch is judged against the bare tier floors, and the only place a hand-set color can
// rescue a cell is the column it was calibrated in.
//
// Usage:
//   gen_contrast_tools          -> write the matrix to stdout
//   gen_contrast_tools -        -> explicit stdout sentinel (used by the gate)
//   gen_contrast_tools out.json -> write a file (triage aid)
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/theming/detail/contrast_audit.h"

namespace {

using aurora::Color;
using aurora::ColorRoles;
using aurora::ColorScheme;
using aurora::Theme;
using aurora::ThemeStyle;
using aurora::detail::AURORA_CONTRAST_AUDITS;
using aurora::detail::AURORA_CONTRAST_PRESET_FLOORS;
using aurora::detail::AURORA_CONTRAST_TIERS;
using aurora::detail::ContrastTier;

// Prefix stripped from registry token names to recover the short name. It is the group prefix, not a
// hand-copied token: the same string `Theme::theme_token_groups()` declares as the `theme` group prefix.
constexpr std::string_view AURORA_TIER_PREFIX = "theme.";  // THEME-LITERAL: 组前缀（非逐枚令牌名），口径同 theme.h

/// @brief Tier label used in the emitted matrix (ASCII, machine-matched by the gate).
/// @param tier audit tier.
/// @return `text` / `non_text` / `not_applicable`.
[[nodiscard]] constexpr auto tier_name(const ContrastTier tier) noexcept -> std::string_view {
    switch (tier) {
        case ContrastTier::Text:
            return "text";
        case ContrastTier::NonText:
            return "non_text";
        case ContrastTier::NotApplicable:
            return "not_applicable";
    }
    return "unknown";
}

/// @brief Style label used in the emitted matrix; must stay in step with the ThemeStyle spelling.
/// @param style style axis value.
/// @return one of the five enum names.
[[nodiscard]] constexpr auto style_name(const ThemeStyle style) noexcept -> std::string_view {
    switch (style) {
        case ThemeStyle::Material:
            return "Material";
        case ThemeStyle::Windows:
            return "Windows";
        case ThemeStyle::MacOs:
            return "MacOs";
        case ThemeStyle::Gnome:
            return "Gnome";
        case ThemeStyle::Kde:
            return "Kde";
    }
    return "Unknown";
}

/// @brief Scheme label used in the emitted matrix.
/// @param scheme light/dark axis value.
/// @return `Light` or `Dark`.
[[nodiscard]] constexpr auto scheme_name(const ColorScheme scheme) noexcept -> std::string_view {
    return scheme == ColorScheme::Dark ? "Dark" : "Light";
}

/// @brief Contrast-level label used in the emitted matrix.
/// @param high_contrast value of `AccessibilitySettings::high_contrast` for the column.
/// @return `high` or `normal`.
[[nodiscard]] constexpr auto contrast_name(const bool high_contrast) noexcept -> std::string_view {
    return high_contrast ? "high" : "normal";
}

/// @brief Role short name without the `theme.` token prefix.
/// @param member ColorRoles member pointer.
/// @return short name (e.g. `on_primary`); empty when the role is not in the token table.
[[nodiscard]] constexpr auto role_short_name(Color ColorRoles::*member) noexcept -> std::string_view {
    std::string_view token = ColorRoles::token_name(member);
    if (token.size() > AURORA_TIER_PREFIX.size() && token.starts_with(AURORA_TIER_PREFIX)) {
        token.remove_prefix(AURORA_TIER_PREFIX.size());
        return token;
    }
    return {};
}

/// @brief Ratio rounded to the emitted precision (three decimals; the gate compares with the same
///        rounding so a fourth-decimal float wobble cannot flip a verdict).
/// @param value raw ratio from aurora::contrast_ratio.
/// @return value scaled to 1/1000 and back.
[[nodiscard]] auto round_ratio(const double value) noexcept -> double { return std::round(value * 1000.0) / 1000.0; }

/// @brief Minimum ratio attached to a tier by the tier table.
/// @param tier audit tier.
/// @return the floor; 0.0 for not_applicable rows.
[[nodiscard]] constexpr auto tier_floor(const ContrastTier tier) noexcept -> double {
    for (const auto &row : AURORA_CONTRAST_TIERS) {
        if (row.tier == tier) {
            return row.min_ratio;
        }
    }
    return 0.0;
}

/// @brief Preset floor registered for one column of one pair, if any.
/// @param style style axis of the column.
/// @param scheme light/dark axis of the column.
/// @param high_contrast contrast level of the column; the derived level is exempt by construction,
///        because the registry calibrates the hand-set preset columns and nothing else.
/// @param fg foreground role of the pair.
/// @param bg background role of the pair.
/// @return pointer into the policy table, or nullptr when the column has no registered floor.
[[nodiscard]] auto preset_floor(const ThemeStyle style, const ColorScheme scheme, const bool high_contrast,
                                Color ColorRoles::*fg, Color ColorRoles::*bg) noexcept
    -> const aurora::detail::ContrastPresetFloor * {
    if (high_contrast) {
        return nullptr;
    }
    for (const auto &row : AURORA_CONTRAST_PRESET_FLOORS) {
        if (row.style == style && row.scheme == scheme && row.fg == fg && row.bg == bg) {
            return &row;
        }
    }
    return nullptr;
}

/// @brief Hex spelling of a color, alpha included, so the matrix is self-describing.
/// @param c the color.
/// @return `#RRGGBBAA`.
[[nodiscard]] auto color_text(const Color &c) -> std::string {
    std::array<char, 10> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    return std::string{buffer.data()};
}

/// @brief Assemble the whole matrix.
/// @return schema-shaped JSON: tiers, preset floors, and one block per column.
[[nodiscard]] auto build_matrix() -> au::Json {
    au::Json out = au::Json::object();
    out.set("gate", "check_theme_contrast");

    au::Json tiers = au::Json::array();
    for (const auto &row : AURORA_CONTRAST_TIERS) {
        au::Json item = au::Json::object();
        item.set("tier", std::string{tier_name(row.tier)});
        item.set("min_ratio", au::Json{row.min_ratio});
        item.set("criterion", std::string{row.criterion});
        tiers.push_back(std::move(item));
    }
    out.set("tiers", std::move(tiers));

    au::Json floors = au::Json::array();
    for (const auto &row : AURORA_CONTRAST_PRESET_FLOORS) {
        au::Json item = au::Json::object();
        item.set("style", std::string{style_name(row.style)});
        item.set("scheme", std::string{scheme_name(row.scheme)});
        // Registered floors are calibrated against the preset column as shipped, so they always belong to
        // the normal level; the gate re-checks that no floor was ever attached to a derived cell.
        item.set("contrast", std::string{contrast_name(false)});
        item.set("pair", std::string{role_short_name(row.fg)} + "/" + std::string{role_short_name(row.bg)});
        item.set("floor_ratio", au::Json{row.floor_ratio});
        item.set("reason", std::string{row.reason});
        floors.push_back(std::move(item));
    }
    out.set("preset_floors", std::move(floors));

    au::Json columns = au::Json::array();
    int column_count = 0;
    for (const ThemeStyle style :
         {ThemeStyle::Material, ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome, ThemeStyle::Kde}) {
        for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
            // Both contrast levels of the same preset column: the derived level goes through the public
            // materialization entry point with the switch set, which is what the runtime root theme does.
            for (const bool high_contrast : {false, true}) {
                const aurora::AccessibilitySettings accessibility{.reduce_motion = false,
                                                                  .high_contrast = high_contrast};
                const Theme themed = Theme::from_style(style, scheme, accessibility);
                const ColorRoles &roles = themed.roles;

                au::Json role_values = au::Json::object();
                for (const auto &[member, token] : ColorRoles::AURORA_TOKENS) {
                    role_values.set(std::string{token}, color_text(roles.*member));
                }

                au::Json pairs = au::Json::array();
                for (const auto &row : AURORA_CONTRAST_AUDITS) {
                    const double ratio = aurora::contrast_ratio(roles.*(row.fg), roles.*(row.bg));
                    const auto *floor = preset_floor(style, scheme, high_contrast, row.fg, row.bg);
                    au::Json item = au::Json::object();
                    item.set("pair", std::string{role_short_name(row.fg)} + "/" + std::string{role_short_name(row.bg)});
                    item.set("tier", std::string{tier_name(row.tier)});
                    item.set("ratio", au::Json{round_ratio(ratio)});
                    item.set("min_ratio", au::Json{tier_floor(row.tier)});
                    item.set("registered_floor", floor != nullptr ? au::Json{floor->floor_ratio} : au::Json{nullptr});
                    item.set("reason", std::string{row.reason});
                    pairs.push_back(std::move(item));
                }

                au::Json column = au::Json::object();
                column.set("style", std::string{style_name(style)});
                column.set("scheme", std::string{scheme_name(scheme)});
                column.set("contrast", std::string{contrast_name(high_contrast)});
                column.set("roles", std::move(role_values));
                column.set("pairs", std::move(pairs));
                columns.push_back(std::move(column));
                ++column_count;
            }
        }
    }
    out.set("columns", std::move(columns));
    out.set("audit_count", au::Json{static_cast<int>(AURORA_CONTRAST_AUDITS.size())});
    out.set("column_count", au::Json{column_count});
    return out;
}

/// @brief Command declaration table: the single output slot is both written and read; `-` (default)
///        means stdout.
[[nodiscard]] auto build_spec() -> const aurora::cli::CommandSpec & {
    static const aurora::cli::CommandSpec ROOT_SPEC = [] {
        aurora::cli::CommandSpec root;
        root.name = "gen_contrast_tools";
        root.about = "Emit the color-role contrast audit matrix computed through aurora::contrast_ratio.";
        root.positionals = {
            aurora::cli::PositionalSchema{
                .name = "OUT",
                .kind = aurora::cli::ValueKind::String,
                .arity = aurora::cli::Arity::optional_one(),
                .help = "Output path; `-` or omitted writes to stdout",
                .default_text = "-",
            },
        };
        root.epilog = "Exit codes: 0 = ok, 2 = usage error, 1 = business failure.";
        return root;
    }();
    return ROOT_SPEC;
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），生成器不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto parsed = aurora::cli::parse(build_spec(), argc, argv);
    if (!parsed) {
        std::string detail = parsed.error().message;
        if (!parsed.error().suggestion.empty()) {
            detail += " -- " + parsed.error().suggestion;
        }
        AURORA_LOG_ERROR("contrast", detail);
        return 2;
    }
    const aurora::cli::Invocation &invocation = parsed.value();
    if (invocation.shows_display()) {
        AURORA_LOG_RAW("contrast", invocation.display_text);
        return 0;
    }
    const std::string output_path = invocation.arguments.positional(0).value().as<std::string>().value();
    const bool to_stdout = (output_path == "-");

    const au::Json matrix = build_matrix();
    const auto dumped = au::json::dump(matrix, {.indent = 2});
    const std::string text = dumped.ok() ? dumped.value() : std::string{};
    if (!dumped.ok()) {
        AURORA_LOG_ERROR("contrast", "json dump failed: ", dumped.error().message);
        return 1;
    }
    if (to_stdout) {
        AURORA_LOG_RAW("contrast", text, "\n");
        return 0;
    }
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        AURORA_LOG_ERROR("contrast", "cannot open output file: ", output_path);
        return 1;
    }
    out << text << "\n";
    return 0;
}
