#!/usr/bin/env python3
"""Compare linked code/readonly ELF sections with a raw PS2 EE RAM dump."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys

SHT_NOBITS = 8
SHT_STRTAB = 3
SHF_ALLOC = 0x2


class RamVerificationError(ValueError):
    """Invalid ELF, RAM dump, or relocation-site report."""


def _read_c_string(data: bytes, offset: int, limit: int) -> str:
    if offset < 0 or offset >= limit or limit > len(data):
        raise RamVerificationError("ELF section name index is out of bounds")
    end = data.find(b"\0", offset, limit)
    if end < 0:
        raise RamVerificationError("ELF section name is not terminated")
    return data[offset:end].decode("utf-8", "replace")


def _is_reference_section(name: str) -> bool:
    # Match the reference port's byte-for-byte checks: EE text, read-only
    # constants, and VU microprogram text. Mutable .data and BSS are omitted.
    return (
        name == ".boot.text"
        or name.startswith(".text.")
        or name.endswith(".rodata")
        or name.startswith(".vutext.")
    )


def compare_elf_to_ram(elf: bytes, ram: bytes, ignored_sites: set[int] | None = None,
                       max_diffs: int = 32) -> dict:
    ignored_sites = ignored_sites or set()
    if len(elf) < 52 or elf[:4] != b"\x7fELF" or elf[4:7] != b"\x01\x01\x01":
        raise RamVerificationError("input is not an ELF32 little-endian image")
    values = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
    if values[1] != 8:
        raise RamVerificationError("ELF machine is not MIPS")
    shoff, shentsize, shnum, shstrndx = values[5], values[10], values[11], values[12]
    if shentsize < 40 or shnum == 0 or shstrndx >= shnum:
        raise RamVerificationError("ELF section-header table is invalid")
    if shoff > len(elf) or shnum > (len(elf) - shoff) // shentsize:
        raise RamVerificationError("ELF section-header table is out of bounds")

    headers = [
        struct.unpack_from("<10I", elf, shoff + index * shentsize)
        for index in range(shnum)
    ]
    strings = headers[shstrndx]
    if strings[1] != SHT_STRTAB or strings[4] > len(elf) or strings[5] > len(elf) - strings[4]:
        raise RamVerificationError("ELF section-name string table is invalid")
    names_data = elf[strings[4]:strings[4] + strings[5]]

    # Dynamic sites are word addresses. Expand to bytes so a section whose
    # segment start is not naturally aligned still masks the exact MIPS word.
    ignored_bytes = {byte for site in ignored_sites for byte in range(site, site + 4)}
    reports = []
    total_words = total_ignored = total_diffs = 0
    first_diffs = []

    for index, header in enumerate(headers):
        if index == 0:
            continue
        name = _read_c_string(names_data, header[0], len(names_data)) if header[0] else ""
        section_type, flags, address, offset, size = header[1], header[2], header[3], header[4], header[5]
        if not (flags & SHF_ALLOC) or section_type == SHT_NOBITS or size == 0 or not _is_reference_section(name):
            continue
        if offset > len(elf) or size > len(elf) - offset:
            raise RamVerificationError(f"{name}: ELF section range is out of bounds")
        if address > len(ram) or size > len(ram) - address:
            raise RamVerificationError(
                f"{name}: guest range 0x{address:08X}+0x{size:X} exceeds RAM dump ({len(ram)} bytes)"
            )

        expected = elf[offset:offset + size]
        actual = ram[address:address + size]
        compared = ignored = different = 0
        for pos in range(0, size, 4):
            width = min(4, size - pos)
            guest_address = address + pos
            if any((guest_address + byte) in ignored_bytes for byte in range(width)):
                ignored += 1
                continue
            compared += 1
            if expected[pos:pos + width] != actual[pos:pos + width]:
                different += 1
                if len(first_diffs) < max_diffs:
                    first_diffs.append({
                        "section": name,
                        "address": f"0x{guest_address:08X}",
                        "elf": expected[pos:pos + width].hex(),
                        "ram": actual[pos:pos + width].hex(),
                    })
        total_words += compared
        total_ignored += ignored
        total_diffs += different
        reports.append({
            "name": name, "address": f"0x{address:08X}", "size": size,
            "compared_words": compared, "ignored_dynamic_words": ignored,
            "different_words": different,
        })

    if not reports:
        raise RamVerificationError("ELF contains no allocated .text/.rodata/.vutext sections to compare")
    return {
        "passed": total_diffs == 0,
        "sections": reports,
        "compared_words": total_words,
        "ignored_dynamic_relocation_words": total_ignored,
        "different_words": total_diffs,
        "first_differences": first_diffs,
    }


def load_ignored_sites(path: Path | None) -> set[int]:
    if path is None:
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    entries = data.get("entries")
    if not isinstance(entries, list):
        raise RamVerificationError("runtime import-site file has no entries array")
    return {int(entry["address"], 0) for entry in entries if "address" in entry}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="link output directory containing sotc_linked.elf")
    parser.add_argument("--ram", type=Path, required=True, help="raw PS2 EE RAM dump starting at guest address 0")
    parser.add_argument("--imports", type=Path, help="runtime_import_sites.json; masks dynamic import words")
    parser.add_argument("--max-diffs", type=int, default=32)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        elf = (args.out / "sotc_linked.elf").read_bytes()
        ram = args.ram.read_bytes()
        imports_path = args.imports if args.imports is not None else args.out / "runtime_import_sites.json"
        ignored = load_ignored_sites(imports_path if imports_path.exists() else None)
        report = compare_elf_to_ram(elf, ram, ignored, args.max_diffs)
    except (OSError, ValueError, KeyError, struct.error) as exc:
        print(f"RAM verification failed: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"Compared {report['compared_words']:,} words across {len(report['sections'])} sections")
        print(f"Ignored dynamic import sites: {report['ignored_dynamic_relocation_words']:,}")
        print(f"Differing words: {report['different_words']:,}")
        for diff in report["first_differences"]:
            print(f"  {diff['section']} {diff['address']}: ELF={diff['elf']} RAM={diff['ram']}")
        print("RAM verification: " + ("PASS" if report["passed"] else "MISMATCH"))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
