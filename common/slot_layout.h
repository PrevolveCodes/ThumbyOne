/*
 * ThumbyOne slot layout — single source of truth for flash regions.
 *
 * Layout is COMPUTED from per-slot sizes + which slots are enabled,
 * via the THUMBYONE_WITH_* compile-time flags propagated from the
 * top-level CMakeLists.  Every slot's C code includes this header
 * and uses the THUMBYONE_*_OFFSET / THUMBYONE_*_SIZE symbols
 * directly — there are no project-private hardcoded duplicates.
 *
 * The partition-table JSON (pt.json) embedded in the lobby is
 * GENERATED from the same flag set by tools/gen_pt.py, so the
 * bootrom and the runtime always agree.
 *
 * Add a new slot:
 *   1. Pick an ID + define THUMBYONE_<NAME>_SIZE below.
 *   2. Add it to the THUMBYONE_WITH_* flag list and the slot enum.
 *   3. Insert its offset block in the "Computed offsets" section.
 *   4. Update gen_pt.py to emit its partition entry.
 *
 * Resize an existing slot:
 *   Edit its THUMBYONE_<NAME>_SIZE constant.  Everything downstream
 *   (offsets, FAT size, generated PT) updates automatically.
 */
#ifndef THUMBYONE_SLOT_LAYOUT_H
#define THUMBYONE_SLOT_LAYOUT_H

#include <stdint.h>

/* --- Slot enables (driven by top-level CMake) ----------------------
 * Default ON so an in-tree experiment that doesn't pass the
 * defines still behaves like the full build. */
#ifndef THUMBYONE_WITH_NES
#  define THUMBYONE_WITH_NES   1
#endif
#ifndef THUMBYONE_WITH_P8
#  define THUMBYONE_WITH_P8    1
#endif
#ifndef THUMBYONE_WITH_DOOM
#  define THUMBYONE_WITH_DOOM  1
#endif
#ifndef THUMBYONE_WITH_MPY
#  define THUMBYONE_WITH_MPY   1
#endif
#ifndef THUMBYONE_WITH_SCUMM
#  define THUMBYONE_WITH_SCUMM 1
#endif
#ifndef THUMBYONE_WITH_CRAFT
#  define THUMBYONE_WITH_CRAFT 1
#endif
/* THUMBYONE_WITH_MD: NES partition grows from 1 MB to 2 MB to fit
 * PicoDrive's flash tables.  Affects only the NES slot's size. */
#ifndef THUMBYONE_WITH_MD
#  define THUMBYONE_WITH_MD    0
#endif

/* --- Slot identifiers -----------------------------------------------
 * Partition IDs are the 0-based index into the partition table that
 * tools/gen_pt.py emits.  Also used as the 4-bit slot field in the
 * watchdog-scratch handoff (see thumbyone_handoff.h). */
typedef enum {
    THUMBYONE_SLOT_LOBBY = 0x0,   /* unpartitioned, flash offset 0 */
    THUMBYONE_SLOT_NES   = 0x1,
    THUMBYONE_SLOT_P8    = 0x2,
    THUMBYONE_SLOT_DOOM  = 0x3,
    THUMBYONE_SLOT_MPY   = 0x4,
    THUMBYONE_SLOT_SCUMM = 0x5,
    THUMBYONE_SLOT_CRAFT = 0x6,
    THUMBYONE_SLOT_COUNT = 0x7
} thumbyone_slot_t;

/* Map a slot enum to its partition INDEX in pt.json — i.e. the index
 * the bootrom expects from rom_get_partition_table_info.  Indices
 * are assigned by gen_pt.py in slot-enable order: NES, P8, DOOM,
 * MPY, SCUMM (any disabled slot is skipped).  So the SCUMM partition
 * is at index 4 in the default all-on build but at index 0 in the
 * scumm-only preset.  The static all-on table the original code
 * used would chain to a non-existent partition on every preset that
 * dropped any of NES/P8/DOOM/MPY → bootrom reset_usb_boot fallback
 * → "blank screen" on slot launch.  This must read the same flags
 * (THUMBYONE_WITH_<X>=0|1, forwarded from CMake) that slot_layout.h
 * uses everywhere else; see the WITH_X fallback block at the top of
 * the file. */
static inline int thumbyone_slot_partition_id(thumbyone_slot_t s) {
    int idx = 0;
    if (s == THUMBYONE_SLOT_NES)   return THUMBYONE_WITH_NES   ? idx : -1;
    if (THUMBYONE_WITH_NES)   idx++;
    if (s == THUMBYONE_SLOT_P8)    return THUMBYONE_WITH_P8    ? idx : -1;
    if (THUMBYONE_WITH_P8)    idx++;
    if (s == THUMBYONE_SLOT_DOOM)  return THUMBYONE_WITH_DOOM  ? idx : -1;
    if (THUMBYONE_WITH_DOOM)  idx++;
    if (s == THUMBYONE_SLOT_MPY)   return THUMBYONE_WITH_MPY   ? idx : -1;
    if (THUMBYONE_WITH_MPY)   idx++;
    if (s == THUMBYONE_SLOT_SCUMM) return THUMBYONE_WITH_SCUMM ? idx : -1;
    if (THUMBYONE_WITH_SCUMM) idx++;
    if (s == THUMBYONE_SLOT_CRAFT) return THUMBYONE_WITH_CRAFT ? idx : -1;
    return -1;
}

/* --- Flash dimensions ---------------------------------------------- */
#define THUMBYONE_FLASH_BASE          0x10000000u
#define THUMBYONE_FLASH_SIZE          (16u * 1024u * 1024u)

/* --- Per-slot sizes (edit here to resize a slot) ------------------- */
#define THUMBYONE_LOBBY_MAX_SIZE      (128u * 1024u)

/* Slot sizes are tuned to the actual measured binary footprint plus a
 * comfortable growth margin (see commit message for the audit table).
 * Bumping a slot's size shifts every downstream slot + the FAT, so
 * keep the chain compact. */
/* Slot sizes are tuned to the measured binary footprint plus a
 * comfortable growth margin.  Bumping a slot's size shifts every
 * downstream slot + the FAT, so keep the chain compact. */
#if THUMBYONE_WITH_MD
#  define THUMBYONE_NES_SIZE          (2048u * 1024u)   /* binary ~1.88 MB + 120 KB headroom */
#else
#  define THUMBYONE_NES_SIZE          (1024u * 1024u)   /* binary ~0.86 MB + 160 KB headroom */
#endif

#define THUMBYONE_P8_SIZE             ( 384u * 1024u)   /* binary ~0.30 MB +  75 KB headroom */
#define THUMBYONE_DOOM_SIZE           (2432u * 1024u)   /* binary ~2.27 MB + 110 KB headroom */
#define THUMBYONE_MPY_SIZE            (1280u * 1024u)   /* binary ~0.93 MB + 320 KB headroom */
#define THUMBYONE_SCUMM_SIZE          ( 640u * 1024u)   /* binary ~0.53 MB + 100 KB headroom (still growing) */
#define THUMBYONE_CRAFT_SIZE          ( 512u * 1024u)   /* binary ~0.37 MB + ~140 KB headroom (~38% margin) */

/* P8's active-cart flash region — 256 KB total, with the last 4 KB
 * sector reserved as the cross-slot settings mirror.  NOT a
 * partition: P8 erases/programs it directly via flash_range_*. */
#define THUMBYONE_P8_SCRATCH_SIZE     (252u * 1024u)
#define THUMBYONE_SETTINGS_MIRROR_SIZE (4u * 1024u)

/* --- Lobby reserved region ----------------------------------------- */
#define THUMBYONE_LOBBY_OFFSET        0x000000u

/* Optional 4 KB handoff sector for payload bigger than the watchdog
 * scratch registers can hold.  Lives inside the lobby's flash
 * region. */
#define THUMBYONE_HANDOFF_SECTOR_OFFSET   0x010000u   /* 64 KB in */
#define THUMBYONE_HANDOFF_SECTOR_SIZE     (4u * 1024u)

/* --- Computed offsets (do not edit — derive from sizes + enables) -- */
#define THUMBYONE_LOBBY_END           (THUMBYONE_LOBBY_OFFSET + THUMBYONE_LOBBY_MAX_SIZE)

#if THUMBYONE_WITH_NES
#  define THUMBYONE_NES_OFFSET        THUMBYONE_LOBBY_END
#  define THUMBYONE_NES_END           (THUMBYONE_NES_OFFSET + THUMBYONE_NES_SIZE)
#else
#  define THUMBYONE_NES_END           THUMBYONE_LOBBY_END
#endif

#if THUMBYONE_WITH_P8
#  define THUMBYONE_P8_OFFSET         THUMBYONE_NES_END
#  define THUMBYONE_P8_END            (THUMBYONE_P8_OFFSET + THUMBYONE_P8_SIZE)
#else
#  define THUMBYONE_P8_END            THUMBYONE_NES_END
#endif

#if THUMBYONE_WITH_DOOM
#  define THUMBYONE_DOOM_OFFSET       THUMBYONE_P8_END
#  define THUMBYONE_DOOM_END          (THUMBYONE_DOOM_OFFSET + THUMBYONE_DOOM_SIZE)
#else
#  define THUMBYONE_DOOM_END          THUMBYONE_P8_END
#endif

#if THUMBYONE_WITH_MPY
#  define THUMBYONE_MPY_OFFSET        THUMBYONE_DOOM_END
#  define THUMBYONE_MPY_END           (THUMBYONE_MPY_OFFSET + THUMBYONE_MPY_SIZE)
#else
#  define THUMBYONE_MPY_END           THUMBYONE_DOOM_END
#endif

#if THUMBYONE_WITH_SCUMM
#  define THUMBYONE_SCUMM_OFFSET      THUMBYONE_MPY_END
#  define THUMBYONE_SCUMM_END         (THUMBYONE_SCUMM_OFFSET + THUMBYONE_SCUMM_SIZE)
#else
#  define THUMBYONE_SCUMM_END         THUMBYONE_MPY_END
#endif

#if THUMBYONE_WITH_CRAFT
#  define THUMBYONE_CRAFT_OFFSET      THUMBYONE_SCUMM_END
#  define THUMBYONE_CRAFT_END         (THUMBYONE_CRAFT_OFFSET + THUMBYONE_CRAFT_SIZE)
#else
#  define THUMBYONE_CRAFT_END         THUMBYONE_SCUMM_END
#endif

/* P8 owns the next 256 KB (active-cart scratch + settings mirror).
 * Always reserved even when WITH_P8=0 — keeps the FAT base
 * deterministic across slot-enable permutations.  When WITH_P8=0,
 * the scratch sits unused; the 4 KB settings mirror still works
 * because the lobby + slots reference it directly. */
#define THUMBYONE_P8_SCRATCH_OFFSET   THUMBYONE_CRAFT_END
#define THUMBYONE_SETTINGS_MIRROR_OFFSET \
    (THUMBYONE_P8_SCRATCH_OFFSET + THUMBYONE_P8_SCRATCH_SIZE)

/* Shared FAT volume consumes the rest of flash. */
#define THUMBYONE_FAT_OFFSET \
    (THUMBYONE_SETTINGS_MIRROR_OFFSET + THUMBYONE_SETTINGS_MIRROR_SIZE)
#define THUMBYONE_FAT_SIZE  (THUMBYONE_FLASH_SIZE - THUMBYONE_FAT_OFFSET)

/* Convenience: XIP (read) addresses for each region.  Use for
 * pointer reads; for flash-erase/program operations pass the
 * offset (not the XIP address) to the flash API. */
#define THUMBYONE_XIP(offset)  (THUMBYONE_FLASH_BASE + (offset))

#endif /* THUMBYONE_SLOT_LAYOUT_H */
