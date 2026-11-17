#!/usr/bin/env python3
"""Structured compiler regressions, independent of ROM and generated game IR."""
import re
import unittest
from pathlib import Path
import subprocess
import tempfile
from llvm_guest import ROOT, TranslationError, inspect, normalize, process, translate, toolchain
from macos_deps import sdk_path
from direct_overlay_bridges import validate_declaration
from native_call_marshalling import emit_native_calls


class GuestExecutionTests(unittest.TestCase):
    def test_memory_conversions_copies_and_callback_execute(self):
        self.exercise_memory()

    def test_mod_memory_copies_use_guest_wrappers(self):
        self.exercise_memory(mod=True)

    def exercise_memory(self, mod=False):
        # Execute the transformed IR against checked host storage. This
        # catches valid-looking IR that still dereferences a PS1 token.
        source = '''
target datalayout = "e-p:64:64-p271:32:32"
@slot = external global i32
declare void @llvm.memcpy.p271.p271.i32(ptr addrspace(271), ptr addrspace(271), i32, i1)
declare void @llvm.memmove.p271.p271.i32(ptr addrspace(271), ptr addrspace(271), i32, i1)
declare void @llvm.memset.p271.i32(ptr addrspace(271), i8, i32, i1)
define i32 @exercise(ptr %native, ptr addrspace(271) %callback) {
  %value = load i32, ptr @slot
  %encoded = ptrtoint ptr %native to i32
  %guest = inttoptr i32 %encoded to ptr addrspace(271)
  %generic = addrspacecast ptr addrspace(271) %guest to ptr
  %converted = addrspacecast ptr %generic to ptr addrspace(271)
  store i32 %value, ptr addrspace(271) %converted
  %dest = getelementptr i8, ptr addrspace(271) %guest, i32 4
  call void @llvm.memcpy.p271.p271.i32(ptr addrspace(271) %dest, ptr addrspace(271) %guest, i32 4, i1 false)
  %overlap = getelementptr i8, ptr addrspace(271) %guest, i32 5
  call void @llvm.memmove.p271.p271.i32(ptr addrspace(271) %overlap, ptr addrspace(271) %dest, i32 4, i1 false)
  %tail = getelementptr i8, ptr addrspace(271) %guest, i32 9
  call void @llvm.memset.p271.i32(ptr addrspace(271) %tail, i8 90, i32 3, i1 false)
  %counter = getelementptr i8, ptr addrspace(271) %guest, i32 12
  %old = atomicrmw add ptr addrspace(271) %counter, i32 3 seq_cst
  %changed = cmpxchg ptr addrspace(271) %counter, i32 3, i32 9 seq_cst seq_cst
  %result = call addrspace(271) i32 %callback(i32 %value)
  ret i32 %result
}
'''
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static _Alignas(4) unsigned char memory[32];
static unsigned resolutions, encodings, callbacks;
void *GuestRuntime_ActiveMemory;
void *GuestRuntime_ResolveData(void *pointer, uint64_t size) {
    uintptr_t address = (uintptr_t)pointer;
    assert(address >= 0x80010000u && address <= 0x80010020u);
    uintptr_t offset = address - 0x80010000u;
    assert(size <= sizeof(memory) - offset);
    ++resolutions;
    return memory + offset;
}
uint32_t GuestRuntime_EncodePointer(void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    ++encodings;
    if (address >= 0x80010000u && address <= 0x80010020u) return address;
    assert(address >= (uintptr_t)memory && address <= (uintptr_t)(memory + sizeof(memory)));
    return 0x80010000u + (uint32_t)(address - (uintptr_t)memory);
}
void *GuestRuntime_memcpy(void *d, const void *s, uint64_t n) {
    memcpy(GuestRuntime_ResolveData(d, n), GuestRuntime_ResolveData((void *)s, n), n);
    return d;
}
void *GuestRuntime_memmove(void *d, const void *s, uint64_t n) {
    memmove(GuestRuntime_ResolveData(d, n), GuestRuntime_ResolveData((void *)s, n), n);
    return d;
}
void *GuestRuntime_memset(void *d, int c, uint64_t n) {
    memset(GuestRuntime_ResolveData(d, n), c, n);
    return d;
}
static int callback(int value) { ++callbacks; return value + 7; }
void *GuestRuntime_ResolveFunction(void *pointer) {
    assert((uintptr_t)pointer == 0x80020000u);
    return (void *)(uintptr_t)&callback;
}
extern int exercise(void *, void *);
int main(void) {
    uint32_t value = 0x12345678u;
    memcpy(memory, &value, 4);
    int result = exercise(memory + 16, (void *)(uintptr_t)0x80020000u);
    assert(result == (int)value + 7);
    const unsigned char expected[] = {0x78,0x56,0x34,0x12,0x78,0x78,0x56,0x34,0x12,0x5a,0x5a,0x5a};
    assert(!memcmp(memory + 16, expected, sizeof(expected)));
    uint32_t counter;
    memcpy(&counter, memory + 28, 4);
    assert(counter == 9);
    assert(callbacks == 1 && encodings == 2 && resolutions == 9);
    return 0;
}
'''
        translated = process('translate', source, pins={'slot': 0x80010000}, mod_unit=mod)
        if mod:
            for name in ('memcpy', 'memmove', 'memset'):
                self.assertIn('call ptr @GuestRuntime_' + name, translated)
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as folder:
            folder = Path(folder)
            (folder / 'fixture.ll').write_text(translated)
            (folder / 'harness.c').write_text(harness)
            binary = folder / 'test'
            subprocess.run([str(toolchain() / 'bin/clang'), '-isysroot', str(sdk_path()),
                            '-O2', str(folder / 'fixture.ll'), str(folder / 'harness.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_native_storage_bypasses_lookup_and_guest_tokens_remain_checked(self):
        source = """
target datalayout = "e-p:64:64-p271:32:32"
@words = global [2 x i32] [i32 11, i32 13]
@slot = external global i32
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
define i32 @exercise(ptr %unknown) {
  %local = alloca [2 x i32], align 4
  call void @llvm.memcpy.p0.p0.i64(ptr %local, ptr @words, i64 8, i1 false)
  %old = atomicrmw add ptr %local, i32 1 seq_cst
  %head = load i32, ptr %local
  %tailptr = getelementptr [2 x i32], ptr %local, i32 0, i32 1
  %tail = load i32, ptr %tailptr
  %guest = load i32, ptr @slot
  %argument = load i32, ptr %unknown
  %a = add i32 %head, %tail
  %b = add i32 %a, %guest
  %c = add i32 %b, %argument
  ret i32 %c
}
"""
        harness = r"""
#include <assert.h>
#include <stdint.h>
static unsigned resolutions;
static int guest = 7;
void *GuestRuntime_ActiveMemory;
void *GuestRuntime_ResolveData(void *pointer, uint64_t size) {
    ++resolutions;
    assert(size == 4);
    if ((uintptr_t)pointer == 0x80010000u) return &guest;
    return pointer;
}
extern int exercise(void *);
int main(void) {
    int unknown = 9;
    assert(exercise(&unknown) == 41);
    assert(resolutions == 1);
}
"""
        translated = translate(source, {'slot': 0x80010000})
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as folder:
            folder = Path(folder)
            (folder / 'fixture.ll').write_text(translated)
            (folder / 'harness.c').write_text(harness)
            binary = folder / 'test'
            subprocess.run([str(toolchain() / 'bin/clang'), '-isysroot', str(sdk_path()),
                            '-O2', str(folder / 'fixture.ll'), str(folder / 'harness.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_inline_ram_aliases_native_arguments_and_checked_fallback(self):
        source = """
define i32 @read_word(ptr %p) {
  %v = load i32, ptr %p, align 1
  ret i32 %v
}
"""
        harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
void *GuestRuntime_ActiveMemory;
static unsigned resolutions;
static int fallback = 91;
void *GuestRuntime_ResolveData(void *p, uint64_t size) {
    (void)p;
    assert(size == 4);
    ++resolutions;
    return &fallback;
}
extern int read_word(void *);
int main(void) {
    unsigned char *ram = calloc(1, 0x200400);
    assert(ram);
    GuestRuntime_ActiveMemory = ram;
    int value = 37;
    memcpy(ram + 0x100, &value, 4);
    assert(read_word(ram + 0x100) == 37);
    assert(read_word((void *)0x100) == 37);
    assert(read_word((void *)0x80000100) == 37);
    assert(read_word((void *)0xa0000100) == 37);
    assert(read_word((void *)(intptr_t)(int32_t)0x80000100u) == 37);
    assert(read_word((void *)(intptr_t)(int32_t)0xa0000100u) == 37);
    assert(resolutions == 0);
    /* Final-byte overflow, RAM end, invalid segment, scratchpad and
       external allocations must still reach the checked runtime. */
    uintptr_t slow[] = {0x801ffffe, 0x80200000, 0x20000100,
                        0xc0000100, 0x1f800000, 0xd0000100};
    for (unsigned i = 0; i < sizeof(slow) / sizeof(slow[0]); ++i)
        assert(read_word((void *)slow[i]) == 91);
    assert(resolutions == 6);
    GuestRuntime_ActiveMemory = NULL;
    assert(read_word((void *)0x80000100) == 91);
    assert(resolutions == 7);
    /* Native storage works even before binding. */
    assert(read_word(&value) == 37 && resolutions == 7);
    free(ram);
}
"""
        translated = translate(source)
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as folder:
            folder = Path(folder)
            (folder / 'fixture.ll').write_text(translated)
            (folder / 'harness.c').write_text(harness)
            binary = folder / 'test'
            subprocess.run([str(toolchain() / 'bin/clang'), '-isysroot', str(sdk_path()),
                            '-O2', str(folder / 'fixture.ll'), str(folder / 'harness.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_selection_preserves_internal_dependency_type(self):
        source = """
define internal double @dependency(i32 %a, double %b) {
  %value = sitofp i32 %a to double
  %result = fadd double %value, %b
  ret double %result
}
define double @selected(i32 %a, double %b) {
  %result = call double @dependency(i32 %a, double %b)
  ret double %result
}
define void @unused() { ret void }
"""
        selected = normalize(source, keep_functions={'selected'})
        signature = inspect(selected)['signatures']['dependency']
        self.assertFalse(signature['definition'])
        self.assertEqual(signature['result'], 'double')
        self.assertEqual(signature['parameters'], [
            {'kind': 'i32', 'signext': False}, {'kind': 'double', 'signext': False}])
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as folder:
            folder = Path(folder)
            (folder / 'selected.ll').write_text(selected)
            (folder / 'harness.c').write_text(
                'double dependency(int a, double b) { return a + b; }\n'
                'extern double selected(int, double);\n'
                'int main(void) { return selected(3, 0.5) != 3.5; }\n')
            binary = folder / 'test'
            subprocess.run([str(toolchain() / 'bin/clang'), '-isysroot', str(sdk_path()),
                            '-O2', str(folder / 'selected.ll'), str(folder / 'harness.c'),
                            '-o', str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=20)
        with self.assertRaisesRegex(TranslationError, 'missing selected function'):
            normalize(source, keep_functions={'missing'})

    def test_unsupported_address_space_rejected(self):
        with self.assertRaisesRegex(TranslationError, 'unsupported pointer address space'):
            translate('define i32 @bad(ptr addrspace(272) %p) { %v = load i32, ptr addrspace(272) %p ret i32 %v }')

    def test_machine_assembly_rejected(self):
        with self.assertRaisesRegex(TranslationError, 'machine assembly is unsupported'):
            translate('define void @bad() { call void asm sideeffect "nop", ""() ret void }')

    def test_scalable_memory_rejected(self):
        with self.assertRaisesRegex(TranslationError, 'scalable memory operation is unsupported'):
            translate('define <vscale x 4 x i32> @bad(ptr %p) { %v = load <vscale x 4 x i32>, ptr %p ret <vscale x 4 x i32> %v }')

    def test_unwind_call_rejected(self):
        with self.assertRaisesRegex(TranslationError, 'invoke/callbr are unsupported'):
            translate('''
declare void @callee()
declare i32 @personality(...)
define void @bad() personality ptr @personality {
  invoke void @callee() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %landing = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %landing
}
''')

    def test_invalid_pinned_address_rejected(self):
        with self.assertRaisesRegex(TranslationError, 'invalid guest address'):
            translate('@value = external global i32', {'value': 0x100000000})

    def test_native_pointer_static_guest_words_rejected(self):
        for initializer in (
            'i32 ptrtoint (ptr @callback to i32)',
            '[2 x i32] [i32 0, i32 ptrtoint (ptr @callback to i32)]',
            '{ i8, i32 } { i8 7, i32 sub (i32 ptrtoint (ptr @callback to i32), i32 16) }',
            'i16 ptrtoint (ptr @callback to i16)',
        ):
            with self.subTest(initializer=initializer):
                with self.assertRaisesRegex(TranslationError,
                        'native pointer in narrow static initializer: callback_word'):
                    translate('declare void @callback()\n@callback_word = global ' + initializer)

    def test_native_width_static_pointer_and_pinned_word_preserved(self):
        result = translate('''
@record = external global i32
@native_address = global i64 ptrtoint (ptr @callback to i64)
@guest_address = global i32 ptrtoint (ptr @record to i32)
declare void @callback()
''', {'record': 0x80018000})
        self.assertIn('@native_address = global i64 ptrtoint (ptr @callback to i64)', result)
        self.assertNotIn('@record', result)
        self.assertIn('2147581952', result)

    def test_literal_contents_and_function_local_ssa_preserved(self):
        result = translate(r'''
@literal = private constant [32 x i8] c"ptrtoint (ptr @callback to i32)\00"
define i64 @first(ptr %0) {
  %2 = ptrtoint ptr %0 to i64
  ret i64 %2
}
define i32 @second(i64 %0) {
  %2 = add i64 %0, 1
  %3 = trunc i64 %2 to i32
  ret i32 %3
}
''')
        self.assertIn(r'c"ptrtoint (ptr @callback to i32)\00"', result)
        self.assertIn('%3 = trunc i64 %2 to i32', result)


class UnloweredUnitTests(unittest.TestCase):
    # build_arm64.py UNLOWERED: the code as compiled, the registration as a
    # lowered unit's (so no guest token after it moves).
    SOURCE = r'''
source_filename = "src/pc/audio/unit.c"
@table = internal global [4 x i32] zeroinitializer
@shared = global i16 7
@.str = private unnamed_addr constant [3 x i8] c"hi\00"
@Mix.gain = internal global i32 256
define void @Mix(ptr %out) #0 {
  %gain = load i32, ptr @Mix.gain
  %slot = getelementptr [4 x i32], ptr @table, i64 0, i64 1
  %value = load i32, ptr %slot
  %sum = add i32 %gain, %value
  store i32 %sum, ptr %out
  ret void
}
attributes #0 = { noinline nounwind optnone }
'''

    @staticmethod
    def registrations(text):
        return re.findall(r'call void @GuestRuntime_RegisterGlobal\(.*\)', text)

    def test_registration_matches_lowered_and_code_is_untouched(self):
        lowered = translate(self.SOURCE, {}, 'GuestRuntime_RegisterUnit_test')
        unlowered = translate(self.SOURCE, {}, 'GuestRuntime_RegisterUnit_test', lower=False)
        self.assertEqual(len(self.registrations(lowered)), 4)
        self.assertEqual(self.registrations(unlowered), self.registrations(lowered))
        self.assertIn('GuestRuntime_ResolveData', lowered)
        self.assertNotIn('GuestRuntime_ResolveData', unlowered)
        self.assertIn('store i32 %sum, ptr %out', unlowered)
        self.assertNotIn('optnone', unlowered)
        self.assertNotIn('unnamed_addr constant', unlowered)

    def test_guest_pointers_and_pinned_symbols_refused(self):
        with self.assertRaisesRegex(TranslationError, 'unlowered unit with a guest pointer: Store'):
            translate('define void @Store(ptr addrspace(271) %to) {\n'
                      '  store i32 1, ptr addrspace(271) %to\n  ret void\n}',
                      {}, 'GuestRuntime_RegisterUnit_test', lower=False)
        with self.assertRaisesRegex(TranslationError, 'unlowered unit with a guest address: Read'):
            translate('@record = external global i32\ndefine i32 @Read() {\n'
                      '  %value = load i32, ptr @record\n  ret i32 %value\n}',
                      {'record': 0x80018000}, 'GuestRuntime_RegisterUnit_test', lower=False)

    def refused(self, source, message, pins=None, **options):
        with self.assertRaisesRegex(TranslationError, message):
            translate(source, pins or {}, 'GuestRuntime_RegisterUnit_test', lower=False, **options)

    def test_narrowed_pointers_refused(self):
        self.refused('define i32 @Word(ptr %p) {\n  %w = ptrtoint ptr %p to i32\n  ret i32 %w\n}',
                     'pointer narrowed to a guest word: Word')
        self.refused('define i32 @Cut(ptr %p) {\n  %w = ptrtoint ptr %p to i64\n'
                     '  %c = trunc i64 %w to i32\n  ret i32 %c\n}', 'pointer narrowed to a guest word: Cut')
        # (applyPins refuses this one first, lowered or not.)
        self.refused('@slot = global i32 0\n@word = global i32 ptrtoint (ptr @slot to i32)',
                     'narrow static initializer: word')
        self.refused('@slot = global i32 0\ndefine i32 @Const() {\n  ret i32 ptrtoint (ptr @slot to i32)\n}',
                     'pointer narrowed to a guest word: Const')
        # A full-width pointer as an integer (a hash, a comparison) is fine.
        translate('define i64 @Hash(ptr %p) {\n  %w = ptrtoint ptr %p to i64\n  ret i64 %w\n}',
                  {}, 'GuestRuntime_RegisterUnit_test', lower=False)

    def test_pointer_from_integer_refused(self):
        self.refused('define i32 @Peek(i64 %a) {\n  %p = inttoptr i64 %a to ptr\n'
                     '  %v = load i32, ptr %p\n  ret i32 %v\n}', 'pointer made from an integer: Peek')

    def test_indirect_call_refused_inline_asm_kept(self):
        self.refused('define void @Call(ptr %f) {\n  call void %f()\n  ret void\n}', 'indirect call: Call')
        translate('define void @Fence() {\n  call void asm sideeffect "", "~{memory}"()\n  ret void\n}',
                  {}, 'GuestRuntime_RegisterUnit_test', lower=False)

    def test_pins_in_initializers_and_aggregates_refused(self):
        self.refused('@record = external global i32\n@table = global [1 x ptr] [ptr @record]',
                     'guest address in the initializer of table', {'record': 0x80018000})
        self.refused('@record = external global i32\n@slot = global { ptr } zeroinitializer\n'
                     'define void @Put() {\n  store { ptr } { ptr @record }, ptr @slot\n  ret void\n}',
                     'guest address: Put', {'record': 0x80018000})

    def test_hooks_refused(self):
        self.refused('define void @Hooked() {\n  ret void\n}', 'unlowered unit with hooks', hooks=True)


class SymbolNormalizationTests(unittest.TestCase):
    def test_private_function_export_preserves_body(self):
        result = normalize('define internal i32 @helper(i32 %value) { ret i32 %value }',
                           export_functions=('helper',))
        self.assertEqual(inspect(result)['definitions'], ['helper'])
        self.assertNotIn('internal', result)
        self.assertIn('ret i32 %value', result)
        with self.assertRaisesRegex(TranslationError, 'missing exported function definition'):
            normalize('declare i32 @helper(i32)', export_functions=('helper',))
    def test_assembler_global_aliases_share_guest_address(self):
        source = r'''
@record = external global [1 x i8]
@"\01record" = external global i8
define ptr @address() {
  ret ptr @"\01record"
}
'''
        normalized = normalize(source)
        self.assertNotIn('record.1', normalized)
        self.assertNotIn('\\01record', normalized)
        self.assertIn('ret ptr @record', normalized)
        adapted = translate(normalized, {'record': 0x80018000})
        self.assertNotIn('@record', adapted)
        self.assertIn('2147581952', adapted)

    def test_function_alias_keeps_definition_and_call_type(self):
        source = r'''
declare void @"\01_transform"(i32)
define i32 @transform(i16 %value) {
  %result = zext i16 %value to i32
  ret i32 %result
}
define void @caller() {
  call void @"\01_transform"(i32 65535)
  ret void
}
'''
        normalized = normalize(source)
        self.assertEqual(inspect(normalized)['definitions'], ['transform', 'caller'])
        self.assertIn('call void @transform(i32 65535)', normalized)
        self.assertNotIn('transform.1', normalized)

    def test_definition_wins_over_existing_declaration(self):
        normalized = normalize(r'''
@value = external global i32
@"\01value" = global i32 7
define ptr @address() { ret ptr @value }
''')
        self.assertEqual(inspect(normalized)['globals'], ['value'])
        self.assertIn('@value = global i32 7', normalized)

    def test_explicit_renames_merge_global_declarations(self):
        normalized = normalize('''
@old = external global i8
@new = global i8 9
define ptr @address() { ret ptr @old }
''', {'old': 'new'})
        self.assertIn('ret ptr @new', normalized)
        self.assertNotIn('@old', normalized)

    def test_duplicate_definitions_fail(self):
        with self.assertRaisesRegex(TranslationError, 'duplicate renamed definition: value'):
            normalize(r'@value = global i32 1' + '\n' + r'@"\01value" = global i32 2')

    def test_incompatible_symbol_kinds_fail(self):
        with self.assertRaisesRegex(TranslationError, 'incompatible renamed symbols: value'):
            normalize('''
@value = external global i32
declare void @old()
''', {'old': 'value'})


class OverlaySignatureTests(unittest.TestCase):
    def test_effect_signature_uses_llvm_types_and_attributes(self):
        # Attribute ordering/numbering is irrelevant to the signature.
        validate_declaration('func_801462B0', '''
declare void @func_801462B0(i16 signext noundef, i16 signext, i32, ptr) #7
attributes #7 = { nounwind }
''')

    def test_effect_signature_mismatches_rejected(self):
        for declaration in (
            'declare void @func_801462B0(i16, i16 signext, i32, ptr)',
            'declare void @func_801462B0(i16 signext, i16 signext, i64, ptr)',
            'declare void @func_801462B0(i16 signext, i16 signext, i32, ptr, ...)',
            'declare i32 @func_801462B0(i16 signext, i16 signext, i32, ptr)',
        ):
            with self.subTest(declaration=declaration):
                with self.assertRaisesRegex(ValueError, 'Unsupported direct overlay entry signature'):
                    validate_declaration('func_801462B0', declaration)

    def test_overlay_special_abi_attributes_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported native ABI attributes.*argument 3 byval'):
            validate_declaration('func_801462B0',
                'declare void @func_801462B0(i16 signext, i16 signext, i32, ptr byval(i32))')


class NativeABITests(unittest.TestCase):
    def test_libgs_psxlong_parameters_must_not_widen(self):
        for name in ('GsSetAmbient', 'GsSetProjection'):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, 'Unaudited native call type'):
                    emit_native_calls([(1, name, 0, 0)], [f'declare void @{name}(i64)'])

    def test_special_parameter_attributes_rejected(self):
        for attribute in ('byval(i32)', 'byref(i32)', 'sret(i32)', 'inalloca(i32)',
                          'preallocated(i32)', 'inreg', 'nest', 'swiftself',
                          'swifterror', 'swiftasync'):
            with self.subTest(attribute=attribute):
                with self.assertRaisesRegex(ValueError, 'Unsupported native ABI attributes.*argument 0'):
                    emit_native_calls([(1, 'Unknown', 0, 0)],
                        [f'declare void @Unknown(ptr {attribute})'])

    def test_calling_convention_and_return_attribute_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported calling convention: Unknown'):
            emit_native_calls([(1, 'Unknown', 0, 0)], ['declare fastcc void @Unknown(ptr)'])
        with self.assertRaisesRegex(ValueError, 'Unsupported native ABI attributes.*result inreg'):
            emit_native_calls([(1, 'Unknown', 0, 0)], ['declare inreg i32 @Unknown()'])

    def test_ordinary_parameter_hints_preserved(self):
        generated = emit_native_calls([(1, 'Unknown', 0, 0)],
            ['declare zeroext i8 @Unknown(i8 zeroext, ptr noundef nonnull)'])
        self.assertIn('uint8_t (*)(uint8_t, void *)', generated)


if __name__ == '__main__':
    unittest.main()
