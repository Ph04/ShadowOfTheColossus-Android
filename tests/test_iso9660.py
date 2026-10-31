from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.iso9660 import Iso9660, SECTOR_SIZE


def both32(value: int) -> bytes:
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def both16(value: int) -> bytes:
    return value.to_bytes(2, "little") + value.to_bytes(2, "big")


def directory_record(lba: int, size: int, name: bytes, is_dir: bool = False) -> bytes:
    record = bytearray(33)
    record[1:9] = both32(lba)
    record[10:18] = both32(size)
    record[25] = 2 if is_dir else 0
    record[28:32] = both16(1)
    record[32] = len(name)
    record.extend(name)
    if len(name) % 2 == 0:
        record.append(0)
    return bytes([len(record)]) + bytes(record)


def pad_directory(records: list[bytes]) -> bytes:
    data = b"".join(records)
    return data + bytes(SECTOR_SIZE - len(data))


def make_test_iso(path: Path) -> None:
    image = bytearray(24 * SECTOR_SIZE)
    pvd = bytearray(SECTOR_SIZE)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[40:72] = b"UNIT TEST DISC".ljust(32, b" ")
    pvd[80:88] = both32(24)
    pvd[128:132] = both16(SECTOR_SIZE)
    pvd[156:190] = directory_record(20, SECTOR_SIZE, b"\\x00", True)
    image[16 * SECTOR_SIZE:17 * SECTOR_SIZE] = pvd

    terminator = bytearray(SECTOR_SIZE)
    terminator[0] = 255
    terminator[1:6] = b"CD001"
    terminator[6] = 1
    image[17 * SECTOR_SIZE:18 * SECTOR_SIZE] = terminator

    system_cnf = b"BOOT2 = cdrom0:\\\\SCES_533.26;1\\r\\nVER = 1.00\\r\\nVMODE = PAL\\r\\n"
    image[21 * SECTOR_SIZE:21 * SECTOR_SIZE + len(system_cnf)] = system_cnf
    nested = pad_directory([
        directory_record(22, SECTOR_SIZE, b"\\x00", True),
        directory_record(20, SECTOR_SIZE, b"\\x01", True),
        directory_record(23, 11, b"INFO.TXT;1"),
    ])
    image[22 * SECTOR_SIZE:23 * SECTOR_SIZE] = nested
    image[23 * SECTOR_SIZE:23 * SECTOR_SIZE + 11] = b"hello world"
    root = pad_directory([
        directory_record(20, SECTOR_SIZE, b"\\x00", True),
        directory_record(20, SECTOR_SIZE, b"\\x01", True),
        directory_record(21, len(system_cnf), b"SYSTEM.CNF;1"),
        directory_record(22, SECTOR_SIZE, b"DATA", True),
    ])
    image[20 * SECTOR_SIZE:21 * SECTOR_SIZE] = root
    path.write_bytes(image)


class Iso9660Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.image = Path(self.temp.name) / "test.iso"
        make_test_iso(self.image)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_case_insensitive_lookup_and_version_suffix(self) -> None:
        with Iso9660(self.image) as iso:
            entry = iso.find("system.cnf")
            self.assertIsNotNone(entry)
            assert entry is not None
            self.assertEqual(entry.path, "SYSTEM.CNF")
            self.assertIn(b"BOOT2", iso.read_entry(entry, 4096))

    def test_nested_directory_lookup(self) -> None:
        with Iso9660(self.image) as iso:
            entry = iso.find("data/info.txt;1")
            self.assertIsNotNone(entry)
            assert entry is not None
            self.assertEqual(iso.read_entry(entry, 64), b"hello world")

    def test_rejects_corrupt_primary_descriptor(self) -> None:
        broken = bytearray(self.image.read_bytes())
        broken[16 * SECTOR_SIZE + 1:16 * SECTOR_SIZE + 6] = b"NOISO"
        self.image.write_bytes(broken)
        with self.assertRaisesRegex(ValueError, "invalid ISO9660"):
            Iso9660(self.image)

    def test_rejects_out_of_range_directory_entry(self) -> None:
        broken = bytearray(self.image.read_bytes())
        root_start = 20 * SECTOR_SIZE
        first_file_record = root_start + 68
        broken[first_file_record + 2:first_file_record + 10] = both32(1000)
        self.image.write_bytes(broken)
        with self.assertRaises(ValueError):
            Iso9660(self.image)


if __name__ == "__main__":
    unittest.main()
