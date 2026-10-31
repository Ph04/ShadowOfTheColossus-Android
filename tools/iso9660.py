"""Bounds-checked ISO9660 reader for PS2 disc inspection."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

SECTOR_SIZE = 2048
MAX_DIRECTORY_BYTES = 16 * 1024 * 1024
MAX_ENTRIES = 50_000
MAX_DEPTH = 16


@dataclass(frozen=True)
class Entry:
    path: str
    lba: int
    size: int
    is_dir: bool


def _le32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 4], "little")


def _both16(data: bytes, offset: int) -> int:
    little = int.from_bytes(data[offset:offset + 2], "little")
    big = int.from_bytes(data[offset + 2:offset + 4], "big")
    if little != big:
        raise ValueError("inconsistent both-endian ISO9660 16-bit field")
    return little


def _both32(data: bytes, offset: int) -> int:
    little = _le32(data, offset)
    big = int.from_bytes(data[offset + 4:offset + 8], "big")
    if little != big:
        raise ValueError("inconsistent both-endian ISO9660 32-bit field")
    return little


def _component(value: str) -> str:
    return value.split(";", 1)[0].rstrip(" \\x00").upper()


def normalize_path(value: str) -> str:
    return "/".join(
        part for part in (_component(p) for p in value.replace("\\\\", "/").split("/")) if part
    )


class Iso9660:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.file: BinaryIO = self.path.open("rb")
        self.file_size = self.path.stat().st_size
        self.volume_id = ""
        self.volume_sectors = 0
        self.entries: dict[str, Entry] = {}
        self._opened = False
        try:
            self._open()
        except Exception:
            self.close()
            raise

    def close(self) -> None:
        if not self.file.closed:
            self.file.close()

    def __enter__(self) -> "Iso9660":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def read_at(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset > self.file_size or size > self.file_size - offset:
            raise ValueError("read range is outside the image")
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise OSError("short read from disc image")
        return data

    def _in_range(self, lba: int, size: int) -> bool:
        offset = lba * SECTOR_SIZE
        return 0 <= offset <= self.file_size and 0 <= size <= self.file_size - offset

    def _open(self) -> None:
        if self.file_size < 17 * SECTOR_SIZE:
            raise ValueError("file is too small to be a PS2 ISO9660 image")

        pvd = None
        for lba in range(16, 80):
            descriptor = self.read_at(lba * SECTOR_SIZE, SECTOR_SIZE)
            if descriptor[1:6] != b"CD001" or descriptor[6] != 1:
                raise ValueError(f"invalid ISO9660 volume descriptor at LBA {lba}")
            if descriptor[0] == 255:
                break
            if descriptor[0] == 1:
                pvd = descriptor
                break
        if pvd is None:
            raise ValueError("no ISO9660 primary volume descriptor found")

        if _both16(pvd, 128) != SECTOR_SIZE:
            raise ValueError("unsupported ISO9660 logical block size")
        self.volume_sectors = _both32(pvd, 80)
        if self.volume_sectors * SECTOR_SIZE > self.file_size:
            raise ValueError("ISO9660 volume exceeds the supplied image")
        self.volume_id = pvd[40:72].decode("ascii", "replace").rstrip()

        root = pvd[156:190]
        if len(root) != 34 or root[0] != 34 or not (root[25] & 2):
            raise ValueError("invalid ISO9660 root directory record")
        self._walk(_both32(root, 2), _both32(root, 10), "", 0, set())
        self._opened = True

    def _walk(self, lba: int, size: int, prefix: str, depth: int,
              visited: set[tuple[int, int]]) -> None:
        if depth > MAX_DEPTH or size > MAX_DIRECTORY_BYTES or not self._in_range(lba, size):
            raise ValueError("ISO9660 directory is outside supported bounds")
        key = (lba, size)
        if key in visited:
            return
        visited.add(key)

        data = self.read_at(lba * SECTOR_SIZE, size)
        offset = 0
        while offset < len(data):
            record_size = data[offset]
            if record_size == 0:
                offset = ((offset // SECTOR_SIZE) + 1) * SECTOR_SIZE
                continue
            if record_size < 34 or offset + record_size > len(data):
                raise ValueError("malformed ISO9660 directory record")
            record = data[offset:offset + record_size]
            name_len = record[32]
            if 33 + name_len > record_size:
                raise ValueError("invalid ISO9660 file identifier length")
            file_lba = _both32(record, 2)
            file_size = _both32(record, 10)
            flags = record[25]
            name = record[33:33 + name_len]
            offset += record_size

            if name_len == 1 and name in (b"\\x00", b"\\x01"):
                continue
            if flags & 0x80:
                raise ValueError("multi-extent ISO9660 files are not supported")
            normalized = _component(name.decode("ascii", "replace"))
            if not normalized:
                continue

            full_path = f"{prefix}/{normalized}" if prefix else normalized
            if not self._in_range(file_lba, file_size):
                raise ValueError(f"ISO9660 entry points outside image: {full_path}")
            entry = Entry(full_path, file_lba, file_size, bool(flags & 2))
            self.entries.setdefault(full_path, entry)
            if len(self.entries) > MAX_ENTRIES:
                raise ValueError("too many ISO9660 directory entries")
            if entry.is_dir:
                self._walk(file_lba, file_size, full_path, depth + 1, visited)

    def find(self, path: str) -> Entry | None:
        return self.entries.get(normalize_path(path)) if self._opened else None

    def read_entry(self, entry: Entry, maximum_size: int) -> bytes:
        if entry.is_dir or entry.size > maximum_size or not self._in_range(entry.lba, entry.size):
            raise ValueError(f"invalid file or size limit exceeded: {entry.path}")
        return self.read_at(entry.lba * SECTOR_SIZE, entry.size)

    def hash_entry(self, entry: Entry, chunk_size: int = 1024 * 1024) -> str:
        import hashlib
        if entry.is_dir or not self._in_range(entry.lba, entry.size):
            raise ValueError(f"invalid ISO9660 file: {entry.path}")
        digest = hashlib.sha256()
        offset = entry.lba * SECTOR_SIZE
        remaining = entry.size
        while remaining:
            amount = min(remaining, chunk_size)
            digest.update(self.read_at(offset, amount))
            offset += amount
            remaining -= amount
        return digest.hexdigest()

    def extract_entry(self, entry: Entry, destination: str | Path,
                      chunk_size: int = 4 * 1024 * 1024) -> None:
        if entry.is_dir or not self._in_range(entry.lba, entry.size):
            raise ValueError(f"invalid ISO9660 file: {entry.path}")
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(destination.suffix + ".partial")
        try:
            offset = entry.lba * SECTOR_SIZE
            remaining = entry.size
            with temporary.open("wb") as output:
                while remaining:
                    amount = min(remaining, chunk_size)
                    output.write(self.read_at(offset, amount))
                    offset += amount
                    remaining -= amount
            temporary.replace(destination)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
