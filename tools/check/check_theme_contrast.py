#!/usr/bin/env python3
# ============================================================================
# check_theme_contrast.py - 色角色对比度三档口径门禁
# ----------------------------------------------------------------------------
# Spec: codespec/specification/07-environment-modifier.md section 5.1 (the three-tier
#   contrast ruling) + codespec/changes/property-single-declaration/proposal.md
#   acceptance criterion 14.
#
#   Contrast is the one accessibility property that every theme column claims to have and that
#   nothing checks: the numbers live in three places (a hand-copied WCAG formula in a unit test,
#   the preset tables, and the prose in the spec), and they agree only by luck. This gate closes
#   that by reading the ratios from the built product itself -- tools/gen/gen_contrast_tools
#   computes them through the library's own aurora::contrast_ratio -- and judging them against the
#   single registry table (include/aurora/theming/detail/contrast_audit.h) plus the spec wording.
#
#   Nothing here re-implements WCAG. The gate never touches a color value directly; it consumes
#   the emitted matrix. That is also why the gate must run after the tools are built (same
#   precondition as check_api_schema_sync).
#
# Judgement legs (ids appear verbatim in the violation lines, for mutation self-proof):
#   C0 environment: the tool is missing / failed / emitted non-JSON, or an input file is absent.
#      Never silently green: any C0 aborts before the ratio legs are reported.
#   C1 table shape: declared std::array sizes == row counts parsed out of the registry header ==
#      what the matrix reports. A dangling default-initialized row (array size bigger than the
#      rows written) or a row added without bumping the size is red here -- both were live bugs.
#   C2 column surface: emitted (style, scheme, contrast) columns == ThemeStyle enumerators parsed
#      from theme_style.h x {Light, Dark} x {normal, high}. A new style value the tool forgot to
#      loop over, or a contrast level it dropped, is red.
#   C3 role coverage: every color role the token table declares (ColorRoles::AURORA_TOKENS) is
#      emitted by every column and appears as one side of at least one registered pair; the role
#      set is identical across columns, pair halves resolve to real roles, and every tier label is
#      one of the three known ones. The expectation comes from the header, not from the emitted
#      matrix -- a role the tool stopped emitting would otherwise shrink the judged surface in
#      silence, which is the one way this leg could pass vacuously.
#   C4 tier table pinned: exactly three tiers, text 4.5 / non_text 3.0 / not_applicable 0.0 with
#      the WCAG SC ids -- the registry's claim checked against the external criterion.
#   C5 tier judgement: text pairs >= 4.5 and non_text pairs >= 3.0 in every normal column, unless
#      that exact column+pair cell carries a registered preset floor, in which case >= that floor.
#      not_applicable rows are not judged but must carry a reason (C7).
#   C6 exemption validity: a registered floor must point at a real normal cell, must sit strictly
#      below its tier floor (otherwise the cell is fine and the exemption is stale -- delete it),
#      and every normal cell below its tier floor must be registered. Both directions, one leg.
#      Floors are calibrated against preset columns as shipped, so a floor attached to a derived
#      cell -- or to any cell outside {Light, Dark} x normal -- is red.
#   C7 reasons: every audit row and every preset floor carries a non-empty ASCII reason.
#   C8 spec agreement: section 5.1 states the same two ratios and criterion ids as the tier table
#      -- and states them on the same line, so the tier bullet cannot drift while some other
#      sentence of the section still spells the number. It also carries the three ruling sentences
#      (unregistered pairs fall to the two tiers; not_applicable rows need a registered reason;
#      derived columns get no exemption).
#   C9 derived columns: the high_contrast level is judged by the switch's own claim, so it gets no
#      exemption at all -- every judged cell must clear its bare tier floor. The leg also pins that
#      the derivation is a response to a defect and nothing more: a derived column must differ from
#      its normal counterpart if and only if that normal column has at least one judged cell below
#      its tier floor (a conforming palette is not re-coloured; a broken one is not left alone), and
#      at least one column must move, otherwise the switch is inert and C9's first half passes
#      vacuously.
#
# Output is ASCII-only (ctest code-page constraint).
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_theme_contrast.py [build_dir] [--root PATH] [--verbose]
#       build_dir  directory holding gen_contrast_tools (default: "build")
# ============================================================================
import argparse
import json
import os
import re
import subprocess
import sys

import props_scan

POLICY = "include/aurora/theming/detail/contrast_audit.h"
STYLE_HEADER = "include/aurora/theming/theme_style.h"
COLOR_ROLES_HEADER = "include/aurora/theming/color_roles.h"
SPEC = "codespec/specification/07-environment-modifier.md"
SPEC_SECTION = "### 5.1 "
SCHEMES = ("Light", "Dark")
# The third column axis: the level as shipped and the level the accessibility switch derives.
CONTRAST_LEVELS = ("normal", "high")

# External truth this gate pins (WCAG 2.2 success criteria; the registry may not renegotiate them).
PINNED_TIERS = {
    "text": (4.5, "WCAG 2.2 SC 1.4.3"),
    "non_text": (3.0, "WCAG 2.2 SC 1.4.11"),
    "not_applicable": (0.0, "not a contrast-carrying pair"),
}
EPS = 1e-9

# Section 5.1 must carry these two ruling sentences. Escaped so this file stays ASCII
# (they are judgement-required document anchors, same treatment as the markers in
# check_no_theme_color_literals.py).
MARKER_DEFAULT_TIER = "\u672a\u767b\u8bb0\u8272\u5bf9\u9ed8\u8ba4\u6309\u524d\u4e24\u6863\u5224"
MARKER_REQUIRING_REASON = "\u9010\u6761\u767b\u8bb0\u7406\u7531"
# The derived level is the switch's own claim, so section 5.1 has to say out loud that the exemption
# table does not extend to it -- otherwise the reader assumes the floors cover both levels.
MARKER_DERIVED_NO_FLOOR = "\u6d3e\u751f\u5217\u4e0d\u4eab\u6709\u4efb\u4f55\u8c41\u514d"

DECL_SIZE_RE = re.compile(r"std::array<\s*(\w+)\s*,\s*(\d+)\s*>\s*(\w+)")
ENUM_RE = re.compile(r"enum\s+class\s+(\w+)\s*(?::\s*[\w:]+\s*)?\{")
ROLE_SHORT_RE = re.compile(r"\"([a-z0-9_.]+)\"")


def label(column, pair_name=None):
    """ASCII label naming one emitted column -- all three axes, so a line never says which one got lost.

    Args:
        column: emitted column object.
        pair_name: optional audited pair naming one cell of that column.

    Returns:
        ``Style/Scheme/level``, optionally followed by the pair.
    """
    text = "%s/%s/%s" % (column.get("style"), column.get("scheme"), column.get("contrast"))
    return text if pair_name is None else "%s %s" % (text, pair_name)


def add(violations, message):
    """Record a violation once.

    Audit-row facts (a role that is never judged, a missing reason) are re-discovered for every
    column of the matrix; repeating them per column would bury the count of real problems.

    Args:
        violations: violation sink.
        message: the line to record.
    """
    if message not in violations:
        violations.append(message)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def resolve_exe(build_dir, name):
    """Locate a built tool under build_dir, tolerating multi-config generators."""
    base = name + (".exe" if os.name == "nt" else "")
    candidates = [os.path.join(build_dir, base)]
    for cfg in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        candidates.append(os.path.join(build_dir, cfg, base))
    for cand in candidates:
        if os.path.isfile(cand):
            return cand
    return None


def parse_matrix(exe):
    """Run the tool with the stdout sentinel and decode its matrix; (matrix, error)."""
    try:
        proc = subprocess.run([exe, "-"], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, "C0 cannot run %s: %s" % (exe, exc)
    if proc.returncode != 0:
        return None, ("C0 gen_contrast_tools exited %d: %s" % (proc.returncode, (proc.stderr or "").strip()[:400]))
    try:
        return json.loads(proc.stdout), None
    except json.JSONDecodeError as exc:
        return None, "C0 gen_contrast_tools output is not JSON: %s" % exc


def array_bodies(text, names):
    """Declared size plus row count for each named std::array<> registry table."""
    found = {}
    for match in DECL_SIZE_RE.finditer(text):
        type_name, size_text, var_name = match.group(1), int(match.group(2)), match.group(3)
        if var_name not in names:
            continue
        open_idx = text.find("{", match.end())
        if open_idx < 0:
            found[var_name] = (type_name, size_text, None)
            continue
        inner, _ = props_scan.matching(text, open_idx, "{", "}")
        found[var_name] = (type_name, size_text, inner)
    return found


def row_count(body, marker):
    """Rows written inside a table body, counted by the marker field each row spells."""
    if body is None:
        return None
    return len(re.findall(re.escape(marker) + r"\s*=", body))


def style_enum(root, violations):
    """ThemeStyle enumerator names, declaration order."""
    path = os.path.join(root, STYLE_HEADER.replace("/", os.sep))
    if not os.path.isfile(path):
        violations.append("C0 missing header: %s" % STYLE_HEADER)
        return None
    text = props_scan.strip_comments(read(path))
    for match in ENUM_RE.finditer(text):
        if match.group(1) != "ThemeStyle":
            continue
        body, _ = props_scan.matching(text, match.end() - 1, "{", "}")
        if body is None:
            break
        names = []
        for piece in body.split(","):
            token = piece.strip().split("=")[0].strip()
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token):
                names.append(token)
        return names
    violations.append("C0 enum class ThemeStyle not found in %s" % STYLE_HEADER)
    return None


def short_role(name):
    """Role short name without the ``theme.`` prefix the token table carries."""
    return name[len("theme."):] if name.startswith("theme.") else name


def token_table_roles(root, violations):
    """Color role names declared by ``ColorRoles::AURORA_TOKENS``, in table order.

    This is the C3 expectation: what the library *has*, read from the header, deliberately not
    derived from the emitted matrix -- otherwise a role the tool stops emitting simply disappears
    from the judged surface and the leg goes green on a smaller problem.
    """
    path = os.path.join(root, COLOR_ROLES_HEADER.replace("/", os.sep))
    if not os.path.isfile(path):
        violations.append("C0 missing header: %s" % COLOR_ROLES_HEADER)
        return None
    text = props_scan.strip_comments(read(path))
    index = text.find("AURORA_TOKENS = std::to_array")
    if index < 0:
        violations.append("C0 ColorRoles::AURORA_TOKENS initializer not found in %s" % COLOR_ROLES_HEADER)
        return None
    open_idx = text.find("{", index)
    if open_idx < 0:
        violations.append("C0 cannot locate the AURORA_TOKENS initializer body in %s" % COLOR_ROLES_HEADER)
        return None
    body, _ = props_scan.matching(text, open_idx, "{", "}")
    if body is None:
        violations.append("C0 unbalanced AURORA_TOKENS initializer body in %s" % COLOR_ROLES_HEADER)
        return None
    names = [short_role(match.group(1)) for match in ROLE_SHORT_RE.finditer(body)]
    if not names:
        violations.append("C0 AURORA_TOKENS declares no role name in %s" % COLOR_ROLES_HEADER)
        return None
    if len(set(names)) != len(names):
        violations.append("C0 AURORA_TOKENS registers a role name twice in %s" % COLOR_ROLES_HEADER)
    return names


def spec_section(root, violations):
    """Text of the section 5.1 block (heading to the next heading)."""
    path = os.path.join(root, SPEC.replace("/", os.sep))
    if not os.path.isfile(path):
        violations.append("C0 missing spec file: %s" % SPEC)
        return None
    lines = read(path).splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.startswith(SPEC_SECTION):
            start = index + 1
            break
    if start is None:
        violations.append("C0 spec has no '%s' heading in %s" % (SPEC_SECTION.strip(), SPEC))
        return None
    collected = []
    for line in lines[start:]:
        if line.startswith("## ") or line.startswith("### "):
            break
        collected.append(line)
    return "\n".join(collected)


def check_table_shape(matrix, policy_text, violations):
    """C1: declared sizes, parsed row counts and the matrix's own counts must agree."""
    names = {"AURORA_CONTRAST_AUDITS": ".fg",
             "AURORA_CONTRAST_PRESET_FLOORS": ".style",
             "AURORA_CONTRAST_TIERS": ".tier"}
    tables = array_bodies(policy_text, names)
    for name in sorted(names):
        if name not in tables:
            violations.append("C1 registry table missing from %s: %s" % (POLICY, name))
            continue
        type_name, size, body = tables[name]
        rows = row_count(body, names[name])
        if rows is None:
            violations.append("C1 cannot parse table body: %s" % name)
            continue
        if rows != size:
            violations.append("C1 %s declares %d but carries %d row(s) "
                              "(a size bigger than the rows leaves a default-initialized ghost row)"
                              % (name, size, rows))
    counts = {
        "AURORA_CONTRAST_AUDITS": matrix.get("audit_count"),
        "AURORA_CONTRAST_PRESET_FLOORS": len(matrix.get("preset_floors") or []),
        "AURORA_CONTRAST_TIERS": len(matrix.get("tiers") or []),
    }
    for name, from_matrix in counts.items():
        entry = tables.get(name)
        if entry is None or from_matrix is None:
            continue
        _type, size, body = entry
        rows = row_count(body, names[name])
        if rows is not None and rows != from_matrix:
            violations.append("C1 %s: header carries %d row(s), the built tool emits %d "
                              "(stale build or divergent table)" % (name, rows, from_matrix))
    columns = matrix.get("columns") or []
    if matrix.get("column_count") != len(columns):
        violations.append("C1 column_count %r != number of emitted columns %d"
                          % (matrix.get("column_count"), len(columns)))
    for column in columns:
        if len(column.get("pairs") or []) != matrix.get("audit_count"):
            violations.append("C1 column %s carries %d pair(s), audit table has %s"
                              % (label(column), len(column.get("pairs") or []), matrix.get("audit_count")))


def check_columns(matrix, styles, violations):
    """C2: emitted columns == every ThemeStyle value x both schemes x both contrast levels."""
    columns = matrix.get("columns") or []
    seen = set()
    for column in columns:
        key = (column.get("style"), column.get("scheme"), column.get("contrast"))
        if key in seen:
            violations.append("C2 duplicate column %s" % label(column))
        seen.add(key)
        if column.get("scheme") not in SCHEMES:
            violations.append("C2 column %s has an unknown scheme" % label(column))
        if column.get("contrast") not in CONTRAST_LEVELS:
            violations.append("C2 column %s has an unknown contrast level" % label(column))
    for style in styles:
        for scheme in SCHEMES:
            for contrast in CONTRAST_LEVELS:
                if (style, scheme, contrast) not in seen:
                    violations.append("C2 %s/%s/%s column is not audited "
                                      "(the tool's style list lags enum class ThemeStyle, or one "
                                      "contrast level stopped being emitted)"
                                      % (style, scheme, contrast))
    for column in columns:
        if column.get("style") not in styles:
            violations.append("C2 column %s names no ThemeStyle value" % label(column))


def check_coverage(matrix, token_roles, violations):
    """C3: token-table roles all emitted and audited, pair halves real, tier labels known."""
    columns = matrix.get("columns") or []
    if not columns:
        violations.append("C3 no audited column at all (the gate would otherwise pass vacuously)")
        return None
    expected = set(token_roles or [])
    reference = None
    for column in columns:
        roles = {short_role(name) for name in (column.get("roles") or {})}
        if not roles:
            add(violations, "C3 column %s emits no role values" % label(column))
            continue
        if reference is None:
            reference = roles
        elif roles != reference:
            add(violations, "C3 column %s role set differs from the first column (%d vs %d)"
                % (label(column), len(roles), len(reference)))
        for name in sorted(expected - roles):
            add(violations, "C3 column %s emits no value for token-table role '%s' (a role the tool "
                            "stopped emitting drops out of the judged surface in silence)"
                % (label(column), name))
    if reference is None:
        return None
    short_names = reference | expected
    audited = set()
    for column in columns:
        for pair in column.get("pairs") or []:
            text = pair.get("pair") or ""
            if text.count("/") != 1:
                add(violations, "C3 malformed pair name '%s' in %s (empty side = a ghost table row)"
                    % (text, label(column)))
                continue
            fg, bg = text.split("/")
            for side in (fg, bg):
                if side not in short_names:
                    add(violations, "C3 pair '%s' names no color role in %s" % (text, label(column)))
                audited.add(side)
            if pair.get("tier") not in PINNED_TIERS:
                add(violations, "C3 pair '%s' has unknown tier '%s'" % (text, pair.get("tier")))
    missing = sorted(short_names - audited)
    for name in missing:
        add(violations, "C3 color role '%s' is never audited (register it as fg, bg, or a reasoned "
                        "not_applicable row)" % name)
    if not audited:
        violations.append("C3 no pair audited at all (judgement surface is empty)")
    return short_names


def check_tier_table(matrix, violations):
    """C4: the registry's tier floors must be the external criteria."""
    tiers = {row.get("tier"): row for row in (matrix.get("tiers") or [])}
    if len(matrix.get("tiers") or []) != len(PINNED_TIERS):
        violations.append("C4 tier table has %d row(s), expected %d"
                          % (len(matrix.get("tiers") or []), len(PINNED_TIERS)))
    for name, (ratio, criterion) in PINNED_TIERS.items():
        row = tiers.get(name)
        if row is None:
            violations.append("C4 tier '%s' is missing from the registry" % name)
            continue
        if abs(float(row.get("min_ratio", -1)) - ratio) > EPS:
            violations.append("C4 tier '%s' registers min_ratio %r, WCAG says %r" % (name, row.get("min_ratio"), ratio))
        if row.get("criterion") != criterion:
            violations.append("C4 tier '%s' cites '%s', expected '%s'" % (name, row.get("criterion"), criterion))
    return tiers


def check_ratios(matrix, violations):
    """C5 + C6: judge the shipped columns, and police the exemption table in both directions."""
    floors = {(row.get("style"), row.get("scheme"), row.get("pair")): row
              for row in (matrix.get("preset_floors") or [])}
    # A floor is a statement about a preset column as shipped; the registry has no contrast axis, so a
    # floor that claims a derived level names a cell that does not exist.
    for row in matrix.get("preset_floors") or []:
        if row.get("contrast") not in (None, "normal"):
            violations.append("C6 preset floor %s/%s/%s %s: floors bind to the shipped column, "
                              "a derived column gets no exemption"
                              % (row.get("style"), row.get("scheme"), row.get("contrast"), row.get("pair")))
    seen_cells = set()
    for column in matrix.get("columns") or []:
        if column.get("contrast") != "normal":
            continue  # C9 judges the derived level, with no exemptions available
        style, scheme = column.get("style"), column.get("scheme")
        for pair in column.get("pairs") or []:
            name, tier = pair.get("pair"), pair.get("tier")
            cell = (style, scheme, name)
            seen_cells.add(cell)
            floor_row = floors.get(cell)
            registered = floor_row.get("floor_ratio") if floor_row else None
            if registered is None and pair.get("registered_floor") is not None:
                violations.append("C6 %s: the matrix carries a floor the preset table does not list"
                                  % label(column, name))
            if tier == "not_applicable":
                if registered is not None:
                    violations.append("C6 %s: not_applicable rows cannot carry a preset floor"
                                      % label(column, name))
                continue
            tier_min = PINNED_TIERS.get(tier, (None, None))[0]
            if tier_min is None:
                continue  # C3 already reported the unknown tier
            ratio = float(pair.get("ratio", 0.0))
            need = registered if registered is not None else tier_min
            if ratio + EPS < need:
                violations.append("C5 %s tier=%s ratio %.3f is below %s%s"
                                  % (label(column, name), tier, ratio, need,
                                     "" if registered is None else " (registered preset floor)"))
            if registered is not None and registered >= tier_min - EPS:
                violations.append("C6 %s: preset floor %.3f is not below the %s tier floor %.1f, "
                                  "so it exempts nothing -- delete the row"
                                  % (label(column, name), registered, tier, tier_min))
            if registered is None and ratio + EPS < tier_min:
                violations.append("C6 %s sits at %.3f under the %s tier floor %.1f without a "
                                  "registered preset floor" % (label(column, name), ratio, tier, tier_min))
    for cell, row in floors.items():
        if cell not in seen_cells:
            violations.append("C6 preset floor %s/%s %s names no audited cell (style, scheme or pair drifted)"
                              % (cell[0], cell[1], cell[2]))
        elif row.get("reason") is None:
            violations.append("C6 preset floor %s/%s %s has no reason" % cell)


def normal_columns(matrix):
    """Shipped columns keyed by (style, scheme) -- the source every derived column diffs against.

    Args:
        matrix: emitted audit matrix.

    Returns:
        dict mapping the two color axes to their ``contrast == normal`` column.
    """
    return {(column.get("style"), column.get("scheme")): column
            for column in matrix.get("columns") or [] if column.get("contrast") == "normal"}


def moved_roles(column, source):
    """Role names whose emitted value differs between a derived column and its shipped counterpart.

    Args:
        column: derived (``contrast == high``) column.
        source: shipped column of the same two axes, or ``{}`` when there is none.

    Returns:
        Sorted list of differing role tokens; empty means the switch left this column alone.
    """
    derived_roles = column.get("roles") or {}
    shipped_roles = source.get("roles") or {}
    return sorted(role for role in derived_roles if derived_roles[role] != shipped_roles.get(role))


def check_derived(matrix, violations):
    """C9: the switch's own output -- bare tier floors, no exemption, and no unwarranted re-colouring.

    The high_contrast level is what the accessibility switch claims to produce, so the two things worth
    checking are that it actually clears the criteria and that it only touches columns that needed it.
    The second half is what keeps the derivation honest: a palette that already conforms must come out
    unchanged (bit-for-bit), otherwise the switch is re-designing the theme rather than fixing contrast,
    and a column that does not conform must come out changed, otherwise the switch silently gave up.

    Args:
        matrix: emitted audit matrix.
        violations: violation sink.
    """
    tier_min = {name: value[0] for name, value in PINNED_TIERS.items()}
    shipped = normal_columns(matrix)
    moved_any = False
    for column in matrix.get("columns") or []:
        if column.get("contrast") != "high":
            continue
        source = shipped.get((column.get("style"), column.get("scheme")))
        if source is None:
            add(violations, "C9 %s has no normal column to derive from" % label(column))
            continue
        unmet = [row for row in source.get("pairs") or []
                 if row.get("tier") in ("text", "non_text")
                 and float(row.get("ratio", 0.0)) + EPS < tier_min[row["tier"]]]
        for pair in column.get("pairs") or []:
            tier = pair.get("tier")
            if pair.get("registered_floor") is not None:
                violations.append("C9 %s carries a registered floor: the derived level is judged by the "
                                  "bare tier, exemptions belong to the shipped column"
                                  % label(column, pair.get("pair")))
            if tier not in ("text", "non_text"):
                continue
            ratio = float(pair.get("ratio", 0.0))
            if ratio + EPS < tier_min[tier]:
                violations.append("C9 %s tier=%s ratio %.3f is below the %s tier floor %.1f "
                                  "(the switch did not reach the criterion it claims to enforce)"
                                  % (label(column, pair.get("pair")), tier, ratio, tier, tier_min[tier]))
        moved = moved_roles(column, source)
        if moved:
            moved_any = True
        if unmet and not moved:
            violations.append("C9 %s leaves all %d cell(s) below their tier floor untouched -- the "
                              "derivation is a no-op for a column that needs one"
                              % (label(column), len(unmet)))
        if not unmet and moved:
            violations.append("C9 %s re-colours %d role(s) of a column whose shipped values already "
                              "clear every tier (the switch must fix contrast, not redesign a palette): %s"
                              % (label(column), len(moved), ", ".join(moved)))
    if not moved_any:
        violations.append("C9 no derived column differs from its shipped counterpart: the switch changes "
                          "nothing, so every C9 ratio assertion above would pass vacuously")


def check_reasons(matrix, violations):
    """C7: every row carries a non-empty ASCII reason (the registration is the point)."""
    def audit(label, item, field="reason"):
        reason = item.get(field)
        if not isinstance(reason, str) or not reason.strip():
            violations.append("C7 %s registers no %s" % (label, field))
            return
        try:
            reason.encode("ascii")
        except UnicodeEncodeError:
            violations.append("C7 %s %s is not ASCII (gate output and the matrix must stay ASCII)" % (label, field))

    for row in matrix.get("tiers") or []:
        audit("tier '%s'" % row.get("tier"), row, "criterion")
    for row in matrix.get("preset_floors") or []:
        audit("preset floor %s/%s/%s %s" % (row.get("style"), row.get("scheme"),
                                            row.get("contrast"), row.get("pair")), row)
    for column in matrix.get("columns") or []:
        for row in column.get("pairs") or []:
            audit(label(column, row.get("pair")), row)
            if row.get("tier") == "not_applicable" and not (row.get("reason") or "").strip():
                add(violations, "C7 %s is skipped by the ratio leg without a reason"
                    % label(column, row.get("pair")))


def check_spec(matrix, spec_text, violations):
    """C8: section 5.1 states the same ruling the tables encode."""
    if spec_text is None:
        return
    lines = spec_text.splitlines()
    for tier, (ratio, criterion) in PINNED_TIERS.items():
        if tier == "not_applicable":
            continue
        spelling = "%.1f:1" % ratio
        # Both halves must sit on the same line: the tier bullet is the sentence that declares the
        # floor. A section-wide substring search lets the bullet drift to 4.6:1 while some prose
        # elsewhere still spells 4.5:1, which is exactly the divergence this leg exists to catch.
        if any(criterion in line and spelling in line for line in lines):
            continue
        if any(criterion in line for line in lines):
            violations.append("C8 spec section 5.1 names '%s' (tier %s) on a line that does not give "
                              "the ratio '%s'" % (criterion, tier, spelling))
        elif spelling in spec_text:
            violations.append("C8 spec section 5.1 states the %s ratio '%s' only outside the line "
                              "declaring '%s'" % (tier, spelling, criterion))
        else:
            violations.append("C8 spec section 5.1 does not state '%s' (tier %s)" % (criterion, tier))
    for marker in (MARKER_DEFAULT_TIER, MARKER_REQUIRING_REASON, MARKER_DERIVED_NO_FLOOR):
        if marker not in spec_text:
            escaped = marker.encode("unicode_escape").decode("ascii")
            violations.append("C8 spec section 5.1 lost the ruling sentence containing %s" % escaped)
    for column in matrix.get("columns") or []:
        for row in column.get("pairs") or []:
            if row.get("tier") != "not_applicable":
                continue
            role = (row.get("pair") or "/").split("/")[0]
            if "`%s`" % role not in spec_text:
                add(violations, "C8 role '%s' is exempted by the tables but not listed in spec "
                                "section 5.1" % role)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Judge the color-role contrast tiers.")
    parser.add_argument("build_dir", nargs="?", default="build", help="directory holding gen_contrast_tools")
    parser.add_argument("--root", default=props_scan.repo_root_of(__file__), help="aurora repository root")
    parser.add_argument("--verbose", action="store_true", help="print the judged matrix summary")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    build = args.build_dir if os.path.isabs(args.build_dir) else os.path.join(root, args.build_dir)
    violations = []

    exe = resolve_exe(build, "gen_contrast_tools")
    if exe is None:
        print("FAIL C0 gen_contrast_tools not found under %s" % build)
        print("FAIL build it first: cmake --build %s --target gen_contrast_tools" % build)
        return 1
    matrix, error = parse_matrix(exe)
    if error is not None:
        for line in error.splitlines():
            print("FAIL %s" % line)
        return 1

    policy_path = os.path.join(root, POLICY.replace("/", os.sep))
    if not os.path.isfile(policy_path):
        print("FAIL C0 missing registry table: %s" % POLICY)
        return 1
    policy_text = props_scan.strip_comments(read(policy_path))

    styles = style_enum(root, violations)
    spec_text = spec_section(root, violations)
    role_names = token_table_roles(root, violations)

    check_table_shape(matrix, policy_text, violations)
    if styles:
        check_columns(matrix, styles, violations)
    if check_coverage(matrix, role_names, violations) is not None:
        check_tier_table(matrix, violations)
        check_ratios(matrix, violations)
        check_derived(matrix, violations)
    check_reasons(matrix, violations)
    check_spec(matrix, spec_text, violations)

    if args.verbose:
        print("matrix: %s audited pair(s) x %s column(s), %s preset floor(s)"
              % (matrix.get("audit_count"), matrix.get("column_count"), len(matrix.get("preset_floors") or [])))
        for column in matrix.get("columns") or []:
            judged = [row for row in column["pairs"] if row["tier"] != "not_applicable"]
            if not judged:
                continue
            worst = min(judged, key=lambda row: row["ratio"] - (row["registered_floor"] or PINNED_TIERS[row["tier"]][0]))
            print("  %-8s %-6s %-7s tightest cell %s = %.3f against %s"
                  % (column["style"], column["scheme"], column["contrast"], worst["pair"], worst["ratio"],
                     worst["registered_floor"] or PINNED_TIERS[worst["tier"]][0]))

    if violations:
        for item in violations:
            print("FAIL %s" % item)
        print("FAIL %d contrast violation(s)" % len(violations))
        return 1

    judged = sum(1 for column in matrix.get("columns") or [] for row in column.get("pairs") or []
                 if row.get("tier") != "not_applicable")
    print("OK contrast: %d judged cell(s) (%d exempt not_applicable), %d registered preset floor(s), "
          "tiers pinned to WCAG 4.5 / 3.0"
          % (judged,
             sum(1 for column in matrix.get("columns") or [] for row in column.get("pairs") or []
                 if row.get("tier") == "not_applicable"),
             len(matrix.get("preset_floors") or [])))
    derived = [column for column in matrix.get("columns") or [] if column.get("contrast") == "high"]
    shipped = normal_columns(matrix)
    print("OK derived: %d high_contrast column(s) judged against the bare tiers, %d of them changed "
          "their shipped roles"
          % (len(derived), sum(1 for column in derived
                                if moved_roles(column, shipped.get((column.get("style"),
                                                                     column.get("scheme")), {})))))
    print("OK registry table, emitted matrix and spec section 5.1 agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
