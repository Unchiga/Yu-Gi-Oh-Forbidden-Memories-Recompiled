#!/usr/bin/env python3
"""config/pc/model_span_words.csv (tools/pc/model_span_words.py), the labels
of each native row's words past the data start and outside the command table:
the table covers every row and slot B's C is slot A's renamed (a failure when
not), and each row's digest (the C its labels came from) and the tool digest
are the tree's or a WARNING on stderr naming the rows and the regeneration
command (no compiler: these run on every CI; labels describe and gate
nothing, so a stale table never fails), and no label makes C by itself.
With clang as the check compiler (model_gate_port's; skipped where it is not
clang, as on Linux CI with gcc): a few rows' labels are what the analysis
gives today and model_gate_scan.py's slot words among them are labelled a
sink; made-up modules whose data words reach a division, an index, a shift
and a loop bound directly, through a work-area member stored whole and read
at another offset, through a work area with no record and through a
callback's argument are labelled with the sink; and no word a read of the
module's C reaches is labelled `unread` (over the made-up modules and the
review's counterexamples, COUNTER and COUNTER2).

    python tools/pc/test_model_span_words.py
"""
from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_gate  # noqa: E402
import model_gate_port  # noqa: E402
import model_span_words  # noqa: E402

# Rows checked against a fresh analysis: shape 54 (GsIMAGE headers and the
# last table entry's overhang), 135 (an unsized GsIMAGE array, and headers
# packed through &D[i] in a loop), 165 (words the C computes with), the
# primary 46 (one entry of a table that runs to the image's end).
SAMPLES = (("variant", 54), ("variant", 135), ("variant", 165), ("primary", 46))


def clang() -> bool:
    """The analysis walks clang's AST: the check compiler must be clang."""
    if not shutil.which(model_gate_port.CHECK_CC):
        return False
    try:
        result = subprocess.run([model_gate_port.CHECK_CC, "--version"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    # "clang version" (Apple's gcc is clang too, and says so on its own line)
    return "clang version" in result.stdout


CLANG = clang()

# --- made-up modules -----------------------------------------------------------------
# Data words that reach a division, an index, a shift and a loop bound (a)
# directly, (b) through a work-area member stored whole and read at another
# offset (work->a then work->a.vy), (c) stored and read back through a work
# area with no record, 2 bytes on, (d) through a callback's argument (one the
# module registers with the game, one it calls through a pointer). Each module
# also has a word only computed with (value) and, in (a), a texture header
# (texture): the controls.

PRELUDE = """#include "types.h"
#include "game/model_texture_upload.h"
typedef struct { s16 vx, vy, vz, pad; } SpanV;
typedef struct { s16 m[3][3]; s32 t[3]; } SpanM;   /* 0x20 bytes, t at 0x14 */
extern s32 g_span_sink[8];
"""

SOURCES = {
    "direct": PRELUDE + """
extern s32 D_8013B100, D_8013B104, D_8013B108, D_8013B10C, D_8013B110;
extern GsIMAGE D_8013B114;

s32 func_8013B004(u8 *context, s32 command)
{
    s32 i, t = 0;
    s32 table[8];
    t += 1000 / D_8013B100;
    table[D_8013B104] = t;
    t += 1 << D_8013B108;
    for (i = 0; i < D_8013B10C; i++) {
        t++;
    }
    g_span_sink[0] = t + D_8013B110 * 3;
    g_span_sink[1] = func_80059A50(0, 1, &D_8013B114);
    return table[0];
}
""",
    "member": PRELUDE + """
typedef struct { s32 head; SpanM a; SpanM b; SpanM c; SpanM d; SpanM e; s32 n; } SpanWork;
extern SpanM D_8013B100, D_8013B120, D_8013B140, D_8013B160, D_8013B180;

s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    s32 i, t = 0;
    s32 table[8];
    if (command >= 0) {
        work->a = D_8013B100;
        work->b = D_8013B120;
        work->c = D_8013B140;
        work->d = D_8013B160;
        work->e = D_8013B180;
        return 0;
    }
    /* t[2]: 0x1C bytes past the start of the store, past NEAR */
    t += 1000 / work->a.t[2];
    table[work->b.t[2]] = t;
    t += 1 << work->c.t[2];
    for (i = 0; i < work->d.t[2]; i++) {
        t++;
    }
    return t + work->e.t[2] * 2 + table[0];
}
""",
    "norecord": PRELUDE + """
extern SpanV D_8013B100, D_8013B108, D_8013B110, D_8013B118, D_8013B120;

s32 func_8013B004(u8 *context, s32 command)
{
    s32 i, t = 0;
    s32 table[8];
    if (command >= 0) {
        *(SpanV *)(context + 0x10) = D_8013B100;
        *(SpanV *)(context + 0x50) = D_8013B108;
        *(SpanV *)(context + 0x90) = D_8013B110;
        *(SpanV *)(context + 0xD0) = D_8013B118;
        *(SpanV *)(context + 0x110) = D_8013B120;
        return 0;
    }
    t += 1000 / *(s16 *)(context + 0x12);
    table[*(s16 *)(context + 0x52)] = t;
    t += 1 << *(s16 *)(context + 0x92);
    for (i = 0; i < *(s16 *)(context + 0xD2); i++) {
        t++;
    }
    return t + *(s16 *)(context + 0x112) * 2 + table[0];
}
""",
    "sinks": PRELUDE + """
extern s32 D_8013B100;
extern s32 *D_8013B104;
extern s32 D_8013B108[4];
extern s32 D_8013B118[2];
extern void SpanGame_Take(s32 *words);

s32 func_8013B004(u8 *context, s32 command)
{
    *(s32 *)D_8013B100 = 1;                  /* a word as an address */
    g_span_sink[0] = *D_8013B104;            /* a pointer the data holds */
    SpanGame_Take(D_8013B108);               /* its address to the game */
    g_span_sink[1] = (s32)D_8013B118 - 4;    /* its address as a number */
    return 0;
}
""",
    "pointercall": PRELUDE + """
extern s32 D_8013B100;
extern void (*g_span_hook)(s32 value);   /* a pointer the game holds: no callback of the module's */

s32 func_8013B004(u8 *context, s32 command)
{
    g_span_hook(D_8013B100);
    return 0;
}
""",
    "callback": PRELUDE + """
extern s32 D_8013B100, D_8013B104, D_8013B108, D_8013B10C;
extern s32 D_8013B110, D_8013B114, D_8013B118, D_8013B11C, D_8013B120;
extern void SpanGame_Register(void (*callback)(s32, s32, s32, s32), s32 a, s32 b, s32 c, s32 d);

void func_8013B200(s32 a, s32 b, s32 c, s32 d)
{
    s32 i, t = 0;
    s32 table[8];
    t += 1000 / a;
    table[b] = t;
    t += 1 << c;
    for (i = 0; i < d; i++) {
        t++;
    }
    g_span_sink[0] = t + table[0];
}

void func_8013B300(s32 a, s32 b, s32 c, s32 d)
{
    s32 i, t = 0;
    s32 table[8];
    t += 1000 / a;
    table[b] = t;
    t += 1 << c;
    for (i = 0; i < d; i++) {
        t++;
    }
    g_span_sink[1] = t + table[0];
}

s32 func_8013B004(u8 *context, s32 command)
{
    void (*later)(s32, s32, s32, s32) = func_8013B300;
    SpanGame_Register(func_8013B200, D_8013B100, D_8013B104, D_8013B108, D_8013B10C);
    later(D_8013B110, D_8013B114, D_8013B118, D_8013B11C);
    if (D_8013B120 * 2 > 5) {
        g_span_sink[2] = 0;
    }
    return 0;
}
""",
}

# {module: {offset: the label it must have}}: "unsafe" means any of
# model_span_words.UNSAFE (a sink, or a row the walk does not follow).
EXPECT = {
    "direct": {0x100: "unsafe", 0x104: "unsafe", 0x108: "unsafe", 0x10C: "unsafe", 0x110: "value",
               0x114: "texture"},
    "member": {0x11C: "unsafe", 0x13C: "unsafe", 0x15C: "unsafe", 0x17C: "unsafe", 0x19C: "value"},
    "norecord": {0x100: "unsafe", 0x108: "unsafe", 0x110: "unsafe", 0x118: "unsafe", 0x120: "value"},
    "sinks": {0x100: "pointer", 0x104: "pointer", 0x108: "address", 0x118: "address"},
    "pointercall": {0x100: "unknown"},
    "callback": {0x100: "unsafe", 0x104: "unsafe", 0x108: "unsafe", 0x10C: "unsafe", 0x110: "unsafe",
                 0x114: "unsafe", 0x118: "unsafe", 0x11C: "unsafe", 0x120: "unknown"},
}

# The review's counterexamples (D:/YGO/scratch/review-part3/counter.py): each
# a construct the walk does not model, or models only conservatively; every
# word named must come out labelled as read, and not as computed with or
# packed: a sink, or `unknown` for a row the walk does not follow.
# {name: (source, {offset: "unsafe"}, command_table)}
COUNTER = {
    # (1) a record-less work area: a 0x20-byte store at +0x10, read at +0x24
    # (0x14 past the store's start, outside NEAR=16).
    "norecord_wide": (PRELUDE + """
extern SpanM D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    if (command >= 0) {
        *(SpanM *)(context + 0x10) = D_8013B100;
        return 0;
    }
    return 1000 / *(s32 *)(context + 0x24);
}
""", {0x114: "unsafe"}),
    # (2) a record with separate s16 members, stored through a wider cast.
    "member_narrow": (PRELUDE + """
typedef struct { s32 head; s16 x; s16 y; s16 z; s16 pad; s32 n; } SpanWork;
extern SpanV D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    if (command >= 0) {
        *(SpanV *)&work->x = D_8013B100;
        return 0;
    }
    return 1000 / work->y;
}
""", {0x102: "unsafe"}),
    # (3) the game keeps what it is given (a setter), the module reads it back.
    "game_global": (PRELUDE + """
extern s32 D_8013B100;
extern s32 D_800A0000;
extern void func_80012340(s32 value);   /* the game: D_800A0000 = value */
s32 func_8013B004(u8 *context, s32 command)
{
    func_80012340(D_8013B100);
    return 1000 / D_800A0000;
}
""", {0x100: "unsafe"}),
    "game_getter": (PRELUDE + """
extern s32 D_8013B100;
extern void func_80012340(s32 value);   /* the game keeps value */
extern s32 func_80012350(void);         /* and gives it back */
s32 func_8013B004(u8 *context, s32 command)
{
    func_80012340(D_8013B100);
    return 1000 / func_80012350();
}
""", {0x100: "unsafe"}),
    # (4) game memory stored by one name, read by another.
    "pool_alias_var": (PRELUDE + """
extern s32 D_8013B100;
extern s32 D_800A0000;
s32 func_8013B004(u8 *context, s32 command)
{
    s32 *p = &D_800A0000;
    *p = D_8013B100;
    return 1000 / D_800A0000;
}
""", {0x100: "unsafe"}),
    "pool_alias_type": (PRELUDE + """
extern s32 D_8013B100;
extern u16 D_800A0000[2];
s32 func_8013B004(u8 *context, s32 command)
{
    *(s32 *)D_800A0000 = D_8013B100;
    return 1000 / D_800A0000[1];
}
""", {0x102: "unsafe"}),
    # (5) a loop the data's terminator ends, through a switch.
    "switch_exit": (PRELUDE + """
extern s32 D_8013B100[4];
s32 func_8013B004(u8 *context, s32 command)
{
    s32 i, table[4];
    for (i = 0; ; i++) {
        switch (D_8013B100[i]) {
        case -1:
            return table[0];
        default:
            table[i] = 1;
            break;
        }
    }
}
""", {0x100: "unsafe"}),
    # (6) a local set only on the paths the retail data takes.
    "uninit": (PRELUDE + """
extern s32 D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    s32 c, table[4];
    switch (D_8013B100) {
    case 0: c = 1; break;
    case 1: c = 2; break;
    }
    return 1000 / c + table[0];
}
""", {0x100: "unsafe"}),
    # (7) a per-frame counter the data bounds (the game's frames are the loop).
    "frame_bound": (PRELUDE + """
typedef struct { s32 frame; s32 table[4]; } SpanWork;
extern s32 D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    if (command >= 0) {
        work->frame = 0;
        return 0;
    }
    {
        s32 local[4];   /* host stack in the C, the guest stack in the interpreter */
        local[work->frame] = 1;
        g_span_sink[0] = local[0];
    }
    work->frame++;
    if (work->frame >= D_8013B100) {
        work->frame = 0;
    }
    return 0;
}
""", {0x100: "unsafe"}),
    # (8) the command picks entry command + 1 of a 2-entry command table
    # (0x100:8:2:0): command 1 reads the word after the table as a divisor.
    "command_plus_one": (PRELUDE + """
typedef struct { s32 a; s32 b; } SpanE;
extern SpanE D_8013B100[];
extern s32 D_8013B110;
s32 func_8013B004(u8 *context, s32 command)
{
    if (command >= 0) {
        return 1000 / D_8013B100[command + 1].a;
    }
    return D_8013B110;
}
""", {0x110: "unsafe"}, "0x100:8:2:0"),
    # (9) a variable index past a symbol's declared size, into the gap after it.
    "declared_overhang": (PRELUDE + """
typedef struct { s32 frame; } SpanWork;
extern s32 D_8013B100[2];
extern s32 D_8013B120;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    if (command >= 0) {
        work->frame = 0;
        return D_8013B120;
    }
    work->frame++;
    return 1000 / D_8013B100[work->frame];
}
""", {0x108: "unsafe"}),
    # (10) memcpy of a work-area slot pointer into a local, then a read through it.
    "memcpy_ptr": (PRELUDE + """
#include <string.h>
typedef struct { s32 *p; } SpanWork;
extern s32 D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    s32 *q;
    if (command >= 0) {
        work->p = &D_8013B100;
        return 0;
    }
    memcpy(&q, &work->p, sizeof q);
    return 1000 / *q;
}
""", {0x100: "unsafe"}),
    # (11) a callback the module registers with the game, which the game calls
    # with the work area, reading what the entry stored there.
    "callback_work": (PRELUDE + """
typedef struct { s32 head; s32 d; } SpanWork;
extern s32 D_8013B100;
extern void SpanGame_Register(void (*callback)(u8 *), u8 *context);
void func_8013B200(u8 *context)
{
    SpanWork *work = (SpanWork *)context;
    g_span_sink[0] = 1000 / work->d;
}
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    work->d = D_8013B100;
    SpanGame_Register(func_8013B200, context);
    return 0;
}
""", {0x100: "unsafe"}),
    # (11b) two records of the work area: a smaller one with a vector member,
    # the largest with separate halfwords at the same bytes.
    "two_records": (PRELUDE + """
typedef struct { s32 head; SpanV v; } SpanSmall;
typedef struct { s32 head; s16 x; s16 y; s16 z; s16 pad; s32 more[8]; } SpanLarge;
extern SpanV D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    if (command >= 0) {
        ((SpanSmall *)context)->v = D_8013B100;
        return 0;
    }
    return 1000 / ((SpanLarge *)context)->y;
}
""", {0x102: "unsafe"}),
    # (13) the modules' own idiom: a scale vector from the data, handed to
    # ScaleMatrix by address, and the matrix then divided by.
    "scale_pointee": (PRELUDE + """
#include "psyq/libgte.h"
extern VECTOR D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    VECTOR scale;
    MATRIX m;
    scale = D_8013B100;
    ScaleMatrix(&m, &scale);
    return 1000 / m.m[0][0];
}
""", {0x100: "unsafe"}),
    # (12) the same callback, but the game is not handed the work area: it
    # keeps it from an earlier call (the callback's first argument).
    "callback_work_kept": (PRELUDE + """
typedef struct { s32 head; s32 d; } SpanWork;
extern s32 D_8013B100;
extern void SpanGame_Register(void (*callback)(u8 *));
void func_8013B200(u8 *context)
{
    SpanWork *work = (SpanWork *)context;
    g_span_sink[0] = 1000 / work->d;
}
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    work->d = D_8013B100;
    SpanGame_Register(func_8013B200);
    return 0;
}
""", {0x100: "unsafe"}),
}




class Table(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = model_span_words.load()

    def test_covers_every_row_exactly(self):
        self.assertTrue(self.table, f"{model_span_words.TABLE} is missing or empty")
        self.assertEqual(model_span_words.coverage(self.table), [])

    def test_slot_b_is_slot_a_renamed(self):
        self.assertEqual(model_span_words.wrappers(), [])

    def test_the_table_is_the_trees_or_warns(self):
        """The digest of each row's C and of the analysis, the scan it builds
        on, the flags and rows it reads and the game's headers the rows
        include (no compiler: Linux CI runs this). Labels describe and gate
        nothing, and regenerating them needs clang on Windows: a table that is
        not the tree's is a WARNING with the rows and the command, never a
        failure (coverage is: test_covers_every_row_exactly)."""
        warnings = model_span_words.stale_warnings()
        model_span_words.warn(warnings)
        for text in warnings:
            self.assertIn("python tools/pc/model_span_words.py", text, "the warning names the command")

    def test_a_stale_table_warns_and_does_not_fail(self):
        with tempfile.TemporaryDirectory(prefix="span-words-") as folder:
            copy = Path(folder) / "model_span_words.csv"
            row = next(r for r in model_span_words.all_rows() if (r.KIND.name, r.shape) == ("variant", 54))
            text = model_span_words.TABLE.read_bytes().decode("utf-8")
            text = text.replace(model_span_words.row_digest(row), "0" * 16)
            text = text.replace(model_span_words.tool_digest(), "1" * 16)
            copy.write_bytes(text.encode("utf-8"))
            warnings = model_span_words.stale_warnings(copy)
            self.assertEqual(len(warnings), 2, warnings)
            self.assertIn("tool digest", warnings[0])
            self.assertIn("variant 54", warnings[1])
            for text in warnings:
                self.assertIn("python tools/pc/model_span_words.py", text)
            self.assertEqual(model_span_words.coverage(model_span_words.load(copy)), [], "coverage is still checked")

    def test_a_changed_tool_is_stale(self):
        plain = model_span_words.tool_digest()
        crlf = model_span_words.tool_digest(lambda path: model_span_words.normalised(path).replace("\n", "\r\n"))
        edited = model_span_words.tool_digest(lambda path: model_span_words.normalised(path) + "\n# edit\n")
        self.assertEqual(plain, crlf, "Windows and Linux line ends")
        self.assertNotEqual(plain, edited)
        names = [path.relative_to(model_span_words.ROOT).as_posix() for path in model_span_words.shared_files()]
        self.assertIn("src/pc/compat/pgxp_game.h", names)
        for name in ("src/port_ptr.h", "src/types.h", "src/psyq/libgte.h", "src/pc/overlays/model_stale_locals.h"):
            self.assertIn(name, names)
        for name in ("tools/pc/model_gate_scan.py", "tools/pc/model_gate.py", "tools/pc/model_gate_port.py",
                     "src/game/model_texture_upload.c", "src/game/func_80059A50.c"):
            self.assertIn(name, model_span_words.TOOL_FILES)
        self.assertTrue(any(name.startswith("src/psyq/") for name in names), names)
        with tempfile.TemporaryDirectory(prefix="span-words-") as folder:
            copy = Path(folder) / "model_span_words.csv"
            text = model_span_words.TABLE.read_bytes().decode("utf-8").replace(plain, "0" * 16)
            copy.write_bytes(text.encode("utf-8"))
            found = model_span_words.tool_stale(copy)
            self.assertEqual(len(found), 1, found)
            self.assertIn("python tools/pc/model_span_words.py", found[0], "the message names the command")

    def test_a_changed_c_is_stale(self):
        row = next(r for r in model_span_words.all_rows() if (r.KIND.name, r.shape) == ("variant", 54))
        plain = model_span_words.row_digest(row)
        crlf = model_span_words.row_digest(row, lambda path: path.read_bytes().decode("utf-8").replace("\r\n", "\n").replace("\n", "\r\n"))
        edited = model_span_words.row_digest(row, lambda path: path.read_bytes().decode("utf-8") + "\n/* edit */\n")
        self.assertEqual(plain, crlf, "Windows and Linux line ends")
        self.assertNotEqual(plain, edited)
        with tempfile.TemporaryDirectory(prefix="span-words-") as folder:
            copy = Path(folder) / "model_span_words.csv"
            text = model_span_words.TABLE.read_bytes().decode("utf-8").replace(plain, "0" * 16)
            copy.write_bytes(text.encode("utf-8"))
            found = model_span_words.stale(copy, [("variant", 54)])
            self.assertEqual(len(found), 1, found)
            self.assertIn("python tools/pc/model_span_words.py", found[0], "the message names the command")

    def test_no_label_makes_c(self):
        """No label is a safety class: every changed word needs a port by
        hand or --accept-data (test_model_migrate.py's labels case, per label)."""
        self.assertEqual(model_span_words.SAFE, ())
        self.assertEqual(set(model_span_words.ORDER), model_span_words.CLASSES)
        # A word with bytes of several labels gets the first: a sink before a
        # use, `unread` last.
        self.assertEqual(model_span_words.ORDER[-1], "unread")

    def test_shape_54(self):
        """The bytes past the command table's last entry are not read by the
        module's own C; the GsIMAGE headers are labelled read (the packed page
        goes into the packets the module hands GsSortPoly, whose pointees a
        game call is taken to read and write back)."""
        for offset in (0x1648, 0x164C, 0x16B4, 0x16B8, 0x16D0):
            found = model_span_words.class_of(self.table, "variant", 54, offset)
            self.assertNotIn(found[0], ("unread", "texture", "value"), hex(offset))
        found = model_span_words.class_of(self.table, "variant", 54, 0x17B8)
        self.assertEqual(found[0], "unread")
        self.assertIsNone(model_span_words.class_of(self.table, "variant", 54, 0x16D4), "the command table")

    def test_class_of_outside_any_row(self):
        self.assertIsNone(model_span_words.class_of(self.table, "variant", 54, 0x100))
        self.assertIsNone(model_span_words.class_of(self.table, "variant", 99999, 0x1648))


def classes(runs) -> dict[int, tuple[str, str]]:
    """{byte offset: (class, symbol)} of a row's runs."""
    return {at: (cls, symbol) for start, size, cls, symbol, _ in runs or () for at in range(start, start + size)}


# The review's second pass (D:/YGO/scratch/review-part3/counter2.py): words the
# C reads that escape the walk's labels. Labels only now: the invariant holds
# for them as for every module, no word any read reaches makes C by itself.
PRELUDE2 = """#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/rand.h"
#include "game/model_texture_upload.h"
typedef struct { s16 vx, vy, vz, pad; } SpanV;
extern s32 g_span_sink[8];
extern s32 Model_GetActiveSlotIndex(void);
extern u32 func_80059A50(s32, s32, GsIMAGE *);
"""



def exempt2(call, word="s32 D_8013B100"):
    """A data word into `call`, the result at a divisor and at a local index."""
    return PRELUDE2 + f"""
extern {word};
s32 func_8013B004(u8 *context, s32 command)
{{
    s32 local[4];
    s32 r = {call};
    local[r] = 1;
    return 1000 / r + local[0];
}}
"""



COUNTER2 = {
    # --- the exempt list: each result is a function of its argument ---
    "pure_GetTPage": (exempt2("GetTPage(D_8013B100, 0, 0, 0)"), {0x100: "unsafe"}),
    "pure_GetClut": (exempt2("GetClut(D_8013B100, 0)"), {0x100: "unsafe"}),
    "pure_rsin": (exempt2("rsin(D_8013B100)"), {0x100: "unsafe"}),
    "pure_rcos": (exempt2("rcos(D_8013B100)"), {0x100: "unsafe"}),
    "pure_csin": (exempt2("csin(D_8013B100)"), {0x100: "unsafe"}),
    "pure_ccos": (exempt2("ccos(D_8013B100)"), {0x100: "unsafe"}),
    "pure_ratan2": (exempt2("ratan2(D_8013B100, 1)"), {0x100: "unsafe"}),
    "pure_SquareRoot0": (exempt2("SquareRoot0(D_8013B100)"), {0x100: "unsafe"}),
    "pure_SquareRoot12": (exempt2("SquareRoot12(D_8013B100)"), {0x100: "unsafe"}),
    "pure_packcopy_slot": (exempt2("func_80059A50(0, 1, &D_8013B100)", "GsIMAGE D_8013B100"), {0x104: "unsafe"}),
    "pure_pack_slot": (exempt2("ModelTexture_PackPageClut(0, 1, &D_8013B100)", "GsIMAGE D_8013B100"),
                       {0x104: "unsafe"}),
    # rand keeps state srand sets: the seed from the data, rand() at a sink.
    "pure_rand_srand": (PRELUDE2 + """
extern u32 D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    s32 local[4];
    srand(D_8013B100);
    local[rand() & 7] = 1;
    return 1000 / (rand() & 0xF) + local[0];
}
""", {0x100: "unsafe"}),
    # The packer handed a copy of the header (a local, the work area): the
    # pointee is not carried for TEXTURE functions.
    "pack_local_copy": (PRELUDE2 + """
extern GsIMAGE D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    GsIMAGE image;
    s32 local[4];
    u32 r;
    image = D_8013B100;
    r = func_80059A50(0, 1, &image);
    local[r & 0xF] = 1;
    return 1000 / (s32)(r & 0xF) + local[0];
}
""", {0x104: "unsafe"}),
    "pack_work_copy": (PRELUDE2 + """
typedef struct { s32 head; GsIMAGE image; u32 packed; } SpanWork;
extern GsIMAGE D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    s32 local[4];
    if (command >= 0) {
        work->image = D_8013B100;
        work->packed = ModelTexture_PackPageClut(0, 1, &work->image);
        return 0;
    }
    local[work->packed & 0xF] = 1;
    return 1000 / (s32)(work->packed & 0xF) + local[0];
}
""", {0x104: "unsafe"}),
    # --- the setter / game-sink lock ---
    # The setter's result is used, so it is no statement and not a setter.
    "setter_used": (PRELUDE2 + """
extern s32 D_8013B100;
extern s32 func_80012340(s32 value);   /* the game keeps value, returns 0 */
extern s32 func_80012350(void);        /* and gives it back */
s32 func_8013B004(u8 *context, s32 command)
{
    g_span_sink[0] = func_80012340(D_8013B100);
    return 1000 / func_80012350();
}
""", {0x100: "unsafe"}),
    # The game gives back a pointer to what it kept: a read of game memory
    # (tag game/memory), which the lock does not count.
    "setter_ptr_getter": (PRELUDE2 + """
extern s32 D_8013B100;
extern void func_80012340(s32 value);
extern s32 *func_80012360(void);
s32 func_8013B004(u8 *context, s32 command)
{
    func_80012340(D_8013B100);
    return 1000 / *func_80012360();
}
""", {0x100: "unsafe"}),
    "setter_abs_address": (PRELUDE2 + """
extern s32 D_8013B100;
extern void func_80012340(s32 value);   /* stores it at 0x800A0000 */
s32 func_8013B004(u8 *context, s32 command)
{
    func_80012340(D_8013B100);
    return 1000 / *(s32 *)0x800A0000;
}
""", {0x100: "unsafe"}),
    "setter_struct_getter": (PRELUDE2 + """
typedef struct { s32 a; s32 kept; } GameThing;
extern s32 D_8013B100;
extern void func_80012340(s32 value);
extern GameThing *func_80012360(void);
s32 func_8013B004(u8 *context, s32 command)
{
    s32 local[4];
    func_80012340(D_8013B100);
    local[func_80012360()->kept] = 1;
    return local[0];
}
""", {0x100: "unsafe"}),
    # --- the known-pointer filter in deref drops the game's memory ---
    "deref_mixed": (PRELUDE2 + """
typedef struct { s32 head; s32 x; } SpanWork;
extern s32 D_8013B100;
extern s32 D_800A0000;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    s32 *p = command >= 0 ? &work->x : &D_800A0000;
    *p = D_8013B100;
    return 1000 / D_800A0000;
}
""", {0x100: "unsafe"}),
    # --- the width lock is checked in assign only ---
    "compound_wide": (PRELUDE2 + """
typedef struct { s32 head; s16 x; s16 y; s32 n; } SpanWork;
extern s32 D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanWork *work = (SpanWork *)context;
    if (command >= 0) {
        *(s32 *)&work->x |= D_8013B100;
        return 0;
    }
    return 1000 / work->y;
}
""", {0x102: "unsafe"}),
    "compound_norecord": (PRELUDE2 + """
typedef struct { s32 m[8]; } SpanBig;
extern SpanBig D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    if (command >= 0) {
        *(SpanBig *)(context + 0x10) = D_8013B100;
        return 0;
    }
    return 1000 / *(s32 *)(context + 0x2C);
}
""", {0x11C: "unsafe"}),
    # --- a branch that is no if/switch/?: ---
    "logical_branch": (PRELUDE2 + """
extern s32 D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    s32 c, local[4];
    D_8013B100 && (c = 1);
    local[0] = 0;
    return 1000 / c + local[0];
}
""", {0x100: "unsafe"}),
    # --- two_records variants: the smaller record read at a sink ---
    # (a) narrow stores through the largest record, a wide read through the smaller, then .vy
    "two_records_a": (PRELUDE2 + """
typedef struct { s32 head; SpanV v; } SpanSmall;
typedef struct { s32 head; s16 x; s16 y; s16 z; s16 pad; s32 more[8]; } SpanLarge;
extern s16 D_8013B100, D_8013B102;
s32 func_8013B004(u8 *context, s32 command)
{
    SpanV v;
    if (command >= 0) {
        ((SpanLarge *)context)->x = D_8013B100;
        ((SpanLarge *)context)->y = D_8013B102;
        return 0;
    }
    v = ((SpanSmall *)context)->v;
    return 1000 / v.vy;
}
""", {0x102: "unsafe"}),
    # (b) the largest record's member is a wide vector, the smaller one's
    # halfwords at the same bytes; stored whole, read as the smaller's y
    "two_records_b": (PRELUDE2 + """
typedef struct { s32 head; s16 x; s16 y; } SpanSmall;
typedef struct { s32 head; SpanV v; s32 more[8]; } SpanLarge;
extern SpanV D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    s32 local[4];
    if (command >= 0) {
        ((SpanLarge *)context)->v = D_8013B100;
        return 0;
    }
    local[((SpanSmall *)context)->y] = 1;
    return local[0];
}
""", {0x102: "unsafe"}),
    # (c) the smaller record is the one with the wide member, and it is
    # stored through a pointer the game fills (a game call writing &small->v)
    "two_records_c": (PRELUDE2 + """
typedef struct { s32 head; SpanV v; } SpanSmall;
typedef struct { s32 head; s16 x; s16 y; s16 z; s16 pad; s32 more[8]; } SpanLarge;
extern SVECTOR D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    if (command >= 0) {
        ((SpanSmall *)context)->v = *(SpanV *)&D_8013B100;
        return 0;
    }
    return 1000 / ((SpanLarge *)context)->y;
}
""", {0x102: "unsafe"}),
    # (d) a narrow read through the smaller record, the store a byte-wide
    # member of the largest at the same bytes, the read 2 bytes wide
    "two_records_d": (PRELUDE2 + """
typedef struct { s32 head; s16 y; } SpanSmall;
typedef struct { s32 head; u8 lo; u8 hi; s32 more[8]; } SpanLarge;
extern u8 D_8013B100, D_8013B101;
s32 func_8013B004(u8 *context, s32 command)
{
    if (command >= 0) {
        ((SpanLarge *)context)->lo = D_8013B100;
        ((SpanLarge *)context)->hi = D_8013B101;
        return 0;
    }
    return 1000 / ((SpanSmall *)context)->y;
}
""", {0x101: "unsafe"}),
    # GTE state set from the data (a setter), read back through an out-parameter
    "gte_outparam": (PRELUDE2 + """
extern VECTOR D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    VECTOR scale, out;
    MATRIX m;
    SVECTOR zero;
    PSXLONG flag;
    s32 local[4];
    scale = D_8013B100;
    setVector(&zero, 0, 0, 0);
    ScaleMatrix(&m, &scale);
    SetRotMatrix(&m);
    SetTransMatrix(&m);
    RotTrans(&zero, &out, &flag);
    local[out.vx] = 1;
    return 1000 / out.vy + local[0];
}
""", {0x100: "unsafe"}),
    "gte_readback": (PRELUDE2 + """
extern MATRIX D_8013B100;
s32 func_8013B004(u8 *context, s32 command)
{
    MATRIX m;
    SetRotMatrix(&D_8013B100);
    ReadRotMatrix(&m);
    return 1000 / m.m[0][0];
}
""", {0x100: "unsafe"}),
}


def analyse_text(folder: str, name: str, text: str, table: str = "none") -> dict:
    """A made-up module's bytes, as model_span_words.analyse classes them: the
    module's own names only (its image at 0x8013B000; a game global is not one)."""
    unit = Path(folder) / f"{name}.c"
    unit.write_text(text, encoding="utf-8")
    names = {f"{kind}_{digits}": int(digits, 16)
             for kind, digits in re.findall(r"\b(func|D)_([0-9A-F]{8})\b", text)
             if 0x8013B000 <= int(digits, 16) < 0x8013B000 + 0x5000}
    row = model_gate.Row(dict(shape="99001", backend="native", span="0x200", data_start="0x100",
                              sources="", command_table=table, identifier_a="0", identifier_b="0",
                              images="1", sha256_a="", sha256_b="", note=""))
    return model_span_words.analyse(row, [str(unit)], names)["bytes"]


@unittest.skipUnless(CLANG, f"{model_gate_port.CHECK_CC} is not clang on PATH")
class Negatives(unittest.TestCase):
    def check(self, found, name, offset, want):
        cls = found[offset][0]
        with self.subTest(module=name, offset=hex(offset)):
            if want == "unsafe":
                self.assertIn(cls, model_span_words.UNSAFE, found[offset])
            elif want == "read":
                self.assertNotIn(cls, ("unread", "texture", "value"), found[offset])
            else:
                self.assertEqual(cls, want, found[offset])

    def test_flows_into_sinks_are_unsafe(self):
        with tempfile.TemporaryDirectory(prefix="span-words-") as folder:
            for name, text in SOURCES.items():
                found = analyse_text(folder, name, text)
                for offset, want in EXPECT[name].items():
                    self.check(found, name, offset, want)

    def test_no_word_a_read_reaches_is_unread(self):
        """In every made-up module and every counterexample, each word the C
        reads (the ones the module names) is not labelled `unread`."""
        cases = [(name, text, EXPECT[name], "none") for name, text in SOURCES.items()]
        cases += [(name, case[0], case[1], case[2] if len(case) > 2 else "none") for name, case in COUNTER.items()]
        cases += [(name, case[0], case[1], "none") for name, case in COUNTER2.items()]
        with tempfile.TemporaryDirectory(prefix="span-words-") as folder:
            for name, text, expect, table in cases:
                found = analyse_text(folder, name, text, table)
                for offset, want in expect.items():
                    with self.subTest(module=name, offset=hex(offset)):
                        self.assertNotEqual(found[offset][0], "unread", found[offset])

    def test_counterexamples_are_locked(self):
        with tempfile.TemporaryDirectory(prefix="span-words-") as folder:
            for name, case in COUNTER.items():
                found = analyse_text(folder, name, case[0], case[2] if len(case) > 2 else "none")
                for offset in case[1]:
                    self.check(found, name, offset, "read")


@unittest.skipUnless(CLANG, f"{model_gate_port.CHECK_CC} is not clang on PATH")
class Analysis(unittest.TestCase):
    def test_samples_match_the_table(self):
        """The classes (the evidence's line numbers may move with the C)."""
        table = model_span_words.load()
        info = {}
        for kind, shape in SAMPLES:
            with self.subTest(kind=kind, shape=shape):
                _, _, runs, extra = model_span_words._one((kind, shape))
                self.assertIsNotNone(runs, extra)
                info[(kind, shape)] = extra
                fresh = model_span_words.load_text(model_span_words.render(runs))
                self.assertEqual(classes(fresh.get((kind, shape))), classes(table.get((kind, shape))))
        self.assertEqual(model_span_words.widened(table, info)[0], [])


if __name__ == "__main__":
    unittest.main()
