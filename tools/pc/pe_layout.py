"""Whether a PE image's sections follow each other in memory, as Windows
needs: build_game32.py checks every Windows link, package.py the stripped
exe, test_package.py the archives' exes.

The PE format wants an image's sections "in ascending order and adjacent",
and Windows refuses an image with a hole (CreateProcess: ERROR_BAD_EXE_FORMAT,
193). llvm-strip makes one when lld reserved more header space than the
sections left need: it shrinks SizeOfHeaders and moves no section. Master's
model modules did that (notes/pc-build.md, "No empty gated-module sections").
The offsets read here are PE32's and PE32+'s alike."""
import struct

PAGE = 0x1000


def pe_layout_gaps(image):
    """Where the sections do not follow each other: the first not where the
    headers end (SizeOfHeaders rounded up to SectionAlignment), or one not
    where the one before it ends (its VirtualSize, or SizeOfRawData when that
    is 0, rounded up). A SectionAlignment below a page is refused: Windows
    maps such an image another way (file offsets as addresses), which this
    does not check, and no build here makes one."""
    header = struct.unpack_from("<I", image, 0x3C)[0]
    count, = struct.unpack_from("<H", image, header + 6)
    optional, = struct.unpack_from("<H", image, header + 20)
    alignment, = struct.unpack_from("<I", image, header + 24 + 32)
    end, = struct.unpack_from("<I", image, header + 24 + 60)          # SizeOfHeaders
    if alignment < PAGE:
        return [f"its SectionAlignment is {alignment:#x}, below a page ({PAGE:#x}), a layout this check does not know"]
    previous, gaps = "the headers", []
    for index in range(count):
        at = header + 24 + optional + 40 * index
        name = image[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
        size, address, raw = struct.unpack_from("<III", image, at + 8)
        expected = -(-end // alignment) * alignment
        if address != expected:
            gaps.append(f"{name} is at {address:#x}, not at {expected:#x} right after {previous}")
        end, previous = address + (size or raw), name
    return gaps


def link_layout_problems(image):
    """What in a linked image stripping could turn into a hole, besides
    pe_layout_gaps: headers over one SectionAlignment. llvm-strip leaves
    fewer headers, which may then end a page or more before the first
    section; within one page they still end where it starts."""
    problems = pe_layout_gaps(image)
    header = struct.unpack_from("<I", image, 0x3C)[0]
    alignment, = struct.unpack_from("<I", image, header + 24 + 32)
    headers, = struct.unpack_from("<I", image, header + 24 + 60)
    if alignment >= PAGE and headers > alignment:
        problems.append(f"its headers take {headers:#x} bytes, more than one SectionAlignment ({alignment:#x}): "
                        "the stripped exe would have a hole before its first section")
    return problems
