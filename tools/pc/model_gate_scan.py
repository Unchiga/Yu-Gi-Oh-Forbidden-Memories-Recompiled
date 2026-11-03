#!/usr/bin/env python3
"""Where a ported variant shape's C divides, takes a remainder, or indexes
with a value a mod can change (`model_gate.py scan`; notes/pc-build.md, "The
model modules in native C").

The hash of a native row covers `span` bytes of the slot's image. The
interpreter survives a zero divisor and an index out of range; native C does
not (x86 faults). So every divisor, remainder and index the C takes from the
slot's data must be inside the span, and one it takes from the command its
caller passes must be bounded by the row's command_table. This finds them in
the C, mechanically, from clang's AST of each slot A unit:

  every `/`, `%`, `/=`, `%=` whose right side is not a constant, and every
  array subscript and pointer step whose index is not one,

and follows the operand back to where its value comes from, through locals,
the module's functions' parameters, and the fields of the work area (keyed
by byte offset, from clang's record layouts, so `work->config` and
`MODEL_VARIANT_WORD(work, 0x1B74)` are one field). A source is one of:

  slot word  a word of the slot's data at a fixed offset (a D_ symbol of the
             module past its data start, plus a constant)
  table      a field of an entry of a table in the slot's data that the
             command picks (`&D_8013C31C[command]`, `(T *)(D + K) + command`):
             the table's offset and entry size, and the work-area field the
             setup keeps the entry's address in, are the row's command_table
  command    the command itself
  other      the work area's own counters, the game's state, resident calls,
             constants: nothing of the slot's data

Each site is then (a) covered: only slot words, all inside the span the scan
proposes; (b) the command table: only table fields (and other); (c) per
record: a slot word or table entry where the retail images of the shape
differ, which no hash can cover; (d) unclassified: a pointer the scan cannot
follow (a slot-data pointer indexed by something other than the command, a
read through a pointer of unknown origin), for review.

The proposed span is the data start, widened to the end of the last slot word
a covered site reads and to the end of the command table (entries the
retail commands pick: --commands, from the oracle's traces)."""
from __future__ import annotations

import collections
import json
import os
from pathlib import Path
import posixpath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_gate  # noqa: E402
import model_gate_port  # noqa: E402

FLAGS = [f for f in model_gate_port.CHECK_FLAGS if not f.startswith("-Werror") and f != "-fsyntax-only"]
BASIC = {"char": 1, "signed char": 1, "unsigned char": 1, "short": 2, "unsigned short": 2, "int": 4,
         "unsigned int": 4, "long": 4, "unsigned long": 4, "long long": 8, "unsigned long long": 8,
         "float": 4, "double": 8, "_Bool": 1, "void": 1}


# --- clang -------------------------------------------------------------------------------

def ast(unit: str) -> list[dict]:
    """The unit's top-level declarations from the module's own files."""
    # Only the module's functions (func_8013xxxx): the headers' declarations
    # are most of a unit's tree and none of them is needed here.
    result = subprocess.run([model_gate_port.CHECK_CC, *FLAGS, "-fsyntax-only", "-Xclang", "-ast-dump=json",
                             "-Xclang", f"-ast-dump-filter=func_{model_gate.BASE['A'] >> 16:04X}", unit],
                            cwd=ROOT, capture_output=True)
    text = result.stdout.decode("utf-8", errors="replace")
    decoder, found, at = json.JSONDecoder(), [], 0
    while True:
        at = text.find("{", at)
        if at < 0:
            break
        node, at = decoder.raw_decode(text, at)
        found.append(node)
    return found


AST_LAYOUT = re.compile(r"\*\*\* Dumping AST Record Layout\nType: ([^\n]+)\n\nLayout: <ASTRecordLayout\n\s+Size:(\d+)\n"
                        r"(?:[^\n]*\n)*?\s+FieldOffsets: \[([^\]]*)\]>")
IR_LAYOUT = re.compile(r"\*\*\* Dumping IRgen Record Layout\nRecord: [^\n]*\n((?:[^\n]+\n)*?)\nLayout: <CGRecordLayout\n"
                       r"\s+LLVMType:%(?:struct|union)\.([\w.]+) = ")
FIELD = re.compile(r"^[|`]-FieldDecl 0x[0-9a-f]+ <[^>]*> (?:col|line):\d+(?::\d+)?(?: referenced)? (\w*) '([^']*)'", re.M)


def layouts(unit: str) -> dict[str, tuple[int, dict[str, tuple[int, str]]]]:
    """{record type: (size, {member: (offset, type)})} in bytes, from clang's
    record layouts of the unit (a compile, so every record it uses is laid out)."""
    result = subprocess.run([model_gate_port.CHECK_CC, *FLAGS, "-c", "-o", os.devnull, "-Xclang",
                             "-fdump-record-layouts-simple", unit], cwd=ROOT, capture_output=True, text=True,
                            errors="replace")
    members = {}
    for fields, name in IR_LAYOUT.findall(result.stdout):
        members.setdefault(re.sub(r"\.\d+$", "", name), FIELD.findall(fields))
    found = {}
    for name, size, offsets in AST_LAYOUT.findall(result.stdout):
        name = name.strip()
        offsets = [int(x) for x in offsets.split(",") if x.strip()]
        fields = members.get(name)
        if fields is None or len(fields) != len(offsets):
            continue
        found[name] = (int(size) // 8, {member: (offset // 8, kind) for (member, kind), offset in zip(fields, offsets)
                                        if member})
    return found


# --- values --------------------------------------------------------------------------------

class Tag(tuple):
    """("const",) ("command",) ("work",) ("workptr", offset) ("slot", offset)
    ("tableptr", offset, stride) ("slotword", offset) ("tablefield", offset,
    stride, field offset) ("other",) ("unknownptr",)."""


def tag(*parts):
    return Tag(parts)


CONST, COMMAND, WORK, OTHER, UNKNOWN = tag("const"), tag("command"), tag("work"), tag("other"), tag("unknownptr")


class Module:
    def __init__(self, row: model_gate.Row):
        self.row = row
        self.base = model_gate.BASE["A"]
        self.data_start = row.data_start
        names = model_gate.module_addresses(row, "A")
        self.functions = {name for name in names if name.startswith("func_")}
        self.data = {name: address - self.base for name, address in names.items() if name.startswith("D_")}
        self.layouts: dict[str, tuple[int, dict]] = {}
        self.params: dict[tuple[str, int], set] = collections.defaultdict(set)
        self.locals: dict[tuple[str, str], set] = collections.defaultdict(set)
        self.fields: dict[int, set] = collections.defaultdict(set)    # work-area offset -> value tags
        self.unknown_fields: set = set()
        self.sites: list[dict] = []
        self.changed = True
        self.function = None

    # sizes ------------------------------------------------------------------------
    def size_of(self, kind: str) -> int | None:
        kind = re.sub(r"\b(const|volatile|struct|union|__ptr32|__uptr|signed(?= char))\b", "", kind).strip()
        kind = re.sub(r"\s+", " ", kind)
        if kind.endswith("*"):
            return 4
        array = re.match(r"^(.*?)\s*\[(\d+)\]$", kind)
        if array:
            inner = self.size_of(array.group(1))
            return inner * int(array.group(2)) if inner else None
        if kind in BASIC:
            return BASIC[kind]
        if kind in self.layouts:
            return self.layouts[kind][0]
        return None

    def pointee(self, node) -> str:
        t = node.get("type", {})
        for key in ("desugaredQualType", "qualType"):
            text = t.get(key, "")
            if "*" in text:
                return text[:text.rindex("*")].strip()
        return ""

    def record_of(self, node, arrow: bool) -> str:
        t = node.get("type", {}).get("qualType", "")
        if arrow:
            t = t[:t.rindex("*")] if "*" in t else t
        return re.sub(r"\b(const|volatile|struct|union|__ptr32|__uptr)\b", "", t).strip()

    # evaluation ----------------------------------------------------------------------
    @staticmethod
    def strip(node):
        while node and node.get("kind") in ("ParenExpr",) or (
                node and node.get("kind") == "ImplicitCastExpr" and node.get("castKind") in
                ("IntegralCast", "NoOp", "BitCast")):
            node = (node.get("inner") or [None])[0]
        return node

    def const(self, node) -> int | None:
        node = self.strip(node)
        if node is None:
            return None
        kind = node.get("kind")
        if kind in ("IntegerLiteral", "CharacterLiteral"):
            return int(node.get("value", 0))
        if kind == "CStyleCastExpr":
            return self.const(node["inner"][0])
        if kind == "UnaryOperator" and node.get("opcode") == "-":
            v = self.const(node["inner"][0])
            return None if v is None else -v
        if kind == "BinaryOperator" and node.get("opcode") in ("+", "-", "*", "<<", "|", "&"):
            a, b = self.const(node["inner"][0]), self.const(node["inner"][1])
            if a is None or b is None:
                return None
            return {"+": a + b, "-": a - b, "*": a * b, "<<": a << b, "|": a | b, "&": a & b}[node["opcode"]]
        if kind == "UnaryExprOrTypeTraitExpr" and node.get("name") == "sizeof":
            arg = node.get("argType", {}).get("qualType") or (node.get("inner") or [{}])[0].get("type", {}).get("qualType", "")
            return self.size_of(arg)
        return None

    def rv(self, node) -> set:
        """What a value may come from."""
        if node is None:
            return {OTHER}
        if self.const(node) is not None:
            return {CONST}
        kind = node.get("kind")
        inner = node.get("inner") or []
        if kind == "ImplicitCastExpr":
            cast = node.get("castKind")
            if cast == "LValueToRValue":
                return self.read(self.lv(inner[0]))
            if cast == "ArrayToPointerDecay":
                return self.address(self.lv(inner[0]))
            if cast == "FunctionToPointerDecay":
                return {CONST}
            return self.rv(inner[0])
        if kind in ("ParenExpr", "CStyleCastExpr"):
            return self.rv(inner[0])
        if kind == "DeclRefExpr":
            return self.read(self.lv(node))
        if kind == "UnaryOperator":
            op = node.get("opcode")
            if op == "&":
                return self.address(self.lv(inner[0]))
            if op == "*":
                return self.read(self.lv(node))
            if op in ("++", "--"):
                return self.read(self.lv(inner[0]))
            return self.rv(inner[0])
        if kind in ("MemberExpr", "ArraySubscriptExpr"):
            return self.read(self.lv(node))
        if kind == "BinaryOperator":
            op = node.get("opcode")
            if op in ("+", "-") and "*" in node.get("type", {}).get("qualType", ""):
                return self.pointer_step(node)
            if op == ",":
                return self.rv(inner[1])
            if op == "=":
                return self.assign(inner[0], inner[1])
            if op in ("+", "-"):
                # An address kept in an integer (MODEL_VARIANT_WORD(work, 0x1B74) + 0x20).
                for one, other in ((inner[0], inner[1]), (inner[1], inner[0])):
                    amount = self.const(other)
                    if amount is not None and (op == "+" or one is inner[0]):
                        tags = self.rv(one)
                        if any(t[0] in ("slot", "tableptr", "workptr", "work") for t in tags):
                            return self.step(tags, amount if op == "+" else -amount, 1, {CONST})
            values = self.rv(inner[0]) | self.rv(inner[1])
            if op in ("/", "%"):
                self.site(node, op, inner[1])
            if op in ("<", ">", "<=", ">=", "==", "!=", "&&", "||"):
                return {OTHER}
            return self.arith(values)
        if kind == "CompoundAssignOperator":
            op = node.get("opcode")
            if op in ("/=", "%="):
                self.site(node, op[0], inner[1])
            values = self.read(self.lv(inner[0])) | self.rv(inner[1])
            self.store(self.lv(inner[0]), self.arith(values))
            return self.arith(values)
        if kind == "ConditionalOperator":
            return self.rv(inner[1]) | self.rv(inner[2])
        if kind == "CallExpr":
            self.call(node)
            return {OTHER}
        if kind == "StmtExpr":
            return {OTHER}
        return {OTHER}

    @staticmethod
    def arith(values: set) -> set:
        """A value computed from others: a pointer stops being one."""
        out = set()
        for value in values:
            if value[0] in ("slot", "tableptr", "workptr", "work", "unknownptr"):
                out.add(OTHER)
            else:
                out.add(value)
        return out or {OTHER}

    def pointer_step(self, node) -> set:
        inner = node["inner"]
        pointer, index = (inner[0], inner[1]) if "*" in inner[0].get("type", {}).get("qualType", "") else (inner[1], inner[0])
        size = self.size_of(self.pointee(node)) or 1
        steps = self.rv(pointer)
        amount = self.const(index)
        sign = -1 if node.get("opcode") == "-" else 1
        if amount is None:
            self.site(node, "+", index, steps)
        return self.step(steps, None if amount is None else sign * amount * size, size, self.rv(index))

    def step(self, pointers: set, delta: int | None, size: int, index: set) -> set:
        out = set()
        for p in pointers:
            if p[0] == "slot":
                if delta is not None:
                    out.add(tag("slot", p[1] + delta))
                elif COMMAND in index and index <= {COMMAND, CONST}:
                    out.add(tag("tableptr", p[1], size, 0))
                else:
                    out.add(tag("slotdyn", p[1]))
            elif p[0] == "tableptr":
                out.add(tag("tableptr", p[1], p[2], p[3] + delta) if delta is not None else tag("slotdyn", p[1]))
            elif p[0] in ("work", "workptr"):
                at = p[1] if p[0] == "workptr" else 0
                out.add(tag("workptr", at + delta) if delta is not None else tag("workdyn"))
            else:
                out.add(p)
        return out

    def assign(self, target, value) -> set:
        values = self.rv(value)
        self.store(self.lv(target), values)
        return values

    def store(self, locations: set, values: set) -> None:
        for location in locations:
            if location[0] == "local":
                key = location[1]
                if not values <= self.locals[key]:
                    self.locals[key] |= values
                    self.changed = True
            elif location[0] == "work":
                if not values <= self.fields[location[1]]:
                    self.fields[location[1]] |= values
                    self.changed = True
            elif location[0] == "workdyn":
                if not values <= self.fields[-1]:
                    self.fields[-1] |= values
                    self.changed = True

    def lv(self, node) -> set:
        """What a location may be."""
        kind = node.get("kind")
        inner = node.get("inner") or []
        if kind in ("ParenExpr", "ImplicitCastExpr", "CStyleCastExpr"):
            return self.lv(inner[0]) if inner else {tag("other")}
        if kind == "DeclRefExpr":
            decl = node.get("referencedDecl", {})
            name = decl.get("name", "")
            if decl.get("kind") == "ParmVarDecl":
                return {tag("param", self.function, name)}
            if decl.get("kind") == "VarDecl":
                if name in self.data:
                    return {tag("slotloc", self.data[name])}
                if name in self.function_locals:
                    return {tag("local", (self.function, name))}
                return {tag("global")}
            return {tag("global")}
        if kind == "UnaryOperator" and node.get("opcode") == "*":
            return self.deref(self.rv(inner[0]), node)
        if kind == "MemberExpr":
            arrow = node.get("isArrow", False)
            base = inner[0]
            record = self.layouts.get(self.record_of(base, arrow))
            offset = record[1].get(node.get("name", ""), (None,))[0] if record else None
            places = self.deref(self.rv(base), node) if arrow else self.lv(base)
            return self.member(places, offset)
        if kind == "ArraySubscriptExpr":
            base, index = inner[0], inner[1]
            size = self.size_of(node.get("type", {}).get("qualType", "")) or 1
            amount = self.const(index)
            if amount is None:
                self.site(node, "[]", index, self.rv(base))
            pointers = self.step(self.rv(base), None if amount is None else amount * size, size, self.rv(index))
            return self.deref(pointers, node)
        return {tag("other")}

    def deref(self, pointers: set, node) -> set:
        out = set()
        for p in pointers:
            if p[0] == "slot":
                out.add(tag("slotloc", p[1]))
            elif p[0] == "tableptr":
                out.add(tag("tableloc", p[1], p[2], p[3]))
            elif p[0] == "slotdyn":
                out.add(tag("slotdynloc", p[1]))
            elif p[0] == "work":
                out.add(tag("work", 0))
            elif p[0] == "workptr":
                out.add(tag("work", p[1]))
            elif p[0] == "workdyn":
                out.add(tag("workdyn"))
            elif p[0] in ("const", "other", "command"):
                out.add(tag("global"))
            else:
                out.add(tag("unknownloc"))
        return out

    def member(self, places: set, offset: int | None) -> set:
        out = set()
        for p in places:
            if p[0] == "slotloc":
                out.add(tag("slotloc", p[1] + offset) if offset is not None else tag("slotdynloc", p[1]))
            elif p[0] == "tableloc":
                out.add(tag("tableloc", p[1], p[2], p[3] + offset) if offset is not None else tag("slotdynloc", p[1]))
            elif p[0] == "work":
                out.add(tag("work", p[1] + offset) if offset is not None else tag("workdyn"))
            else:
                out.add(p)
        return out

    def read(self, locations: set) -> set:
        out = set()
        for location in locations:
            kind = location[0]
            if kind == "local":
                out |= self.locals.get(location[1]) or {OTHER}
            elif kind == "param":
                out |= self.params.get((location[1], location[2])) or {OTHER}
            elif kind == "slotloc":
                out.add(tag("slotword", location[1]))
            elif kind == "tableloc":
                out.add(tag("tablefield", location[1], location[2], location[3]))
            elif kind == "slotdynloc":
                out.add(tag("slotdynword", location[1]))
            elif kind == "work":
                out |= self.fields.get(location[1]) or {OTHER}
            elif kind == "workdyn":
                out |= self.fields.get(-1) or {OTHER}
            elif kind == "unknownloc":
                out.add(tag("unknownword"))
            else:
                out.add(OTHER)
        return out

    def address(self, locations: set) -> set:
        out = set()
        for location in locations:
            kind = location[0]
            if kind == "slotloc":
                out.add(tag("slot", location[1]))
            elif kind == "tableloc":
                out.add(tag("tableptr", location[1], location[2], location[3]))
            elif kind == "work":
                out.add(tag("workptr", location[1]))
            elif kind == "slotdynloc":
                out.add(tag("slotdyn", location[1]))
            else:
                out.add(OTHER)
        return out

    def call(self, node) -> None:
        inner = node.get("inner") or []
        callee = self.strip(inner[0]) if inner else None
        name = None
        while callee is not None and callee.get("kind") in ("ImplicitCastExpr", "ParenExpr"):
            callee = (callee.get("inner") or [None])[0]
        if callee is not None and callee.get("kind") == "DeclRefExpr":
            name = callee.get("referencedDecl", {}).get("name")
        arguments = [self.rv(argument) for argument in inner[1:]]
        if name in self.function_params:
            for index, values in enumerate(arguments):
                params = self.function_params[name]
                if index < len(params):
                    key = (name, params[index])
                    if not values <= self.params[key]:
                        self.params[key] |= values
                        self.changed = True

    # sites -----------------------------------------------------------------------------
    def site(self, node, op, operand, base=None) -> None:
        if self.const(operand) is not None:
            return
        where = node.get("range", {}).get("begin", {})
        self.current_sites.append({"op": op, "operand": self.rv(operand), "base": base, "node": node,
                                   "function": self.function})

    # walking ---------------------------------------------------------------------------
    def walk(self, node) -> None:
        kind = node.get("kind")
        if kind in ("BinaryOperator", "CompoundAssignOperator", "CallExpr", "UnaryOperator", "ArraySubscriptExpr",
                    "MemberExpr", "ConditionalOperator", "ImplicitCastExpr", "CStyleCastExpr", "ParenExpr",
                    "DeclRefExpr"):
            self.rv(node)
            return
        if kind == "VarDecl" and node.get("inner"):
            self.store({tag("local", (self.function, node.get("name")))}, self.rv(node["inner"][-1]))
            return
        if kind == "ReturnStmt":
            for child in node.get("inner") or []:
                self.rv(child)
            return
        if kind in ("ForStmt", "WhileStmt", "DoStmt"):
            # A loop counter reaches its bound: `for (i = 0; i < config->count; i++)`
            # makes i an index as good as config->count.
            for child in node.get("inner") or []:
                self.bound(child)
        for child in node.get("inner") or []:
            self.walk(child)

    def bound(self, node) -> None:
        node = self.strip(node) if node else None
        if not node or node.get("kind") != "BinaryOperator":
            return
        op, inner = node.get("opcode"), node.get("inner") or []
        if op in ("&&", "||"):
            for child in inner:
                self.bound(child)
            return
        if op not in ("<", "<=", ">", ">=", "!=") or len(inner) != 2:
            return
        for one, other in ((inner[0], inner[1]), (inner[1], inner[0])):
            one = self.strip(one)
            while one and one.get("kind") == "ImplicitCastExpr":
                one = (one.get("inner") or [None])[0]
            if one and one.get("kind") == "DeclRefExpr" and \
                    one.get("referencedDecl", {}).get("name") in self.function_locals:
                values = {v for v in self.arith(self.rv(other)) if v != CONST}
                if values:
                    self.store({tag("local", (self.function, one["referencedDecl"]["name"]))}, values)


def locate(node, files):
    """file:line of a node (clang's JSON gives the file only when it changes:
    the walk below tracks it)."""
    return node.get("_where")


def annotate(tree: list[dict]) -> None:
    """Give every node the file and line it begins at."""
    state = {"file": None, "line": None}

    def plain(where):
        if "file" in where:
            state["file"] = where["file"].replace("\\", "/")
        if "line" in where:
            state["line"] = where["line"]
        if "col" in where:
            state["col"] = where["col"]
        return state["file"], state["line"], state.get("col")

    def spell(where):
        """clang writes a location's spelling, then its expansion, each with
        only what changed since the location written before: follow both in
        that order; the place is the expansion's (where a macro was used)."""
        if not where:
            return None
        if "spellingLoc" in where or "expansionLoc" in where:
            place = plain(where["spellingLoc"]) if "spellingLoc" in where else None
            if "expansionLoc" in where:
                place = plain(where["expansionLoc"])
            return place
        return plain(where)

    def visit(node):
        spell(node.get("loc"))
        if node.get("range"):
            node["_where"] = spell(node["range"].get("begin"))
            spell(node["range"].get("end"))
        for child in node.get("inner") or []:
            visit(child)

    for node in tree:
        visit(node)


def scan_row(row: model_gate.Row, entry_command_param: int = 1) -> dict:
    """The sites of one native candidate shape, classified."""
    module = Module(row)
    units = list(model_gate.source_patterns(row, "A"))
    trees = []
    for unit in units:
        module.layouts.update(layouts(unit))
        tree = ast(unit)
        annotate(tree)
        trees.append((unit, tree))
    functions = {}
    for unit, tree in trees:
        for node in tree:
            if node.get("kind") == "FunctionDecl" and any(c.get("kind") == "CompoundStmt" for c in node.get("inner") or []):
                where = node.get("_where") or (None, None)
                if where[0] and model_gate_port.NATIVE in posixpath.normpath(where[0]):
                    functions[node["name"]] = node
    module.function_params = {name: [c.get("name") for c in node.get("inner") or [] if c.get("kind") == "ParmVarDecl"]
                              for name, node in functions.items()}
    entry = f"func_{model_gate.BASE['A'] + 4:08X}"
    if entry in module.function_params:
        params = module.function_params[entry]
        if params:
            module.params[(entry, params[0])] = {WORK}
        if len(params) > entry_command_param:
            module.params[(entry, params[entry_command_param])] = {COMMAND}
    for name, params in module.function_params.items():
        for param in params:
            module.params.setdefault((name, param), set())
    # A function of the module no other calls by name (a callback, or code
    # the module never reaches) is taken to get the work area first, as the
    # entry's helpers do, and anything after: its sites are scanned too.
    called = set()

    def calls(node):
        if node.get("kind") == "DeclRefExpr" and node.get("referencedDecl", {}).get("kind") == "FunctionDecl":
            called.add(node["referencedDecl"].get("name"))
        for child in node.get("inner") or []:
            calls(child)
    for name, node in functions.items():
        for child in node.get("inner") or []:
            if child.get("kind") == "CompoundStmt":
                calls(child)
    for name, params in module.function_params.items():
        if name != entry and name not in called and params:
            module.params[(name, params[0])] = {WORK}
            for param in params[1:]:
                module.params[(name, param)] = {OTHER}
    rounds = 0
    while module.changed and rounds < 12:
        module.changed = False
        rounds += 1
        module.sites = []
        for name, node in functions.items():
            module.function = name
            module.function_locals = set()

            def collect(n):
                if n.get("kind") == "VarDecl" and n.get("storageClass") not in ("extern", "static"):
                    module.function_locals.add(n.get("name"))
                for c in n.get("inner") or []:
                    collect(c)
            collect(node)
            module.current_sites = []
            for child in node.get("inner") or []:
                if child.get("kind") == "CompoundStmt":
                    module.walk(child)
            module.sites += module.current_sites
    return {"module": module, "rounds": rounds}


def classify(row: model_gate.Row, common: int, data_start: int) -> dict:
    """The row's sites as (a) covered, (b) command table, (c) per record, (d) unclassified,
    and the span and command_table they ask for."""
    result = scan_row(row)
    module = result["module"]
    seen = {}
    tables = collections.Counter()
    for site in module.sites:
        where = site["node"].get("_where") or ("?", 0)
        path = posixpath.normpath(where[0] or "?")
        if path.startswith(str(ROOT).replace("\\", "/")):
            path = path[len(str(ROOT)) + 1:]
        key = (path, where[1], where[2] if len(where) > 2 else 0, site["op"])
        values = set(site["operand"])
        if site["op"] in ("[]", "+") and site.get("base"):
            values |= {v for v in site["base"] if v[0] in ("slotdyn", "unknownptr")}
        entry = seen.setdefault(key, {"where": f"{path}:{where[1]}", "op": site["op"], "values": set(),
                                      "slot_base": False})
        entry["values"] |= values
        if any(v[0] in ("slot", "tableptr") for v in site.get("base") or ()):
            entry["slot_base"] = True
    out = {"a": [], "b": [], "c": [], "d": [], "picks": [], "slot_words": set(), "tables": collections.Counter()}
    for entry in seen.values():
        values = entry["values"]
        kinds = {v[0] for v in values}
        if kinds <= {"const", "other", "work"}:
            continue
        words = [v[1] for v in values if v[0] == "slotword" and v[1] >= data_start]
        table_fields = [v for v in values if v[0] == "tablefield" and v[1] >= data_start]
        bad = kinds & {"slotdynword", "unknownword", "slotdyn", "unknownptr", "workdyn"}
        picks = entry["op"] in ("[]", "+") and entry.get("slot_base")
        if "command" in kinds and picks and kinds <= {"command", "const", "other"}:
            out["picks"].append(entry)   # the command picking a table entry: the command table
            continue
        if "command" in kinds:
            bad = bad | {"command"}
        rodata_only = all(v[1] < data_start for v in values if v[0] in ("slotword", "tablefield"))
        text = sorted(" ".join(str(x) if not isinstance(x, int) else hex(x) for x in v) for v in values
                      if v[0] not in ("const", "other"))
        entry["text"] = ", ".join(text)
        if bad:
            out["d"].append(entry)
        elif rodata_only and not (kinds & {"slotword", "tablefield"} and (words or table_fields)):
            continue
        elif any(w + 4 > common for w in words) or any(t[1] > common for t in table_fields):
            out["c"].append(entry)
        elif table_fields:
            for t in table_fields:
                out["tables"][(t[1], t[2])] += 1
            out["b"].append(entry)
        else:
            out["slot_words"].update(words)
            out["a"].append(entry)
    # the work-area field that keeps the address of the entry the command picked
    fields = [offset for offset, values in module.fields.items()
              if offset >= 0 and any(v[0] == "tableptr" for v in values)]
    out["entry_fields"] = sorted(set(fields))
    out["command_fields"] = sorted(offset for offset, values in module.fields.items()
                                   if offset >= 0 and COMMAND in values)
    out["table_pointers"] = sorted({(v[1], v[2], offset) for offset, values in module.fields.items() for v in values
                                    if v[0] == "tableptr"})
    return out


# --- the report ------------------------------------------------------------------------------

def propose(row: model_gate.Row, common: int, commands: dict | None, compile_errors: list[str]) -> dict:
    """What a shape's row should be, from its scan: backend, span,
    command_table, note, and the sites behind them."""
    out = classify(row, common, row.data_start)
    data_start = row.data_start
    result = {"shape": row.shape, "data_start": data_start, "common": common, "sites": {
        k: [{"where": e["where"], "op": e["op"], "from": e["text"]} for e in out[k]] for k in "abcd"},
        "picks": [e["where"] for e in out["picks"]], "slot_words": sorted(out["slot_words"]),
        "tables": [[t, s, n] for (t, s), n in sorted(out["tables"].items())],
        "table_pointers": out["table_pointers"], "command_fields": out["command_fields"]}
    reasons = []
    span = max([data_start] + [w + 4 for w in out["slot_words"]])
    command_table = "none"
    tables = sorted({(t, s) for t, s in out["tables"]} | {(t, s) for t, s, f in out["table_pointers"]})
    fields = sorted({f for t, s, f in out["table_pointers"] if f >= 0})
    if compile_errors:
        reasons.append("does not compile against the port's headers: " + "; ".join(compile_errors[:3]))
    if out["d"]:
        reasons.append(f"{len(out['d'])} unclassified site(s)")
    if out["tables"]:
        # A divisor or index comes from an entry the command picks: the
        # command table bounds it (model_modules.c), which needs the entry's
        # address in a work field for the updates.
        if len(tables) != 1:
            reasons.append(f"{len(tables)} command tables {[(hex(t), hex(s)) for t, s in tables]}")
        elif len(fields) != 1:
            kept = out["command_fields"]
            reasons.append(f"the entry the command picks is kept in {len(fields)} work fields {[hex(f) for f in fields]}"
                           + (f" (the setup keeps the command at work +{', '.join(hex(f) for f in kept)}: the gate "
                              "could bound the updates by that word)" if kept else
                              " and the command in none: the updates cannot be told from the work area"))
        else:
            table, stride = tables[0]
            count = (commands or {}).get(row.shape)
            if count is None:
                reasons.append("no retail commands for the table's count (the oracle's traces)")
            else:
                reads = [int(v.split()[3], 16) for e in out["b"] for v in e["text"].split(", ")
                         if v.startswith("tablefield")]
                end = table + (count - 1) * stride + max([stride - 4] + reads) + 4
                if end > model_gate.IMAGE_SIZE:
                    reasons.append(f"retail commands up to {count - 1} pick entries past the image "
                                   f"(table 0x{table:X}, 0x{stride:X} bytes): the C reduces the command "
                                   "(command % 100), which command_table cannot say")
                else:
                    span = max(span, end)
                    command_table = f"0x{table:X}:0x{stride:X}:{count}:0x{fields[0]:X}"
    span = (span + 3) & ~3   # a hash of whole words
    if out["c"]:
        backend = "keep"
        note = "per-record words as divisor or index: " + ", ".join(e["where"] for e in out["c"][:4])
    elif reasons:
        backend = "exception"
        note = "; ".join(reasons)
    else:
        backend = "native"
        widened = []
        if out["slot_words"]:
            widened.append("slot words " + ", ".join(f"0x{w:X}" for w in sorted(out["slot_words"])))
        if command_table != "none":
            widened.append(f"the command table {command_table.split(':')[0]} ({command_table.split(':')[2]} entries)")
        note = ("span widened over " + " and ".join(widened) + " the C divides by or indexes with") if span > data_start else ""
    if backend == "native" and span > common:
        backend = "keep"
        note = (f"the span 0x{span:X} the C needs is past where the retail images agree (0x{common:X}): "
                "words that differ per record")
    result.update(backend=backend, span=span, command_table=command_table, note=note)
    return result


def _one(job):
    shape, common, commands, errors = job
    row = {r.shape: r for r in model_gate.load_table()}[shape]
    try:
        return propose(row, common, commands, errors)
    except Exception as error:   # a scan that fails is an exception, with the reason
        return {"shape": shape, "backend": "exception", "note": f"scan failed: {error!r}", "span": None,
                "command_table": "", "sites": {"a": [], "b": [], "c": [], "d": []}}


def scan_all(shapes: list[int], disc: Path, commands: dict | None, errors: dict[int, list[str]], jobs: int) -> list[dict]:
    import concurrent.futures
    found = model_gate.read_shapes(disc)
    work = [(shape, min(found[shape].common("A"), found[shape].common("B")), commands, errors.get(shape, []))
            for shape in shapes]
    with concurrent.futures.ProcessPoolExecutor(jobs) as pool:
        return list(pool.map(_one, work))


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("shapes", nargs="*", type=int, help="default: every row whose C is listed")
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"))
    parser.add_argument("--commands", type=Path, help="JSON {shape: retail command count} (the oracle's traces)")
    parser.add_argument("--compile-errors", type=Path, help="a model_gate.py port log: its 'does not compile' lines")
    parser.add_argument("--out", type=Path, required=True, help="the report (.json; a .txt beside it)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    arguments = parser.parse_args()
    rows = {row.shape: row for row in model_gate.load_table()}
    listed = model_gate.listed_sources()
    shapes = arguments.shapes or sorted(shape for shape, row in rows.items() if row.module("A") in listed)
    commands = ({int(k): v for k, v in json.loads(arguments.commands.read_text()).items()}
                if arguments.commands else None)
    errors = collections.defaultdict(list)
    if arguments.compile_errors:
        failing = collections.defaultdict(list)
        for line in arguments.compile_errors.read_text(encoding="utf-8").splitlines():
            if "does not compile: " in line:
                where = line.split("does not compile: ", 1)[1]
                failing[where.split(":", 1)[0]].append(where)
        for shape in shapes:
            files = {model_gate.os.path.relpath(path, ROOT).replace("\\", "/")
                     for unit in model_gate.source_patterns(rows[shape], "A")
                     for path in model_gate.port_closure(ROOT / unit)}
            for path, lines in failing.items():
                if path in files:
                    errors[shape] += lines
    results = scan_all(shapes, arguments.disc, commands, errors, arguments.jobs)
    arguments.out.write_text(json.dumps(results, indent=1, default=list), encoding="utf-8")
    counts = collections.Counter(r["backend"] for r in results)
    lines = [f"{len(results)} shapes: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))]
    for r in sorted(results, key=lambda r: r["shape"]):
        span = f"0x{r['span']:X}" if r.get("span") is not None else "-"
        lines.append(f"{r['shape']:>4} {r['backend']:<9} span {span:<7} command_table {r['command_table'] or '-':<24} {r['note']}")
    lines.append("")
    lines.append("unclassified sites (d), for review:")
    for r in sorted(results, key=lambda r: r["shape"]):
        for e in r["sites"]["d"]:
            lines.append(f"  shape {r['shape']}: {e['where']} {e['op']} from {e['from']}")
    arguments.out.with_suffix(".txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(lines[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
