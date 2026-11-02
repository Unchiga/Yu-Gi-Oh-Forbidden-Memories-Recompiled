/* The stack the interpreter (src/pc/guest/mips.c) runs MIPS code on, as save
 * states carry it (Memories_MipsStack, state_subsystems.c "mips-stack").
 * Retail variant modules read locals they never wrote, which hold what an
 * earlier routine left at that depth: one routine stores a word in its frame,
 * another of the same frame size reads it there without writing it. A state
 * restores the stack's bytes, so after a copy of them is put back the read
 * gives the saved word again whatever ran in between; a state from before
 * the stack was carried clears it, and the read gives 0. */
#include "pc/guest/image.h"
#include "pc/guest/mips.h"
#include "pc/debug/log.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the interpreter links against, kept out of the way. */
const MemoriesGuestFunction Memories_FunctionMap[1];
const unsigned Memories_FunctionMapCount = 0;
const MemoriesModule Memories_Modules[1];
const unsigned Memories_ModuleCount = 0;
int Memories_ModuleIsResident(unsigned bank, unsigned identifier) { (void)bank; (void)identifier; return 1; }
void Memories_GteCommand(uint32_t instruction) { (void)instruction; }
uint32_t Memories_GteReadData(unsigned index) { (void)index; return 0; }
uint32_t Memories_GteReadControl(unsigned index) { (void)index; return 0; }
void Memories_GteWriteData(unsigned index, uint32_t value) { (void)index; (void)value; }
void Memories_GteWriteControl(unsigned index, uint32_t value) { (void)index; (void)value; }
void Memories_GteLoad(unsigned index, const void *address) { (void)index; (void)address; }
void Memories_GteStore(unsigned index, void *address) { (void)index; (void)address; }
int rsin(int angle) { return angle; }
int rcos(int angle) { return angle; }
int Psx_csin(int angle) { return angle; }
int Psx_ccos(int angle) { return angle; }
int Memories_Rand(void) { return 0; }
void Model_QueueTintRequestForPartList(int32_t slot, int32_t part, uint32_t start, uint32_t end, int32_t duration,
                                       const int32_t *parts)
{
    (void)slot; (void)part; (void)start; (void)end; (void)duration; (void)parts;
}
int Memories_ScratchpadRetailView = 1; /* image.c: the desktops' layout */
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
int Log_Enabled(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
void Crash_ReportFatal(const char *kind, const char *detail) { fprintf(stderr, "%s: %s\n", kind, detail); }

enum { V0 = 2, A0 = 4, SP = 29, RA = 31 };
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((imm) & 0xffffu))
#define ADDIU(rt, rs, imm) I(9, rs, rt, imm)
#define SW(rt, offset, base) I(0x2b, base, rt, offset)
#define LW(rt, offset, base) I(0x23, base, rt, offset)
#define JR(rs) ((uint32_t)(rs) << 21 | 8)

/* store(a0): a local at sp+8 of a 32-byte frame. load(): the same local, never written. */
static uint32_t store[] = {ADDIU(SP, SP, -32), SW(A0, 8, SP), JR(RA), ADDIU(SP, SP, 32)};
static uint32_t load[] = {ADDIU(SP, SP, -32), LW(V0, 8, SP), JR(RA), ADDIU(SP, SP, 32)};

static uint32_t run(uint32_t *code, uint32_t argument)
{
    uint32_t result = 0xffffffffu;
    assert(Memories_MipsTry((uint32_t)(uintptr_t)code, &argument, 1, &result) == 0);
    return result;
}

int main(void)
{
    size_t size = 0;
    uint8_t *stack, *saved;

    run(store, 0x12345678u);
    assert(run(load, 0) == 0x12345678u);   /* what the earlier routine left there */

    stack = Memories_MipsStack(&size);
    assert(stack && size >= 0x1000);
    saved = malloc(size);
    assert(saved);
    memcpy(saved, stack, size);            /* a state saved here */
    run(store, 0xdeadbeefu);
    assert(run(load, 0) == 0xdeadbeefu);
    memcpy(stack, saved, size);            /* ... and loaded */
    assert(run(load, 0) == 0x12345678u);

    memset(stack, 0, size);                /* a state from before the stack was in them */
    assert(run(load, 0) == 0);
    free(saved);
    puts("mips stack: ok");
    return 0;
}
