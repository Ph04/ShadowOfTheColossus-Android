#!/usr/bin/env python3
"""Exercise the pinned PS2Recomp analyzer and recompiler using a synthetic ELF."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.elf_writer import ImageSection, ImageSymbol, write_elf32_mips
from tools.xff2 import SHT_PROGBITS, STT_FUNC

# jr $ra; nop — a minimal valid R5900 function, not game code.
RETURN_FUNCTION = bytes.fromhex("0800e00300000000")


def run_tool(command: list[str], cwd: Path, label: str) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=300,
    )
    if result.returncode != 0:
        raise RuntimeError(f"{label} failed with exit code {result.returncode}:\n{result.stdout}")
    return result.stdout


def smoke_test(analyzer: Path, recompiler: Path, workdir: Path) -> dict:
    analyzer = analyzer.resolve()
    recompiler = recompiler.resolve()
    workdir = workdir.resolve()
    workdir.mkdir(parents=True, exist_ok=True)

    elf = workdir / "synthetic_mips.elf"
    sections = [
        ImageSection(
            ".text.smoke",
            0x00100000,
            RETURN_FUNCTION,
            len(RETURN_FUNCTION),
            SHT_PROGBITS,
            0x6,  # SHF_ALLOC | SHF_EXECINSTR
            4,
        )
    ]
    symbols = [
        ImageSymbol("smoke_entry", 0x00100000, len(RETURN_FUNCTION), 1, STT_FUNC, 1)
    ]
    write_elf32_mips(elf, 0x00100000, 0x20924001, sections, symbols)

    config = workdir / "synthetic_recomp.toml"
    analyzer_output = run_tool([str(analyzer), str(elf), str(config)], workdir, "ps2_analyzer")
    if not config.is_file() or config.stat().st_size == 0:
        raise RuntimeError("ps2_analyzer did not produce a TOML configuration")

    recomp_output = run_tool([str(recompiler), str(config)], workdir, "ps2_recomp")
    generated_dir = workdir / "output"
    generated_cpp = sorted(generated_dir.rglob("*.cpp")) if generated_dir.exists() else []
    if not generated_cpp:
        raise RuntimeError(
            "ps2_recomp returned success but did not produce any C++ source files\n"
            f"stdout:\n{recomp_output}"
        )

    return {
        "elf": str(elf),
        "config": str(config),
        "generated_cpp_count": len(generated_cpp),
        "generated_cpp": [str(path.relative_to(workdir)) for path in generated_cpp[:8]],
        "analyzer_output_tail": analyzer_output.splitlines()[-8:],
        "recompiler_output_tail": recomp_output.splitlines()[-8:],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analyzer", type=Path, required=True)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = smoke_test(args.analyzer, args.recompiler, args.workdir)
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"PS2Recomp smoke test failed: {exc}", file=sys.stderr)
        return 1
    print("PS2Recomp end-to-end synthetic ELF smoke test: PASS")
    print(f"Generated C++ files: {result['generated_cpp_count']}")
    for path in result["generated_cpp"]:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
