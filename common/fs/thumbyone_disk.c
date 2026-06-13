/*
 * ThumbyOne shared-FAT flash block device — direct ops, no cache.
 *
 * Reads: pull straight from the XIP-mapped FAT region. ATRANS[1]
 * (identity mapping for physical 0x400000..0x800000, configured
 * in thumbyone_slot_init for slots and inherent for the lobby)
 * makes this just a memcpy from XIP_BASE + offset.
 *
 * Writes: full erase-program cycles. Each 4 KB touched flash
 * sector is RMW'd — read current contents from XIP into a stack
 * buffer, splat in the sectors being written, erase the flash
 * block, program back. Interrupts are disabled around each
 * flash op (SDK's flash_range_erase / flash_range_program do
 * it internally) and ATRANS + fast-XIP config are restored
 * after each op (the bootrom's flash routines reset both).
 *
 * No caching here — the lobby's MSC path can layer a write-back
 * cache on top for burst-write smoothing. Slots and the lobby's
 * own mount-time reads use this directly.
 */
#include "thumbyone_disk.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "slot_layout.h"
#include "thumbyone_handoff.h"    /* for thumbyone_xip_fast_setup */

#define XIP_BASE_ADDR 0x10000000u

/* Sectors per flash erase block — 4096 / 512 = 8. */
#define SECTORS_PER_ERASE   (THUMBYONE_DISK_ERASE_SIZE / THUMBYONE_DISK_SECTOR_SIZE)


uint32_t thumbyone_disk_sector_count(void) {
    return THUMBYONE_FAT_SIZE / THUMBYONE_DISK_SECTOR_SIZE;
}

uint32_t thumbyone_disk_sector_size(void) {
    return THUMBYONE_DISK_SECTOR_SIZE;
}

/* Compute the XIP virtual address that reads from the given byte
 * offset within the shared FAT.  The virtual XIP space is divided
 * into four 4 MB windows by ATRANS[0..3]:
 *
 *   ATRANS[0]:  virtual 0..4 MB    — the slot's own partition
 *                                    (BASE = slot's physical offset,
 *                                     SIZE widened to 0x400 / 4 MB
 *                                     by thumbyone_slot_init).
 *   ATRANS[1]:  virtual 4..8 MB    — identity over physical 4..8 MB
 *   ATRANS[2]:  virtual 8..12 MB   — identity over physical 8..12 MB
 *   ATRANS[3]:  virtual 12..16 MB  — identity over physical 12..16 MB
 *
 * For any given FAT byte we pick whichever window can reach it.
 * Specifically:
 *
 *  - The portion of FAT that lies within (slot_phys, slot_phys + 4MB)
 *    is reachable via SLOT-RELATIVE virtual addressing through
 *    ATRANS[0].  The virtual offset within the window is
 *    (FAT_OFFSET - slot_phys_base) + byte_offset; if that's < 4 MB,
 *    ATRANS[0]'s BASE field translates it back to the right physical
 *    FAT byte.
 *
 *  - Anything past that uses ABSOLUTE virtual addressing
 *    (XIP_BASE + FAT_OFFSET + byte_offset), which lands in
 *    ATRANS[1..3]'s identity-mapped range.
 *
 * The runtime branch is on the *resulting* virtual address, not on
 * the FAT base location — this matters because FAT can extend
 * across the 4 MB ATRANS[0] boundary (scumm-only preset: FAT base
 * 1 MB, end 16 MB → first ~3.1 MB of FAT goes through ATRANS[0],
 * the rest through ATRANS[1..3]). */
static inline const uint8_t *thumbyone_fat_xip_addr(uint32_t byte_offset) {
    uint32_t atrans0 = qmi_hw->atrans[0];
    uint32_t atrans1 = qmi_hw->atrans[1];
    uint32_t slot_4kb       = atrans0 & 0xFFFu;
    uint32_t slot_phys_base = slot_4kb * 4096u;
    uint32_t a1_base        = atrans1 & 0xFFFu;
    uint32_t expected_cont  = (slot_4kb + 0x400u) & 0xFFFu;

    /* SCUMM slot reconfigures ATRANS[1..3] as continuous extensions
     * of ATRANS[0] so a single virtual base can address all 16 MB
     * of flash slot-relative.  When that pattern is in effect,
     * always use slot-relative — the absolute path would land on
     * the wrong physical bytes because ATRANS[1..3] no longer
     * identity-map. */
    if (a1_base == expected_cont) {
        return (const uint8_t *)(XIP_BASE_ADDR
                                  + (THUMBYONE_FAT_OFFSET - slot_phys_base)
                                  + byte_offset);
    }

    /* Default layout: ATRANS[1..3] are identity over physical
     * 4..16 MB.  Use slot-relative for early FAT bytes that fall
     * inside ATRANS[0]'s window, absolute otherwise. */
    uint32_t slot_relative_virt = (THUMBYONE_FAT_OFFSET - slot_phys_base)
                                  + byte_offset;
    if (slot_relative_virt < 0x400000u) {
        return (const uint8_t *)(XIP_BASE_ADDR + slot_relative_virt);
    }
    return (const uint8_t *)(XIP_BASE_ADDR
                              + THUMBYONE_FAT_OFFSET
                              + byte_offset);
}


/* ATRANS snapshot helpers — the SDK's flash routines reset QMI
 * during erase/program, including the identity mappings our
 * chained slot needs for this FAT region. Save on the way in,
 * restore (plus fast-XIP config) on the way out. */
static inline void save_atrans(uint32_t out[4]) {
    out[0] = qmi_hw->atrans[0];
    out[1] = qmi_hw->atrans[1];
    out[2] = qmi_hw->atrans[2];
    out[3] = qmi_hw->atrans[3];
}

static inline void restore_atrans(const uint32_t in[4]) {
    qmi_hw->atrans[0] = in[0];
    qmi_hw->atrans[1] = in[1];
    qmi_hw->atrans[2] = in[2];
    qmi_hw->atrans[3] = in[3];
    /* Always re-establish fast QPI XIP after a flash op. This
     * applies equally to lobby and slot builds: the slots need
     * it because they run in QPI mode post-handoff and the SDK
     * flash op resets QMI to single-SPI, and the lobby needs it
     * because its SUBSEQUENT flash ops (and ultimately the slot
     * chain-image handoff) depend on flash being in a consistent
     * fast-XIP state throughout. Gating this call out of the lobby
     * build caused DOOM to drop back to single-SPI performance
     * after the lobby's first flash write and MPY slot games to
     * fail to launch. */
    thumbyone_xip_fast_setup();
}


int thumbyone_disk_read(uint8_t *dst, uint32_t sector, uint32_t count) {
    if (sector + count > thumbyone_disk_sector_count()) return -1;
    memcpy(dst,
           thumbyone_fat_xip_addr(sector * THUMBYONE_DISK_SECTOR_SIZE),
           count * THUMBYONE_DISK_SECTOR_SIZE);
    return 0;
}


/* Commit one 4 KB flash block. `block_idx` is the index within
 * the FAT region (block 0 is flash offset THUMBYONE_FAT_OFFSET,
 * block 1 is +4096, etc.). `buf` must be exactly 4 KB. */
static int commit_block(uint32_t block_idx, const uint8_t *buf) {
    uint32_t flash_off = THUMBYONE_FAT_OFFSET +
                         block_idx * THUMBYONE_DISK_ERASE_SIZE;

    /* Erase — IRQs off for ~50 ms. */
    {
        uint32_t ints = save_and_disable_interrupts();
        uint32_t saved[4];
        save_atrans(saved);
        flash_range_erase(flash_off, THUMBYONE_DISK_ERASE_SIZE);
        restore_atrans(saved);
        restore_interrupts(ints);
    }

    /* Program — split into 256-byte page programs so we can let
     * IRQs breathe between pages. Each program is ~1 ms with IRQs
     * off. This matches ThumbyNES's proven pattern — without the
     * split, a single 4 KB commit keeps IRQs off for 16 ms which
     * is enough to starve USB (not an issue here since MSC isn't
     * on this path, but keep the discipline for when it is). */
    const uint32_t PROG_CHUNK = 256u;
    for (uint32_t off = 0; off < THUMBYONE_DISK_ERASE_SIZE; off += PROG_CHUNK) {
        uint32_t ints = save_and_disable_interrupts();
        uint32_t saved[4];
        save_atrans(saved);
        flash_range_program(flash_off + off, buf + off, PROG_CHUNK);
        restore_atrans(saved);
        restore_interrupts(ints);
    }

    /* Verify — read back through XIP and confirm the program
     * landed. Anything but an exact match means the flash chip
     * didn't accept the write, or ATRANS is pointing somewhere
     * wrong. Either way the caller needs to know. */
    const uint8_t *xip = thumbyone_fat_xip_addr(
        block_idx * THUMBYONE_DISK_ERASE_SIZE);
    if (memcmp(xip, buf, THUMBYONE_DISK_ERASE_SIZE) != 0) {
        return -1;
    }
    return 0;
}


int thumbyone_disk_write(const uint8_t *src, uint32_t sector, uint32_t count) {
    if (sector + count > thumbyone_disk_sector_count()) return -1;

    /* Walk the write range one 4 KB erase block at a time. For each
     * block, assemble the merged contents (existing XIP outside the
     * write range + incoming bytes inside) in a buffer, then commit.
     *
     * Slots with a small stack (ELITE runs a 4 KB SCRATCH_X stack) blow
     * it when a save's call chain reaches here with this 4 KB local on
     * top — that was the "save-write hang". Those slots define
     * THUMBYONE_DISK_STATIC_MERGE_BUF to move it to .bss instead (they
     * have the spare RAM; RAM-tight slots like CRAFT keep it on the
     * stack). Flash writes are serialised on one core, so a single
     * shared static buffer is safe. */
#ifdef THUMBYONE_DISK_STATIC_MERGE_BUF
    static uint8_t buf[THUMBYONE_DISK_ERASE_SIZE];
#else
    uint8_t buf[THUMBYONE_DISK_ERASE_SIZE];
#endif

    uint32_t remaining = count;
    uint32_t cur_sector = sector;
    const uint8_t *cur_src = src;

    while (remaining > 0) {
        uint32_t block_idx      = cur_sector / SECTORS_PER_ERASE;
        uint32_t sector_in_blk  = cur_sector % SECTORS_PER_ERASE;
        uint32_t sectors_in_blk = SECTORS_PER_ERASE - sector_in_blk;
        if (sectors_in_blk > remaining) sectors_in_blk = remaining;

        /* Seed the block buffer from XIP (the pre-existing 4 KB). */
        memcpy(buf,
               thumbyone_fat_xip_addr(block_idx * THUMBYONE_DISK_ERASE_SIZE),
               THUMBYONE_DISK_ERASE_SIZE);

        /* Overlay the sectors being written. */
        memcpy(buf + sector_in_blk * THUMBYONE_DISK_SECTOR_SIZE,
               cur_src,
               sectors_in_blk * THUMBYONE_DISK_SECTOR_SIZE);

        if (commit_block(block_idx, buf) != 0) return -1;

        cur_sector += sectors_in_blk;
        cur_src    += sectors_in_blk * THUMBYONE_DISK_SECTOR_SIZE;
        remaining  -= sectors_in_blk;
    }
    return 0;
}


int thumbyone_disk_sync(void) {
    /* No cache at this layer → nothing to flush. A cached layer
     * (lobby MSC) would override or wrap this. */
    return 0;
}
