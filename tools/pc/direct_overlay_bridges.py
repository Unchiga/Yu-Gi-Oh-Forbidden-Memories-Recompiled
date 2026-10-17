"""Typed direct imports whose actual target depends on the loaded PS1 bank."""
from llvm_guest import inspect, validate_c_abi
ENTRIES = {'func_8016AA6C': (0x8016AA6C, False),
           'func_8016866C': (0x8016866C, True),
           'func_80168FB4': (0x80168FB4, False),
           'func_801462B0': (0x801462B0, 'effect')}

def validate_signature(name, signature, aapcs64=False):
    validate_c_abi(name, signature)
    expected = ([('i16', True), ('i16', True), ('i32', False), ('ptr', False)]
                if name == 'func_801462B0' else
                [('i32', False)] if ENTRIES[name][1] else [])
    actual = [(parameter['kind'], parameter['signext'])
              for parameter in signature['parameters']]
    # Darwin's ARM64 ABI has the caller sign-extend the i16 arguments;
    # AAPCS64 (Android, aapcs64=True) leaves that to the callee, so clang
    # marks no parameter there and the bridge, built for the same target,
    # agrees. Darwin keeps the strict check.
    if aapcs64:
        expected = [(kind, False) for kind, _ in expected]
    if (signature['result'] != 'void' or signature['variadic'] or
            actual != expected):
        raise ValueError(f'Unsupported direct overlay entry signature: {name}: {signature}')


def validate_declaration(name, module_ir):
    signature = inspect(module_ir)['signatures'].get(name)
    if signature is None:
        raise ValueError('Missing direct overlay entry signature: ' + name)
    validate_signature(name, signature)

def emit_bridge(name):
    if name == 'func_801462B0':
        return ('#include "game/duel_effect_request.h"\n'
                'extern void Memories_DuelEffectControl(short, short, int, DuelEffectRequest *);\n'
                'void func_801462B0(short id, short state, int buffer, DuelEffectRequest *request) '
                '{ Memories_DuelEffectControl(id, state, buffer, request); }\n')
    address, takes_index = ENTRIES[name]
    argument = 'int32_t index' if takes_index else 'void'
    pointer_argument = 'int32_t' if takes_index else 'void'
    call = 'index' if takes_index else ''
    return f'void {name}({argument}) {{ ((void (*)({pointer_argument}))GuestRuntime_ResolveFunction((void *)(uintptr_t)0x{address:08x}u))({call}); }}\n'
