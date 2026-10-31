#!/usr/bin/env python3
"""Bounds-checked parser and sequential linker for the game's xff2 EE modules.

The format is documented in docs/XFF_LINKER.md. This module deliberately handles
only the XFF2 container and the four relocation kinds used by this disc profile.
It does not execute game code or contain game data.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import struct
from typing import Iterable

MAGIC = b"xff2"
HEADER_SIZE = 0x70
POINTER_BIAS = 0x40010000

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_NOBITS = 8
SHT_REL = 9
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

SHN_UNDEF = 0
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2

STB_LOCAL = 0
STB_GLOBAL = 1
STB_WEAK = 2
STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3

R_MIPS_NONE = 0
R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
SUPPORTED_RELOCATIONS = {R_MIPS_NONE, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16}


class XffFormatError(ValueError):
    """The XFF2 container is truncated, inconsistent, or unsupported."""


def _u32(data: bytes | bytearray, offset: int, label: str) -> int:
    _check_range(data, offset, 4, label)
    return struct.unpack_from("<I", data, offset)[0]


def _check_range(data: bytes | bytearray, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise XffFormatError(f"{label}: file range 0x{offset:X}+0x{size:X} is out of bounds")


def _checked_array(data: bytes | bytearray, offset: int, count: int, stride: int, label: str) -> None:
    if count < 0 or stride < 0 or count > (len(data) // max(stride, 1)):
        raise XffFormatError(f"{label}: unreasonable element count {count}")
    _check_range(data, offset, count * stride, label)


def _c_string(data: bytes | bytearray, offset: int, limit: int, label: str) -> str:
    if offset < 0 or limit < offset or limit > len(data) or offset >= limit:
        raise XffFormatError(f"{label}: string offset 0x{offset:X} is out of bounds")
    end = data.find(b"\0", offset, limit)
    if end < 0:
        raise XffFormatError(f"{label}: unterminated string at 0x{offset:X}")
    return bytes(data[offset:end]).decode("latin-1")


def _align(value: int, alignment: int) -> int:
    alignment = max(1, alignment)
    return (value + alignment - 1) & ~(alignment - 1) if alignment & (alignment - 1) == 0 else ((value + alignment - 1) // alignment) * alignment


@dataclass(frozen=True)
class XffSection:
    index: int
    name: str
    runtime_address: int
    image_offset: int | None
    size: int
    alignment: int
    type: int
    flags: int
    file_offset: int

    @property
    def is_allocated(self) -> bool:
        return bool(self.flags & SHF_ALLOC)

    @property
    def has_file_data(self) -> bool:
        return self.type != SHT_NOBITS and self.image_offset is not None and self.size > 0


@dataclass(frozen=True)
class XffSymbol:
    index: int
    name: str
    value: int
    size: int
    binding: int
    type: int
    section_index: int

    @property
    def is_undefined(self) -> bool:
        return self.section_index == SHN_UNDEF

    @property
    def is_absolute(self) -> bool:
        return self.section_index == SHN_ABS

    @property
    def is_exported(self) -> bool:
        return bool(self.name) and self.binding != STB_LOCAL and not self.is_undefined


@dataclass(frozen=True)
class XffRelocation:
    offset: int
    symbol_index: int
    type: int
    original_word: int
    auxiliary_word: int


@dataclass(frozen=True)
class XffRelocationTable:
    type: int
    target_section: int
    entries: tuple[XffRelocation, ...]


class XffModule:
    """Read one XFF2 module and validate all table and section bounds."""

    MAX_SECTIONS = 512
    MAX_SYMBOLS = 1_000_000
    MAX_RELOCATION_TABLES = 4096

    def __init__(self, name: str, source: bytes | bytearray):
        self.name = name
        self.data = bytes(source)
        if len(self.data) < HEADER_SIZE or self.data[:4] != MAGIC:
            raise XffFormatError(f"{name}: missing XFF2 header")

        self.first_global = _u32(self.data, 0x0C, "first global symbol")
        self.file_size = _u32(self.data, 0x14, "file size")
        self.import_count = _u32(self.data, 0x1C, "import count")
        self.symbol_count = _u32(self.data, 0x24, "symbol count")
        self.relocation_table_count = _u32(self.data, 0x38, "relocation table count")
        self.section_count = _u32(self.data, 0x40, "section count")
        self.entry_offset = _u32(self.data, 0x4C, "entry offset")

        if self.file_size != len(self.data):
            raise XffFormatError(f"{name}: header size {self.file_size} does not match actual size {len(self.data)}")
        if self.section_count == 0 or self.section_count > self.MAX_SECTIONS:
            raise XffFormatError(f"{name}: invalid section count {self.section_count}")
        if self.symbol_count > self.MAX_SYMBOLS:
            raise XffFormatError(f"{name}: invalid symbol count {self.symbol_count}")
        if self.relocation_table_count > self.MAX_RELOCATION_TABLES:
            raise XffFormatError(f"{name}: invalid relocation table count {self.relocation_table_count}")
        if self.first_global > self.symbol_count:
            raise XffFormatError(f"{name}: first global symbol is outside the symbol table")

        self.symbol_table_offset = _u32(self.data, 0x54, "symbol table offset")
        self.string_table_offset = _u32(self.data, 0x58, "string table offset")
        self.section_table_offset = _u32(self.data, 0x5C, "section table offset")
        self.symbol_value_offset = _u32(self.data, 0x60, "symbol value array offset")
        self.relocation_descriptor_offset = _u32(self.data, 0x64, "relocation descriptor offset")
        self.section_name_index_offset = _u32(self.data, 0x68, "section name index offset")
        self.section_name_strings_offset = _u32(self.data, 0x6C, "section name strings offset")

        _checked_array(self.data, self.section_table_offset, self.section_count, 32, "section headers")
        _checked_array(self.data, self.symbol_table_offset, self.symbol_count, 16, "symbol table")
        _checked_array(self.data, self.section_name_index_offset, self.section_count, 4, "section name indexes")
        _checked_array(self.data, self.relocation_descriptor_offset, self.relocation_table_count, 28, "relocation descriptors")

        self.sections = self._parse_sections()
        self._validate_entry_point()
        self.symbols = self._parse_symbols()
        self.relocation_tables = self._parse_relocations()

    def _parse_sections(self) -> tuple[XffSection, ...]:
        sections: list[XffSection] = []
        for index in range(self.section_count):
            offset = self.section_table_offset + index * 32
            runtime, image_ptr, size, alignment, section_type, flags, _state, file_offset = struct.unpack_from("<8I", self.data, offset)
            name_index = _u32(self.data, self.section_name_index_offset + 4 * index, "section name index")
            name = _c_string(self.data, self.section_name_strings_offset + name_index, len(self.data), f"{self.name} section name")

            image_offset: int | None = None
            if image_ptr >= POINTER_BIAS:
                image_offset = image_ptr - POINTER_BIAS
            elif image_ptr != 0:
                raise XffFormatError(f"{self.name}:{name}: invalid biased image pointer 0x{image_ptr:X}")

            if alignment == 0:
                alignment = 1
            if image_offset is not None and section_type != SHT_NOBITS and size:
                _check_range(self.data, image_offset, size, f"{self.name}:{name} section data")
                if file_offset != image_offset:
                    raise XffFormatError(f"{self.name}:{name}: image pointer and file offset disagree")
            elif section_type != SHT_NOBITS and size and flags & SHF_ALLOC:
                raise XffFormatError(f"{self.name}:{name}: allocated data section has no file image")

            sections.append(XffSection(index, name, runtime, image_offset, size, alignment,
                                       section_type, flags, file_offset))
        return tuple(sections)

    def _validate_entry_point(self) -> None:
        text = next((section for section in self.sections if section.name == ".text"), None)
        if text is None or not text.is_allocated or text.type != SHT_PROGBITS:
            raise XffFormatError(f"{self.name}: required allocated .text section is missing")
        if self.entry_offset >= text.size or self.entry_offset & 3:
            raise XffFormatError(f"{self.name}: entry offset 0x{self.entry_offset:X} is outside .text")

    def _parse_symbols(self) -> tuple[XffSymbol, ...]:
        strings_limit = len(self.data)
        strtab = next((section for section in self.sections if section.name == ".strtab"), None)
        if strtab is not None and strtab.image_offset is not None:
            strings_limit = strtab.image_offset + strtab.size
            if self.string_table_offset != strtab.image_offset:
                raise XffFormatError(f"{self.name}: header .strtab pointer disagrees with section table")

        symbols: list[XffSymbol] = []
        for index in range(self.symbol_count):
            offset = self.symbol_table_offset + 16 * index
            name_offset, value, size, info, _other, section_index = struct.unpack_from("<IIIBBH", self.data, offset)
            name = _c_string(self.data, self.string_table_offset + name_offset, strings_limit,
                             f"{self.name} symbol name") if name_offset else ""
            if section_index < 0xFF00 and section_index >= self.section_count:
                raise XffFormatError(f"{self.name}:{name or index}: invalid section index {section_index}")
            symbols.append(XffSymbol(index, name, value, size, info >> 4, info & 0x0F, section_index))
        return tuple(symbols)

    def _parse_relocations(self) -> tuple[XffRelocationTable, ...]:
        tables: list[XffRelocationTable] = []
        for table_index in range(self.relocation_table_count):
            offset = self.relocation_descriptor_offset + 28 * table_index
            table_type, count, target_section, _entries_ptr, _originals_ptr, entries_offset, originals_offset = struct.unpack_from("<7I", self.data, offset)
            if table_type != SHT_REL:
                raise XffFormatError(f"{self.name}: relocation table {table_index} has unsupported type {table_type}")
            if target_section >= self.section_count:
                raise XffFormatError(f"{self.name}: relocation table {table_index} targets missing section {target_section}")
            _checked_array(self.data, entries_offset, count, 8, f"{self.name} relocation entries")
            _checked_array(self.data, originals_offset, count, 8, f"{self.name} relocation originals")
            target = self.sections[target_section]
            if not target.is_allocated or target.image_offset is None or target.type == SHT_NOBITS:
                raise XffFormatError(f"{self.name}: relocations target non-file-backed section {target.name}")

            entries: list[XffRelocation] = []
            for entry_index in range(count):
                entry_offset, info = struct.unpack_from("<II", self.data, entries_offset + entry_index * 8)
                original, auxiliary = struct.unpack_from("<II", self.data, originals_offset + entry_index * 8)
                symbol_index, relocation_type = info >> 8, info & 0xFF
                if symbol_index >= self.symbol_count:
                    raise XffFormatError(f"{self.name}:{target.name}: relocation references symbol {symbol_index}")
                if relocation_type not in SUPPORTED_RELOCATIONS:
                    raise XffFormatError(f"{self.name}:{target.name}: unsupported MIPS relocation {relocation_type}")
                if entry_offset & 3 or entry_offset + 4 > target.size:
                    raise XffFormatError(f"{self.name}:{target.name}: relocation offset 0x{entry_offset:X} is invalid")
                entries.append(XffRelocation(entry_offset, symbol_index, relocation_type, original, auxiliary))
            tables.append(XffRelocationTable(table_type, target_section, tuple(entries)))
        return tuple(tables)


@dataclass
class LinkedModule:
    module: XffModule
    base: int
    bss_base: int
    image: bytearray = field(init=False)
    section_addresses: dict[int, int] = field(init=False, default_factory=dict)
    symbol_addresses: dict[int, int] = field(init=False, default_factory=dict)
    entry_address: int = field(init=False)
    unresolved_imports: list[dict] = field(init=False, default_factory=list)

    def __post_init__(self) -> None:
        self.image = bytearray(self.module.data)
        bss_cursor = self.bss_base
        for section in self.module.sections:
            if not section.is_allocated or section.size == 0:
                continue
            if section.type == SHT_NOBITS:
                bss_cursor = _align(bss_cursor, section.alignment)
                self.section_addresses[section.index] = bss_cursor
                bss_cursor += section.size
            elif section.image_offset is not None:
                address = self.base + section.image_offset
                if address > 0xFFFFFFFF or address + section.size > 0x100000000:
                    raise XffFormatError(f"{self.module.name}:{section.name}: linked section exceeds 32-bit guest memory")
                self.section_addresses[section.index] = address
            if section.type == SHT_NOBITS and bss_cursor > 0x100000000:
                raise XffFormatError(f"{self.module.name}:{section.name}: BSS exceeds 32-bit guest memory")

        text = next(section for section in self.module.sections if section.name == ".text")
        self.entry_address = self.section_addresses[text.index] + self.module.entry_offset
        if self.entry_address > 0xFFFFFFFF:
            raise XffFormatError(f"{self.module.name}: entry point exceeds 32-bit guest memory")

    def address_of_symbol(self, symbol: XffSymbol, exports: dict[str, int], undefined_stub: int) -> tuple[int, bool]:
        if symbol.is_absolute:
            return symbol.value, False
        if symbol.is_undefined:
            if symbol.name in exports:
                return exports[symbol.name], False
            return undefined_stub, True
        section_address = self.section_addresses.get(symbol.section_index)
        if section_address is None:
            raise XffFormatError(f"{self.module.name}:{symbol.name}: symbol belongs to an unloaded section")
        if symbol.value > self.module.sections[symbol.section_index].size:
            raise XffFormatError(f"{self.module.name}:{symbol.name}: symbol value is outside its section")
        return (section_address + symbol.value) & 0xFFFFFFFF, False


class XffLinker:
    """Relocate modules in loader order, resolving imports from earlier modules."""

    def __init__(self, undefined_stub: int):
        self.undefined_stub = undefined_stub & 0xFFFFFFFF
        self.exports: dict[str, int] = {}
        self.modules: list[LinkedModule] = []
        self.unresolved_imports: list[dict] = []
        self.duplicate_exports: list[dict] = []

    def link(self, module: XffModule, base: int, bss_base: int) -> LinkedModule:
        linked = LinkedModule(module, base & 0xFFFFFFFF, bss_base & 0xFFFFFFFF)
        for symbol in module.symbols:
            if not symbol.is_undefined and symbol.section_index != SHN_COMMON:
                try:
                    linked.symbol_addresses[symbol.index] = linked.address_of_symbol(symbol, self.exports, self.undefined_stub)[0]
                except XffFormatError:
                    # Non-allocated/debug-only symbols are not used to resolve game imports.
                    continue

        for table in module.relocation_tables:
            target = module.sections[table.target_section]
            target_address = linked.section_addresses[target.index]
            assert target.image_offset is not None
            pending_hi: dict[int, list[tuple[int, int]]] = {}

            for relocation in table.entries:
                if relocation.type == R_MIPS_NONE:
                    continue
                symbol = module.symbols[relocation.symbol_index]
                symbol_address, used_stub = linked.address_of_symbol(symbol, self.exports, self.undefined_stub)
                image_position = target.image_offset + relocation.offset
                place_address = target_address + relocation.offset
                original = relocation.original_word

                if relocation.type == R_MIPS_32:
                    patched = (original + symbol_address) & 0xFFFFFFFF
                    struct.pack_into("<I", linked.image, image_position, patched)
                elif relocation.type == R_MIPS_26:
                    if original >> 26 not in (2, 3):
                        raise XffFormatError(f"{module.name}:{target.name}: R_MIPS_26 does not reference J/JAL")
                    target_address_value = (((original & 0x03FFFFFF) << 2) + symbol_address) & 0xFFFFFFFF
                    if target_address_value & 3:
                        raise XffFormatError(f"{module.name}:{target.name}: unaligned R_MIPS_26 target")
                    if (target_address_value & 0xF0000000) != ((place_address + 4) & 0xF0000000):
                        raise XffFormatError(f"{module.name}:{target.name}: R_MIPS_26 target crosses a 256 MiB region")
                    patched = (original & 0xFC000000) | ((target_address_value >> 2) & 0x03FFFFFF)
                    struct.pack_into("<I", linked.image, image_position, patched)
                elif relocation.type == R_MIPS_HI16:
                    pending_hi.setdefault(relocation.symbol_index, []).append((image_position, original))
                elif relocation.type == R_MIPS_LO16:
                    low_addend = original & 0xFFFF
                    if low_addend & 0x8000:
                        low_addend -= 0x10000
                    for hi_position, hi_original in pending_hi.pop(relocation.symbol_index, []):
                        addend = ((hi_original & 0xFFFF) << 16) + low_addend
                        value = (symbol_address + addend) & 0xFFFFFFFF
                        high = ((value + 0x8000) >> 16) & 0xFFFF
                        struct.pack_into("<I", linked.image, hi_position, (hi_original & 0xFFFF0000) | high)
                    patched_low = (original & 0xFFFF0000) | ((symbol_address + low_addend) & 0xFFFF)
                    struct.pack_into("<I", linked.image, image_position, patched_low)

                if used_stub:
                    record = {
                        "module": module.name,
                        "section": target.name,
                        "address": f"0x{place_address:08X}",
                        "relocation": relocation.type,
                        "symbol": symbol.name,
                        "original_word": f"0x{original:08X}",
                        "link_time_word": f"0x{struct.unpack_from('<I', linked.image, image_position)[0]:08X}",
                        "undefined_stub": f"0x{self.undefined_stub:08X}",
                    }
                    linked.unresolved_imports.append(record)
                    self.unresolved_imports.append(record)

            dangling = {index: entries for index, entries in pending_hi.items() if entries}
            if dangling:
                symbols = ", ".join(module.symbols[index].name or f"#{index}" for index in dangling)
                raise XffFormatError(f"{module.name}:{target.name}: HI16 relocation without matching LO16 for {symbols}")

        # Publish only after the module is relocated, matching the loader's module order.
        for symbol in module.symbols:
            if not symbol.is_exported or symbol.section_index == SHN_COMMON:
                continue
            address = linked.symbol_addresses.get(symbol.index)
            if address is None:
                try:
                    address = linked.address_of_symbol(symbol, self.exports, self.undefined_stub)[0]
                except XffFormatError:
                    continue
            previous = self.exports.get(symbol.name)
            if previous is not None and previous != address:
                self.duplicate_exports.append({"name": symbol.name, "kept": f"0x{previous:08X}", "ignored": f"0x{address:08X}", "module": module.name})
            else:
                self.exports.setdefault(symbol.name, address)

        self.modules.append(linked)
        return linked

    def link_in_order(self, modules: Iterable[tuple[XffModule, int, int]]) -> list[LinkedModule]:
        return [self.link(module, base, bss) for module, base, bss in modules]
