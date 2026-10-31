#!/usr/bin/env python3
"""Build a merged ELF32/MIPS image from the boot ELF and relocated XFF modules."""
from __future__ import annotations

import csv
from dataclasses import dataclass
import struct
from pathlib import Path
from typing import Iterable

try:
    from .xff2 import (
        SHN_ABS,
        SHN_UNDEF,
        SHT_NOBITS,
        SHT_PROGBITS,
        STB_LOCAL,
        STT_FUNC,
        STT_SECTION,
        XffFormatError,
        XffModule,
        LinkedModule,
    )
except ImportError:  # direct execution from the tools directory
    from xff2 import (
        SHN_ABS, SHN_UNDEF, SHT_NOBITS, SHT_PROGBITS, STB_LOCAL, STT_FUNC,
        STT_SECTION, XffFormatError, XffModule, LinkedModule,
    )

ELF_HEADER_SIZE = 52
ELF_PROGRAM_HEADER_SIZE = 32
ELF_SECTION_HEADER_SIZE = 40
ELFCLASS32 = 1
ELFDATA2LSB = 1
ET_EXEC = 2
EM_MIPS = 8
EV_CURRENT = 1
PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHN_XINDEX = 0xFFFF


@dataclass(frozen=True)
class ImageSection:
    name: str
    address: int
    data: bytes | None
    size: int
    type: int
    flags: int
    alignment: int = 1

    @property
    def is_code(self) -> bool:
        if not (self.flags & SHF_EXECINSTR):
            return False
        # PS2Recomp knows these ABI sections contain VU instructions, not EE/R5900 code.
        return not (
            self.name == ".vutext" or self.name.startswith(".vutext.")
            or self.name == ".DVP.overlay" or self.name.startswith(".DVP.overlay.")
        )


@dataclass(frozen=True)
class ImageSymbol:
    name: str
    value: int
    size: int
    binding: int
    type: int
    section_index: int


def _check_range(data: bytes | bytearray, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise XffFormatError(f"{label}: range 0x{offset:X}+0x{size:X} is outside the file")


def _read_c_string(data: bytes, base: int, index: int, label: str) -> str:
    offset = base + index
    if offset < base or offset >= len(data):
        raise XffFormatError(f"{label}: string index is outside the string table")
    end = data.find(b"\0", offset)
    if end < 0:
        raise XffFormatError(f"{label}: unterminated string")
    return data[offset:end].decode("latin-1")


def _power_of_two_alignment(value: int) -> int:
    if value <= 1:
        return 1
    if value & (value - 1):
        # ELF p_align must be a power of two. A malformed/non-standard section
        # alignment is represented conservatively rather than causing huge padding.
        return 1
    return min(value, 0x1000)


def _align_congruent(offset: int, alignment: int, address: int) -> int:
    if alignment <= 1:
        return offset
    remainder = offset % alignment
    wanted = address % alignment
    return offset + ((wanted - remainder) % alignment)


def load_boot_sections(elf: bytes) -> tuple[int, int, list[ImageSection]]:
    """Read allocated sections from the stripped retail boot ELF."""
    if len(elf) < ELF_HEADER_SIZE or elf[:4] != b"\x7fELF":
        raise XffFormatError("boot image is not an ELF file")
    if elf[4] != ELFCLASS32 or elf[5] != ELFDATA2LSB or elf[6] != EV_CURRENT:
        raise XffFormatError("boot image must be ELF32 little-endian")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
    e_type, e_machine, e_version, entry, _phoff, _shoff, flags, _ehsize, _phentsize, _phnum, shentsize, shnum, shstrndx = fields
    if e_type != ET_EXEC or e_machine != EM_MIPS or e_version != EV_CURRENT:
        raise XffFormatError("boot ELF is not a supported MIPS executable")
    if shentsize < ELF_SECTION_HEADER_SIZE or shnum == 0 or shstrndx >= shnum:
        raise XffFormatError("boot ELF has an invalid section table")
    shoff = struct.unpack_from("<I", elf, 32)[0]
    _check_range(elf, shoff, shentsize * shnum, "boot ELF section header table")

    headers = [
        struct.unpack_from("<10I", elf, shoff + i * shentsize)
        for i in range(shnum)
    ]
    shstr = headers[shstrndx]
    if shstr[1] != 3:
        raise XffFormatError("boot ELF section-name table is not STRTAB")
    _check_range(elf, shstr[4], shstr[5], "boot ELF section-name table")
    sections: list[ImageSection] = []
    for index, sh in enumerate(headers):
        name = _read_c_string(elf, shstr[4], sh[0], "boot ELF section name") if sh[0] else ""
        section_type, section_flags, address, offset, size, alignment = sh[1], sh[2], sh[3], sh[4], sh[5], sh[8]
        if index == 0 or not (section_flags & SHF_ALLOC) or size == 0:
            continue
        if section_type == SHT_NOBITS:
            content = None
        else:
            _check_range(elf, offset, size, f"boot ELF {name}")
            content = elf[offset:offset + size]
        sections.append(ImageSection(
            name=f".boot.{name.lstrip('.')}",
            address=address,
            data=content,
            size=size,
            type=section_type,
            flags=section_flags,
            alignment=max(1, alignment),
        ))
    if not sections:
        raise XffFormatError("boot ELF contains no allocated sections")
    if not any(section.is_code and section.address <= entry < section.address + section.size for section in sections):
        raise XffFormatError("boot ELF entry point is not inside an executable section")
    return entry, flags, sections


def sections_from_linked_modules(modules: Iterable[LinkedModule]) -> list[ImageSection]:
    output: list[ImageSection] = []
    for linked in modules:
        module = linked.module
        prefix = module.name.lower()
        for section in module.sections:
            if not section.is_allocated or section.size == 0:
                continue
            address = linked.section_addresses.get(section.index)
            if address is None:
                raise XffFormatError(f"{module.name}:{section.name}: allocated section has no runtime address")
            data: bytes | None
            if section.type == SHT_NOBITS:
                data = None
            elif section.has_file_data:
                assert section.image_offset is not None
                data = bytes(linked.image[section.image_offset:section.image_offset + section.size])
            else:
                raise XffFormatError(f"{module.name}:{section.name}: allocated section has no file data")
            lower = section.name.lower()
            if lower == ".text":
                name = f".text.{prefix}"
            elif lower == ".vutext":
                name = f".vutext.{prefix}"
            elif lower == ".dvp.overlay":
                name = f".DVP.overlay.{prefix}"
            elif lower == ".dvp.ovlytab":
                name = f".DVP.ovlytab.{prefix}"
            else:
                name = f".{prefix}{section.name}"
            output.append(ImageSection(
                name=name,
                address=address,
                data=data,
                size=section.size,
                type=section.type,
                flags=section.flags,
                alignment=max(1, section.alignment),
            ))
    return output


def symbols_from_modules(modules: Iterable[LinkedModule], sections: list[ImageSection]) -> list[ImageSymbol]:
    """Export defined XFF symbols, retaining absolute boot-ELF symbols."""
    output: list[ImageSymbol] = []
    # Each linked XFF section name maps to its final ELF section index, assigned by writer.
    for linked in modules:
        module = linked.module
        for symbol in module.symbols:
            if not symbol.name or symbol.type == STT_SECTION:
                continue
            if symbol.section_index == SHN_UNDEF:
                output.append(ImageSymbol(symbol.name, 0, symbol.size, symbol.binding, symbol.type, SHN_UNDEF))
                continue
            if symbol.section_index == SHN_ABS:
                output.append(ImageSymbol(symbol.name, symbol.value, symbol.size, symbol.binding, symbol.type, SHN_ABS))
                continue
            address = linked.symbol_addresses.get(symbol.index)
            if address is None:
                continue
            section = module.sections[symbol.section_index]
            lower = section.name.lower()
            if lower == ".text":
                out_name = f".text.{module.name.lower()}"
            elif lower == ".vutext":
                out_name = f".vutext.{module.name.lower()}"
            elif lower == ".dvp.overlay":
                out_name = f".DVP.overlay.{module.name.lower()}"
            elif lower == ".dvp.ovlytab":
                out_name = f".DVP.ovlytab.{module.name.lower()}"
            else:
                out_name = f".{module.name.lower()}{section.name}"
            match = next((i + 1 for i, item in enumerate(sections) if item.name == out_name and item.address <= address < item.address + item.size), None)
            if match is None:
                continue
            output.append(ImageSymbol(symbol.name, address, symbol.size, symbol.binding, symbol.type, match))
    return output


def _add_string(table: bytearray, text: str) -> int:
    encoded = text.encode("utf-8", "replace") + b"\0"
    offset = len(table)
    table.extend(encoded)
    return offset


def write_elf32_mips(path: Path, entry: int, e_flags: int,
                     sections: list[ImageSection], symbols: list[ImageSymbol]) -> dict[str, int]:
    if not sections:
        raise XffFormatError("cannot write an ELF without loadable sections")
    if any(not (section.flags & SHF_ALLOC) for section in sections):
        raise XffFormatError("all output image sections must be allocated guest-memory sections")
    section_names = [section.name for section in sections]
    if len(section_names) != len(set(section_names)):
        raise XffFormatError("merged ELF section names are not unique")
    for section in sections:
        if section.address < 0 or section.size < 0 or section.address + section.size > 0x100000000:
            raise XffFormatError(f"{section.name}: guest address range exceeds 32-bit space")
        if section.type != SHT_NOBITS and (section.data is None or len(section.data) != section.size):
            raise XffFormatError(f"{section.name}: section payload size does not match its header")
        if section.type == SHT_NOBITS and section.data is not None:
            raise XffFormatError(f"{section.name}: NOBITS section unexpectedly has file data")

    # ELF section indexes: 0 is NULL, then game sections, then SYMTAB/STRTAB/SHSTRTAB.
    section_index_by_name = {section.name: i + 1 for i, section in enumerate(sections)}
    symtab_index = len(sections) + 1
    strtab_index = symtab_index + 1
    shstrtab_index = strtab_index + 1
    shnum = shstrtab_index + 1
    phdr_sections = [section for section in sections if section.flags & SHF_ALLOC]
    phnum = len(phdr_sections)

    body = bytearray(ELF_HEADER_SIZE + phnum * ELF_PROGRAM_HEADER_SIZE)
    program_headers: list[tuple[int, ...]] = []
    output_shdrs: list[tuple[int, ...]] = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]

    for section in sections:
        alignment = _power_of_two_alignment(section.alignment)
        if section.type == SHT_NOBITS:
            file_offset = _align_congruent(len(body), alignment, section.address)
            if file_offset > len(body):
                body.extend(bytes(file_offset - len(body)))
            flags = PF_R | (PF_W if section.flags & SHF_WRITE else 0) | (PF_X if section.flags & SHF_EXECINSTR else 0)
            program_headers.append((PT_LOAD, file_offset, section.address, section.address, 0, section.size, flags, alignment))
        else:
            file_offset = _align_congruent(len(body), alignment, section.address)
            if file_offset > len(body):
                body.extend(bytes(file_offset - len(body)))
            assert section.data is not None
            body.extend(section.data)
            flags = PF_R | (PF_W if section.flags & SHF_WRITE else 0) | (PF_X if section.flags & SHF_EXECINSTR else 0)
            program_headers.append((PT_LOAD, file_offset, section.address, section.address, section.size, section.size, flags, alignment))
        output_shdrs.append((
            0, section.type, section.flags, section.address, file_offset, section.size,
            0, 0, max(1, section.alignment), 0
        ))

    # Build a standard ELF32 .symtab. Local symbols precede globals, as required by sh_info.
    sorted_symbols = sorted(symbols, key=lambda s: (s.binding != STB_LOCAL, s.value, s.name))
    string_table = bytearray(b"\0")
    symtab = bytearray(16)  # mandatory null symbol
    first_global = 1
    for symbol in sorted_symbols:
        if symbol.section_index not in (SHN_UNDEF, SHN_ABS) and not (1 <= symbol.section_index < shstrtab_index):
            raise XffFormatError(f"{symbol.name}: invalid output section index {symbol.section_index}")
        name_offset = _add_string(string_table, symbol.name)
        symtab.extend(struct.pack(
            "<IIIBBH",
            name_offset,
            symbol.value & 0xFFFFFFFF,
            symbol.size & 0xFFFFFFFF,
            ((symbol.binding & 0x0F) << 4) | (symbol.type & 0x0F),
            0,
            symbol.section_index,
        ))
    for index, symbol in enumerate(sorted_symbols, start=1):
        if symbol.binding != STB_LOCAL:
            first_global = index
            break
    body.extend(bytes(_align_congruent(len(body), 4, 0) - len(body)))
    symtab_offset = len(body)
    body.extend(symtab)
    strtab_offset = len(body)
    body.extend(string_table)

    shstrtab = bytearray(b"\0")
    section_name_offsets: list[int] = []
    for section in sections:
        section_name_offsets.append(_add_string(shstrtab, section.name))
    symtab_name_offset = _add_string(shstrtab, ".symtab")
    strtab_name_offset = _add_string(shstrtab, ".strtab")
    shstrtab_name_offset = _add_string(shstrtab, ".shstrtab")
    shstrtab_offset = len(body)
    body.extend(shstrtab)

    output_shdrs[1:] = [
        (name_off, sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7], sh[8], sh[9])
        for name_off, sh in zip(section_name_offsets, output_shdrs[1:])
    ]
    output_shdrs.append((symtab_name_offset, 2, 0, 0, symtab_offset, len(symtab), strtab_index, first_global, 4, 16))
    output_shdrs.append((strtab_name_offset, 3, 0, 0, strtab_offset, len(string_table), 0, 0, 1, 0))
    output_shdrs.append((shstrtab_name_offset, 3, 0, 0, shstrtab_offset, len(shstrtab), 0, 0, 1, 0))

    # Place the section header table after all string/symbol data.
    shoff = len(body)
    for header in output_shdrs:
        body.extend(struct.pack("<10I", *header))

    # The program-header array was reserved immediately after the ELF header.
    phoff = ELF_HEADER_SIZE
    for index, phdr in enumerate(program_headers):
        struct.pack_into("<8I", body, phoff + index * ELF_PROGRAM_HEADER_SIZE, *phdr)

    ident = b"\x7fELF" + bytes([ELFCLASS32, ELFDATA2LSB, EV_CURRENT, 0, 0]) + bytes(7)
    header = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        ET_EXEC, EM_MIPS, EV_CURRENT, entry & 0xFFFFFFFF, phoff, shoff, e_flags & 0xFFFFFFFF,
        ELF_HEADER_SIZE, ELF_PROGRAM_HEADER_SIZE, phnum,
        ELF_SECTION_HEADER_SIZE, shnum, shstrtab_index,
    )
    body[:ELF_HEADER_SIZE] = header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)
    return section_index_by_name


def write_function_map(path: Path, sections: list[ImageSection], symbols: list[ImageSymbol]) -> int:
    code_sections = [section for section in sections if section.is_code]
    candidates: dict[int, tuple[ImageSymbol, ImageSection]] = {}
    for symbol in symbols:
        if symbol.type != STT_FUNC or not symbol.name or symbol.value == 0:
            continue
        section = None
        if symbol.section_index == SHN_ABS:
            section = next((s for s in code_sections if s.address <= symbol.value < s.address + s.size), None)
        else:
            # Output section indexes are ordered as in write_elf32_mips().
            if 1 <= symbol.section_index <= len(sections):
                possible = sections[symbol.section_index - 1]
                if possible.is_code and possible.address <= symbol.value < possible.address + possible.size:
                    section = possible
        if section is None:
            continue
        old = candidates.get(symbol.value)
        if old is None or (old[0].size == 0 and symbol.size > 0):
            candidates[symbol.value] = (symbol, section)

    by_section: dict[str, list[tuple[int, ImageSymbol, ImageSection]]] = {}
    for address, (symbol, section) in candidates.items():
        by_section.setdefault(section.name, []).append((address, symbol, section))

    rows: list[tuple[str, int, int, int]] = []
    for entries in by_section.values():
        entries.sort(key=lambda item: item[0])
        for index, (start, symbol, section) in enumerate(entries):
            next_start = entries[index + 1][0] if index + 1 < len(entries) else section.address + section.size
            end = start + symbol.size if symbol.size else next_start
            end = min(end, next_start, section.address + section.size)
            if end <= start:
                continue
            name = symbol.name.replace(",", "_").replace("\r", "_").replace("\n", "_")
            rows.append((name, start, end, end - start))

    rows.sort(key=lambda row: (row[1], row[0]))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["name", "start", "end", "size"])
        for name, start, end, size in rows:
            writer.writerow([name, f"0x{start:08X}", f"0x{end:08X}", size])
    return len(rows)


def write_recompiler_config(path: Path, elf_path: str = "sotc_linked.elf",
                            map_path: str = "sotc_functions.csv",
                            reloc_path: str = "runtime_relocations.csv") -> None:
    """Write a PS2Recomp config using paths relative to this config file."""
    # The CSV currently contains named functions only. Do not feed it to
    # ghidra_output until anonymous function boundaries are also authoritative:
    # PS2Recomp filters heuristic starts when a Ghidra map is supplied.
    content = f'''[general]
input = "{elf_path}"
ghidra_output = ""
runtime_relocations = "{reloc_path}"
output = "generated"
single_file_output = false
output_worker_threads = 0
low_memory_mode = true
patch_syscalls = false
patch_cop0 = false
patch_cache = false
auto_stub_by_name = false
direct_tail_calls = false
stubs = []
skip = []
'''
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
