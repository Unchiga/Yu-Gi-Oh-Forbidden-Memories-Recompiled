"""The UI tab's Frames page.

This is a visual editor for ``card_layout.frame_styles``.  It intentionally
edits the same manifest objects the game reads: assets live on a style, while
the title/art/stars/stat placements live in that style's named part.  The
preview is the 320x240 card-viewer composition, so dragging uses game pixels.
"""
from __future__ import annotations

import tkinter as tk
from tkinter import messagebox, simpledialog, ttk
from pathlib import Path
import shutil
import subprocess

from . import art, card_icons, card_text, manifest, packs, pngio, ttf, ui_assets
from .ui_tab import (FRAME_IMAGE_DIR, GAME_H, GAME_W, ElementList, StageView, as_int, ensure, import_image,
                     mod_image, section, set_member, sized)
from .widgets import CardField, px
from .gamedata import TYPE_NAMES

CARD_W, CARD_H = 140, 196
HAND_COORD_LIMIT = 128
HAND_SIZE_LIMIT = 128

# id, user-facing label, manifest path, default rectangle, can hide, can resize.
PARTS = (
    ("frame", "Frame image", ("image",), (0, 0, 140, 196), False, True),
    ("back", "Back image", ("back_image",), (0, 0, 140, 196), False, True),
    ("hand", "Duel hand image", ("hand_image",), (0, 0, 52, 60), False, False),
    ("field", "Face-up field image", ("field_image",), (0, 0, 40, 56), False, False),
    ("field_back", "Face-down field image", ("field_back_image",), (0, 0, 40, 56), False, False),
    ("library", "Library icon", ("library_icon",), (0, 0, 24, 32), False, False),
    ("digits", "Digit strip", ("digits", "image"), (0, 0, 10, 12), False, False),
    ("title", "Card name", ("title",), (12, 14, 96, 14), True, False),
    # With no ``art`` object, func_80028B08 keeps the retail artwork quad:
    # +0x13,+0x32 and 0x66×0x60.  A custom rectangle is full-bleed only and
    # is therefore an opt-in, not this editor's default.
    ("art", "Artwork", ("art",), (19, 50, 102, 96), True, True),
    ("stars", "Level stars", ("stars",), (119, 32, 56, 9), True, False),
    ("attribute", "Attribute icon", ("attribute",), (110, 13, 16, 16), True, True),
    # The digit strip has one glyph size shared by the ATK and DEF values.
    # Listing it on each value is deliberate: either one can be selected,
    # moved and sized without making the user hunt for the strip asset.
    ("atk", "ATK value", ("digits", "atk"), (97, 169, 10, 12), True, True),
    ("def", "DEF value", ("digits", "def"), (97, 184, 10, 12), True, True),
    ("atk_label", "ATK label", ("atk_label",), (12, 171, 16, 8), True, True),
    ("dfd_label", "DFD label", ("dfd_label",), (12, 184, 16, 8), True, True),
    # Retail's caption sprite has transparent leading pixels: its visible
    # lettering is at the right of the card, around (84, 35).  A replacement
    # image needs that explicit box, otherwise its own ink lands at (0, 0).
    ("magic_label", "Magic-card caption", ("type_label", "magic"), (84, 35, 56, 16), True, True),
    ("trap_label", "Trap-card caption", ("type_label", "trap"), (84, 35, 56, 16), True, True),
    ("ritual_label", "Ritual-card caption", ("type_label", "ritual"), (84, 35, 56, 16), True, True),
    ("equip_label", "Equip-card caption", ("type_label", "equip"), (84, 35, 56, 16), True, True),
    # Magic, ritual and equip use the Magic ball; Trap has its own one.  The
    # runtime reads this from spell.icon, not the monster-only attribute key.
    ("spell_icon", "Spell/trap icon", ("spell", "icon"), (110, 13, 16, 16), True, True),
)
SPECS = {item[0]: item for item in PARTS}
# The duel hand has a separate 52x60 coordinate system.  These are real
# ``style.hand`` manifest entries, not aliases of the Main-card elements.
HAND_PARTS = (
    ("hand_art", "Artwork", ("hand", "art"), (6, 6, 40, 32), True, True),
    ("hand_atk", "ATK value", ("hand", "atk"), (14, 40, 8, 8), True, True),
    ("hand_def", "DEF value", ("hand", "def"), (14, 48, 8, 8), True, True),
    ("hand_atk_label", "ATK label", ("hand", "atk_label"), (5, 40, 8, 8), True, True),
    ("hand_dfd_label", "DEF label", ("hand", "dfd_label"), (5, 48, 8, 8), True, True),
)
SPECS.update({item[0]: item for item in HAND_PARTS})
DIRECT_ASSETS = {"frame", "back", "hand", "field", "field_back", "library"}
IMAGE_PARTS = {"digits", "atk_label", "dfd_label", "magic_label", "trap_label", "ritual_label", "equip_label"}
MOVABLE = {"title", "art", "stars", "attribute", "spell_icon", "atk", "def", "atk_label", "dfd_label",
           "magic_label", "trap_label", "ritual_label", "equip_label", "hand_art", "hand_atk", "hand_def",
           "hand_atk_label", "hand_dfd_label"}
CAPTION_KEYS = ("magic_label", "trap_label", "ritual_label", "equip_label")
CAPTION_TEXT = {
    "magic_label": "Normal Magic Card",
    "trap_label": "Trap Card",
    "ritual_label": "Ritual Magic Card",
    "equip_label": "Equip Magic Card",
}
PREVIEW_KINDS = (
    ("monster", "Monster", None, 0),
    ("magic", "Magic", "magic_label", 6),
    ("equip", "Equip", "equip_label", 6),
    ("ritual", "Ritual", "ritual_label", 6),
    ("trap", "Trap", "trap_label", 7),
)

# The editor list is organized by render target.  Hand rows map to the
# independent ``style.hand`` placements the duel renderer reads.
PART_GROUP_LABELS = {"category_main": "Main", "category_hand": "Hand", "category_field": "Field",
                     "category_library": "Library"}
PART_GROUP_ORDER = ("category_main", "category_hand", "category_field", "category_library")
PART_GROUP_TEMPLATE = {"category_main": "main", "category_hand": "hand", "category_field": "field",
                       "category_library": "library"}
DISPLAY_PARTS = (
    ("frame", "category_main", "frame", "Frame image"),
    ("back", "category_main", "back", "Back image"),
    ("digits", "category_main", "digits", "Digit strip"),
    ("title", "category_main", "title", "Card name"),
    ("art", "category_main", "art", "Artwork"),
    ("stars", "category_main", "stars", "Level stars"),
    ("attribute", "category_main", "attribute", "Attribute icon"),
    ("atk", "category_main", "atk", "ATK value"),
    ("atk_label", "category_main", "atk_label", "ATK label"),
    ("def", "category_main", "def", "DEF value"),
    ("dfd_label", "category_main", "dfd_label", "DEF label"),
    ("magic_label", "category_main", "magic_label", "Magic-card caption"),
    ("equip_label", "category_main", "equip_label", "Equip-card caption"),
    ("ritual_label", "category_main", "ritual_label", "Ritual-card caption"),
    ("trap_label", "category_main", "trap_label", "Trap-card caption"),
    ("hand", "category_hand", "hand", "Hand image"),
    ("hand_art", "category_hand", "hand_art", "Artwork"),
    ("hand_atk", "category_hand", "hand_atk", "ATK value"),
    ("hand_atk_label", "category_hand", "hand_atk_label", "ATK label"),
    ("hand_def", "category_hand", "hand_def", "DEF value"),
    ("hand_dfd_label", "category_hand", "hand_dfd_label", "DEF label"),
    ("field", "category_field", "field", "Field face-up image"),
    ("field_back", "category_field", "field_back", "Field face-down image"),
    ("library", "category_library", "library", "Library icon"),
)
DISPLAY_SPEC = {item[0]: item for item in DISPLAY_PARTS}


def solid(width, height, color):
    return pngio.Image(width, height, bytes(color) * width * height)


def checker(width, height):
    out = bytearray()
    for y in range(height):
        for x in range(width):
            out += bytes((30, 42, 64, 255) if (x // 8 + y // 8) & 1 else (20, 30, 48, 255))
    return pngio.Image(width, height, bytes(out))


def text_picture(font, text, width, height, color):
    """A retail-font text strip stretched into the element's exact box.

    Canvas text chooses its own bounds, so it made the title and stat values
    look unchanged when their Width/Height fields were edited.  The game
    renders these as sprites; a small transparent bitmap is both closer to
    that behaviour and gives the editor an honest, selectable rectangle.
    """
    if not font or not text:
        return None
    cells = [font.cell(c) for c in text]
    native_w, native_h = max(1, len(cells) * card_text.CELL_W), card_text.CELL_H
    rgba = bytearray(native_w * native_h * 4)
    try:
        rgb = tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))
    except (TypeError, ValueError):
        rgb = (255, 255, 255)
    for col, cell in enumerate(cells):
        if cell is None:
            continue
        for y in range(card_text.CELL_H):
            for x in range(card_text.CELL_W):
                if not cell[y * card_text.CELL_W + x]:
                    continue
                at = ((y * native_w) + col * card_text.CELL_W + x) * 4
                rgba[at:at + 4] = bytes((*rgb, 255))
    return sized(pngio.Image(native_w, native_h, bytes(rgba)), width, height)


_title_faces = {}


def title_face():
    """A TrueType serif face for the title-plate preview.

    The game asks FreeType for ``Times``.  Linux commonly fulfils that with
    Nimbus Roman in CFF/OTF form, which the editor's intentionally small TTF
    outline reader cannot parse.  Prefer that exact match wherever it is a
    TTF, otherwise use the closest installed TrueType serif so we can still
    reproduce the title plate's coordinates and coverage.
    """
    candidates = [packs.serif_path()]
    match = shutil.which("fc-match")
    if match:
        for family in ("Liberation Serif:regular", "Noto Serif:regular", "serif:regular"):
            try:
                path = subprocess.run([match, "-f", "%{file}", family], capture_output=True, text=True,
                                      timeout=5).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                continue
            candidates.append(path)
    for path in candidates:
        if not path or not Path(path).is_file():
            continue
        try:
            face = _title_faces.get(path)
            if face is None:
                face = _title_faces[path] = ttf.Font(path)
            return face
        except (ttf.FontError, OSError, ValueError):
            continue
    return None


def title_plate_picture(text, width, height, color):
    """The HD title sprite the game makes for a card name.

    This deliberately does *not* use the port's general HD UI font.  Card
    names come from ``art.c:set_name``: a fixed 96x14 Times-style plate whose
    ink begins three game pixels inside its left edge.  Keeping that inset is
    what makes the editor's configured title position line up with the game.
    """
    if not text:
        return None
    try:
        face = title_face()
        if face is None:
            return None
        factor = 4
        inks = packs.name_plate_inks(text, face, factor)
    except (ttf.FontError, OSError, ValueError, IndexError, KeyError):
        return None
    if inks is None:
        return None
    try:
        rgb = tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))
    except (TypeError, ValueError):
        rgb = (255, 255, 255)
    rgba = bytearray(96 * factor * 14 * factor * 4)
    for index, ink in enumerate(inks):
        if not ink:
            continue
        # The game turns the plate's seven coverage inks into the configured
        # title colour.  The same alpha ramp retains antialiasing on a
        # transparent preview canvas.
        alpha = round(255 * art.INK_SHARE[ink]) if ink < len(art.INK_SHARE) else 0
        rgba[index * 4:index * 4 + 4] = bytes((*rgb, alpha))
    return sized(pngio.Image(96 * factor, 14 * factor, bytes(rgba)), width * factor, height * factor)


def digit_cell(strip, digit, dim=False):
    """One of CardLayoutArt_DigitCell's 40×48 source cells from a style PNG."""
    if strip is None or digit < 0 or digit > 9:
        return None
    col, row = digit % 5, digit // 5 + (2 if dim else 0)
    return pngio.crop(strip, col * 40, row * 48, 40, 48)


# The hand's stat overlay is not text: duel_card_frame_draw.c samples these
# 8x8 sprites from the duel boot texture page. Keep a small decoded VRAM
# cache so the Hand preview uses the same digits, sword and shield as game.
_hand_sheets = {}


def hand_sprite(wa, u, v, width=8, height=8):
    if not wa:
        return None
    key = id(wa)
    sheet = _hand_sheets.get(key)
    if sheet is None:
        sheet = _hand_sheets[key] = ui_assets.DuelArt(wa)
    if not sheet.ok:
        return None
    # duel_screen.py documents this retail source: page (896, 256), 8-bit,
    # with the gold hand-frame palette at CLUT (256, 241).  This must be
    # cached by source rectangle, not recreated for every canvas redraw:
    # Tk retains photos while an image item is displayed, and a sequence of
    # short-lived glyph images could otherwise be associated with the wrong
    # item as the hand preview is clicked or dragged.
    source = ("frame-hand-sprite", u, v, width, height)
    return sheet._cached(source,
                         lambda: sheet.vram.sprite(896, 256, 8, 256, 241, u, v, width, height))


class FramesPage(ttk.Frame):
    def __init__(self, master, tab):
        super().__init__(master)
        self.tab, self.chosen, self.loading = tab, "frame", False
        self.list_chosen, self.preview_template = "frame", "main"
        self.style_name = tk.StringVar(value="custom")
        self.face = tk.BooleanVar(value=True)
        self.preview_kind = tk.StringVar(value="monster")
        # Frames has its own three-column layout: styles/rules, live card,
        # then the selected element and its inspector.  The other UI pages
        # use page_layout(), whose one right column is too cramped here.
        self.columnconfigure(1, weight=1)
        self.rowconfigure(0, weight=1)
        left = ttk.LabelFrame(self, text="Frame styles", padding=6)
        left.grid(row=0, column=0, sticky="nsw")
        # Unlike the other UI previews, a frame is itself a portrait card,
        # not the full 320x240 viewer screen.  Giving StageView this exact
        # logical rectangle lets it use the whole middle column without
        # changing any on-card coordinate or its aspect ratio.
        self.view = StageView(self, self.draw, game_width=CARD_W, game_height=CARD_H, smooth=True,
                              on_select=self.select, on_move=self.moved, on_wheel=self.wheel,
                              on_drag=self.dragging)
        self.view.grid(row=0, column=1, sticky="nsew", padx=(px(self, 10), px(self, 10)))
        self.stage = self.view.stage
        self.side = ttk.Frame(self, width=px(self, 340))
        self.side.grid(row=0, column=2, sticky="nsew")
        self.side.columnconfigure(0, weight=1)
        self.side.rowconfigure(0, weight=1)
        self.list = ElementList(self.side, self.pick, height=15)
        self.list.grid(row=0, column=0, sticky="nsew")
        self.list_tools = ttk.Frame(self.side)
        self.list_tools.grid(row=1, column=0, sticky="ew", pady=(px(self, 4), 0))
        ttk.Separator(self.side).grid(row=2, column=0, sticky="ew", pady=(px(self, 8), px(self, 6)))
        head = ttk.Frame(self.side)
        head.grid(row=3, column=0, sticky="ew")
        self.heading = ttk.Label(head)
        self.heading.pack(anchor="w")
        self.what = ttk.Label(head, style="Hint.TLabel", wraplength=px(self, 320), justify="left")
        self.what.pack(anchor="w")
        self.revert_page = ttk.Button(self.list_tools, text="Revert page", command=lambda: self.tab.revert_page(self))
        self.revert_page.pack(side="right")
        self.style_list = tk.Listbox(left, exportselection=False, height=12, width=20)
        self.style_list.pack(fill="x")
        self.style_list.bind("<<ListboxSelect>>", lambda e: self.changed_style())
        style_tools = ttk.Frame(left)
        style_tools.pack(fill="x", pady=(6, 0))
        ttk.Button(style_tools, text="New...", command=self.new_style).pack(side="left")
        ttk.Button(style_tools, text="Delete", command=self.delete_style).pack(side="left", padx=(4, 0))
        rules = ttk.LabelFrame(left, text="Apply this style", padding=6)
        rules.pack(fill="x", pady=(10, 0))
        global_row = ttk.Frame(rules)
        global_row.pack(anchor="w")
        ttk.Button(global_row, text="Set as global default", command=self.set_default).pack(side="left")
        ttk.Button(global_row, text="Reset to retail", command=self.reset_default).pack(side="left", padx=(4, 0))
        ttk.Label(rules, text="Default for kind").pack(anchor="w", pady=(8, 0))
        self.kind = tk.StringVar(value="monster")
        ttk.Combobox(rules, textvariable=self.kind, state="readonly", width=17,
                     values=("monster", "effect_monster", "magic", "equip", "ritual", "trap")).pack(anchor="w")
        kind_row = ttk.Frame(rules)
        kind_row.pack(anchor="w", pady=(3, 0))
        ttk.Button(kind_row, text="Apply to kind", command=self.set_kind).pack(side="left")
        ttk.Button(kind_row, text="Reset to retail", command=self.reset_kind).pack(side="left", padx=(4, 0))
        ttk.Label(rules, text="Default for printed type").pack(anchor="w", pady=(8, 0))
        self.type_name = tk.StringVar(value="Dragon")
        ttk.Combobox(rules, textvariable=self.type_name, width=17, values=TYPE_NAMES,
                     state="readonly").pack(anchor="w")
        type_row = ttk.Frame(rules)
        type_row.pack(anchor="w", pady=(3, 0))
        ttk.Button(type_row, text="Apply to type", command=self.set_type).pack(side="left")
        ttk.Button(type_row, text="Reset to retail", command=self.reset_type).pack(side="left", padx=(4, 0))
        ttk.Separator(rules).pack(fill="x", pady=(9, 7))
        ttk.Label(rules, text="Specific card").pack(anchor="w")
        self.card = CardField(rules, lambda: self.project, width=17)
        self.card.pack(fill="x", pady=(2, 0))
        # A specific-card assignment is also a useful preview target.  Redraw
        # as soon as its number/name changes, without making the field itself
        # alter the saved assignment until Apply is pressed.
        self.card.var.trace_add("write", lambda *_: self.draw())
        card_row = ttk.Frame(rules)
        card_row.pack(anchor="w", pady=(3, 0))
        ttk.Button(card_row, text="Apply to card", command=self.set_card).pack(side="left")
        ttk.Button(card_row, text="Reset to retail", command=self.reset_card).pack(side="left", padx=(4, 0))
        ttk.Label(self.view.tools, text="Preview").pack(side="left")
        ttk.Radiobutton(self.view.tools, text="Front", variable=self.face, value=True, command=self.draw).pack(side="left")
        ttk.Radiobutton(self.view.tools, text="Back", variable=self.face, value=False, command=self.draw).pack(side="left")
        ttk.Separator(self.view.tools, orient="vertical").pack(side="left", fill="y", padx=8)
        for key, label, _, _ in PREVIEW_KINDS:
            ttk.Radiobutton(self.view.tools, text=label, variable=self.preview_kind, value=key,
                            command=self.draw).pack(side="left")
        form = ttk.Frame(self.side)
        form.grid(row=4, column=0, sticky="new", pady=(6, 0))
        form.columnconfigure(1, weight=1)
        ttk.Label(form, text="Style").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=2)
        self.styles = ttk.Label(form, textvariable=self.style_name)
        self.styles.grid(row=0, column=1, sticky="w", pady=2)
        self.vars = {key: tk.StringVar() for key in ("x", "y", "width", "height")}
        self.rows = {}
        for row, key in enumerate(("x", "y", "width", "height"), 1):
            ttk.Label(form, text=key.capitalize()).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=2)
            box = ttk.Spinbox(form, from_=-320, to=320, width=7, textvariable=self.vars[key],
                              command=lambda k=key: self.typed(k))
            box.grid(row=row, column=1, sticky="w", pady=2)
            box.bind("<Return>", lambda e, k=key: self.typed(k))
            box.bind("<FocusOut>", lambda e, k=key: self.typed(k))
            self.rows[key] = (form.grid_slaves(row=row, column=0)[0], box)
        actions = ttk.Frame(form)
        self.choose = ttk.Button(actions, text="Choose PNG...", command=self.choose_image)
        self.choose.pack(side="left")
        self.remove = ttk.Button(actions, text="Remove image", command=self.remove_image)
        self.remove.pack(side="left", padx=(4, 0))
        ttk.Label(form, text="Image").grid(row=5, column=0, sticky="w", padx=(0, 8), pady=(7, 2))
        actions.grid(row=5, column=1, sticky="w", pady=(7, 2))
        self.image_name = ttk.Label(form, style="Hint.TLabel", wraplength=px(self, 300))
        self.image_name.grid(row=6, column=1, sticky="w")
        self.hidden = tk.BooleanVar()
        self.hide = ttk.Checkbutton(form, text="Hidden", variable=self.hidden, command=self.set_hidden)
        self.hide.grid(row=7, column=1, sticky="w", pady=(6, 0))
        self.restore = ttk.Button(form, text="Back to the game's", command=self.reset)
        self.restore.grid(row=8, column=1, sticky="w", pady=(10, 0))
        self.note = ttk.Label(self.side, style="Hint.TLabel", wraplength=px(self, 440), justify="left")
        self.note.grid(row=5, column=0, sticky="w", pady=(8, 0))

    @property
    def project(self): return self.tab.project

    def layout(self): return section(self.project, "card_layout")

    def styles_dict(self, create=False):
        if create:
            if not self.styles_dict(): self.declare_switch()     # the layout's first style
            return ensure(self.project, "card_layout", "frame_styles")
        value = self.layout().get("frame_styles")
        return value if isinstance(value, dict) else {}

    def declare_switch(self):
        """The game reads a mod's card layout only while the mod's own
        "full_bleed" setting is on, so a first style declares it, on: the
        frame the modder sees here is the one the game draws (they can untick
        it in the Mods window, or give it another default in Mod info)."""
        info = self.tab.app.info
        if info in self.tab.app._pending or not manifest.declare_full_bleed(self.project):
            return
        info.refresh()      # its box would otherwise store the old list over this one

    def style(self, create=False):
        styles = self.styles_dict(create)
        if create:
            made = self.style_name.get() not in styles
            value = styles.setdefault(self.style_name.get(), {"hand_color": "gold"})
            # A first style should be usable immediately.  More styles can
            # still be assigned per card/type in the Cards tab or mod.json.
            if made and len(styles) == 1:
                ensure(self.project, "card_layout").setdefault("default_style", self.style_name.get())
            return value
        value = styles.get(self.style_name.get())
        return value if isinstance(value, dict) else {}

    def part(self, key, create=False):
        spec = SPECS[key]
        at = self.style(create)
        # ``image`` is an asset member, not another manifest object.  For a
        # digit strip we need its parent object (``digits``), because that is
        # where width/height/step and ATK/DEF placements live.
        path = spec[2][:-1] if key in IMAGE_PARTS and spec[2][-1] == "image" else spec[2]
        for name in path:
            if name == "image" and len(spec[2]) == 1:
                return at
            if create: at = at.setdefault(name, {})
            else:
                at = at.get(name)
                if not isinstance(at, dict): return {}
        return at if isinstance(at, dict) else {}

    def asset(self, key):
        path = SPECS[key][2]
        if key in DIRECT_ASSETS: return self.style().get(path[0])
        if key in IMAGE_PARTS: return self.part(key).get("image")
        return None

    def set_asset(self, key, image):
        path = SPECS[key][2]
        if key in DIRECT_ASSETS: set_member(self.style(True), path[0], image)
        elif key in IMAGE_PARTS:
            part = self.part(key, True)
            set_member(part, "image", image)
            # The game's replacement-caption API defaults omitted coordinates
            # to zero, unlike the transparent retail sprite.  Materialize the
            # editor's retail-aligned defaults when a PNG is first assigned so
            # saved preview and in-game placement agree.
            if image and key in CAPTION_KEYS:
                x, y, w, h = SPECS[key][3]
                part.setdefault("x", x); part.setdefault("y", y)
                part.setdefault("width", w); part.setdefault("height", h)

    def fill(self):
        if self.project is None: return
        styles = self.styles_dict()
        names = list(styles) or ["custom"]
        if self.style_name.get() not in names: self.style_name.set(names[0])
        self.style_list.delete(0, "end")
        for name in names:
            self.style_list.insert("end", name)
        self.style_list.selection_set(names.index(self.style_name.get()))
        self.fill_list(); self.draw(); self.fill_form()

    def reset_choice(self):
        self.chosen, self.list_chosen, self.preview_template = "frame", "frame", "main"

    def pick(self, iid, group=None):
        spec = DISPLAY_SPEC.get(iid)
        if spec is None:
            return
        self.list_chosen, self.preview_template, self.chosen = iid, PART_GROUP_TEMPLATE[spec[1]], spec[2]
        self.draw(); self.fill_form()

    def fill_list(self):
        rows = [(group, "", PART_GROUP_LABELS[group], (), False, ("group",))
                for group in PART_GROUP_ORDER]
        for iid, group, key, label in DISPLAY_PARTS:
            changed = bool(self.asset(key) or self.part(key))
            rows.append((iid, group, label, (), changed, ()))
        self.list.fill(rows, self.list_chosen)

    def rect(self, key):
        _, _, _, defaults, _, resize = SPECS[key]
        part = self.part(key)
        if key in ("frame", "back"):
            part = self.style()
        # A digit strip supplies one glyph size for both numeric values.
        # Show it on either ATK or DEF so the element users see is the one
        # whose size they edit, while retaining the runtime's shared strip.
        if key in ("atk", "def"):
            digits = self.style().get("digits")
            if not isinstance(digits, dict):
                digits = self.layout().get("digits")
            if not isinstance(digits, dict):
                digits = {}
            x = as_int(part.get("x"), defaults[0]); y = as_int(part.get("y"), defaults[1])
            w = as_int(digits.get("width"), defaults[2]); h = as_int(digits.get("height"), defaults[3])
            return x, y, max(1, w), max(1, h)
        x = as_int(part.get("x"), defaults[0]); y = as_int(part.get("y"), defaults[1])
        w = as_int(part.get("width"), defaults[2]); h = as_int(part.get("height"), defaults[3])
        return x, y, max(1, w), max(1, h)

    def size_part(self, key, create=False):
        """The manifest object which owns Width/Height for ``key``."""
        if key in ("atk", "def"):
            return self.part("digits", create)
        return self.style(create) if key in ("frame", "back") else self.part(key, create)

    def paint_picture(self, stage, key, x, y, w, h):
        image = mod_image(self.project, self.asset(key))
        if image: stage.picture(key, image, x, y, drag=key in MOVABLE, box=(x, y, w, h), high_quality=True)
        else: stage.rectangle(key, x, y, w, h, fill="#35415b", outline="#9da9bf", drag=key in MOVABLE)

    def preview_card_id(self):
        """The card selected in the assignment field, or the sample card."""
        if self.project is not None:
            cid = self.card.get()
            if cid in self.project.cards:
                return cid
        return 1       # Blue-Eyes remains the neutral sample before selection.

    def paint_small_template(self, kind):
        """Render one of the independent hand/field/Library targets.

        Those targets are deliberately not layered on the large viewer card.
        The hand gets representative retail overlays because that is how the
        game renders its 52x60 card sprite; their positions are fixed by the
        game rather than by a frame-style's main-card layout settings.
        """
        stage = self.stage
        stage.clear(); stage.rectangle(None, 0, 0, CARD_W, CARD_H, fill="#06080d")
        if kind == "library":
            key, w, h = "library", 24, 32
            self.paint_picture(stage, key, (CARD_W - w) // 2, (CARD_H - h) // 2, w, h)
            stage.outline(self.list_chosen)
            return
        if kind == "field":
            key, w, h = ("field_back", 52, 60) if self.list_chosen == "field_back" else ("field", 52, 60)
            self.paint_picture(stage, key, (CARD_W - w) // 2, (CARD_H - h) // 2, w, h)
            stage.outline(self.list_chosen)
            return

        # Duel hand: its style image is one 52x60 flat sprite.  Its artwork
        # and markers then use the style's independent hand placements.
        x, y, w, h = (CARD_W - 52) // 2, (CARD_H - 60) // 2, 52, 60
        self.paint_picture(stage, "hand", x, y, w, h)
        wa = getattr(getattr(self.tab.app, "files", None), "wa", None)
        retail_art = art.in_game(self.project, wa, self.preview_card_id(), "art") if wa else None
        try:
            font = card_text.RetailFont(wa) if wa else None
        except ValueError:
            font = None
        ax, ay, aw, ah = self.rect("hand_art")
        if self.part("hand_art").get("hide") is not True:
            if retail_art:
                stage.picture("hand_art", retail_art, x + ax, y + ay, box=(x + ax, y + ay, aw, ah), high_quality=True)
            else:
                stage.rectangle("hand_art", x + ax, y + ay, aw, ah, fill="#245175", outline="#b9c7da")
        else:
            stage.rectangle("hand_art", x + ax, y + ay, aw, ah, outline="#999", dash=(3, 3), drag=True)
        # Hand cards use the actual duel page's sword/shield sprites, rather
        # than the similarly sized card-bar icons. The source is one 8x16
        # retail cell; split it because these markers move independently.
        for key, icon in (("hand_atk_label", hand_sprite(wa, 120, 112)),
                          ("hand_dfd_label", hand_sprite(wa, 120, 120))):
            bx, by, bw, bh = self.rect(key)
            if self.part(key).get("hide") is True:
                stage.rectangle(key, x + bx, y + by, bw, bh, outline="#999", dash=(3, 3), drag=True)
                continue
            if icon:
                stage.picture(key, icon, x + bx, y + by, box=(x + bx, y + by, bw, bh), high_quality=True)
            else:
                stage.rectangle(key, x + bx, y + by, bw, bh, fill="#9da9bf", outline="#dfe8f6")
        for key, text in (("hand_atk", "3000"), ("hand_def", "2500")):
            bx, by, bw, bh = self.rect(key)
            if self.part(key).get("hide") is True:
                stage.rectangle(key, x + bx, y + by, bw * len(text), bh, outline="#999", dash=(3, 3), drag=True)
                continue
            # `Text_EncodeDecimalDigits` writes four values to the game's
            # 8x8 digit row. Draw those exact cells rather than generic text.
            for index, character in enumerate(text):
                digit = hand_sprite(wa, 8 * int(character), 88)
                dx = x + bx + index * bw
                if digit:
                    stage.picture((key, index), digit, dx, y + by,
                                  drag=False, box=(dx, y + by, bw, bh), high_quality=True)
                else:
                    stage.rectangle((key, index), dx, y + by, bw, bh, fill="#ffffff")
            stage.boxes[key] = (x + bx, y + by, bw * len(text), bh)
            stage.draggable.add(key)
        stage.outline(self.list_chosen)

    def draw(self):
        if self.project is None: return
        self.tab.draw_compared(self, self.paint)
        self.fill_list()

    def paint(self):
        if self.preview_template != "main":
            self.paint_small_template(self.preview_template)
            return
        stage = self.stage; stage.clear(); stage.rectangle(None, 0, 0, CARD_W, CARD_H, fill="#06080d")
        frame = "frame" if self.face.get() else "back"
        fx, fy, fw, fh = self.rect(frame)
        self.paint_picture(stage, frame, fx, fy, fw, fh)
        if not self.face.get(): stage.outline(self.chosen); return
        preview_kind = self.preview_kind.get()
        monster = preview_kind == "monster"
        # Use the card's actual viewer sprites.  The text font's icon atlas
        # has printed-type icons (its first one is Dragon) and a different
        # Star glyph, so it cannot represent these card-view elements.
        wa = getattr(getattr(self.tab.app, "files", None), "wa", None)
        retail_art = art.in_game(self.project, wa, self.preview_card_id(), "art") if wa else None
        try:
            font = card_text.RetailFont(wa) if wa else None
        except ValueError:
            font = None
        star_icon = card_icons.level_star(wa) if wa else None
        # The preview card is Blue-Eyes (Light) as a monster.  Non-monsters
        # use the card sheet's Magic or Trap ball instead, exactly as the
        # card viewer does (ritual/equip use Magic).
        kind_info = next((item for item in PREVIEW_KINDS if item[0] == preview_kind), PREVIEW_KINDS[0])
        caption, attribute_index = kind_info[2], kind_info[3]
        attr_icon = card_icons.attribute(wa, attribute_index) if wa else None
        retail_caption = card_icons.kind_label(wa, preview_kind) if caption and wa else None
        digits_image = mod_image(self.project, self.asset("digits"))
        parts = [("art", "#245175", "CARD ART"), ("title", "#ffffff", "Blue-eyes White Dragon")]
        if monster:
            parts += [("stars", "#ffcf41", ""), ("attribute", "#ffe171", ""),
                      ("atk", "#ffffff", "3000"), ("def", "#ffffff", "2500"),
                      ("atk_label", "#cce7ff", "ATK"), ("dfd_label", "#cce7ff", "DFD")]
        else:
            # One caption and one corresponding Magic/Trap attribute ball
            # exist on a non-monster card.  The caption's own style entry is
            # selected by the preview toggle, never by whichever row happens
            # to be selected in the inspector.
            parts += [("spell_icon", "#ffe171", ""), (caption, "#111111", CAPTION_TEXT[caption])]
        for key, color, text in parts:
            part = self.part(key)
            x, y, w, h = self.rect(key)
            digit_w, digit_h = w, h
            if key in ("atk", "def"):
                # The runtime stores a stat position as its centre and the
                # digit-strip dimensions as one glyph.  Expand the preview
                # to its four visible glyphs before putting that centre at
                # the requested x/y.
                step = as_int(self.part("digits").get("step"), digit_w)
                w = (len(text) - 1) * step + digit_w
                x -= w // 2
                y -= h // 2
            stage.axes[key] = "xy"
            if part.get("hide") is True:
                stage.rectangle(key, x, y, w, h, outline="#999", dash=(3, 3), drag=True); continue
            if key in ("art", "atk_label", "dfd_label", *CAPTION_KEYS) and self.asset(key):
                self.paint_picture(stage, key, x, y, w, h)
            elif key == "art" and retail_art: stage.picture(key, retail_art, x, y, box=(x, y, w, h), high_quality=True)
            elif key == "art": stage.rectangle(key, x, y, w, h, fill=color, outline="#b9c7da", drag=True)
            elif key == "stars" and star_icon:
                for n in range(12):
                    stage.picture((key, n), star_icon, x - n * 9, y, drag=False,
                                  box=(x - n * 9, y, 9, 9), high_quality=True)
                stage.boxes[key] = (x - 99, y, 108, 9); stage.draggable.add(key)
            elif key in ("attribute", "spell_icon") and attr_icon:
                stage.picture(key, attr_icon, x, y, box=(x, y, w, h), high_quality=True)
            elif key in CAPTION_KEYS and retail_caption:
                # No style image means the game keeps its own compact
                # caption sprite.  Showing that exact asset matters here:
                # generic text is much wider and makes a correctly placed
                # caption appear to be in the wrong place.
                stage.picture(key, retail_caption, x, y, box=(x, y, w, h), high_quality=True)
            elif key in ("atk", "def") and digits_image:
                step = as_int(self.part("digits").get("step"), digit_w)
                for n, char in enumerate(text):
                    image = digit_cell(digits_image, int(char))
                    if image:
                        dx = x + n * step
                        stage.picture((key, n), image, dx, y, drag=False,
                                      box=(dx, y, digit_w, digit_h), high_quality=True)
                stage.boxes[key] = (x, y, w, h)
                stage.draggable.add(key)
            elif key == "title":
                picture = title_plate_picture(text, w, h, color)
                if picture:
                    stage.picture(key, picture, x, y, box=(x, y, w, h), high_quality=True)
                else:
                    picture = text_picture(font, text, w, h, color)
                    if picture:
                        stage.picture(key, picture, x, y, box=(x, y, w, h), high_quality=True)
                    else:
                        stage.text(key, x, y + h // 2, text, color, max(6, h - 2), drag=True, anchor="w")
            else:
                # Raster text has a real box; Tk text picks its own bounds
                # and therefore hid Width/Height changes from the preview.
                picture = text_picture(font, text, w, h, color)
                if picture:
                    stage.picture(key, picture, x, y, box=(x, y, w, h), high_quality=True)
                else:
                    stage.text(key, x, y + h // 2, text, color, max(6, h - 2), drag=True, anchor="w")
        stage.outline(self.list_chosen)

    def select(self, key):
        # A click in a compact template must keep that template active rather
        # than jumping back to the large-card preview.
        if key is None:
            return
        if key in ("hand", "field", "field_back", "library"):
            self.pick(key)
            return
        if key.startswith("hand_"):
            self.pick(key)
            return
        if key not in SPECS: return
        self.chosen, self.list_chosen, self.preview_template = key, key, "main"
        self.list.show(key); self.stage.outline(key); self.fill_form()

    def changed_style(self):
        picked = self.style_list.curselection()
        if picked:
            self.style_name.set(self.style_list.get(picked[0]))
        self.fill()

    def moved(self, key, dx, dy):
        if key not in MOVABLE: return
        part = self.part(key, True)
        x, y, _, _ = self.rect(key)
        if key.startswith("hand_"):
            x = max(-HAND_COORD_LIMIT, min(HAND_COORD_LIMIT, x + dx))
            y = max(-HAND_COORD_LIMIT, min(HAND_COORD_LIMIT, y + dy))
        else:
            x, y = x + dx, y + dy
        set_member(part, "x", x, SPECS[key][3][0]); set_member(part, "y", y, SPECS[key][3][1])
        self.done()

    def dragging(self, key, dx, dy):
        if key == self.chosen and key in MOVABLE:
            x, y, _, _ = self.rect(key); self.vars["x"].set(str(x + dx)); self.vars["y"].set(str(y + dy))

    def wheel(self, key, step):
        if key not in SPECS or not SPECS[key][5]: return
        part = self.size_part(key, True)
        _, _, w, h = self.rect(key); default = SPECS[key][3]
        width_max = HAND_SIZE_LIMIT if key.startswith("hand_") else 140
        height_max = HAND_SIZE_LIMIT if key.startswith("hand_") else 196
        set_member(part, "width", max(1, min(width_max, w + step * 4)), default[2])
        set_member(part, "height", max(1, min(height_max, h + step * 4)), default[3]); self.done()

    def fill_form(self):
        if self.project is None: return
        self.loading = True; key = self.chosen; spec = SPECS[key]; x, y, w, h = self.rect(key)
        shared = " ATK and DEF share this digit-strip glyph size." if key in ("atk", "def") else ""
        label = DISPLAY_SPEC.get(self.list_chosen, (None, None, None, spec[1]))[3]
        self.heading.configure(text=label)
        hand_note = (" Five-digit values use five cells; keep X + 5 × Width within the 52px hand card."
                     if key in ("hand_atk", "hand_def") else "")
        self.what.configure(text="Drag on the preview or use the fields. Wheel resizes elements that have a size." + shared + hand_note)
        for name, value in (("x", x), ("y", y), ("width", w), ("height", h)): self.vars[name].set(str(value))
        for name in ("x", "y", "width", "height"):
            visible = (name in ("x", "y") and key in MOVABLE) or (name in ("width", "height") and spec[5])
            for widget in self.rows[name]:
                (widget.grid if visible else widget.grid_remove)()
        self.hidden.set(self.part(key).get("hide") is True)
        self.hide.state(["!disabled"] if spec[4] else ["disabled"])
        image = self.asset(key); self.image_name.configure(text=image or "the game's artwork")
        editable_image = key in DIRECT_ASSETS or key in IMAGE_PARTS
        self.choose.state(["!disabled"] if editable_image else ["disabled"])
        self.remove.state(["!disabled"] if image and editable_image else ["disabled"])
        self.restore.state(["!disabled"])
        self.note.configure(text="Assets are saved in frames/. The first style becomes the layout's default and declares the mod's “full_bleed” setting, on by default (Mod info): untick it in the Mods window to see the game's own frame. The front preview is a game-scale card viewer; hand, field and Library assets are selectable here but belong to their own game screens.")
        self.loading = False

    def typed(self, name):
        if self.loading or self.project is None: return
        try: value = int(self.vars[name].get())
        except ValueError: return
        key, spec = self.chosen, SPECS[self.chosen]
        if name in ("x", "y") and key not in MOVABLE: return
        if name in ("width", "height") and not spec[5]: return
        part = self.size_part(key, True) if name in ("width", "height") else \
            (self.style(True) if key in ("frame", "back") else self.part(key, True))
        default = {"x": spec[3][0], "y": spec[3][1], "width": spec[3][2], "height": spec[3][3]}[name]
        if key.startswith("hand_"):
            minimum = -HAND_COORD_LIMIT if name in ("x", "y") else 1
            limit = HAND_COORD_LIMIT if name in ("x", "y") else HAND_SIZE_LIMIT
        else:
            minimum = -320
            limit = 140 if name == "width" else 196 if name == "height" else 320
        set_member(part, name, max(minimum, min(limit, value)), default); self.done()

    def choose_image(self):
        if self.chosen not in DIRECT_ASSETS | IMAGE_PARTS: return
        image = import_image(self, self.project, f"{self.style_name.get()}-{self.chosen}", FRAME_IMAGE_DIR)
        if image: self.set_asset(self.chosen, image); self.done()

    def remove_image(self):
        if self.chosen in DIRECT_ASSETS | IMAGE_PARTS:
            self.set_asset(self.chosen, None); self.done()

    def set_hidden(self):
        if not self.loading and SPECS[self.chosen][4]: set_member(self.part(self.chosen, True), "hide", self.hidden.get(), None); self.done()

    def reset(self):
        key = self.chosen; path = SPECS[key][2]
        if key in DIRECT_ASSETS: self.style(True).pop(path[0], None)
        else:
            part = self.part(key, True); part.clear()
        self.done()

    def new_style(self):
        name = simpledialog.askstring("New frame style", "Style name (for mod.json):", parent=self)
        if not name: return
        name = "".join(c for c in name if c.isalnum() or c in "_- ").strip().replace(" ", "_")
        if not name: return
        self.styles_dict(True).setdefault(name, {"hand_color": "gold"}); self.style_name.set(name); self.done()

    def style_references(self, name):
        """Return every manifest assignment that points at *name*.

        A style name is only useful while it exists in ``frame_styles``.  Keep
        these references together so deleting a style cannot leave a manifest
        that silently falls through to retail at some later point.
        """
        layout = self.project.other.get("card_layout", {})
        references = []
        if layout.get("default_style") == name:
            references.append(("Global default", layout, "default_style"))
        defaults = layout.get("default_styles", {})
        if not isinstance(defaults, dict):
            defaults = {}
        for group, label in (("kinds", "Kind"), ("types", "Printed type")):
            values = defaults.get(group, {})
            if isinstance(values, dict):
                for key, value in values.items():
                    if value == name:
                        references.append((f"{label}: {key}", values, key))
        rules = layout.get("frame_for")
        for rule in rules if isinstance(rules, list) else ():
            if isinstance(rule, dict) and rule.get("style") == name:
                limits = ", ".join(f"{k} {rule[k]}" for k in ("class", "tag", "setting") if k in rule)
                references.append((f"Rule: {limits or 'every card'}", rule, "style"))
        def card_label(cid):
            card = self.project.cards.get(cid)
            return card.name if card is not None else str(cid)

        for cid, entry in self.project.card_extra.items():
            if entry.get("frame_style") == name:
                references.append((f"Card: {card_label(cid)}", entry, "frame_style"))
        for cid, added in self.project.added.items():
            if added.extra.get("frame_style") == name:
                references.append((f"Card: {card_label(cid)}", added.extra, "frame_style"))
        return references

    def delete_style(self):
        name = self.style_name.get()
        styles = self.styles_dict(True)
        if name not in styles:
            return
        references = self.style_references(name)
        if references:
            listed = "\n".join(f"• {label}" for label, _, _ in references)
            if not messagebox.askyesno(
                    "Delete frame style?",
                    f'"{name}" is assigned to:\n{listed}\n\n'
                    "Delete the style and clear these assignments?",
                    parent=self):
                return
            for _, owner, key in references:
                owner.pop(key, None)
            # Do not leave empty bookkeeping objects behind after their last
            # assignment was cleared.  Added cards own their extra mapping;
            # retail replacements can be removed entirely.
            layout = self.project.other.get("card_layout", {})
            defaults = layout.get("default_styles")
            if isinstance(defaults, dict):
                for group in ("kinds", "types"):
                    if not defaults.get(group):
                        defaults.pop(group, None)
                if not defaults:
                    layout.pop("default_styles", None)
            # A frame_for rule is nothing without its style: drop it whole.
            rules = layout.get("frame_for")
            if isinstance(rules, list):
                rules[:] = [r for r in rules if not isinstance(r, dict) or "style" in r]
                if not rules:
                    layout.pop("frame_for", None)
            for cid, entry in list(self.project.card_extra.items()):
                if not entry:
                    self.project.card_extra.pop(cid, None)
        styles.pop(name)
        self.style_name.set(next(iter(styles), "custom")); self.done()

    def set_default(self):
        ensure(self.project, "card_layout")["default_style"] = self.style_name.get(); self.done()

    def reset_default(self):
        """Remove the global fallback so unmatched cards use retail."""
        self.project.other.get("card_layout", {}).pop("default_style", None)
        self.done()

    def set_kind(self):
        ensure(self.project, "card_layout", "default_styles", "kinds")[self.kind.get()] = self.style_name.get()
        self.done()

    def reset_kind(self):
        """Remove this kind fallback; resolution continues to the global/retail tier."""
        layout = self.project.other.get("card_layout", {})
        defaults = layout.get("default_styles", {})
        kinds = defaults.get("kinds", {})
        kinds.pop(self.kind.get(), None)
        self.done()

    def set_type(self):
        name = self.type_name.get().strip()
        if name:
            ensure(self.project, "card_layout", "default_styles", "types")[name] = self.style_name.get()
            self.done()

    def reset_type(self):
        """Remove this printed-type fallback; resolution continues to kind/global/retail."""
        name = self.type_name.get().strip()
        layout = self.project.other.get("card_layout", {})
        defaults = layout.get("default_styles", {})
        types = defaults.get("types", {})
        if name:
            types.pop(name, None)
        self.done()

    def set_card(self):
        """Give one card an explicit style, above all default-style rules."""
        cid = self.card.get()
        if not cid or cid not in self.project.cards:
            return
        # Added cards preserve manifest-only keys on their added-card entry;
        # retail cards use the project's ordinary replace-entry extras.
        if cid in self.project.added:
            entry = self.project.added[cid].extra
        else:
            entry = self.project.card_extra.setdefault(cid, {})
        entry["frame_style"] = self.style_name.get()
        self.tab.app.changed()
        self.tab.mark_reverts()

    def reset_card(self):
        """Remove this card's explicit style and resume normal fallback resolution."""
        cid = self.card.get()
        if not cid or cid not in self.project.cards:
            return
        if cid in self.project.added:
            entry = self.project.added[cid].extra
        else:
            entry = self.project.card_extra.get(cid)
        if entry is not None:
            entry.pop("frame_style", None)
        self.tab.app.changed()
        self.tab.mark_reverts()

    def done(self):
        self.tab.changed("card_layout"); self.fill()
