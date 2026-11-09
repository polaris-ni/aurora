#pragma once

// Single source of truth for the color-role contrast audit: which role pairs are judged, at which
// tier, and which official preset columns are allowed to sit below the tier floor. Four consumers
// read this one table and none of them carries its own copy of the pair list:
//   * include/aurora/theming/color_roles.h -- `with_high_contrast()` drives the tone remap off the
//                                            tier floors, so the accessibility switch enforces
//                                            exactly what the gate judges (no second rule book);
//   * tools/gen/gen_contrast.cpp           -- computes every registered ratio through the library's
//                                            own aurora::contrast_ratio and emits the audit matrix;
//   * tools/check/check_theme_contrast.py  -- parses this table as text (the same way the theme
//                                            literal exemption table is parsed) and judges tiers,
//                                            coverage, preset floors, derived columns and the
//                                            specification wording against it;
//   * tests/unit/utest_theme_roles.cpp     -- asserts the same table at test time.
//
// Tier semantics (the ruling written up in codespec/specification/07-environment-modifier.md §5.1):
//   text         -- a foreground that carries readable text on that background: WCAG 2.2 SC 1.4.3, 4.5:1.
//   non_text     -- a boundary or state indicator whose shape carries the information: SC 1.4.11, 3.0:1.
//   not_applicable -- the stored role value is never what lands on screen (it is composited through an
//                                    explicit alpha at the call site) or it is decorative; each row needs
//                                    a reason, and an unregistered pair is judged by its tier, not skipped.

#include <array>
#include <cstdint>
#include <string_view>

#include "aurora/theming/color_roles.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/theming/theme_style.h"

namespace aurora::detail {

/// @brief Audit tier: decides the minimum ratio, or declares the pair unjudgeable.
enum class ContrastTier : std::uint8_t {
    Text,  ///< Readable text pair -- judged at 4.5:1.
    NonText,  ///< Non-text boundary / state indicator pair -- judged at 3.0:1.
    NotApplicable,  ///< Decorative, or composited through an alpha the role value does not carry.
};

/// @brief One registered pair plus the tier it is judged under.
struct ContrastPairAudit {
    Color ColorRoles::*fg;  ///< Foreground role member pointer.
    Color ColorRoles::*bg;  ///< Background role member pointer.
    ContrastTier tier;  ///< Tier that fixes the minimum ratio.
    std::string_view reason;  ///< Why this pair and this tier (ASCII, printed verbatim in the matrix).
};

/// @brief The floor attached to each tier, plus the criterion that names it.
///
/// The gate parses the specification wording against this table, so the numbers live exactly once.
struct ContrastTierFloor {
    ContrastTier tier;  ///< Tier this floor belongs to.
    double min_ratio;  ///< Minimum ratio; 0.0 for NotApplicable (no floor to meet).
    std::string_view criterion;  ///< External criterion id, e.g. "WCAG 2.2 SC 1.4.3".
};

/// @brief An official preset column allowed to sit below its tier floor.
///
/// Rationale: several vendor presets ship accent colors that are the only value in their palette
/// (Aqua controlAccentColor, KDE Breeze #3DAEE9). Darkening them to reach 4.5:1 would mean inventing
/// a color the platform does not have, which is a worse outcome than a recorded floor. A floor row is
/// only legal while the measured ratio is still below the tier floor -- once the value is fixed the
/// row has to be deleted, and the gate turns red on both directions. The floors bind to the *normal*
/// columns only: the high-contrast derivation is expected to clear every tier, which is a gate leg.
struct ContrastPresetFloor {
    ThemeStyle style;  ///< Preset column (style axis).
    ColorScheme scheme;  ///< Preset column (light/dark axis).
    Color ColorRoles::*fg;  ///< Foreground role of the audited pair.
    Color ColorRoles::*bg;  ///< Background role of the audited pair.
    double floor_ratio;  ///< Measured floor to pin the current state at.
    std::string_view reason;  ///< Which upstream value this copies (ASCII).
};

/// @brief Tier floors -- the three-tier ruling itself.
inline constexpr std::array<ContrastTierFloor, 3> AURORA_CONTRAST_TIERS{{
    {.tier = ContrastTier::Text, .min_ratio = 4.5, .criterion = "WCAG 2.2 SC 1.4.3"},
    {.tier = ContrastTier::NonText, .min_ratio = 3.0, .criterion = "WCAG 2.2 SC 1.4.11"},
    {.tier = ContrastTier::NotApplicable, .min_ratio = 0.0, .criterion = "not a contrast-carrying pair"},
}};

/// @brief Registered audit pairs. Every one of the 38 color roles must appear here, as a foreground,
///        as a background, or in a not_applicable row -- that coverage is a gate leg, not a comment.
///
/// The declared size is checked by the gate against the number of rows it parses out of this table,
/// so a row added without bumping the count (or the reverse: a dangling default-initialized row whose
/// pair reads as `/`) turns the gate red instead of silently auditing 30 of 31 pairs.
inline constexpr std::array<ContrastPairAudit, 30> AURORA_CONTRAST_AUDITS{{
    // -- the ten on_* pairings the base columns are built around ----------------------------------
    {.fg = &ColorRoles::on_primary,
     .bg = &ColorRoles::primary,
     .tier = ContrastTier::Text,
     .reason = "label painted on a filled primary container"},
    {.fg = &ColorRoles::on_primary_container,
     .bg = &ColorRoles::primary_container,
     .tier = ContrastTier::Text,
     .reason = "label painted on a primary container"},
    {.fg = &ColorRoles::on_secondary,
     .bg = &ColorRoles::secondary,
     .tier = ContrastTier::Text,
     .reason = "label painted on a filled secondary container"},
    {.fg = &ColorRoles::on_secondary_container,
     .bg = &ColorRoles::secondary_container,
     .tier = ContrastTier::Text,
     .reason = "label painted on a secondary container"},
    {.fg = &ColorRoles::on_tertiary,
     .bg = &ColorRoles::tertiary,
     .tier = ContrastTier::Text,
     .reason = "label painted on a filled tertiary container"},
    {.fg = &ColorRoles::on_tertiary_container,
     .bg = &ColorRoles::tertiary_container,
     .tier = ContrastTier::Text,
     .reason = "label painted on a tertiary container"},
    {.fg = &ColorRoles::on_error,
     .bg = &ColorRoles::error,
     .tier = ContrastTier::Text,
     .reason = "label painted on a filled error container"},
    {.fg = &ColorRoles::on_error_container,
     .bg = &ColorRoles::error_container,
     .tier = ContrastTier::Text,
     .reason = "label painted on an error container"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::Text,
     .reason = "body text on the base surface"},
    {.fg = &ColorRoles::on_surface_variant,
     .bg = &ColorRoles::surface_variant,
     .tier = ContrastTier::Text,
     .reason = "secondary text on the variant surface"},
    // -- the same foreground over every neutral the library paints text on ------------------------
    {.fg = &ColorRoles::on_surface_variant,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::Text,
     .reason = "secondary text on the base surface"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_dim,
     .tier = ContrastTier::Text,
     .reason = "body text on the dimmed surface"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_bright,
     .tier = ContrastTier::Text,
     .reason = "body text on the brightened surface"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_container_lowest,
     .tier = ContrastTier::Text,
     .reason = "body text on the lowest elevation container"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_container_low,
     .tier = ContrastTier::Text,
     .reason = "body text on the low elevation container"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_container,
     .tier = ContrastTier::Text,
     .reason = "body text on the base elevation container"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_container_high,
     .tier = ContrastTier::Text,
     .reason = "body text on the high elevation container"},
    {.fg = &ColorRoles::on_surface,
     .bg = &ColorRoles::surface_container_highest,
     .tier = ContrastTier::Text,
     .reason = "body text on the highest elevation container"},
    // -- the inverted surface family (snackbars, tooltips) ---------------------------------------
    {.fg = &ColorRoles::inverse_on_surface,
     .bg = &ColorRoles::inverse_surface,
     .tier = ContrastTier::Text,
     .reason = "body text on an inverted floating surface"},
    {.fg = &ColorRoles::inverse_primary,
     .bg = &ColorRoles::inverse_surface,
     .tier = ContrastTier::Text,
     .reason = "accent text or icon on an inverted floating surface"},
    // -- non-text: boundaries and state indicators that carry information by their shape ---------
    {.fg = &ColorRoles::outline,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NonText,
     .reason = "input component boundary drawn on the base surface"},
    {.fg = &ColorRoles::focus_ring,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NonText,
     .reason = "focus indicator ring -- the only cue that a control is focused"},
    {.fg = &ColorRoles::error,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NonText,
     .reason = "error-state glyph or underline on the base surface"},
    {.fg = &ColorRoles::primary,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NonText,
     .reason = "active-track and thumb graphics of the interactive controls"},
    // -- not applicable: recorded, reasoned, never silently skipped ------------------------------
    {.fg = &ColorRoles::outline_variant,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NotApplicable,
     .reason = "decorative divider; no state or boundary is identified by it"},
    {.fg = &ColorRoles::scrim,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NotApplicable,
     .reason = "painted only through an alpha chosen at the call site; the stored value never lands"},
    {.fg = &ColorRoles::shadow,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NotApplicable,
     .reason = "elevation ramp composited with a per-caster alpha; same reason as scrim"},
    {.fg = &ColorRoles::surface_tint,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NotApplicable,
     .reason = "elevation tint blended under state layers; identifies no boundary or state"},
    {.fg = &ColorRoles::hover,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NotApplicable,
     .reason = "state overlay ships a partial alpha; contrast is defined only after compositing"},
    {.fg = &ColorRoles::pressed,
     .bg = &ColorRoles::surface,
     .tier = ContrastTier::NotApplicable,
     .reason = "state overlay ships a partial alpha; same reason as hover"},
}};

/// @brief Preset columns that copy an upstream accent value which does not reach the tier floor.
///
/// Provenance for every row is the measured / official value table in
/// codespec/changes/platform-theme-styles/proposal.md; a floor may only be registered while the
/// column really does sit below its tier floor, which is the other half of the assertion.
inline constexpr std::array<ContrastPresetFloor, 14> AURORA_CONTRAST_PRESET_FLOORS{{
    {.style = ThemeStyle::MacOs,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::on_primary,
     .bg = &ColorRoles::primary,
     .floor_ratio = 4.0,
     .reason = "Aqua controlAccentColor #007AFF with white label; HIG ships no darker accent"},
    {.style = ThemeStyle::MacOs,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::on_primary,
     .bg = &ColorRoles::primary,
     .floor_ratio = 4.0,
     .reason = "same Aqua accent value in the dark column"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::on_primary,
     .bg = &ColorRoles::primary,
     .floor_ratio = 2.4,
     .reason = "Breeze #3DAEE9 is the Plasma 6 accent in both columns; white label is the real look"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::on_primary,
     .bg = &ColorRoles::primary,
     .floor_ratio = 2.4,
     .reason = "same Breeze accent value in the dark column"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::on_error,
     .bg = &ColorRoles::error,
     .floor_ratio = 4.2,
     .reason = "Breeze ForegroundNegative #DA4453 with white label"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::on_error,
     .bg = &ColorRoles::error,
     .floor_ratio = 4.2,
     .reason = "same ForegroundNegative value in the dark column"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::on_surface_variant,
     .bg = &ColorRoles::surface_variant,
     .floor_ratio = 3.2,
     .reason = "Breeze ForegroundInactive is the de-emphasis role; it sits below AA by design"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::on_surface_variant,
     .bg = &ColorRoles::surface_variant,
     .floor_ratio = 3.8,
     .reason = "same ForegroundInactive role in the dark column"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::on_surface_variant,
     .bg = &ColorRoles::surface,
     .floor_ratio = 3.6,
     .reason = "Breeze InactiveText #707D8A over the Window background; de-emphasis is the point"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::primary,
     .bg = &ColorRoles::surface,
     .floor_ratio = 2.1,
     .reason = "Breeze Highlight #3DAEE9 is the only Plasma 6 accent; light Window background"},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .fg = &ColorRoles::focus_ring,
     .bg = &ColorRoles::surface,
     .floor_ratio = 2.1,
     .reason = "the focus ring copies the same Breeze Highlight accent"},
    {.style = ThemeStyle::Windows,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::error,
     .bg = &ColorRoles::surface,
     .floor_ratio = 2.8,
     .reason = "measured Win11 danger red #C42B1C, shared by both columns; Fluent ships no darker one"},
    {.style = ThemeStyle::MacOs,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::error,
     .bg = &ColorRoles::surface,
     .floor_ratio = 2.6,
     .reason = "the macOS column does not override the error seed, so #E01B24 sits on the dark Aqua window"},
    {.style = ThemeStyle::Gnome,
     .scheme = ColorScheme::Dark,
     .fg = &ColorRoles::error,
     .bg = &ColorRoles::surface,
     .floor_ratio = 2.5,
     .reason = "Adwaita dark destructive_bg_color #C01C28; official value, built to carry white text"},
}};

}  // namespace aurora::detail
