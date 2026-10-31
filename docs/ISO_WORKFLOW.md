# Disc identification and recompilation inputs

## Supported release profile

The first target is exactly the PAL retail release **SCES-53326 v1.00**:

- 2048-byte-sector ISO9660 image, 4,635,918,336 bytes.
- SYSTEM.CNF boots SCES_533.26;1 and reports VER = 1.00.
- SCES_533.26 is a stripped ELF32 little-endian MIPS executable (entry 0x00100010).
- The game code is loaded later from three xff2 modules: KERNEL.XFF, MANAGER.XFF and GAMECORE.XFF.

Expected sizes and SHA-256 values are stored in game/profiles/SCES-53326_v1.00.json. The profile was transcribed from the pinned PC reference port's GAME_BUILD documentation and profile file. It contains no game data.

## Android validation

After the user selects an image through the Android document picker, the native reader:

1. Scans ISO9660 volume descriptors beginning at LBA 16.
2. Checks CD001, descriptor version, both-endian block-size/volume fields and the root directory extent.
3. Builds a bounded directory index and resolves PS2 paths case-insensitively, ignoring ;1 file-version suffixes.
4. Reads SYSTEM.CNF and confirms the boot path and version.
5. Verifies the boot ELF format, machine, entry address, size and SHA-256.
6. Verifies the sizes, xff2 magic and SHA-256 of the three EE modules.

The scan reads directory records and the small boot/config/module files. It does not hash the complete 4.6 GB image and does not copy the disc into app storage. The original SAF URI remains the source for future sector reads.

## Host-side preparation

Run the profile verifier on a development computer:

    python tools/inspect_disc.py /path/to/your/SCES-53326.iso

To extract only the four inputs needed by the game-specific linker:

    python tools/inspect_disc.py /path/to/your/SCES-53326.iso --extract-dir build/game-input

This writes SCES_533.26, the three XFF modules and input_manifest.json below build/game-input. The directory is ignored by Git. The optional --full-disc-hash flag checks the complete disc SHA-256; it reads every byte of the 4.6 GB image.

## What this stage does not do

Extraction and validation do not yet relocate the XFF modules or generate C++ code. The next pipeline component must reproduce the module link layout, relocations, function boundaries and runtime relocation map before invoking ps2_recomp. Running the recompiler against only SCES_533.26 would translate the loader but omit the engine and game-logic modules, so this project deliberately does not do that.

The Android app does not include the ISO, ELF, XFF files, generated C++ or commercial game assets.
