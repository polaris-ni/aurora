// AUTO-GENERATED from codespec/i18n_builtin.json — DO NOT EDIT.
// Regenerate: gen_i18n --emit-builtin <the header above> codespec/i18n_builtin.json
/// @brief Built-in message vocabulary: reserved-region Id constants, canonical names and English
/// fallback templates (the run-time defaults the library ships without translations).
/// @file
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "aurora/i18n/message_id.h"

namespace aurora::msg {

inline constexpr MessageId AURORA_REORDER_CANCELLED{0};  ///< aurora.reorder.cancelled
inline constexpr MessageId AURORA_REORDER_DROPPED{1};  ///< aurora.reorder.dropped
inline constexpr MessageId AURORA_REORDER_DROPPED_IN_PLACE{2};  ///< aurora.reorder.dropped_in_place
inline constexpr MessageId AURORA_REORDER_GRABBED{3};  ///< aurora.reorder.grabbed
inline constexpr MessageId AURORA_REORDER_POSITION{4};  ///< aurora.reorder.position
inline constexpr MessageId AURORA_COMMAND_PALETTE_NO_RESULTS{5};  ///< command_palette.no_results
inline constexpr MessageId AURORA_COMMAND_PALETTE_PLACEHOLDER{6};  ///< command_palette.placeholder
}  // namespace aurora::msg

namespace aurora::detail {

/// @brief Base of the built-in reserved region (application vocabularies start at 1024).
inline constexpr std::uint32_t AURORA_BUILTIN_MESSAGE_ID_BASE = 0;
/// @brief Built-in canonical names, sorted by name (Id order follows the source order).
inline constexpr std::array<std::string_view, 7> AURORA_BUILTIN_MESSAGE_NAMES{
    "aurora.reorder.cancelled",    "aurora.reorder.dropped",  "aurora.reorder.dropped_in_place",
    "aurora.reorder.grabbed",      "aurora.reorder.position", "command_palette.no_results",
    "command_palette.placeholder",
};
/// @brief Built-in English templates, parallel with the names (call-site fallbacks, not translations).
inline constexpr std::array<std::string_view, 7> AURORA_BUILTIN_MESSAGE_TEMPLATES{
    "Reorder cancelled. Item returned to position {position} of {total}.",
    "Item moved from position {from} to position {to} of {total}.",
    "Item dropped at position {position} of {total}.",
    "Position {position} of {total}. Item grabbed. Use arrow keys to move, space to drop, escape to cancel.",
    "Position {position} of {total}.",
    "No matching commands",
    "Type a command...",
};

/// @brief Built-in canonical name for an Id.
/// @param id Message Id, expected inside the reserved region.
/// @return Canonical name; empty view when the Id lies outside the region.
/// @note Thread: thread-safe (pure lookup)
/// @note Side-effects: none
/// @note Rebuildable: yes
constexpr auto builtin_name(aurora::MessageId id) -> std::string_view {
    if (id.value < AURORA_BUILTIN_MESSAGE_ID_BASE ||
        id.value >= AURORA_BUILTIN_MESSAGE_ID_BASE + AURORA_BUILTIN_MESSAGE_NAMES.size()) {
        return {};
    }
    return AURORA_BUILTIN_MESSAGE_NAMES.at(id.value - AURORA_BUILTIN_MESSAGE_ID_BASE);
}

}  // namespace aurora::detail
