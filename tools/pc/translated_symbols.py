"""The translated Android game's symbol table, version 2
(symbols/<buildid>.txt; src/pc/guest/translated_state_symbols.c reads it).

libgame.so is linked at 0, so every address is an offset into the image.
One row per function, data object and allocated section, by offset:

    # memories-symbols 2 <image size>
    <offset> <size> <kind> <hash> <gap> <gap hash> <name>

kind is F (code), D (data), or S / X (the start of a data / code section:
its gap runs to the section's first symbol). hash is the symbol's content, gap and gap hash
the bytes after it up to the next row or the end of its section. Words
that the dynamic linker fills (.rela.dyn) are left out of every hash, and
so are the fields of an instruction that depend on where things are: the
26-bit offset of a B/BL leaving the function, ADRP's page, the low 12 bits
an ADD/LDR/STR adds to a register ADRP set, ADR/LDR-literal/CBZ/TBZ/B.cond
offsets leaving the function. A function or object with the same hash
under the same name is the same code or data at another place.

A local symbol's name is qualified by its translation unit
("unit.c:name"), from the STT_FILE entry before it, so two units' statics
are never mistaken for each other; a name that is still not unique is not
used for carrying anything (the reader refuses it).

Run alone (python tools/pc/translated_symbols.py libgame.so out.txt) for
inspection."""
import hashlib
import struct
import sys


def sections(data):
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    rows = []
    for i in range(shnum):
        name, kind, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, shoff + i * shentsize)
        rows.append(dict(name=name, type=kind, flags=flags, addr=addr, offset=offset, size=size, link=link,
                         entsize=entsize))
    names = rows[shstrndx]
    for row in rows:
        end = data.index(b"\0", names["offset"] + row["name"])
        row["name"] = data[names["offset"] + row["name"]:end].decode()
    return rows


SHF_ALLOC, SHF_EXECINSTR, SHT_NOBITS, SHT_SYMTAB, SHT_RELA = 2, 4, 8, 2, 4


def masked_code(code, start, end):
    """The instruction words with their position-dependent fields zeroed."""
    words = list(struct.unpack_from(f"<{len(code) // 4}I", code))
    adrp = set()
    for index, word in enumerate(words):
        if word & 0x9F000000 == 0x90000000:              # ADRP
            adrp.add(word & 31)
            words[index] = word & ~0x60FFFFE0
    for index, word in enumerate(words):
        at = start + index * 4

        def leaves(offset):
            return not start <= at + offset < end

        if word & 0x7C000000 == 0x14000000:              # B, BL
            offset = (word & 0x03FFFFFF) << 2
            offset -= (1 << 28) if offset & (1 << 27) else 0
            if leaves(offset):
                words[index] = word & ~0x03FFFFFF
        elif word & 0x9F000000 == 0x10000000 or word & 0x3B000000 == 0x18000000 or \
                word & 0x7E000000 == 0x34000000 or word & 0xFF000010 == 0x54000000:
            # ADR, LDR (literal), CBZ/CBNZ, B.cond: imm19 (ADR: immhi + immlo)
            if word & 0x9F000000 == 0x10000000:
                offset = ((word >> 5) & 0x7FFFF) << 2 | (word >> 29) & 3
                offset -= (1 << 21) if offset & (1 << 20) else 0
                mask = 0x60FFFFE0
            else:
                offset = ((word >> 5) & 0x7FFFF) << 2
                offset -= (1 << 21) if offset & (1 << 20) else 0
                mask = 0x00FFFFE0
            if leaves(offset):
                words[index] = word & ~mask
        elif word & 0x7E000000 == 0x36000000:            # TBZ/TBNZ
            offset = ((word >> 5) & 0x3FFF) << 2
            offset -= (1 << 16) if offset & (1 << 15) else 0
            if leaves(offset):
                words[index] = word & ~0x0007FFE0
        elif (word & 0x7F000000 == 0x11000000 or word & 0x3B000000 == 0x39000000) and (word >> 5) & 31 in adrp:
            words[index] = word & ~0x003FFC00                # ADD/LDR/STR #lo12 off an ADRP register
    return struct.pack(f"<{len(words)}I", *words)


def table(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{path}: not an ELF64 file")
    rows = sections(data)
    symtab = next(row for row in rows if row["type"] == SHT_SYMTAB)
    strtab = rows[symtab["link"]]
    relocated = set()
    for row in rows:
        if row["type"] == SHT_RELA and row["flags"] & SHF_ALLOC:
            for at in range(row["offset"], row["offset"] + row["size"], 24):
                relocated.add(struct.unpack_from("<Q", data, at)[0])
    allocated = [row for row in rows if row["flags"] & SHF_ALLOC and row["size"]]
    image_size = max(row["addr"] + row["size"] for row in allocated)

    def section_of(address):
        return next((row for row in allocated if row["addr"] <= address < row["addr"] + row["size"]), None)

    def content(start, size, section, code):
        if section["type"] == SHT_NOBITS:
            return b"nobits %d" % size
        raw = bytearray(data[section["offset"] + start - section["addr"]:
                             section["offset"] + start - section["addr"] + size])
        for word in range(start & ~7, start + size, 8):
            if word in relocated:
                for byte in range(max(word, start), min(word + 8, start + size)):
                    raw[byte - start] = 0
        if code and len(raw) % 4 == 0:
            raw = masked_code(bytes(raw), start, start + size)
        return bytes(raw)

    unit = ""
    found = []
    for at in range(symtab["offset"], symtab["offset"] + symtab["size"], 24):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, at)
        name = data[strtab["offset"] + name:data.index(b"\0", strtab["offset"] + name)].decode()
        kind, bind = info & 15, info >> 4
        if kind == 4:                                    # STT_FILE
            unit = name
            continue
        if kind not in (1, 2) or not size or not shndx or shndx >= 0xFF00 or not name:
            continue
        section = section_of(value)
        if not section:
            continue
        code = bool(section["flags"] & SHF_EXECINSTR)
        qualified = f"{unit}:{name}" if bind == 0 and unit else name
        found.append([value, size, "F" if code else "D", qualified, section])
    for row in allocated:
        found.append([row["addr"], 0, "X" if row["flags"] & SHF_EXECINSTR else "S", "section:" + row["name"], row])
    found.sort(key=lambda row: (row[0], row[2] not in "SX"))
    lines = [f"# memories-symbols 2 {image_size:x}\n"]
    for index, (start, size, kind, name, section) in enumerate(found):
        end_section = section["addr"] + section["size"]
        following = [row[0] for row in found[index + 1:index + 64] if row[0] >= start + size]
        gap_end = min([end_section] + [value for value in following if value < end_section])
        gap_start = start + size
        own = hashlib.sha256(content(start, size, section, kind == "F") if size else b"").hexdigest()[:16]
        gap = max(0, gap_end - gap_start)
        gap_hash = hashlib.sha256(content(gap_start, gap, section, False) if gap else b"").hexdigest()[:16]
        lines.append(f"{start:x} {size:x} {kind} {own} {gap:x} {gap_hash} {name}\n")
    return "".join(lines)


if __name__ == "__main__":
    text = table(sys.argv[1])
    with open(sys.argv[2], "w", newline="\n") as handle:
        handle.write(text)
