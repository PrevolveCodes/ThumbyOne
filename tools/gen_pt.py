#!/usr/bin/env python3
"""
ThumbyOne — generate the partition-table JSON from per-slot sizes
and which slots are enabled.

This script + common/slot_layout.h share one source of truth: the
slot sizes below MUST equal the THUMBYONE_*_SIZE values in
slot_layout.h.  CMake invokes this script with the same
THUMBYONE_WITH_* flags it propagates to the C compile, so the
bootrom's partition table and the runtime's offset constants
always agree.

Usage:
  gen_pt.py [--with-md] [--no-nes|--no-p8|--no-doom|--no-mpy|--no-scumm]
            -o <out.json>

KB-based math.  Offsets / sizes are expressed in KB strings (per
the picotool tooling convention).
"""
import argparse
import json
import sys

# Keep these in sync with common/slot_layout.h
LOBBY_KB           = 128

NES_KB             = 1024
NES_KB_WITH_MD     = 2048

P8_KB              = 384
DOOM_KB            = 2432
MPY_KB             = 1280
SCUMM_KB           = 640
MOTE_RUNNER_KB     = 320    # Mote engine OS (no USB); games live on the FAT, not here
MOTE_LOBBY_KB      = 128    # Mote launcher + USB + FatFs (small; no 3D engine)
CRAFT_KB           = 480    # MUST match THUMBYONE_CRAFT_SIZE in common/slot_layout.h
                            # (trimmed 512->480 there; gen_pt was left stale, which
                            # pushed the FAT + any post-CRAFT slot 32 KB past the C view)
ROGUE_KB           = 512
ELITE_KB           = 256
ROGUE9_KB          = 512    # optional 9th slot

P8_SCRATCH_KB      = 252
SETTINGS_MIRROR_KB = 4

PERMS = {
    "secure": "rw",
    "nonsecure": "rw",
    "bootloader": "rw"
}


def part(name, pid, start_kb, size_kb):
    return {
        "name": name,
        "id": pid,
        "start": f"{start_kb}K",
        "size":  f"{size_kb}K",
        "families": ["absolute"],
        "permissions": PERMS,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--with-md",    action="store_true")
    ap.add_argument("--no-nes",     action="store_true")
    ap.add_argument("--no-p8",      action="store_true")
    ap.add_argument("--no-doom",    action="store_true")
    ap.add_argument("--no-mpy",     action="store_true")
    ap.add_argument("--no-scumm",   action="store_true")
    ap.add_argument("--no-mote",    action="store_true")
    ap.add_argument("--no-craft",   action="store_true")
    ap.add_argument("--no-rogue",   action="store_true")
    ap.add_argument("--no-elite",   action="store_true")
    ap.add_argument("--rogue9",     action="store_true",
                    help="append ThumbyRogue as a 9th slot after Elite")
    ap.add_argument("-o", "--output", required=True,
                    help="output partition-table JSON path")
    ap.add_argument("--cmake-out",
                    help="optional path to emit a CMake include file "
                         "with offset variables (mirrors slot_layout.h)")
    args = ap.parse_args()

    nes_size = NES_KB_WITH_MD if args.with_md else NES_KB

    partitions = []
    cur_kb = LOBBY_KB

    # Order matters — partition IDs are assigned by insertion order.
    if not args.no_nes:
        partitions.append(part("NES",   0, cur_kb, nes_size))
        cur_kb += nes_size
    if not args.no_p8:
        partitions.append(part("P8",    1, cur_kb, P8_KB))
        cur_kb += P8_KB
    if not args.no_doom:
        partitions.append(part("DOOM",  2, cur_kb, DOOM_KB))
        cur_kb += DOOM_KB
    if not args.no_mpy:
        partitions.append(part("MPY",   3, cur_kb, MPY_KB))
        cur_kb += MPY_KB
    if not args.no_scumm:
        partitions.append(part("SCUMM", 4, cur_kb, SCUMM_KB))
        cur_kb += SCUMM_KB
    # Mote = two partitions after SCUMM: RUNNER then LOBBY (order must match the
    # C-side thumbyone_slot_partition_id). Bootrom partition INDEX is the position
    # in this list; the "id" labels below are just unique.
    if not args.no_mote:
        partitions.append(part("MOTERUN", 9, cur_kb, MOTE_RUNNER_KB))
        cur_kb += MOTE_RUNNER_KB
        partitions.append(part("MOTELOB", 10, cur_kb, MOTE_LOBBY_KB))
        cur_kb += MOTE_LOBBY_KB
    if not args.no_craft:
        partitions.append(part("CRAFT", 5, cur_kb, CRAFT_KB))
        cur_kb += CRAFT_KB
    if not args.no_rogue:
        partitions.append(part("ROGUE", 6, cur_kb, ROGUE_KB))
        cur_kb += ROGUE_KB
    if not args.no_elite:
        partitions.append(part("ELITE", 7, cur_kb, ELITE_KB))
        cur_kb += ELITE_KB
    if args.rogue9:
        partitions.append(part("ROGUE9", 8, cur_kb, ROGUE9_KB))
        cur_kb += ROGUE9_KB

    pt = {
        "version": [1, 0],
        "unpartitioned": {
            "families": ["absolute"],
            "permissions": PERMS,
        },
        "partitions": partitions,
    }

    with open(args.output, "w") as f:
        json.dump(pt, f, indent=2)
        f.write("\n")

    # Optional CMake mirror — same offsets as the JSON, but as
    # set() commands the parent CMakeLists can include() to
    # configure UF2 rebase addresses without re-hardcoding hex
    # literals.  Variable names match the slot_layout.h macros.
    if args.cmake_out:
        # Recompute KB offsets for each partition that was emitted.
        offsets = {}
        sizes   = {}
        cur = LOBBY_KB
        if not args.no_nes:
            offsets["NES"] = cur; sizes["NES"] = nes_size; cur += nes_size
        if not args.no_p8:
            offsets["P8"]  = cur; sizes["P8"]  = P8_KB;    cur += P8_KB
        if not args.no_doom:
            offsets["DOOM"]= cur; sizes["DOOM"]= DOOM_KB;  cur += DOOM_KB
        if not args.no_mpy:
            offsets["MPY"] = cur; sizes["MPY"] = MPY_KB;   cur += MPY_KB
        if not args.no_scumm:
            offsets["SCUMM"]=cur; sizes["SCUMM"]=SCUMM_KB; cur += SCUMM_KB
        if not args.no_mote:
            offsets["MOTE_RUNNER"]=cur; sizes["MOTE_RUNNER"]=MOTE_RUNNER_KB; cur += MOTE_RUNNER_KB
            offsets["MOTE_LOBBY"] =cur; sizes["MOTE_LOBBY"] =MOTE_LOBBY_KB;  cur += MOTE_LOBBY_KB
        if not args.no_craft:
            offsets["CRAFT"]=cur; sizes["CRAFT"]=CRAFT_KB; cur += CRAFT_KB
        if not args.no_rogue:
            offsets["ROGUE"]=cur; sizes["ROGUE"]=ROGUE_KB; cur += ROGUE_KB
        if not args.no_elite:
            offsets["ELITE"]=cur; sizes["ELITE"]=ELITE_KB; cur += ELITE_KB
        if args.rogue9:
            offsets["ROGUE9"]=cur; sizes["ROGUE9"]=ROGUE9_KB; cur += ROGUE9_KB
        # P8 scratch + settings mirror reserved AFTER all slots, then
        # FAT consumes the rest of flash.
        fat_offset_kb = cur + P8_SCRATCH_KB + SETTINGS_MIRROR_KB
        fat_size_kb   = 16 * 1024 - fat_offset_kb
        with open(args.cmake_out, "w") as f:
            f.write("# Auto-generated by gen_pt.py — do not edit.\n")
            f.write("# Mirrors common/slot_layout.h offsets so CMake\n")
            f.write("# (UF2 rebase math, partition-id lookups) shares\n")
            f.write("# the same source-of-truth as the C side.\n")
            for name, off in offsets.items():
                f.write(f"set(THUMBYONE_{name}_OFFSET 0x{off*1024:08x})\n")
                f.write(f"set(THUMBYONE_{name}_SIZE   0x{sizes[name]*1024:08x})\n")
            f.write(f"set(THUMBYONE_FAT_OFFSET 0x{fat_offset_kb*1024:08x})\n")
            f.write(f"set(THUMBYONE_FAT_SIZE   0x{fat_size_kb*1024:08x})\n")

    # Sanity: report layout to stderr so the build log shows what
    # we generated.
    sys.stderr.write(
        f"  gen_pt: WITH_MD={int(args.with_md)} -> "
        f"{len(partitions)} partitions, "
        f"slots end at {cur_kb} KB, "
        f"FAT = {16 * 1024 - cur_kb - P8_SCRATCH_KB - SETTINGS_MIRROR_KB} KB"
        f"\n"
    )


if __name__ == "__main__":
    sys.exit(main() or 0)
