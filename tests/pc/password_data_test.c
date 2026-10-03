/* Shop and on-card viewer share the same policy, including added cards and
 * explicit overrides equal to the disc's value (previously lost). */
#include "../../src/pc/cards/password_data.c"
#include <assert.h>
#include <string.h>

int gCard_nCount = 724;
static int disc_ok = 1, reads;
static unsigned own[CARD_TABLE_ID_END], override[CARD_TABLE_ID_END];
static unsigned char has_own[CARD_TABLE_ID_END], has_override[CARD_TABLE_ID_END];
int Cards_Valid(int id) { return id >= 1 && id <= gCard_nCount; }
int Cards_OwnPassword(int id, unsigned *out)
{
    if (!has_own[id]) return 0;
    *out = own[id];
    return 1;
}
int Tables_PasswordShop(int id, unsigned *price, unsigned *password)
{
    int changed = 0;
    if (has_override[id]) {
        changed = *password != override[id];
        *password = override[id];
    }
    if (id == 723) { *price /= 10; changed = 1; }
    if (id == 724) { *price = 0; changed = 1; }
    return changed;
}
int Memories_DiscFileStart(const char *path) { (void)path; return disc_ok ? 0 : -1; }
static void word(unsigned char *p, unsigned value)
{
    int i;
    for (i = 0; i < 4; i++, value >>= 8) p[i] = value & 255;
}
int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    unsigned char *data = out;
    int id;
    assert(lba == TABLE_SECTOR && sectors == TABLE_SECTORS);
    memset(data, 0, sectors * 2048);
    for (id = 1; id <= CARD_COUNT; id++) {
        word(data + id * 8, 1000);
        word(data + id * 8 + 4, BLUE_EYES_PASSWORD);
    }
    reads++;
    return sectors;
}
int main(void)
{
    assert(Cards_Password(0) == CARD_PASSWORD_NONE);
    assert(Cards_Password(725) == CARD_PASSWORD_NONE && reads == 0);
    assert(Cards_Password(1) == BLUE_EYES_PASSWORD && reads == 1);
    assert(Cards_PasswordPrice(1) == 1000 && reads == 1);
    has_own[58] = 1; own[58] = 0x723;
    assert(Cards_Password(58) == 0x723);
    has_override[58] = 1; override[58] = BLUE_EYES_PASSWORD;
    assert(Cards_Password(58) == BLUE_EYES_PASSWORD);
    override[58] = CARD_PASSWORD_NONE;
    assert(Cards_Password(58) == CARD_PASSWORD_NONE);
    assert(Cards_Password(723) == CARD_PASSWORD_NONE);
    has_own[723] = 1; own[723] = 0x00000723;
    assert(Cards_Password(723) == 0x00000723);
    has_override[723] = 1; override[723] = 0x12345678;
    assert(Cards_Password(723) == 0x12345678);
    assert(Cards_PasswordPrice(723) == 99999);
    assert(Cards_PasswordPrice(723) == 99999); /* percentage applied once */
    assert(Cards_PasswordPrice(724) == 0);
    /* No disc: added and replaced cards' manifest passwords still work. */
    table_state = 0; disc_ok = 0;
    assert(Cards_Password(723) == 0x12345678 && reads == 1);
    has_override[58] = 0;
    assert(Cards_Password(58) == 0x723);
    assert(Cards_Password(1) == CARD_PASSWORD_NONE);
    puts("password data: ok");
    return 0;
}
