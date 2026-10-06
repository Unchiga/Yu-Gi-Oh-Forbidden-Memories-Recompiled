"""The Fusions tab's "Bulk..." window: every pair of the cards chosen as A
with the cards chosen as B made into a card, or taken away, with a preview
before anything changes (bulk_fusions.py holds the rules)."""
from __future__ import annotations

import tkinter as tk
from tkinter import messagebox, ttk

from . import bulk_fusions as bulk, screen
from .gamedata import ATTRIBUTE_NAMES, STAR_NAMES, TYPE_NAMES
from .widgets import CardField, grab, px, scrolled_tree, show_text

MONSTER_TYPES = TYPE_NAMES[:20]
DELAY = 300          # ms of quiet before the preview is worked out again


def _number(text: str):
    """A bound: a whole number, or None when blank; ValueError otherwise."""
    text = text.strip()
    if not text:
        return None
    return int(text)


class FilterPanel(ttk.LabelFrame):
    """The filters of one side: all of them hold for a card to be chosen."""

    def __init__(self, master, title, on_change, star_names=None):
        super().__init__(master, text=title, padding=6)
        self.on_change = on_change
        self.kinds = {k: tk.BooleanVar() for k in bulk.KINDS}
        self.attributes = [tk.BooleanVar() for _ in ATTRIBUTE_NAMES]
        self.texts = {k: tk.StringVar() for k in ("atk_min", "atk_max", "def_min", "def_max", "level_min",
                                                  "level_max", "name", "text", "cards")}
        self.results_only = tk.BooleanVar()
        row = 0
        ttk.Label(self, text="Kind").grid(row=row, column=0, sticky="nw")
        line = ttk.Frame(self)
        line.grid(row=row, column=1, columnspan=3, sticky="w")
        for kind, var in self.kinds.items():
            ttk.Checkbutton(line, text=kind.capitalize(), variable=var, command=on_change).pack(side="left")
        row += 1
        ttk.Label(self, text="Monster type").grid(row=row, column=0, sticky="nw", pady=(4, 0))
        ttk.Label(self, text="Guardian star").grid(row=row, column=2, sticky="nw", pady=(4, 0), padx=(8, 0))
        row += 1
        self.types = self._listbox(MONSTER_TYPES, 7)
        self.types.master.grid(row=row, column=0, columnspan=2, sticky="nsew")
        # The mod's own stars too (tabs.star_choices), past the disc's ten.
        self.stars = self._listbox(star_names or STAR_NAMES[1:], 7)
        self.stars.master.grid(row=row, column=2, columnspan=2, sticky="nsew", padx=(8, 0))
        row += 1
        ttk.Label(self, text="Attribute").grid(row=row, column=0, sticky="w", pady=(4, 0))
        line = ttk.Frame(self)
        line.grid(row=row, column=1, columnspan=3, sticky="w", pady=(4, 0))
        for name, var in zip(ATTRIBUTE_NAMES, self.attributes):
            ttk.Checkbutton(line, text=name, variable=var, command=on_change).pack(side="left")
        row += 1
        for label, low, high in (("ATK", "atk_min", "atk_max"), ("DEF", "def_min", "def_max"),
                                 ("Level", "level_min", "level_max")):
            ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", pady=1)
            line = ttk.Frame(self)
            line.grid(row=row, column=1, columnspan=3, sticky="w", pady=1)
            ttk.Label(line, text="from").pack(side="left")
            ttk.Entry(line, textvariable=self.texts[low], width=7).pack(side="left", padx=2)
            ttk.Label(line, text="to").pack(side="left")
            ttk.Entry(line, textvariable=self.texts[high], width=7).pack(side="left", padx=2)
            row += 1
        for label, key, hint in (("Name has", "name", ""), ("Text has", "text", "words of the card's text"),
                                 ("Cards", "cards", "numbers, 10-20, names; comma between")):
            ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", pady=1)
            ttk.Entry(self, textvariable=self.texts[key], width=34).grid(row=row, column=1, columnspan=3,
                                                                         sticky="we", pady=1)
            if hint:
                row += 1
                ttk.Label(self, text=hint, foreground="#777").grid(row=row, column=1, columnspan=3, sticky="w")
            row += 1
        ttk.Checkbutton(self, text="Only cards a fusion makes", variable=self.results_only,
                        command=on_change).grid(row=row, column=0, columnspan=3, sticky="w", pady=(2, 0))
        line = ttk.Frame(self)
        line.grid(row=row + 1, column=0, columnspan=4, sticky="we", pady=(4, 0))
        self.count = ttk.Label(line)
        self.count.pack(side="left")
        ttk.Button(line, text="List...", command=self.list_cards).pack(side="right")
        ttk.Button(line, text="Clear", command=self.clear).pack(side="right", padx=4)
        self.columnconfigure(1, weight=1)
        self.columnconfigure(3, weight=1)
        for var in self.texts.values():
            var.trace_add("write", lambda *_: on_change())
        self.chosen = []
        self.project = None

    def _listbox(self, names, height):
        frame = ttk.Frame(self)
        box = tk.Listbox(frame, selectmode="multiple", height=height, exportselection=False, activestyle="none")
        for name in names:
            box.insert("end", name)
        bar = ttk.Scrollbar(frame, orient="vertical", command=box.yview)
        box.configure(yscrollcommand=bar.set)
        box.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        box.bind("<<ListboxSelect>>", lambda e: self.on_change())
        return box

    def read(self) -> bulk.CardFilter:
        """The filter; ValueError naming a bound that is not a number."""
        numbers = {}
        for key in ("atk_min", "atk_max", "def_min", "def_max", "level_min", "level_max"):
            try:
                numbers[key] = _number(self.texts[key].get())
            except ValueError:
                raise ValueError(f"{self.cget('text')}: {key.replace('_', ' ')} is not a whole number") from None
        return bulk.CardFilter(
            kinds={k for k, var in self.kinds.items() if var.get()},
            types=set(self.types.curselection()),
            attributes={i for i, var in enumerate(self.attributes) if var.get()},
            stars={i + 1 for i in self.stars.curselection()},
            name=self.texts["name"].get(), text=self.texts["text"].get(), cards=self.texts["cards"].get(),
            results_only=self.results_only.get(), **numbers)

    def write(self, other: "FilterPanel"):
        """Take the other side's filters."""
        for key, var in self.kinds.items():
            var.set(other.kinds[key].get())
        for mine, theirs in zip(self.attributes, other.attributes):
            mine.set(theirs.get())
        for key, var in self.texts.items():
            var.set(other.texts[key].get())
        self.results_only.set(other.results_only.get())
        for box, source in ((self.types, other.types), (self.stars, other.stars)):
            box.selection_clear(0, "end")
            for i in source.curselection():
                box.selection_set(i)
        self.on_change()

    def clear(self):
        for var in list(self.kinds.values()) + self.attributes + [self.results_only]:
            var.set(False)
        for var in self.texts.values():
            var.set("")
        for box in (self.types, self.stars):
            box.selection_clear(0, "end")
            box.see(0)
        self.on_change()

    def show_count(self, project, chosen):
        self.project, self.chosen = project, chosen
        self.count.configure(text=f"{len(chosen)} cards")

    def list_cards(self):
        if self.project is None:
            return
        lines = [f"{cid:4}  {self.project.cards[cid].name}  (ATK {self.project.cards[cid].attack}, "
                 f"{TYPE_NAMES[self.project.cards[cid].type] if self.project.cards[cid].type < len(TYPE_NAMES) else '?'})"
                 for cid in self.chosen]
        show_text(self, f"{self.cget('text')}: {len(lines)} cards", "\n".join(lines) or "(none)", width=70, height=30)


class BulkFusionsDialog(tk.Toplevel):
    def __init__(self, tab):
        super().__init__(tab)
        self.tab = tab
        self.title("Bulk fusions")
        self.transient(tab.winfo_toplevel())
        self.pending = None
        self.current = None      # the last plan
        self.mode = tk.StringVar(value="add")
        self.result_mode = tk.StringVar(value="card")
        self.ladder = tk.StringVar()
        self.stronger = tk.BooleanVar(value=True)
        self.allow_self = tk.BooleanVar()
        self.overwrite = tk.StringVar(value="skip")

        top = ttk.Frame(self, padding=(10, 8, 10, 0))
        top.pack(fill="x")
        ttk.Radiobutton(top, text="Add fusions", value="add", variable=self.mode,
                        command=self.mode_changed).pack(side="left")
        ttk.Radiobutton(top, text="Take fusions away", value="remove", variable=self.mode,
                        command=self.mode_changed).pack(side="left", padx=8)
        ttk.Label(top, text="Every card of A with every card of B; empty filters mean any card. A+B and B+A "
                            "are one pair.", foreground="#777").pack(side="left", padx=8)

        sides = ttk.Frame(self, padding=(10, 6))
        sides.pack(fill="x")
        from .tabs import star_choices
        names = star_choices(tab.project)[1:]
        self.a = FilterPanel(sides, "Material A", self.schedule, names)
        self.b = FilterPanel(sides, "Material B", self.schedule, names)
        self.a.grid(row=0, column=0, sticky="nsew")
        middle = ttk.Frame(sides)
        middle.grid(row=0, column=1, padx=4)
        ttk.Button(middle, text="A → B", width=6, command=lambda: self.b.write(self.a)).pack(pady=2)
        ttk.Button(middle, text="A ← B", width=6, command=lambda: self.a.write(self.b)).pack(pady=2)
        self.b.grid(row=0, column=2, sticky="nsew")
        sides.columnconfigure(0, weight=1)
        sides.columnconfigure(2, weight=1)

        self.outcome = ttk.LabelFrame(self, text="Result", padding=6)
        self.outcome.pack(fill="x", padx=10)
        self.card_choice = ttk.Radiobutton(self.outcome, text="This card", value="card", variable=self.result_mode,
                                           command=self.mode_changed)
        self.card_choice.grid(row=0, column=0, sticky="w")
        self.result = CardField(self.outcome, lambda: self.tab.project, width=34)
        self.result.grid(row=0, column=1, sticky="we", padx=4)
        self.result.var.trace_add("write", lambda *_: self.schedule())
        self.ladder_choice = ttk.Radiobutton(self.outcome, text="The weakest of these that beats both materials",
                                             value="ladder", variable=self.result_mode, command=self.mode_changed)
        self.ladder_choice.grid(row=1, column=0, sticky="w")
        self.ladder_entry = ttk.Entry(self.outcome, textvariable=self.ladder, width=40)
        self.ladder_entry.grid(row=1, column=1, sticky="we", padx=4)
        self.ladder.trace_add("write", lambda *_: self.schedule())
        options = ttk.Frame(self.outcome)
        options.grid(row=2, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self.stronger_box = ttk.Checkbutton(options, text="Only when the result's ATK beats both materials'",
                                            variable=self.stronger, command=self.schedule)
        self.stronger_box.pack(side="left")
        ttk.Checkbutton(options, text="A card may fuse with itself", variable=self.allow_self,
                        command=self.schedule).pack(side="left", padx=12)
        self.policy = ttk.Frame(self.outcome)
        self.policy.grid(row=3, column=0, columnspan=2, sticky="w", pady=(2, 0))
        ttk.Label(self.policy, text="A pair that already fuses:").pack(side="left")
        ttk.Radiobutton(self.policy, text="keep its result", value="skip", variable=self.overwrite,
                        command=self.schedule).pack(side="left", padx=4)
        ttk.Radiobutton(self.policy, text="replace it", value="overwrite", variable=self.overwrite,
                        command=self.schedule).pack(side="left")
        self.outcome.columnconfigure(1, weight=1)

        preview = ttk.LabelFrame(self, text="Preview", padding=6)
        preview.pack(fill="both", expand=True, padx=10, pady=6)
        self.summary = ttk.Label(preview, justify="left")
        self.summary.pack(fill="x")
        self.budget = ttk.Label(preview, foreground="#777")
        self.budget.pack(fill="x")
        self.problem = ttk.Label(preview, foreground="#c01c28", justify="left")
        self.problem.pack(fill="x")
        self.warning = ttk.Label(preview, foreground="#9c6500", justify="left")
        self.warning.pack(fill="x")
        frame, self.tree = scrolled_tree(preview, [("a", "Card A"), ("b", "Card B"), ("before", "Now"),
                                                   ("after", "After"), ("what", "")],
                                         [220, 220, 220, 220, 70], 9)
        self.tree.tag_configure("kept", foreground="#777")
        frame.pack(fill="both", expand=True, pady=(4, 0))

        buttons = ttk.Frame(self, padding=(10, 0, 10, 10))
        buttons.pack(fill="x", side="bottom", before=preview)  # shrink the preview, not the buttons, on a short screen
        ttk.Button(buttons, text="Close", command=self.destroy).pack(side="right")
        self.apply_button = ttk.Button(buttons, text="Apply...", command=self.apply)
        self.apply_button.pack(side="right", padx=4)
        self.undo_button = ttk.Button(buttons, text="Undo last batch", command=self.undo)
        self.undo_button.pack(side="left")
        self.undo_note = ttk.Label(buttons, foreground="#777")
        self.undo_note.pack(side="left", padx=6)
        self.bind("<Escape>", lambda e: self.destroy())
        self.minsize(px(self, 900), px(self, 640))
        screen.fit(self)
        self.mode_changed()
        self.show_undo()
        grab(self)

    def destroy(self):
        if self.pending:
            self.after_cancel(self.pending)
            self.pending = None
        super().destroy()

    # --- reading the form -----------------------------------------------------

    def spec(self) -> bulk.BulkSpec:
        ladder = self.mode.get() == "add" and self.result_mode.get() == "ladder"
        return bulk.BulkSpec(a=self.a.read(), b=self.b.read(), mode=self.mode.get(),
                             result=0 if ladder else self.result.get(),
                             ladder=self.ladder.get() if ladder else "",
                             stronger=self.stronger.get(), allow_self=self.allow_self.get(),
                             overwrite=self.overwrite.get() == "overwrite")

    def mode_changed(self):
        adding = self.mode.get() == "add"
        self.outcome.configure(text="Result" if adding else "Only fusions that make (empty: any)")
        for widget in (self.ladder_choice, self.ladder_entry):
            widget.configure(state="normal" if adding else "disabled")
        # the list's choice beats both materials already
        self.stronger_box.configure(state="normal" if adding and self.result_mode.get() == "card" else "disabled")
        self.card_choice.configure(state="normal" if adding else "disabled")
        if adding:
            self.policy.grid()
        else:
            self.policy.grid_remove()
        self.apply_button.configure(text="Apply..." if adding else "Take away...")
        self.schedule()

    # --- the preview ------------------------------------------------------------

    def schedule(self, *_):
        if self.pending:
            self.after_cancel(self.pending)
        self.pending = self.after(DELAY, self.refresh)

    def refresh(self):
        if self.pending:            # called before the timer: it has nothing left to do
            self.after_cancel(self.pending)
        self.pending = None
        project = self.tab.project
        if project is None:
            return None
        try:
            spec = self.spec()
        except ValueError as problem:
            self.current = None
            self.show(None, str(problem))
            return None
        results = bulk.fusion_results(project) if (spec.a.results_only or spec.b.results_only) else None
        self.a.show_count(project, spec.a.select(project, results)[0])
        self.b.show_count(project, spec.b.select(project, results)[0])
        self.current = bulk.plan(project, spec)
        self.show(self.current)
        return self.current

    def show(self, the_plan, problem=""):
        self.tree.delete(*self.tree.get_children())
        if the_plan is None:
            self.summary.configure(text="")
            self.budget.configure(text="")
            self.problem.configure(text=problem)
            self.warning.configure(text="")
            self.apply_button.state(["disabled"])
            return
        p = self.tab.project
        self.summary.configure(text=the_plan.summary())
        self.budget.configure(text=the_plan.budget_line())
        self.problem.configure(text="\n".join(the_plan.errors))
        self.warning.configure(text="\n".join(the_plan.warnings))
        label = lambda cid: p.card_label(cid) if cid else ("(forbidden)" if cid == 0 else "(none)")
        tags = {"add": "added", "replace": "changed", "remove": "removed", "kept": "kept"}
        for i, (pair, before, after, what) in enumerate(the_plan.samples):
            self.tree.insert("", "end", iid=str(i), tags=(tags[what],),
                             values=(p.card_label(pair[0]), p.card_label(pair[1]), label(before),
                                     label(after) if what != "remove" else "(no fusion)", what))
        shown = len(the_plan.samples)
        if shown >= bulk.SAMPLE:
            self.tree.insert("", "end", iid="more", values=("", "", "", f"(the first {shown} shown)", ""))
        self.apply_button.state(["!disabled"] if the_plan.ok() else ["disabled"])

    # --- carrying it out ----------------------------------------------------------

    def apply(self):
        the_plan = self.refresh()
        if the_plan is None or not the_plan.ok():
            return
        if the_plan.mode == "add":
            what = f"add {the_plan.added} fusions and replace {the_plan.replaced}"
        else:
            what = f"take {the_plan.removed} fusions away"
        if not messagebox.askokcancel("Bulk fusions", f"{the_plan.summary()}\n\n{the_plan.budget_line()}\n\n"
                                      f"This will {what}. \"Undo last batch\" puts them back.", parent=self):
            return
        batch = bulk.apply(self.tab.project, the_plan, description=what)
        self.tab.bulk_batch = batch
        self.tab.app.changed()
        self.tab.fill()
        self.show_undo()
        self.refresh()

    def show_undo(self):
        batch = getattr(self.tab, "bulk_batch", None)
        usable = batch is not None and batch.project is self.tab.project
        self.undo_button.state(["!disabled"] if usable else ["disabled"])
        self.undo_note.configure(text=f"last: {batch.description}" if usable else "")

    def undo(self):
        batch = getattr(self.tab, "bulk_batch", None)
        if batch is None or batch.project is not self.tab.project:
            return
        restored, skipped = bulk.undo(self.tab.project, batch)
        self.tab.bulk_batch = None
        self.tab.app.changed()
        self.tab.fill()
        self.show_undo()
        note = f"{restored} pairs put back."
        if skipped:
            note += f" {skipped} changed since are left as they are."
        messagebox.showinfo("Bulk fusions", note, parent=self)
        self.refresh()


def open_bulk(tab):
    if tab.project is None:
        return None
    return BulkFusionsDialog(tab)
