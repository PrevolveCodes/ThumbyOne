/*
 * ThumbyOne lobby — shared-FAT cluster-level defragmenter.
 *
 * Lifted from ThumbyNES/device/nes_picker.c (the cluster-level path
 * from "enum { CLD_MAX_FILES … }" through nes_picker_defrag_compact),
 * with ThumbyOne's platform APIs substituted:
 *
 *   nes_flash_disk_read  →  thumbyone_disk_read
 *   nes_flash_disk_write →  thumbyone_disk_write
 *   nes_flash_disk_flush →  thumbyone_disk_sync
 *   nes_buttons_*        →  GPIO polling matching lobby_main.c's idiom
 *   nes_led_red / off    →  dropped (lobby_main owns the front LED via
 *                            its own pwm helpers — and the defrag UI
 *                            already paints a loud red banner)
 *   tud_task()           →  lobby_usb_task() (pumps tinyUSB MSC too)
 *   g_fs                 →  module-local FATFS pointer set by the lobby
 *                            via lobby_defrag_set_fs() before calling
 *
 * The file-level defrag (defrag_one / defrag_one_path / via_scratch)
 * is intentionally omitted: on the lobby's near-full 9.6 MB shared
 * FAT, free space < file size for the big game data files, so only
 * the cluster-level path can actually consolidate anything.
 *
 * Algorithm summary (full details in ThumbyNES history):
 *
 *   1. ANALYZE: BFS the directory tree, building a flat file table.
 *      For each file (and subdir) record current_sclust, n_clusters,
 *      and the (parent, byte_offset) of its 32-byte directory entry.
 *      Walk every chain to build a per-cluster current_owner[] map.
 *      Pin orphan clusters (FAT entry says in-use but no directory
 *      entry claims it) so we don't lose data on a partial walk.
 *      Run an attribution scan to attach filenames to pins; truly
 *      unowned pins are released as reclaimable storage.
 *
 *   2. PLAN: enumerate block-shift subsets (up to 2^16) to find the
 *      target layout that minimises writes while maximising the
 *      largest contiguous free run. K-weight is user-adjustable from
 *      the preview screen. Fragmented files are best-fit-decreasing
 *      placed into the resulting runs. Fallback planner: size-DESC
 *      pack-left from cluster 0.
 *
 *   3. EXECUTE (after the user confirms with A):
 *      a. Cycle sort: walk each cluster, follow the permutation
 *         current_owner → target_owner via two 4 KB RAM pivots until
 *         the cycle closes.
 *      b. Rebuild FAT from scratch (contiguous chain per placed file;
 *         preserve pinned cluster chains as-is).
 *      c. Patch every moved file/subdir's SFN slot in its parent;
 *         then patch subdirectories' `.` and `..` entries.
 *      d. Remount FatFs so all chain caches refresh.
 */

#include "lobby_defrag.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "ff.h"
#include "thumbyone_disk.h"
#include "lcd_gc9107.h"
#include "font.h"
#include "lobby_usb.h"

/* ---- platform glue --------------------------------------------- */

/* The lobby owns the shared FATFS object; we get a pointer to it via
 * lobby_defrag_set_fs() before any of our public entry points run.
 * Internally we still use a `g_fs` macro so the lifted code reads
 * identically to the ThumbyNES original (where g_fs is an extern in
 * the same translation unit). */
static FATFS *s_lobby_fs;
void lobby_defrag_set_fs(FATFS *fs) { s_lobby_fs = fs; }
#define g_fs (*s_lobby_fs)

/* Disk I/O rename — every call site below uses the original names. */
#define nes_flash_disk_read(buf, lba, n)  thumbyone_disk_read((buf), (lba), (n))
#define nes_flash_disk_write(buf, lba, n) thumbyone_disk_write((buf), (lba), (n))
#define nes_flash_disk_flush()            thumbyone_disk_sync()

/* USB pump — the lobby's task helper also drives its MSC cache. */
#define tud_task() lobby_usb_task()

/* Framebuffer dimensions — fixed for the Thumby Color LCD. */
#define FB_W 128
#define FB_H 128

/* Path / file caps. The cluster-level code carries its own
 * CLD_PATH_MAX which is the only one that mattered upstream. */
#define LOBBY_DEFRAG_NAME_MAX  96

/* ---- input polling matching lobby_main.c -----------------------
 *
 * The lobby reads the D-pad / face buttons directly through GPIO
 * (pull-up, active-low). We replicate the same pin map locally so
 * the defrag preview's A/B/LEFT/RIGHT handling doesn't need an extra
 * dependency on lobby_main internals. lobby_main.c initialises the
 * GPIOs at boot, so by the time lobby_defrag_compact runs we just
 * read them. */
#define LD_PIN_LEFT  0
#define LD_PIN_UP    1
#define LD_PIN_RIGHT 2
#define LD_PIN_DOWN  3
#define LD_PIN_LB    6
#define LD_PIN_A    21
#define LD_PIN_RB   22
#define LD_PIN_B    25
#define LD_PIN_MENU 26

static inline bool ld_btn(int pin) { return !gpio_get(pin); }

/* Packed buttons-read returning the same bits the upstream code
 * cares about: 0x01=LEFT, 0x02=RIGHT, 0x10=B, 0x20=A. (UP/DOWN aren't
 * polled in the preview; LB/RB likewise.) */
static uint8_t lobby_defrag_buttons_read(void) {
    uint8_t b = 0;
    if (ld_btn(LD_PIN_LEFT))  b |= 0x01;
    if (ld_btn(LD_PIN_RIGHT)) b |= 0x02;
    if (ld_btn(LD_PIN_B))     b |= 0x10;
    if (ld_btn(LD_PIN_A))     b |= 0x20;
    return b;
}
static inline bool lobby_defrag_menu_pressed(void) { return ld_btn(LD_PIN_MENU); }

/* ---- framebuffer helpers --------------------------------------- */

static void fb_clear(uint16_t *fb, uint16_t c) {
    for (int i = 0; i < FB_W * FB_H; i++) fb[i] = c;
}
static void fb_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= FB_H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if ((unsigned)xx >= FB_W) continue;
            fb[yy * FB_W + xx] = c;
        }
    }
}

#define COL_BG     0x0000   /* black */
#define COL_FG     0xFFFF   /* white */
#define COL_DIM    0x8410   /* mid grey */
#define COL_HIGHLT 0x07E0   /* green */
#define COL_TITLE  0xFD20   /* orange */
#define COL_ERR    0xF800   /* red */

/* ---- FAT12/16 entry access ------------------------------------- *
 * The shared ThumbyOne volume is FAT12 (4 KB clusters, < 4085 clusters); the
 * lighter >8 MB presets can be FAT16. The defrag's in-memory plan stores each
 * cluster's "next" as a uint16 — fine for both (FAT12's max cluster 4084 fits,
 * and 0xFFFF truncates to 0xFFF = a valid FAT12 EOC when packed) — so only the
 * on-disk read/write needs to know the entry width. fat_get reads one entry
 * (12- or 16-bit, with a 1-sector cache; the two bytes may straddle a sector);
 * fat_eoc / fat_bad test the end-of-chain and bad-cluster markers. */
static uint8_t s_fatget_sec[512];
static LBA_t   s_fatget_lba = (LBA_t)-1;
static void  fat_get_flush(void) { s_fatget_lba = (LBA_t)-1; }   /* call after any FAT write */
static int   fat_is12(void)      { return g_fs.fs_type == FS_FAT12; }
static int   fat_eoc(DWORD v)    { return fat_is12() ? (v >= 0x0FF8u)  : (v >= 0xFFF8u); }
static int   fat_bad(DWORD v)    { return v == (fat_is12() ? 0x0FF7u : 0xFFF7u); }

static DWORD fat_get(DWORD clst) {
    int   is12 = fat_is12();
    DWORD bo   = is12 ? (clst + (clst >> 1)) : (clst * 2);   /* entry byte offset in the FAT */
    DWORD b[2];
    for (int k = 0; k < 2; k++) {                            /* two bytes; FAT12 may straddle a sector */
        DWORD bb = bo + (DWORD)k;
        LBA_t l  = (LBA_t)g_fs.fatbase + bb / 512;
        if (l != s_fatget_lba) {
            if (nes_flash_disk_read(s_fatget_sec, (uint32_t)l, 1) != 0) return 0xFFFFFFFFu;
            s_fatget_lba = l;
        }
        b[k] = s_fatget_sec[bb % 512];
    }
    DWORD v = b[0] | (b[1] << 8);
    if (is12) v = (clst & 1) ? (v >> 4) : (v & 0x0FFFu);     /* odd: high 12 bits; even: low 12 */
    return v;
}

/* ---- chain_is_contiguous probe --------------------------------- */

/* Walk the FAT chain starting at `start_cluster` and verify it is
 * physically contiguous. Returns 1 if every cluster in the chain is
 * the previous cluster + 1; 0 otherwise. Used by the analyzer's
 * fragment detection AND the preview's before/after counters so both
 * agree. */
static int chain_is_contiguous(DWORD start_cluster, DWORD n_clusters) {
    if (n_clusters <= 1) return 1;
    DWORD prev = start_cluster;
    for (DWORD i = 1; i < n_clusters; i++) {
        DWORD next = fat_get(prev);
        if (next == 0xFFFFFFFFu) return 0;                 /* read error */
        if (fat_eoc(next)) return (i == n_clusters - 1);
        if (next != prev + 1) return 0;
        prev = next;
    }
    return 1;
}

/* =================================================================
 * Cluster-level defragmenter (lifted from nes_picker.c).
 * ================================================================= */

enum {
    CLD_MAX_FILES        = 2000,
    CLD_MAX_DIRS         = 128,
    CLD_PATH_MAX         = 192,
    CLD_ABS_MAX_CLUSTERS = 16384,
};

typedef struct {
    DWORD    current_sclust;
    DWORD    target_sclust;       /* 0 = couldn't place, leave alone */
    uint32_t n_clusters;
    int      parent_idx;          /* -1 = lives in root dir */
    DWORD    byte_in_parent;
    uint8_t  is_dir;
} cld_file_t;

typedef struct {
    cld_file_t *files;
    int         n_files;
    uint32_t   *current_owner;
    uint32_t   *target_owner;
    uint8_t    *pinned_bits;
    uint8_t    *done_bits;
    uint8_t    *buf_a;
    uint8_t    *buf_b;
    DWORD       n_clust;
    DWORD       bpc;
} cld_ctx_t;

#define CLD_BIT_GET(arr, i)  ((arr)[(i) >> 3] & (1u << ((i) & 7)))
#define CLD_BIT_SET(arr, i)  ((arr)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))
#define CLD_BIT_CLR(arr, i)  ((arr)[(i) >> 3] &= (uint8_t)~(1u << ((i) & 7)))

/* Pin-owner diagnostic state — populated by the orphan-attribution
 * scan during analyze, freed before lobby_defrag_compact returns. */
#define CLD_MAX_PIN_NAMES 64
typedef struct {
    DWORD cluster;
    char  name[CLD_PATH_MAX];
} cld_pin_name_t;
static cld_pin_name_t *s_pin_names = NULL;
static int             s_n_pin_names = 0;
static int             s_pin_display_idx = 0;
static int             s_n_reclaimed   = 0;
static uint32_t        s_reclaimed_kb  = 0;

static int s_defrag_last_error = 0;
int lobby_defrag_last_error(void) { return s_defrag_last_error; }

/* Walk a FAT chain (FAT12/16) starting at sclust, invoking cb for each
 * cluster. Returns the number of clusters walked. */
static int cld_walk_fat(DWORD sclust,
                         void (*cb)(DWORD clst, void *arg),
                         void *cb_arg) {
    if (sclust < 2 || sclust >= g_fs.n_fatent) return 0;
    DWORD clst = sclust;
    int n = 0;
    while (clst >= 2 && clst < g_fs.n_fatent && n < CLD_ABS_MAX_CLUSTERS) {
        cb(clst, cb_arg);
        n++;
        DWORD next = fat_get(clst);
        if (next == 0xFFFFFFFFu || fat_eoc(next) || next < 2) break;
        clst = next;
    }
    return n;
}

typedef struct {
    cld_ctx_t *ctx;
    int        fidx;
    uint32_t   off;
} cld_owner_arg_t;

static void cld_cb_set_owner(DWORD clst, void *arg) {
    cld_owner_arg_t *oa = (cld_owner_arg_t *)arg;
    DWORD ci = clst - 2;
    if (ci < oa->ctx->n_clust) {
        oa->ctx->current_owner[ci] = ((uint32_t)(oa->fidx + 1) << 16) | (oa->off & 0xFFFF);
    }
    oa->off++;
}

/* --- target-layout planner: block-subset search ----------------- */

#define CLD_SCORE_REGRESS_P    1000000
#define CLD_BLOCK_EXHAUSTIVE   16

static const int s_plan_k_values[] = { 0, 1, 2, 3, 5, 8, 13, 25, 50, 100, 200, 500, -1 };
#define CLD_PLAN_K_COUNT ((int)(sizeof(s_plan_k_values) / sizeof(s_plan_k_values[0])))
#define CLD_PLAN_K_DEFAULT_IDX 4
#define CLD_PLAN_K_IS_PACK_LEFT(v) ((v) < 0)
static int s_plan_k_idx = CLD_PLAN_K_DEFAULT_IDX;

typedef struct {
    DWORD    start;
    uint32_t size;
    int      first_file;
    int      n_files;
} cld_block_t;

typedef struct {
    cld_block_t *blocks;
    int         *block_files;
    int          n_blocks;
    int         *fragments;
    int          n_fragments;
    DWORD       *pinned_list;
    int          n_pinned;
    uint32_t     current_max_free;
} cld_layout_t;

static int cld_layout_build(cld_ctx_t *ctx, cld_layout_t *out) {
    int n_files = ctx->n_files;

    int *contig = (int *)malloc((size_t)n_files * sizeof(int));
    int *frag   = (int *)malloc((size_t)n_files * sizeof(int));
    if (!contig || !frag) {
        free(contig); free(frag);
        return -1;
    }
    int n_contig = 0, n_frag = 0;
    for (int f = 0; f < n_files; f++) {
        uint32_t nc = ctx->files[f].n_clusters;
        if (nc == 0) continue;
        if (chain_is_contiguous(ctx->files[f].current_sclust, nc)) {
            contig[n_contig++] = f;
        } else {
            frag[n_frag++] = f;
        }
    }

    for (int i = 1; i < n_contig; i++) {
        int key = contig[i];
        DWORD key_s = ctx->files[key].current_sclust;
        int j = i - 1;
        while (j >= 0 && ctx->files[contig[j]].current_sclust > key_s) {
            contig[j + 1] = contig[j];
            j--;
        }
        contig[j + 1] = key;
    }

    cld_block_t *blocks = (cld_block_t *)malloc((size_t)(n_contig + 1) * sizeof(cld_block_t));
    if (!blocks) { free(contig); free(frag); return -1; }
    int n_blocks = 0;
    int i = 0;
    while (i < n_contig) {
        DWORD bs = ctx->files[contig[i]].current_sclust - 2;
        uint32_t bsize = ctx->files[contig[i]].n_clusters;
        int j = i + 1;
        while (j < n_contig) {
            DWORD next_s = ctx->files[contig[j]].current_sclust - 2;
            if (next_s == bs + bsize) {
                bsize += ctx->files[contig[j]].n_clusters;
                j++;
            } else break;
        }
        blocks[n_blocks].start      = bs;
        blocks[n_blocks].size       = bsize;
        blocks[n_blocks].first_file = i;
        blocks[n_blocks].n_files    = j - i;
        n_blocks++;
        i = j;
    }

    DWORD *pinned = (DWORD *)malloc(((size_t)ctx->n_clust + 1) * sizeof(DWORD));
    if (!pinned) { free(contig); free(frag); free(blocks); return -1; }
    int n_pinned = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        if (CLD_BIT_GET(ctx->pinned_bits, c)) pinned[n_pinned++] = c;
    }

    uint32_t cur_free = 0, max_free = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        int pin = CLD_BIT_GET(ctx->pinned_bits, c) ? 1 : 0;
        if (!pin && ctx->current_owner[c] == 0) {
            cur_free++;
            if (cur_free > max_free) max_free = cur_free;
        } else cur_free = 0;
    }

    out->blocks           = blocks;
    out->block_files      = contig;
    out->n_blocks         = n_blocks;
    out->fragments        = frag;
    out->n_fragments      = n_frag;
    out->pinned_list      = pinned;
    out->n_pinned         = n_pinned;
    out->current_max_free = max_free;
    return 0;
}

static void cld_layout_free(cld_layout_t *layout) {
    free(layout->blocks);       layout->blocks = NULL;
    free(layout->block_files);  layout->block_files = NULL;
    free(layout->fragments);    layout->fragments = NULL;
    free(layout->pinned_list);  layout->pinned_list = NULL;
}

static DWORD cld_find_clear_range(const cld_layout_t *layout, DWORD n_clust,
                                    DWORD cursor, uint32_t size) {
    while (cursor + size <= n_clust) {
        int p = 0;
        while (p < layout->n_pinned && layout->pinned_list[p] < cursor) p++;
        if (p >= layout->n_pinned || layout->pinned_list[p] >= cursor + size) {
            return cursor;
        }
        cursor = layout->pinned_list[p] + 1;
    }
    return n_clust;
}

typedef struct {
    DWORD    start;
    uint32_t length;
} cld_run_t;

static int cld_sim_subset(cld_ctx_t *ctx, const cld_layout_t *layout,
                           uint32_t mask,
                           DWORD *block_targets,
                           DWORD *fragment_targets,
                           int *out_moves,
                           uint32_t *out_max_free) {
    int moves = 0;
    DWORD cursor = 0;
    for (int i = 0; i < layout->n_blocks; i++) {
        const cld_block_t *b = &layout->blocks[i];
        bool shift = (mask >> i) & 1u;
        if (shift) {
            DWORD t = cld_find_clear_range(layout, ctx->n_clust, cursor, b->size);
            if (t >= ctx->n_clust) return -1;
            block_targets[i] = t;
            if (t != b->start) moves += (int)b->size;
            cursor = t + b->size;
        } else {
            if (b->start < cursor) return -1;
            block_targets[i] = b->start;
            cursor = b->start + b->size;
        }
    }

    int max_runs = layout->n_blocks + layout->n_pinned + 2;
    cld_run_t *runs = (cld_run_t *)malloc((size_t)max_runs * sizeof(cld_run_t));
    if (!runs) return -1;

    typedef struct { DWORD s, e; } cld_iv_t;
    cld_iv_t *iv = (cld_iv_t *)malloc((size_t)(layout->n_blocks + layout->n_pinned) * sizeof(cld_iv_t));
    if (!iv) { free(runs); return -1; }
    int n_iv = 0;
    for (int i = 0; i < layout->n_blocks; i++) {
        iv[n_iv].s = block_targets[i];
        iv[n_iv].e = block_targets[i] + layout->blocks[i].size;
        n_iv++;
    }
    for (int p = 0; p < layout->n_pinned; p++) {
        iv[n_iv].s = layout->pinned_list[p];
        iv[n_iv].e = layout->pinned_list[p] + 1;
        n_iv++;
    }
    for (int i = 1; i < n_iv; i++) {
        cld_iv_t key = iv[i];
        int j = i - 1;
        while (j >= 0 && iv[j].s > key.s) { iv[j+1] = iv[j]; j--; }
        iv[j+1] = key;
    }
    int n_runs = 0;
    DWORD pos = 0;
    for (int i = 0; i < n_iv; i++) {
        if (iv[i].s > pos && n_runs < max_runs) {
            runs[n_runs].start  = pos;
            runs[n_runs].length = iv[i].s - pos;
            n_runs++;
        }
        if (iv[i].e > pos) pos = iv[i].e;
    }
    if (pos < ctx->n_clust && n_runs < max_runs) {
        runs[n_runs].start  = pos;
        runs[n_runs].length = ctx->n_clust - pos;
        n_runs++;
    }
    free(iv);

    int *frag_sorted = (int *)malloc((size_t)(layout->n_fragments + 1) * sizeof(int));
    if (!frag_sorted) { free(runs); return -1; }
    for (int i = 0; i < layout->n_fragments; i++) frag_sorted[i] = layout->fragments[i];
    for (int i = 1; i < layout->n_fragments; i++) {
        int key = frag_sorted[i];
        uint32_t key_sz = ctx->files[key].n_clusters;
        int j = i - 1;
        while (j >= 0 && ctx->files[frag_sorted[j]].n_clusters < key_sz) {
            frag_sorted[j+1] = frag_sorted[j]; j--;
        }
        frag_sorted[j+1] = key;
    }
    for (int i = 0; i < layout->n_fragments; i++) {
        int f = frag_sorted[i];
        uint32_t sz = ctx->files[f].n_clusters;
        int best = -1;
        for (int r = 0; r < n_runs; r++) {
            if (runs[r].length >= sz) {
                if (best < 0 || runs[r].length < runs[best].length) best = r;
            }
        }
        if (best < 0) { free(frag_sorted); free(runs); return -1; }
        int orig_idx = -1;
        for (int k = 0; k < layout->n_fragments; k++) {
            if (layout->fragments[k] == f) { orig_idx = k; break; }
        }
        if (orig_idx >= 0) fragment_targets[orig_idx] = runs[best].start;
        runs[best].start  += sz;
        runs[best].length -= sz;
        moves += (int)sz;
    }
    free(frag_sorted);

    uint32_t max_free = 0;
    for (int r = 0; r < n_runs; r++) {
        if (runs[r].length > max_free) max_free = runs[r].length;
    }
    free(runs);

    *out_moves    = moves;
    *out_max_free = max_free;
    return 0;
}

static int cld_score_weighted(int moves, uint32_t plan_free, uint32_t current_max_free) {
    if (plan_free < current_max_free) {
        return moves + CLD_SCORE_REGRESS_P * (int)(current_max_free - plan_free);
    }
    int k = s_plan_k_values[s_plan_k_idx];
    return moves - k * (int)(plan_free - current_max_free);
}

static void cld_apply_mask(cld_ctx_t *ctx, const cld_layout_t *layout,
                            const DWORD *block_targets,
                            const DWORD *fragment_targets) {
    memset(ctx->target_owner, 0, ctx->n_clust * sizeof(uint32_t));
    for (int f = 0; f < ctx->n_files; f++) ctx->files[f].target_sclust = 0;

    for (int b = 0; b < layout->n_blocks; b++) {
        DWORD base = block_targets[b];
        DWORD off  = 0;
        for (int k = 0; k < layout->blocks[b].n_files; k++) {
            int f = layout->block_files[layout->blocks[b].first_file + k];
            ctx->files[f].target_sclust = base + off + 2;
            uint32_t sz = ctx->files[f].n_clusters;
            for (uint32_t j = 0; j < sz; j++) {
                ctx->target_owner[base + off + j] =
                    ((uint32_t)(f + 1) << 16) | (j & 0xFFFF);
            }
            off += sz;
        }
    }

    for (int i = 0; i < layout->n_fragments; i++) {
        int f = layout->fragments[i];
        DWORD base = fragment_targets[i];
        uint32_t sz = ctx->files[f].n_clusters;
        ctx->files[f].target_sclust = base + 2;
        for (uint32_t j = 0; j < sz; j++) {
            ctx->target_owner[base + j] =
                ((uint32_t)(f + 1) << 16) | (j & 0xFFFF);
        }
    }
}

#define CLD_DIAG_MAX_DIRS 256
static void cld_scan_pin_owners(cld_ctx_t *ctx) {
    if (s_pin_names) { free(s_pin_names); s_pin_names = NULL; }
    s_n_pin_names = 0;
    s_pin_display_idx = 0;

    int n_pins = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        if (CLD_BIT_GET(ctx->pinned_bits, c)) n_pins++;
    }
    if (n_pins == 0) return;

    int cap = (n_pins < CLD_MAX_PIN_NAMES) ? n_pins : CLD_MAX_PIN_NAMES;
    s_pin_names = (cld_pin_name_t *)calloc((size_t)cap, sizeof(cld_pin_name_t));
    if (!s_pin_names) return;
    s_n_pin_names = cap;

    int p = 0;
    for (DWORD c = 0; c < ctx->n_clust && p < cap; c++) {
        if (CLD_BIT_GET(ctx->pinned_bits, c)) {
            s_pin_names[p].cluster = c;
            s_pin_names[p].name[0] = 0;
            p++;
        }
    }

    typedef struct { char path[CLD_PATH_MAX]; } scan_q_t;
    scan_q_t *q = (scan_q_t *)malloc((size_t)CLD_DIAG_MAX_DIRS * sizeof(scan_q_t));
    if (!q) return;
    int q_head = 0, q_tail = 0;
    strncpy(q[0].path, "/", CLD_PATH_MAX - 1);
    q[0].path[CLD_PATH_MAX - 1] = 0;
    q_tail = 1;

    while (q_head < q_tail) {
        char cur[CLD_PATH_MAX];
        strncpy(cur, q[q_head++].path, CLD_PATH_MAX - 1);
        cur[CLD_PATH_MAX - 1] = 0;

        DIR d;
        FILINFO info;
        if (f_opendir(&d, cur) != FR_OK) continue;
        while (f_readdir(&d, &info) == FR_OK) {
            if (info.fname[0] == 0) break;
            if (info.fname[0] == '.'
                && (info.fname[1] == 0
                    || (info.fname[1] == '.' && info.fname[2] == 0))) {
                continue;
            }
            char full[CLD_PATH_MAX];
            if (cur[0] == '/' && cur[1] == 0)
                snprintf(full, sizeof(full), "/%s", info.fname);
            else
                snprintf(full, sizeof(full), "%s/%s", cur, info.fname);

            DWORD sclust = 0;
            if (info.fattrib & AM_DIR) {
                DIR subd;
                if (f_opendir(&subd, full) != FR_OK) continue;
                sclust = subd.obj.sclust;
                f_closedir(&subd);
                if (q_tail < CLD_DIAG_MAX_DIRS) {
                    strncpy(q[q_tail].path, full, CLD_PATH_MAX - 1);
                    q[q_tail].path[CLD_PATH_MAX - 1] = 0;
                    q_tail++;
                }
            } else {
                FIL probe;
                if (f_open(&probe, full, FA_READ) != FR_OK) continue;
                sclust = probe.obj.sclust;
                f_close(&probe);
            }
            if (sclust < 2 || sclust >= g_fs.n_fatent) continue;

            DWORD clst = sclust;
            int safety = (int)ctx->n_clust + 1;
            while (clst >= 2 && clst < g_fs.n_fatent && safety-- > 0) {
                DWORD ci = clst - 2;
                if (ci < ctx->n_clust
                    && CLD_BIT_GET(ctx->pinned_bits, ci)) {
                    for (int i = 0; i < s_n_pin_names; i++) {
                        if (s_pin_names[i].cluster == ci
                            && s_pin_names[i].name[0] == 0) {
                            strncpy(s_pin_names[i].name, full,
                                    CLD_PATH_MAX - 1);
                            s_pin_names[i].name[CLD_PATH_MAX - 1] = 0;
                            break;
                        }
                    }
                }
                DWORD next = fat_get(clst);
                if (next == 0xFFFFFFFFu || fat_eoc(next) || next < 2) break;
                clst = next;
            }
        }
        f_closedir(&d);
    }
    free(q);

    /* Reclaim truly unowned pins. */
    s_n_reclaimed  = 0;
    s_reclaimed_kb = 0;
    for (int i = 0; i < s_n_pin_names; i++) {
        if (s_pin_names[i].name[0] != 0) continue;
        DWORD ci = s_pin_names[i].cluster;
        if (ci >= ctx->n_clust) continue;
        if (!CLD_BIT_GET(ctx->pinned_bits, ci)) continue;
        CLD_BIT_CLR(ctx->pinned_bits, ci);
        s_n_reclaimed++;
    }
    s_reclaimed_kb = (uint32_t)(((uint64_t)s_n_reclaimed * ctx->bpc) / 1024);
}

static void cld_mark_unplaceable_stay(cld_ctx_t *ctx) {
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        if (ctx->target_owner[c] != 0) continue;
        uint32_t owner = ctx->current_owner[c];
        if (owner == 0) continue;
        int fidx = (int)((owner >> 16) - 1);
        if (fidx < 0 || fidx >= ctx->n_files) continue;
        if (ctx->files[fidx].target_sclust != 0) continue;
        ctx->target_owner[c] = owner;
    }
}

static int cld_plan_pack_left(cld_ctx_t *ctx) {
    int n_files = ctx->n_files;

    int n_pinned = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        if (CLD_BIT_GET(ctx->pinned_bits, c)) n_pinned++;
    }
    DWORD *pinned = NULL;
    if (n_pinned > 0) {
        pinned = (DWORD *)malloc((size_t)n_pinned * sizeof(DWORD));
        if (!pinned) return -1;
        int p = 0;
        for (DWORD c = 0; c < ctx->n_clust; c++) {
            if (CLD_BIT_GET(ctx->pinned_bits, c)) pinned[p++] = c;
        }
    }
    cld_layout_t layout = {0};
    layout.pinned_list = pinned;
    layout.n_pinned    = n_pinned;

    int *sorted = (int *)malloc((size_t)(n_files + 1) * sizeof(int));
    if (!sorted) { free(pinned); return -1; }
    for (int i = 0; i < n_files; i++) sorted[i] = i;
    for (int i = 1; i < n_files; i++) {
        int      key    = sorted[i];
        uint32_t key_sz = ctx->files[key].n_clusters;
        DWORD    key_sc = ctx->files[key].current_sclust;
        int j = i - 1;
        while (j >= 0) {
            uint32_t sz = ctx->files[sorted[j]].n_clusters;
            DWORD    sc = ctx->files[sorted[j]].current_sclust;
            bool shift = (sz < key_sz) || (sz == key_sz && sc > key_sc);
            if (!shift) break;
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    memset(ctx->target_owner, 0, ctx->n_clust * sizeof(uint32_t));
    for (int f = 0; f < n_files; f++) ctx->files[f].target_sclust = 0;

    DWORD cursor = 0;
    for (int s = 0; s < n_files; s++) {
        int f = sorted[s];
        uint32_t nc = ctx->files[f].n_clusters;
        if (nc == 0) continue;
        DWORD t = cld_find_clear_range(&layout, ctx->n_clust, cursor, nc);
        if (t >= ctx->n_clust) continue;
        ctx->files[f].target_sclust = t + 2;
        for (uint32_t j = 0; j < nc; j++) {
            ctx->target_owner[t + j] =
                ((uint32_t)(f + 1) << 16) | (j & 0xFFFF);
        }
        cursor = t + nc;
    }

    free(sorted);
    free(pinned);
    cld_mark_unplaceable_stay(ctx);
    return 0;
}

static int cld_plan_block_search(cld_ctx_t *ctx) {
    cld_layout_t layout = {0};
    if (cld_layout_build(ctx, &layout) != 0) return -1;

    int n_blocks = layout.n_blocks;

    DWORD *bt_cur  = (DWORD *)malloc((size_t)(n_blocks + 1) * sizeof(DWORD));
    DWORD *ft_cur  = (DWORD *)malloc((size_t)(layout.n_fragments + 1) * sizeof(DWORD));
    DWORD *bt_best = (DWORD *)malloc((size_t)(n_blocks + 1) * sizeof(DWORD));
    DWORD *ft_best = (DWORD *)malloc((size_t)(layout.n_fragments + 1) * sizeof(DWORD));
    if ((n_blocks > 0 && (!bt_cur || !bt_best)) ||
        (layout.n_fragments > 0 && (!ft_cur || !ft_best))) {
        free(bt_cur); free(ft_cur); free(bt_best); free(ft_best);
        cld_layout_free(&layout);
        return -1;
    }

    int       best_score = INT_MAX;
    int       best_moves = INT_MAX;
    uint32_t  best_free  = 0;
    bool      have_best  = false;
    uint32_t  best_mask  = 0;

    if (n_blocks <= CLD_BLOCK_EXHAUSTIVE) {
        uint32_t limit = (n_blocks == 0) ? 1u : (1u << n_blocks);
        for (uint32_t mask = 0; mask < limit; mask++) {
            int moves;
            uint32_t max_free;
            if (cld_sim_subset(ctx, &layout, mask, bt_cur, ft_cur,
                                &moves, &max_free) != 0) continue;
            int score = cld_score_weighted(moves, max_free, layout.current_max_free);
            if (score < best_score) {
                best_score = score; best_moves = moves; best_free = max_free;
                best_mask = mask; have_best = true;
                for (int i = 0; i < n_blocks; i++) bt_best[i] = bt_cur[i];
                for (int i = 0; i < layout.n_fragments; i++) ft_best[i] = ft_cur[i];
            }
        }
    } else {
        uint32_t all_mask = (n_blocks >= 32) ? ~0u : ((1u << n_blocks) - 1);
        uint32_t seeds[4];
        int n_seeds = 0;
        seeds[n_seeds++] = 0;
        seeds[n_seeds++] = all_mask;
        if (n_blocks >= 2) {
            int half = n_blocks / 2;
            uint32_t first_half = ((1u << half) - 1);
            seeds[n_seeds++] = first_half;
            seeds[n_seeds++] = all_mask & ~first_half;
        }

        for (int s = 0; s < n_seeds; s++) {
            uint32_t mask = seeds[s];
            int      cur_moves = 0;
            uint32_t cur_free  = 0;
            int      cur_score = INT_MAX;
            bool     cur_valid = false;

            if (cld_sim_subset(ctx, &layout, mask, bt_cur, ft_cur,
                                &cur_moves, &cur_free) == 0) {
                cur_valid = true;
                cur_score = cld_score_weighted(cur_moves, cur_free,
                                                 layout.current_max_free);
            }

            for (int round = 0; round < 2 * n_blocks; round++) {
                int      best_flip = -1;
                int      best_flip_score = cur_valid ? cur_score : INT_MAX;
                int      flip_moves = 0;
                uint32_t flip_free  = 0;

                for (int b = 0; b < n_blocks; b++) {
                    uint32_t trial = mask ^ (1u << b);
                    int      tm;
                    uint32_t tf;
                    if (cld_sim_subset(ctx, &layout, trial, bt_cur, ft_cur,
                                        &tm, &tf) != 0) continue;
                    int ts = cld_score_weighted(tm, tf,
                                                  layout.current_max_free);
                    if (ts < best_flip_score) {
                        best_flip = b;
                        best_flip_score = ts;
                        flip_moves = tm;
                        flip_free = tf;
                    }
                }
                if (best_flip < 0) break;
                mask ^= (1u << best_flip);
                cur_score = best_flip_score;
                cur_moves = flip_moves;
                cur_free  = flip_free;
                cur_valid = true;
            }

            if (cur_valid && cur_score < best_score) {
                best_score = cur_score;
                best_moves = cur_moves;
                best_free  = cur_free;
                best_mask  = mask;
                if (cld_sim_subset(ctx, &layout, mask, bt_best, ft_best,
                                    &best_moves, &best_free) == 0) {
                    have_best = true;
                }
            }
        }
    }

    if (have_best) {
        cld_apply_mask(ctx, &layout, bt_best, ft_best);
        cld_mark_unplaceable_stay(ctx);
    } else {
        cld_plan_pack_left(ctx);
    }

    (void)best_moves; (void)best_free; (void)best_mask; (void)best_score;
    free(bt_cur); free(ft_cur); free(bt_best); free(ft_best);
    cld_layout_free(&layout);
    return 0;
}

static int cld_plan_dispatch(cld_ctx_t *ctx) {
    if (CLD_PLAN_K_IS_PACK_LEFT(s_plan_k_values[s_plan_k_idx])) {
        return cld_plan_pack_left(ctx);
    }
    return cld_plan_block_search(ctx);
}

/* ----------------- ANALYZE ------------------------------------- */

static int cld_analyze(cld_ctx_t *ctx) {
    ctx->n_clust = (g_fs.n_fatent >= 2) ? (g_fs.n_fatent - 2) : 0;
    if (ctx->n_clust == 0) return -1;
    if (ctx->n_clust > CLD_ABS_MAX_CLUSTERS) return -4;
    ctx->bpc = (DWORD)g_fs.csize * 512u;

    ctx->current_owner =
        (uint32_t *)malloc((size_t)ctx->n_clust * sizeof(uint32_t));
    ctx->target_owner =
        (uint32_t *)malloc((size_t)ctx->n_clust * sizeof(uint32_t));
    ctx->pinned_bits = (uint8_t *)calloc((ctx->n_clust + 7) / 8, 1);
    ctx->done_bits   = (uint8_t *)calloc((ctx->n_clust + 7) / 8, 1);
    if (!ctx->current_owner || !ctx->target_owner
        || !ctx->pinned_bits || !ctx->done_bits) {
        return -5;
    }

    typedef struct { char path[CLD_PATH_MAX]; int parent_idx; } queue_ent_t;
    queue_ent_t *dir_q = (queue_ent_t *)malloc((size_t)CLD_MAX_DIRS * sizeof(queue_ent_t));
    if (!dir_q) return -6;

    int q_head = 0, q_tail = 0;
    strncpy(dir_q[0].path, "/", CLD_PATH_MAX - 1);
    dir_q[0].path[CLD_PATH_MAX - 1] = 0;
    dir_q[0].parent_idx = -1;
    q_tail = 1;

    int n_files = 0;
    while (q_head < q_tail && n_files < CLD_MAX_FILES) {
        char        cur_dir_path[CLD_PATH_MAX];
        int         cur_parent_idx;
        strncpy(cur_dir_path, dir_q[q_head].path, CLD_PATH_MAX - 1);
        cur_dir_path[CLD_PATH_MAX - 1] = 0;
        cur_parent_idx = dir_q[q_head].parent_idx;
        q_head++;

        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, cur_dir_path) != FR_OK) continue;
        while (n_files < CLD_MAX_FILES && f_readdir(&dir, &info) == FR_OK) {
            if (info.fname[0] == 0) break;
            if (info.fname[0] == '.'
                && (info.fname[1] == 0
                    || (info.fname[1] == '.' && info.fname[2] == 0))) {
                continue;
            }

            /* FatFs's f_readdir advances dptr past the SFN we just
             * returned by SZDIRE (32). Subtract to recover the SFN's
             * own offset. */
            DWORD e_byte_in_parent = (dir.dptr >= 32) ? (dir.dptr - 32) : 0;

            char full[CLD_PATH_MAX];
            if (cur_dir_path[0] == '/' && cur_dir_path[1] == 0)
                snprintf(full, sizeof(full), "/%s", info.fname);
            else
                snprintf(full, sizeof(full), "%s/%s", cur_dir_path, info.fname);

            DWORD entry_sclust = 0;
            uint8_t is_dir     = (info.fattrib & AM_DIR) ? 1 : 0;
            uint32_t nc        = 0;

            if (is_dir) {
                DIR subd;
                if (f_opendir(&subd, full) != FR_OK) continue;
                entry_sclust = subd.obj.sclust;
                f_closedir(&subd);
                nc = 0;
                {
                    DWORD clst = entry_sclust;
                    int safety = 64;
                    while (clst >= 2 && clst < g_fs.n_fatent && safety-- > 0) {
                        nc++;
                        DWORD next = fat_get(clst);
                        if (next == 0xFFFFFFFFu || fat_eoc(next) || next < 2) break;
                        clst = next;
                    }
                }
            } else {
                FIL probe;
                if (f_open(&probe, full, FA_READ) != FR_OK) continue;
                entry_sclust = probe.obj.sclust;
                FSIZE_t sz   = f_size(&probe);
                f_close(&probe);
                if (sz == 0) continue;
                nc = (uint32_t)(((DWORD)sz + ctx->bpc - 1) / ctx->bpc);
            }

            int my_idx = n_files;
            ctx->files[my_idx].current_sclust = entry_sclust;
            ctx->files[my_idx].n_clusters     = nc;
            ctx->files[my_idx].parent_idx     = cur_parent_idx;
            ctx->files[my_idx].byte_in_parent = e_byte_in_parent;
            ctx->files[my_idx].is_dir         = is_dir;
            ctx->files[my_idx].target_sclust  = 0;
            n_files++;

            if (is_dir && q_tail < CLD_MAX_DIRS) {
                strncpy(dir_q[q_tail].path, full, CLD_PATH_MAX - 1);
                dir_q[q_tail].path[CLD_PATH_MAX - 1] = 0;
                dir_q[q_tail].parent_idx = my_idx;
                q_tail++;
            }
        }
        f_closedir(&dir);
    }
    free(dir_q);
    ctx->n_files = n_files;
    if (n_files == 0) return 0;

    memset(ctx->current_owner, 0, ctx->n_clust * sizeof(uint32_t));
    for (int f = 0; f < n_files; f++) {
        cld_owner_arg_t oa = { .ctx = ctx, .fidx = f, .off = 0 };
        cld_walk_fat(ctx->files[f].current_sclust, cld_cb_set_owner, &oa);
    }

    /* Orphan-cluster scan: a cluster the FAT marks in-use but no directory
     * entry claims. Skip free (0), reserved (1) and bad-cluster markers. */
    {
        for (DWORD ci = 0; ci < ctx->n_clust; ci++) {
            if (ctx->current_owner[ci] != 0) continue;
            if (CLD_BIT_GET(ctx->pinned_bits, ci)) continue;
            DWORD v = fat_get(ci + 2);
            if (v == 0xFFFFFFFFu) break;            /* read error */
            if (v == 0 || v == 1 || fat_bad(v)) continue;
            CLD_BIT_SET(ctx->pinned_bits, ci);
        }
    }

    cld_scan_pin_owners(ctx);
    return cld_plan_dispatch(ctx);
}

/* ----------------- PREVIEW DRAW --------------------------------- */

static void cld_draw_execute_overlay(uint16_t *fb, const cld_ctx_t *ctx,
                                      const char *title,
                                      int moves, int moves_planned) {
    enum {
        MAP_X = 4, MAP_Y = 22,
        CELL_W = 2, CELL_H = 2,
        MAP_COLS = 60, MAP_ROWS = 40,
        N_CELLS = MAP_COLS * MAP_ROWS,
    };
    static const uint16_t palette[32] = {
        0x07FF, 0xFFE0, 0x07E0, 0xF81F, 0xFD20,
        0xAFE5, 0x87FF, 0xFB56, 0x5E7F, 0xFCE0,
        0xA8F4, 0x6E7D, 0x34BF, 0xBE5B, 0xEF7E,
        0x001F, 0x801F, 0xC81F, 0x7FE0, 0x03BF,
        0x045F, 0x0280, 0x63FF, 0x9FE0, 0x9A3F,
        0xCC1F, 0x07EB, 0x601F, 0xFFA0, 0x45FF,
        0xFD60, 0x4408,
    };
    const uint16_t FREE_COLOR = 0x10A2;

    fb_clear(fb, COL_BG);
    fb_rect(fb, 0, 0, FB_W, 12, COL_ERR);
    const char *warn = "DO NOT POWER OFF";
    nes_font_draw(fb, warn, (FB_W - nes_font_width(warn)) / 2, 3, COL_FG);
    if (title) {
        nes_font_draw(fb, title,
                       (FB_W - nes_font_width(title)) / 2, 13, COL_TITLE);
    }

    fb_rect(fb, MAP_X - 1, MAP_Y - 1,
            MAP_COLS * CELL_W + 2, MAP_ROWS * CELL_H + 2, 0x4208);

    DWORD n_clust = ctx->n_clust;
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int co = 0; co < MAP_COLS; co++) {
            uint32_t cell_idx = (uint32_t)(r * MAP_COLS + co);
            uint32_t ci_start =
                (uint32_t)(((uint64_t)cell_idx * n_clust) / N_CELLS);
            uint32_t ci_end =
                (uint32_t)(((uint64_t)(cell_idx + 1) * n_clust) / N_CELLS);
            if (ci_end <= ci_start) ci_end = ci_start + 1;
            if (ci_end > n_clust)   ci_end = n_clust;

            int fidx = -1;
            for (uint32_t ci = ci_start; ci < ci_end; ci++) {
                if (ctx->current_owner[ci] != 0) {
                    fidx = (int)((ctx->current_owner[ci] >> 16) - 1);
                    break;
                }
            }
            uint16_t color = (fidx >= 0) ? palette[fidx % 32] : FREE_COLOR;
            fb_rect(fb, MAP_X + co * CELL_W, MAP_Y + r * CELL_H,
                    CELL_W, CELL_H, color);
        }
    }

    char cnt[24];
    int denom = moves_planned > 0 ? moves_planned : 1;
    int num   = moves > denom ? denom : moves;
    snprintf(cnt, sizeof(cnt), "%d / %d", num, denom);
    nes_font_draw(fb, cnt, (FB_W - nes_font_width(cnt)) / 2, 116, COL_DIM);

    int bar_y = 124, bar_h = 2;
    fb_rect(fb, 4, bar_y, FB_W - 8, bar_h, 0x2104);
    int fill = denom > 0 ? ((FB_W - 8) * num) / denom : 0;
    if (fill > FB_W - 8) fill = FB_W - 8;
    fb_rect(fb, 4, bar_y, fill, bar_h, COL_HIGHLT);

    nes_lcd_wait_idle();
    nes_lcd_present(fb);
    tud_task();
}

static void cld_draw_map_small(uint16_t *fb, const uint32_t *owner_map,
                                const uint8_t *pinned_bits,
                                DWORD n_clust, int y_top) {
    enum { COLS = 60, ROWS = 17, CELL = 2, X0 = 4, N_CELLS = COLS * ROWS };
    static const uint16_t palette[32] = {
        0x07FF, 0xFFE0, 0x07E0, 0xF81F, 0xFD20,
        0xAFE5, 0x87FF, 0xFB56, 0x5E7F, 0xFCE0,
        0xA8F4, 0x6E7D, 0x34BF, 0xBE5B, 0xEF7E,
        0x001F, 0x801F, 0xC81F, 0x7FE0, 0x03BF,
        0x045F, 0x0280, 0x63FF, 0x9FE0, 0x9A3F,
        0xCC1F, 0x07EB, 0x601F, 0xFFA0, 0x45FF,
        0xFD60, 0x4408,
    };
    fb_rect(fb, X0 - 1, y_top - 1, COLS * CELL + 2, ROWS * CELL + 2, 0x4208);
    for (int r = 0; r < ROWS; r++) {
        for (int co = 0; co < COLS; co++) {
            uint32_t cell_idx = (uint32_t)(r * COLS + co);
            uint32_t ci_start =
                (uint32_t)(((uint64_t)cell_idx * n_clust) / N_CELLS);
            uint32_t ci_end =
                (uint32_t)(((uint64_t)(cell_idx + 1) * n_clust) / N_CELLS);
            if (ci_end <= ci_start) ci_end = ci_start + 1;
            if (ci_end > n_clust)   ci_end = n_clust;

            int  fidx    = -1;
            bool pinned  = false;
            for (uint32_t ci = ci_start; ci < ci_end; ci++) {
                if (CLD_BIT_GET(pinned_bits, ci)) pinned = true;
                if (owner_map[ci] != 0) {
                    fidx = (int)((owner_map[ci] >> 16) - 1);
                    break;
                }
            }
            uint16_t color;
            if (fidx >= 0)      color = palette[fidx % 32];
            else if (pinned)    color = COL_ERR;
            else                color = 0x10A2;
            fb_rect(fb, X0 + co * CELL, y_top + r * CELL, CELL, CELL, color);
        }
    }
}

static void cld_show_preview(const cld_ctx_t *ctx, uint16_t *fb,
                              int moves_planned, int unplaceable) {
    fb_clear(fb, COL_BG);

    const char *title = "DEFRAG PREVIEW";
    nes_font_draw(fb, title,
                   (FB_W - nes_font_width(title)) / 2, 2, COL_TITLE);

    nes_font_draw(fb, "before", 4, 12, COL_DIM);
    cld_draw_map_small(fb, ctx->current_owner, ctx->pinned_bits,
                        ctx->n_clust, 20);

    nes_font_draw(fb, "after", 4, 56, COL_DIM);
    cld_draw_map_small(fb, ctx->target_owner, ctx->pinned_bits,
                        ctx->n_clust, 64);

    int frag_before = 0;
    for (int f = 0; f < ctx->n_files; f++) {
        uint32_t nc = ctx->files[f].n_clusters;
        if (nc <= 1) continue;
        if (!chain_is_contiguous(ctx->files[f].current_sclust, nc)) {
            frag_before++;
        }
    }
    int frag_after = unplaceable;

    uint32_t cur_before = 0, max_free_before = 0;
    uint32_t cur_after  = 0, max_free_after  = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        int pinned = CLD_BIT_GET(ctx->pinned_bits, c) ? 1 : 0;
        if (ctx->current_owner[c] == 0 && !pinned) {
            cur_before++;
            if (cur_before > max_free_before) max_free_before = cur_before;
        } else cur_before = 0;
        if (ctx->target_owner[c] == 0 && !pinned) {
            cur_after++;
            if (cur_after > max_free_after) max_free_after = cur_after;
        } else cur_after = 0;
    }
    uint32_t free_b_kb = (uint32_t)((max_free_before * ctx->bpc) / 1024);
    uint32_t free_a_kb = (uint32_t)((max_free_after  * ctx->bpc) / 1024);

    #define LD_FMT_KB(buf, kb) do { \
        if ((kb) >= 1024) snprintf((buf), sizeof(buf), "%lu.%luM", \
                                    (unsigned long)((kb)/1024), \
                                    (unsigned long)(((kb)%1024)/103)); \
        else              snprintf((buf), sizeof(buf), "%luK", \
                                    (unsigned long)(kb)); \
    } while (0)
    char free_b[16], free_a[16];
    LD_FMT_KB(free_b, free_b_kb);
    LD_FMT_KB(free_a, free_a_kb);
    #undef LD_FMT_KB

    int pin_c = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        if (CLD_BIT_GET(ctx->pinned_bits, c)) pin_c++;
    }
    uint32_t pin_kb = (uint32_t)(((uint64_t)pin_c * ctx->bpc) / 1024);

    char line1[40];
    snprintf(line1, sizeof(line1), "frag: %d -> %d", frag_before, frag_after);
    nes_font_draw(fb, line1,
                   (FB_W - nes_font_width(line1)) / 2, 100,
                   (frag_after == 0) ? COL_FG : COL_ERR);

    char line2[40];
    snprintf(line2, sizeof(line2), "free: %s -> %s", free_b, free_a);
    nes_font_draw(fb, line2,
                   (FB_W - nes_font_width(line2)) / 2, 107, COL_HIGHLT);

    char line3[40];
    uint16_t line3_col = COL_DIM;
    if (pin_c > 0 && s_n_reclaimed > 0) {
        snprintf(line3, sizeof(line3), "cost:%d pin:%d rclm:%d",
                 moves_planned, pin_c, s_n_reclaimed);
        line3_col = COL_ERR;
    } else if (pin_c > 0) {
        snprintf(line3, sizeof(line3), "cost:%d pin:%d(%luk)",
                 moves_planned, pin_c, (unsigned long)pin_kb);
        line3_col = COL_ERR;
    } else if (s_n_reclaimed > 0) {
        snprintf(line3, sizeof(line3), "cost:%d rclm:%d(%luk)",
                 moves_planned, s_n_reclaimed,
                 (unsigned long)s_reclaimed_kb);
        line3_col = COL_HIGHLT;
    } else {
        snprintf(line3, sizeof(line3), "cost: %d writes", moves_planned);
    }
    nes_font_draw(fb, line3,
                   (FB_W - nes_font_width(line3)) / 2, 114, line3_col);

    char line4[40];
    if (CLD_PLAN_K_IS_PACK_LEFT(s_plan_k_values[s_plan_k_idx])) {
        snprintf(line4, sizeof(line4), "<PACK> A=go B=x");
    } else {
        snprintf(line4, sizeof(line4), "<K=%d> A=go B=x",
                 s_plan_k_values[s_plan_k_idx]);
    }
    nes_font_draw(fb, line4,
                   (FB_W - nes_font_width(line4)) / 2, 121, COL_HIGHLT);

    nes_lcd_wait_idle();
    nes_lcd_present(fb);
    /* Pump USB so MSC doesn't time out while user reads the preview. */
    tud_task();
    (void)s_pin_display_idx;
}

enum {
    CLD_UI_APPLY  = 1,
    CLD_UI_CANCEL = 0,
    CLD_UI_K_DOWN = 2,
    CLD_UI_K_UP   = 3,
};

static int cld_wait_interaction(void) {
    /* Drain currently-held buttons first. */
    while (lobby_defrag_buttons_read() != 0 || lobby_defrag_menu_pressed()) {
        tud_task();
        sleep_ms(10);
    }
    while (1) {
        tud_task();
        sleep_ms(10);
        uint8_t b = lobby_defrag_buttons_read();
        if (b & 0x20) return CLD_UI_APPLY;    /* A */
        if (b & 0x10) return CLD_UI_CANCEL;   /* B */
        if (lobby_defrag_menu_pressed()) return CLD_UI_CANCEL;
        if (b & 0x01) return CLD_UI_K_DOWN;   /* LEFT */
        if (b & 0x02) return CLD_UI_K_UP;     /* RIGHT */
    }
}

/* ----------------- EXECUTE -------------------------------------- */

static int cld_execute(cld_ctx_t *ctx, uint16_t *fb, int moves_planned) {
    DWORD n_clust = ctx->n_clust;
    memset(ctx->done_bits, 0, (n_clust + 7) / 8);

    int moves = 0;
    int progress_tick = 0;

    /* Phase 3a: cycle sort. */
    for (DWORD c = 0; c < n_clust; c++) {
        if (CLD_BIT_GET(ctx->done_bits, c))   continue;
        if (CLD_BIT_GET(ctx->pinned_bits, c)) continue;
        if (ctx->current_owner[c] == ctx->target_owner[c]) {
            CLD_BIT_SET(ctx->done_bits, c);
            continue;
        }

        DWORD    pos = c;
        uint32_t carried_id = ctx->current_owner[c];

        LBA_t start_lba = (LBA_t)g_fs.database + (LBA_t)c * g_fs.csize;
        if (nes_flash_disk_read(ctx->buf_a, (uint32_t)start_lba, g_fs.csize) != 0)
            return -10;

        int cycle_safety = (int)n_clust + 4;
        while (cycle_safety-- > 0) {
            DWORD dest;
            if (carried_id == 0) {
                ctx->current_owner[c] = 0;
                CLD_BIT_SET(ctx->done_bits, c);
                break;
            }
            int fidx = (int)((carried_id >> 16) - 1);
            int off  = (int)(carried_id & 0xFFFF);
            if (fidx < 0 || fidx >= ctx->n_files
                || ctx->files[fidx].target_sclust == 0) {
                LBA_t cur_lba =
                    (LBA_t)g_fs.database + (LBA_t)pos * g_fs.csize;
                nes_flash_disk_write(ctx->buf_a, (uint32_t)cur_lba, g_fs.csize);
                CLD_BIT_SET(ctx->done_bits, pos);
                break;
            }
            dest = (ctx->files[fidx].target_sclust - 2) + (DWORD)off;

            if (dest == c) {
                if (nes_flash_disk_write(ctx->buf_a, (uint32_t)start_lba, g_fs.csize) != 0)
                    return -11;
                ctx->current_owner[c] = carried_id;
                CLD_BIT_SET(ctx->done_bits, c);
                moves++;
                break;
            }

            LBA_t dest_lba =
                (LBA_t)g_fs.database + (LBA_t)dest * g_fs.csize;
            if (nes_flash_disk_read(ctx->buf_b, (uint32_t)dest_lba, g_fs.csize) != 0)
                return -12;
            uint32_t dest_id = ctx->current_owner[dest];
            if (nes_flash_disk_write(ctx->buf_a, (uint32_t)dest_lba, g_fs.csize) != 0)
                return -13;
            ctx->current_owner[dest] = carried_id;
            CLD_BIT_SET(ctx->done_bits, dest);
            moves++;

            carried_id = dest_id;
            { uint8_t *tmp = ctx->buf_a; ctx->buf_a = ctx->buf_b; ctx->buf_b = tmp; }
            pos = dest;

            if ((++progress_tick & 0xF) == 0) {
                nes_flash_disk_flush();
                cld_draw_execute_overlay(fb, ctx, "moving clusters",
                                          moves, moves_planned);
            }
        }
    }
    nes_flash_disk_flush();

    /* Phase 3b: rebuild FAT. */
    cld_draw_execute_overlay(fb, ctx, "rewriting FAT",
                              moves_planned, moves_planned);

    uint16_t *new_fat = (uint16_t *)malloc(n_clust * sizeof(uint16_t));
    if (!new_fat) return -20;
    memset(new_fat, 0, n_clust * sizeof(uint16_t));

    /* Seed pinned (orphan) clusters' entries verbatim from the live FAT. */
    for (DWORD ci = 0; ci < n_clust; ci++) {
        if (!CLD_BIT_GET(ctx->pinned_bits, ci)) continue;
        DWORD v = fat_get(ci + 2);
        if (v == 0xFFFFFFFFu) { free(new_fat); return -21; }
        new_fat[ci] = (uint16_t)v;
    }

    for (int f = 0; f < ctx->n_files; f++) {
        if (ctx->files[f].target_sclust == 0) {
            /* Unmoved file: copy its existing chain into new_fat verbatim. */
            DWORD clst = ctx->files[f].current_sclust;
            int safety = (int)n_clust + 1;
            while (clst >= 2 && clst < g_fs.n_fatent && safety-- > 0) {
                DWORD ci = clst - 2;
                DWORD next = fat_get(clst);
                if (next == 0xFFFFFFFFu) break;            /* read error: leave as-is */
                if (ci < n_clust) new_fat[ci] = (uint16_t)next;
                if (fat_eoc(next) || next < 2) break;
                clst = next;
            }
            continue;
        }
        DWORD    tstart = ctx->files[f].target_sclust;
        uint32_t nc     = ctx->files[f].n_clusters;
        for (uint32_t i = 0; i < nc; i++) {
            DWORD ci = tstart + i - 2;
            if (ci >= n_clust) break;
            new_fat[ci] = (i == nc - 1)
                            ? 0xFFFF
                            : (uint16_t)(tstart + i + 1);
        }
    }

    /* Write every FAT copy from new_fat[]. Build the whole FAT image in a buffer
     * first so FAT12's 12-bit, sector-spanning entries pack correctly — a
     * per-sector read-patch-write can't, since an entry straddles the 512-byte
     * boundary. Reserved entries 0,1 are preserved from the live FAT; clusters
     * not in any chain default to 0 (free). FAT16 packs 2 bytes/entry as before. */
    {
        int   is12   = fat_is12();
        DWORD fbytes = (DWORD)g_fs.fsize * 512u;
        uint8_t *img = (uint8_t *)malloc(fbytes);
        if (!img) { free(new_fat); return -22; }
        if (nes_flash_disk_read(img, (uint32_t)g_fs.fatbase, 1) != 0) {   /* entries 0,1 */
            free(img); free(new_fat); return -22;
        }
        DWORD resv = is12 ? 3u : 4u;                       /* bytes occupied by entries 0,1 */
        memset(img + resv, 0, (size_t)fbytes - resv);      /* every data-cluster entry starts at 0 */
        for (DWORD c = 2; c < g_fs.n_fatent; c++) {
            DWORD ci = c - 2;
            DWORD v  = (ci < n_clust) ? new_fat[ci] : 0u;  /* 0xFFFF truncates to FAT12's 0xFFF EOC */
            if (is12) {
                DWORD bo = c + (c >> 1);
                if (c & 1) { img[bo]     = (uint8_t)((img[bo] & 0x0F) | ((v << 4) & 0xF0));
                             img[bo + 1] = (uint8_t)((v >> 4) & 0xFF); }
                else       { img[bo]     = (uint8_t)(v & 0xFF);
                             img[bo + 1] = (uint8_t)((img[bo + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
            } else {
                DWORD bo = c * 2u;
                img[bo]     = (uint8_t)(v & 0xFF);
                img[bo + 1] = (uint8_t)((v >> 8) & 0xFF);
            }
        }
        for (int fatno = 0; fatno < g_fs.n_fats; fatno++) {
            LBA_t fat_start = (LBA_t)g_fs.fatbase + (LBA_t)fatno * g_fs.fsize;
            for (DWORD sec = 0; sec < g_fs.fsize; sec++) {
                if (nes_flash_disk_write(img + (size_t)sec * 512u,
                                         (uint32_t)(fat_start + sec), 1) != 0) {
                    free(img); free(new_fat); return -23;
                }
            }
        }
        free(img);
        fat_get_flush();   /* the FAT just changed under fat_get's read cache */
    }
    free(new_fat);
    nes_flash_disk_flush();

    /* Phase 3c: directory-entry patches. */
#define CLD_EFFECTIVE_SCLUST(file) \
    ((file).target_sclust != 0 ? (file).target_sclust : (file).current_sclust)

    for (int f = 0; f < ctx->n_files; f++) {
        cld_file_t *ent = &ctx->files[f];
        DWORD new_sc = CLD_EFFECTIVE_SCLUST(*ent);
        if (new_sc == ent->current_sclust) {
            /* unmoved */
        } else {
            LBA_t sfn_lba;
            uint32_t sfn_off;
            if (ent->parent_idx == -1) {
                LBA_t root_base = (LBA_t)g_fs.dirbase;
                sfn_lba = root_base + ent->byte_in_parent / 512;
                sfn_off = ent->byte_in_parent % 512;
            } else {
                cld_file_t *par = &ctx->files[ent->parent_idx];
                DWORD par_sclust = CLD_EFFECTIVE_SCLUST(*par);
                DWORD b = ent->byte_in_parent;
                DWORD in_cluster = b / ctx->bpc;
                DWORD in_byte    = b % ctx->bpc;
                DWORD clst       = par_sclust + in_cluster;
                sfn_lba = (LBA_t)g_fs.database + (LBA_t)(clst - 2) * g_fs.csize
                          + in_byte / 512;
                sfn_off = in_byte % 512;
            }
            uint8_t sec[512];
            if (nes_flash_disk_read(sec, (uint32_t)sfn_lba, 1) != 0) return -30;
            sec[sfn_off + 26] = (uint8_t)(new_sc & 0xFF);
            sec[sfn_off + 27] = (uint8_t)((new_sc >> 8) & 0xFF);
            if (nes_flash_disk_write(sec, (uint32_t)sfn_lba, 1) != 0) return -31;
        }
    }
    nes_flash_disk_flush();

    for (int f = 0; f < ctx->n_files; f++) {
        cld_file_t *ent = &ctx->files[f];
        if (!ent->is_dir) continue;
        DWORD my_sc = CLD_EFFECTIVE_SCLUST(*ent);
        DWORD parent_sc = 0;
        if (ent->parent_idx >= 0) {
            parent_sc = CLD_EFFECTIVE_SCLUST(ctx->files[ent->parent_idx]);
        }
        LBA_t first_lba = (LBA_t)g_fs.database + (LBA_t)(my_sc - 2) * g_fs.csize;
        uint8_t sec[512];
        if (nes_flash_disk_read(sec, (uint32_t)first_lba, 1) != 0) return -32;
        sec[26] = (uint8_t)(my_sc & 0xFF);
        sec[27] = (uint8_t)((my_sc >> 8) & 0xFF);
        sec[32 + 26] = (uint8_t)(parent_sc & 0xFF);
        sec[32 + 27] = (uint8_t)((parent_sc >> 8) & 0xFF);
        if (nes_flash_disk_write(sec, (uint32_t)first_lba, 1) != 0) return -33;
    }
    nes_flash_disk_flush();
#undef CLD_EFFECTIVE_SCLUST

    /* Phase 3d: remount. */
    f_unmount("");
    if (f_mount(&g_fs, "", 1) != FR_OK) return -40;

    return moves;
}

int lobby_defrag_compact(uint16_t *fb) {
    if (!s_lobby_fs) return -2;

    cld_ctx_t ctx = {0};

    ctx.files = (cld_file_t *)malloc(CLD_MAX_FILES * sizeof(cld_file_t));
    ctx.buf_a = (uint8_t *)malloc((size_t)g_fs.csize * 512);
    ctx.buf_b = (uint8_t *)malloc((size_t)g_fs.csize * 512);

    int rc = 0;
    if (!ctx.files || !ctx.buf_a || !ctx.buf_b) {
        rc = -2;
        goto cleanup;
    }

    s_defrag_last_error = 0;

    int ar = cld_analyze(&ctx);
    if (ar != 0) {
        if (ar == -4) {
            rc = -1000 - (int)ctx.n_clust;
        } else {
            rc = ar * 10;
        }
        goto cleanup;
    }

    if (ctx.n_files == 0) {
        rc = 0;
        goto cleanup;
    }

    int apply = 0;
    int moves_planned = 0, unplaceable = 0;
    for (;;) {
        moves_planned = 0;
        unplaceable = 0;
        for (int f = 0; f < ctx.n_files; f++) {
            if (ctx.files[f].target_sclust == 0) unplaceable++;
        }
        for (DWORD c = 0; c < ctx.n_clust; c++) {
            if (CLD_BIT_GET(ctx.pinned_bits, c)) continue;
            if (ctx.target_owner[c] != 0
                && ctx.target_owner[c] != ctx.current_owner[c]) moves_planned++;
        }

        cld_show_preview(&ctx, fb, moves_planned, unplaceable);
        int action = cld_wait_interaction();
        if (action == CLD_UI_APPLY)  { apply = 1; break; }
        if (action == CLD_UI_CANCEL) { apply = 0; break; }

        int new_idx = s_plan_k_idx + (action == CLD_UI_K_UP ? 1 : -1);
        if (new_idx < 0) new_idx = 0;
        if (new_idx >= CLD_PLAN_K_COUNT) new_idx = CLD_PLAN_K_COUNT - 1;
        if (new_idx != s_plan_k_idx) {
            s_plan_k_idx = new_idx;
            cld_plan_dispatch(&ctx);
        }
    }
    if (!apply) {
        rc = 0;
        goto cleanup;
    }

    int moves = cld_execute(&ctx, fb, moves_planned);
    if (moves < 0) {
        s_defrag_last_error = moves;
        rc = moves;
    } else {
        rc = moves;
    }

cleanup:
    free(ctx.files);
    free(ctx.current_owner);
    free(ctx.target_owner);
    free(ctx.pinned_bits);
    free(ctx.done_bits);
    free(ctx.buf_a);
    free(ctx.buf_b);
    if (s_pin_names) { free(s_pin_names); s_pin_names = NULL; }
    s_n_pin_names = 0;
    s_pin_display_idx = 0;
    s_n_reclaimed  = 0;
    s_reclaimed_kb = 0;
    return rc;
}

/* ----------------- count_fragmented ----------------------------- */

int lobby_defrag_count_fragmented_named(char *out_first, size_t out_sz) {
    if (!s_lobby_fs) return -1;
    if (out_first && out_sz > 0) out_first[0] = 0;
    enum { PATH_LEN = CLD_PATH_MAX, MAX_DIRS = 32 };
    char (*queue)[PATH_LEN] = (char(*)[PATH_LEN])malloc((size_t)MAX_DIRS * PATH_LEN);
    if (!queue) return -1;

    int q_head = 0, q_tail = 0, frag = 0;
    strncpy(queue[q_tail], "/", PATH_LEN - 1);
    queue[q_tail][PATH_LEN - 1] = 0;
    q_tail++;

    while (q_head < q_tail) {
        const char *dir_path = queue[q_head++];
        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, dir_path) != FR_OK) continue;
        while (f_readdir(&dir, &info) == FR_OK) {
            if (info.fname[0] == 0) break;
            if (info.fname[0] == '.'
                && (info.fname[1] == 0
                    || (info.fname[1] == '.' && info.fname[2] == 0))) {
                continue;
            }
            char full[PATH_LEN];
            if (dir_path[0] == '/' && dir_path[1] == 0)
                snprintf(full, sizeof(full), "/%s", info.fname);
            else
                snprintf(full, sizeof(full), "%s/%s", dir_path, info.fname);

            if (info.fattrib & AM_DIR) {
                if (q_tail < MAX_DIRS) {
                    strncpy(queue[q_tail], full, PATH_LEN - 1);
                    queue[q_tail][PATH_LEN - 1] = 0;
                    q_tail++;
                }
                continue;
            }
            if (info.fsize < 4096) continue;

            FIL f;
            if (f_open(&f, full, FA_READ) != FR_OK) continue;
            DWORD sc = f.obj.sclust;
            FSIZE_t sz = f_size(&f);
            f_close(&f);
            if (sz == 0 || sc < 2) continue;
            DWORD bpc = (DWORD)g_fs.csize * 512u;
            DWORD nc  = ((DWORD)sz + bpc - 1) / bpc;
            if (!chain_is_contiguous(sc, nc)) {
                if (frag == 0 && out_first && out_sz > 0) {
                    const char *tail = full;
                    size_t len = strlen(full);
                    if (len > 16) tail = full + len - 16;
                    snprintf(out_first, out_sz, "%s s%lu n%lu",
                             tail, (unsigned long)sc, (unsigned long)nc);
                }
                frag++;
            }
        }
        f_closedir(&dir);
    }

    free(queue);
    return frag;
}

int lobby_defrag_count_fragmented(void) {
    return lobby_defrag_count_fragmented_named(NULL, 0);
}
