#ifndef MEMORIES_PC_PLATFORM_MENU_LETTERS_H
#define MEMORIES_PC_PLATFORM_MENU_LETTERS_H
/* A menu button's words in the game's own letters, on the game's own frame
 * (menu_letters.c): an item's "font": "game" (notes/modding.md, "The
 * title's menus"). The letters are cut out of the title's entries as the
 * main menu's package put them in VRAM, so this works only while the title
 * is open (TitleImages_Prepare), and only for words whose letters the
 * entries have. The PNG is `factor` times the size it is drawn at, as
 * MenuLabel_Make's; `stand_in` is the entry whose frame it is made on (an
 * entry's own; -1 for a button: SAVE's). 0, and `why`, when it cannot be
 * made, for the caller to draw the label as menu_label.c does. */
#include <stddef.h>

int MenuLetters_Make(const char *text, int selected, int stand_in, int factor, char *path, size_t size, int *width,
                     int *height, char *why, size_t why_size);
/* The entries had not been drawn yet when a label was asked for (their
 * sprites are worked out as they are first drawn): the title's pictures
 * want making again once they have (TitleMenu_Draw). A few times at most. */
int MenuLetters_Waiting(void);
#endif
