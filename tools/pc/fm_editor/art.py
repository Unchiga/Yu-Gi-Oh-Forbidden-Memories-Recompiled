"""Card art: the pictures the disc holds for each card, and a mod's own.

A card's art record is seven sectors of WA_MRG.MRG from sector 722
(func_800289BC; src/pc/cards/art.h has the layout): the 102x96 picture at
one byte a pixel through its 256-entry palette (+0x2640), the 96x14 name
plate at four bits a pixel (+0x2840, inks 0 clear, 1 darkest to 7
faintest, drawn subtractively over the card's gold), and the thumbnail
block. The duel reads the 40x32 thumbnail from the card's own sector n-1
(64-entry palette at +0x500): the hand and the field draw it.

A mod replaces them two ways, and the editor uses one per card:

* a retail card's picture and thumbnail: a texture pack (mod.json
  "textures", notes/modding.md), an entry per image keyed by where the words
  are on the disc, exactly as tools/pc/extract_images.py and
  hd_assets_pack.py address them. A pack image may be larger than the
  texture: Internal 2x and 4x draw it at its own resolution.
* a card the mod adds (a copy) has the disc offsets of its base, so a pack
  cannot tell them apart: its picture and thumbnail are the entry's "art"
  and "thumbnail" PNGs (cards.c), made into 102x96 and 40x32 at 255 and 63
  colours when the game starts; one bigger than that is also drawn at its
  own resolution at Internal 2x and 4x, as a pack's is.
* the name plate of any card: the entry's "title" PNG (a retail card gets a
  "replace" entry for it).

A retail card whose "replace" entry has "art" gets that drawn over the
record and reported written, which hides a pack's picture of it: importing
a picture for a retail card moves it to the pack.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from . import map_art, pngio
from .gamedata import CARD_COUNT
from .pngio import Image

SECTOR = 2048
ARCHIVE = "WA_MRG.MRG"
ART_SECTOR = 722               # the first card's art record
ART_SECTORS = 7
ART_CLUT = 0x2640
TITLE_PIXELS = 0x2840
THUMB_CLUT = 0x500             # in the card's own sector

PARTS = ("art", "thumbnail", "title")
LABELS = {"art": "Picture", "thumbnail": "Thumbnail", "title": "Name plate"}
SIZES = {"art": (102, 96), "thumbnail": (40, 32), "title": (96, 14)}
MAX_SCALE = 4                  # stored at most 4x: View > Internal 4x shows all of it
PACK_DIR = "textures"
KEY_DIR = "art"

# What each plate ink takes from the gold, as a share of what 1 takes, and
# the coverage from which a pixel is drawn in it (art.c ink_of).
INK_SHARE = (0.0, 1.0, 0.93, 0.8, 0.6, 0.5, 0.35, 0.18)
INK_FROM = (247, 221, 179, 140, 109, 69, 23)
GOLD = (0xD8, 0xB4, 0x58)
# The end of the alias of a thumbnail the editor made from the picture (the
# game does not read "alias"): reverting the picture takes it too.
DERIVED = " (made from the art)"

_THUMB_CROPS = None


def record_base(cid: int) -> int:
    """The art record's first byte in WA_MRG.MRG."""
    return ((cid - 1) * ART_SECTORS + ART_SECTOR) * SECTOR


def pack_identity(cid: int, part: str) -> dict:
    """The pack entry keys that address a retail card's picture or
    thumbnail (extract_images.py cards, hd_assets_pack.py)."""
    if part == "art":
        base = record_base(cid)
        return {"archive": ARCHIVE, "offset": base, "words": 0x33, "rows": 0x60, "bpp": 8,
                "clut_offset": base + ART_CLUT, "clut_entries": 256}
    small = (cid - 1) * SECTOR
    return {"archive": ARCHIVE, "offset": small, "words": 20, "rows": 32, "bpp": 8,
            "clut_offset": small + THUMB_CLUT, "clut_entries": 64}


def pack_entry(cid: int, part: str, file: str, derived: bool = False) -> dict:
    ident = pack_identity(cid, part)
    width = ident["words"] * 2
    entry = {"file": file, "alias": f"card {cid} {'art' if part == 'art' else 'thumbnail'}" + (DERIVED if derived else "")}
    entry.update(ident)
    entry.update({"width": width, "height": ident["rows"], "crop_left": 0, "stride": ident["words"],
                  "row_offsets": None})
    return entry


def _number(value, default):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else default


def entry_card(entry) -> tuple:
    """(card, part) when a pack entry replaces a retail card's picture or
    thumbnail read the way the game reads it, else None."""
    if not isinstance(entry, dict) or str(entry.get("archive", "")).upper() != ARCHIVE:
        return None
    offset = _number(entry.get("offset"), -1)
    if not isinstance(offset, int) or offset < 0:
        return None
    if offset % SECTOR == 0 and offset // SECTOR < CARD_COUNT:
        cid, part = offset // SECTOR + 1, "thumbnail"
    elif offset % SECTOR == 0 and (offset // SECTOR - ART_SECTOR) % ART_SECTORS == 0 and \
            1 <= (offset // SECTOR - ART_SECTOR) // ART_SECTORS + 1 <= CARD_COUNT:
        cid, part = (offset // SECTOR - ART_SECTOR) // ART_SECTORS + 1, "art"
    else:
        return None
    ident = pack_identity(cid, part)
    for key in ("words", "rows", "bpp", "clut_offset", "clut_entries"):
        if _number(entry.get(key), None) != ident[key]:
            return None
    if _number(entry.get("stride"), ident["words"]) != ident["words"] or entry.get("row_offsets") is not None:
        return None
    if _number(entry.get("crop_left"), 0) != 0 or _number(entry.get("width"), ident["words"] * 2) != ident["words"] * 2:
        return None
    return cid, part


# --- the disc's pictures ------------------------------------------------------------

def _colour(word: int) -> bytes:
    """A 15-bit VRAM word as RGBA; 0 is the transparent colour
    (extract_images.expand)."""
    if word == 0:
        return b"\x00\x00\x00\x00"
    r, g, b = word & 0x1F, (word >> 5) & 0x1F, (word >> 10) & 0x1F
    return bytes((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255))


def _paletted(wa: bytes, at: int, width: int, height: int, clut_at: int, entries: int) -> Image:
    palette = [_colour(wa[clut_at + i * 2] | wa[clut_at + i * 2 + 1] << 8) for i in range(entries)]
    palette += [b"\x00\x00\x00\x00"] * (256 - entries)     # an index past a short palette
    return Image(width, height, b"".join(palette[i] for i in wa[at:at + width * height]))


def plate_image(inks, width=96, height=14, background=None) -> Image:
    """Plate inks (0-7, a list of width*height) as a picture: dark ink on
    white (what a "title" PNG is), or over a colour as the card view shows
    it."""
    out = bytearray()
    for ink in inks:
        share = INK_SHARE[ink] if 0 <= ink < len(INK_SHARE) else 0.0
        if background is None:
            v = 255 - round(share * 255)
            out += bytes((v, v, v, 255))
        else:
            out += bytes(round(c * (1 - share * 0.85)) for c in background) + b"\xff"
    return Image(width, height, bytes(out))


def disc_plate_inks(wa: bytes, cid: int) -> list:
    at = record_base(cid) + TITLE_PIXELS
    inks = []
    for byte in wa[at:at + 48 * 14]:
        inks += [byte & 0x0F, byte >> 4]    # the even pixel in the low nibble (art.c put_ink)
    return inks


def disc_image(wa: bytes, cid: int, part: str) -> Image:
    """A retail card's picture, thumbnail or name plate as the disc has it."""
    if not 1 <= cid <= CARD_COUNT:
        raise ValueError(f"card {cid} is not on the disc")
    base = record_base(cid)
    if part == "art":
        return _paletted(wa, base, 102, 96, base + ART_CLUT, 256)
    if part == "thumbnail":
        small = (cid - 1) * SECTOR
        return _paletted(wa, small, 40, 32, small + THUMB_CLUT, 64)
    return plate_image(disc_plate_inks(wa, cid))


def ink_of(coverage: int) -> int:
    for ink, start in enumerate(INK_FROM):
        if coverage >= start:
            return ink + 1
    return 0


def plate_inks(image: Image) -> list:
    """What the port makes of a "title" PNG (art.c CardArt_TitleFromImage):
    ink coverage (dark and opaque is full ink), the middle at the plate's
    shape averaged down to 96x14, each texel the ink of the nearest tone."""
    src, cover = image.rgba, bytearray()
    for i in range(0, len(src), 4):
        luma = (src[i] * 3 + src[i + 1] * 6 + src[i + 2]) // 10
        v = (255 - luma) * src[i + 3] // 255
        cover += bytes((v, v, v, 255))
    grey = Image(image.width, image.height, bytes(cover))
    small = pngio.resample(grey, 96, 14, pngio.middle(grey, 96, 14))
    return [ink_of(small.rgba[i]) for i in range(0, len(small.rgba), 4)]


# --- import --------------------------------------------------------------------------

def normalize(image: Image, part: str):
    """(image, notes): an imported PNG as the editor stores it. Cut to the
    part's shape from the middle (the port does that for a card's own art;
    a pack would stretch it instead), made opaque over black for a picture
    or thumbnail (in a pack, a pixel under half alpha is a hole in the
    card), and at most 4x the game's size."""
    notes = []
    w, h = SIZES[part]
    if abs(image.width / image.height - w / h) > 0.01 * w / h:
        left, top, cw, ch = pngio.middle(image, w, h)
        image = pngio.crop(image, round(left), round(top), round(cw), round(ch))
        notes.append(f"its shape is not {w}:{h}: the middle {image.width}x{image.height} of it is kept")
    if part != "title" and not image.opaque():
        image = pngio.flatten(image)
        notes.append("its transparent pixels are drawn over black")
    if image.width > w * MAX_SCALE or image.height > h * MAX_SCALE:
        image = pngio.resample(image, w * MAX_SCALE, h * MAX_SCALE)
        notes.append(f"it is kept at {w * MAX_SCALE}x{h * MAX_SCALE} (4x, what Internal 4x draws)")
    elif image.width < w or image.height < h:
        notes.append(f"it is smaller than the game's {w}x{h}: it will look blurred")
    return image, notes


def thumb_crops() -> dict:
    """The rectangle of its art each retail thumbnail is cut from (in 102x96
    texels), from tools/pc/hd_recipes/thumb_crops.json when the editor runs
    from the source tree; {} otherwise."""
    global _THUMB_CROPS
    if _THUMB_CROPS is None:
        _THUMB_CROPS = {}
        path = Path(__file__).resolve().parents[1] / "hd_recipes" / "thumb_crops.json"
        try:
            for key, value in json.loads(path.read_text(encoding="utf-8")).items():
                if isinstance(value, list) and len(value) == 5:
                    _THUMB_CROPS[int(key)] = tuple(value[1:])
        except (OSError, ValueError):
            pass
    return _THUMB_CROPS


def thumbnail_from(image: Image, cid: int) -> Image:
    """A retail card's thumbnail made from a new picture: cut where the
    game's own thumbnail is cut from its art when that is known, from the
    middle at 40:32 otherwise, at the picture's scale (up to 4x)."""
    scale = max(1, min(MAX_SCALE, image.width // 102))
    crop = thumb_crops().get(cid) if cid <= CARD_COUNT else None
    if crop:
        x, y, w, h = crop
        sx, sy = image.width / 102, image.height / 96
        box = (x * sx, y * sy, w * sx, h * sy)
    else:
        box = pngio.middle(image, 40, 32)
    return pngio.resample(image, 40 * scale, 32 * scale, box)


# --- the mod's replacements ------------------------------------------------------------

@dataclass
class Replacement:
    kind: str                  # "pack" (a texture pack entry) or "key" (the card entry's "art"/"thumbnail"/"title")
    file: str                  # relative to the pack directory (pack) or the mod folder (key)
    image: Image = None        # None until read
    pending: bool = False      # to be written on save
    derived: bool = False      # a thumbnail the editor made from the picture


class ArtState:
    def __init__(self):
        self.entries = None            # the pack's manifest.json as read (every entry, kept as written)
        self.owned = {}                # id(entry) -> (card, part): the entries that are a card part's replacement
        self.problem = ""              # why the pack's manifest.json could not be read
        self.folder = None             # the mod folder the pack and PNGs were read from
        self.images = {}               # (card, part) -> Replacement
        self.gated = {}                # (card, part) -> the first entry for it a setting switches (assets-hd)
        self.changed = False           # the manifest needs writing

    def adopt(self, entries: list):
        """Take `entries` as the pack on disk: the first plain entry for a
        card part (no "setting") is that part's replacement; one a setting
        switches is shown, and a replacement goes before it."""
        self.entries, self.owned, self.gated = entries, {}, {}
        for entry in entries:
            found = entry_card(entry)
            if not found or not isinstance(entry.get("file"), str):
                continue
            if entry.get("setting"):
                self.gated.setdefault(found, entry)
            elif found not in self.owned.values():
                self.owned[id(entry)] = found


def state(project) -> ArtState:
    st = getattr(project, "art_state", None)
    if st is None:
        st = project.art_state = ArtState()
    _sync(project, st)
    return st


def _extra_keys(project, cid: int) -> dict:
    if cid in project.added:
        return project.added[cid].extra
    return project.card_extra.get(cid, {})


def _sync(project, st: ArtState):
    """A key replacement lasts while the card's entry names its file: a
    card taken out, or reverted in the Cards tab, drops it; one an importer
    wrote (a .ygomods card's art.png) is taken in."""
    for (cid, part), rep in list(st.images.items()):
        if rep.kind == "key" and (cid not in project.cards or _extra_keys(project, cid).get(part) != rep.file):
            del st.images[(cid, part)]
    for cid in set(project.card_extra) | set(project.added):
        for part in PARTS:
            file = _extra_keys(project, cid).get(part)
            if isinstance(file, str) and file and (cid, part) not in st.images and cid in project.cards:
                st.images[(cid, part)] = Replacement("key", file)


def _set_extra(project, cid: int, key: str, value):
    if cid in project.added:
        extra = project.added[cid].extra
    else:
        extra = project.card_extra.setdefault(cid, {})
    if value is None:
        extra.pop(key, None)
        if cid not in project.added and not extra:
            project.card_extra.pop(cid, None)
    else:
        extra[key] = value


def mechanism(project, cid: int, part: str) -> str:
    return "pack" if part != "title" and cid not in project.added else "key"


def pack_dir(project) -> str:
    value = project.other.get("textures")
    return value if isinstance(value, str) and value else PACK_DIR


def read_mod(project, folder, messages: list = None):
    """Take a mod folder's art in: its pack's entries for retail cards'
    pictures and thumbnails, and its cards' "art", "thumbnail" and "title"."""
    messages = [] if messages is None else messages
    st = state(project)
    st.folder = Path(folder)
    value = project.other.get("textures")
    if isinstance(value, str) and value:
        path = st.folder / value / "manifest.json"
        try:
            entries = json.loads(path.read_text(encoding="utf-8-sig"))
            if not isinstance(entries, list):
                raise ValueError("it is not an array")
            st.adopt(entries)
            map_art.adopt(project, st)
        except (OSError, ValueError) as problem:
            st.problem = f"{value}/manifest.json: {problem}"
            messages.append(f"\"textures\": {st.problem}; the editor leaves the pack as it is")
        by_id = {id(e): e for e in st.entries or []}
        for entry_id, found in st.owned.items():
            alias = by_id[entry_id].get("alias")
            st.images[found] = Replacement("pack", by_id[entry_id]["file"],
                                           derived=isinstance(alias, str) and alias.endswith(DERIVED))
    for cid in sorted(set(project.card_extra) | set(project.added)):
        extra = _extra_keys(project, cid)
        for part in PARTS:
            file = extra.get(part)
            if isinstance(file, str) and file:
                if getattr(st.images.get((cid, part)), "kind", None) == "pack":
                    # The key wins in the game; the pack's entry is kept as written.
                    messages.append(f"card {cid}: its \"{part}\" hides the texture pack's picture of it")
                    st.owned = {k: v for k, v in st.owned.items() if v != (cid, part)}
                st.images[(cid, part)] = Replacement("key", file)
    return messages


def _file_path(project, rep: Replacement):
    folder = state(project).folder or project.source_dir
    if folder is None:
        return None
    return Path(folder) / (pack_dir(project) if rep.kind == "pack" else "") / rep.file


def replacement_image(project, cid: int, part: str):
    """The mod's own picture for a card part as stored (read from the mod
    folder the first time), or None."""
    rep = state(project).images.get((cid, part))
    if rep is None:
        return None
    if rep.image is None:
        blob = project.files.get(rep.file) if rep.kind == "key" else None
        if blob is not None:            # an importer's, not written yet
            rep.image = pngio.decode(blob)
            return rep.image
        path = _file_path(project, rep)
        if path is None:
            return None
        rep.image = pngio.read(path)
    return rep.image


def gated_image(project, cid: int, part: str):
    """(image, setting) of the pack's entry a setting switches for a card
    part, or None."""
    entry = state(project).gated.get((cid, part))
    folder = state(project).folder or project.source_dir
    if entry is None or folder is None:
        return None
    try:
        return pngio.read(Path(folder) / pack_dir(project) / entry["file"]), entry.get("setting")
    except (OSError, pngio.PngError):
        return None


def _editor_file(project, cid: int, part: str) -> str:
    if mechanism(project, cid, part) == "pack":
        return f"cards/{cid:04d}{'.small' if part == 'thumbnail' else ''}.png"
    stem = project.added[cid].key if cid in project.added else f"{cid:04d}"
    suffix = {"art": "", "thumbnail": ".small", "title": ".title"}[part]
    return f"{KEY_DIR}/{stem}{suffix}.png"


def _key_shared(project, owner, file: str) -> bool:
    """Another card part's "art"/"thumbnail"/"title" names `file`."""
    for cid in set(project.card_extra) | set(project.added):
        for part in PARTS:
            if (cid, part) != owner and _extra_keys(project, cid).get(part) == file:
                return True
    return False


def _free_key_file(project, owner) -> str:
    stem = _editor_file(project, *owner)[:-4]
    name, n = stem + ".png", 2
    while _key_shared(project, owner, name):
        name, n = f"{stem}-{n}.png", n + 1
    return name


def set_image(project, cid: int, part: str, image: Image) -> list:
    """Use `image` as the card part (normalized first); notes on what was
    done to it. The PNGs are written to the mod folder on save."""
    st = state(project)
    kind = mechanism(project, cid, part)
    if kind == "pack" and st.problem:
        raise ValueError(f"the mod's texture pack cannot be read ({st.problem}); correct it first")
    image, notes = normalize(image, part)
    moved = {}
    if kind == "pack":
        # A retail card's own "art" or "thumbnail" is drawn over the record
        # and reported written, which would hide the pack's: the pack has
        # them now, the one not being replaced moved there as it is. One
        # that cannot be read stops the import rather than being lost.
        for key in ("art", "thumbnail"):
            file = _extra_keys(project, cid).get(key)
            if key == part or not isinstance(file, str):
                continue
            try:
                own = replacement_image(project, cid, key)
            except (OSError, pngio.PngError) as problem:
                raise ValueError(f"its \"{key}\" ({file}) cannot be read ({problem}); revert or correct it first")
            if own is None:
                raise ValueError(f"its \"{key}\" ({file}) is not saved anywhere; save the mod first")
            moved[key] = normalize(own, key)[0]
    old = st.images.get((cid, part))
    if old is not None and old.kind == kind and not (kind == "key" and _key_shared(project, (cid, part), old.file)):
        file = old.file
    else:
        file = _free_key_file(project, (cid, part)) if kind == "key" else _editor_file(project, cid, part)
    st.images[(cid, part)] = Replacement(kind, file, image, pending=True)
    if kind == "key":
        _set_extra(project, cid, part, file)
        return notes
    st.changed = True
    for key in ("art", "thumbnail"):
        if not isinstance(_extra_keys(project, cid).get(key), str):
            continue
        if key in moved:
            st.images[(cid, key)] = Replacement("pack", _editor_file(project, cid, key), moved[key], pending=True)
        _set_extra(project, cid, key, None)
        notes.append(f"its \"{key}\" in mod.json moves to the texture pack")
    _sync(project, st)
    if part == "art":
        thumb = st.images.get((cid, "thumbnail"))
        if thumb is None or thumb.derived:
            st.images[(cid, "thumbnail")] = Replacement(
                "pack", thumb.file if thumb is not None else _editor_file(project, cid, "thumbnail"),
                thumbnail_from(image, cid), pending=True, derived=True)
            notes.append("the thumbnail is made from it (import one of its own to change that)")
    return notes


def revert(project, cid: int, part: str):
    """Back to the disc's picture (a thumbnail made from a reverted picture
    goes with it)."""
    st = state(project)
    rep = st.images.pop((cid, part), None)
    if rep is None:
        return
    if rep.kind == "key":
        _set_extra(project, cid, part, None)
    else:
        st.changed = True
    if part == "art":
        thumb = st.images.get((cid, "thumbnail"))
        if thumb is not None and thumb.derived:
            revert(project, cid, "thumbnail")


def changed_cards(project) -> set:
    return {cid for cid, _ in state(project).images}


def _baked(project, cid: int, part: str) -> bool:
    """Made into the art record's texels when the game starts (a card's own
    "art"/"thumbnail"); a PNG bigger than the part is also drawn at its own
    resolution above 1x (cards.c, add_full_picture)."""
    st = state(project)
    rep = st.images.get((cid, part))
    if rep is not None:
        return rep.kind == "key"
    own_art = st.images.get((cid, "art"))
    return part == "thumbnail" and own_art is not None and own_art.kind == "key"


def shown_image(project, wa: bytes, cid: int, part: str):
    """(image, where it comes from): what the game draws for a card part,
    at the size it is stored; a plate as it looks over the card's gold."""
    st = state(project)
    rep = st.images.get((cid, part))
    if rep is not None:
        try:
            image = replacement_image(project, cid, part)
        except (OSError, pngio.PngError) as problem:
            return None, f"the mod's {rep.file} cannot be read ({problem})"
        if image is None:
            return None, f"the mod's {rep.file} is not saved anywhere yet"
        if part == "title":
            image = plate_image(plate_inks(image), background=GOLD)
        return image, "the mod"
    base = project.base_of(cid)
    gated = gated_image(project, cid, part) if part != "title" and base == cid else None
    if gated is not None:
        return gated[0], f"the mod's texture pack, while its \"{gated[1]}\" setting is on"
    if part == "thumbnail" and _baked(project, cid, part):
        return port_thumbnail(project, cid), "a thumbnail made from its own picture when the game starts"
    if part == "title" and own_name(project, cid):
        return None, f"its name, \"{project.cards[cid].name}\", set in Times on the plate when it starts"
    if base != cid and part != "title" and (base, part) in st.images:
        image, where = shown_image(project, wa, base, part)
        return image, f"its base's (card {base}) in the mod" if image is not None else where
    if part == "title":
        return plate_image(disc_plate_inks(wa, base), background=GOLD), \
            "the disc" if base == cid else f"its base's (card {base}) on the disc"
    return disc_image(wa, base, part), "the disc" if base == cid else f"its base's (card {base}) on the disc"


def own_name(project, cid: int) -> bool:
    """The card has a name of its own in mod.json, which the port sets on
    its plate (cards.c: a copy always has one as the editor writes it)."""
    return cid in project.added or project.cards[cid].name != project.retail.cards[cid].name


def port_thumbnail(project, cid: int):
    """The thumbnail the port makes from a card's own "art" (art.c): the
    middle at 40:32, at the picture's resolution (cards.c draws it from
    there above 1x)."""
    try:
        art = replacement_image(project, cid, "art")
    except (OSError, pngio.PngError):
        return None
    if art is None:
        return None
    left, top, w, h = pngio.middle(art, 40, 32)
    return pngio.crop(art, round(left), round(top), max(1, round(w)), max(1, round(h)))


def in_game(project, wa: bytes, cid: int, part: str, scale: int = 1):
    """The part as the game draws it (before the card's frame) at the
    console's resolution (1) or Internal 2x/4x."""
    image, _ = shown_image(project, wa, cid, part)
    if image is None:
        return None
    if part == "title":
        return pngio.scale_nearest(image, scale)
    w, h = SIZES[part]
    if _baked(project, cid, part):
        box = pngio.middle(image, w, h)
        if scale > 1 and (box[2] > w or box[3] > h):
            return pngio.resample(image, w * scale, h * scale, box)
        small = pngio.resample(image, w, h, box)
        return pngio.scale_nearest(pngio.to_15bit(small), scale)
    if image.size == (w, h):
        return pngio.scale_nearest(image, scale)
    # A pack image: averaged to the texture at 1x, sampled at its own
    # resolution above that.
    drawn = pngio.resample(image, w * scale, h * scale)
    return pngio.to_15bit(drawn) if scale == 1 else drawn


def describe(project, cid: int, part: str) -> str:
    """How the mod stores the part, for the tab."""
    rep = state(project).images.get((cid, part))
    w, h = SIZES[part]
    if rep is None:
        if part == "thumbnail" and _baked(project, cid, part):
            return "Made from the card's own picture when the game starts (the middle at 40:32)."
        if part == "title" and own_name(project, cid):
            return ("The card has a name of its own: the game sets it in Times at the plate's size (not shown "
                    "here). Import a PNG to draw the plate yourself.")
        if part != "title" and (cid, part) in state(project).gated:
            setting = state(project).gated[(cid, part)].get("setting")
            return (f"The texture pack's, while the \"{setting}\" setting is on (the disc's when it is off). "
                    "An imported PNG is used whatever the setting.")
        return "As the disc has it."
    try:
        image = replacement_image(project, cid, part)
    except (OSError, pngio.PngError):
        image = None
    size = f"{image.width}x{image.height}" if image is not None else "?"
    where = f"{pack_dir(project)}/{rep.file}" if rep.kind == "pack" else rep.file
    if rep.kind == "pack":
        text = f"Texture pack ({where}), {size}{', made from the picture' if rep.derived else ''}. "
        if image is not None and image.width >= w * 2:
            text += f"Averaged to {w}x{h} at the console's resolution; Internal {min(4, image.width // w)}x shows all of it."
        else:
            text += f"{w}x{h}: import a bigger PNG (up to {w * 4}x{h * 4}) for detail at Internal 2x/4x."
        return text
    if part == "title":
        return f"\"title\" in mod.json ({where}), {size}: set in the plate's 7 inks at 96x14."
    text = f"\"{part}\" in mod.json ({where}), {size}: {w}x{h} at the console's resolution. "
    if image is not None and image.width >= w * 2:
        return text + f"Internal {min(4, image.width // w)}x shows all of it."
    return text + f"Import a bigger PNG (up to {w * 4}x{h * 4}) for detail at Internal 2x/4x."


# --- saving -------------------------------------------------------------------------------

def entries_now(project) -> list:
    """The pack's manifest as it will be written: every entry as read, a
    replaced card part's pointing at its file, a reverted one's gone, new
    ones at the end."""
    st = state(project)
    out, placed = [], set()
    for entry in st.entries or []:
        found = st.owned.get(id(entry))
        if found is not None:
            rep = st.images.get(found)
            if rep is None or rep.kind != "pack":
                continue        # reverted, or the card's own key now
            placed.add(found)
            if entry.get("file") != rep.file:
                entry = dict(entry, file=rep.file)
            alias = entry.get("alias")
            if isinstance(alias, str) and alias.endswith(DERIVED) != rep.derived:
                entry = dict(entry, alias=alias + DERIVED if rep.derived else alias[:-len(DERIVED)])
        out.append(entry)
    for key in sorted(st.images):
        rep = st.images[key]
        if rep.kind == "pack" and key not in placed:
            out.append(pack_entry(key[0], key[1], rep.file, rep.derived))
    # The first entry for the same words is the one drawn (texture_pack.c
    # compare): a replacement goes before one a setting switches.
    for key, rep in st.images.items():
        if rep.kind != "pack":
            continue
        same = [i for i, e in enumerate(out) if entry_card(e) == key]
        mine = next((i for i in same if out[i].get("file") == rep.file and not out[i].get("setting")), None)
        if mine is not None and same[0] < mine:
            out.insert(same[0], out.pop(mine))
    return out + map_art.entries(project)


def _free_file(st: ArtState, owner, name: str) -> str:
    """`name`, or another when an entry not the card part's own also draws
    from that PNG (hd_assets_pack.py gives identical pictures one file)."""
    def shared(candidate):
        return any(e.get("file") == candidate and st.owned.get(id(e)) != owner
                   for e in st.entries or [] if isinstance(e, dict))
    if not shared(name):
        return name
    stem, n = name[:-4] if name.lower().endswith(".png") else name, 2
    while shared(f"{stem}-{n}.png"):
        n += 1
    return f"{stem}-{n}.png"


def _adopt_imported(project, st: ArtState):
    """An importer may have put a texture pack in project.files (a .ygomods
    package's portraits): the art state takes its manifest over, so that
    the entries survive a save that also writes card art."""
    name = pack_dir(project) + "/manifest.json"
    blob = project.files.get(name)
    if blob is None or st.entries is not None:
        return
    try:
        entries = json.loads(blob.decode("utf-8-sig"))
        if not isinstance(entries, list):
            raise ValueError("it is not an array")
    except ValueError:
        return          # left in project.files, written as the importer made it
    st.adopt(entries)
    map_art.adopt(project, st)
    del project.files[name]
    st.changed = True


def write_mod(project, folder):
    """Write the pending PNGs into the mod folder and the pack's
    manifest.json; mod.json's "textures" names the pack while it has
    entries. Run before mod.json is built."""
    st = state(project)
    folder = Path(folder)
    _adopt_imported(project, st)
    for key, rep in st.images.items():
        if rep.kind == "pack" and rep.pending:
            rep.file = _free_file(st, key, rep.file)
    textures = pack_dir(project)
    map_art.write(project, folder)
    for key, rep in sorted(st.images.items()):
        if rep.pending and rep.image is not None:
            path = folder / (textures if rep.kind == "pack" else "") / rep.file
            path.parent.mkdir(parents=True, exist_ok=True)
            pngio.write(path, rep.image)
            rep.pending = False
    if st.changed and not st.problem:
        entries = entries_now(project)
        (folder / textures).mkdir(parents=True, exist_ok=True)
        with open(folder / textures / "manifest.json", "w", encoding="utf-8", newline="\n") as out:
            json.dump(entries, out, indent=1)
            out.write("\n")
        if entries:
            project.other["textures"] = textures
        else:
            project.other.pop("textures", None)
        st.adopt(entries)
        map_art.adopt(project, st)
        st.changed = False
    st.folder = folder


# --- checks ---------------------------------------------------------------------------------

def contained(path) -> bool:
    """The port's Paths_Contained: relative, forward slashes, no empty, "."
    or ".." part, no drive."""
    if not isinstance(path, str) or not path or path[0] in "/\\" or (len(path) > 1 and path[1] == ":"):
        return False
    if "\\" in path:
        return False
    return all(part not in ("", ".", "..") for part in path.split("/"))


def check(project, out: list):
    """The texture pack loader's checks (texture_pack.c TexturePack_Load,
    mods.c) and cards.c's on the "art", "thumbnail" and "title" PNGs."""
    from .validate import Issue
    st = state(project)
    folder = st.folder or project.source_dir
    textures = project.other.get("textures")
    pending = {(rep.kind, rep.file) for rep in st.images.values() if rep.pending}
    pending |= {("pack", file) for file in map_art.pending_files(project)}
    if textures is not None and (not isinstance(textures, str) or not contained(textures)):
        out.append(Issue("error", "Art", "textures", f"{textures!r} is outside the mod: the pack is not loaded"))
        return
    if st.problem:
        out.append(Issue("error", "Art", "textures", f"{st.problem}: the pack is not loaded"))
    textures = pack_dir(project)
    entries = entries_now(project) if not st.problem else []
    declared = {s.get("key") for s in project.info.settings if isinstance(s, dict)}
    if len(entries) > 65535:
        out.append(Issue("warning", "Art", "textures", f"{len(entries)} images: the port takes the first 65535"))
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            out.append(Issue("error", "Art", f"textures[{index}]", "not an object; left out"))
            continue
        file, archive = entry.get("file"), entry.get("archive")
        found = entry_card(entry)
        where = f"textures[{index}] {file}" if isinstance(file, str) else f"textures[{index}]"
        target = found[0] if found else None

        def bad(message, level="error"):
            out.append(Issue(level, "Art", where, message, target))
        setting = entry.get("setting")
        if setting is not None and (not isinstance(setting, str) or not setting or setting not in declared):
            bad(f"names a setting the mod does not declare ({setting!r}); the image is used anyway", "warning")
        if not isinstance(file, str) or not file or not isinstance(archive, str) or len(archive) >= 32:
            bad("names no file or archive; left out")
            continue
        if not contained(file):
            bad("is outside the pack; left out")
            continue
        offset, clut = _number(entry.get("offset"), -1), _number(entry.get("clut_offset"), 0)
        words, rows, bpp = _number(entry.get("words"), 0), _number(entry.get("rows"), 0), _number(entry.get("bpp"), 0)
        stride, crop_left = _number(entry.get("stride"), words), _number(entry.get("crop_left"), 0)
        per = {4: 4, 8: 2, 16: 1}.get(bpp, 0)
        if not (0 <= offset < 1e9 and 0 <= clut < 1e9 and 1 <= words <= 1024 and 1 <= rows <= 512 and per and
                1 <= stride <= 1e6 and 0 <= crop_left < words * per):
            bad("has measures out of range; left out")
            continue
        width = _number(entry.get("width"), words * per - crop_left)
        if not (width >= 1 and crop_left + width <= words * per):
            bad("has measures out of range (width); left out")
            continue
        row_offsets = entry.get("row_offsets")
        if row_offsets is not None and (not isinstance(row_offsets, list) or len(row_offsets) != rows):
            bad("its row_offsets do not match its rows: read with the stride instead", "warning")
        if ("pack", file) not in pending and folder is not None and not (Path(folder) / textures / file).is_file():
            bad("the file is not there; left out")
    for cid in sorted(set(project.card_extra) | set(project.added)):
        extra = _extra_keys(project, cid)
        for part in PARTS:
            file = extra.get(part)
            if not isinstance(file, str) or not file:
                continue
            where = f"card {cid} \"{part}\""
            if not contained(file):
                out.append(Issue("error", "Art", where, f"{file} is outside the mod: not used", cid))
            elif ("key", file) not in pending and folder is not None:
                try:
                    with open(Path(folder) / file, "rb") as handle:
                        ok = handle.read(8) == pngio.SIGNATURE
                except OSError:
                    out.append(Issue("error", "Art", where, f"{file} is not there: not used", cid))
                    continue
                if not ok:
                    out.append(Issue("error", "Art", where, f"{file} is not a PNG: not used", cid))
            if part == "art" and cid not in project.added and any(
                    entry_card(e) == (cid, "art") for e in entries if isinstance(e, dict)):
                out.append(Issue("warning", "Art", where, "hides the texture pack's picture of this card", cid))
