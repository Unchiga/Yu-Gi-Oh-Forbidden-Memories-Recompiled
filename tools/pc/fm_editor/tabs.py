"""The editor's tabs: Cards, Fusions, Equips, Rituals, Duelists, Mod info
and Conflicts. Each works on app.project and calls app.changed() after an
edit."""
from __future__ import annotations

import json
import tkinter as tk
from tkinter import messagebox, ttk

from . import bulk_dialog, guardian_stars, manifest, pools as poolmath, validate
from .gamedata import (FUSION_GROUPS, ATTRIBUTE_NAMES, CARD_COUNT, DECK_COPY_LIMIT, DECK_SIZE, DUELIST_NAMES, FRAME_NAMES,
                       POOL_LABELS, POOL_TOTAL, POOLS, STAR_NAMES, STARTER_WEIGHT_LIMIT, TYPE_EQUIP, TYPE_NAMES, TYPE_RITUAL,
                       exodia_piece, type_frame)
from . import fixed_decks
from .fixed_deck_view import FixedDeckView
from .model import KEY_RE, StarterDeck
from .widgets import CardField, FormDialog, ScrolledForm, card_matches, card_named, grab, pick_card, px, scrolled_tree, show_text, ui_font

ATTRIBUTE_CHOICES = ATTRIBUTE_NAMES + ["6 (magic)", "7 (trap)"]
STAR_CHOICES = ["(none)"] + STAR_NAMES[1:]
FRAME_CHOICES = ["By type"] + FRAME_NAMES
# Each frame's colour, as the hand's frames have it (the duel's palette rows 1-6).
FRAME_COLOURS = ["#e0a838", "#409830", "#b040a0", "#2848b0", "#8868d8", "#e07000"]


def type_label(t: int) -> str:
    return TYPE_NAMES[t] if 0 <= t < len(TYPE_NAMES) else str(t)


def attribute_label(a: int) -> str:
    return ATTRIBUTE_CHOICES[a] if 0 <= a < len(ATTRIBUTE_CHOICES) else str(a)


def frame_label(f: int) -> str:
    return FRAME_CHOICES[f + 1] if -1 <= f < len(FRAME_NAMES) else str(f)


def star_choices(project=None) -> list:
    """The stars a card may have in this mod (guardian_stars.choices)."""
    return guardian_stars.choices(project.other.get("guardian_stars") if project is not None else None)


def star_label(s: int, project=None) -> str:
    choices = star_choices(project)
    return choices[s] if 0 <= s < len(choices) else str(s)


def parse_choice(text: str, choices) -> int:
    if text in choices:
        return choices.index(text)
    head = text.split(" ", 1)[0]
    return int(head) if head.lstrip("-").isdigit() else -1


class Tab(ttk.Frame):
    def __init__(self, notebook, app, title):
        super().__init__(notebook, padding=6)
        self.app = app
        notebook.add(self, text=title)

    @property
    def project(self):
        return self.app.project

    def refresh(self):
        pass

    def commit(self):
        """Store what the tab's form holds; False when it cannot."""
        return True


# --- Cards --------------------------------------------------------------------

class CardsTab(Tab):
    FILTERS = ["All cards", "Changed", "Added by the mod", "With notes", "Monsters", "Non-monsters"] + TYPE_NAMES

    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Cards")
        self.current = None
        self._shown_price = ""
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        left = ttk.Frame(self)
        left.grid(row=0, column=0, sticky="nsew")
        top = ttk.Frame(left)
        top.pack(fill="x")
        ttk.Label(top, text="Search").pack(side="left")
        self.search = tk.StringVar()
        ttk.Entry(top, textvariable=self.search, width=24).pack(side="left", padx=4)
        self.filter = tk.StringVar(value=self.FILTERS[0])
        ttk.Combobox(top, textvariable=self.filter, values=self.FILTERS, state="readonly", width=16).pack(side="left")
        self.search.trace_add("write", lambda *_: self.fill())
        self.filter.trace_add("write", lambda *_: self.fill())
        frame, self.tree = scrolled_tree(left, [("id", "#"), ("name", "Name"), ("type", "Type"), ("atk", "ATK"),
                                                ("def", "DEF"), ("state", "")], [50, 230, 100, 50, 50, 60], 24, sort_numeric=("id", "atk", "def"))
        frame.pack(fill="both", expand=True, pady=4)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.select())
        bottom = ttk.Frame(left)
        bottom.pack(side="bottom", fill="x", before=frame)
        ttk.Button(bottom, text="Add a card (copy of the selected one)", command=self.add_card).pack(side="left")
        self.count = ttk.Label(bottom)
        self.count.pack(side="right")

        self.card_scroll = ScrolledForm(self)
        self.card_scroll.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        form = ttk.LabelFrame(self.card_scroll.body, text="Card", padding=8)
        form.pack(fill="both", expand=True)
        self.form = form
        self.vars = {k: tk.StringVar() for k in ("name", "attack", "defense", "type", "attribute", "level",
                                                  "star1", "star2", "password", "starchips", "key", "frame")}
        row = 0

        def line(label, widget, hint=None):
            nonlocal row
            ttk.Label(form, text=label).grid(row=row, column=0, sticky="w", pady=2)
            widget.grid(row=row, column=1, sticky="we", pady=2)
            if hint is not None:
                hint.grid(row=row, column=2, sticky="w", padx=6)
            row += 1
            return widget

        self.title = ttk.Label(form, font=ui_font(11))
        self.title.grid(row=row, column=0, columnspan=3, sticky="w", pady=(0, 6))
        row += 1
        self.hints = {}

        def hint(key):
            self.hints[key] = ttk.Label(form, style="Hint.TLabel")
            return self.hints[key]

        line("Name", ttk.Entry(form, textvariable=self.vars["name"], width=32), hint("name"))
        line("Type", ttk.Combobox(form, textvariable=self.vars["type"], values=TYPE_NAMES, state="readonly", width=18),
             hint("type"))
        line("Attribute", ttk.Combobox(form, textvariable=self.vars["attribute"], values=ATTRIBUTE_CHOICES,
                                       state="readonly", width=18), hint("attribute"))
        line("Level", ttk.Spinbox(form, textvariable=self.vars["level"], from_=0, to=12, width=8), hint("level"))
        line("ATK", ttk.Spinbox(form, textvariable=self.vars["attack"], from_=0, to=5110, increment=10, width=8),
             hint("attack"))
        line("DEF", ttk.Spinbox(form, textvariable=self.vars["defense"], from_=0, to=5110, increment=10, width=8),
             hint("defense"))
        self.star_boxes = [ttk.Combobox(form, textvariable=self.vars[key], values=STAR_CHOICES, state="readonly",
                                        width=18) for key in ("star1", "star2")]
        line("Guardian star 1", self.star_boxes[0], hint("star1"))
        line("Guardian star 2", self.star_boxes[1], hint("star2"))
        line("Password", ttk.Entry(form, textvariable=self.vars["password"], width=12), hint("password"))
        self.price = line("Starchips", ttk.Entry(form, textvariable=self.vars["starchips"], width=12), hint("starchips"))
        ttk.Label(form, text="0 = free; empty = default price", style="Hint.TLabel").grid(
            row=row, column=1, columnspan=2, sticky="w")
        row += 1
        ttk.Label(form, text="Card text").grid(row=row, column=0, sticky="nw", pady=2)
        # 21 columns: the game's 20 letters a line and room for the cursor.
        self.text = tk.Text(form, width=21, height=9, wrap="word", font=("Consolas", 10))
        self.text.grid(row=row, column=1, columnspan=2, sticky="w", pady=2)
        row += 1
        self.lines = ttk.Label(form, style="Hint.TLabel")
        self.lines.grid(row=row, column=1, columnspan=2, sticky="w")
        row += 1
        self.text.bind("<KeyRelease>", lambda e: self.count_lines())
        # The frame the card view, the Library and the duel draw it in: its
        # type's unless the mod picks one (cards.c "frame").
        ttk.Label(form, text="Frame").grid(row=row, column=0, sticky="w", pady=2)
        ttk.Combobox(form, textvariable=self.vars["frame"], values=FRAME_CHOICES, state="readonly",
                     width=18).grid(row=row, column=1, sticky="we", pady=2)
        beside = ttk.Frame(form)
        beside.grid(row=row, column=2, sticky="w", padx=6)
        self.swatch = tk.Label(beside, width=2, relief="solid", borderwidth=1)
        self.swatch.pack(side="left")
        self.hints["frame"] = ttk.Label(beside, style="Hint.TLabel")
        self.hints["frame"].pack(side="left", padx=(6, 0))
        row += 1
        self.vars["frame"].trace_add("write", lambda *_: self.show_swatch())
        self.vars["type"].trace_add("write", lambda *_: self.show_swatch())
        ttk.Label(form, text="Notes").grid(row=row, column=0, sticky="nw", pady=2)
        self.notes = tk.Text(form, width=36, height=4, wrap="word", undo=True)
        self.notes.grid(row=row, column=1, columnspan=2, sticky="we", pady=2)
        row += 1
        self.added_frame = ttk.LabelFrame(form, text="Added card", padding=6)
        self.added_frame.grid(row=row, column=0, columnspan=3, sticky="we", pady=6)
        row += 1
        ttk.Label(self.added_frame, text="Stable id").grid(row=0, column=0, sticky="w")
        ttk.Entry(self.added_frame, textvariable=self.vars["key"], width=24).grid(row=0, column=1, sticky="w")
        self.base_label = ttk.Label(self.added_frame)
        self.base_label.grid(row=1, column=0, columnspan=2, sticky="w")
        self.drops = tk.BooleanVar()
        self.opponents = tk.BooleanVar()
        ttk.Checkbutton(self.added_frame, text="Can be won in its base's place", variable=self.drops).grid(
            row=2, column=0, columnspan=2, sticky="w")
        ttk.Checkbutton(self.added_frame, text="Opponents' decks can deal it in its base's place",
                        variable=self.opponents).grid(row=3, column=0, columnspan=2, sticky="w")
        ttk.Label(self.added_frame, style="Hint.TLabel", wraplength=px(form, 320), justify="left",
                  text="A new card starts in nobody's chest. Players win it in its base's place (above), "
                       "from a starter deck (Starter decks tab) or with Game > Cheats > Give. Its password "
                       "is shown in View > Card passwords only: the Password screen sells the disc's cards.").grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(4, 0))
        ttk.Button(self.added_frame, text="Remove this card", command=self.remove_card).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self.extra = ttk.Label(form, style="Hint.TLabel", wraplength=px(form, 320), justify="left")
        self.extra.grid(row=row, column=0, columnspan=3, sticky="w")
        row += 1
        buttons = ttk.Frame(form)
        buttons.grid(row=row, column=0, columnspan=3, sticky="we", pady=(8, 0))
        ttk.Button(buttons, text="Apply", command=self.apply).pack(side="left")
        ttk.Button(buttons, text="Revert to retail", command=self.revert).pack(side="left", padx=4)
        self.status = ttk.Label(form, style="Error.TLabel", wraplength=px(form, 320), justify="left")
        self.status.grid(row=row + 1, column=0, columnspan=3, sticky="w", pady=(6, 0))
        for child in form.winfo_children():
            if isinstance(child, (ttk.Entry, ttk.Spinbox)):
                child.bind("<Return>", lambda e: self.apply())
        self.fill()
        self.show(None)

    # the list
    def wanted(self, cid) -> bool:
        card = self.project.cards[cid]
        f = self.filter.get()
        if f == "Changed" and not self.project.card_changed(cid):
            return False
        if f == "Added by the mod" and cid not in self.project.added:
            return False
        if f == "With notes" and cid not in self.project.notes:
            return False
        if f == "Monsters" and not card.is_monster():
            return False
        if f == "Non-monsters" and card.is_monster():
            return False
        if f in TYPE_NAMES and card.type != TYPE_NAMES.index(f):
            return False
        search = self.search.get()
        return card_matches(self.project, cid, search) or (
            bool(search.strip()) and search.lower().strip() in self.project.notes.get(cid, "").lower())

    def row(self, cid):
        card = self.project.cards[cid]
        state = ("added" if cid in self.project.added else "changed" if self.project.card_changed(cid)
                 else "notes" if cid in self.project.notes else "")
        return (cid, card.name, type_label(card.type), card.attack, card.defense, state), (state,) if state else ()

    def fill(self):
        if not hasattr(self, "tree") or self.project is None:
            return
        self.tree.delete(*self.tree.get_children())
        shown = 0
        for cid in sorted(self.project.cards):
            if self.wanted(cid):
                values, tags = self.row(cid)
                self.tree.insert("", "end", iid=str(cid), values=values, tags=tags)
                shown += 1
        self.tree.sorting.apply()
        self.count.configure(text=f"{shown} cards")
        if self.current and self.tree.exists(str(self.current)):
            self.tree.selection_set(str(self.current))
            self.tree.see(str(self.current))

    def refresh(self):
        self.current = None
        self.fill()
        self.show(None)

    def update_row(self, cid):
        if self.tree.exists(str(cid)):
            values, tags = self.row(cid)
            self.tree.item(str(cid), values=values, tags=tags)
            self.tree.sorting.apply()

    def select(self):
        selection = self.tree.selection()
        cid = int(selection[0]) if selection else None
        if cid == self.current:
            return
        if self.current is not None and not self.apply(quiet=True):
            self.tree.selection_set(str(self.current))    # stay on the card whose form cannot be stored
            return
        self.show(cid)

    def goto(self, cid):
        self.search.set("")
        self.filter.set(self.FILTERS[0])
        self.fill()
        if self.tree.exists(str(cid)):
            self.tree.selection_set(str(cid))
            self.tree.see(str(cid))

    # the form
    def show(self, cid):
        self.current = cid
        self.status.configure(text="")
        state = "normal" if cid else "disabled"
        for child in self.form.winfo_children():
            try:
                if isinstance(child, ttk.Combobox):
                    child.configure(state="readonly" if cid else "disabled")
                elif isinstance(child, (ttk.Entry, ttk.Spinbox, ttk.Button)):
                    child.configure(state=state)
            except tk.TclError:
                pass
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.notes.configure(state="normal")
        self.notes.delete("1.0", "end")
        self.notes.edit_reset()
        if not cid:
            self.title.configure(text="Select a card")
            for var in self.vars.values():
                var.set("")
            self.added_frame.grid_remove()
            self.extra.configure(text="")
            for label in self.hints.values():
                label.configure(text="")
            self.lines.configure(text="")
            self.text.configure(state="disabled")
            self.notes.configure(state="disabled")
            if self.app.text_preview is not None:
                self.app.text_preview.later()
            return
        card = self.project.cards[cid]
        self.title.configure(text=f"#{cid}" + ("  (added by the mod)" if cid in self.project.added else ""))
        self.vars["name"].set(card.name)
        self.vars["attack"].set(card.attack)
        self.vars["defense"].set(card.defense)
        self.vars["type"].set(type_label(card.type))
        self.vars["attribute"].set(attribute_label(card.attribute))
        self.vars["level"].set(card.level)
        # The mod's stars (the Guardian Stars tab) are in the lists too.
        for box in self.star_boxes:
            box.configure(values=star_choices(self.project))
        self.vars["star1"].set(star_label(card.star1, self.project))
        self.vars["star2"].set(star_label(card.star2, self.project))
        self.vars["frame"].set(frame_label(card.frame))
        self.vars["password"].set(self.project.password(cid))
        price = self.project.starchip_cost(cid)
        self._shown_price = "" if price is None else str(price)
        self.vars["starchips"].set(self._shown_price)
        self.price.configure(state="normal")
        self.hints["starchips"].configure(text=(
            f"Retail: {self.project.retail.starchips.get(cid, 'unknown')}" if cid in self.project.retail.cards
            else "Default: 999999"))
        self.text.insert("1.0", card.description)
        self.notes.insert("1.0", self.project.notes.get(cid, ""))
        self.notes.edit_reset()
        self.count_lines()
        reference = self.project.retail.cards.get(cid) or self.project.cards.get(self.project.base_of(cid))
        what = "Retail" if cid in self.project.retail.cards else "Base"
        for key, label in (("name", reference.name), ("type", type_label(reference.type)),
                           ("attribute", attribute_label(reference.attribute)), ("level", reference.level),
                           ("attack", reference.attack), ("defense", reference.defense),
                           ("star1", star_label(reference.star1)), ("star2", star_label(reference.star2)),
                           ("frame", frame_label(reference.frame).lower())):
            self.hints[key].configure(text=f"{what}: {label}" if key != "name" or len(str(label)) < 28 else what)
        if cid in self.project.retail.cards:
            self.hints["password"].configure(text=f"Retail: {self.project.retail.passwords.get(cid) or 'none'}")
        else:
            self.hints["password"].configure(text="Card view only")
        if cid in self.project.added:
            added = self.project.added[cid]
            self.vars["key"].set(added.key)
            self.drops.set(added.drops)
            self.opponents.set(added.opponents)
            self.base_label.configure(text=f"Copy of {self.project.card_label(added.base)}; "
                                           f"identity {self.project.identity(cid)}")
            self.added_frame.grid()
            extra = added.extra
        else:
            self.added_frame.grid_remove()
            extra = self.project.card_extra.get(cid, {})
        self.extra.configure(text=("Kept as written in mod.json: " + ", ".join(sorted(extra))) if extra else "")

    def show_swatch(self):
        """The colour the frame will be: the chosen one, or the type's."""
        frame = parse_choice(self.vars["frame"].get(), FRAME_CHOICES) - 1
        kind = parse_choice(self.vars["type"].get(), TYPE_NAMES)
        if frame < 0 and kind >= 0:
            frame = type_frame(kind)
        if 0 <= frame < len(FRAME_COLOURS):
            self.swatch.configure(background=FRAME_COLOURS[frame])
        else:
            self.swatch.configure(background=self.swatch.master.winfo_toplevel().cget("background"))

    def count_lines(self):
        text = self.text.get("1.0", "end-1c")
        lines = validate.text_lines(text)
        self.lines.configure(text=f"{lines} of 8 lines (20 letters a line, as the game wraps it)",
                             style="Error.TLabel" if lines > 8 else "Hint.TLabel")
        if self.app.text_preview is not None:
            self.app.text_preview.later()

    def read_form(self, cid):
        """The card as the form has it, or an error text."""
        card = self.project.cards[cid].copy()
        try:
            card.attack = int(self.vars["attack"].get())
            card.defense = int(self.vars["defense"].get())
            card.level = int(self.vars["level"].get())
        except ValueError:
            return "ATK, DEF and level are whole numbers"
        card.name = self.vars["name"].get()
        card.description = self.text.get("1.0", "end-1c")
        values = [parse_choice(self.vars["type"].get(), TYPE_NAMES),
                  parse_choice(self.vars["attribute"].get(), ATTRIBUTE_CHOICES),
                  parse_choice(self.vars["star1"].get(), star_choices(self.project)),
                  parse_choice(self.vars["star2"].get(), star_choices(self.project))]
        if min(values) < 0:
            return "choose a type, an attribute and two stars"
        card.type, card.attribute, card.star1, card.star2 = values
        card.frame = max(-1, parse_choice(self.vars["frame"].get(), FRAME_CHOICES) - 1)
        return card

    def apply(self, quiet=False):
        cid = self.current
        if not cid or cid not in self.project.cards:
            return True
        card = self.read_form(cid)
        if isinstance(card, str):
            self.status.configure(text=card)
            return False
        password = self.vars["password"].get().strip()
        if password and not (len(password) <= 8 and password.isdigit() and password.isascii()):
            self.status.configure(text="a password is up to 8 digits, or empty for none")
            return False
        password = password.zfill(8) if password else ""
        price_text = self.vars["starchips"].get().strip()
        price = None
        if price_text:
            if not price_text.isascii() or not price_text.isdigit() or len(price_text) > 6:
                self.status.configure(text="Starchips is a whole number from 0 to 999999, or empty for the default")
                return False
            price = int(price_text)
        price_changed = price_text != self._shown_price
        if price_changed and not isinstance(self.project.other.get("passwords", {}), dict):
            self.status.configure(text='The mod\'s "passwords" must be an object before editing Starchips')
            return False
        changed = not card.same(self.project.cards[cid])
        if cid in self.project.added:
            added = self.project.added[cid]
            key = self.vars["key"].get().strip()
            if key != added.key:
                if not KEY_RE.match(key) or any(a.key == key for a in self.project.added.values()):
                    self.status.configure(text="the stable id is letters, digits, _ and -, and unique")
                    return False
                self.project.set_card_key(cid, key)
                changed = True
            if (added.drops, added.opponents) != (self.drops.get(), self.opponents.get()):
                added.drops, added.opponents = self.drops.get(), self.opponents.get()
                changed = True
        # Stored with the rest, once the form has passed every check.
        notes = self.notes.get("1.0", "end-1c")
        if notes != self.project.notes.get(cid, "") and (notes.strip() or cid in self.project.notes):
            self.project.set_notes(cid, notes)
            changed = True
        if password != self.project.password(cid):
            self.project.set_password(cid, password)
            self.vars["password"].set(password)
            changed = True
        if price_changed:
            self.project.set_starchips(cid, price)
            changed = True
        cost = self.project.starchip_cost(cid)
        self._shown_price = "" if cost is None else str(cost)
        self.vars["starchips"].set(self._shown_price)
        if changed:
            self.project.cards[cid] = card
            self.app.changed()
            self.update_row(cid)
        problems = [i.message for i in validate.validate_card(self.project, cid)] if changed or not quiet else []
        self.status.configure(text="\n".join(problems))
        return True

    def commit(self):
        return self.apply(quiet=True)

    def revert(self):
        cid = self.current
        if not cid:
            return
        self.project.revert_card(cid)
        self.app.changed()
        self.update_row(cid)
        self.show(cid)

    def add_card(self):
        base = self.current or pick_card(self, self.project, "Base of the new card", only=lambda c: c <= CARD_COUNT)
        if not base:
            return
        if not self.apply(quiet=True):
            return
        source = self.project.cards[base]
        base = self.project.base_of(base)
        cid = self.project.add_card(base)
        # The selected card as it is (an added card's own stats too); its base
        # is the disc's card under it.
        self.project.cards[cid] = source.copy(id=cid, name=source.name + " II")
        self.app.changed()
        self.filter.set(self.FILTERS[0])
        self.search.set("")
        self.fill()
        self.current = None
        self.tree.selection_set(str(cid))
        self.tree.see(str(cid))

    def remove_card(self):
        cid = self.current
        if cid not in self.project.added:
            return
        if not messagebox.askyesno("Remove card", f"Remove {self.project.card_label(cid)} and every fusion, "
                                   "equip, ritual and pool entry that names it?", parent=self):
            return
        self.project.remove_card(cid)
        self.app.changed()
        self.current = None
        self.fill()
        self.show(None)


# --- Fusions -------------------------------------------------------------------

class FusionsTab(Tab):
    LIMIT = 3000

    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Fusions")
        top = ttk.Frame(self)
        top.pack(fill="x")
        ttk.Label(top, text="Card (name or #)").pack(side="left")
        self.search = tk.StringVar()
        ttk.Entry(top, textvariable=self.search, width=28).pack(side="left", padx=4)
        self.changed_only = tk.BooleanVar()
        ttk.Checkbutton(top, text="Changed only", variable=self.changed_only, command=self.fill).pack(side="left")
        self.search.trace_add("write", lambda *_: self.fill())
        self.count = ttk.Label(top)
        self.count.pack(side="right")
        frame, self.tree = scrolled_tree(self, [("a", "Card A"), ("b", "Card B"), ("result", "Result"),
                                                ("state", "")], [260, 260, 260, 80], 24, selectmode="extended")
        frame.pack(fill="both", expand=True, pady=4)
        self.tree.bind("<Double-1>", lambda e: self.edit())
        buttons = ttk.Frame(self)
        buttons.pack(fill="x")
        ttk.Button(buttons, text="Add fusion...", command=self.add).pack(side="left")
        ttk.Button(buttons, text="Change result...", command=self.edit).pack(side="left", padx=4)
        ttk.Button(buttons, text="Remove (no fusion)", command=self.remove).pack(side="left")
        ttk.Button(buttons, text="Remove recipes of...", command=self.remove_result).pack(side="left", padx=(4, 0))
        ttk.Button(buttons, text="Revert to retail", command=self.revert).pack(side="left", padx=4)
        ttk.Button(buttons, text="Bulk...", command=lambda: bulk_dialog.open_bulk(self)).pack(side="left")
        ttk.Label(buttons, text="A pair fuses the same in either order. Brown rows are the retail table's "
                                "\"glitch\" fusions.", style="Hint.TLabel").pack(side="right")

    def refresh(self):
        self.fill()

    def fill(self):
        if self.project is None:
            return
        self.tree.delete(*self.tree.get_children())
        p = self.project
        text = self.search.get().strip()
        named = {pair for pair in p.own_fusion_pairs()[0] if pair[0] in p.cards and pair[1] in p.cards}
        # A card's own "fusions" list makes what no rule of the mod decides
        # first: the row shows what the game plays. A copy's pair its base's
        # rule decides is no row of its own (it would read "forbidden").
        removes = set(p.active_removes()) if named else set()
        own = {pair: p.own_fusion(pair, removes) for pair in named}
        pairs = set(p.fusions) | set(p.retail.fusions) | p.fusion_explicit | \
            {pair for pair, made in own.items() if made is not None}
        rows = []
        for pair in pairs:
            status = "own list" if own.get(pair) is not None else p.fusion_status(pair)
            if self.changed_only.get() and status in ("", "glitch"):
                continue
            result = own.get(pair) if status == "own list" else p.fusions.get(pair) or p.retail.fusions.get(pair)
            if text and not (card_matches(p, pair[0], text) or card_matches(p, pair[1], text)
                             or card_matches(p, result, text)):
                continue
            rows.append((pair, status))
        rows.sort()
        for pair, status in rows[:self.LIMIT]:
            result = p.fusions.get(pair)
            retail = p.retail.fusions.get(pair)
            if status == "own list":
                shown = f"{p.card_label(own[pair]) if own[pair] else '(none)'} (a card's own fusions list)"
            else:
                shown = p.card_label(result) if result else \
                    f"(none; retail {p.card_label(retail)})" if retail else "(none: forbidden)"
            tag = "changed" if status == "own list" else status
            self.tree.insert("", "end", iid=f"{pair[0]}:{pair[1]}", tags=(tag,) if tag else (),
                             values=(p.card_label(pair[0]), p.card_label(pair[1]), shown, status))
        more = f" (first {self.LIMIT} shown; search to narrow)" if len(rows) > self.LIMIT else ""
        self.count.configure(text=f"{len(rows)} fusions{more}")

    def selected(self):
        return [tuple(int(x) for x in iid.split(":")) for iid in self.tree.selection()]

    def dialog(self, title, pair=None, result=None):
        fields = {}

        def build(dialog, body):
            for i, (key, label) in enumerate((("a", "Card A"), ("b", "Card B"), ("r", "Result"))):
                ttk.Label(body, text=label).grid(row=i, column=0, sticky="w", pady=2)
                fields[key] = CardField(body, lambda: self.project, width=36)
                fields[key].grid(row=i, column=1, sticky="we", pady=2)
            if pair:
                fields["a"].set(pair[0])
                fields["b"].set(pair[1])
            if result:
                fields["r"].set(result)

        def ok(dialog):
            a, b, r = fields["a"].get(), fields["b"].get(), fields["r"].get()
            if not (a and b and r):
                return "name three cards (a number, a name, or pick one with ...)"
            if pair and self.project.pair(a, b) != pair:
                self.project.set_fusion(pair[0], pair[1], None)     # the fusion moved to other cards
            self.project.set_fusion(a, b, r)
            self.app.changed()
            self.search.set(self.project.card_label(a).split(" ", 1)[1] if " " in self.project.card_label(a) else "")
            self.fill()
            return None

        FormDialog(self, title, build, ok)

    def add(self):
        self.dialog("Add fusion")

    def edit(self):
        chosen = self.selected()
        if chosen:
            pair = chosen[0]
            self.dialog("Change fusion", pair, self.project.fusions.get(pair) or self.project.retail.fusions.get(pair))

    def remove(self):
        if not self.selected():
            return
        for pair in self.selected():
            self.project.set_fusion(pair[0], pair[1], None)
        self.app.changed()
        self.fill()

    def revert(self):
        if not self.selected():
            return
        for pair in self.selected():
            self.project.revert_fusion(pair)
        self.app.changed()
        self.fill()

    def remove_result(self):
        """{"remove": card} (notes/gameplay-tables.md): one rule takes away
        every disc recipe of the card; reverting one of them writes it back."""
        fields = {}
        chosen = self.selected()

        def build(dialog, body):
            ttk.Label(body, text="Result").grid(row=0, column=0, sticky="w", pady=2)
            fields["r"] = CardField(body, lambda: self.project, width=36)
            fields["r"].grid(row=0, column=1, sticky="we", pady=2)
            if chosen:
                fields["r"].set(self.project.fusions.get(chosen[0]) or self.project.retail.fusions.get(chosen[0]))
            ttk.Label(body, text="No recipe on the disc makes this card any more: one \"remove\" rule in place of\n"
                                 "a rule per pair. The mod's own fusions, and an added card's own recipes, still\n"
                                 "make it. Revert a pair to bring that recipe back.",
                      style="Hint.TLabel").grid(row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))

        def ok(dialog):
            result = fields["r"].get()
            if not result:
                return "name the card (a number, a name, or pick one with ...)"
            if not self.project.retail_recipes(result):
                return f"no recipe on the disc makes {self.project.card_label(result)}"
            self.project.remove_recipes(result)
            self.app.changed()
            self.fill()
            return None

        dialog = FormDialog(self, "Remove disc recipes", build, ok)
        dialog.fields = fields
        return dialog


# --- Equips ---------------------------------------------------------------------

class EquipsTab(Tab):
    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Equips")
        self.current = None
        left = ttk.Frame(self)
        left.pack(side="left", fill="y")
        ttk.Label(left, text="Equip cards").pack(anchor="w")
        frame, self.equips = scrolled_tree(left, [("id", "#"), ("name", "Equip card"), ("n", "Fits")],
                                           [50, 200, 50], 26, sort_numeric=("id", "n"))
        frame.pack(fill="y", expand=True)
        self.equips.bind("<<TreeviewSelect>>", lambda e: self.select())
        right = ttk.Frame(self)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))
        self.heading = ttk.Label(right, font=ui_font(11))
        self.heading.pack(anchor="w")
        frame, self.monsters = scrolled_tree(right, [("id", "#"), ("name", "Monster"), ("type", "Type"),
                                                     ("atk", "ATK"), ("def", "DEF"), ("state", "")],
                                                     [50, 220, 100, 50, 50, 80], 22, selectmode="extended",
                                                     sort_numeric=("id", "atk", "def"))
        frame.pack(fill="both", expand=True, pady=4)
        buttons = ttk.Frame(right)
        buttons.pack(fill="x")
        ttk.Button(buttons, text="Add a monster...", command=self.add).pack(side="left")
        self.type_choice = tk.StringVar(value=TYPE_NAMES[0])
        ttk.Button(buttons, text="Add every", command=lambda: self.by_type(True)).pack(side="left", padx=(8, 2))
        ttk.Combobox(buttons, textvariable=self.type_choice, values=TYPE_NAMES[:20], state="readonly",
                     width=14).pack(side="left")
        ttk.Button(buttons, text="Remove every", command=lambda: self.by_type(False)).pack(side="left", padx=2)
        ttk.Button(buttons, text="Remove selected", command=self.remove).pack(side="left", padx=(8, 0))
        ttk.Button(buttons, text="Revert to retail", command=self.revert).pack(side="left", padx=4)

    def refresh(self):
        self.current = None
        self.fill_equips()
        self.fill()

    def fill_equips(self):
        if self.project is None:
            return
        self.equips.delete(*self.equips.get_children())
        p = self.project
        cards = sorted(set(p.equip_cards()) | set(p.equips))
        for cid in cards:
            if cid not in p.cards:
                continue
            now = p.equips.get(cid, set())
            changed = now != p.equip_baseline(cid)
            self.equips.insert("", "end", iid=str(cid), values=(cid, p.cards[cid].name, len(now)),
                               tags=("changed",) if changed else ())
        self.equips.sorting.apply()
        if self.current and self.equips.exists(str(self.current)):
            self.equips.selection_set(str(self.current))

    def select(self):
        selection = self.equips.selection()
        if selection and int(selection[0]) != self.current:
            self.current = int(selection[0])
            self.fill()

    def fill(self):
        self.monsters.delete(*self.monsters.get_children())
        p = self.project
        if not self.current or p is None:
            self.heading.configure(text="Select an equip card")
            return
        self.heading.configure(text=f"{p.card_label(self.current)} may equip:")
        now = p.equips.get(self.current, set())
        retail = p.equip_baseline(self.current)
        for cid in sorted(now | retail):
            state = "" if cid in now and cid in retail else "added" if cid in now else "removed"
            card = p.cards.get(cid)
            if card is None:
                continue
            self.monsters.insert("", "end", iid=str(cid), values=(
                cid, card.name, type_label(card.type), card.attack, card.defense, state),
                tags=(state,) if state else ())
        self.monsters.sorting.apply()

    def edited(self):
        self.app.changed()
        self.fill()
        self.fill_equips()

    def add(self):
        if not self.current:
            return
        cid = pick_card(self, self.project, "Monster it may equip", only=lambda c: 0 <= self.project.cards[c].type < 20)
        if cid:
            self.project.equips.setdefault(self.current, set()).add(cid)
            self.edited()

    def by_type(self, allow):
        if not self.current:
            return
        t = TYPE_NAMES.index(self.type_choice.get())
        members = {cid for cid, card in self.project.cards.items() if card.type == t}
        now = self.project.equips.setdefault(self.current, set())
        if allow:
            now |= members
        else:
            now -= members
        self.edited()

    def remove(self):
        if not self.current:
            return
        now = self.project.equips.setdefault(self.current, set())
        for iid in self.monsters.selection():
            now.discard(int(iid))
        self.edited()

    def revert(self):
        if self.current:
            self.project.equips[self.current] = self.project.equip_baseline(self.current)
            self.edited()


# --- Rituals ---------------------------------------------------------------------

class RitualsTab(Tab):
    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Rituals")
        frame, self.tree = scrolled_tree(self, [("ritual", "Ritual card"), ("t1", "Tribute 1"), ("t2", "Tribute 2"),
                                                ("t3", "Tribute 3"), ("result", "Summons"), ("state", "")],
                                         [210, 170, 170, 170, 200, 70], 24)
        frame.pack(fill="both", expand=True)
        self.tree.bind("<Double-1>", lambda e: self.edit())
        buttons = ttk.Frame(self)
        buttons.pack(fill="x", pady=(4, 0))
        ttk.Button(buttons, text="Edit recipe...", command=self.edit).pack(side="left")
        ttk.Button(buttons, text="Remove recipe", command=self.remove).pack(side="left", padx=4)
        ttk.Button(buttons, text="Revert to retail", command=self.revert).pack(side="left")
        ttk.Label(buttons, text="A ritual is a ritual card (an added copy has its base's recipe until given its "
                                "own); the three tributes are monsters on the field; custom recipes may use conditions.",
                  style="Hint.TLabel").pack(side="right")

    def refresh(self):
        self.fill()

    def fill(self):
        if self.project is None:
            return
        p = self.project
        self.tree.delete(*self.tree.get_children())
        for ritual in sorted(set(p.ritual_cards()) | set(p.rituals) | set(p.retail.rituals)):
            if ritual not in p.cards:
                continue
            conditional = ritual in p.ritual_requirements
            state = p.ritual_status(ritual)
            recipe = p.rituals.get(ritual) or (None, None, None, None)
            if ritual in p.added and ritual not in p.rituals and not conditional:
                # Without a recipe of its own, a copy is its base's ritual.
                recipe = p.rituals.get(p.base_of(ritual)) or recipe
                state = state or "as base"
            labels = [p.card_label(c) if c else "-" for c in recipe]
            if conditional:
                for i, req in enumerate(p.ritual_requirements[ritual]):
                    parts = []
                    if req.get("card"):
                        parts.append(p.card_label(req["card"]))
                    if req.get("type") is not None:
                        parts.append(str(req["type"]))
                    if req.get("fusion_group") is not None:
                        parts.append(f'Group: {req["fusion_group"]}')
                    if req.get("min_attack") is not None:
                        parts.append(f'ATK ≥ {req["min_attack"]}')
                    if req.get("min_defense") is not None:
                        parts.append(f'DEF ≥ {req["min_defense"]}')
                    if req.get("max_attack") is not None:
                        parts.append(f'ATK ≤ {req["max_attack"]}')
                    if req.get("max_defense") is not None:
                        parts.append(f'DEF ≤ {req["max_defense"]}')
                    if req.get("min_level") is not None:
                        parts.append(f'Level ≥ {req["min_level"]}')
                    if req.get("max_level") is not None:
                        parts.append(f'Level ≤ {req["max_level"]}')
                    if req.get("defense_gt_attack"):
                        parts.append("DEF > ATK")
                    labels[i] = " & ".join(parts) or "-"
            self.tree.insert("", "end", iid=str(ritual), values=[p.card_label(ritual)] + labels + [state],
                             tags=(state,) if state else ())

    def selected(self):
        selection = self.tree.selection()
        return int(selection[0]) if selection else None

    def edit(self):
        ritual = self.selected()
        if not ritual:
            return
        p = self.project
        recipe = (p.rituals.get(ritual) or p.retail.rituals.get(ritual) or p.rituals.get(p.base_of(ritual))
                  or (0, 0, 0, 0))      # an added copy starts from its base's
        saved = p.ritual_requirements.get(ritual)
        requirements = [dict(r) for r in saved] if saved else [{"card": recipe[i]} if recipe[i] else {} for i in range(3)]

        dialog = tk.Toplevel(self)
        dialog.title("Ritual recipe")
        dialog.transient(self)
        dialog.resizable(True, False)
        dialog.minsize(px(dialog, 620), 0)
        grab(dialog)
        dialog.bind("<Escape>", lambda e: dialog.destroy())
        dialog.protocol("WM_DELETE_WINDOW", dialog.destroy)
        body = ttk.Frame(dialog, padding=12)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text=self.project.card_label(ritual), font=ui_font(10)).pack(
            anchor="w", pady=(0, 8))

        panels = []
        numeric_inputs = [dict(), dict(), dict()]
        card_inputs = [None, None, None]   # a Specific Card's text, read on Save
        open_index = tk.IntVar(value=0)

        def describe(req):
            parts = []
            if req.get("card"):
                parts.append("Specific Card")
            if req.get("type") is not None:
                parts.append("Monster Type")
            if req.get("fusion_group") is not None:
                parts.append("Fusion Group")
            if req.get("min_attack") is not None:
                parts.append("Minimum ATK")
            if req.get("min_defense") is not None:
                parts.append("Minimum DEF")
            if req.get("max_attack") is not None:
                parts.append("Maximum ATK")
            if req.get("max_defense") is not None:
                parts.append("Maximum DEF")
            if req.get("min_level") is not None:
                parts.append("Minimum Level")
            if req.get("max_level") is not None:
                parts.append("Maximum Level")
            if req.get("defense_gt_attack"):
                parts.append("DEF > ATK")
            return parts

        def render(index):
            panel = panels[index][1]
            for child in panel.winfo_children():
                child.destroy()
            req = requirements[index]
            kinds = describe(req)
            for kind in kinds:
                row = ttk.Frame(panel)
                row.pack(fill="x", pady=2)
                ttk.Label(row, text=kind, width=18).pack(side="left")
                if kind == "Specific Card":
                    field = CardField(row, lambda: self.project, width=36,
                                      only=lambda c: 0 <= self.project.cards[c].type < 20)
                    field.pack(side="left", fill="x", expand=True)
                    # Typed text survives a redraw, as the numbers' does.
                    if card_inputs[index] is not None:
                        field.var.set(card_inputs[index].get())
                    else:
                        field.set(req["card"])
                    card_inputs[index] = field.var
                elif kind == "Monster Type":
                    value = tk.StringVar(value=req["type"])
                    combo = ttk.Combobox(row, textvariable=value, values=TYPE_NAMES[:20], state="readonly", width=22)
                    combo.pack(side="left")
                    combo.bind("<<ComboboxSelected>>", lambda e, v=value, r=req: r.__setitem__("type", v.get()))
                elif kind == "Fusion Group":
                    value = tk.StringVar(value=req["fusion_group"])
                    combo = ttk.Combobox(row, textvariable=value, values=FUSION_GROUPS, state="readonly", width=22)
                    combo.pack(side="left")
                    combo.bind("<<ComboboxSelected>>",
                               lambda e, v=value, r=req: r.__setitem__("fusion_group", v.get()))
                elif kind in ("Minimum ATK", "Minimum DEF", "Maximum ATK", "Maximum DEF", "Minimum Level", "Maximum Level"):
                    if kind == "Minimum ATK":
                        key, limit = "min_attack", 9999
                    elif kind == "Minimum DEF":
                        key, limit = "min_defense", 9999
                    elif kind == "Maximum ATK":
                        key, limit = "max_attack", 9999
                    elif kind == "Maximum DEF":
                        key, limit = "max_defense", 9999
                    elif kind == "Minimum Level":
                        key, limit = "min_level", 12
                    else:
                        key, limit = "max_level", 12
                    # Keep pending edits when changing panels or adding another
                    # condition rebuilds the widgets. Save validates the text.
                    previous = numeric_inputs[index].get(key)
                    value = previous[0] if previous else tk.StringVar(value=str(req[key]))
                    entry = ttk.Entry(row, textvariable=value, width=10)
                    entry.pack(side="left")
                    numeric_inputs[index][key] = (value, limit)
                else:
                    ttk.Label(row, text="Required").pack(side="left")
                def delete(k=kind, r=req, n=index):
                    keys = {"Specific Card": "card", "Monster Type": "type", "Fusion Group": "fusion_group",
                            "Minimum ATK": "min_attack", "Minimum DEF": "min_defense",
                            "Maximum ATK": "max_attack", "Maximum DEF": "max_defense",
                            "Minimum Level": "min_level", "Maximum Level": "max_level",
                            "DEF > ATK": "defense_gt_attack"}
                    if len(describe(r)) <= 1:
                        return
                    r.pop(keys[k], None)
                    numeric_inputs[n].pop(keys[k], None)
                    if keys[k] == "card":
                        card_inputs[n] = None
                    render(n)
                ttk.Button(row, text="Remove", command=delete,
                           state="normal" if len(kinds) > 1 else "disabled").pack(side="right", padx=(6, 0))
            ttk.Button(panel, text="+ Add requirement", command=lambda n=index: add_requirement(n)).pack(
                anchor="w", pady=(6, 0))

        def add_requirement(index):
            req = requirements[index]
            menu = tk.Menu(dialog, tearoff=False)
            choices = [("Specific Card", "card"), ("Monster Type", "type"), ("Fusion Group", "fusion_group"),
                       ("Minimum ATK", "min_attack"), ("Minimum DEF", "min_defense"),
                       ("Maximum ATK", "max_attack"), ("Maximum DEF", "max_defense"),
                       ("Minimum Level", "min_level"), ("Maximum Level", "max_level"),
                       ("DEF > ATK", "defense_gt_attack")]
            def add(key):
                if key in req:
                    return
                if key == "card":
                    cid = pick_card(dialog, self.project, "Specific ritual tribute",
                                    only=lambda c: 0 <= self.project.cards[c].type < 20)
                    if not cid: return
                    req[key] = cid
                elif key == "type":
                    req[key] = TYPE_NAMES[0]
                elif key == "fusion_group":
                    req[key] = "Elf"
                elif key in ("min_attack", "min_defense", "max_attack", "max_defense"):
                    req[key] = 1000
                elif key == "min_level":
                    req[key] = 1
                elif key == "max_level":
                    req[key] = 12
                else:
                    req[key] = True
                render(index)
            for label, key in choices:
                menu.add_command(label=label, state="disabled" if key in req else "normal",
                                 command=lambda k=key: add(k))
            widget = panels[index][1].winfo_children()[-1]
            menu.tk_popup(widget.winfo_rootx(), widget.winfo_rooty() + widget.winfo_height())

        def show_panel(index):
            open_index.set(index)
            for i, (button, panel) in enumerate(panels):
                button.configure(text=("▼ " if i == index else "▶ ") + f"TRIBUTE {i + 1}")
                if i == index:
                    panel.pack(fill="x", padx=(18, 0), pady=(2, 8))
                    render(i)
                else:
                    panel.pack_forget()

        for i in range(3):
            section = ttk.Frame(body)
            section.pack(fill="x")
            header = ttk.Button(section, command=lambda n=i: show_panel(n))
            header.pack(fill="x")
            panel = ttk.Frame(section, padding=(4, 4))
            panels.append((header, panel))
        show_panel(0)

        ttk.Separator(body).pack(fill="x", pady=(4, 8))
        summon = ttk.Frame(body)
        summon.pack(fill="x")
        ttk.Label(summon, text="Summons", width=18).pack(side="left")
        result = CardField(summon, lambda: self.project, width=38, only=lambda c: 0 <= self.project.cards[c].type < 20)
        result.pack(side="left", fill="x", expand=True)
        if recipe[3]:
            result.set(recipe[3])
        error = ttk.Label(body, style="Error.TLabel")
        error.pack(fill="x", pady=(6, 0))
        buttons = ttk.Frame(body)
        buttons.pack(fill="x", pady=(8, 0))

        def fail(index, text):
            error.configure(text=f"Tribute {index + 1}: {text}")
            show_panel(index)

        def save():
            for index, req in enumerate(requirements):
                if "card" in req and card_inputs[index] is not None:
                    text = card_inputs[index].get().strip()
                    cid = card_named(self.project, text)
                    if text and not cid:
                        return fail(index, f"no card \"{text}\".")
                    if cid:
                        req["card"] = cid
                    else:
                        req.pop("card")
                for key, (value, limit) in numeric_inputs[index].items():
                    try:
                        number = int(value.get())
                    except ValueError:
                        return fail(index, "ATK, DEF and Level requirements must be whole numbers.")
                    if not 0 <= number <= limit:
                        return fail(index, f"{key.replace('_', ' ').title()} must be between 0 and {limit}.")
                    req[key] = number
            result_id = result.get()
            for index, req in enumerate(requirements):
                for low, high, what in (("min_attack", "max_attack", "ATK"), ("min_defense", "max_defense", "DEF"),
                                        ("min_level", "max_level", "Level")):
                    if req.get(low) is not None and req.get(high) is not None and req[low] > req[high]:
                        return fail(index, f"Minimum {what} cannot be greater than Maximum {what}.")
                if not req:
                    return fail(index, "needs at least one requirement.")
            if not result_id:
                error.configure(text="Summons must name a monster.")
                return
            display = [req.get("card", 0) for req in requirements]
            self.project.rituals[ritual] = tuple(display + [result_id])
            traditional = all(set(req) == {"card"} for req in requirements)
            if traditional:
                self.project.ritual_requirements.pop(ritual, None)
            else:
                self.project.ritual_requirements[ritual] = [dict(req) for req in requirements]
            self.app.changed()
            self.fill()
            dialog.destroy()

        ttk.Button(buttons, text="Save", command=save).pack(side="right")
        ttk.Button(buttons, text="Cancel", command=dialog.destroy).pack(side="right", padx=4)
        dialog.bind("<Control-s>", lambda e: save())

    def remove(self):
        ritual = self.selected()
        if ritual:
            self.project.rituals.pop(ritual, None)
            self.project.ritual_requirements.pop(ritual, None)
            self.app.changed()
            self.fill()

    def revert(self):
        ritual = self.selected()
        if ritual:
            self.project.revert_ritual(ritual)
            self.app.changed()
            self.fill()


# --- Duelists ---------------------------------------------------------------------

class DuelistsTab(Tab):
    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Duelists")
        self.duelist = 1
        left = ttk.Frame(self)
        left.pack(side="left", fill="y")
        frame, self.list = scrolled_tree(left, [("id", "#"), ("name", "Opponent"), ("state", "")],
                                         [36, 170, 60], 26, sort_numeric=("id",))
        frame.pack(fill="y", expand=True)
        self.list.bind("<<TreeviewSelect>>", lambda e: self.select())
        right = ttk.Frame(self)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))
        top = ttk.Frame(right)
        top.pack(fill="x")
        self.pool = tk.StringVar(value="deck")
        for pool in POOLS:
            ttk.Radiobutton(top, text=POOL_LABELS[pool], value=pool, variable=self.pool,
                            command=self.fill).pack(side="left", padx=(0, 8))
        self.total = ttk.Label(top, font=ui_font(10))
        self.total.pack(side="right")
        frame, self.tree = scrolled_tree(right, [("id", "#"), ("name", "Card"), ("type", "Type"),
                                                 ("atk", "ATK"), ("def", "DEF"), ("w", "Weight"),
                                                 ("pct", "Chance"), ("retail", "Retail"), ("state", "")],
                                         [50, 220, 100, 50, 50, 60, 60, 60, 70], 22, selectmode="extended",
                                         sort_numeric=("id", "atk", "def", "w", "pct", "retail"))
        frame.pack(fill="both", expand=True, pady=4)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.pick_row())
        edit = ttk.Frame(right)
        edit.pack(fill="x")
        self.fixed = FixedDeckView(self, right, top)     # the deck pool may be forty cards written down
        ttk.Button(edit, text="Add a card...", command=self.add).pack(side="left")
        ttk.Label(edit, text="Weight").pack(side="left", padx=(10, 2))
        self.weight = tk.StringVar()
        entry = ttk.Entry(edit, textvariable=self.weight, width=7)
        entry.pack(side="left")
        entry.bind("<Return>", lambda e: self.set_weight())
        ttk.Button(edit, text="Set", command=self.set_weight).pack(side="left", padx=2)
        ttk.Button(edit, text="Remove selected", command=self.remove).pack(side="left", padx=(8, 0))
        ttk.Button(edit, text="Normalize to 2048", command=self.normalize).pack(side="left", padx=4)
        ttk.Button(edit, text="Revert pool", command=self.revert).pack(side="left")
        ttk.Label(right, text="Weights are chances out of 2048. A deck is 40 cards dealt from at least 14; "
                              "a drop pool needs one card left.", style="Hint.TLabel").pack(anchor="w", pady=(4, 0))

    def refresh(self):
        self.fill_list()
        self.fill()

    def fill_list(self):
        if self.project is None:
            return
        self.list.delete(*self.list.get_children())
        for d, name in enumerate(DUELIST_NAMES[:len(self.project.pools)]):
            changed = any({c: w for c, w in self.project.pools[d][p].items() if w} != self.project.retail.pools[d][p]
                          for p in POOLS)
            state = "fixed" if fixed_decks.deck_of(self.project, d) else "changed" if changed else ""
            self.list.insert("", "end", iid=str(d), values=(d, name, state), tags=("changed",) if state else ())
        self.list.sorting.apply()
        if self.list.exists(str(self.duelist)):
            self.list.selection_set(str(self.duelist))

    def select(self):
        selection = self.list.selection()
        if selection:
            self.duelist = int(selection[0])
            self.fill()

    def current_pool(self):
        return self.project.pools[self.duelist][self.pool.get()]

    def fill(self):
        if self.project is None or self.fixed.fill():
            return
        p = self.project
        self.tree.delete(*self.tree.get_children())
        pool = self.current_pool()
        retail = p.retail.pools[self.duelist][self.pool.get()]
        for cid in sorted(set(pool) | set(retail), key=lambda c: (-pool.get(c, 0), c)):
            weight, before = pool.get(cid, 0), retail.get(cid, 0)
            if not weight and not before:
                continue
            state = "" if weight == before else "added" if not before else "removed" if not weight else "changed"
            card = p.cards.get(cid)
            self.tree.insert("", "end", iid=str(cid), tags=(state,) if state else (), values=(
                cid, card.name if card else "?", type_label(card.type) if card else "",
                card.attack if card else "", card.defense if card else "", weight,
                f"{weight * 100 / POOL_TOTAL:.2f}%", before, state))
        self.tree.sorting.apply()
        total = sum(pool.values())
        cards = sum(1 for w in pool.values() if w)
        self.total.configure(text=f"{DUELIST_NAMES[self.duelist]}: {cards} cards, total {total} / {POOL_TOTAL}",
                             style="Ok.TLabel" if total == POOL_TOTAL else "Error.TLabel")

    def pick_row(self):
        selection = self.tree.selection()
        if len(selection) == 1:
            self.weight.set(str(self.current_pool().get(int(selection[0]), 0)))

    def edited(self):
        pool = self.current_pool()
        for cid in [c for c, w in pool.items() if not w]:
            del pool[cid]
        self.app.changed()
        self.fill()
        self.fill_list()

    def add(self):
        cid = pick_card(self, self.project, "Card to add to the pool")
        if not cid:
            return
        try:
            weight = int(self.weight.get() or "0")
        except ValueError:
            weight = 0
        self.current_pool()[cid] = weight if weight > 0 else 1
        self.edited()
        if self.tree.exists(str(cid)):
            self.tree.selection_set(str(cid))
            self.tree.see(str(cid))

    def set_weight(self):
        try:
            weight = int(self.weight.get())
        except ValueError:
            messagebox.showerror("Weight", "A weight is a whole number, 0 or more.", parent=self)
            return
        if weight < 0 or weight > 0xFFFF:
            messagebox.showerror("Weight", "A weight is 0 to 65535 (out of 2048).", parent=self)
            return
        pool = self.current_pool()
        chosen = [int(i) for i in self.tree.selection()]
        for cid in chosen:
            pool[cid] = weight
        self.edited()
        for cid in chosen:
            if self.tree.exists(str(cid)):
                self.tree.selection_add(str(cid))

    def remove(self):
        pool = self.current_pool()
        for iid in self.tree.selection():
            pool.pop(int(iid), None)
        self.edited()

    def normalize(self):
        self.project.pools[self.duelist][self.pool.get()] = poolmath.normalize(self.current_pool())
        self.edited()

    def revert(self):
        self.project.revert_pool(self.duelist, self.pool.get())
        self.edited()

    def goto(self, target):
        d, pool = target
        self.duelist = d
        self.pool.set(pool)
        self.fill_list()
        self.list.see(str(d))
        self.fill()


# --- Starter decks ---------------------------------------------------------------

class StarterTab(Tab):
    """The decks a new game may be dealt in place of the disc's weighted
    pools ("starter", notes/starter-deck.md). The disc has none of these, so
    every deck here is the mod's own: the list is what it offers, and one of
    them is picked for each new game, by their weights."""

    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Starter decks")
        self.deck = 0
        left = ttk.Frame(self)
        left.pack(side="left", fill="y")
        frame, self.list = scrolled_tree(left, [("n", "#"), ("name", "Deck"), ("w", "Weight"), ("cards", "Cards")],
                                         [30, 150, 55, 60], 22)
        frame.pack(fill="y", expand=True)
        self.list.bind("<<TreeviewSelect>>", lambda e: self.select())
        buttons = ttk.Frame(left)
        buttons.pack(fill="x", pady=(4, 0))
        ttk.Button(buttons, text="Add deck", command=self.add_deck).pack(side="left")
        ttk.Button(buttons, text="Edit...", command=self.edit_deck).pack(side="left", padx=2)
        ttk.Button(buttons, text="Remove", command=self.remove_deck).pack(side="left")
        right = ttk.Frame(self)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))
        top = ttk.Frame(right)
        top.pack(fill="x")
        self.title = ttk.Label(top, font=ui_font(10))
        self.title.pack(side="left")
        self.total = ttk.Label(top, font=ui_font(10))
        self.total.pack(side="right")
        frame, self.tree = scrolled_tree(right, [("id", "#"), ("name", "Card"), ("type", "Type"),
                                                 ("copies", "Copies"), ("state", "")],
                                         [50, 260, 110, 60, 150], 20, selectmode="extended")
        frame.pack(fill="both", expand=True, pady=4)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.pick_row())
        edit = ttk.Frame(right)
        edit.pack(fill="x")
        ttk.Button(edit, text="Add a card...", command=self.add_card).pack(side="left")
        ttk.Label(edit, text="Copies").pack(side="left", padx=(10, 2))
        self.copies = tk.StringVar()
        entry = ttk.Entry(edit, textvariable=self.copies, width=5)
        entry.pack(side="left")
        entry.bind("<Return>", lambda e: self.set_copies())
        ttk.Button(edit, text="Set", command=self.set_copies).pack(side="left", padx=2)
        ttk.Button(edit, text="Remove selected", command=self.remove_card).pack(side="left", padx=(8, 0))
        ttk.Label(right, text=f"A deck is exactly {DECK_SIZE} cards written down, so it may hold a card the mod "
                              f"adds. More than {DECK_COPY_LIMIT} copies, or more than one Exodia piece, is dealt "
                              "as written but Build Deck will not take it back.",
                  style="Hint.TLabel", wraplength=px(self, 520), justify="left").pack(anchor="w", pady=(4, 0))

    # --- the list ----------------------------------------------------------

    def decks(self):
        return self.project.starter if self.project else []

    def current(self):
        decks = self.decks()
        return decks[self.deck] if 0 <= self.deck < len(decks) else None

    def refresh(self):
        self.fill_list()
        self.fill()

    def fill_list(self):
        if self.project is None:
            return
        self.list.delete(*self.list.get_children())
        for i, deck in enumerate(self.decks()):
            self.list.insert("", "end", iid=str(i), tags=() if deck.complete() else ("error",),
                             values=(i + 1, deck.name or "(unnamed)", deck.weight, f"{deck.total()}/{DECK_SIZE}"))
        if self.decks():
            self.deck = min(self.deck, len(self.decks()) - 1)
            if self.list.exists(str(self.deck)):
                self.list.selection_set(str(self.deck))

    def select(self):
        selection = self.list.selection()
        if selection:
            self.deck = int(selection[0])
            self.fill()

    def fill(self):
        if self.project is None:
            return
        deck = self.current()
        self.tree.delete(*self.tree.get_children())
        if deck is None:
            self.title.configure(text="No starter deck")
            self.total.configure(text="", style="TLabel")
            return
        p = self.project
        for cid in sorted(deck.cards):
            copies = deck.cards[cid]
            card = p.cards.get(cid)
            notes = []
            if copies > DECK_COPY_LIMIT:
                notes.append(f"over {DECK_COPY_LIMIT}")
            if exodia_piece(cid) and copies > 1:
                notes.append("Exodia piece")
            self.tree.insert("", "end", iid=str(cid), tags=("warning",) if notes else (), values=(
                cid, card.name if card else "?", type_label(card.type) if card else "", copies, ", ".join(notes)))
        # A card the editor could not place keeps its place in the deck, and
        # its row, so its copies are not quietly lost.
        for name, copies in deck.kept.items():
            self.tree.insert("", "end", iid=f"kept:{name}", tags=("removed",),
                             values=("", name, "", copies, "no such card; kept as written"))
        self.title.configure(text=deck.name or "(unnamed)")
        total = deck.total()
        self.total.configure(text=f"{total} / {DECK_SIZE} cards",
                             style="Ok.TLabel" if total == DECK_SIZE else "Error.TLabel")

    def pick_row(self):
        selection = self.tree.selection()
        deck = self.current()
        if len(selection) == 1 and deck and not selection[0].startswith("kept:"):
            self.copies.set(str(deck.cards.get(int(selection[0]), 0)))

    def edited(self):
        deck = self.current()
        if deck:
            for cid in [c for c, n in deck.cards.items() if not n]:
                del deck.cards[cid]
        self.app.changed()
        self.fill()
        self.fill_list()

    # --- decks -------------------------------------------------------------

    def deck_dialog(self, title, deck):
        fields = {}

        def build(dialog, body):
            ttk.Label(body, text="Name").grid(row=0, column=0, sticky="w", pady=2)
            fields["name"] = tk.StringVar(value=deck.name)
            ttk.Entry(body, textvariable=fields["name"], width=32).grid(row=0, column=1, sticky="we", pady=2)
            ttk.Label(body, text="Weight").grid(row=1, column=0, sticky="w", pady=2)
            fields["weight"] = tk.StringVar(value=str(deck.weight))
            ttk.Entry(body, textvariable=fields["weight"], width=10).grid(row=1, column=1, sticky="w", pady=2)
            ttk.Label(body, text=f"How often this deck is the one picked, against the other decks\n"
                                 f"offered. 0 is a deck that is kept but never picked.",
                      style="Hint.TLabel").grid(row=2, column=0, columnspan=2, sticky="w", pady=(6, 0))

        def ok(dialog):
            text = fields["weight"].get().strip()
            if not text.isdigit() or int(text) > STARTER_WEIGHT_LIMIT:
                return f"a weight is a whole number, 0 to {STARTER_WEIGHT_LIMIT}"
            deck.name = fields["name"].get().strip()
            deck.weight = int(text)
            self.app.changed()
            self.fill()
            self.fill_list()
            return None

        FormDialog(self, title, build, ok)

    def add_deck(self):
        if self.project is None:
            return
        deck = StarterDeck(name=f"Deck {len(self.decks()) + 1}")
        self.project.starter.append(deck)
        self.deck = len(self.decks()) - 1
        self.edited()
        if self.list.exists(str(self.deck)):
            self.list.selection_set(str(self.deck))
        self.deck_dialog("Add starter deck", deck)

    def edit_deck(self):
        deck = self.current()
        if deck:
            self.deck_dialog("Starter deck", deck)

    def remove_deck(self):
        deck = self.current()
        if deck is None:
            return
        if not messagebox.askyesno("Remove deck", f"Remove {deck.name or 'this deck'} and its "
                                                  f"{deck.total()} cards?", parent=self):
            return
        self.project.starter.pop(self.deck)
        self.deck = max(0, self.deck - 1)
        self.edited()

    # --- cards -------------------------------------------------------------

    def add_card(self):
        deck = self.current()
        if deck is None:
            messagebox.showinfo("Starter decks", "Add a deck first.", parent=self)
            return
        cid = pick_card(self, self.project, "Card to add to the deck")
        if not cid:
            return
        try:
            copies = int(self.copies.get() or "0")
        except ValueError:
            copies = 0
        deck.cards[cid] = copies if 0 < copies <= DECK_SIZE else 1
        self.edited()
        if self.tree.exists(str(cid)):
            self.tree.selection_set(str(cid))
            self.tree.see(str(cid))

    def set_copies(self):
        deck = self.current()
        if deck is None:
            return
        try:
            copies = int(self.copies.get())
        except ValueError:
            messagebox.showerror("Copies", "Copies are a whole number.", parent=self)
            return
        if not 0 <= copies <= DECK_SIZE:
            messagebox.showerror("Copies", f"A card's copies are 0 to {DECK_SIZE}.", parent=self)
            return
        chosen = [i for i in self.tree.selection() if not i.startswith("kept:")]
        for iid in chosen:
            deck.cards[int(iid)] = copies
        self.edited()
        for iid in chosen:
            if self.tree.exists(iid):
                self.tree.selection_add(iid)

    def remove_card(self):
        deck = self.current()
        if deck is None:
            return
        for iid in self.tree.selection():
            if iid.startswith("kept:"):
                deck.kept.pop(iid[5:], None)
            else:
                deck.cards.pop(int(iid), None)
        self.edited()

    def goto(self, target):
        self.deck = target if isinstance(target, int) else 0
        self.fill_list()
        if self.list.exists(str(self.deck)):
            self.list.see(str(self.deck))
        self.fill()


# --- Mod info -------------------------------------------------------------------

class ModInfoTab(Tab):
    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Mod info")
        form = ttk.Frame(self)
        form.pack(fill="x")
        self.vars = {k: tk.StringVar() for k in ("id", "name", "version", "author")}
        labels = {"id": "Id (letters, digits, - and _)", "name": "Name", "version": "Version", "author": "Author"}
        for row, key in enumerate(self.vars):
            ttk.Label(form, text=labels[key]).grid(row=row, column=0, sticky="w", pady=2)
            entry = ttk.Entry(form, textvariable=self.vars[key], width=50)
            entry.grid(row=row, column=1, sticky="w", pady=2)
            entry.bind("<FocusOut>", lambda e: self.commit())
        ttk.Label(form, text="Description").grid(row=4, column=0, sticky="nw", pady=2)
        self.description = tk.Text(form, width=70, height=4, wrap="word")
        self.description.grid(row=4, column=1, sticky="w", pady=2)
        self.folder = ttk.Label(form, style="Hint.TLabel")
        self.folder.grid(row=5, column=1, sticky="w")
        boxes = ttk.Frame(self)
        boxes.pack(fill="both", expand=True, pady=(8, 0))
        left = ttk.LabelFrame(boxes, text="Settings (JSON list; see notes/modding.md)", padding=4)
        left.pack(side="left", fill="both", expand=True)
        self.settings = tk.Text(left, width=50, height=14, wrap="none", font=("Consolas", 10))
        self.settings.pack(fill="both", expand=True)
        right = ttk.LabelFrame(boxes, text="Other mod.json keys, kept as written (data, text, textures, audio, "
                                           "requires...)", padding=4)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))
        self.other = tk.Text(right, width=60, height=14, wrap="none", font=("Consolas", 10))
        self.other.pack(fill="both", expand=True)
        self.status = ttk.Label(self, style="Error.TLabel")
        self.status.pack(anchor="w")
        buttons = ttk.Frame(self)
        buttons.pack(fill="x")
        ttk.Button(buttons, text="Apply", command=self.commit).pack(side="left")
        ttk.Button(buttons, text="Preview mod.json", command=self.preview).pack(side="left", padx=4)

    def refresh(self):
        if self.project is None:
            return
        info = self.project.info
        for key, var in self.vars.items():
            var.set(getattr(info, key))
        for box, value in ((self.description, info.description),
                           (self.settings, json.dumps(info.settings, indent=2, ensure_ascii=False) if info.settings else ""),
                           (self.other, json.dumps(self.shown_other(), indent=2, ensure_ascii=False)
                            if self.shown_other() else "")):
            box.delete("1.0", "end")
            box.insert("1.0", value)
        source = self.project.source_dir
        self.folder.configure(text=f"Folder: {source}" if source else "Not saved yet")
        self.status.configure(text="")

    def commit(self):
        if self.project is None:
            return True
        info = self.project.info
        before = (info.id, info.name, info.version, info.author, info.description, info.settings, self.project.other)
        try:
            settings_text = self.settings.get("1.0", "end").strip()
            settings = json.loads(settings_text) if settings_text else []
            if not isinstance(settings, list):
                raise ValueError("settings is a JSON list")
            other_text = self.other.get("1.0", "end").strip()
            other = json.loads(other_text) if other_text else {}
            if not isinstance(other, dict):
                raise ValueError("the other keys are a JSON object")
            reserved = set(other) & {"id", "name", "version", "author", "description", "settings", "cards",
                                     "fusions", "equips", "rituals", "drops", "decks", "limits", "guardian_stars"}
            if reserved:
                raise ValueError(f"edit {', '.join(sorted(reserved))} in the editor's own tabs")
        except ValueError as problem:
            self.status.configure(text=f"Not applied: {problem}")
            return False
        info.id = self.vars["id"].get().strip()
        info.name = self.vars["name"].get()
        info.version = self.vars["version"].get().strip()
        info.author = self.vars["author"].get()
        info.description = self.description.get("1.0", "end-1c")
        info.settings = settings
        # "limits" is the Limits tab's (limits_tab.py), "guardian_stars" the
        # Guardian Stars tab's (guardian_stars_tab.py), not this box's.
        for key in ("limits", "guardian_stars"):
            if key in self.project.other:
                other[key] = self.project.other[key]
        self.project.other = other
        self.status.configure(text="")
        after = (info.id, info.name, info.version, info.author, info.description, info.settings, self.project.other)
        if after != before:
            self.app.changed()
        return True

    def shown_other(self) -> dict:
        """The other keys this box shows: all but the Limits and Guardian Stars tabs'."""
        return {key: value for key, value in self.project.other.items() if key not in ("limits", "guardian_stars")}

    def preview(self):
        if self.app.commit_all():
            show_text(self, "mod.json preview", manifest.dumps(manifest.build(self.project)))


# --- Conflicts ------------------------------------------------------------------

class ConflictsTab(Tab):
    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Conflicts")
        top = ttk.Frame(self)
        top.pack(fill="x")
        ttk.Button(top, text="Check now", command=self.run).pack(side="left")
        self.summary = ttk.Label(top)
        self.summary.pack(side="left", padx=8)
        ttk.Label(top, text="Double-click a line to go to it.", style="Hint.TLabel").pack(side="right")
        # The other installed mods this one is checked against (validate.cross_mod):
        # the player's mods folder, or one chosen here (kept in the editor's settings).
        other = ttk.Frame(self)
        other.pack(fill="x", pady=(4, 0))
        ttk.Button(other, text="Other mods folder...", command=self.choose_folder).pack(side="left")
        ttk.Button(other, text="Player's folder", command=lambda: self.set_folder(None)).pack(side="left", padx=4)
        self.others = ttk.Label(other, style="Hint.TLabel", wraplength=900, justify="left")
        self.others.pack(side="left", padx=8, fill="x", expand=True)
        from . import settings
        self.other_folder = settings.load().get("other_mods")
        frame, self.tree = scrolled_tree(self, [("level", "Level"), ("area", "Where"), ("what", "What"),
                                                ("message", "Conflict")], [70, 90, 260, 560], 26)
        frame.pack(fill="both", expand=True, pady=4)
        self.tree.bind("<Double-1>", lambda e: self.go())
        self.issues = []

    def choose_folder(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(parent=self, title="A folder of mods to check this one against",
                                         initialdir=self.other_folder or str(self.app.mods_dir()))
        if folder:
            self.set_folder(folder)

    def set_folder(self, folder):
        from . import settings
        self.other_folder = folder
        settings.save("other_mods", folder)
        self.run()

    def refresh(self):
        self.run()

    def run(self):
        if self.project is None:
            return []
        self.app.commit_all()
        self.issues = validate.validate(self.project)
        try:
            from pathlib import Path
            folders = [Path(self.other_folder)] if self.other_folder else None
            others, said = validate.cross_mod(self.project, folders)
        except Exception as problem:   # noqa: BLE001 -- never let another mod stop this one opening
            others, said = [], f"The other mods could not be checked: {type(problem).__name__}: {problem}"
        self.issues += others
        self.others.configure(text=said)
        self.tree.delete(*self.tree.get_children())
        for i, issue in enumerate(self.issues):
            self.tree.insert("", "end", iid=str(i), values=(issue.level, issue.area, issue.where, issue.message),
                             tags=(issue.level,))
        errors = len(validate.errors(self.issues))
        notes = sum(1 for issue in self.issues if issue.level == "note")
        self.summary.configure(text=f"{errors} errors, {len(self.issues) - errors - notes} warnings, {notes} notes",
                               style="Error.TLabel" if errors else "Ok.TLabel")
        return self.issues

    def go(self):
        selection = self.tree.selection()
        if selection:
            self.app.go_to(self.issues[int(selection[0])])
