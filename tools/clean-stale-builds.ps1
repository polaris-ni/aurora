# clean-stale-builds.ps1 — Permanently remove stale Aurora build/test artifacts.
#
# WHY THIS EXISTS (root-cause fix):
#   Deleting build directories via Windows Explorer sends them to the Recycle
#   Bin, which silently accumulates hundreds of thousands of files. This script
#   uses Remove-Item, which is PERMANENT and bypasses the Recycle Bin entirely.
#   Always clean build dirs with this (or `rm -rf` / PowerShell `Remove-Item`),
#   never Explorer's Delete.
#
# USAGE:
#   pwsh ./tools/clean-stale-builds.ps1          # dry run
#   pwsh ./tools/clean-stale-builds.ps1 -Yes     # actually delete (irreversible)
param([switch]$Yes)

$Root = Resolve-Path (Join-Path $PSScriptRoot '..')
# Stale artifact patterns. `build/` (Release) and `cmake-build-debug/` (CLion
# default debug) are intentionally EXCLUDED. Edit freely.
$patterns = @('build-*', 'cmake-build-debug-*', 'test_temp')

Write-Host "Project root: $Root"
Write-Host "Mode: $(if ($Yes) { 'DELETE' } else { 'DRY-RUN' })"
Write-Host ""

$total = 0
foreach ($pat in $patterns) {
  $matches = Get-ChildItem -Path $Root -Filter $pat -Directory -Force
  foreach ($d in $matches) {
    if ($Yes) {
      Write-Host "RM       $($d.FullName)"
      Remove-Item -Recurse -Force $d.FullName
    }
    else {
      Write-Host "WOULD RM $($d.FullName)"
    }
    $total++
  }
}

Write-Host ""
Write-Host "Matched $total stale artifact(s)."
if (-not $Yes) {
  Write-Host "Dry run only — rerun with -Yes to actually delete."
}
else {
  Write-Host "Permanently deleted."
}
