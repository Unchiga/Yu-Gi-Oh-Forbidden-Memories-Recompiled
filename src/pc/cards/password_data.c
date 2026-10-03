/* Shared Password shop and View > Card passwords data. */
#include "passwords.h"
#include "tables.h"
#include "pc/sdk/disc.h"
#include "game/file_constants.h"
#include <stdio.h>

/* --- the disc's table ----------------------------------------------------- */

/* Password_LoadPackageStage reads the Password screen's package from
 * sector FILE_WA_PASSWORD_START_SECTOR of WA_MRG.MRG: 64 sectors of
 * pictures, 4 more, then 3 to 0x801A8000, the table. It has one record per
 * card id from 0, each the card's price in starchips and its password, a
 * nibble per digit, both little-endian words. */
#define TABLE_SECTOR (FILE_WA_PASSWORD_START_SECTOR + 64 + 4)
#define TABLE_SECTORS 3
#define BLUE_EYES_PASSWORD 0x89631139u  /* card 1, the check that the table is where it should be */

static unsigned table[CARD_ID_END], prices[CARD_ID_END];
static int table_state;                  /* 0 unread, 1 read, -1 not there */

static int bcd(unsigned value)
{
    int i;
    for (i = 0; i < 8; i++, value >>= 4) {
        if ((value & 0xF) > 9) return 0;
    }
    return 1;
}

static void read_table(void)
{
    static unsigned char data[TABLE_SECTORS * 2048];
    int start = Memories_DiscFileStart("\\DATA\\WA_MRG.MRG;1"), id, none = 0;
    table_state = -1;
    if (start < 0 || Memories_DiscReadSectors(start + TABLE_SECTOR, TABLE_SECTORS, data) != TABLE_SECTORS) {
        fprintf(stderr, "memories-pc: card passwords: the disc's table cannot be read\n");
        return;
    }
    for (id = 1; id <= CARD_COUNT; id++) {
        const unsigned char *p = data + id * 8 + 4;
        unsigned value = (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
        if (value != CARD_PASSWORD_NONE && !bcd(value)) {
            fprintf(stderr, "memories-pc: card passwords: card %d's is not a password (%08x); not shown\n", id, value);
            return;
        }
        none += value == CARD_PASSWORD_NONE;
        table[id] = value;
        p -= 4;
        prices[id] = (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
    }
    table_state = 1;
    fprintf(stderr, "memories-pc: card passwords: read, card 1's is %08X%s, %d cards have none\n", table[1],
            table[1] == BLUE_EYES_PASSWORD ? " (as it should be)" : " (not 89631139: a changed disc)", none);
}

/* Resolve afresh over the cached raw disc values: a rule that sets the
 * retail password still wins over cards[].password; price percentages are
 * never applied twice. Added cards do not depend on a disc read succeeding. */
static void resolve(int id, unsigned *price, unsigned *password)
{
    *price = CARD_PASSWORD_DEFAULT_PRICE;
    *password = CARD_PASSWORD_NONE;
    if (!Cards_Valid(id)) return;
    if (id <= CARD_COUNT) {
        if (!table_state) read_table();
        if (table_state > 0) {
            *price = prices[id];
            *password = table[id];
        }
    }
    Cards_OwnPassword(id, password);
    Tables_PasswordShop(id, price, password);
}

unsigned CardPassword_Resolve(int id)
{
    unsigned price, password;
    resolve(id, &price, &password);
    return password;
}

unsigned CardPassword_ResolvePrice(int id)
{
    unsigned price, password;
    resolve(id, &price, &password);
    return price;
}
