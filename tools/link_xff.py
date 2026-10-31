#!/usr/bin/env python3
"""Link the PAL SCES-53326 boot ELF and its XFF modules for PS2Recomp."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import sys

try:
    from .xff2 import (
        R_MIPS_26,
        R_MIPS_32,
        R_MIPS_HI16,
        R_MIPS_LO16,
        SHN_UNDEF,
        XffFormatError,
        XffLinker,
        XffModule,
    )
    from .elf_writer import (
        load_boot_sections,
        sections_from_linked_modules,
        symbols_from_modules,
        write_elf32_mips,
        write_function_map,
        write_recompiler_config,
    )
except ImportError:  # direct execution as tools/link_xff.py
    from xff2 import (
        R_MIPS_26, R_MIPS_32, R_MIPS_HI16, R_MIPS_LO16, SHN_UNDEF,
        XffFormatError, XffLinker, XffModule,
    )
    from elf_writer import (
        load_boot_sections, sections_from_linked_modules, symbols_from_modules,
        write_elf32_mips, write_function_map, write_recompiler_config,
    )

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "game" / "profiles" / "SCES-53326_v1.00.json"
RUNTIME_KIND = {
    R_MIPS_26: "j26",
    R_MIPS_HI16: "hi16",
    R_MIPS_LO16: "lo16",
}


def parse_address(value: str | int) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def collect_runtime_import_sites(modules) -> list[dict]:
    """Keep every undefined-symbol relocation, including kinds PS2Recomp cannot currently consume."""
    kind_names = {
        R_MIPS_32: "w32",
        R_MIPS_26: "j26",
        R_MIPS_HI16: "hi16",
        R_MIPS_LO16: "lo16",
    }
    records: list[dict] = []
    for linked in modules:
        module = linked.module
        for table in module.relocation_tables:
            target = module.sections[table.target_section]
            if target.image_offset is None:
                continue
            target_address = linked.section_addresses.get(target.index)
            if target_address is None:
                continue
            for relocation in table.entries:
                symbol = module.symbols[relocation.symbol_index]
                if symbol.section_index != SHN_UNDEF:
                    continue
                image_position = target.image_offset + relocation.offset
                link_word = struct.unpack_from("<I", linked.image, image_position)[0]
                records.append({
                    "module": module.name,
                    "section": target.name,
                    "address": f"0x{target_address + relocation.offset:08X}",
                    "kind": kind_names.get(relocation.type, f"mips_{relocation.type}"),
                    "symbol": symbol.name,
                    "original_word": f"0x{relocation.original_word:08X}",
                    "link_time_word": f"0x{link_word:08X}",
                })
    records.sort(key=lambda item: (int(item["address"], 0), item["module"], item["section"]))
    return records


def write_runtime_relocation_csv(path: Path, modules) -> tuple[int, int]:
    """Emit only code-site kinds understood by the pinned PS2Recomp runtime-relocation parser."""
    rows: list[tuple[int, str, str, str]] = []
    unsupported_word_relocations = 0
    for linked in modules:
        module = linked.module
        for table in module.relocation_tables:
            target = module.sections[table.target_section]
            if target.name != ".text":
                continue
            target_address = linked.section_addresses.get(target.index)
            if target_address is None:
                continue
            for relocation in table.entries:
                symbol = module.symbols[relocation.symbol_index]
                if symbol.section_index != SHN_UNDEF:
                    continue
                kind = RUNTIME_KIND.get(relocation.type)
                if kind is None:
                    if relocation.type == R_MIPS_32:
                        unsupported_word_relocations += 1
                    continue
                rows.append((
                    target_address + relocation.offset,
                    kind,
                    module.name,
                    symbol.name,
                ))
    rows.sort(key=lambda row: (row[0], row[1], row[2], row[3]))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["address", "kind", "module", "symbol"])
        for address, kind, module, symbol in rows:
            writer.writerow([f"0x{address:08X}", kind, module, symbol])
    return len(rows), unsupported_word_relocations


def link_disc_modules(files_dir: Path, profile_path: Path, output_dir: Path,
                      undefined_stub: int | None = None) -> dict:
    profile = json.loads(profile_path.read_text(encoding="utf-8"))

    boot_spec = profile["boot_elf"]
    boot_path = files_dir / boot_spec["path"]
    boot = boot_path.read_bytes()
    if len(boot) != boot_spec["size_bytes"]:
        raise XffFormatError(f"{boot_spec['path']}: expected {boot_spec['size_bytes']} bytes, found {len(boot)}")
    if sha256(boot).lower() != boot_spec["sha256"].lower():
        raise XffFormatError(f"{boot_spec['path']}: SHA-256 mismatch")
    boot_entry, boot_flags, boot_sections = load_boot_sections(boot)

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
            if not section.is_allocated or section.size == 0:
                continue
            name = f"{item.module.name.lower()}_{section.name.lstrip('.').replace('/', '_')}.bin"
            local_file = None
            section_digest = None
            if section.has_file_data:
                assert section.image_offset is not None
                section_data = bytes(item.image[section.image_offset:section.image_offset + section.size])
                (section_dir / name).write_bytes(section_data)
                local_file = f"sections/{name}"
                section_digest = sha256(section_data)
            module_sections.append({
                "name": section.name,
                "guest_address": f"0x{item.section_addresses[section.index]:08X}",
                "size": section.size,
                "flags": section.flags,
                "nobits": section.type == 8,
                "sha256_after_relocation": section_digest,
                "local_file": local_file,
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

    merged_sections = boot_sections + sections_from_linked_modules(linked)
    merged_symbols = symbols_from_modules(linked, merged_sections)
    elf_path = output_dir / "sotc_linked.elf"
    write_elf32_mips(elf_path, boot_entry, boot_flags, merged_sections, merged_symbols)
    function_map_path = output_dir / "sotc_functions.csv"
    function_count = write_function_map(function_map_path, merged_sections, merged_symbols)

    runtime_csv_path = output_dir / "runtime_relocations.csv"
    runtime_site_count, unsupported_word_relocations = write_runtime_relocation_csv(runtime_csv_path, linked)
    runtime_import_sites = collect_runtime_import_sites(linked)
    runtime_import_sites_path = output_dir / "runtime_import_sites.json"
    runtime_import_sites_path.write_text(json.dumps({
        "format": "sotc-runtime-import-sites-v1",
        "profile": f"{profile['serial']} v{profile['version']}",
        "entries": runtime_import_sites,
    }, indent=2) + "\n", encoding="utf-8")

    relocation_output = {
        "format": "sotc-runtime-relocations-v1",
        "profile": f"{profile['serial']} v{profile['version']}",
        "undefined_import_stub": f"0x{stub:08X}",
        "entries": linker.unresolved_imports,
        "note": "This JSON lists unresolved imports at static link time; runtime_relocations.csv separately lists all code import sites for later data-module rebinding.",
    }
    (output_dir / "runtime_relocations.json").write_text(json.dumps(relocation_output, indent=2) + "\n", encoding="utf-8")
    write_recompiler_config(
        output_dir / "sotc_recomp.toml",
        elf_path.name,
        function_map_path.name,
        runtime_csv_path.name,
    )

    manifest = {
        "format": "sotc-xff-link-manifest-v2",
        "profile": f"{profile['serial']} v{profile['version']}",
        "boot_entry": f"0x{boot_entry:08X}",
        "merged_elf": elf_path.name,
        "merged_elf_size": elf_path.stat().st_size,
        "merged_elf_sha256": sha256(elf_path.read_bytes()),
        "function_map": function_map_path.name,
        "named_function_count": function_count,
        "runtime_relocations": runtime_csv_path.name,
        "runtime_relocation_site_count": runtime_site_count,
        "runtime_import_sites": runtime_import_sites_path.name,
        "runtime_import_site_count": len(runtime_import_sites),
        "runtime_relocation_r_mips_32_not_supported_by_ps2recomp": unsupported_word_relocations,
        "modules": manifest_modules,
        "exports_resolved": len(linker.exports),
        "unresolved_imports": len(linker.unresolved_imports),
        "duplicate_exports": linker.duplicate_exports,
        "note": "Merged ELF and PS2Recomp config are generated locally. Real-disc relocation fidelity and RAM-dump comparison are still required.",
    }
    (output_dir / "link_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", type=Path, required=True, help="directory containing the extracted boot ELF and XFF modules")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--out", type=Path, required=True, help="local output directory (ignored by Git)")
    parser.add_argument("--undefined-stub", type=lambda value: int(value, 0), help="override the loader's unresolved-import trap address")
    args = parser.parse_args()
    try:
        result = link_disc_modules(args.files, args.profile, args.out, args.undefined_stub)
    except (OSError, ValueError, KeyError, struct.error, XffFormatError) as exc:
        print(f"XFF/ELF link failed: {exc}", file=sys.stderr)
        return 2
    print(f"Linked {len(result['modules'])} modules for {result['profile']}")
    print(f"ELF: {result['merged_elf']} ({result['merged_elf_size']} bytes)")
    print(f"Named functions: {result['named_function_count']}; runtime relocation sites: {result['runtime_relocation_site_count']}")
    print(f"Unresolved imports: {result['unresolved_imports']}; R_MIPS_32 code sites omitted from PS2Recomp's runtime map: {result['runtime_relocation_r_mips_32_not_supported_by_ps2recomp']}")
    print(f"Local output: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
