#ifndef MEMORIES_PC_PASSWORDS_H
#define MEMORIES_PC_PASSWORDS_H
/* View > Card passwords (SET_CARD_PASSWORDS): the card's eight-digit
 * password in the card viewer the duel, Build Deck (deck and trunk) and
 * Trade share, and in the Library's card page (notes/pc-build.md).
 *
 * The password is a line of the game's own text: once the card's face is
 * up and its text has settled, the viewer's text box is made again in
 * place from its layout with the digits on the second star's row, so HD
 * text and a mod's fonts draw them as they draw the rest; on the frame the
 * viewer starts to close it goes back to the game's layout. Both happen in
 * the two screens' updates (the tables in duel_effect_tables.c and
 * main_modes.c call the wrappers below in place of the game's functions).
 * Off, nothing is made again and nothing changes. */
#include "cards.h"

/* The string id of the layout with the password (Text_Resolve). */
#define CARD_PASSWORD_TEXT_ID 0xFFFE

/* Card `id`'s password: eight BCD digits, as the disc's price and password
 * table (the one the Password screen loads to 0x801A8000) has them, or a
 * mod's "password" for the card; CARD_PASSWORD_NONE when the card has
 * none. The disc's table is read once, the first time it is asked. */
unsigned Cards_Password(int id);
/* Price for any loaded card. Added cards default to 999999 until a mod
 * supplies a price through "passwords". These two policy entry points are
 * hookable game functions; both the shop and viewer use Cards_Password. */
#define CARD_PASSWORD_DEFAULT_PRICE 999999u
unsigned Cards_PasswordPrice(int id);
/* Unhooked defaults, backed by the disc, cards[] and passwords tables. */
unsigned CardPassword_Resolve(int id);
unsigned CardPassword_ResolvePrice(int id);

/* Text_Resolve's question: the composed line for CARD_PASSWORD_TEXT_ID. */
const unsigned char *CardPassword_Text(int id);

/* gDuelEffect_apfnStateHandler's card viewer and gMain_apfnModeRunner's
 * Library under MEMORIES_PC: the game's function, then the password. */
void CardPassword_UpdateViewer(void);
void CardPassword_RunLibraryMenu(void);
#endif
