/* The part list of Model_QueueTintRequestForParts (src/game/func_80058838.c)
 * as the MODEL.MRG variant modules pass it: a MIPS routine run by the
 * interpreter (src/pc/guest/mips.c) stores eighteen parts and the -1 that
 * ends them on its stack, 24 argument words in all, and calls 0x80058838.
 * Every part must reach the mask Model_QueueTintRequest gets, not only the
 * ones that fit in the twelve words the interpreter forwards to a native
 * function. Then the same list from C, through the variadic entry. */
#include "pc/guest/image.h"
#include "pc/guest/mips.h"
#include "pc/debug/log.h"
#include "game/func_80058938.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* What the interpreter links against, kept out of the way. */
const MemoriesGuestFunction Memories_FunctionMap[] = {
    {0x80058838u, (void (*)(void))Model_QueueTintRequestForParts, 0, 0},
};
const unsigned Memories_FunctionMapCount = 1;
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
int Memories_ScratchpadRetailView = 1; /* image.c: the desktops' layout */
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
int Log_Enabled(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
void Crash_ReportFatal(const char *kind, const char *detail) { fprintf(stderr, "%s: %s\n", kind, detail); }

static struct {
    int calls;
    s32 slot, selection, duration;
    ModelTintColor start, end;
    u8 mask[8];
} got;

void Model_QueueTintRequest(s32 slot, s32 selection, ModelTintColor start, ModelTintColor end, s32 duration,
                            const u8 *part_mask)
{
    got.calls++;
    got.slot = slot;
    got.selection = selection;
    got.start = start;
    got.end = end;
    got.duration = duration;
    memcpy(got.mask, part_mask, sizeof(got.mask));
}

enum { V0 = 2, A0 = 4, A1, A2, A3, T9 = 25, SP = 29, RA = 31 };
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((imm) & 0xffffu))
#define ADDIU(rt, rs, imm) I(9, rs, rt, imm)
#define SW(rt, offset, base) I(0x2b, base, rt, offset)
#define LW(rt, offset, base) I(0x23, base, rt, offset)
#define LUI(rt, imm) I(0xf, 0, rt, imm)
#define ORI(rt, rs, imm) I(0xd, rs, rt, imm)
#define JALR(rs) ((uint32_t)(rs) << 21 | (uint32_t)RA << 11 | 9)
#define JR(rs) ((uint32_t)(rs) << 21 | 8)

/* Parts 1 to 18, as the shared variant module 0x2F0 asks for them. */
static const u8 expected[8] = {0xfe, 0xff, 0x07, 0, 0, 0, 0, 0};

static void check(const char *how, s32 selection)
{
    int i;
    if (memcmp(got.mask, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "%s: mask", how);
        for (i = 0; i < 8; i++) fprintf(stderr, " %02x", got.mask[i]);
        fprintf(stderr, ", expected fe ff 07 00 00 00 00 00\n");
    }
    assert(memcmp(got.mask, expected, sizeof(expected)) == 0);
    assert(got.calls == 1 && got.slot == 1 && got.selection == selection && got.duration == 6);
    assert(got.start.b0 == 0x60 && got.start.b1 == 0x60 && got.start.b2 == 0x60 && got.start.b3 == 2);
    assert(got.end.b0 == 0x10 && got.end.b1 == 0 && got.end.b2 == 0);
}

int main(void)
{
    static uint32_t code[64];
    ModelTintColor start = {0x60, 0x60, 0x60, 0}, end = {0x10, 0, 0, 0};
    uint32_t result = 0;
    int n = 0, part;

    code[n++] = ADDIU(SP, SP, -104);
    code[n++] = SW(RA, 100, SP);
    code[n++] = ADDIU(V0, 0, 6);
    code[n++] = SW(V0, 16, SP);          /* the duration, the fifth argument */
    for (part = 1; part <= 18; part++) {
        code[n++] = ADDIU(V0, 0, part);
        code[n++] = SW(V0, 16 + part * 4, SP);
    }
    code[n++] = ADDIU(V0, 0, -1);
    code[n++] = SW(V0, 92, SP);          /* the list's end: the 24th word */
    code[n++] = ADDIU(A0, 0, 1);
    code[n++] = ADDIU(A1, 0, 0x82);      /* part 2, selection 0x80 */
    code[n++] = LUI(A2, 0x0060);
    code[n++] = ORI(A2, A2, 0x6060);
    code[n++] = ORI(A3, 0, 0x0010);
    code[n++] = LUI(T9, 0x8005);
    code[n++] = ORI(T9, T9, 0x8838);
    code[n++] = JALR(T9);
    code[n++] = 0;
    code[n++] = LW(RA, 100, SP);
    code[n++] = JR(RA);
    code[n++] = ADDIU(SP, SP, 104);
    assert(n <= (int)(sizeof(code) / sizeof(code[0])));

    assert(Memories_MipsTry((uint32_t)(uintptr_t)code, NULL, 0, &result) == 0);
    check("interpreted", 0x80);

    memset(&got, 0, sizeof(got));
    Model_QueueTintRequestForParts(1, 2, start, end, 6, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                   18, 64, 200, -1);   /* 64 and up: outside the mask, left out */
    check("variadic", 0);
    puts("mips varargs: ok");
    return 0;
}
