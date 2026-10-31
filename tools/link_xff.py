#!/usr/bin/env python3
"""Link the PAL SCES-53326 EE XFF modules into local, inspectable sections."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from xff2 import XffFormatError, XffLinker, XffModule

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "game" / "profiles" / "SCES-53326_v1.00.json"


def parse_address(value: str | int) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def link_disc_modules(files_dir: Path, profile_path: Path, output_dir: Path, undefined_stub: int | None = None) -> dict:
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    modules: list[tuple[XffModule, int, int]] = []
    for spec in profile["ee_modules"]:
        path = files_dir / spec["path"]
        raw = path.read_bytes()
        if len(raw) != spec["size_bytes"]:
            raise XffFormatError(f"{spec['path']}: expected {spec['size_bytes']} bytes, found {len(raw)}")
        actual_hash = sha256(raw)
        if actual_hash.lower() != spec["sha256"].lower():
            raise XffFormatError(f"{spec['path']}: SHA-256 mismatch")
        modules.append((XffModule(spec["name"], raw), parse_address(spec["load_base"]), parse_address(spec["bss"])))

    stub = undefined_stub if undefined_stub is not None else parse_address(profile.get("undefined_import_stub", "0x001B28F8"))
    linker = XffLinker(stub)
    linked = linker.link_in_order(modules)

    output_dir.mkdir(parents=True, exist_ok=True)
    section_dir = output_dir / "sections"
    section_dir.mkdir(parents=True, exist_ok=True)
    for old_file in section_dir.glob("*.bin"):
        old_file.unlink()
    manifest_modules = []
    for item in linked:
        module_sections = []
        for section in item.module.sections:
            if not section.is_allocated or not section.has_file_data:
                continue
            assert section.image_offset is not None
            section_data = bytes(item.image[section.image_offset:section.image_offset + section.size])
            name = f"{item.module.name.lower()}_{section.name.lstrip('.').replace('/', '_')}.bin"
            (section_dir / name).write_bytes(section_data)
            module_sections.append({
                "name": section.name,
                "guest_address": f"0x{item.section_addresses[section.index]:08X}",
                "size": section.size,
                "flags": section.flags,
                "sha256_after_relocation": sha256(section_data),
                "local_file": f"sections/{name}",
            })
        manifest_modules.append({
            "name": item.module.name,
            "base": f"0x{item.base:08X}",
            "entry": f"0x{item.entry_address:08X}",
            "bss_base": f"0x{item.bss_base:08X}",
            "sections": module_sections,
            "symbol_count": len(item.module.symbols),
            "relocation_table_count": len(item.module.relocation_tables),
            "unresolved_import_count": len(item.unresolved_imports),
        })

    relocation_output = {
        "format": "sotc-runtime-relocations-v1",
        "profile": f"{profile['serial']} v{profile['version']}",
        "undefined_import_stub": f"0x{stub:08X}",
        "entries": linker.unresolved_imports,
    }
    (output_dir / "runtime_relocations.json").write_text(json.dumps(relocation_output, indent=2) + "\n", encoding="utf-8")

    manifest = {
        "format": "sotc-xff-link-manifest-v1",
        "profile": f"{profile['serial']} v{profile['version']}",
        "modules": manifest_modules,
        "exports_resolved": len(linker.exports),
        "unresolved_imports": len(linker.unresolved_imports),
        "duplicate_exports": linker.duplicate_exports,
        "runtime_relocations": "runtime_relocations.json",
        "note": "XFF sections have been relocated. This is not yet the final merged ELF for PS2Recomp.",
    }
    (output_dir / "link_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", type=Path, required=True, help="directory containing the extracted XFF modules")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--out", type=Path, required=True, help="local output directory (ignored by Git)")
    parser.add_argument("--undefined-stub", type=lambda value: int(value, 0), help="override the loader's unresolved-import trap address")
    args = parser.parse_args()
    try:
        result = link_disc_modules(args.files, args.profile, args.out, args.undefined_stub)
    except (OSError, ValueError, KeyError, XffFormatError) as exc:
        print(f"XFF link failed: {exc}", file=sys.stderr)
        return 2
    print(f"Linked {len(result['modules'])} modules for {result['profile']}")
    print(f"Resolved exports: {result['exports_resolved']}; unresolved imports: {result['unresolved_imports']}")
    print(f"Local link output: {args.out}")
    print("Note: merged ELF generation and real-memory validation are the next linker increment.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
