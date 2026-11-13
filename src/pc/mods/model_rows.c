/* The control-module rows code mods register: model_rows.h. */
#include "pc/mods/model_rows.h"

#include <stdio.h>
#include <string.h>

#include "pc/guest/sha256.h"
#include "pc/mods/mods.h"
#include "pc/overlays/model_gate_command.h"

typedef struct {
    ModelGateRow row;
    int used;
    int primary;
    int over_disc;
    char sha256[65]; /* lowercase; row.sha256 points here */
} Slot;

/* Never compacted while in use: a removed row's slot is reused by a later
 * Add, so Find's pointers hold until the next change (model_rows.h). */
static Slot rows[MODEL_ROWS_MAX];
static int high;        /* slots in use or once used: rows[0..high) */
static int live;        /* rows in use */
static int serial;      /* the last token */
static unsigned generation;
static uint32_t running;
static unsigned state_loads;

#define VARIANT_BYTES 0x5000u
#define PRIMARY_BYTES 0x1000u
/* Where an update's entry may be read in the work area (`context`): the
 * work area is the slot's module data argument, 0x80135000 (slot 0) or
 * 0x80175000 (slot 1), up to the primary module at 0x8013A000 / 0x8017A000
 * (notes/high-memory-load-addresses.md: D_8001001C, D_80010020, D_8001000C,
 * D_80010010), so 0x5000 bytes. 70 of the native shapes read their entry past
 * 0x1000, the furthest at 0x4A64 (shape 446). */
#define WORK_AREA_BYTES 0x5000u

static int refuse(char *why, size_t size, const char *text)
{
    if (why && size) snprintf(why, size, "%s", text);
    return 0;
}

static int hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* A slot that is not in use, other than `taken`; -1 when the array is full. */
static int free_slot(int taken)
{
    int next = high;
    for (int i = 0; i < high; i++) {
        if (!rows[i].used && i != taken) return i;
    }
    if (next == taken) next++;
    return next < MODEL_ROWS_MAX ? next : -1;
}

int ModelRows_Add(int owner, const MemoriesModelModule *from, char *why, size_t size)
{
    MemoriesModelModule row;
    uint32_t limit, copied;
    int slots[2], count, token;
    char text[160];
    if (why && size) why[0] = '\0';
    if (owner < 0 || !from) return refuse(why, size, "no row");
    copied = from->size < sizeof(row) ? from->size : (uint32_t)sizeof(row);
    if (copied < offsetof(MemoriesModelModule, entry) + sizeof(row.entry)) {
        snprintf(text, sizeof(text), "the row's size (%u) does not reach its entry: set size to sizeof(MemoriesModelModule)",
                 (unsigned)from->size);
        return refuse(why, size, text);
    }
    memset(&row, 0, sizeof(row));
    memcpy(&row, from, copied);
    if (row.flags & ~(MEMORIES_MODEL_BOUND | MEMORIES_MODEL_OVER_DISC)) {
        snprintf(text, sizeof(text), "flags 0x%X has bits this game does not know (MEMORIES_MODEL_BOUND, _OVER_DISC)",
                 (unsigned)row.flags);
        return refuse(why, size, text);
    }
    if (row.kind > MEMORIES_MODEL_PRIMARY) return refuse(why, size, "kind is not MEMORIES_MODEL_VARIANT or _PRIMARY");
    if (row.slot > MEMORIES_MODEL_SLOT_BOTH) return refuse(why, size, "slot is not MEMORIES_MODEL_SLOT_A, _B or _BOTH");
    if (!row.entry) return refuse(why, size, "no entry");
    limit = row.kind == MEMORIES_MODEL_PRIMARY ? PRIMARY_BYTES : VARIANT_BYTES;
    if (row.span < 8 || row.span > limit) {
        snprintf(text, sizeof(text), "span 0x%X is not from 8 (the identifier and the entry word) to the module's 0x%X",
                 (unsigned)row.span, (unsigned)limit);
        return refuse(why, size, text);
    }
    for (int i = 0; i < 64; i++) {
        if (!hex_digit(row.sha256[i])) return refuse(why, size, "sha256 is not 64 hex digits");
    }
    if (row.sha256[64]) return refuse(why, size, "sha256 is not 64 hex digits");
    if (row.flags & MEMORIES_MODEL_BOUND) {
        uint32_t width = row.index_width ? row.index_width : 4u;
        if (row.index_width != 0 && row.index_width != 1 && row.index_width != 2 && row.index_width != 4)
            return refuse(why, size, "index_width is not 0, 1, 2 or 4");
        if (row.entries == MODEL_GATE_NO_TABLE)
            return refuse(why, size, "entries 0xFFFFFFFF bounds nothing: leave out MEMORIES_MODEL_BOUND");
        if (row.modulus && row.index_width)
            return refuse(why, size, "a modulus is only for a pointer row (index_width 0)");
        if (!row.index_width &&
            (!row.stride || (uint64_t)row.table + (uint64_t)row.entries * row.stride > (uint64_t)row.span))
            return refuse(why, size, "the bound's table (table + entries * stride, stride above 0) is not inside the span");
        if ((uint64_t)row.field + width > WORK_AREA_BYTES)
            return refuse(why, size, "the bound's field is past the work area (0x5000 bytes)");
    } else {
        row.entries = MODEL_GATE_NO_TABLE;
        row.table = row.stride = row.field = row.index_width = row.modulus = 0;
    }
    count = row.slot == MEMORIES_MODEL_SLOT_BOTH ? 2 : 1;
    slots[0] = free_slot(-1);
    slots[1] = count == 2 && slots[0] >= 0 ? free_slot(slots[0]) : -1;
    if (slots[0] < 0 || (count == 2 && slots[1] < 0) || serial == 0x7FFFFFFF) {
        snprintf(text, sizeof(text), "more than %d control-module rows", MODEL_ROWS_MAX);
        return refuse(why, size, text);
    }
    token = ++serial;
    for (int n = 0; n < count; n++) {
        Slot *slot = &rows[slots[n]];
        memset(slot, 0, sizeof(*slot));
        for (int i = 0; i < 64; i++) {
            char c = row.sha256[i];
            slot->sha256[i] = c >= 'A' && c <= 'F' ? (char)(c - 'A' + 'a') : c;
        }
        slot->used = 1;
        slot->primary = row.kind == MEMORIES_MODEL_PRIMARY;
        slot->over_disc = (row.flags & MEMORIES_MODEL_OVER_DISC) != 0;
        slot->row.slot = count == 2 ? n : (int)row.slot;
        slot->row.identifier = row.identifier;
        slot->row.span = row.span;
        slot->row.sha256 = slot->sha256;
        /* The game's call is the mod's: the same two arguments and an int back. */
        slot->row.entry = (ModelEntry)(void (*)(void))row.entry;
        slot->row.entries = row.entries;
        slot->row.table = row.table;
        slot->row.stride = row.stride;
        slot->row.field = row.field;
        slot->row.index_width = row.index_width;
        slot->row.modulus = row.modulus;
        slot->row.owner = owner;
        slot->row.token = token;
        if (slots[n] >= high) high = slots[n] + 1;
        live++;
    }
    generation++;
    return token;
}

void ModelRows_Remove(int owner, int token)
{
    int changed = 0;
    for (int i = 0; i < high; i++) {
        if (rows[i].used && rows[i].row.owner == owner && rows[i].row.token == token) {
            rows[i].used = 0;
            live--;
            changed = 1;
        }
    }
    if (changed) generation++;
}

void ModelRows_Clear(int owner)
{
    int changed = 0;
    for (int i = 0; i < high; i++) {
        if (rows[i].used && rows[i].row.owner == owner) {
            rows[i].used = 0;
            live--;
            changed = 1;
        }
    }
    if (changed) generation++;
}

int ModelRows_Count(void) { return live; }
unsigned ModelRows_Generation(void) { return generation; }

static int hex_equal(const uint8_t digest[32], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        if (hex[2 * i] != digits[digest[i] >> 4] || hex[2 * i + 1] != digits[digest[i] & 15]) return 0;
    }
    return 1;
}

const ModelGateRow *ModelRows_Find(int primary, int slot, uint32_t identifier, const void *image, int over_disc,
                                   uint32_t *longest)
{
    /* Digests by span: rows of one module often share it. */
    uint32_t spans[8];
    uint8_t digests[8][32];
    int hashed = 0;
    const Slot *best = NULL;
    unsigned best_sequence = 0;
    if (longest) *longest = 0;
    if (!live) return NULL;
    for (int i = 0; i < high; i++) {
        const Slot *row = &rows[i];
        const uint8_t *digest = NULL;
        unsigned sequence;
        if (!row->used || row->primary != (primary != 0) || row->row.slot != slot || row->row.identifier != identifier ||
            !Mods_Active(row->row.owner))
            continue;
        if (longest && row->row.span > *longest) *longest = row->row.span;
        if (row->over_disc != (over_disc != 0)) continue;
        sequence = Mods_Sequence(row->row.owner);
        if (best && sequence <= best_sequence) continue;
        for (int n = 0; n < hashed; n++) {
            if (spans[n] == row->row.span) digest = digests[n];
        }
        if (!digest) {
            int n = hashed < 8 ? hashed++ : 7;
            spans[n] = row->row.span;
            Sha256(image, row->row.span, digests[n]);
            digest = digests[n];
        }
        if (hex_equal(digest, row->sha256)) {
            best = row;
            best_sequence = sequence;
        }
    }
    return best ? &best->row : NULL;
}

void ModelRows_SetRunning(uint32_t base) { running = base; }
uint32_t ModelRows_Running(void) { return running; }
void ModelRows_StateLoaded(void) { state_loads++; }
unsigned ModelRows_StateLoads(void) { return state_loads; }
