/* Card Number Passwords, based on Douglas's original mod. The shared
 * password/price policy keeps the shop and View > Card passwords in sync. */
#include "pc/mods/modapi.h"
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "pc/cards/passwords.h"
#include "overlays/password/shop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOCK_CARD_COUNT 722
#define MAX_OVERRIDES 4096

typedef struct { int id; int price; } PriceOverride;

static const MemoriesModHost *g_host;
static PriceOverride g_overrides[MAX_OVERRIDES];
static int g_override_count;
static int g_stock_default = -1;
static int g_added_default = 999999;
static void *g_original_lookup;
static void *g_original_password;
static void *g_original_price;

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    return s;
}

static int valid_price(long value) { return value >= -1 && value <= 999999; }

static void load_config(void)
{
    FILE *file = g_host->open_asset(g_host, "config.ini");
    char line[256];
    int in_cards = 0;
    if (!file) {
        g_host->log(g_host, "config.ini nao encontrado; usando valores padrao");
        return;
    }
    while (fgets(line, sizeof line, file)) {
        char *p = trim(line), *eq, *key, *value, *tail;
        long number, id;
        if (!*p || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            in_cards = strcmp(p, "[cards]") == 0;
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        key = trim(p);
        value = trim(eq + 1);
        number = strtol(value, &tail, 10);
        if (*trim(tail)) continue;
        if (!in_cards) {
            if (!strcmp(key, "stock_cards_default") && valid_price(number)) g_stock_default = (int)number;
            else if (!strcmp(key, "added_cards_default") && number >= 0 && number <= 999999) g_added_default = (int)number;
            continue;
        }
        id = strtol(key, &tail, 10);
        if (*trim(tail) || id < 1 || id > 32766 || number < 0 || number > 999999) continue;
        if (g_override_count < MAX_OVERRIDES) {
            g_overrides[g_override_count].id = (int)id;
            g_overrides[g_override_count].price = (int)number;
            g_override_count++;
        }
    }
    fclose(file);
}

/* An explicit manifest password wins over a numeric alias. Start with an
 * invalid BCD sentinel to distinguish an absent password from an explicit
 * disabled one. The shop table overrides the card's display password, as
 * it does in View > Card passwords. Never reapply prices to the loaded table:
 * percentage rules have already been applied by Main_RunPasswordMenu. */
static int mod_password(int id, unsigned *password)
{
    unsigned ignored_price = 0;
    *password = 0xFFFFFFFFu;
    Cards_OwnPassword(id, password);
    Tables_PasswordShop(id, &ignored_price, password);
    return *password != 0xFFFFFFFFu;
}

static s32 lookup_by_card_number(void)
{
    int i, card, id = 0;
    unsigned packed = 0, password;
    for (i = 0; i < 8; i++) {
        if (gPassword_abDigits[i] > 9) return 0;
        id = id * 10 + gPassword_abDigits[i];
        packed = (packed << 4) | gPassword_abDigits[i];
    }
    /* Match explicit passwords before IDs, including added cards and
     * replacements. Ascending IDs give duplicate passwords a stable winner. */
    for (card = 1; card <= gCard_nCount; card++)
        if (mod_password(card, &password) && password == packed) return card;
    if (!Cards_Valid(id)) return 0;
    if (mod_password(id, &password) && password == CARD_PASSWORD_NONE) return 0;
    return id;
}


/* An explicit password can occupy another card's numeric alias. Do not
 * print that alias on the other card: entering it would buy the wrong one.
 * Manifest cards/tables are fixed until restart; re-enable/reset invalidates
 * this small map, and a changed live count rebuilds it too. */
static unsigned char reserved_number[CARD_TABLE_ID_END];
static int reserved_count = -1;
static void reset_numbers(void) { reserved_count = -1; }
static void applied(int on) { (void)on; reset_numbers(); }
static int number_reserved(int id)
{
    int card;
    if (reserved_count != gCard_nCount) {
        memset(reserved_number, 0, sizeof(reserved_number));
        for (card = 1; card <= gCard_nCount; card++) {
            unsigned password, number = 0;
            int shift;
            if (!mod_password(card, &password) || password == CARD_PASSWORD_NONE) continue;
            for (shift = 28; shift >= 0; shift -= 4)
                number = number * 10 + ((password >> shift) & 15);
            if (number < CARD_TABLE_ID_END) reserved_number[number] = 1;
        }
        reserved_count = gCard_nCount;
    }
    return reserved_number[id];
}

static unsigned numbered_password(int id)
{
    unsigned password, packed = 0;
    int shift;
    if (!Cards_Valid(id)) return CARD_PASSWORD_NONE;
    if (mod_password(id, &password)) return password;
    if (number_reserved(id)) return CARD_PASSWORD_NONE;
    for (shift = 0; shift < 32; shift += 4, id /= 10)
        packed |= (unsigned)(id % 10) << shift;
    return packed;
}

static unsigned card_price(int id)
{
    unsigned price, password = CARD_PASSWORD_NONE;
    int i;
    for (i = g_override_count - 1; i >= 0; i--)
        if (g_overrides[i].id == id) return (unsigned)g_overrides[i].price;
    if (id <= STOCK_CARD_COUNT) {
        if (g_stock_default >= 0) return (unsigned)g_stock_default;
        return ((unsigned (*)(int))g_original_price)(id);
    }
    price = (unsigned)g_added_default;
    Tables_PasswordShop(id, &price, &password);
    return price;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (!from || from->api < 9) return 0;
    g_host = from;
    load_config();
    mod->api = MEMORIES_MOD_API;
    mod->name = "Card Number Passwords";
    mod->reset = reset_numbers;
    mod->applied = applied;
    if (!from->hook(from, (void *)Cards_Password, (void *)numbered_password, &g_original_password)) return 0;
    if (!from->hook(from, (void *)Cards_PasswordPrice, (void *)card_price, &g_original_price)) return 0;
    if (!from->hook(from, (void *)Password_LookupCardID, (void *)lookup_by_card_number, &g_original_lookup)) return 0;
    from->log(from, "passwords numericos ativos na loja e no visualizador; %d precos especificos", g_override_count);
    return 1;
}
