#!/usr/bin/env bash
# clean-stale-builds.sh — Permanently remove stale Aurora build/test artifacts.
#
# WHY THIS EXISTS (root-cause fix):
#   Deleting build directories via Windows Explorer sends them to the Recycle
#   Bin, which silently accumulates hundreds of thousands of files. This script
#   uses `rm -rf`, so deletions are PERMANENT and bypass the Recycle Bin entirely
#   — nothing piles up. Always clean build dirs with this (or `rm -rf` /
#   PowerShell `Remove-Item`), never Explorer's Delete.
#
# USAGE:
#   ./tools/clean-stale-builds.sh          # dry run — lists what WOULD be removed
#   ./tools/clean-stale-builds.sh --yes    # actually delete (irreversible)
#
# SAFE BY DEFAULT: dry run unless --yes is passed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ACTUAL_DELETE=0
for a in "$@"; do
  [ "$a" = "--yes" ] && ACTUAL_DELETE=1
done

# Stale artifact patterns. The canonical `build/` (Release) and
# `cmake-build-debug/` (CLion default debug) are intentionally EXCLUDED so you
# never nuke the tree you are actively developing in. Edit this list freely.
STALE_PATTERNS=('build-*' 'cmake-build-debug-*' 'test_temp')

echo "Project root: $ROOT"
echo "Mode: $([ "$ACTUAL_DELETE" -eq 1 ] && echo DELETE || echo DRY-RUN)"
echo

shopt -s nullglob
TOTAL=0
for pat in "${STALE_PATTERNS[@]}"; do
  for d in "$ROOT"/$pat; do
    if [ -e "$d" ]; then
      if [ "$ACTUAL_DELETE" -eq 1 ]; then
        echo "RM       $d"
        rm -rf "$d"
      else
        echo "WOULD RM $d"
      fi
      TOTAL=$((TOTAL + 1))
    fi
  done
done
shopt -u nullglob

echo
echo "Matched $TOTAL stale artifact(s)."
if [ "$ACTUAL_DELETE" -eq 1 ]; then
  echo "Permanently deleted."
else
  echo "Dry run only — rerun with --yes to actually delete."
fi
