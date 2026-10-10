/* Guardian star icons a mod gives (stars.h): made from its PNG at the
 * console's 16x16 and 4 bits, kept in a texture bank of the software GPU
 * (soft_gpu.h), and drawn where the game draws a star's icon: the text's
 * icon entries (the SELECT A GUARDIAN STAR box, the card view, the field
 * bar, the lists) and the battle's star effect.
 *
 * The disc has icons for stars 1-10 only, in the boot sheet's page 0xB
 * with one palette at (0x280, 0xFA); an icon id past them is a controller
 * button, and the battle effect draws an error cross. A star with no icon
 * of the mod's stays the disc's (1-10) or, past them, is a plain disc in the
 * stars' own colors. Nothing of the console's VRAM is used. */
#include "stars.h"
#include "art.h"
#include "pc/mods/mods.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_pack.h"
#include <stdio.h>
#include <string.h>

#define ICON_BANK 14            /* 3D Monsters takes 1-12, the added glyphs 15 */
#define ICON_PAGE 0             /* the bank's page at x 0, y 0 */
#define STAR_CLUT_X 0x280       /* the disc's stars' palette */
#define STAR_CLUT_Y 0xFA
#define OWN_CLUT_Y 256          /* + star: a PNG's own palette, in the bank */
#define RETAIL_PAGE_X 704       /* page 0xB, where the disc's stars are */
#define SIDE CARD_ICON_SIDE

static unsigned pending;
static unsigned char made[STARS_MAX + 1];   /* 0 not yet, 1 made, 2 failed */

static int own_icon(int star)
{
    return star >= 1 && star <= STARS_MAX && (star > STARS_RETAIL || Stars_Icon(star));
}

void Stars_MarkIcon(int star)
{
    pending = own_icon(star) ? STARS_ICON_CODE | (unsigned)star : 0;
}

unsigned Stars_TakeIconMark(void)
{
    unsigned code = pending;
    pending = 0;
    return code;
}

static void store(uint16_t *bank, int star, const unsigned char *pixels)
{
    int y, x;
    for (y = 0; y < SIDE; y++) {
        for (x = 0; x < SIDE / 4; x++) {
            const unsigned char *p = &pixels[y * SIDE / 2 + x * 2];
            bank[y * SOFT_GPU_WIDTH + ICON_PAGE * 64 + (star - 1) * SIDE / 4 + x] = (uint16_t)(p[0] | (p[1] << 8));
        }
    }
}

/* The disc's stars' palette, as the boot sheet loaded it. */
static void star_palette(unsigned short palette[16])
{
    memcpy(palette, &SoftGpu_Vram()[STAR_CLUT_Y * SOFT_GPU_WIDTH + STAR_CLUT_X], 16 * sizeof(*palette));
}

/* A star with no icon: a disc in the stars' colors, its edge in the one
 * the disc's icons outline with and its face in the one they fill with. */
static void plain_disc(unsigned char *pixels)
{
    const uint16_t *vram = SoftGpu_Vram();
    int edge_count[16] = {0}, fill_count[16] = {0}, edge = 1, fill = 2, star, x, y, i;
    /* Count, over the ten icons, which index borders the transparent and
     * which is inside. */
    for (star = 0; star < STARS_RETAIL; star++) {
        int u = 0x80 + (star & 7) * 16, v = 0x30 + (star >> 3) * 16;
        for (y = 0; y < SIDE; y++) {
            for (x = 0; x < SIDE; x++) {
                int tu = u + x, index, outside = 0, dx, dy;
                index = (vram[(v + y) * SOFT_GPU_WIDTH + RETAIL_PAGE_X + tu / 4] >> ((tu & 3) * 4)) & 0xF;
                if (!index) continue;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy, nu = u + nx;
                        if (nx < 0 || ny < 0 || nx >= SIDE || ny >= SIDE ||
                            !((vram[(v + ny) * SOFT_GPU_WIDTH + RETAIL_PAGE_X + nu / 4] >> ((nu & 3) * 4)) & 0xF))
                            outside = 1;
                    }
                }
                if (outside) edge_count[index]++; else fill_count[index]++;
            }
        }
    }
    for (i = 1; i < 16; i++) {
        if (edge_count[i] > edge_count[edge]) edge = i;
    }
    fill = edge == 1 ? 2 : 1;
    for (i = 1; i < 16; i++) {
        if (i != edge && fill_count[i] > fill_count[fill]) fill = i;
    }
    memset(pixels, 0, SIDE * SIDE / 2);
    for (y = 0; y < SIDE; y++) {
        for (x = 0; x < SIDE; x++) {
            /* Twice the distance from the middle (7.5, 7.5), squared. */
            int dx = 2 * x - 15, dy = 2 * y - 15, d = dx * dx + dy * dy, index;
            if (d > 13 * 13) continue;
            index = d > 10 * 10 ? edge : fill;
            pixels[(y * SIDE + x) / 2] |= (unsigned char)(index << ((x & 1) * 4));
        }
    }
}

static int make(int star, uint16_t *bank)
{
    unsigned char pixels[SIDE * SIDE / 2];
    unsigned short palette[16], clut[16];
    char why[1400];
    const char *icon = Stars_Icon(star);
    star_palette(palette);
    if (icon) {
        int own = Stars_IconOwnPalette(star);
        if (!CardArt_IconFromImage(icon, own ? NULL : palette, pixels, clut, why, sizeof(why))) {
            fprintf(stderr, "memories-pc: guardian star %d: %s\n", star, why);
            if (star <= STARS_RETAIL) return 0;   /* the disc's icon, then */
            plain_disc(pixels);
        } else if (own) {
            memcpy(&bank[(OWN_CLUT_Y + star) * SOFT_GPU_WIDTH], clut, sizeof(clut));
        }
        /* The indexed 16x16 form is the console/1x texture; retain the
         * source PNG for higher internal resolutions. */
        TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_STARS);
        TexturePack_AddBankSprite(ICON_BANK, ICON_PAGE * 64, 0, 0, (star - 1) * SIDE, 0, SIDE, SIDE, icon);
    } else {
        plain_disc(pixels);
    }
    store(bank, star, pixels);
    return 1;
}

int Stars_EffectCell(int star, int *tpage, int *u, int *v, int *clut)
{
    int clut_x, clut_y;
    if (!Stars_IconCell(own_icon(star) ? STARS_ICON_CODE | (unsigned)star : 0, tpage, u, v, &clut_x, &clut_y))
        return 0;
    *clut = (clut_y << 6) | ((clut_x >> 4) & 0x3F);
    return 1;
}

int Stars_IconCell(unsigned code, int *tpage, int *u, int *v, int *clut_x, int *clut_y)
{
    uint16_t *bank;
    int star = (int)(code & 0xF);
    if ((code & 0xFFF0u) != STARS_ICON_CODE || !own_icon(star)) return 0;
    if (made[star] == 2 || !(bank = SoftGpu_Bank(ICON_BANK))) return 0;
    if (!made[star]) made[star] = make(star, bank) ? 1 : 2;
    if (made[star] != 1) return 0;
    /* The bank's palettes are its own: the stars' is copied in each time,
     * as the game may have changed it (a fade). */
    memcpy(&bank[STAR_CLUT_Y * SOFT_GPU_WIDTH + STAR_CLUT_X],
           &SoftGpu_Vram()[STAR_CLUT_Y * SOFT_GPU_WIDTH + STAR_CLUT_X], 16 * sizeof(uint16_t));
    /* And the one func_80035E20's shaded pass (GT4) draws over it with. */
    memcpy(&bank[0xFF * SOFT_GPU_WIDTH + 544], &SoftGpu_Vram()[0xFF * SOFT_GPU_WIDTH + 544], 16 * sizeof(uint16_t));
    *tpage = ICON_PAGE | (ICON_BANK << 11);
    *u = (star - 1) * SIDE;
    *v = 0;
    if (Stars_Icon(star) && Stars_IconOwnPalette(star)) {
        *clut_x = 0;
        *clut_y = OWN_CLUT_Y + star;
    } else {
        *clut_x = STAR_CLUT_X;
        *clut_y = STAR_CLUT_Y;
    }
    return 1;
}
