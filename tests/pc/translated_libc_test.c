#include "pc/guest/translated_runtime.h"
#include "pc/guest/translated_libc.h"
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define G(offset) ((void *)(uintptr_t)(0x80000000u + (offset)))
static int compare(const void *a, const void *b)
{
    assert((uintptr_t)a <= UINT32_MAX && (uintptr_t)b <= UINT32_MAX);
    int x = *(int *)GuestRuntime_ResolveData((void *)a, sizeof(int));
    int y = *(int *)GuestRuntime_ResolveData((void *)b, sizeof(int));
    return (x > y) - (x < y);
}
static void expect_abort(int which)
{
    int status; pid_t child = fork(); assert(child >= 0);
    if (!child) {
        if (which == 7) GuestRuntime_fwrite(G(0x100), SIZE_MAX, 2, stdout);
        if (which == 8) GuestRuntime_fgets(G(MEMORIES_RAM_SIZE - 1), 8, stdin);
        if (which == 9) GuestRuntime_fclose(G(0x100));
        if (which == 10) GuestRuntime_fread(G(0x100), SIZE_MAX, 2, stdin);
        if (which == 11) GuestRuntime_fread(G(MEMORIES_RAM_SIZE - 1), 1, 8, stdin);
        if (which == 12) GuestRuntime_ftell(G(0x100));
        if (which == 13) GuestRuntime_bsearch(G(0x5a0), G(0x300), SIZE_MAX, 4, compare);
        if (which == 0) GuestRuntime_strlen((char *)(uintptr_t)0xd0000000u);
        if (which == 1) GuestRuntime_memcpy(G(MEMORIES_RAM_SIZE - 1), "xx", 2);
        if (which == 2) GuestRuntime_sprintf(G(0x200), "%ls", G(0x100));
        if (which == 4) GuestRuntime___memcpy_chk(G(0x100), "abc", 3, 2);
        if (which == 5) GuestRuntime___sprintf_chk(G(0x100), 0, 2, "%s", "abc");
        if (which == 6) GuestRuntime___snprintf_chk(G(0x100), 8, 0, 4, "%s", "abc");
        if (which == 3) GuestRuntime_qsort(G(0x300), SIZE_MAX, 4, compare);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    char unterminated[3] = {'a','b','c'};
    assert(memory && GuestRuntime_Bind(memory) == 0);
    assert(GuestRuntime_RegisterData(unterminated, 3, 0xd0000000u) == 0);
    assert(GuestRuntime_strncmp((char *)(uintptr_t)0xd0000000u, "abc", 3) == 0);
    assert(GuestRuntime_strncpy(G(0x200), (char *)(uintptr_t)0xd0000000u, 3) == G(0x200));
    assert(!memcmp(memory->ram + 0x200, "abc", 3));
    assert(GuestRuntime_strcpy(G(0x100), "guest") == G(0x100));
    assert(GuestRuntime_strlen(G(0x100)) == 5);
    assert(GuestRuntime_strcat(G(0x100), " text") == G(0x100));
    assert(GuestRuntime_strcmp(G(0x100), "guest text") == 0);
    assert(GuestRuntime_sprintf(G(0x200), "%s %04x %.*s %% %d", G(0x100), 42u, 2, G(0x100), -7) == 23);
    assert(!strcmp((char *)memory->ram + 0x200, "guest text 002a gu % -7"));
    assert(GuestRuntime_memmove(G(0x102), G(0x100), 5) == G(0x102));
    assert(!memcmp(memory->ram + 0x102, "guest", 5));
    GuestRuntime_bzero(G(0x120), 8);
    GuestRuntime_memset(G(0x121), 0xab, 3);
    assert(memory->ram[0x120] == 0 && memory->ram[0x121] == 0xab && memory->ram[0x124] == 0);
    assert(GuestRuntime_memcmp(G(0x121), "\xab\xab\xab", 3) == 0);
    GuestRuntime_strcpy(G(0x400), "fortify works");
    assert(GuestRuntime_strchr(G(0x400), 'w') == G(0x408));
    assert(GuestRuntime_strchr(G(0x400), 0) == G(0x40d));
    assert(GuestRuntime_strrchr(G(0x400), 'r') == G(0x40a));
    assert(GuestRuntime_strcspn(G(0x400), " ") == 7);
    assert(GuestRuntime_strspn(G(0x400), "fortify") == 7);
    assert(GuestRuntime_strstr(G(0x400), "works") == G(0x408));
    assert(GuestRuntime_strnlen((char *)(uintptr_t)0xd0000000u, 3) == 3);
    assert(GuestRuntime_memchr(G(0x400), 'w', 13) == G(0x408));
    assert(GuestRuntime_memchr(G(0x400), 'x', 13) == NULL);
    assert(GuestRuntime_memchr(NULL, 'x', 0) == NULL);
    char native_memory[] = "native";
    assert(GuestRuntime_memchr(native_memory, 't', sizeof(native_memory)) == native_memory + 2);
    int values[] = {7, -2, 4, 0};
    GuestRuntime_memcpy(G(0x300), values, sizeof(values));
    assert(GuestRuntime_RegisterFunction(0x80010000u, (void (*)(void))compare) == 0);
    GuestRuntime_qsort(G(0x300), 4, sizeof(int), (int (*)(const void *, const void *))(uintptr_t)0x80010000u);
    int *sorted = (int *)(memory->ram + 0x300);
    assert(sorted[0] == -2 && sorted[1] == 0 && sorted[2] == 4 && sorted[3] == 7);
    int key = 4, absent = 5;
    memcpy(memory->ram + 0x5a0, &key, sizeof(key));
    memcpy(memory->ram + 0x5b0, &absent, sizeof(absent));
    typedef int (*Compare)(const void *, const void *);
    assert(GuestRuntime_bsearch(G(0x5a0), G(0x300), 4, sizeof(int), (Compare)(uintptr_t)0x80010000u) == G(0x308));
    assert(GuestRuntime_bsearch(G(0x5b0), G(0x300), 4, sizeof(int), (Compare)(uintptr_t)0x80010000u) == NULL);
    assert(GuestRuntime_bsearch(G(0x5a0), G(0x300), 0, sizeof(int), (Compare)(uintptr_t)0x80010000u) == NULL);
    assert(GuestRuntime___memcpy_chk(G(0x400), "fortify", 8, 8) == G(0x400));
    assert(GuestRuntime___strcat_chk(G(0x400), " works", 32) == G(0x400));
    assert(GuestRuntime_snprintf(G(0x450), 5, "%s", G(0x400)) == 13);
    assert(!strcmp((char *)memory->ram + 0x450, "fort"));
    assert(GuestRuntime___sprintf_chk(G(0x480), 0, 32, "%s %d", G(0x400), 7) == 15);
    assert(!strcmp((char *)memory->ram + 0x480, "fortify works 7"));
    assert(GuestRuntime_snprintf(NULL, 0, "%s", G(0x400)) == 13);
    GuestRuntime_strcpy(G(0x500), " -123tail");
    char *end = NULL;
    assert(GuestRuntime_strtol(G(0x500), &end, 10) == -123 && end == G(0x505));
    assert(GuestRuntime_strtol(G(0x500), G(0x520), 10) == -123);
    memcpy(&end, memory->ram + 0x520, sizeof(end));
    assert(end == G(0x505));
    assert(GuestRuntime_strtol("42", NULL, 10) == 42);
    assert(GuestRuntime_atoi(G(0x500)) == -123 && GuestRuntime_atoi("77x") == 77);
    GuestRuntime_strcpy(G(0x5c0), "ff zz");
    assert(GuestRuntime_strtoul(G(0x5c0), &end, 16) == 255 && end == G(0x5c2));
    assert(GuestRuntime_strtoul(G(0x5c0), G(0x520), 16) == 255);
    memcpy(&end, memory->ram + 0x520, sizeof(end));
    assert(end == G(0x5c2));
    GuestRuntime_strcpy(G(0x5d0), "2.5e1rest");
    assert(GuestRuntime_strtod(G(0x5d0), &end) == 25.0 && end == G(0x5d5));
    assert(GuestRuntime_strtod("1.5", NULL) == 1.5);
    GuestRuntime_strcpy(G(0x5e0), "ab");
    assert(GuestRuntime_strncat(G(0x5e0), "cdefg", 3) == G(0x5e0));
    assert(!strcmp((char *)memory->ram + 0x5e0, "abcde"));
    assert(GuestRuntime_strncat(G(0x5e0), "!", 9) == G(0x5e0));
    assert(!strcmp((char *)memory->ram + 0x5e0, "abcde!"));
    assert(GuestRuntime_strncat(G(0x5e0), "x", 0) == G(0x5e0) && !strcmp((char *)memory->ram + 0x5e0, "abcde!"));
    assert(GuestRuntime_strtol("invalid", &end, 10) == 0 && !strcmp(end, "invalid"));
    GuestRuntime_strcpy(G(0x5f0), "zz");
    assert(GuestRuntime_strtoul(G(0x5f0), &end, 10) == 0 && end == G(0x5f0));
    assert(GuestRuntime_strtod(G(0x5f0), &end) == 0.0 && end == G(0x5f0));
    /* A native string is parsed where it is, whatever follows it (a language
     * file's lines are slices of one buffer with no NUL between them), and the
     * end it leaves is native too, in a native or a guest pointer. */
    static const char slice[] = "[a1] x [b2] y";
    assert(GuestRuntime_strtoul(slice + 1, &end, 16) == 0xa1 && end == slice + 3);
    assert(GuestRuntime_strtoul(slice + 8, G(0x520), 16) == 0xb2);
    memcpy(&end, memory->ram + 0x520, sizeof(end));
    assert(end == slice + 10);
    assert(GuestRuntime_strtod(slice + 1, &end) == 0.0 && end == slice + 1);
    assert(GuestRuntime_strtol("12 34", &end, 10) == 12 && !strcmp(end, " 34"));
    assert(GuestRuntime_atoi("99999999999") == INT_MAX && GuestRuntime_atoi("-99999999999") == INT_MIN);
    assert(GuestRuntime_atoi(G(0x5f0)) == 0);
    GuestRuntime_strcpy(G(0x5e8), "q");
    GuestRuntime_strcpy(G(0x5f8), "cdefg");
    assert(GuestRuntime_strncat(G(0x5e8), G(0x5f8), 3) == G(0x5e8));
    assert(!strcmp((char *)memory->ram + 0x5e8, "qcde"));
    assert(GuestRuntime_strlen(G(0x5f8)) == 5 && GuestRuntime_strnlen(G(0x5f8), 3) == 3);
    assert(GuestRuntime_strnlen(G(0x5f8), 0) == 0 && GuestRuntime_strlen(slice) == sizeof(slice) - 1);
    char native_text[8] = "ab";
    assert(GuestRuntime_strncat(native_text, "cdefg", 2) == native_text && !strcmp(native_text, "abcd"));
    assert(GuestRuntime_isdigit('0') && GuestRuntime_isdigit('9') && !GuestRuntime_isdigit('A') && !GuestRuntime_isdigit(EOF));
    assert(GuestRuntime_isalnum('A') && !GuestRuntime_isalnum('!'));
    assert(GuestRuntime_isspace(' ') && GuestRuntime_tolower('A') == 'a');
    FILE *file = tmpfile(); assert(file);
    GuestRuntime_strcpy(G(0x540), "123\n");
    assert(GuestRuntime_fwrite(G(0x540), 1, 4, file) == 4);
    assert(GuestRuntime_fseek(file, 0, SEEK_SET) == 0);
    assert(GuestRuntime_fgets(G(0x560), 16, file) == G(0x560));
    assert(!strcmp((char *)memory->ram + 0x560, "123\n"));
    assert(GuestRuntime_fgets(G(0x560), 16, file) == NULL);
    assert(GuestRuntime_fseek(file, 0, SEEK_SET) == 0);
    char native_line[16];
    assert(GuestRuntime_fgets(native_line, sizeof(native_line), file) == native_line);
    assert(!strcmp(native_line, "123\n"));
    assert(GuestRuntime_ftell(file) == 4);
    assert(GuestRuntime_fseek(file, 1, SEEK_SET) == 0 && GuestRuntime_ftell(file) == 1);
    memset(memory->ram + 0x580, 0, 8);
    assert(GuestRuntime_fread(G(0x580), 1, 8, file) == 3);   /* "23\n" is all that is left */
    assert(!memcmp(memory->ram + 0x580, "23\n", 4));
    assert(GuestRuntime_fseek(file, 0, SEEK_SET) == 0);
    assert(GuestRuntime_fread(G(0x590), 2, 2, file) == 2 && !memcmp(memory->ram + 0x590, "123\n", 4));
    assert(GuestRuntime_fread(G(0x590), 0, 5, file) == 0 && GuestRuntime_fread(G(0x590), 5, 0, file) == 0);
    assert(GuestRuntime_fseek(file, 0, SEEK_SET) == 0);
    char native_bytes[4];
    assert(GuestRuntime_fread(native_bytes, 1, 4, file) == 4 && !memcmp(native_bytes, "123\n", 4));
    assert(GuestRuntime_fclose(file) == 0);
    for (int i = 0; i < 14; i++) expect_abort(i);
    GuestRuntime_Reset(); free(memory);
    puts("Translated libc: guest strings, bounded reads, files, memory, formatting, callbacks and invalid spans passed");
}
