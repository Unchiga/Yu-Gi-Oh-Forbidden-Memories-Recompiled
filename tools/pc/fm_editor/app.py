"""The FM Editor window."""
from __future__ import annotations

import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

from . import disc, file_dialogs, gamedata, manifest, settings, theme, validate
from .model import KEY_RE, Project
from .editing import RECOVERY_CHOICES, Editing, recovery_label
from . import build_info, card_links, history, recovery, screen, zoom
from .art_tab import ArtTab
from .map_tab import MapTab
from .values_tab import ValuesTab
from .guardian_stars_tab import GuardianStarsTab
from .packs_tab import PacksTab
from .tower_tab import TowerTab
from .duelists_tab import DuelistsTab
from .rituals_tab import RitualsTab
from .ui_tab import UiTab
from .tabs import CardsTab, EquipsTab, FusionsTab, ModInfoTab, ConflictsTab, StarterTab
from .widgets import Pages, px, ui_font

APP_TITLE = "FM Editor"
HISTORY_PAUSE = 350      # ms of no change before an undo step is taken
PROJECT_URL = "https://github.com/Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled"


def initial_geometry(window, area=None) -> str:
    """1600x960 at 96 dpi, no more than 90% of the monitor's work area,
    in its middle."""
    x, y, width, height = area or screen.work_area(window)
    w = min(px(window, 1600), width * 9 // 10)
    h = min(px(window, 960), height * 9 // 10)
    return f"{w}x{h}+{x + (width - w) // 2}+{y + (height - h) // 2}"


class App(Editing, tk.Tk):
    def __init__(self, game=None, mod=None, ask=True, autostart=True):
        theme.dpi_awareness()      # Windows: before the first window, or it is drawn stretched
        super().__init__()
        self.title(APP_TITLE)
        self.theme = theme.Theme(self)
        # Sizes for 96 dpi, grown with the desktop's font (widgets.ui_scale)
        # but kept on the monitor it opens on, centred there; a row as tall
        # as a line of text.
        area = screen.work_area(self)
        self.geometry(initial_geometry(self, area))
        width, height = area[2:]
        self.minsize(min(px(self, 1000), width * 9 // 10), min(px(self, 640), height * 9 // 10))
        # View > Interface size: a fixed one from the start, built at it;
        # Fit to window once the window shows.
        self.zoom = zoom.Zoom(self)
        self.zoom.update()
        self.retail = None
        self.files = None
        self.project = None
        self.dirty = False
        self.hooks = []            # extra menu entries (importers) add themselves here
        self.dark = tk.BooleanVar(self, value=settings.load().get("dark") is True)
        self.text_preview = None   # Tools > Card text preview, while open
        self.current_card = None   # the card Cards and Art show, and Fusions and Equips follow (card_links)
        self.init_editing()
        build_info.prefetch()       # Help > About's git commands, off the Tk thread
        self.build_menu()
        self.notebook = Pages(self)
        self.notebook.pack(fill="both", expand=True)
        self.notebook.enable_traversal()        # Ctrl+Tab and Ctrl+Shift+Tab go from tab to tab
        # Over the tabs until there is a game to read: what the editor needs.
        self.welcome = ttk.Frame(self, padding=24, relief="ridge", borderwidth=2)
        ttk.Label(self.welcome, text="The FM Editor makes mods for the PC port of Yu-Gi-Oh! Forbidden Memories.",
                  font=ui_font(12)).pack(anchor="w")
        ttk.Label(self.welcome, justify="left", wraplength=px(self, 620), text=(
            "It reads the cards, fusions, equips, rituals, decks and drops from your own copy of the game, and "
            "saves a mod folder whose mod.json holds only what you change. It never writes the game itself.\n\n"
            "Choose the game's disc image (.bin), or a folder with SLUS_014.11 and DATA/WA_MRG.MRG (the "
            "game/ folder the port plays from).")).pack(anchor="w", pady=(10, 0))
        ttk.Button(self.welcome, text="Choose the game files...", command=self.choose_game).pack(anchor="w",
                                                                                                pady=(14, 0))
        ttk.Button(self.welcome, text="The FM Editor guide",
                   command=lambda: self.open_guide("tools/pc/fm_editor/README.md")).pack(anchor="w", pady=(6, 0))
        self.cards = CardsTab(self.notebook, self)
        self.art = ArtTab(self.notebook, self)
        self.fusions = FusionsTab(self.notebook, self)
        self.equips = EquipsTab(self.notebook, self)
        self.rituals = RitualsTab(self.notebook, self)
        self.duelists = DuelistsTab(self.notebook, self)
        self.ui = UiTab(self.notebook, self)
        self.starter = StarterTab(self.notebook, self)
        self.map = MapTab(self.notebook, self)
        self.values = ValuesTab(self.notebook, self)
        self.stars = GuardianStarsTab(self.notebook, self)
        self.packs = PacksTab(self.notebook, self)
        self.tower = TowerTab(self.notebook, self)
        self.info = ModInfoTab(self.notebook, self)
        self.conflicts = ConflictsTab(self.notebook, self)
        self.tabs = [self.cards, self.art, self.fusions, self.equips, self.rituals, self.duelists, self.ui,
                     self.starter, self.map, self.values, self.stars, self.packs, self.tower, self.info,
                     self.conflicts]
        self.status = ttk.Label(self, relief="sunken", anchor="w", padding=(6, 2))
        self.status.pack(fill="x", side="bottom", before=self.notebook)
        self.install_editing()
        card_links.install_all(self)
        if self.dark.get():
            self.theme.use(True)
        self.notebook.bind("<<NotebookTabChanged>>", lambda e: self.tab_changed())
        self.protocol("WM_DELETE_WINDOW", self.quit_app)
        # On the window, not bind_all: a dialog's keys stay its own. Text's
        # own Ctrl+O inserts a line, so the text boxes get the shortcut too.
        self.bind("<Control-s>", lambda e: self.shortcut(self.save))
        self.bind("<Control-o>", lambda e: self.shortcut(self.open_mod))
        self.bind("<Control-f>", lambda e: self.shortcut(self.find))
        self.bind_class("Text", "<Control-o>", lambda e: self.shortcut(self.open_mod))
        if autostart:
            self.after(50, lambda: self.start(game, mod, ask))

    def find(self):
        """Ctrl+F: the tab's search box (Cards, Art, Fusions), its text picked."""
        entry = getattr(self.notebook.current(), "search_entry", None)
        if entry is not None:
            entry.focus_set()
            entry.selection_range(0, "end")
            entry.icursor("end")

    def shortcut(self, action):
        if self.grab_current() is None:     # not while a dialog is up
            action()
        return "break"

    # --- menus -------------------------------------------------------------

    def build_menu(self):
        bar = tk.Menu(self)
        self.file_menu = tk.Menu(bar, tearoff=False)
        self.file_menu.add_command(label="New mod", command=self.new_mod)
        self.file_menu.add_command(label="Open mod folder...", accelerator="Ctrl+O", command=self.open_mod)
        self.file_menu.add_command(label="Save", accelerator="Ctrl+S", command=self.save)
        self.file_menu.add_command(label="Save as...", command=lambda: self.save(ask=True))
        self.file_menu.add_command(label="Export mod...", command=lambda: self.save(ask=True, export=True))
        self.file_menu.add_command(label="Recover work...", command=self.recover_work)
        recovery_menu = tk.Menu(self.file_menu, tearoff=False)
        for minutes in RECOVERY_CHOICES:
            recovery_menu.add_radiobutton(label=recovery_label(minutes), variable=self.recovery_choice,
                                          value=minutes, command=self.choose_recovery)
        self.file_menu.add_cascade(label="Recovery copy", menu=recovery_menu)
        self.file_menu.add_separator()
        self.import_index = self.file_menu.index("end")
        self.file_menu.add_command(label="Game files...", command=self.choose_game)
        self.file_menu.add_separator()
        self.file_menu.add_command(label="Exit", command=self.quit_app)
        bar.add_cascade(label="File", menu=self.file_menu)
        self.edit_menu = tk.Menu(bar, tearoff=False)
        self.edit_menu.add_command(label="Undo", accelerator="Ctrl+Z", command=self.undo)
        self.edit_menu.add_command(label="Redo", accelerator="Ctrl+Y", command=self.redo)
        self.edit_menu.add_separator()
        self.edit_menu.add_command(label="Apply edits", command=self.apply_edits)
        self.edit_menu.add_command(label="Discard form edits", command=self.discard_forms)
        bar.add_cascade(label="Edit", menu=self.edit_menu)
        tools = tk.Menu(bar, tearoff=False)
        tools.add_command(label="Check the mod", command=self.show_conflicts)
        tools.add_command(label="Preview mod.json", command=lambda: self.info.preview())
        tools.add_command(label="Card text preview", command=self.show_text_preview)
        bar.add_cascade(label="Tools", menu=tools)
        view = tk.Menu(bar, tearoff=False)
        view.add_checkbutton(label="Dark mode", variable=self.dark, command=self.toggle_dark)
        view.add_separator()
        self.zoom.build_menu(view)
        bar.add_cascade(label="View", menu=view)
        helps = tk.Menu(bar, tearoff=False)
        # The guides, as GitHub shows them (the notes are Markdown).
        for label, path in (("FM Editor guide", "tools/pc/fm_editor/README.md"),
                            ("Fusions, equips, rituals, drops and values", "notes/gameplay-tables.md"),
                            ("New cards and changed ones", "notes/more-cards.md"),
                            ("Starter decks and pools", "notes/starter-deck.md"),
                            ("Card packs", "notes/card-packs.md"),
                            ("Added duelists and portraits", "notes/more-duelists.md"),
                            ("Tower mode", "notes/tower-mode.md"),
                            ("Making mods", "notes/modding.md")):
            helps.add_command(label=label, command=lambda path=path: self.open_guide(path))
        helps.add_separator()
        helps.add_command(label="About", command=self.about)
        bar.add_cascade(label="Help", menu=helps)
        self.config(menu=bar)

    def toggle_dark(self):
        """View > Dark mode: the whole window and its dialogs at once, and
        remembered for the next start (settings.py)."""
        self.theme.use(self.dark.get())
        problem = settings.save("dark", self.dark.get())
        if problem:
            self.say(f"Could not remember the dark mode: {problem}")

    def show_text_preview(self):
        """The selected card's text as the card view draws it (preview.py)."""
        if self.text_preview is None:
            from .preview import CardTextPreview
            self.text_preview = CardTextPreview(self)
        else:
            self.text_preview.refresh()
            self.text_preview.lift()

    def add_import(self, label, command):
        """An importer's menu entry, above "Game files..."."""
        self.file_menu.insert_command(self.import_index, label=label, command=command)
        self.import_index += 1

    # --- starting ----------------------------------------------------------------

    def start(self, game, mod, ask):
        files = None
        try:
            files = disc.load(game) if game else disc.find_game()
        except (disc.GameFilesError, OSError) as problem:
            messagebox.showerror(APP_TITLE, str(problem), parent=self)
        while files is None or not self.use_game(files):
            files = None
            if not ask:
                self.say("No game files: File > Game files... to choose them.")
                self.show_welcome()
                return
            answer = messagebox.askokcancel(
                APP_TITLE, "The editor reads the retail tables from your own copy of Yu-Gi-Oh! Forbidden "
                "Memories (USA, SLUS-01411).\n\nChoose the .bin disc image the port runs from (or, next, "
                "a folder holding SLUS_014.11 and DATA/WA_MRG.MRG).", parent=self)
            if not answer:
                self.say("No game files: File > Game files... to choose them.")
                return
            files = self.ask_game_files()
        if mod:
            self.load_mod(mod)
        if ask and any(not row[2].get("backup") for row in recovery.records(self.recovery.folder)):
            if messagebox.askyesno(APP_TITLE, "Unsaved recovery copies are available. Review them?", parent=self):
                self.recover_work()

    def ask_game_files(self):
        path = file_dialogs.askopenfilename(parent=self, title="The game's disc image",
                                            filetypes=[("Disc image", "*.bin *.iso *.img"), ("SLUS_014.11", "SLUS_014.11"),
                                                       ("All files", "*.*")])
        if not path:
            path = file_dialogs.askdirectory(parent=self, title="Or a folder with SLUS_014.11 and DATA/WA_MRG.MRG")
        if not path:
            return None
        try:
            return disc.load(path)
        except (disc.GameFilesError, OSError) as problem:
            messagebox.showerror(APP_TITLE, str(problem), parent=self)
            return None

    def show_welcome(self):
        if self.project is None:
            self.welcome.place(relx=0.5, rely=0.4, anchor="center")
            self.welcome.lift()
        else:
            self.welcome.place_forget()

    def use_game(self, files):
        try:
            self.retail = gamedata.load_game(files)
        except Exception as problem:     # a file that is not the game
            messagebox.showerror(APP_TITLE, f"Could not read the game's tables: {problem}", parent=self)
            return False
        self.files = files
        self.set_project(Project(self.retail))
        notes = "; ".join(self.retail.notes)
        self.say(f"Game files: {files.source}" + (f" ({notes})" if notes else ""))
        return True

    def choose_game(self):
        if not self.confirm_discard():
            return
        files = self.ask_game_files()
        if files:
            self.use_game(files)

    # --- the project ---------------------------------------------------------------

    def set_project(self, project):
        self.cancel_edit_jobs()
        self.clear_recovery()
        self.recovery = recovery.Recovery()
        self._pending.clear()
        self._recovered = False
        self._recovered_from = None
        self.project = project
        self.welcome.place_forget()
        self.current_card = None
        self.dirty = False
        self._refreshing = True
        try:
            for tab in self.tabs:
                tab.refresh()
        finally:
            self._refreshing = False
        self.history = history.History(project)
        self.update_title()
        self.update_edit_state()

    def changed(self):
        if self._refreshing:
            return
        self.dirty = True
        self.update_title()
        # The undo step once the changes pause: a snapshot of the whole mod
        # takes a quarter second with the disc's tables, and a field that
        # applies as it is typed (the Map's camera) made one at every key.
        # Undo and the like take a waiting one first (flush_history).
        if self._history_job is not None:
            self.after_cancel(self._history_job)
        self._history_job = self.after(HISTORY_PAUSE, self.record_edit)
        self.schedule_recovery()
        self.update_edit_state()

    def update_title(self):
        if self.project is None:
            self.title(APP_TITLE)
            return
        where = f" - {self.project.source_dir}" if self.project.source_dir else ""
        self.title(f"{'*' if self.dirty or self._pending else ''}{self.project.info.name} ({self.project.info.id}){where} - {APP_TITLE}")

    def say(self, text):
        self.status.configure(text=text)

    def commit_all(self, show=False):
        """Store every tab's form. With show, a form that cannot be stored
        is brought up with the reason."""
        if self._refreshing:
            return True
        for tab in self.tabs:
            if not tab.commit():
                self._pending.add(tab)
                self.update_title()
                self.update_edit_state()
                if self.notebook.current() is not tab:
                    self._redirecting_tab = True
                    self.notebook.select(tab)
                if show:
                    self.notebook.select(tab)
                    messagebox.showerror(APP_TITLE, f"The {self.notebook.tab(tab, 'text')} tab holds something "
                                         "that cannot be stored (the tab says what). Correct it or revert it first.",
                                         parent=self)
                return False
            self._pending.discard(tab)
        self.update_title()
        self.update_edit_state()
        return True

    def tab_changed(self):
        card_links.close_menu()
        if self._refreshing:
            return
        if getattr(self, "_redirecting_tab", False):
            self._redirecting_tab = False
            return
        if not self.commit_all():
            return
        current = self.notebook.current()
        # The card to follow, before the tab shows its own again (Art's
        # show() makes its card the window's).
        card = self.current_card
        # Other tabs may have changed what this one shows (a card's name or
        # type): fill it again, keeping its selection.
        if current is self.conflicts:
            current.run()
        elif current is self.equips:
            current.fill_equips()
            current.fill()
        elif current is self.duelists:
            current.fill_list()
            current.fill()
        elif current is self.packs:
            current.fill_list()
            current.fill()
        elif current in (self.fusions, self.rituals, self.cards):
            current.fill()
            if current is self.cards:
                current.show_picture(current.current)   # the Art tab may have changed it
        elif current is self.art:
            current.fill()
            current.show(current.current)
        elif current is self.stars:
            current.fill()          # the cards' stars may have changed
        elif current is self.info and current not in self._pending:
            current.refresh()       # other tabs keep keys beside its box's (starter_pools...)
        self.current_card = card
        follow = getattr(current, "follow", None)
        if follow is not None and card is not None:
            follow(card)

    # --- a card in another tab (card_links) -------------------------------------

    def open_tab(self, tab) -> bool:
        """Bring a tab up; False when a form elsewhere cannot be stored."""
        self.notebook.select(tab)
        return self.notebook.current() is tab

    def open_card(self, tab, cid):
        if self.project is None or cid not in self.project.cards:
            return
        # The tab-changed event comes after select() and makes the tab follow
        # the window's card: make that this card first.
        self.current_card = cid
        if self.open_tab(tab):
            tab.show_card(cid)

    def open_starter_pool(self, index, cid=None):
        """The Starter decks tab's Weighted pools page, on pool `index` (and
        the card's row)."""
        if not self.open_tab(self.starter):
            return
        page = self.starter.pools
        self.starter.pages.select(page)
        page.index = index
        page.fill()
        if cid is not None and page.tree.exists(str(cid)):
            page.tree.selection_set(str(cid))
            page.tree.see(str(cid))

    def open_pool(self, d, pool, cid=None):
        if self.open_tab(self.duelists):
            self.duelists.goto((d, pool))
            # After the opponent list's selection event, which fills the tab again.
            self.after_idle(self.select_row, (self.duelists.tree, self.duelists.fixed.tree), cid)

    def open_starter(self, index, cid=None):
        if self.open_tab(self.starter):
            self.starter.goto(index)
            self.after_idle(self.select_row, (self.starter.tree,), cid)

    def open_pack(self, index):
        if self.open_tab(self.packs):
            self.packs.goto(index)

    @staticmethod
    def select_row(trees, cid):
        for tree in trees:
            if cid is not None and tree.exists(str(cid)):
                tree.selection_set(str(cid))
                tree.see(str(cid))

    def need_game(self):
        if self.retail is None:
            messagebox.showinfo(APP_TITLE, "Choose the game files first (File > Game files...).", parent=self)
            return False
        return True

    def confirm_discard(self):
        if not self.commit_all(show=True):
            return False
        if not self.dirty:
            return True
        answer = messagebox.askyesnocancel(APP_TITLE, "Save the changes to this mod first?", parent=self)
        if answer is None:
            return False
        return self.save() if answer else True

    def new_mod(self):
        if self.need_game() and self.confirm_discard():
            self.set_project(Project(self.retail))
            self.say("New mod: everything as retail.")

    def open_mod(self):
        if not self.need_game() or not self.confirm_discard():
            return
        folder = file_dialogs.askdirectory(parent=self, title="A mod folder (the one holding mod.json)",
                                           initialdir=str(self.open_dir()))
        if folder:
            self.load_mod(folder)

    def load_mod(self, folder):
        folder = Path(folder)
        if not (folder / "mod.json").is_file():
            messagebox.showerror(APP_TITLE, f"{folder} has no mod.json.", parent=self)
            return
        try:
            project, messages = manifest.open_mod(self.retail, folder)
        except (ValueError, OSError) as problem:
            messagebox.showerror(APP_TITLE, f"Could not read {folder / 'mod.json'}: {problem}", parent=self)
            return
        self.set_project(project)
        self.say(f"Opened {folder}")
        if messages:
            self.report("Opened with notes", "What the editor noticed while reading mod.json:\n\n" +
                        "\n".join("- " + m for m in messages))

    def report(self, title, text):
        from .widgets import show_text
        show_text(self, title, text, width=110, height=24)

    def mods_dir(self) -> Path:
        """The user directory's mods folder: where Export puts a mod."""
        folder = disc.user_dir() / "mods"
        return folder if folder.is_dir() else Path.cwd()

    def chosen_dir(self):
        """The mods folder the player chose for the game (validate.chosen_in_use),
        when it is there; the game reads it, and Export never writes to it."""
        folder = validate.chosen_in_use()
        try:
            return folder if folder is not None and folder.is_dir() else None
        except OSError:
            return None

    def open_dir(self) -> Path:
        """Where Open starts: the player's chosen mods folder, where their own
        mods are, else the user directory's."""
        return self.chosen_dir() or self.mods_dir()

    def save(self, ask=False, export=False):
        """Save, Save as (ask) or Export mod (export: a folder named after
        the mod's id is always made in the chosen one, as the game's mods
        folder wants one folder a mod)."""
        if self.project is None or not self.commit_all(show=True):
            return False
        issues = validate.validate(self.project)
        errors = validate.errors(issues)
        if errors:
            self.conflicts.run()
            self.notebook.select(self.conflicts)
            if not messagebox.askyesno(APP_TITLE, f"The loader would refuse {len(errors)} thing(s) in this mod "
                                       f"(see Conflicts), for example:\n\n{errors[0]}\n\nSave anyway?",
                                       icon="warning", default="no", parent=self):
                return False
        folder = self.project.source_dir
        if ask or self._recovered or folder is None:
            if export and not KEY_RE.match(self.project.info.id or ""):
                self.notebook.select(self.info)
                messagebox.showerror(APP_TITLE, "The exported folder is named after the mod's id: give it one of "
                                     "letters, digits, hyphens and underscores (Mod info).", parent=self)
                return False
            title = (f"Export to: a folder \"{self.project.info.id}\" is made in the one you choose (the game's "
                     "mods folder, say)" if export else f"Where to save: an empty folder, or its parent (a folder "
                     f"\"{self.project.info.id}\" is made)")
            chosen = file_dialogs.askdirectory(parent=self, initialdir=str(self.mods_dir()), title=title)
            if not chosen:
                return False
            chosen = Path(chosen)
            if export:
                chosen = chosen / self.project.info.id
            elif chosen.is_dir() and any(chosen.iterdir()) and not (chosen / "mod.json").exists():
                if not KEY_RE.match(self.project.info.id or ""):
                    self.notebook.select(self.info)
                    messagebox.showerror(APP_TITLE, "The mod's folder is named after its id: give it one of "
                                         "letters, digits, hyphens and underscores (Mod info), or choose an empty "
                                         "folder.", parent=self)
                    return False
                chosen = chosen / self.project.info.id
            folder = chosen
            if (folder / "mod.json").exists() and folder != self.project.source_dir:
                if not messagebox.askyesno(APP_TITLE, f"{folder} already holds a mod. Replace its mod.json "
                                           "and matching asset files?",
                                           parent=self):
                    return False
        self.flush_history()
        try:
            recovery.backup(folder)
            path = manifest.save_mod(self.project, folder)
        except (ValueError, OSError) as problem:
            messagebox.showerror(APP_TITLE, str(problem), parent=self)
            if self.recovery_choice.get():
                self.autosave(wait=False)       # the work is not on disk: a copy of it now
            return False
        self.dirty = False
        self._recovered = False
        self.forget_recovered_copy()
        self.history.mark_saved(self.project)
        self.cancel_edit_jobs()
        self.clear_recovery()
        self.update_title()
        self.update_edit_state()
        self.info.refresh()
        self.say(f"{'Exported' if export else 'Saved'} {path}. Enable it in the game under Game > Mods and "
                 "restart the game.")
        return True

    def show_conflicts(self):
        if self.notebook.current() is self.conflicts:
            self.conflicts.run()
        else:
            self.notebook.select(self.conflicts)    # the tab switch checks

    def go_to(self, issue):
        target = issue.target
        if issue.area == "Cards" and target:
            self.notebook.select(self.cards)
            self.cards.goto(target)
        elif issue.area == "Art":
            self.notebook.select(self.art)
            if target:
                self.art.goto(target)
        elif issue.area == "Fusions" and target:
            self.current_card = target[0]       # what the tab follows once it is up
            self.notebook.select(self.fusions)
            self.fusions.show_card(target[0])
        elif issue.area == "Equips" and target:
            self.notebook.select(self.equips)
            if self.equips.equips.exists(str(target)):
                self.equips.equips.selection_set(str(target))
        elif issue.area == "Rituals" and target:
            self.notebook.select(self.rituals)
            if self.rituals.tree.exists(str(target)):
                self.rituals.tree.selection_set(str(target))
        elif issue.area == "Duelists" and target:
            self.notebook.select(self.duelists)
            self.duelists.goto(target)
        elif issue.area == "Starter decks":
            self.notebook.select(self.starter)
            self.starter.goto(target)
        elif issue.area == "Map":
            self.notebook.select(self.map)
            self.map.goto(target)
        elif issue.area == "Packs":
            self.notebook.select(self.packs)
            self.packs.goto(target)
        elif issue.area == "Mod info":
            self.notebook.select(self.info)
        elif issue.area == "Values":
            self.notebook.select(self.values)
        elif issue.area == "Guardian Stars":
            self.notebook.select(self.stars)
        elif issue.area == "Tower":
            self.notebook.select(self.tower)
        elif issue.area == "Starter pools":
            self.open_starter_pool(target if isinstance(target, int) else 0)
        elif issue.area == "Other mods" and isinstance(target, tuple):
            self.go_to_overlap(*target)

    def go_to_overlap(self, kind, key):
        """The tab that edits what another mod changes too (overlaps.KINDS),
        on the card when the overlap is about one."""
        from . import overlaps as ov
        card = key if isinstance(key, int) and not isinstance(key, bool) else None
        if kind == ov.EQUIPS and isinstance(key, tuple) and key and key[0] == "equip":
            card = key[-1] if isinstance(key[-1], int) else None     # ("equip", base, card): overlaps.py
        if kind == ov.CARDS:
            self.notebook.select(self.cards)
            if card in self.project.cards:
                self.cards.goto(card)
        elif kind == ov.FUSIONS:
            pair = [k for k in key[1:] if isinstance(k, int)] if isinstance(key, tuple) else []
            if pair and pair[0] in self.project.cards:
                self.current_card = pair[0]
            self.notebook.select(self.fusions)
            pair = [k for k in key[1:] if isinstance(k, int)] if isinstance(key, tuple) else []
            if pair and pair[0] in self.project.cards:
                self.current_card = pair[0]     # what the tab follows once it is up
                self.fusions.show_card(pair[0])
        elif kind == ov.EQUIPS:
            self.notebook.select(self.equips)
            equip = card
            if equip in self.project.cards:
                self.current_card = equip
            if equip and self.equips.equips.exists(str(equip)):
                self.equips.show_card(equip)
        elif kind == ov.RITUALS:
            self.notebook.select(self.rituals)
            if card is not None and self.rituals.tree.exists(str(card)):
                self.rituals.tree.selection_set(str(card))
                self.rituals.tree.see(str(card))
        else:
            tab = {ov.POOLS: self.duelists, ov.DUELISTS: self.duelists, ov.STARTER: self.starter,
                   ov.PACKS: self.packs, ov.PASSWORDS: self.packs, ov.STARS: self.stars, ov.LIMITS: self.values,
                   ov.TEXTURES: self.art}.get(kind, self.info)
            self.notebook.select(tab)

    def open_guide(self, path):
        import webbrowser
        webbrowser.open(f"{PROJECT_URL}/blob/master/{path}")

    def about(self):
        messagebox.showinfo(APP_TITLE, build_info.description() + "\n\nMakes mods for the PC port of Yu-Gi-Oh! Forbidden Memories. "
                            "It reads the retail tables from your own game files and saves a mod folder whose "
                            "mod.json holds only what you changed. It never writes the disc or game/.\n\n"
                            "tools/pc/fm_editor/README.md", parent=self)

    def quit_app(self):
        if self.confirm_discard():
            self.clear_recovery()
            self.destroy()

    def destroy(self):
        self.cancel_edit_jobs()
        if self._recovery_writer is not None:      # a copy being written: not cut off half-way
            self._recovery_writer.wait()
        # Also stop widget/dialog idle callbacks before their Tcl commands
        # disappear. Cancelled only: after_cancel() also deletes a job's
        # command, which a widget registered and deletes itself on destroy
        # ("can't delete Tcl command" then).
        for job in self.tk.call("after", "info"):
            self.tk.call("after", "cancel", job)
        super().destroy()


def main(game=None, mod=None):
    from . import importers
    app = App(game, mod)
    importers.install(app)
    app.mainloop()
    return 0
