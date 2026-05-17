/*
 * ThumbyOne lobby — shared-FAT cluster-level defragmenter.
 *
 * Ported from ThumbyNES (device/nes_picker.c). Compacts the shared
 * FAT volume so heavily-fragmented game data files become physically
 * contiguous on flash. ThumbyScummby in particular reads game files
 * via XIP pointers (file's first cluster → XIP base + offset) which
 * requires the cluster chain to be contiguous; on a near-full /
 * heavily-shuffled volume some files (e.g. /scumm/indy4/atlantis.001)
 * end up with thousands of fragmentation gaps and stop being usable
 * through that fast path.
 *
 * The algorithm works at PHYSICAL cluster granularity (cycle sort
 * over the cluster owners + a full FAT rebuild), not at file level —
 * a file-level rewrite can't help when free space is smaller than the
 * file you want to defragment. See the top of lobby_defrag.c for the
 * full design notes.
 *
 * Lifecycle:
 *   1. Caller mounts the shared FAT (lobby already does this).
 *   2. Caller passes the FATFS pointer in via lobby_defrag_set_fs().
 *   3. Caller invokes lobby_defrag_compact(fb). The function takes
 *      over the LCD for the duration of the pass, draws a preview
 *      cluster map, waits for the user to confirm (A) or cancel (B),
 *      then performs the moves. Returns:
 *        >= 0  : number of clusters rewritten (0 = nothing to do)
 *        <  0  : analysis or execution failed; lobby_defrag_last_error
 *                returns a more specific code in the execute case.
 *   4. Lobby refreshes its home screen.
 *
 * Memory: heap-only — every working buffer (per-cluster owner maps,
 * cycle pivot buffers, file table, FAT image) is malloc'd at the
 * start of lobby_defrag_compact and freed before return. Approx peak
 * is ~200 KB while running; nothing persistent.
 */
#ifndef LOBBY_DEFRAG_H
#define LOBBY_DEFRAG_H

#include <stddef.h>
#include <stdint.h>

#include "ff.h"

/* Tell the defragger where to find the lobby's mounted FATFS. Must
 * be called before lobby_defrag_compact / count_fragmented. */
void lobby_defrag_set_fs(FATFS *fs);

/* Walk the shared FAT and count files whose cluster chain isn't
 * contiguous (skips files < 4 KB — single-cluster files are
 * trivially contiguous). */
int  lobby_defrag_count_fragmented(void);

/* Same as count_fragmented but also writes the first fragmented
 * file's path into out_first (truncated to out_sz). Useful for
 * surfacing what's blocking XIP mmap. */
int  lobby_defrag_count_fragmented_named(char *out_first, size_t out_sz);

/* The main entry point. Takes over the 128×128 RGB565 framebuffer
 * for the duration of the pass. Renders a preview cluster map, waits
 * for user confirmation, then performs the in-place cycle-sort
 * compaction. */
int  lobby_defrag_compact(uint16_t *fb);

/* Return code of the most recent execute failure (cluster-level
 * write error, FAT rebuild error, directory patch error, remount
 * error). 0 if the last pass succeeded or never executed. */
int  lobby_defrag_last_error(void);

#endif /* LOBBY_DEFRAG_H */
