#ifndef MEMORIES_PC_FIELDS_H
#define MEMORIES_PC_FIELDS_H

/* The duel-field registry.  The disc owns ids 1-6; the byte which records a
 * field leaves 7-255 for data mods.  A field name is an identity, just like a
 * card type or attribute name, so tables and tower floors may use it rather
 * than an opaque number. */

struct JsonValue;

#define FIELDS_RETAIL 6
#define FIELDS_MAX 255

void Fields_Clear(void);
void Fields_Build(void);
void Fields_Add(const char *mod, const struct JsonValue *manifest);

int Fields_Value(const struct JsonValue *value);
int Fields_Find(const char *name);
int Fields_Declared(int field);
const char *Fields_Name(int field);
/* Whether Fields_Name(field) is a mod's own word rather than the retail
 * carrier's: always true for a declared added field, true for a replaced
 * retail field only when the mod gave it a "name" of its own. The duel
 * HUD's FIELD box (hd_text.c) shows this instead of the carrier's retail
 * word when it is. */
int Fields_HasCustomName(int field);
/* The absolute PNG path declared as "picture", or NULL when the field uses
 * its retail board/has no picture. */
const char *Fields_Picture(int field);
/* The logical mod field and the retail terrain package it borrows for art. */
void Fields_Activate(int field);
int Fields_Active(void);
/* A duel begun on `field`, as a tower floor names it: active with no field
 * spell's effect pending. */
void Fields_Start(int field);
/* The duel's own setup (func_80024DC8, every entry point) calls this the
 * moment it resets gDuel_bTerrain for a new duel, so Fields_Active cannot
 * still report a previous duel's field. */
void Fields_ResetActive(void);
/* DuelEffect_ApplyTerrain (func_80024E58.c) calls this each time it commits
 * a new gDuel_bTerrain, so an ordinary retail field spell played after
 * Fields_Activate leaves Fields_Active reporting the terrain actually on
 * screen rather than a stale custom field. */
void Fields_Observe(void);
int Fields_Carrier(int field);
/* The board renderer asks this while it emits a centre-floor polygon.  An
 * added field with a full board picture and `"mirror": false` opts out of
 * the retail model's mirrored centre strip. */
int Fields_UnmirrorCentre(void);
/* Applies that remap to one quad's four raw UV halfwords (u8 u, u8 v packed
 * low/high) in place, given each corner's model-space Z. A no-op unless
 * Fields_UnmirrorCentre and every corner's V lies in the retail centre
 * band. */
void Fields_UnmirrorQuadUV(unsigned short uv[4], const int z[4]);

#endif
