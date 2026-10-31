#!/usr/bin/env python3
"""Identify and verify the supported Shadow of the Colossus PS2 disc."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from iso9660 import Iso9660

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "game" / "profiles" / "SCES-53326_v1.00.json"


def parse_system_cnf(data: bytes) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in data.decode("ascii", "replace").splitlines():
        if "=" in raw_line:
            key, value = raw_line.split("=", 1)
            values[key.strip().upper()] = value.strip()
    return values


def normalize_boot_path(value: str) -> str:
    value = value.replace("\\\\", "/")
    if ":" in value:
        value = value.split(":", 1)[1]
    return value.rsplit("/", 1)[-1].split(";", 1)[0].strip().upper()


def verify_file(iso: Iso9660, spec: dict, check_xff: bool = False) -> dict:
    entry = iso.find(spec["path"])
    if entry is None or entry.is_dir:
        raise ValueError(f"required file is missing: {spec['path']}")
    if entry.size != spec["size_bytes"]:
        raise ValueError(f"{spec['path']}: expected {spec['size_bytes']} bytes, found {entry.size}")
    if check_xff and iso.read_entry(entry, 4) != b"xff2":
        raise ValueError(f"{spec['path']}: xff2 signature missing")
    actual = iso.hash_entry(entry)
    if actual.lower() != spec["sha256"].lower():
        raise ValueError(f"{spec['path']}: SHA-256 mismatch")
    return {"path": entry.path, "size_bytes": entry.size, "sha256": actual, "lba": entry.lba}


def verify_boot_elf(iso: Iso9660, profile: dict) -> dict:
    spec = profile["boot_elf"]
    entry = iso.find(spec["path"])
    if entry is None or entry.is_dir or entry.size != spec["size_bytes"]:
        raise ValueError("boot ELF missing or has an unexpected size")
    header = iso.read_at(entry.lba * 2048, 52)
    if header[:4] != b"\\x7fELF" or header[4] != spec["elf_class"] or header[5] != 1:
        raise ValueError("boot file is not little-endian ELF32")
    machine = struct.unpack_from("<H", header, 18)[0]
    entry_point = struct.unpack_from("<I", header, 24)[0]
    if machine != 8 or entry_point != int(spec["entry_point"], 16):
        raise ValueError("ELF machine or entry point does not match the supported profile")
    return verify_file(iso, spec)


def full_disc_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_disc(path: Path, profile: dict, extract_dir: Path | None,
                 verify_full_hash: bool) -> dict:
    size = path.stat().st_size
    expected_disc = profile["disc"]
    if size != expected_disc["size_bytes"]:
        raise ValueError(f"disc size mismatch: got {size}, expected {expected_disc['size_bytes']} bytes")

    with Iso9660(path) as iso:
        if iso.volume_sectors != expected_disc["volume_sectors"]:
            raise ValueError("ISO9660 volume sector count does not match the disc profile")

        system_spec = profile["system_cnf"]
        system_entry = iso.find(system_spec["path"])
        if system_entry is None or system_entry.size != system_spec["size_bytes"]:
            raise ValueError("SYSTEM.CNF missing or has unexpected size")
        system_data = iso.read_entry(system_entry, 64 * 1024)
        system = parse_system_cnf(system_data)
        boot_path = normalize_boot_path(system.get("BOOT2", system.get("BOOT", "")))
        if boot_path != profile["boot_elf"]["path"].upper():
            raise ValueError(f"SYSTEM.CNF boot target mismatch: {boot_path!r}")
        if system.get("VER") != profile["version"]:
            raise ValueError(f"SYSTEM.CNF version mismatch: {system.get('VER')!r}")

        verified = [
            verify_file(iso, system_spec),
            verify_boot_elf(iso, profile),
            verify_file(iso, profile["startup"]),
        ]
        for module in profile["ee_modules"]:
            verified.append(verify_file(iso, module, check_xff=True))

        extracted: list[str] = []
        if extract_dir is not None:
            for spec in (profile["boot_elf"], *profile["ee_modules"]):
                entry = iso.find(spec["path"])
                assert entry is not None
                iso.extract_entry(entry, extract_dir / spec["path"])
                extracted.append(spec["path"])

        result = {
            "supported": True,
            "game": profile["game"],
            "serial": profile["serial"],
            "region": profile["region"],
            "version": profile["version"],
            "disc_size_bytes": size,
            "volume_id": iso.volume_id,
            "volume_sectors": iso.volume_sectors,
            "system_cnf": system,
            "verified_files": verified,
            "extracted_files": extracted,
        }

    if verify_full_hash:
        digest = full_disc_sha256(path)
        if digest.lower() != expected_disc["sha256"].lower():
            raise ValueError("full disc SHA-256 mismatch")
        result["disc_sha256"] = digest

    if extract_dir is not None:
        extract_dir.mkdir(parents=True, exist_ok=True)
        manifest = {
            "profile": f"{profile['serial']} v{profile['version']}",
            "disc_sha256_expected": expected_disc["sha256"],
            "verified_files": verified,
            "generated_from_user_disc": True,
            "generated_files_are_local_only": True,
        }
        (extract_dir / "input_manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\\n", encoding="utf-8"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path, help="path to your own PS2 disc image")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--extract-dir", type=Path,
                        help="optional local directory for the boot ELF and three XFF modules")
    parser.add_argument("--full-disc-hash", action="store_true",
                        help="also hash every byte of the 4.6 GB image")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8"))
        result = inspect_disc(args.iso, profile, args.extract_dir, args.full_disc_hash)
    except (OSError, ValueError, KeyError, AssertionError) as exc:
        print(f"Disc validation failed: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"Supported disc: {result['game']} — {result['serial']} v{result['version']}")
        print(f"Region: {result['region']}; image size: {result['disc_size_bytes']:,} bytes")
        print("Verified: SYSTEM.CNF, boot ELF, STARTUP.XFF and three EE XFF modules")
        if args.full_disc_hash:
            print(f"Full image SHA-256: {result['disc_sha256']}")
        if args.extract_dir:
            print(f"Recompilation inputs extracted to: {args.extract_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
