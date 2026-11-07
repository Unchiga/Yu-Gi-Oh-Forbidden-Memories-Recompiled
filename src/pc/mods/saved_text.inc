/* A copy of `text` that a save state carries. Not strdup: the translated build
 * renames malloc to GuestRuntime_malloc but not strdup, whose block would be libc's,
 * in no saved region, while the block that points at it is saved: a state loaded in
 * another session would leave that pointer stale. For anything kept in a block of
 * src/pc's own heap (tests/pc/saved_text_test.c). */
static char *saved_text(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}
