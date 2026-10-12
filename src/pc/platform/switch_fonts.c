/* The Switch's shared system font as a file, for the fontconfig stand-in
 * (src/pc/compat/switch/fontconfig/fontconfig.h): FreeType opens fonts by
 * path in the port, and the console keeps its fonts in shared memory, so
 * the standard one (Japanese and Latin) is copied to the SD card once. */
#ifdef __SWITCH__
#include <fontconfig/fontconfig.h>
#include "paths.h"
#include "pc/compat/fs.h"
#include <switch.h>
#include <stdio.h>
#include <sys/stat.h>

#define FONT_FOLDER PATHS_SWITCH_DIR "/fonts"
#define FONT_PATH FONT_FOLDER "/standard.ttf"

static int copied(void)
{
    PlFontData font;
    FILE *file;
    size_t written;
    if (R_FAILED(plInitialize(PlServiceType_User))) return 0;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) || !font.address || !font.size) {
        plExit();
        return 0;
    }
    Paths_MakeDirs(FONT_FOLDER);
    if (!(file = fopen(FONT_PATH ".part", "wb"))) {
        plExit();
        return 0;
    }
    written = fwrite(font.address, 1, font.size, file);
    plExit();
    if (fclose(file) || written != font.size || rename(FONT_PATH ".part", FONT_PATH)) {
        remove(FONT_PATH ".part");
        return 0;
    }
    return 1;
}

const char *SwitchFonts_Path(void)
{
    static int state; /* 0 not looked for, 1 there, -1 cannot be had */
    struct stat info;
    if (!state) state = (!stat(FONT_PATH, &info) && info.st_size > 0) || copied() ? 1 : -1;
    return state > 0 ? FONT_PATH : NULL;
}
#endif
