/* PC-only policy entry points. Resident game code makes these hookable by
 * code mods, so the shop and on-card viewer always ask the same policy. */
#ifdef MEMORIES_PC
#include "pc/cards/passwords.h"

unsigned Cards_Password(int id)
{
    return CardPassword_Resolve(id);
}

unsigned Cards_PasswordPrice(int id)
{
    return CardPassword_ResolvePrice(id);
}
#endif
