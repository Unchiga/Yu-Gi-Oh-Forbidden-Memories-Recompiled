"""The Art tab: a card's picture, thumbnail and name plate as the disc has
them, beside the mod's own and how the game draws them (art.py)."""
from __future__ import annotations

import re
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from . import art, pngio
from .tabs import Tab, type_label
from .widgets import card_matches, px, scrolled_tree, ui_font

# Each part shown at a zoom, and the internal resolution its third view draws.
ZOOM = {"art": 2, "thumbnail": 4, "title": 2}
INTERNAL = {"art": 2, "thumbnail": 4}


def photo(master, image: pngio.Image, zoom: int = 1):
    """A Tk image of `image`, `zoom` times its size (nearest)."""
    return tk.PhotoImage(master=master, data=pngio.ppm(pngio.scale_nearest(image, zoom)), format="PPM")


class ArtTab(Tab):
    FILTERS = ["All cards", "Art of the mod", "Added by the mod"]

    def __init__(self, notebook, app):
        super().__init__(notebook, app, "Art")
        self.current = None
        self.photos = {}
        left = ttk.Frame(self)
        left.pack(side="left", fill="y")
        top = ttk.Frame(left)
        top.pack(fill="x")
        ttk.Label(top, text="Search").pack(side="left")
        self.search = tk.StringVar()
        ttk.Entry(top, textvariable=self.search, width=20).pack(side="left", padx=4)
        self.filter = tk.StringVar(value=self.FILTERS[0])
        ttk.Combobox(top, textvariable=self.filter, values=self.FILTERS, state="readonly", width=16).pack(side="left")
        self.search.trace_add("write", lambda *_: self.fill())
        self.filter.trace_add("write", lambda *_: self.fill())
        frame, self.tree = scrolled_tree(left, [("id", "#"), ("name", "Name"), ("type", "Type"), ("state", "Art")],
                                         [50, 200, 100, 110], 28)
        frame.pack(fill="both", expand=True, pady=4)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.select())
        self.count = ttk.Label(left, style="Hint.TLabel")
        self.count.pack(anchor="w")

        right = ttk.Frame(self, padding=(10, 0, 0, 0))
        right.pack(side="left", fill="both", expand=True)
        self.heading = ttk.Label(right, font=ui_font(11))
        self.heading.pack(anchor="w")
        self.how = ttk.Label(right, style="Note.TLabel", wraplength=px(right, 720), justify="left")
        self.how.pack(anchor="w", pady=(0, 4))
        self.rows = {}
        for part in art.PARTS:
            box = ttk.LabelFrame(right, text=art.LABELS[part], padding=6)
            box.pack(fill="x", pady=3)
            views = ttk.Frame(box)
            views.pack(side="left")
            titles = ["Disc", "In game (1x)"] + ([f"Internal {INTERNAL[part]}x"] if part in INTERNAL else [])
            labels = []
            for column, title in enumerate(titles):
                ttk.Label(views, text=title, style="Note.TLabel").grid(row=0, column=column, padx=4)
                label = ttk.Label(views, relief="sunken")
                label.grid(row=1, column=column, padx=4)
                labels.append(label)
            side = ttk.Frame(box, padding=(10, 0, 0, 0))
            side.pack(side="left", fill="both", expand=True)
            info = ttk.Label(side, wraplength=px(side, 330), justify="left")
            info.pack(anchor="w")
            where = ttk.Label(side, style="Hint.TLabel", wraplength=px(side, 330), justify="left")
            where.pack(anchor="w", pady=(2, 4))
            buttons = ttk.Frame(side)
            buttons.pack(anchor="w")
            ttk.Button(buttons, text="Import PNG...", command=lambda p=part: self.import_png(p)).pack(side="left")
            ttk.Button(buttons, text="Export disc's...", command=lambda p=part: self.export_png(p, False)).pack(
                side="left", padx=4)
            export_mod = ttk.Button(buttons, text="Export mod's...", command=lambda p=part: self.export_png(p, True))
            export_mod.pack(side="left")
            revert = ttk.Button(buttons, text="Revert", command=lambda p=part: self.revert(p))
            revert.pack(side="left", padx=4)
            self.rows[part] = {"labels": labels, "info": info, "where": where, "export": export_mod, "revert": revert}
        self.status = ttk.Label(right, style="Warning.TLabel", wraplength=px(right, 720), justify="left")
        self.status.pack(anchor="w", pady=4)
        self.fill()

    @property
    def wa(self):
        files = self.app.files
        return files.wa if files is not None else None

    # the list
    def wanted(self, cid) -> bool:
        f = self.filter.get()
        if f == "Art of the mod" and cid not in art.changed_cards(self.project):
            return False
        if f == "Added by the mod" and cid not in self.project.added:
            return False
        return card_matches(self.project, cid, self.search.get())

    def row(self, cid):
        card = self.project.cards[cid]
        st = art.state(self.project)
        parts = [art.LABELS[p].split()[-1].lower() for p in art.PARTS if (cid, p) in st.images]
        state = ", ".join(parts)
        return (cid, card.name, type_label(card.type), state), ("changed",) if state else ()

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
        self.count.configure(text=f"{shown} cards")
        if self.current in self.project.cards and self.tree.exists(str(self.current)):
            self.tree.selection_set(str(self.current))
            self.tree.see(str(self.current))

    def refresh(self):
        self.current = None
        self.fill()
        self.show(None)

    def select(self):
        selection = self.tree.selection()
        cid = int(selection[0]) if selection else None
        if cid != self.current:
            self.show(cid)

    def goto(self, cid):
        self.search.set("")
        self.filter.set(self.FILTERS[0])
        self.fill()
        if self.tree.exists(str(cid)):
            self.tree.selection_set(str(cid))
            self.tree.see(str(cid))
            self.show(cid)

    # the card
    def show(self, cid):
        self.current = cid
        self.status.configure(text="")
        self.photos = {}
        if cid is None or self.project is None or cid not in self.project.cards or self.wa is None:
            self.heading.configure(text="Choose a card" if self.wa is not None or self.project is None
                                   else "The Art tab needs the game files (File > Game files...)")
            self.how.configure(text="")
            for row in self.rows.values():
                for label in row["labels"]:
                    label.configure(image="")
                row["info"].configure(text="")
                row["where"].configure(text="")
            return
        project = self.project
        base = project.base_of(cid)
        self.heading.configure(text=project.card_label(cid) + (f"  (a copy of {project.card_label(base)})"
                                                              if base != cid else ""))
        if cid in project.added:
            how = ("A card the mod adds keeps its picture and thumbnail in its mod.json entry (\"art\", "
                   "\"thumbnail\"): any size up to 4x; Internal 2x and 4x draw its own detail. Without a "
                   "thumbnail, the game cuts one from the picture; without either, it shows its base's. "
                   "Restart the game after a change.")
        else:
            how = ("A retail card's picture and thumbnail go in the mod's texture pack (\"textures\"): any size "
                   "up to 4x; the console's resolution averages it down, Internal 2x and 4x draw its own "
                   "detail. A copy of this card without art of its own shows it too.")
        self.how.configure(text=how + " The name plate is the entry's \"title\" PNG (dark ink on white).")
        for part in art.PARTS:
            self.show_part(cid, part)

    def show_part(self, cid, part):
        project, wa, row = self.project, self.wa, self.rows[part]
        zoom = ZOOM[part]
        base = project.base_of(cid)
        views = []
        try:
            disc = art.disc_image(wa, base, part)
            if part == "title":
                disc = art.plate_image(art.disc_plate_inks(wa, base), background=art.GOLD)
            views.append(disc)
            views.append(art.in_game(project, wa, cid, part, 1))
            if part in INTERNAL:
                views.append(art.in_game(project, wa, cid, part, INTERNAL[part]))
            _, where = art.shown_image(project, wa, cid, part)
        except (OSError, pngio.PngError, ValueError) as problem:
            views, where = [], str(problem)
        for i, label in enumerate(row["labels"]):
            image = views[i] if i < len(views) else None
            if image is None:
                label.configure(image="")
                continue
            factor = zoom if i < 2 else max(1, zoom // INTERNAL.get(part, zoom))
            self.photos[(part, i)] = photo(self, image, factor)
            label.configure(image=self.photos[(part, i)])
        row["info"].configure(text=art.describe(project, cid, part))
        row["where"].configure(text=f"The game shows {where}.")
        owned = (cid, part) in art.state(project).images
        row["export"].state(["!disabled"] if owned else ["disabled"])
        row["revert"].state(["!disabled"] if owned else ["disabled"])

    def update_row(self, cid):
        if self.tree.exists(str(cid)):
            values, tags = self.row(cid)
            self.tree.item(str(cid), values=values, tags=tags)

    # actions
    def import_png(self, part):
        cid = self.current
        if cid is None:
            return
        path = filedialog.askopenfilename(parent=self, title=f"{art.LABELS[part]} for {self.project.card_label(cid)}",
                                          filetypes=[("PNG images", "*.png"), ("All files", "*.*")])
        if not path:
            return
        self.use_file(part, path)

    def use_file(self, part, path):
        """Import `path` as the current card's part (the Import button, and
        tests)."""
        cid = self.current
        try:
            image = pngio.read(path)
            before = image.size
            notes = art.set_image(self.project, cid, part, image)
        except (OSError, pngio.PngError, ValueError) as problem:
            messagebox.showerror("FM Editor", f"Could not use {path}: {problem}", parent=self)
            return False
        self.app.changed()
        self.update_row(cid)
        self.show(cid)
        w, h = art.SIZES[part]
        text = f"{Path(path).name} ({before[0]}x{before[1]}) is the {art.LABELS[part].lower()}"
        if notes:
            text += ": " + "; ".join(notes)
        self.status.configure(text=text + ".")
        self.app.say(f"Card {cid}: {art.LABELS[part].lower()} from {Path(path).name}. Save to write it.")
        if any("shape" in n for n in notes):
            messagebox.showwarning("FM Editor", f"The picture is not the {art.LABELS[part].lower()}'s shape "
                                   f"({w}:{h}), so only its middle is kept. Crop it yourself to choose "
                                   "the part.", parent=self)
        return True

    def export_png(self, part, own):
        cid = self.current
        if cid is None:
            return
        project = self.project
        base = project.base_of(cid)
        try:
            if own:
                image = art.replacement_image(project, cid, part)
            elif part == "title":
                image = art.plate_image(art.disc_plate_inks(self.wa, base))
            else:
                image = art.disc_image(self.wa, base, part)
        except (OSError, pngio.PngError, ValueError) as problem:
            messagebox.showerror("FM Editor", str(problem), parent=self)
            return
        if image is None:
            return
        name = re.sub(r"[^a-z0-9]+", "-", project.cards[cid].name.lower()).strip("-")
        suffix = {"art": "", "thumbnail": ".small", "title": ".title"}[part]
        path = filedialog.asksaveasfilename(parent=self, defaultextension=".png",
                                            initialfile=f"{cid:04d}-{name}{suffix}.png",
                                            filetypes=[("PNG images", "*.png")])
        if path:
            pngio.write(path, image)
            self.app.say(f"Wrote {path}")

    def revert(self, part):
        cid = self.current
        if cid is None:
            return
        art.revert(self.project, cid, part)
        self.app.changed()
        self.update_row(cid)
        self.show(cid)
