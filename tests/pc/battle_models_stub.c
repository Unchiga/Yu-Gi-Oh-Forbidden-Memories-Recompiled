/* cards.c without the game: a mod's bigger model never fits (the battle's
 * check parses it, src/pc/cards/battle_models.c). */
int BattleModels_Check(int slot);
int BattleModels_Check(int slot) { (void)slot; return 0; }
