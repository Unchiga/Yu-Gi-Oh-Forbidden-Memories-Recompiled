#define MAIN_MODE_STATE_NEXT_AS_SCALAR
#define MAIN_MODE_STATE_ACTIVE_AS_SCALAR
#include "../types.h"
#include "../overlays/password/name_entry_keyboard.h"
#include "../overlays/password/shop.h"
#include "../psyq/rand.h"
#include "../psyq/setjmp.h"
#include "fade.h"
#include "file_transfer.h"
#include "main_menu_selection.h"
#include "game_over.h"
#include "main_modes.h"
#include "menu_record_reset.h"
#include "sound.h"
#include "main_services.h"
#include "options.h"
#include "../unmatched.h"
#include "main_mode_state.h"
#ifdef MEMORIES_PC
#include "card_constants.h"
#include "pc/cards/tables.h"
#include "pc/cards/passwords.h"
#include "pc/cards/pack_shop.h"
#endif

void Main_RunPasswordMenu(void)
{
    unsigned char flags = D_8009B26C;

    if ((flags & 0x40) == 0) {
        D_8009B26C = flags | 0x40;
        File_RequestPasswordPackage();
#ifdef MEMORIES_PC
        {   /* The mods' "passwords" (tables.h) over the table just loaded,
               then, once a run, two cards with one password noted. */
            static int checked;
            static unsigned passwords[CARD_TABLE_ID_END];
            int id;
            for (id = 1; id <= gCard_nCount; id++) {
                unsigned password = Cards_Password(id);
                if (id <= CARD_COUNT) {
                    D_801A8000[id].price = Cards_PasswordPrice(id);
                    D_801A8000[id].password = (s32)password;
                }
                passwords[id] = password;
            }
            if (!checked) {
                checked = 1;
                Tables_CheckPasswords(passwords);
            }
        }
#endif
        Password_InitShopScreen();
#ifdef MEMORIES_PC
        PackShop_Enter();   /* the card packs a mod sells (pack_shop.h) */
#endif
    }
    Password_UpdateShopScreen();
}
