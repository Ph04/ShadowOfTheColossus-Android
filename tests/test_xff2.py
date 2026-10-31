from __future__ import annotations

import struct
import unittest

from tools.xff2 import (
    POINTER_BIAS,
    R_MIPS_26,
    R_MIPS_32,
    R_MIPS_HI16,
    R_MIPS_LO16,
    SHN_ABS,
    SHN_UNDEF,
    XffFormatError,
    XffLinker,
    XffModule,
)


def make_xff(name: str = "KERNEL", *, bad_reloc_offset: bool = False) -> bytes:
    """Create a small XFF2 fixture with text/data/BSS and four relocations."""
    data = bytearray(0x500)
    data[:4] = b"xff2"

    strings = b"\0external\0absolute\0local_data\0public_func\0"
    (data.__setitem__(slice(0x140, 0x140 + len(strings)), strings))
    string_offsets = {
        "external": strings.index(b"external"),
        "absolute": strings.index(b"absolute"),
        "local_data": strings.index(b"local_data"),
        "public_func": strings.index(b"public_func"),
    }

    section_names = b"\0.text\0.data\0.bss\0.strtab\0.symtab\0.shstrtab\0"
    data[0x400:0x400 + len(section_names)] = section_names
    name_offsets = [
        0,
        section_names.index(b".text"),
        section_names.index(b".data"),
        section_names.index(b".bss"),
        section_names.index(b".strtab"),
        section_names.index(b".symtab"),
        section_names.index(b".shstrtab"),
    ]

    # Four source instructions: .word relocation, JAL relocation, LUI/ADDIU HI16/LO16 pair.
    struct.pack_into("<8I", data, 0x100,
                     0x00000010, 0x0C000000, 0x3C080000, 0x25087000,
                     0, 0, 0, 0)
    data[0x120:0x130] = bytes(0x10)

    symbols = [
        (0, 0, 0, 0, 0, SHN_UNDEF),
        (string_offsets["external"], 0, 0, 0x10, 0, SHN_UNDEF),
        (string_offsets["absolute"], 0x00123480, 0, 0x12, 0, SHN_ABS),
        (string_offsets["local_data"], 0, 4, 0x11, 0, 2),
        (string_offsets["public_func"], 0x10, 4, 0x12, 0, 1),
    ]
    for index, row in enumerate(symbols):
        struct.pack_into("<IIIBBH", data, 0x180 + index * 16, *row)

    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0),
        (0, POINTER_BIAS + 0x100, 0x20, 16, 1, 0x6, 0, 0x100),
        (0, POINTER_BIAS + 0x120, 0x10, 16, 1, 0x3, 0, 0x120),
        (0, 0, 0x20, 16, 8, 0x3, 0, 0),
        (0, POINTER_BIAS + 0x140, len(strings), 1, 3, 0, 0, 0x140),
        (0, POINTER_BIAS + 0x180, len(symbols) * 16, 4, 2, 0, 0, 0x180),
        (0, POINTER_BIAS + 0x240, len(section_names), 1, 3, 0, 0, 0x240),
    ]
    for index, row in enumerate(sections):
        struct.pack_into("<8I", data, 0x280 + index * 32, *row)
        struct.pack_into("<I", data, 0x3D0 + index * 4, name_offsets[index])

    # Relocations target .text. The HI16 and LO16 refer to the same imported symbol.
    relocations = [
        (0, (3 << 8) | R_MIPS_32, 0x10),
        (4, (2 << 8) | R_MIPS_26, 0x0C000000),
        (8, (1 << 8) | R_MIPS_HI16, 0x3C080000),
        (12 if not bad_reloc_offset else 0x40, (1 << 8) | R_MIPS_LO16, 0x25087000),
    ]
    struct.pack_into("<7I", data, 0x370, 9, len(relocations), 1, 0, 0, 0x390, 0x3B0)
    for index, (offset, info, original) in enumerate(relocations):
        struct.pack_into("<II", data, 0x390 + index * 8, offset, info)
        struct.pack_into("<II", data, 0x3B0 + index * 8, original, 0)

    struct.pack_into("<I", data, 0x0C, 1)       # first global symbol
    struct.pack_into("<I", data, 0x14, len(data))
    struct.pack_into("<I", data, 0x1C, 1)       # imports
    struct.pack_into("<I", data, 0x24, len(symbols))
    struct.pack_into("<I", data, 0x38, 1)       # relocation tables
    struct.pack_into("<I", data, 0x40, len(sections))
    struct.pack_into("<I", data, 0x4C, 0x10)    # .text entry offset
    struct.pack_into("<7I", data, 0x54, 0x180, 0x140, 0x280, 0, 0x370, 0x3D0, 0x400)
    return bytes(data)


class Xff2ParserTests(unittest.TestCase):
    def test_parses_sections_symbols_and_relocations(self) -> None:
        module = XffModule("KERNEL", make_xff())
        self.assertEqual(module.sections[1].name, ".text")
        self.assertEqual(module.sections[3].type, 8)
        self.assertEqual(module.symbols[2].section_index, SHN_ABS)
        self.assertEqual(module.symbols[1].section_index, SHN_UNDEF)
        self.assertEqual(len(module.relocation_tables), 1)
        self.assertEqual([r.type for r in module.relocation_tables[0].entries],
                         [R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16])

    def test_rejects_truncated_file_size(self) -> None:
        raw = bytearray(make_xff())
        struct.pack_into("<I", raw, 0x14, len(raw) + 1)
        with self.assertRaisesRegex(XffFormatError, "does not match actual size"):
            XffModule("BROKEN", raw)

    def test_rejects_out_of_bounds_relocation(self) -> None:
        with self.assertRaisesRegex(XffFormatError, "relocation offset"):
            XffModule("BROKEN", make_xff(bad_reloc_offset=True))

    def test_applies_all_supported_relocations(self) -> None:
        module = XffModule("KERNEL", make_xff())
        linker = XffLinker(undefined_stub=0x1234F000)
        linked = linker.link(module, base=0x1000, bss_base=0x3000)
        text_offset = module.sections[1].image_offset
        assert text_offset is not None

        self.assertEqual(struct.unpack_from("<I", linked.image, text_offset)[0], 0x1120 + 0x10)
        self.assertEqual(struct.unpack_from("<I", linked.image, text_offset + 4)[0], 0x0C000000 | (0x00123480 >> 2))
        # 0x1234F000 + 0x7000 = 0x12356000; HI16 applies the MIPS carry adjustment.
        self.assertEqual(struct.unpack_from("<I", linked.image, text_offset + 8)[0], 0x3C081235)
        self.assertEqual(struct.unpack_from("<I", linked.image, text_offset + 12)[0], 0x25086000)
        self.assertEqual(linked.entry_address, 0x1110)
        self.assertEqual(linked.section_addresses[3], 0x3000)
        self.assertEqual(len(linked.unresolved_imports), 2)
        self.assertEqual(linked.unresolved_imports[0]["symbol"], "external")

    def test_exports_resolve_imports_in_later_modules(self) -> None:
        first_raw = bytearray(make_xff())
        # Turn symbol 1 (external) into a global function export at .text+0x4.
        struct.pack_into("<IIIBBH", first_raw, 0x190, 1, 4, 4, 0x12, 0, 1)
        # Remove first module's relocations so its import table is not used.
        struct.pack_into("<I", first_raw, 0x38, 0)
        struct.pack_into("<I", first_raw, 0x64, 0)
        first = XffModule("KERNEL", first_raw)

        second_raw = bytearray(make_xff())
        # Keep the imported symbol named "external" so it resolves against KERNEL.
        second = XffModule("MANAGER", second_raw)
        linker = XffLinker(undefined_stub=0x001B28F8)
        first_linked = linker.link(first, 0x1000, 0x3000)
        second_linked = linker.link(second, 0x5000, 0x7000)
        self.assertIn("external", linker.exports)
        self.assertEqual(len(second_linked.unresolved_imports), 0)
        second_text_offset = second.sections[1].image_offset
        assert second_text_offset is not None
        self.assertEqual(struct.unpack_from("<I", second_linked.image, second_text_offset)[0], 0x5130)
        self.assertEqual(first_linked.entry_address, 0x1110)


if __name__ == "__main__":
    unittest.main()
