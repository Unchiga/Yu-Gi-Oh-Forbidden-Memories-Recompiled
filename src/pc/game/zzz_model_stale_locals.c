/* The storage of the model modules' stale locals (pc/overlays/model_stale_locals.h).
 *
 * A game unit, like trigger_state.c, so it is in the game's data and in every
 * save state, as the retail stack it stands for (the game stack) is: a state
 * saved in the middle of an attack comes back reading what it read. Game
 * units link in name order, and "zzz_" keeps this one after
 * zz_tower_storage.c, so no existing variable moves. Its zeros still make
 * the game's bss longer, and state.c restores a bss chunk only of this
 * build's size, so a state from an earlier build loads without the game's
 * zeroed variables (as after zz_tower_storage.c came in). The translated
 * build (Android) numbers its globals in link order, so there a state from
 * an earlier build is refused, as after any new game variable or module.
 *
 * Zero at boot. The retail images of shape 169 all wait (`delay` 65) before
 * the trail moves, and the waiting branch writes vertices[0], so the first
 * read takes this attack's value. A mod's row with `delay` 0 reads the slot's
 * previous attack's value (or zero), where retail read whatever its stack
 * held. */
#include "pc/overlays/model_stale_locals.h"

SVECTOR gModelVariant169Vertices[2][4] = {{{0}}};
