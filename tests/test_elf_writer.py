from __future__ import annotations

import csv
import struct
import tempfile
import unittest
from pathlib import Path

from tools.elf_writer import (
    ImageSection,
    ImageSymbol,
    load_boot_sections,
    write_elf32_mips,
    write_function_map,
)
from tools.xff2 import SHT_NOBITS, SHT_PROGBITS, STT_FUNC, SHN_ABS


def make_boot_elf() -> bytes:
    data = bytearray(0x240)
    data[:7] = b"\x7fELF\x01\x01\x01"
    shstr = b"\0.text\0.shstrtab\0"
    data[0x100:0x110] = bytes(range(16))
    data[0x120:0x120 + len(shstr)] = shstr
    # ELF32 ET_EXEC, EM_MIPS, little endian, entry inside .text.
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        data,
        16,
        2, 8, 1, 0x00100004, 0, 0x140, 0x20924001,
        52, 32, 0, 40, 3, 2,
    )
    # NULL, .text, .shstrtab
    struct.pack_into("<10I", data, 0x140 + 0 * 40, *([0] * 10))
    struct.pack_into("<10I", data, 0x140 + 1 * 40,
                     1, SHT_PROGBITS, 0x6, 0x00100000, 0x100, 0x10, 0, 0, 16, 0)
    struct.pack_into("<10I", data, 0x140 + 2 * 40,
                     7, 3, 0, 0, 0x120, len(shstr), 0, 0, 1, 0)
    return bytes(data)


class ElfWriterTests(unittest.TestCase):
    def test_loads_allocated_boot_sections_and_entry(self) -> None:
        entry, flags, sections = load_boot_sections(make_boot_elf())
        self.assertEqual(entry, 0x00100004)
        self.assertEqual(flags, 0x20924001)
        self.assertEqual(len(sections), 1)
        self.assertEqual(sections[0].name, ".boot.text")
        self.assertEqual(sections[0].data, bytes(range(16)))
        self.assertTrue(sections[0].is_code)

    def test_writes_elf32_mips_with_loadable_bss_and_symbols(self) -> None:
        sections = [
            ImageSection(".boot.text", 0x00100000, bytes(range(16)), 16,
                         SHT_PROGBITS, 0x6, 16),
            ImageSection(".text.kernel", 0x001A7530, b"\x11" * 32, 32,
                         SHT_PROGBITS, 0x6, 16),
            ImageSection(".bss.kernel", 0x001EFA00, None, 64,
                         SHT_NOBITS, 0x3, 16),
        ]
        symbols = [
            ImageSymbol("boot_function", 0x00100004, 8, 1, STT_FUNC, SHN_ABS),
            ImageSymbol("kernel_function", 0x001A7530, 16, 1, STT_FUNC, 2),
        ]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            elf_path = root / "sotc_linked.elf"
            write_elf32_mips(elf_path, 0x00100004, 0x20924001, sections, symbols)
            data = elf_path.read_bytes()

            self.assertEqual(data[:4], b"\x7fELF")
            self.assertEqual(data[4:7], b"\x01\x01\x01")
            values = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
            self.assertEqual(values[0:4], (2, 8, 1, 0x00100004))
            phoff, shoff = values[4], values[5]
            phnum, shentsize, shnum, shstrndx = values[9], values[10], values[11], values[12]
            self.assertEqual(phoff, 52)
            self.assertEqual(phnum, 3)
            self.assertEqual(shentsize, 40)
            self.assertEqual(shnum, 7)
            headers = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
            shstr_header = headers[shstrndx]
            shstr = data[shstr_header[4]:shstr_header[4] + shstr_header[5]]
            names = {
                (shstr.find(b"\0", 0) + 1): ""
            }
            section_names = []
            for header in headers:
                off = header[0]
                end = shstr.find(b"\0", off)
                section_names.append(shstr[off:end].decode("utf-8") if off else "")
            self.assertIn(".boot.text", section_names)
            self.assertIn(".text.kernel", section_names)
            self.assertIn(".bss.kernel", section_names)
            bss_header = headers[section_names.index(".bss.kernel")]
            self.assertEqual(bss_header[1], SHT_NOBITS)
            self.assertEqual(bss_header[5], 64)

            bss_phdr = struct.unpack_from("<8I", data, phoff + 2 * 32)
            self.assertEqual(bss_phdr[0], 1)
            self.assertEqual(bss_phdr[4], 0)
            self.assertEqual(bss_phdr[5], 64)

            symtab_header = headers[section_names.index(".symtab")]
            self.assertEqual(symtab_header[1], 2)
            self.assertEqual(symtab_header[6], shstrndx - 1)  # linked .strtab section
            self.assertEqual(symtab_header[9], 16)

            csv_path = root / "sotc_functions.csv"
            count = write_function_map(csv_path, sections, symbols)
            self.assertEqual(count, 2)
            with csv_path.open(newline="", encoding="utf-8") as source:
                rows = list(csv.reader(source))
            self.assertEqual(rows[0], ["name", "start", "end", "size"])
            self.assertEqual(rows[1][0], "boot_function")
            self.assertEqual(rows[2][0], "kernel_function")


if __name__ == "__main__":
    unittest.main()
