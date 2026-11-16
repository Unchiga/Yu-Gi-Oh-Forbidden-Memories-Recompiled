#define _POSIX_C_SOURCE 200809L
#include "scratch.h"
#include "pc/guest/state_io.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

/* Windows tmpfile() uses the drive root, which CI cannot write. Use the
 * shared Unicode-safe scratch directory and its automatic cleanup. */
static FILE *state_file(void)
{
    static char directory[SCRATCH_MAX];
    static unsigned count;
    char path[SCRATCH_MAX + 32];
    if (!*directory && !scratch_dir(directory, sizeof(directory), "memories-state")) return NULL;
    snprintf(path, sizeof(path), "%s/chunk%u.bin", directory, count++);
    return fopen(path, "w+b");
}

int main(void)
{
    uint32_t number = 0x12345678, length;
    uint8_t flags[2] = {7, 9}, image[42], zero[16] = {0};
    MemoriesStateField fields[] = {{&number, sizeof(number)}, {flags, sizeof(flags)}, {NULL, 0}};
    MemoriesState save = {0, state_file(), NULL, 0};
    MemoriesState load = {1, NULL, image, sizeof(image)};
    size_t size;
    assert(save.file);
    Memories_StateWrite(&save, zero, sizeof(zero));
    assert(!Memories_StateChunk(&save, "fixture", fields, 3));
    assert(!ferror(save.file) && ftell(save.file) == sizeof(image));
    rewind(save.file);
    assert(fread(image, 1, sizeof(image), save.file) == sizeof(image));
    fclose(save.file);
    number = 0;
    memset(flags, 0, sizeof(flags));
    assert(Memories_StateLoading(&load));
    assert(Memories_StateChunk(&load, "fixture", fields, 3));
    assert(number == 0x12345678 && flags[0] == 7 && flags[1] == 9);
    assert(Memories_StateFindChunk(&load, "fixture", &size) == image + 36 && size == 6);
    number = 42;
    load.image_size--;
    assert(!Memories_StateChunk(&load, "fixture", fields, 3) && number == 42);
    load.image_size++;
    length = 5;
    memcpy(image + 32, &length, 4);
    assert(!Memories_StateChunk(&load, "fixture", fields, 3) && number == 42);
    length = UINT32_MAX;
    memcpy(image + 32, &length, 4);
    assert(!Memories_StateFindChunk(&load, "fixture", &size));
    load.image_size = 15;
    assert(!Memories_StateFindChunk(&load, "fixture", &size));
    assert(!Memories_StateLoading(NULL));
    assert(!Memories_StateChunk(NULL, "fixture", fields, 3) && errno == EINVAL);
    fields[0].size = SIZE_MAX;
    assert(!Memories_StateChunk(&load, "fixture", fields, 3) && errno == EINVAL);
    {
        uint8_t sealed[68];
        MemoriesState writer = {0, state_file(), NULL, 0};
        MemoriesState reader = {1, NULL, sealed, sizeof(sealed)};
        MemoriesStateField field = {&number, sizeof(number)};
        assert(writer.file);
        Memories_StateWrite(&writer, zero, sizeof(zero));
        Memories_StateChunk(&writer, "payload", &field, 1);
        assert(!Memories_StateSeal(&writer));
        assert(ftell(writer.file) == sizeof(sealed));
        rewind(writer.file);
        assert(fread(sealed, 1, sizeof(sealed), writer.file) == sizeof(sealed));
        fclose(writer.file);
        assert(Memories_StateIntegrity(&reader));
        sealed[36] ^= 1;
        assert(!Memories_StateIntegrity(&reader));
        sealed[36] ^= 1;
        reader.image_size--;
        assert(!Memories_StateIntegrity(&reader));
    }
    {
        const char *tags[] = {"", "12345678901234", "123456789012345",
                              "1234567890123456", "12345678901234567890"};
        for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
            uint8_t tagged[36];
            MemoriesState writer = {0, state_file(), NULL, 0};
            MemoriesState reader = {1, NULL, tagged, sizeof(tagged)};
            size_t tag_length = strlen(tags[i]);
            size_t stored = tag_length < 15 ? tag_length : 15;
            assert(writer.file);
            Memories_StateWrite(&writer, zero, sizeof(zero));
            Memories_StateChunk(&writer, tags[i], NULL, 0);
            rewind(writer.file);
            assert(fread(tagged, 1, sizeof(tagged), writer.file) == sizeof(tagged));
            fclose(writer.file);
            assert(!memcmp(tagged + 16, tags[i], stored));
            for (size_t at = stored; at < 16; ++at) assert(tagged[16 + at] == 0);
            assert(Memories_StateFindChunk(&reader, tags[i], &size) == tagged + 36);
            assert(size == 0);
        }
    }
    {
        /* A chunk this build has no part for (a state saved by a game with
         * the MIPS interpreter: "mips-stack", or the translated game's
         * "arm64-mips") is passed over: the chunks after it load. */
        uint8_t old[16 + 20 + 8 + 20 + 4], stack[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint32_t after = 0xCAFEF00Du;
        MemoriesStateField unknown = {stack, sizeof(stack)}, known = {&after, sizeof(after)};
        MemoriesState writer = {0, state_file(), NULL, 0};
        MemoriesState reader = {1, NULL, old, sizeof(old)};
        assert(writer.file);
        Memories_StateWrite(&writer, zero, sizeof(zero));
        Memories_StateChunk(&writer, "mips-stack", &unknown, 1);
        Memories_StateChunk(&writer, "after", &known, 1);
        rewind(writer.file);
        assert(fread(old, 1, sizeof(old), writer.file) == sizeof(old));
        fclose(writer.file);
        after = 0;
        assert(Memories_StateChunk(&reader, "after", &known, 1) && after == 0xCAFEF00Du);
    }
    puts("State chunks: round trip, field ordering, truncation, layout mismatch, overflow and unknown chunks passed");
    return 0;
}
