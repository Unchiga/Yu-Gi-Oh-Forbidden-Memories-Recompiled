"""Small Tkinter pieces the editor's tabs share: a card field with a picker,
a scrolled tree, and dialogs."""
from __future__ import annotations

import sys
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

from . import theme
from .gamedata import TYPE_NAMES
from .model import card_matches  # noqa: F401 (model.py's; tabs.py and art_tab.py import it from here)


def ui_scale(widget) -> float:
    """How much bigger than at 96 dpi the default font is drawn: a desktop
    set to 144 dpi (Xft.dpi) enlarges the text but not Tk's pixel sizes, so
    widths, wrap lengths and row heights given in pixels grow by this.
    On Windows the program is dpi aware (theme.dpi_awareness): Tk's scaling
    (pixels a point) is the dpi's, 4/3 at 100%, and the fonts in points
    follow it, so the pixel sizes do too."""
    if sys.platform == "win32":
        return max(1.0, float(widget.tk.call("tk", "scaling")) * 72 / 96)
    return max(1.0, tkfont.nametofont("TkDefaultFont", root=widget).metrics("linespace") / 19)


def px(widget, pixels: int) -> int:
    """`pixels` at 96 dpi, at the desktop's size (ui_scale)."""
    return round(pixels * ui_scale(widget))


def ui_font(size: int, weight: str = "bold"):
    """The default font's face at another size (a heading): ("TkDefaultFont",
    size, weight) names no face, which X11 matches to its default sans but
    Windows to Arial, so there the face is TkDefaultFont's own."""
    if sys.platform == "win32":
        return (tkfont.nametofont("TkDefaultFont").actual("family"), size, weight)
    return ("TkDefaultFont", size, weight)


class ScrolledForm(ttk.Frame):
    """A form with a vertical scrollbar, wheel support and focus visibility.

    Put controls in body. A private binding tag handles the wheel before
    Spinbox/Combobox bindings can change values. Text boxes and lists keep
    their own scrolling while they have more content in that direction.
    """

    def __init__(self, parent, **kwargs):
        super().__init__(parent, **kwargs)
        self.canvas = tk.Canvas(self, width=1, height=1, highlightthickness=0,
                                yscrollincrement=1)
        self.bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.bar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.body = ttk.Frame(self.canvas)
        self.window = self.canvas.create_window(0, 0, window=self.body, anchor="nw")
        self.body.bind("<Configure>", self._layout)
        self.canvas.bind("<Configure>", self._layout)
        self.bind("<<ThemeChanged>>", self._theme)
        self._theme()
        self._tag = f"ScrolledForm:{self}"
        self._bindings = [(sequence, self.bind_class(self._tag, sequence, self._wheel))
                          for sequence in ("<MouseWheel>", "<Button-4>", "<Button-5>")]
        self._top = self.winfo_toplevel()
        self._map_binding = self._top.bind("<Map>", self._mapped, add=True)
        self._focus_binding = self._top.bind("<FocusIn>", self._focus, add=True)
        self.bind("<Destroy>", self._destroyed, add=True)

    def _contains(self, widget):
        while widget is not None:
            if widget is self.body or widget is self.canvas:
                return True
            widget = getattr(widget, "master", None)
        return False

    def _mapped(self, event):
        if self._contains(event.widget) and self._tag not in event.widget.bindtags():
            event.widget.bindtags((self._tag,) + event.widget.bindtags())

    def _theme(self, event=None):
        self.canvas.configure(background=ttk.Style(self).lookup("TFrame", "background"))

    def _layout(self, event=None):
        width = self.body.winfo_reqwidth()
        if int(self.canvas.cget("width")) != width:
            self.canvas.configure(width=width)
        self.canvas.itemconfigure(self.window, width=max(width, self.canvas.winfo_width()))
        self.canvas.configure(scrollregion=(0, 0, width, self.body.winfo_reqheight()))

    def _wheel(self, event):
        if getattr(event, "num", None) in (4, 5):
            units = -3 if event.num == 4 else 3
        else:
            delta = event.delta
            if not delta:
                return
            units = -int(delta) if sys.platform == "darwin" else -int(delta / 120)
            if not units:
                units = -1 if delta > 0 else 1
        widget = event.widget
        if isinstance(widget, (tk.Text, tk.Listbox, ttk.Treeview)):
            first, last = widget.yview()
            if (units < 0 and first > 0) or (units > 0 and last < 1):
                return  # its class binding scrolls its contents
        if self.body.winfo_reqheight() > self.canvas.winfo_height():
            self.canvas.yview_scroll(units * px(self, 20), "units")
            return "break"

    def _focus(self, event):
        widget = event.widget
        if not self._contains(widget) or not widget.winfo_ismapped():
            return
        top = widget.winfo_rooty() - self.body.winfo_rooty()
        bottom = top + widget.winfo_height()
        first = self.canvas.canvasy(0)
        height = self.canvas.winfo_height()
        target = top if top < first else bottom - height if bottom > first + height else first
        if target != first:
            self.canvas.yview_moveto(max(0, target) / max(1, self.body.winfo_reqheight()))

    def _destroyed(self, event):
        if event.widget is self:
            self._top.unbind("<Map>", self._map_binding)
            self._top.unbind("<FocusIn>", self._focus_binding)
            for sequence, command in self._bindings:
                self.unbind_class(self._tag, sequence)
                self._root().deletecommand(command)


class ScrolledPage(ttk.Frame):
    """A notebook page that stretches its tab to fill the window and, when
    the window is smaller than the tab needs, scrolls it instead: the
    scrollbars show only then. Build the tab in inner (Tab does).

    The inner frame's grid has the canvas's size as its least, so it is as
    big as the canvas or as the tab, whichever is bigger; it takes that size
    itself (the canvas item has none of its own), so a tab that grows after
    the window is shown makes it lay out again.
    """

    def __init__(self, parent, **kwargs):
        super().__init__(parent, **kwargs)
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.canvas = tk.Canvas(self, width=1, height=1, highlightthickness=0, xscrollincrement=1,
                                yscrollincrement=1)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.ybar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.xbar = ttk.Scrollbar(self, orient="horizontal", command=self.canvas.xview)
        self.canvas.configure(xscrollcommand=self.xbar.set, yscrollcommand=self.ybar.set)
        self.ybar.grid(row=0, column=1, sticky="ns")
        self.xbar.grid(row=1, column=0, sticky="we")
        self.ybar.grid_remove()
        self.xbar.grid_remove()
        self.inner = ttk.Frame(self.canvas)
        self.inner.rowconfigure(0, weight=1)
        self.inner.columnconfigure(0, weight=1)
        self.canvas.create_window(0, 0, window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", self._layout)
        self.canvas.bind("<Configure>", self._layout)
        self.bind("<<ThemeChanged>>", self._theme)
        self._theme()
        self._tag = f"ScrolledPage:{self}"
        self._bindings = [(sequence, self.bind_class(self._tag, sequence, self._wheel))
                          for sequence in ("<MouseWheel>", "<Button-4>", "<Button-5>")]
        self._top = self.winfo_toplevel()
        self._map_binding = self._top.bind("<Map>", self._mapped, add=True)
        self.bind("<Destroy>", self._destroyed, add=True)

    def _content(self):
        slaves = self.inner.grid_slaves()
        return slaves[0] if slaves else self.inner

    def _theme(self, event=None):
        self.canvas.configure(background=ttk.Style(self).lookup("TFrame", "background"))

    def _layout(self, event=None):
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        self.inner.columnconfigure(0, minsize=width)
        self.inner.rowconfigure(0, minsize=height)
        content = self._content()
        wide, tall = content.winfo_reqwidth() > width, content.winfo_reqheight() > height
        for bar, shown in ((self.xbar, wide), (self.ybar, tall)):
            if shown != bool(bar.winfo_manager()):
                bar.grid() if shown else bar.grid_remove()
        self.canvas.configure(scrollregion=(0, 0, max(width, self.inner.winfo_reqwidth()),
                                            max(height, self.inner.winfo_reqheight())))

    def _mapped(self, event):
        widget = event.widget
        if self._tag in widget.bindtags():
            return
        while widget is not None and widget is not self.inner:
            widget = getattr(widget, "master", None)
        if widget is self.inner:
            event.widget.bindtags((self._tag,) + event.widget.bindtags())

    def _wheel(self, event):
        if not self.ybar.winfo_manager():
            return
        # What scrolls by itself keeps the wheel: lists, text, pictures, and
        # the scrolled forms some tabs hold.
        widget = event.widget
        while widget is not None and widget is not self.inner:
            if isinstance(widget, (tk.Text, tk.Listbox, tk.Canvas, ttk.Treeview, ScrolledForm)):
                return
            widget = getattr(widget, "master", None)
        if getattr(event, "num", None) in (4, 5):
            units = -3 if event.num == 4 else 3
        else:
            delta = event.delta
            if not delta:
                return
            units = -int(delta) if sys.platform == "darwin" else -int(delta / 120)
            if not units:
                units = -1 if delta > 0 else 1
        self.canvas.yview_scroll(units * px(self, 20), "units")
        return "break"

    def _destroyed(self, event):
        if event.widget is self:
            self._top.unbind("<Map>", self._map_binding)
            for sequence, command in self._bindings:
                self.unbind_class(self._tag, sequence)
                self._root().deletecommand(command)


class Pages(ttk.Notebook):
    """A notebook of ScrolledPages that takes a tab where ttk wants its page:
    select(tab), tab(tab, ...), index(tab). current() is the shown tab."""

    @staticmethod
    def _page(tab_id):
        return getattr(tab_id, "page", tab_id)

    def select(self, tab_id=None):
        return super().select(None if tab_id is None else self._page(tab_id))

    def tab(self, tab_id, option=None, **kw):
        return super().tab(self._page(tab_id), option, **kw)

    def index(self, tab_id):
        return super().index(self._page(tab_id))

    def current(self):
        page = self.nametowidget(super().select())
        return getattr(page, "tab", page)


class TreeSort:
    """Sort displayed rows without changing their identity, selection or data."""

    def __init__(self, tree, columns, numeric):
        self.tree = tree
        self.labels = dict(columns)
        self.numeric = set(numeric)
        self.column = None
        self.reverse = False
        for column, label in columns:
            if label:
                tree.heading(column, command=lambda c=column: self.choose(c))

    def choose(self, column):
        self.reverse = not self.reverse if self.column == column else False
        self.column = column
        self.apply()

    def apply(self):
        if self.column is None:
            return
        tree, column = self.tree, self.column
        rows, missing = [], []
        for iid in tree.get_children():
            value = tree.set(iid, column)
            if value == "":
                missing.append(iid)
            else:
                key = float(value.rstrip("%")) if column in self.numeric else value.casefold()
                rows.append((key, iid))
        # Equal values keep a deterministic card/opponent-number order.
        rows.sort(key=lambda row: (0, int(row[1])) if row[1].isdigit() else (1, row[1]))
        rows.sort(key=lambda row: row[0], reverse=self.reverse)
        for index, iid in enumerate([iid for _, iid in rows] + missing):
            tree.move(iid, "", index)
        for key, label in self.labels.items():
            arrow = (" ▼" if self.reverse else " ▲") if key == column else ""
            tree.heading(key, text=label + arrow)


def scrolled_tree(parent, columns, widths, height=20, selectmode="browse", *, sort_numeric=None):
    """A Treeview with a vertical scrollbar, in a frame of its own."""
    frame = ttk.Frame(parent)
    tree = ttk.Treeview(frame, columns=[c for c, _ in columns], show="headings", height=height, selectmode=selectmode)
    # The widths as made: a theme change (theme.py) asks the tree's size
    # again from its columns, which stretching has widened by then.
    tree.widths = {key: px(frame, width) for (key, _), width in zip(columns, widths)}
    for (key, label), width in zip(columns, widths):
        tree.heading(key, text=label)
        tree.column(key, width=tree.widths[key], stretch=width > 120, anchor="w")
    bar = ttk.Scrollbar(frame, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=bar.set)
    tree.grid(row=0, column=0, sticky="nsew")
    bar.grid(row=0, column=1, sticky="ns")
    frame.rowconfigure(0, weight=1)
    frame.columnconfigure(0, weight=1)
    if sort_numeric is not None:
        tree.sorting = TreeSort(tree, columns, sort_numeric)
        horizontal = ttk.Scrollbar(frame, orient="horizontal", command=tree.xview)
        tree.configure(xscrollcommand=horizontal.set)
        horizontal.grid(row=1, column=0, sticky="ew")
    for tag in theme.TAGS:
        tree.tag_configure(tag, foreground=theme.tag_color(tree, tag))
    return frame, tree


class CardPicker(tk.Toplevel):
    """A dialog to choose a card by searching its name or number."""

    def __init__(self, master, project, title="Choose a card", only=None, initial=""):
        super().__init__(master)
        self.title(title)
        self.transient(master)
        self.resizable(True, True)
        self.project = project
        self.only = only
        self.result = None
        self.search = tk.StringVar(value=initial)
        entry = ttk.Entry(self, textvariable=self.search, width=40)
        entry.pack(fill="x", padx=8, pady=(8, 4))
        frame, self.tree = scrolled_tree(self, [("id", "#"), ("name", "Name"), ("type", "Type")], [50, 240, 110], 16)
        frame.pack(fill="both", expand=True, padx=8)
        buttons = ttk.Frame(self)
        buttons.pack(fill="x", padx=8, pady=8)
        ttk.Button(buttons, text="OK", command=self.ok).pack(side="right")
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="right", padx=4)
        self.search.trace_add("write", lambda *_: self.fill())
        self.tree.bind("<Double-1>", lambda e: self.ok())
        entry.bind("<Return>", lambda e: self.ok())
        entry.bind("<Down>", lambda e: (self.tree.focus_set(), self._select_first()))
        self.bind("<Escape>", lambda e: self.destroy())
        self.fill()
        entry.focus_set()
        grab(self)

    def _select_first(self):
        children = self.tree.get_children()
        if children:
            self.tree.selection_set(children[0])
            self.tree.focus(children[0])

    def fill(self):
        self.tree.delete(*self.tree.get_children())
        text = self.search.get()
        shown = 0
        for cid in sorted(self.project.cards):
            if self.only and not self.only(cid):
                continue
            if not card_matches(self.project, cid, text):
                continue
            card = self.project.cards[cid]
            kind = TYPE_NAMES[card.type] if 0 <= card.type < len(TYPE_NAMES) else str(card.type)
            self.tree.insert("", "end", iid=str(cid), values=(cid, card.name, kind))
            shown += 1
            if shown >= 800:
                break
        self._select_first()

    def ok(self):
        selection = self.tree.selection()
        if selection:
            self.result = int(selection[0])
            self.destroy()


def pick_card(master, project, title="Choose a card", only=None, initial=""):
    dialog = CardPicker(master, project, title, only, initial)
    master.wait_window(dialog)
    # The picker took the grab: a dialog it was opened from gets it back.
    top = master.winfo_toplevel()
    if isinstance(top, tk.Toplevel) and top.winfo_exists():
        grab(top)
    return dialog.result


def grab(window):
    """window.grab_set(), once the window is on screen: a dialog opened by a
    double-click is often not mapped yet, and Tk refuses the grab ("window
    not viewable")."""
    try:
        window.grab_set()
    except tk.TclError:
        window.after(20, lambda: window.winfo_exists() and grab(window))


def card_named(project, text: str) -> int:
    """The card a CardField's text names, or 0."""
    text = text.strip()
    if not text:
        return 0
    # A number, or a card as CardField.set() shows it ("7 Name"); otherwise a
    # name, which may start with a digit ("7 Colored Fish").
    if text.isdigit() and int(text) in project.cards:
        return int(text)
    head = text.split(" ", 1)[0]
    if head.isdigit() and int(head) in project.cards and project.card_label(int(head)) == text:
        return int(head)
    cid = project.resolve(text)
    if cid:
        return cid
    for other, card in project.cards.items():
        if card.name.lower() == text.lower():
            return other
    return 0


class CardField(ttk.Frame):
    """An entry naming a card (number or name) with a "..." picker."""

    def __init__(self, master, project_getter, width=28, only=None):
        super().__init__(master)
        self.project_getter = project_getter
        self.only = only
        self.var = tk.StringVar()
        self.entry = ttk.Entry(self, textvariable=self.var, width=width)
        self.entry.pack(side="left", fill="x", expand=True)
        ttk.Button(self, text="...", width=3, command=self.pick).pack(side="left", padx=(2, 0))

    def pick(self):
        project = self.project_getter()
        cid = pick_card(self.winfo_toplevel(), project, only=self.only, initial="")
        if cid:
            self.set(cid)

    def set(self, cid):
        project = self.project_getter()
        self.var.set(project.card_label(cid) if cid else "")

    def get(self) -> int:
        """The card named, or 0."""
        return card_named(self.project_getter(), self.var.get())


class FormDialog(tk.Toplevel):
    """A small modal form: rows of (label, widget factory); on OK the
    callback gets the dialog and returns an error text or None."""

    def __init__(self, master, title, build, on_ok):
        super().__init__(master)
        self.title(title)
        self.transient(master)
        self.resizable(False, False)
        self.on_ok = on_ok
        self.ok_pressed = False
        body = ttk.Frame(self, padding=10)
        body.pack(fill="both", expand=True)
        build(self, body)
        self.error = ttk.Label(self, style="Error.TLabel")
        self.error.pack(fill="x", padx=10)
        buttons = ttk.Frame(self, padding=(10, 0, 10, 10))
        buttons.pack(fill="x")
        ttk.Button(buttons, text="OK", command=self.ok).pack(side="right")
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="right", padx=4)
        self.bind("<Escape>", lambda e: self.destroy())
        grab(self)

    def ok(self):
        problem = self.on_ok(self)
        if problem:
            self.error.configure(text=problem)
            return
        self.ok_pressed = True
        self.destroy()


def show_text(master, title, text, width=100, height=36):
    window = tk.Toplevel(master)
    window.title(title)
    box = tk.Text(window, width=width, height=height, wrap="none", font=("Consolas", 10))
    bar = ttk.Scrollbar(window, orient="vertical", command=box.yview)
    box.configure(yscrollcommand=bar.set)
    box.insert("1.0", text)
    box.configure(state="disabled")
    box.pack(side="left", fill="both", expand=True)
    bar.pack(side="right", fill="y")
    return window
