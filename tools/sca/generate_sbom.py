#!/usr/bin/env python3
# ============================================================================
# generate_sbom.py - emit a CycloneDX SBOM from the vendored version inventory
# ----------------------------------------------------------------------------
# The repository has no package manifest, so syft/grype/CDX generators scanning
# for lockfiles would emit an empty BOM. The authoritative input is
# codespec/third_party_versions.json, which check_third_party_versions.py keeps
# byte-honest against the vendored sources. This script translates it into a
# CycloneDX 1.6 JSON document so the shipped dependency set is machine-readable
# by downstream consumers (artifact on each release / weekly scan).
#
# version_kind == "proxy" is preserved as an SBOM property: the value is a
# stand-in identifier, not an upstream release number. Consumers must not treat
# it as a comparable version.
#
# Output is deterministic (no timestamps, no random serial) so the same tree
# always produces byte-identical bytes. Python 3.8+, standard library only.
# Output is all ASCII.
#
# Usage: generate_sbom.py --root <repo> --out <cdx.json>
# Exit:  0 written, 1 inventory error, 2 usage/IO error.
# ============================================================================
import argparse
import json
import sys
from pathlib import Path

INVENTORY_REL = "codespec/third_party_versions.json"
SPEC_VERSION = "1.6"
SERIAL = "urn:uuid:00000000-0000-4000-8000-000000000000"


def license_object(spdx):
    if not spdx or " OR " in spdx or " " in spdx:
        return [{"license": {"name": spdx or "see THIRD_PARTY_LICENSES.md"}}]
    return [{"license": {"id": spdx}}]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    root = Path(args.root).resolve()
    data = json.loads((root / INVENTORY_REL).read_text(encoding="utf-8"))
    entries = data.get("dependencies", [])
    if not entries:
        print("[FAIL] inventory has zero dependencies")
        return 1

    components = []
    for e in entries:
        name = e.get("name", "")
        version = str(e.get("version", ""))
        if not name or not version:
            print(f"[FAIL] incomplete entry: {name!r}")
            return 1
        props = [
            {"name": "aurora:source", "value": e.get("source", "")},
            {"name": "aurora:versionKind", "value": e.get("version_kind", "")},
            {"name": "aurora:upstream", "value": e.get("upstream", "")},
        ]
        if e.get("note"):
            props.append({"name": "aurora:note", "value": e["note"]})
        components.append({
            "type": "library",
            "name": name,
            "version": version,
            "purl": f"pkg:generic/{name}@{version}",
            "licenses": license_object(e.get("license", "")),
            "properties": props,
        })
    components.sort(key=lambda c: c["name"])

    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": SPEC_VERSION,
        "serialNumber": SERIAL,
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "aurora",
                "scope": "required",
            },
            "properties": [
                {"name": "aurora:sbomSource", "value": INVENTORY_REL},
                {"name": "aurora:verifiedBy", "value": "check_third_party_versions"},
            ],
        },
        "components": components,
    }

    Path(args.out).write_text(json.dumps(bom, indent=2, sort_keys=False) + "\n",
                             encoding="utf-8")
    print(f"[OK] wrote CycloneDX {SPEC_VERSION} SBOM with {len(components)} components")
    return 0


if __name__ == "__main__":
    sys.exit(main())
