#include "../types.h"
#include "card_constants.h"
#include "func_80036C14.h"
#include "duel_card.h"
#include "text_constants.h"
#include "duel_effect.h"
#include "text_encode_decimal_digits.h"
#include "text_stream_read_u32_le.h"
#include "text_stream_read_u16_le.h"
#include "duel_effect_command.h"
#include "../unmatched.h"
#ifdef MEMORIES_PC
#include "pc/cards/cards.h"
#include "pc/free_duel/duelists.h"
#include "pc/text/text.h"
#include "pc/text/number_width.h"
#include "pc/cards/stars.h"
#include "pc/cards/card_text_colors.h"
#include "pc/cards/duel_ui.h"
#include "pc/cards/type_icons.h"

/* A star's second name, left out: an empty string in the names bank. */
static const u8 no_star_name[] = {0xFF};
#endif

#define TEXT_STREAM_OWNER(object) ((TextStreamOwner *)(object))

/* Entries 0 through 12 of the secondary text-command table D_80090EAC,
   the handlers the F8 escape reaches, together with func_80038024, the
   helper two of them share -- which is not itself a table entry, so this is
   fourteen definitions rather than thirteen entries. Each entry takes the
   text channel and reads its operands from the channel's live stream. Entry
   13, Text_StartCampaignDuel, is next in both the table and the image, but it
   only builds at gcc_2_8_1_g0 and stays its own unit.

   The nine former sources were recorded at gcc_2_8_1_g8_split,
   gcc_2_8_1_g8, gcc_2_8_1_g0 and gcc_2_8_1_g0_split, and every member
   compiles to an identical object at gcc_2_8_1_g8_split. Bounded below by
   the primary handlers Text_ExtendGlyphCode and Text_SetStateFromStream in
   text_stream_commands.c. */

void func_80037DA4(DuelEffectChannel *object)
{
    s32 op;
    s32 id;
    s32 n;
    s32 kind;
    s32 stats;
    s32 type;
    u8 *text;
    u8 *current;
    u8 *G32 *slot;
#ifdef MEMORIES_PC
    s32 star = 0;   /* the guardian star an icon stands for (stars.h) */
    s32 naming = 0; /* the card's name: its letters are the card bar's "name" (duel_ui.h) */
    s32 full_type = 0; /* kind 0's real type (cards.h), past the Dragon stats keep for it */
#endif

    text = (u8 *G32)(s32)object->stream_58;
    object->field_62 = 0;
    text = (u8 *)((u32)text * 4);
    {
        u8 *stream = (u8 *)object;

        stream += (u32)text;
        text = stream;
        current = *(u8 *G32 *)text;
        op = *current++;
        *(u8 *G32 *)text = current;
    }
    n = 0;
#ifdef MEMORIES_PC
    /* A previous inserted card field may have borrowed this box's color. */
    CardTextColors_Restore(object);
#endif
    if (op & 0x10) {
        object->field_54 = D_8009B320;
        return;
    }
    if (op & 0x20) {
#ifdef MEMORIES_PC
        CardTextColors_Apply(object, CARD_TEXT_COLOR_NAME, 0);
        naming = 1;
#endif
#ifdef MEMORIES_PC
        /* A card past the disc's has a name of its own, from its mod, or
           its base's (cards.h); the text bank only has the retail ones. */
        text = (u8 *)Cards_RowNameText(gDuel_wSelectedCardID);   /* a variant's row: its label first */
        if (text != 0) {
            object->stream_58++;
            goto store;
        }
        if (Cards_NameText(gDuel_wSelectedCardID) != 0) {
            object->stream_58++;
            text = (u8 *)Cards_NameText(gDuel_wSelectedCardID);
            goto store;
        }
        id = Cards_BaseId(gDuel_wSelectedCardID) + 0x8000;
#else
        id = gDuel_wSelectedCardID + 0x8000;
#endif
    } else if (op & 0x40) {
#ifdef MEMORIES_PC
        CardTextColors_Apply(object, CARD_TEXT_COLOR_DESCRIPTION, 0);
#endif
#ifdef MEMORIES_PC
        if (Cards_DescriptionText(gDuel_wSelectedCardID) != 0) {
            object->stream_58++;
            text = (u8 *)Cards_DescriptionText(gDuel_wSelectedCardID);
            goto store;
        }
        id = Cards_BaseId(gDuel_wSelectedCardID) + 0xD100;
#else
        id = gDuel_wSelectedCardID + 0xD100;
#endif
    } else {
        kind = op & 0xF;
        id = 0;
        switch (kind) {
        case 0:
            id = (gDuel_adwCardStats[gDuel_wSelectedCardID - 1] >>
                  CARD_STAT_TYPE_SHIFT) & CARD_STAT_TYPE_MASK;
#ifdef MEMORIES_PC
            /* Past the retail word's 24 types (cards.h): the stats word
               itself keeps the Dragon compatibility value id just read, so
               the name and icon come from the card's real, banked type. */
            full_type = Cards_Type(gDuel_wSelectedCardID);
#endif
            break;
        case 1:
            stats = gDuel_adwCardStats[gDuel_wSelectedCardID - 1];
            id = (stats >> CARD_STAT_GUARDIAN_STAR_1_SHIFT) &
                 CARD_STAT_GUARDIAN_STAR_MASK;
            type = (stats >> CARD_STAT_TYPE_SHIFT) & CARD_STAT_TYPE_MASK;
#ifdef MEMORIES_PC
            star = id;
            /* A monster a mod gave no star (none of the disc's): no icon
               and no name, as for a card with no second (stars.h). */
            if (id == 0 && type < CARD_TYPE_MAGIC && Stars_NoStarUsed()) {
                if (op & 0x80) {
                    object->stream_58++;
                    text = (u8 *)no_star_name;
                    goto store;
                }
                n = 1;
            }
#endif
            id += 0x17;
            if ((u32)(type - CARD_TYPE_MAGIC) < CARD_NON_MONSTER_TYPE_COUNT) {
                object->field_62 = type;
            }
            break;
        case 2:
            id = (gDuel_adwCardStats[gDuel_wSelectedCardID - 1] >>
                  CARD_STAT_GUARDIAN_STAR_2_SHIFT) &
                 CARD_STAT_GUARDIAN_STAR_MASK;
#ifdef MEMORIES_PC
            /* A card with one star (the same one twice; none of the disc's)
               shows it once, as the disc shows a card with no second. */
            if (id != 0 && Stars_CardSingle(gDuel_wSelectedCardID)) {
                id = 0;
            }
            star = id;
            if (id == 0 && (op & 0x80)) {
                /* Nor a name for the star it does not have. */
                object->stream_58++;
                text = (u8 *)no_star_name;
                goto store;
            }
#endif
            id += 0x17;
            if (id == 0x17) {
                n = 1;
            }
            break;
        }
        if (!(op & 0x80)) {
            goto plain;
        }
#ifdef MEMORIES_PC
        if (kind == 1 || kind == 2)
            CardTextColors_Apply(object, CARD_TEXT_COLOR_GUARDIAN_STAR, star);
        /* A mod's own name for the type, whether past the disc's 24
           (cards.h) or a rename of one of those: the retail bank only has
           the disc's own spelling. */
        if (kind == 0 && Cards_TypeNameText(full_type) != 0) {
            object->stream_58++;
            text = (u8 *)Cards_TypeNameText(full_type);
            goto store;
        }
#endif
        id += 0x8300;
    }
    object->stream_58++;
    n = id;
    if (id > 0xCFFF) {
        text = (u8 *)((u32)D_801C0000 & 0xFFFF0000) +
               D_801C0000[id - 0xD000];
    } else if (id > 0x7FFF) {
        text = (u8 *)((u32)D_801D5800 & 0xFFFF0000) +
               D_801D5800[id - 0x8000];
    } else {
        if (id >= 0x500) {
            n = id - 0x100;
        }
        text = (u8 *)((u32)D_801B0000 & 0xFFFF0000) + D_801C0000[n];
    }
#ifdef MEMORIES_PC
    text = (u8 *)Text_Resolve(id, text);   /* a translation's (text.h) */
#endif
store:
    slot = &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
    *slot = text;
#ifdef MEMORIES_PC
    if (naming) {
        DuelUi_NameStream(object, 1);
    }
#endif
    return;
plain:
    object->flags_34 |= 0x80;
    if ((u8)n == 0) {
#ifdef MEMORIES_PC
        /* Stars 11-15 are past the disc's icons, and a mod may give any
           star its own: the entry says which star it is (stars.h). */
        Stars_MarkIcon(star);
        /* A type past the disc's 24 with an icon of its own: the entry says
           which (type_icons.h); id itself stays the Dragon compatibility
           value, same as the stats word, so func_80035E20's own math for it
           stays in range where no mark is taken. */
        if (kind == 0) TypeIcons_MarkIcon(full_type);
#endif
        func_80036C14(object, id);
    }
    object->flags_34 &= 0xFF7F;
    object->field_38 += 0x10;
}

void func_80038024(DuelEffectChannel *object, s32 value)
{
    *(u8 *)&object->flags_34 = *(u8 *)&object->flags_34;
    object->flags_34 |= 0x80;
    func_80036C14(object, value);
    object->flags_34 &= 0xFF7F;
    object->field_38 += 0x10;
}

void func_80038070(DuelEffectChannel *object)
{
#ifdef MEMORIES_PC
    /* The field bar's active star (func_80023144 keeps it + 0x17). A
       monster a mod gave no star has none to draw: the place stays, as
       func_80037DA4 leaves it for a card with no second star (stars.h). */
    if (D_8009B344 == 0x17 && Stars_NoStarUsed()) {
        object->flags_34 &= 0xFF7F;
        object->field_38 += 0x10;
        return;
    }
    Stars_MarkIcon(D_8009B344 - 0x17);
#endif
    func_80038024(object, D_8009B344);
}

void func_80038094(DuelEffectChannel *object)
{
    u8 *G32 *stream =
        &TEXT_STREAM_OWNER(object)->streams[object->stream_58];

    func_80038024(object, *(*stream)++);
}

void func_800380D4(DuelEffectChannel *object)
{
    register u8 *G32 *stream;
    register u8 *current;
    register u32 value;

    object->field_38 = 0;
    stream =
        &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
    current = *stream;
    value = current[0];
    current++;
    *stream = current;
    object->field_3A += (s8)value;
}

void func_80038110(DuelEffectChannel *object)
{
    u8 *G32 *stream =
        &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
    register u8 *G32 *slot = stream;
    register u8 *current = *slot;
    register u32 value = current[0];

    current++;
    *slot = current;
    object->field_38 += value;
}

void func_80038148(DuelEffectChannel *object)
{
    u8 buf[8];
    u8 *e;
    u8 *bp;
    s32 r;
    s32 c;
    s32 t;
    s32 k;
    s32 i;
    s32 h;
    s32 w;

    r = TextStream_ReadU32LE(TEXT_STREAM_OWNER(object));
    t = *TEXT_STREAM_OWNER(object)->streams[object->stream_58]++;
    c = t;
#ifdef MEMORIES_PC
    {
        /* A number wider than the field its string gives it keeps all of its
           digits, up to eight, rather than lose the first: three are how the
           retail strings print a card number, and the PC port's run to five
           (card_constants.h); four print ATK, DEF and LP, and six the
           starchips, which a mod's "limits" may take past 9999 and 999999
           (pc/cards/tables.h). A number that fits is left as it was. */
        s32 value = *(s32 *G32)r;
        s32 need = 1;
        s32 bound = 10;

        while (need < 8 && value >= bound) {
            need++;
            bound *= 10;
        }
        /* Fields of three digits and more: card numbers, ATK, DEF, LP and
           starchips. A one- or two-digit field prints what it always did. */
        if ((c & 0xF) >= 3 && need > (c & 0xF)) {
            /* The field keeps its width: the digits are drawn closer
               together (number_width.h), so nothing after it moves and a
               box sized for the field still holds it. A card number (three
               digits) is not squeezed, as it never was. */
            if ((c & 0xF) >= 4) {
                NumberWidth_Squeeze(object->index_57, need, c & 0xF, object->field_5A);
            }
            c = (c & 0xF0) | need;
        }
        if ((c & 0xF) > 6) {
            /* Past the six digits Text_EncodeDecimalDigits' table holds:
               the same digits, lowest first, and blanks for leading zeros. */
            for (i = 0; i < (c & 0xF); i++) {
                buf[i] = value % TEXT_DECIMAL_RADIX;
                value /= TEXT_DECIMAL_RADIX;
            }
            for (i = (c & 0xF) - 1; i > 0 && buf[i] == 0; i--) {
                buf[i] = TEXT_DECIMAL_BLANK_DIGIT;
            }
        } else {
            Text_EncodeDecimalDigits(*(s32 *G32)r, c & 0xF, buf);
        }
    }
#else
    Text_EncodeDecimalDigits(*(s32 *)r, c & 0xF, buf);
#endif

    h = 0;

    if ((c & 0x80) != 0) {
        if ((c & 0x40) == 0) {
            goto skip;
        }
        h = *(u16 *)&D_800EAFF8[0];
        e = object->text_44;
        goto write;
    }

    if (c < 2) {
        e = object->text_44;
        goto write;
    }

    bp = buf;
    k = c - 1;
    while (1) {
        if (bp[k] < TEXT_DECIMAL_RADIX) {
            break;
        }
        c = k;
        if (k < 2) {
            break;
        }
        k = c - 1;
    }

skip:
    e = object->text_44;

write:
    i = (c & 0xF) - 1;
    do {
        w = h;
        if (buf[i] < TEXT_DECIMAL_RADIX) {
            w = *(u16 *)&D_800EAFF8[buf[i]];
        }
        if (w >= TEXT_SINGLE_BYTE_GLYPH_LIMIT) {
            *e = (w >> 8) - 0x10;
            e[1] = w;
            e += 2;
        } else {
            *e = w;
            e += 1;
        }
        i--;
    } while (i >= 0);

    *e = TEXT_STRING_TERMINATOR;
    object->stream_58++;
    TEXT_STREAM_OWNER(object)->streams[object->stream_58] = object->text_44;
}

/* Inlining keeps the stream value and channel in independent live ranges. */
static __inline__ u32 read_operand(DuelEffectChannel *object)
{
    u8 *G32 *stream =
        &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
    u8 *cursor = *stream;
    u32 value = *cursor++;

    *stream = cursor;
    return value;
}

void func_800382A8(DuelEffectChannel *object)
{
    u32 value;

    object->flags_34 &= 0xFEFF;
    value = read_operand(object);
    switch (value) {
    case 1:
        object->field_5A = 8;
        object->field_5B = 8;
        break;
    case 2:
        object->field_5A = 8;
        object->field_5B = 12;
        break;
    }
    if (value == 1)
        object->flags_34 |= 0x100;
}

void func_80038334(DuelEffectChannel *object)
{
    /* Separate lifetimes preserve allocation across the two stream reads. */
    {
        u8 *G32 *stream =
            &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
        u8 *current = *stream;
        u8 value = *current++;

        *stream = current;
        object->field_5A = value;
    }
    {
        u8 *G32 *stream =
            &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
        u8 *current = *stream;
        u8 value = *current++;

        *stream = current;
        object->field_5B = value;
    }
}

void func_80038388(DuelEffectChannel *object)
{
    object->field_38 = TextStream_ReadU16LE(object);
}

void func_800383B0(DuelEffectChannel *object)
{
    object->field_60 = 0;
    object->field_61 = TextStream_ReadU16LE(object);
}

u32 *func_800383DC(DuelEffectChannel *a0) {
    DuelEffectChannel *a3 = a0;
    s32 a2 = D_8009B32E;
    u32 v1;
    u8 counter;
    s32 offset;
    u32 *slot;

    if (a2 > 0xCFFF) {
        v1 = ((u32)D_801C0000 & TEXT_BANK_ADDRESS_MASK) +
             D_801C0000[a2 - 0xD000];
    } else if (a2 > (TEXT_GLOBAL_STRING_ID_BASE - 1)) {
        v1 = ((u32)D_801D5800 & TEXT_BANK_ADDRESS_MASK) +
             D_801D5800[a2 - TEXT_GLOBAL_STRING_ID_BASE];
    } else {
        if (a2 >= 0x500) {
            a2 -= 0x100;
        }
        v1 = ((u32)D_801B0000 & TEXT_BANK_ADDRESS_MASK) + D_801C0000[a2];
    }
#ifdef MEMORIES_PC
    v1 = (u32)Text_Resolve(D_8009B32E, (const u8 *)v1);   /* a translation's (text.h) */
    /* then an added duelist's own name, which no translation has. */
    v1 = (u32)Duelists_Text(D_8009B32E, (const u8 *)v1);
#endif

    counter = *(u8 *)&a3->stream_58 + 1;
    *(u8 *)&a3->stream_58 = counter;
    offset = (u32)&((u32 *)0)[(s8)counter];
    slot = (u32 *)((u8 *)a3 + offset);
    *slot = v1;
    return slot;
}

void func_80038498(DuelEffectChannel *object)
{
    u8 *G32 *slot =
        &TEXT_STREAM_OWNER(object)->streams[object->stream_58];
    u8 *q = *slot;
    s32 v = *q;
    s32 w;

    *slot = q + 1;
    w = v;
    if (v & 0x80) {
        w = gText_abColorSlots[v & 0xF];
    }
    object->field_54 = w;
}

void func_800384E4(DuelEffectChannel*object){register DuelEffectChannel*obj;register u8 *G32 *stream;register u8*current;register unsigned int value;obj=object;obj->flags_34&=0xEFFF;stream=&((u8 *G32 *)obj)[obj->stream_58];current=*stream;value=*current;current++;*stream=current;if(value)obj->flags_34|=0x1000;}
