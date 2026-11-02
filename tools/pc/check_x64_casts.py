#!/usr/bin/env python3
"""Check (and fix) the casts a native 64-bit build gets wrong without a word.

G32 (src/port_ptr.h, `make check-g32`) covers declarations. Three kinds of
cast in game code are just as wrong in the 64-bit build
(build_game32.py --target windows-x64), and neither compiler nor
check_g32.py says so:

  signed   `(T *)i` from a signed integer. A native pointer sign-extends
           0x80xxxxxx (guest RAM) and 0xB0xxxxxx (the game stack) into
           kernel space. Write `(T *G32)i`: it keeps the low 32 bits and
           zero-extends where it is used.
  ptrptr   `(T **)p` over a table of guest pointers: a native T ** steps 8
           bytes over 4-byte slots. Write `(T *G32 *)p`. A cast of a native
           table (a local array of pointers, the address of a pointer
           variable) is left alone.
  widened  `(T *)(i + sizeof(x))`: the sum is unsigned 64-bit, but the
           signed 32-bit `i` in it was sign-extended to get there, so the
           native pointer lands in the top of the address space when `i`
           holds an address with bit 31 set (guest RAM, the game stack, or
           arm64's game library at 0xC0000000). Keep the arithmetic 32 bits
           wide (`(u32)sizeof(x)`, or the whole offset `(u32)(...)`) or cast
           to `T *G32`. Not fixed by --fix.

Every game unit, and the port's SDK and platform code (src/pc/sdk,
src/pc/platform, which build guest pointers from integers too; the window
backends aside), is parsed by the 64-bit build's clang (x86_64-w64-mingw32,
the build's own flags for each) with its JSON AST, about two minutes. A
cast inside a macro is reported, and fixed, at the macro's definition.

--fix inserts G32 into each cast it can: not a function-pointer cast (a
call through one needs CALL32 as well) nor a typedef'd pointer, which it
lists for the author. Parameters and locals that then receive a G32 table
are reported by the compiler ("changes address space of nested pointer").
Exit status 1 while anything is left."""
import argparse, collections, concurrent.futures, glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.argv, ARGV = ["build_game32.py", "--target", "windows-x64"], sys.argv
sys.path.insert(0, HERE)
import build_game32 as build  # noqa: E402  (the flags and the unit list)
sys.argv = ARGV

SIGNED = re.compile(r"^(const )?(volatile )?(signed )?(char|short|int|long|long long)$")
AST = ["-fsyntax-only", "-ferror-limit=0", "-Xclang", "-ast-dump=json"]
FLAGS = [f for f in build.CFLAGS if f not in ("-g", "-gcodeview") and not f.startswith("-mretpoline")] + AST
NATIVE_FLAGS = [f for f in build.NATIVE_CFLAGS if f not in ("-g", "-gcodeview") and not f.startswith("-mretpoline")] + \
               ["-w"] + AST
# The port's code that is scanned besides the game's.
PORT = ("src/pc/sdk/", "src/pc/platform/")
PREFIX = ROOT.replace("\\", "/") + "/"


def qual(node):
    return node.get("type", {}).get("qualType", "")


def desugared(node):
    t = node.get("type", {})
    return t.get("desugaredQualType", t.get("qualType", ""))


def strip(node):
    while node and node.get("kind") in ("ImplicitCastExpr", "ParenExpr"):
        node = (node.get("inner") or [None])[0]
    return node


WIDE = re.compile(r"^(const )?(volatile )?(unsigned )?long long$")   # LLP64: `long` is 4 bytes


def variable(node):
    """Whether an expression reads something other than constants."""
    if node.get("kind") == "DeclRefExpr":
        return node.get("referencedDecl", {}).get("kind") != "EnumConstantDecl"
    if node.get("kind") in ("UnaryExprOrTypeTraitExpr", "OffsetOfExpr"):
        return False
    return any(variable(child) for child in node.get("inner", []) or [])


def widened(node):
    """A 64-bit sum that sign-extends a signed 32-bit operand: the arithmetic
    of `(T *)(i + sizeof(x))`, which is unsigned 64-bit, not signed, so the
    `signed` kind misses it, while `i` is still sign-extended first. Only
    through the arithmetic itself: an explicit cast is the author's."""
    if not node or not WIDE.match(desugared(node)):
        return False
    kind = node.get("kind")
    if kind == "ImplicitCastExpr" and node.get("castKind") == "IntegralCast":
        operand = (node.get("inner") or [{}])[0]
        return bool(SIGNED.match(desugared(operand))) and not WIDE.match(desugared(operand)) and variable(operand)
    if kind in ("ParenExpr", "BinaryOperator", "ImplicitCastExpr"):
        return any(widened(child) for child in node.get("inner", []) or [])
    return False


def native_ptrptr(text):
    """A pointer to a native pointer: `T **` or `T *[N]`, no __ptr32 level."""
    text = text.replace(" ", "")
    return "__ptr32" not in text and bool(re.search(r"\*\*|\*\[", text)) and "(*" not in text


class Locations:
    """clang's JSON AST repeats a file or line only when it changes."""

    def __init__(self):
        self.file = self.line = None

    def spelling(self, where):
        if not where:
            return None
        if "spellingLoc" in where or "expansionLoc" in where:
            spelled = self.spelling(where["spellingLoc"]) if "spellingLoc" in where else None
            if "expansionLoc" in where:
                self.spelling(where["expansionLoc"])
            return spelled
        if "file" in where:
            self.file = where["file"].replace("\\", "/")
        if "line" in where:
            self.line = where["line"]
        return self.file, self.line, where.get("col"), where.get("offset")


def scan(unit):
    flags = NATIVE_FLAGS if unit.startswith(PORT) else FLAGS
    result = subprocess.run([build.CC, *flags, unit], cwd=ROOT, capture_output=True)
    try:
        tree = json.loads(result.stdout)
    except ValueError:
        return unit, [], result.stderr.decode(errors="replace").strip().splitlines()[-1:]
    locations, found = Locations(), []

    def walk(node):
        locations.spelling(node.get("loc"))
        begin = None
        if node.get("range"):
            begin = locations.spelling(node["range"].get("begin"))
            locations.spelling(node["range"].get("end"))
        kind = node.get("kind")
        if kind in ("CStyleCastExpr", "ImplicitCastExpr") and begin and begin[0]:
            path = begin[0][len(PREFIX):] if begin[0].startswith(PREFIX) else begin[0]
            if path.startswith("src/") and (not path.startswith("src/pc/") or path.startswith(PORT)):
                operand = (node.get("inner") or [{}])[0]
                kinds = []
                # Annotated when the OUTERMOST level is __ptr32: `(T *G32 *)i`
                # still sign-extends. The sugared spelling keeps the levels in
                # place; a G32 function pointer's is `(*__ptr32 __uptr)(...)`.
                outer = qual(node).rstrip()
                if node.get("castKind") == "IntegralToPointer" and \
                        not re.search(r"(__ptr32|__uptr)$", outer) and \
                        not re.match(r"^[^(]*\(\*\s*(__ptr32|__uptr)", outer) and \
                        (strip(operand) or {}).get("kind") != "IntegerLiteral" and SIGNED.match(desugared(operand)):
                    kinds.append("signed")
                elif node.get("castKind") == "IntegralToPointer" and \
                        not re.search(r"(__ptr32|__uptr)$", outer) and \
                        not re.match(r"^[^(]*\(\*\s*(__ptr32|__uptr)", outer) and widened(operand):
                    kinds.append("widened")
                target = desugared(node).replace(" ", "")
                # Annotated when __ptr32 is on a level inside the outermost
                # one (the sugared spelling keeps the levels in place).
                inner = re.sub(r"(__ptr32|__uptr)+$", "", qual(node).replace(" ", ""))
                if kind == "CStyleCastExpr" and "(*" not in target and "__ptr32" not in inner and \
                        re.search(r"\*\*", re.sub(r"__ptr32|__uptr", "", target)) and \
                        not native_ptrptr(desugared(operand)):
                    kinds.append("ptrptr")
                if kinds:
                    found.append({"path": path, "line": begin[1], "col": begin[2], "offset": begin[3],
                                  "kind": kind, "kinds": kinds, "type": qual(node)})
        for child in node.get("inner", []) or []:
            walk(child)

    for child in tree.get("inner", []):
        walk(child)
    return unit, found, None


def units():
    found = glob.glob("src/game/*.c", root_dir=ROOT) + glob.glob("src/pc/game/*.c", root_dir=ROOT)
    native = {source.replace("\\", "/") for source in build.NATIVE}
    found += [path for path in native if path.startswith(PORT)]
    for _, pattern, _, _ in build.MODULES:
        for one in ((pattern,) if isinstance(pattern, str) else pattern):
            found += glob.glob(one, root_dir=ROOT)
    return sorted({path.replace("\\", "/") for path in found})


def fix(sites):
    """Insert G32 into the casts at `sites`; return those it could not."""
    left, by_file = [], collections.defaultdict(list)
    for site in sites:
        (by_file[site["path"]] if site["kind"] == "CStyleCastExpr" and site["offset"] is not None and
         "widened" not in site["kinds"] else left).append(site)
    for path, entries in by_file.items():
        with open(os.path.join(ROOT, path), "rb") as handle:
            raw = bytearray(handle.read())
        for site in sorted(entries, key=lambda s: -s["offset"]):
            at, depth, end = site["offset"], 0, None
            for i in range(at, min(len(raw), at + 200)):
                depth += {b"("[0]: 1, b")"[0]: -1}.get(raw[i], 0)
                if depth == 0:
                    end = i
                    break
            inner = raw[at + 1:end].decode(errors="replace") if raw[at:at + 1] == b"(" and end else ""
            stars = [i for i, c in enumerate(inner) if c == "*"]
            if not stars or "(" in inner or "G32" in inner:
                left.append(site)
                continue
            wanted = ({stars[-1]} if "signed" in site["kinds"] else set()) | \
                     (set(stars[:-1]) if "ptrptr" in site["kinds"] else set())
            text = list(inner)
            for i in sorted(wanted, reverse=True):
                text.insert(i + 1, "G32 " if i + 1 < len(inner) and inner[i + 1] == "*" else "G32")
            raw[at + 1:end] = "".join(text).replace("G32 )", "G32)").encode()
        with open(os.path.join(ROOT, path), "wb") as handle:
            handle.write(bytes(raw))
    return left


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fix", action="store_true", help="insert G32 where a cast can take it")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    options = parser.parse_args()
    sites, unreadable = {}, []
    with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
        for unit, found, error in pool.map(scan, units()):
            if error is not None:
                unreadable.append(f"{unit}: {' '.join(error)}")
            for site in found:
                sites.setdefault((site["path"], site["offset"], site["line"], site["col"]), site)
    findings = sorted(sites.values(), key=lambda s: (s["path"], s["line"] or 0, s["col"] or 0))
    if options.fix and findings:
        left = fix(findings)
        print(f"check-x64-casts: {len(findings) - len(left)} cast(s) given G32; {len(left)} left to do by hand")
        findings = left
    for site in findings:
        print(f"{site['path']}:{site['line']}:{site['col']}: {'/'.join(site['kinds'])} cast to {site['type']}")
    for line in unreadable:
        print(f"check-x64-casts: could not parse {line}", file=sys.stderr)
    print(f"check-x64-casts: {len(findings)} site(s) left, {len(unreadable)} unit(s) unreadable", file=sys.stderr)
    return 1 if findings or unreadable else 0


if __name__ == "__main__":
    sys.exit(main())
