/*
 * ThumbyOne slot layout — flash offsets and sizes.
 *
 * Must stay in sync with common/pt.json. If you change the JSON,
 * change these too (and vice versa). The PT is authoritative for
 * the bootrom; these constants are for C code that needs to
 * reference slot regions without going through a runtime PT
 * lookup (e.g. the P8 active-cart scratch region, the shared
 * FAT volume, or the handoff sector).
 */
#ifndef THUMBYONE_SLOT_LAYOUT_H
#define THUMBYONE_SLOT_LAYOUT_H

#include <stdint.h>

/* --- Slot identifiers -------------------------------------------------
 * Partition IDs in pt.json. Also used as the 4-bit slot field in the
 * handoff magic (see thumbyone_handoff.h).
 *
 * SLOT_LOBBY is not a partition — it's the unpartitioned image at the
 * start of flash that bootrom picks by default. We still give it an ID
 * so code can talk about "returning to the lobby" symmetrically.
 */
typedef enum {
    THUMBYONE_SLOT_LOBBY = 0x0,   /* unpartitioned, flash offset 0 */
    THUMBYONE_SLOT_NES   = 0x1,   /* partition id 0 in pt.json */
    THUMBYONE_SLOT_P8    = 0x2,   /* partition id 1 */
    THUMBYONE_SLOT_DOOM  = 0x3,   /* partition id 2 */
    THUMBYONE_SLOT_MPY   = 0x4,   /* partition id 3 */
    THUMBYONE_SLOT_SCUMM = 0x5,   /* partition id 4 */
    THUMBYONE_SLOT_COUNT = 0x6
} thumbyone_slot_t;

/* Map a slot id to its partition id in pt.json. Only valid for
 * non-lobby slots. Returns -1 if not a launchable slot. */
static inline int thumbyone_slot_partition_id(thumbyone_slot_t s) {
    switch (s) {
        case THUMBYONE_SLOT_NES:   return 0;
        case THUMBYONE_SLOT_P8:    return 1;
        case THUMBYONE_SLOT_DOOM:  return 2;
        case THUMBYONE_SLOT_MPY:   return 3;
        case THUMBYONE_SLOT_SCUMM: return 4;
        default:                   return -1;
    }
}

/* --- Flash layout (keep in sync with pt.json) ------------------------
 * All offsets are BYTES from flash base (0x10000000 XIP).
 * XIP address = 0x10000000 + offset.
 */
#define THUMBYONE_FLASH_BASE          0x10000000u
#define THUMBYONE_FLASH_SIZE          (16u * 1024u * 1024u)

/* Reserved at the very start of flash for the lobby firmware and
 * the handoff sector. Lobby links at 0x10000000 with the PT
 * embedded; first flash sector carries the PT, the lobby code
 * follows. Room up to 128 KB before NES partition starts. */
#define THUMBYONE_LOBBY_OFFSET        0x000000u
#define THUMBYONE_LOBBY_MAX_SIZE      (128u * 1024u)

/* Optional 4 KB handoff sector for payload bigger than the
 * watchdog scratch registers can hold (e.g. MicroPython game
 * paths, P8 cart stems with sub-cart chain state). Lives inside
 * the lobby's flash region; only the lobby touches it. */
#define THUMBYONE_HANDOFF_SECTOR_OFFSET   0x010000u   /* 64 KB in */
#define THUMBYONE_HANDOFF_SECTOR_SIZE     (4u * 1024u)

/* Partition offsets and sizes — must match pt.json (or, when
 * THUMBYONE_WITH_MD is defined, pt_with_md.json) exactly.
 *
 * Two layouts:
 *   Default (THUMBYONE_WITH_MD undefined / 0):
 *     NES=1 MB, matches the original NES+SMS+GG+GB footprint. Used
 *     for backward-compat builds that want the full FAT.
 *   WITH_MD (THUMBYONE_WITH_MD=1):
 *     NES=2 MB to hold PicoDrive's precomputed YM2612/FAME/cz80
 *     flash tables (~850 KB). Every partition shifts up 1 MB.
 *
 * SCUMM slot adds a fixed 768 KB partition above MPY for the
 * ThumbyScummby engine (game data lives in the shared FAT, not
 * the slot itself).  This shifts P8 scratch + settings mirror +
 * FAT up by 768 KB unconditionally.  Net FAT sizes:
 *   Default:        FAT 8.9 MB (was 9.6 MB)
 *   WITH_MD:        FAT 7.9 MB (was 8.6 MB) */
#define THUMBYONE_NES_OFFSET          0x020000u   /* 128 KB — unchanged */
#if defined(THUMBYONE_WITH_MD) && THUMBYONE_WITH_MD
#  define THUMBYONE_NES_SIZE          (2048u * 1024u)
#  define THUMBYONE_P8_OFFSET         0x220000u   /* 2176 KB */
#  define THUMBYONE_P8_SIZE           (512u * 1024u)
#  define THUMBYONE_DOOM_OFFSET       0x2A0000u   /* 2688 KB */
#  define THUMBYONE_DOOM_SIZE         (2560u * 1024u)
#  define THUMBYONE_MPY_OFFSET        0x520000u   /* 5248 KB */
#  define THUMBYONE_MPY_SIZE          (2048u * 1024u)
#  define THUMBYONE_SCUMM_OFFSET      0x720000u   /* 7296 KB */
#  define THUMBYONE_SCUMM_SIZE        (768u * 1024u)
#else
#  define THUMBYONE_NES_SIZE          (1024u * 1024u)
#  define THUMBYONE_P8_OFFSET         0x120000u   /* 1152 KB */
#  define THUMBYONE_P8_SIZE           (512u * 1024u)
#  define THUMBYONE_DOOM_OFFSET       0x1A0000u   /* 1664 KB */
#  define THUMBYONE_DOOM_SIZE         (2560u * 1024u)
#  define THUMBYONE_MPY_OFFSET        0x420000u   /* 4224 KB */
#  define THUMBYONE_MPY_SIZE          (2048u * 1024u)
#  define THUMBYONE_SCUMM_OFFSET      0x620000u   /* 6272 KB */
#  define THUMBYONE_SCUMM_SIZE        (768u * 1024u)
#endif

/* P8 active-cart scratch region. Not a partition — P8 owns it,
 * erases and reprograms it per cart launch. Survives reboots
 * into any slot (the other slots never touch it).
 *
 * Note: the last 4 KB sector of this region is carved off as the
 * cross-slot settings mirror (see THUMBYONE_SETTINGS_MIRROR_*
 * below). P8 erases / programs only up to SIZE, leaving the mirror
 * untouched. Net P8 cart space: 252 KB — still plenty (the
 * biggest compressed cart we've seen is < 64 KB). */
#if defined(THUMBYONE_WITH_MD) && THUMBYONE_WITH_MD
#  define THUMBYONE_P8_SCRATCH_OFFSET 0x7E0000u   /* 8064 KB (shifted +768K for SCUMM slot) */
#else
#  define THUMBYONE_P8_SCRATCH_OFFSET 0x6E0000u   /* 7040 KB (shifted +768K for SCUMM slot) */
#endif
#define THUMBYONE_P8_SCRATCH_SIZE     (252u * 1024u)

/* Cross-slot settings mirror — one 4 KB flash sector that the
 * lobby + FatFs-enabled slots keep in sync with the tiny FatFs
 * /.volume and /.brightness files. DOOM has no FatFs so can't
 * read the files directly; it reads this mirror via XIP instead.
 *
 * Layout inside the sector (see thumbyone_settings_mirror.{c,h}):
 *   byte 0..3 : magic "TSM1"
 *   byte 4    : volume   (0..20)
 *   byte 5    : brightness (0..255)
 *   rest      : 0xFF padding (erased flash default)
 *
 * The mirror is written by thumbyone_settings_save_* after each
 * FatFs write. Readers (including DOOM) must tolerate a stale
 * mirror — the FatFs copy is the source of truth; the mirror is
 * just DOOM's view. */
#if defined(THUMBYONE_WITH_MD) && THUMBYONE_WITH_MD
#  define THUMBYONE_SETTINGS_MIRROR_OFFSET 0x81F000u   /* 8316 KB (shifted +768K) */
#else
#  define THUMBYONE_SETTINGS_MIRROR_OFFSET 0x71F000u   /* 7292 KB (shifted +768K) */
#endif
#define THUMBYONE_SETTINGS_MIRROR_SIZE    (4u * 1024u)

/* Shared FAT volume consumes the rest of flash. Not a partition —
 * lobby and each slot access it via the common FatFs diskio
 * driver as raw flash reads/writes. WITH_MD shifts the FAT up
 * by 1 MB to make room for the enlarged NES partition, reducing
 * the ROM-storage area from 9.6 MB to 8.6 MB. */
#if defined(THUMBYONE_WITH_MD) && THUMBYONE_WITH_MD
#  define THUMBYONE_FAT_OFFSET        0x820000u   /* 8320 KB (shifted +768K) */
#else
#  define THUMBYONE_FAT_OFFSET        0x720000u   /* 7296 KB (shifted +768K) */
#endif
#define THUMBYONE_FAT_SIZE            (THUMBYONE_FLASH_SIZE - THUMBYONE_FAT_OFFSET)

/* Convenience: XIP (read) addresses for each region. Use for
 * pointer reads; for flash-erase/program operations pass the
 * offset (not the XIP address) to the flash API. */
#define THUMBYONE_XIP(offset)  (THUMBYONE_FLASH_BASE + (offset))

#endif /* THUMBYONE_SLOT_LAYOUT_H */
