"""The Guardian Stars tab's "Set stars by rule..." window: the cards a filter
chooses get a star from their attribute or monster type through a table
the modder fills in (a Fire monster's first star is Fire), or one star for
all of them, with a preview before anything changes and an undo after
(star_rules.py holds the rules; the filters are the Bulk fusions window's)."""
from __future__ import annotations

import tkinter as tk
from tkinter import messagebox, ttk

from . import screen, star_rules
from .bulk_dialog import FilterPanel
from .gamedata import ATTRIBUTE_NAMES, TYPE_NAMES
from .widgets import px, scrolled_tree

DELAY = 300
KEEP = "(leave)"


class StarRulesDialog(tk.Toplevel):
    def __init__(self, tab):
        super().__init__(tab)
        from .tabs import star_choices
        self.tab = tab
        self.title("Set stars by rule")
        self.transient(tab.winfo_toplevel())
        self.pending = None
        self.current = None
        self.choices = [KEEP] + star_choices(tab.project)     # (leave), (none), the stars
        self.which = tk.StringVar(value="first")
        self.source = tk.StringVar(value="attribute")

        top = ttk.Frame(self, padding=(10, 8, 10, 0))
        top.pack(fill="x")
        ttk.Label(top, text="Set the cards'").pack(side="left")
        for value in star_rules.WHICH:
            ttk.Radiobutton(top, text=value, value=value, variable=self.which,
                            command=self.schedule).pack(side="left", padx=2)
        ttk.Label(top, text="star from their").pack(side="left", padx=(6, 2))
        for value, label in (("attribute", "attribute"), ("type", "monster type"), ("star", "nothing: one star")):
            ttk.Radiobutton(top, text=label, value=value, variable=self.source,
                            command=self.source_changed).pack(side="left", padx=2)

        middle = ttk.Frame(self, padding=(10, 6))
        middle.pack(fill="both", expand=True)
        self.filter = FilterPanel(middle, "Cards", self.schedule, star_choices(tab.project)[1:])
        self.filter.grid(row=0, column=0, sticky="nsew")
        self.table = ttk.LabelFrame(middle, text="Which star", padding=6)
        self.table.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        middle.columnconfigure(0, weight=1)
        self.rows = {}
        self.source_changed()

        preview = ttk.LabelFrame(self, text="Preview", padding=6)
        preview.pack(fill="both", expand=True, padx=10, pady=6)
        self.summary = ttk.Label(preview, justify="left")
        self.summary.pack(fill="x")
        self.problem = ttk.Label(preview, style="Error.TLabel", justify="left")
        self.problem.pack(fill="x")
        frame, self.tree = scrolled_tree(preview, [("card", "Card"), ("before", "Now"), ("after", "After")],
                                         [260, 200, 200], 9)
        frame.pack(fill="both", expand=True, pady=(4, 0))
        buttons = ttk.Frame(self, padding=(10, 0, 10, 10))
        buttons.pack(fill="x", side="bottom", before=preview)  # shrink the preview, not the buttons, on a short screen
        ttk.Button(buttons, text="Close", command=self.destroy).pack(side="right")
        self.apply_button = ttk.Button(buttons, text="Apply...", command=self.apply)
        self.apply_button.pack(side="right", padx=4)
        self.undo_button = ttk.Button(buttons, text="Undo last batch", command=self.undo)
        self.undo_button.pack(side="left")
        self.bind("<Escape>", lambda e: self.destroy())
        self.minsize(px(self, 900), px(self, 620))
        screen.fit(self)
        self.show_undo()
        self.schedule()

    def destroy(self):
        if self.pending:
            self.after_cancel(self.pending)
            self.pending = None
        super().destroy()

    def source_changed(self):
        for child in self.table.winfo_children():
            child.destroy()
        self.rows = {}
        source = self.source.get()
        keys = [(i, name) for i, name in enumerate(ATTRIBUTE_NAMES)] if source == "attribute" else \
            [(i, name) for i, name in enumerate(TYPE_NAMES[:20])] if source == "type" else [(None, "Every card")]
        for row, (key, name) in enumerate(keys):
            ttk.Label(self.table, text=name).grid(row=row % 10, column=(row // 10) * 2, sticky="w", pady=1)
            var = tk.StringVar(value=KEEP)
            box = ttk.Combobox(self.table, textvariable=var, values=self.choices, state="readonly", width=16)
            box.grid(row=row % 10, column=(row // 10) * 2 + 1, sticky="w", padx=(4, 10), pady=1)
            box.bind("<<ComboboxSelected>>", lambda e: self.schedule())
            self.rows[key] = var
        self.schedule()

    def spec(self) -> star_rules.RuleSpec:
        mapping = {}
        for key, var in self.rows.items():
            text = var.get()
            if text != KEEP:
                mapping[key] = self.choices.index(text) - 1      # (none) is 0
        return star_rules.RuleSpec(filter=self.filter.read(), which=self.which.get(), source=self.source.get(),
                                   mapping=mapping)

    def schedule(self, *_):
        if self.pending:
            self.after_cancel(self.pending)
        self.pending = self.after(DELAY, self.refresh)

    def refresh(self):
        if self.pending:
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
        self.filter.show_count(project, spec.filter.select(project)[0])
        self.current = star_rules.plan(project, spec, len(self.choices) - 2)
        self.show(self.current)
        return self.current

    def show(self, the_plan, problem=""):
        from .tabs import star_label
        self.tree.delete(*self.tree.get_children())
        if the_plan is None:
            self.summary.configure(text="")
            self.problem.configure(text=problem)
            self.apply_button.state(["disabled"])
            return
        project = self.tab.project
        self.summary.configure(text=the_plan.summary())
        self.problem.configure(text="\n".join(the_plan.errors))
        pair = lambda stars: " / ".join(star_label(s, project) for s in stars)
        for cid, before, after in the_plan.changes[:star_rules.SAMPLE]:
            self.tree.insert("", "end", iid=str(cid), values=(project.card_label(cid), pair(before), pair(after)))
        self.apply_button.state(["!disabled"] if the_plan.ok() else ["disabled"])

    def apply(self):
        the_plan = self.refresh()
        if the_plan is None or not the_plan.ok():
            return
        if not messagebox.askokcancel("Set stars by rule", f"{the_plan.summary()}\n\nChange {len(the_plan.changes)} "
                                      "cards' stars? \"Undo last batch\" puts them back.", parent=self):
            return
        self.tab.star_batch = star_rules.apply(self.tab.project, the_plan,
                                               description=f"{len(the_plan.changes)} cards' stars")
        self.tab.app.changed()
        self.tab.fill()
        self.show_undo()
        self.refresh()

    def show_undo(self):
        batch = getattr(self.tab, "star_batch", None)
        usable = batch is not None and batch.project is self.tab.project
        self.undo_button.state(["!disabled"] if usable else ["disabled"])

    def undo(self):
        batch = getattr(self.tab, "star_batch", None)
        if batch is None or batch.project is not self.tab.project:
            return
        restored, skipped = star_rules.undo(self.tab.project, batch)
        self.tab.star_batch = None
        self.tab.app.changed()
        self.tab.fill()
        self.show_undo()
        note = f"{restored} cards put back."
        if skipped:
            note += f" {skipped} changed since are left as they are."
        messagebox.showinfo("Set stars by rule", note, parent=self)
        self.refresh()
