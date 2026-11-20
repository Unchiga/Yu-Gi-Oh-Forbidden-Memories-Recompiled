#ifndef MEMORIES_PC_SOFT_GPU_H
#define MEMORIES_PC_SOFT_GPU_H
#include "pc/compat/pgxp.h"
#include <stddef.h>
#include <stdint.h>

/* Software PS1 GPU: 1024x512 words of 15-bit color plus the mask bit.
 * Single-threaded. Timing, interlace and 24-bit display are not modelled. */
#define SOFT_GPU_WIDTH 1024
#define SOFT_GPU_HEIGHT 512

void SoftGpu_Reset(void);
/* Execute complete GP0 commands. A command cut off by the end of the buffer
 * is dropped; unknown opcodes consume one word. Returns words consumed. */
size_t SoftGpu_Gp0(const uint32_t *words, size_t count);
/* CPU-side transfers, clipped to VRAM with coordinate wraparound. */
void SoftGpu_Load(int x, int y, int w, int h, const uint16_t *pixels);
void SoftGpu_Store(int x, int y, int w, int h, uint16_t *pixels);
void SoftGpu_Move(int sx, int sy, int dx, int dy, int w, int h);
void SoftGpu_Fill(int x, int y, int w, int h, uint32_t rgb24);

/* Settling (notes/pc-build.md, "Settling VRAM before it is read"): every
 * reader of the words primitives raster into goes through one of these,
 * so that a raster which keeps a frame's primitives back (a later change)
 * draws them before anything reads what they would have drawn. The raster
 * is immediate today: settling only tells SoftGpu_SettleObserver.
 *
 * SoftGpu_Settle: whatever has been drawn within x,y,w,h (VRAM words,
 * wrapping as transfers do) is in VRAM when it returns. */
void SoftGpu_Settle(int x, int y, int w, int h);
/* The words a primitive's texture can read: its page (4-bit, depth 0, is
 * 64 words wide, 8-bit 128, 16-bit 256; 256 rows) and, below 16 bits, its
 * palette row (16 or 256 entries). For a host lookup keyed by a primitive's
 * texture (hd_text.c, menu_letters.c, ...). */
void SoftGpu_SettleTexture(int depth, int page_x, int page_y, int clut_x, int clut_y);
/* VRAM, settled whole first. For any reader that does not say which words
 * it reads, a code mod's included (SoftGpu_Vram is lent to mods). */
const uint16_t *SoftGpu_Vram(void);
#ifndef MEMORIES_MOD /* the port's own: not lent to code mods (build_config.py, NOT_LENT_TO_MODS) */
/* VRAM without settling: only for a reader that settled the words it reads
 * itself, or reads none that are drawn. tools/pc/check_vram_readers.py
 * lists every caller and why. */
const uint16_t *SoftGpu_VramRaw(void);
/* Tests: told every rectangle settled (SoftGpu_Vram's whole VRAM too). */
extern void (*SoftGpu_SettleObserver)(int x, int y, int w, int h);

/* The deferred raster (notes/pc-build.md, "The deferred raster"): while on,
 * a primitive drawn into a full-screen drawing area is kept back with the
 * state it came with and rastered at the first settle that crosses the
 * area, in the order it came, or dropped when a fill covers the whole area
 * (the game's clear). What it cannot keep back is rastered as it comes, as
 * ever. `on`: 0 off, 1 on, SOFT_GPU_DEFERRED_AUTO on only while a recorder
 * draws the picture at 2x and above and keeps its copy of VRAM
 * (SoftGpu_CopyTaken), which is the default (MEMORIES_DEFERRED_RASTER,
 * libgpu.c). Changing it settles everything. `poison`: a held area reads as
 * a loud colour to anyone who reads it without settling (a debug aid). */
#define SOFT_GPU_DEFERRED_AUTO 2
void SoftGpu_SetDeferred(int on, int poison);
int SoftGpu_Deferred(void);
/* The recorder's copy of VRAM (gl_picture.c's texture, which its primitives
 * sample and its moves copy from): taken whole and raw (SoftGpu_VramRaw)
 * after each replay, so it misses what is held there then. Right after it
 * takes it, the recorder calls SoftGpu_CopyTaken with itself (`taker`: only
 * the recorder set now that names itself keeps a copy, so a mod's recorder,
 * whose struct may end before `patch`, never does). A held area that the copy
 * missed is, when it is settled, rastered up to what it held then and its
 * words kept as a patch (the area as the copy would have had it), before
 * the rest is rastered. At the start of its next replay, before anything
 * in the record, and again at each flush of what it gathered, before it
 * draws and before the transfer that flushes it, whether anything is drawn
 * or not (its own host lookups settle too, during the replay), the
 * recorder calls SoftGpu_CopyPatches, which
 * hands each patch to its `patch` and forgets it; patches it has not taken
 * when it takes the copy again are forgotten by SoftGpu_CopyTaken (the copy
 * then has those words already). It returns how many it handed. Only a
 * recorder with `patch` that has taken its copy lets work be held at 2x and
 * above; a patch is made without allocating (it can be made from the
 * clock's tick): their buffer is made at the recorder's first take, and
 * without it no copy is kept. */
struct SoftGpuRecorder;
void SoftGpu_CopyTaken(const struct SoftGpuRecorder *taker);
int SoftGpu_CopyPatches(void);
/* How many texels past the last u and v a primitive's coordinates span the
 * recorder may read its copy at (texture xBR: gl_picture.c tells it at each
 * replay, before its patches), SOFT_GPU_REACH_PAGE for anywhere in the page
 * (wrapping as the texels do). What it reads there is settled as the rest
 * is (texture_rects), so the copy gets it as a patch. A change settles
 * everything (what is held was let hold under the old reach). */
#define SOFT_GPU_REACH_PAGE 256
void SoftGpu_SetCopyReach(int texels);
/* How many applied code mods bind SoftGpu_Vram or SoftGpu_StateData (mods.c):
 * such a mod may keep VRAM's pointer and write or read through it at any
 * time, so nothing is kept back while there is one. Told as each binding is
 * made (Mods_VramUsersChanged, set in libgpu.c) and at every flush. */
void SoftGpu_SetModVramUsers(int users);
/* Holds off the game clock's tick (SIGALRM, MEMORIES_CLOCK=interrupt) while
 * held work is queued, rastered or looked up: set by the platform when the
 * interrupt clock runs (platform_common.c), NULL otherwise. hold 1, 0. */
extern void (*SoftGpu_HoldClock)(int hold);
/* Whether the game clock's tick is running now (MEMORIES_CLOCK=interrupt:
 * there, between two instructions of the main thread, nothing may be
 * allocated, and an interrupt-level LoadImage can flush a batch): set by the
 * platform with SoftGpu_HoldClock (platform_common.c), NULL otherwise. A
 * primitive that would need a frozen copy made there is rastered at once. */
extern int (*SoftGpu_InTick)(void);
/* Microseconds from any fixed point, to time the deferred raster's settles
 * (SoftGpuDeferredStats.settle_us): set by the platform (platform_common.c);
 * NULL, not timed. */
extern unsigned long long (*SoftGpu_Clock)(void);
/* What the deferred raster did since SoftGpu_SetDeferred turned it on. */
enum {
    SOFT_GPU_WHY_READ,      /* a reader settled it (a store, a move's source, a host lookup, a hash...) */
    SOFT_GPU_WHY_WRITE,     /* a load, move or fill wrote into it, or into a texture it samples, or a bank or
                             * the pack's shadow it samples changed (soft_gpu.c, frozen_sync) */
    SOFT_GPU_WHY_DRAW,      /* a primitive rastered at once drew into it, or samples it */
    SOFT_GPU_WHY_AREA,      /* another drawing area took its place */
    SOFT_GPU_WHY_ALL,       /* all of VRAM: a state, a scale, the switch, a mod */
    SOFT_GPU_WHY_COUNT
};
enum {
    SOFT_GPU_NOW_BANK,      /* samples a texture bank, and there is no memory for its frozen copy */
    SOFT_GPU_NOW_SHADOW,    /* samples the texture pack's shadow, and no memory for its frozen copy */
    SOFT_GPU_NOW_TAGS,      /* would clear provenance tags or shadow cells */
    SOFT_GPU_NOW_TEXTURE,   /* its texture or palette lies in held work */
    SOFT_GPU_NOW_PANEL,     /* the life-point panel the opponent's name goes over */
    SOFT_GPU_NOW_OTHER,     /* widescreen's second pass, too much held, no memory (not a mod's VRAM or a
                             * scale: then nothing is looked at, or counted) */
    SOFT_GPU_NOW_AFTER,     /* one of the above drew into its area since the area's last clear, or the
                             * panel (a bank, the shadow) in the cycle before (soft_gpu.c, `again`) */
    SOFT_GPU_NOW_COUNT
};
#define SOFT_GPU_QUEUES 2
typedef struct SoftGpuDeferredStats {
    unsigned long long held, dropped, rastered; /* primitives kept back; of them, dropped and rastered */
    unsigned long long immediate;               /* into a full-screen area, rastered at once */
    unsigned long long cleared, settled;        /* areas' held work dropped whole / rastered */
    unsigned long long settle_us;               /* time spent rastering held work */
    unsigned long long settle_outside_us;       /* of it, outside SoftGpu_Gp0 (a reader's) */
    unsigned long long why[SOFT_GPU_WHY_COUNT];  /* settles, by cause */
    unsigned long long now[SOFT_GPU_NOW_COUNT];  /* immediate primitives, by cause */
    unsigned long long patches;                 /* patches made for the recorder's copy (SoftGpu_CopyTaken) */
    unsigned long long copy_missed;             /* areas held when the copy was taken (counted at each take) */
    /* Held now, queue by queue (two drawing areas can hold at once): the
     * area's corner (-1, -1 for none) and how many primitives it holds. */
    int queue_x[SOFT_GPU_QUEUES], queue_y[SOFT_GPU_QUEUES];
    unsigned long long queued[SOFT_GPU_QUEUES];
} SoftGpuDeferredStats;
void SoftGpu_DeferredStats(SoftGpuDeferredStats *out);
#endif

/* Texture banks: alternate VRAM that a primitive can sample instead, chosen
 * by bits 11-14 of its texture-page word, which the hardware leaves unused
 * and retail always writes as zero (bank 0 is VRAM itself), and for banks
 * 16-31 bit 10 too: a polygon's page word only (SOFT_GPU_BANK_TPAGE), as the
 * hardware ignores that bit there but an E1 command's is the drawing to the
 * shown area, so an E1 names banks 0-15 alone. They let several
 * monsters keep their 256x256 texture block at once, which the console's one
 * megabyte could never do; src/pc/mods uses them. A bank is VRAM-shaped, so
 * a primitive's page, window and palette coordinates mean the same in it.
 * Returns NULL if the bank cannot be allocated.
 *
 * Who writes which, so two never share a place: 1-12 the 3D Monsters mod
 * (a cached card's art and its model's textures, field_art.c and
 * field_models.c; bank 1 also its Card art glow, at x 0x100); 13 the card
 * frames (card_layout_art.c); 14 the star, attribute and type icons and the
 * duel UI's pictures (duel_ui.c), at places of their own; 15 the added
 * glyphs (glyphs.h); 17 and 18 a bigger battle model's own texture pages,
 * slot 0's and slot 1's (battle_models.c). */
#define SOFT_GPU_BANKS 32
/* A polygon's page word's bank bits for `bank` (0-31). */
#define SOFT_GPU_BANK_TPAGE(bank) ((((unsigned)(bank) & 15u) << 11) | (((unsigned)(bank) >> 4 & 1u) << 10))
uint16_t *SoftGpu_Bank(int bank);
/* The bank's pixels if it has been made, NULL otherwise (a primitive naming
 * a bank that was never made samples VRAM). */
const uint16_t *SoftGpu_BankPixels(int bank);
/* A textured polygon that samples a bank can also fade into what is under
 * it: the upper half of its third texture-coordinate word, which the hardware
 * ignores, is SOFT_GPU_FADE | amount, amount 0 (opaque) to 255 (gone). Both
 * renderers honour it; src/pc/mods (3D Monsters) uses it. */
#define SOFT_GPU_FADE 0x8000u
static inline int SoftGpu_FadeOf(uint32_t half)
{
    return (half & SOFT_GPU_FADE) ? (int)(half & 0xff) : 0;
}
/* Widescreen: full-screen drawing areas get a companion buffer 4/3 as wide
 * (see soft_gpu.c). Turning it off frees them. */
void SoftGpu_SetWidescreen(int on);
int SoftGpu_Widescreen(void);
/* The rule a drawing area x1..x2, y1..y2 (inclusive) gets a widescreen
 * target by: the margin it is widened by on each side, 0 for none (off, not
 * a full screen, or no room in VRAM's width). The OpenGL picture draws its
 * own targets by it (gl_picture.c). */
int SoftGpu_WideMargin(int x1, int y1, int x2, int y2);
/* Full-width flat rectangles include the game's global fades, their wipe
 * bands and tinted flashes. Extend only those in the widened replay;
 * ordinary sprites and small UI rectangles retain the console's clip. */
static inline int SoftGpu_WidenFill(const uint32_t *words, size_t count, int clip_x1, int clip_x2,
                                  int offset_x, int margin, uint32_t out[3])
{
    int x, width;
    if (count < 3 || (words[0] >> 24 != 0x60 && words[0] >> 24 != 0x62)) return 0;
    x = (int32_t)(words[1] << 21) >> 21;
    width = words[2] & 0x3ff;
    if (x + offset_x > clip_x1 || x + offset_x + width <= clip_x2 || width + 2 * margin > 1023) return 0;
    out[0] = words[0];
    out[1] = (words[1] & 0xffff0000u) | (uint16_t)(x - margin);
    out[2] = (words[2] & 0xffff0000u) | (uint32_t)(width + 2 * margin);
    return 1;
}
/* The widened picture of the display area x,y,w,h, if it has a target:
 * VRAM-shaped pixels, and the x and width to show. Returns 0 otherwise. */
int SoftGpu_WideFrame(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w);
/* The same picture without presenting it (frame dumps): nothing changes. */
int SoftGpu_WideFrameView(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w);
/* 0 while a recorder draws a scaled picture: the targets then keep the
 * bookkeeping (what WideFrame finds, whether anything was drawn) but not the
 * primitives, whose widened picture the recorder draws (SoftGpu_WidePicture
 * is NULL then), so their pixels are not to be shown. */
int SoftGpu_WideRastered(void);
/* The opponent's name over the life-point panel at the console's
 * resolution (hd_text.h, HdText_NamePixels): set while View > Opponent's
 * name for COM is on (libgpu.c), NULL otherwise, when nothing changes. */
extern const uint8_t *(*SoftGpu_PanelName)(int which, int *x, int *y, int *width, int *height, int *stride);
/* With it, whether the panel is drawn in pieces at any size (a mod's "ui",
 * pc/cards/duel_ui.h): the names then go over its quads too. */
extern int SoftGpu_PanelCut;
/* Scaled widened picture, SOFT_GPU_WIDTH * scale pixels per row. NULL at
 * console resolution, while a recorder draws the picture (it draws the
 * widened ones too), or if allocation failed. Call after WideFrame to also
 * observe its clearing of stale side borders. */
const uint32_t *SoftGpu_WidePicture(int x, int y, int w, int h);
/* Internal resolution: with a scale above 1 every primitive is also drawn,
 * at scale x scale pixels per VRAM word, into a second picture of the whole
 * of VRAM in 24-bit color, which is what is presented; VRAM itself stays
 * exactly what the console's would be, since the game reads it back and
 * states hold it. Uploads, fills and moves keep the picture in step; a
 * texture pack's images are sampled at their own resolution there
 * (texture_dump.h, `sample`). Returns 0 if the picture cannot be made. */
int SoftGpu_SetScale(int scale);
/* What is drawn through this palette shows only in the scaled picture: while
 * the recorder draws that, it is not rastered into VRAM as well. For a
 * full-screen picture nothing reads back, which would be most of a menu's
 * frame on the CPU. */
void SoftGpu_PictureOnlyClut(int clut_x, int clut_y);
/* While on, nothing drawn into a full-screen drawing area is rastered into
 * VRAM as well, while the recorder draws the scaled picture: for frames
 * nothing reads back from the frame buffer (a mod's whole scene, which
 * would otherwise cost the CPU its own pass at 1x). Set before each flush
 * (libgpu.c, from Mods_PictureOnlyFrame). */
void SoftGpu_PictureOnlyFrame(int on);
int SoftGpu_Scale(void);
/* The picture: SOFT_GPU_WIDTH * scale words per row, 0x00RRGGBB. */
const uint32_t *SoftGpu_Picture(void);
/* Redraw the picture from VRAM (after a state load). */
void SoftGpu_PictureFromVram(void);
/* A second renderer's record of what changes VRAM (gl_picture.h): every
 * GP0 batch as it is executed, every transfer made outside one, in order,
 * and a resync when VRAM or the state changed wholesale (a state load, a
 * reset, a new scale), with the drawing state as GP0 words E1 to E6. At a
 * scale of 1 only the resyncs are recorded (nothing draws a picture then;
 * the next scale's resync brings the state). While a recorder is set and
 * the scale is above 1 no picture is drawn here and SoftGpu_Picture() is
 * NULL: the recorder draws it. A load's pixels are
 * the caller's and only valid during the call. */
typedef struct SoftGpuRecorder {
    void (*gp0)(const uint32_t *words, size_t count);
    void (*load)(int x, int y, int w, int h, const uint16_t *pixels);
    void (*move)(int sx, int sy, int dx, int dy, int w, int h);
    void (*fill)(int x, int y, int w, int h, uint32_t rgb24);
    void (*resync)(int scale, const uint32_t state[6]);
    /* PGXP (pc/compat/pgxp.h): the precise vertices of the batch recorded
     * next, by their word's index in it. May be NULL. */
    void (*precise)(const PgxpVertex *vertices, size_t count);
    /* SoftGpu_Capture: dx,dy was just loaded with what sx,sy shows. May be
     * NULL. */
    void (*capture)(int sx, int sy, int dx, int dy, int w, int h);
    /* SoftGpu_CopyPatches: words x,y,w,h of the recorder's copy of VRAM
     * (w x h, row by row) as the copy should have had them when it was last
     * taken. Only the copy: not the picture. May be NULL (then nothing is
     * held at 2x and above). Read only once the recorder has named itself to
     * SoftGpu_CopyTaken: a recorder made with an older header (a mod's) ends
     * before it, and is never read this far. */
    void (*patch)(int x, int y, int w, int h, const uint16_t *words);
} SoftGpuRecorder;
/* The game read w x h of the picture at sx,sy (StoreImage) and loaded it,
 * as good as unchanged, at dx,dy to draw with (LoadImage): the recorder
 * keeps that part of its scaled picture for primitives textured from
 * dx,dy, which VRAM only has at the console's resolution. Color only: a
 * texel's transparency and semi-transparency bit stay VRAM's. */
void SoftGpu_Capture(int sx, int sy, int dx, int dy, int w, int h);
void SoftGpu_SetRecorder(const SoftGpuRecorder *recorder);
/* The precise vertices of the next SoftGpu_Gp0's words (PGXP), handed to
 * the recorder with them; the software GPU itself draws as ever. */
void SoftGpu_SetPrecise(const PgxpVertex *vertices, size_t count);
void SoftGpu_StateWords(uint32_t words[6]);

/* Save states: VRAM (index 0) and the drawing state (index 1). */
void *SoftGpu_StateData(int index, size_t *size);
#endif
