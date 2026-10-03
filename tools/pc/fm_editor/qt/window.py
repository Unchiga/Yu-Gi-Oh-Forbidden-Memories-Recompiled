"""The editor window: the frame, the file menu, and every page mixed in."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401
from .art import ArtMixin
from .cards import CardsMixin
from .duelists import DuelistsMixin
from .equips import EquipsMixin
from .fusions import FusionsMixin
from .limits import LimitsMixin
from .map import MapMixin
from .modinfo import ModinfoMixin
from .packs import PacksMixin
from .problems import ProblemsMixin
from .rituals import RitualsMixin
from .stars import StarsMixin
from .starter import StarterMixin


class ModernEditor(ArtMixin, CardsMixin, DuelistsMixin, EquipsMixin, FusionsMixin, LimitsMixin, MapMixin, ModinfoMixin, PacksMixin, ProblemsMixin, RitualsMixin, StarsMixin, StarterMixin, QMainWindow):
    FILTERS = ["All cards", "Changed", "Added by the mod", "With notes", "Monsters", "Non-monsters"] + TYPE_NAMES
    NAV = ["Cards", "Art", "Campaign", "Fusions", "Equips", "Rituals", "Duelists", "Starter decks",
           "Limits", "Guardian Stars", "Packs", "Mod info", "Problems"]
    def __init__(self, game=None, mod=None):
        super().__init__()
        self.setWindowTitle("FM Editor — Forbidden Memories Mod Studio")
        self.resize(1580, 980)
        self.setMinimumSize(1280, 760)
        self.files = self._load_game(game)
        self.retail = gamedata.load_game(self.files)
        self.project = Project(self.retail)
        self.frame_cache = {}
        self.fusion_art_cache = {}
        self.preview_scale = 1
        self.current = None
        self.current_art_card = None
        self.current_workspace = "Cards"
        self.workspace_forms = {}
        self.workspace_controls = {}
        self._loading_workspace = False
        self._loading = False
        self._last_type_index = None
        self.dirty = False
        self._build_ui()
        if mod:
            self.open_mod_path(mod)
        else:
            self.refresh_cards(select_id=1)
        self.refresh_art_list(select_id=self.current)
        for workspace_name in self.NAV[2:]:
            self._refresh_workspace(workspace_name)
        # The .ui layout has not reached its final geometry during __init__.
        # Render once the event loop has shown and laid out the preview label.
        QTimer.singleShot(0, self._render_preview)
    def _load_game(self, game):
        try:
            files = disc.load(game) if game else disc.find_game()
        except (OSError, disc.GameFilesError) as problem:
            files = None
            message = str(problem)
        else:
            message = ""
        if files is None:
            folder = QFileDialog.getExistingDirectory(self, "Choose the game files", str(Path.cwd()))
            if folder:
                try:
                    files = disc.load(folder)
                except (OSError, disc.GameFilesError) as problem:
                    QMessageBox.critical(self, "Game files", str(problem))
                    raise SystemExit(2)
            else:
                QMessageBox.warning(self, "Game files needed", message or
                                    "Choose a folder with SLUS_014.11 and DATA/WA_MRG.MRG to continue.")
                raise SystemExit(2)
        return files
    def _build_ui(self):
        self.setStyleSheet(APP_QSS)
        root = QWidget(objectName="root")
        outer = QVBoxLayout(root)
        outer.setContentsMargins(10, 10, 10, 8)
        outer.setSpacing(8)
        self.setCentralWidget(root)

        top = QWidget(objectName="topbar")
        top_layout = QHBoxLayout(top)
        top_layout.setContentsMargins(16, 10, 14, 10)
        brand = QLabel("🎮  Yu-Gi-Oh! Forbidden Memories")
        brand.setStyleSheet("font-size:19px;font-weight:700;color:#f3f7fc")
        sub = QLabel("Mod Editor")
        sub.setStyleSheet("font-size:14px;color:#aebfd6")
        self.workspace_toggle = QPushButton("☰")
        self.workspace_toggle.setObjectName("workspaceToggle")
        self.workspace_toggle.setCheckable(True)
        self.workspace_toggle.setChecked(True)
        self.workspace_toggle.setToolTip("Collapse workspace navigation")
        self.workspace_toggle.setAccessibleName("Toggle workspace navigation")
        self.workspace_toggle.toggled.connect(self._toggle_workspace)
        top_layout.addWidget(self.workspace_toggle)
        top_layout.addWidget(brand)
        top_layout.addWidget(sub)
        top_layout.addStretch(1)
        self.game_label = QLabel("●  Game loaded")
        self.game_label.setStyleSheet("color:#13866a;font-weight:600")
        top_layout.addWidget(self.game_label)
        for label, slot in (("New mod", self.new_mod), ("Open mod", self.open_mod),
                            ("Preview mod.json", self.preview_manifest),
                            ("Save as…", lambda: self.save_mod(choose=True)), ("Save", self.save_mod)):
            button = QPushButton(label)
            if label == "Save":
                button.setObjectName("primary")
            button.clicked.connect(slot)
            top_layout.addWidget(button)
        outer.addWidget(top)

        body = QSplitter(Qt.Orientation.Horizontal)
        body.setObjectName("workspaceSplitter")
        body.setHandleWidth(20)
        outer.addWidget(body, 1)
        self.workspace_splitter = body
        sidebar = QWidget(objectName="sidebar")
        self.workspace_sidebar = sidebar
        sidebar.setMinimumWidth(155)
        sidebar.setMaximumWidth(230)
        nav = QVBoxLayout(sidebar)
        nav.setContentsMargins(10, 12, 10, 12)
        section = QLabel("WORKSPACE")
        section.setStyleSheet("font-size:11px;font-weight:700;color:#75849a")
        nav.addWidget(section)
        self.nav_buttons = {}
        self.workspace_indices = {"Cards": 0, "Art": 1}
        for name in self.NAV:
            b = QPushButton(name)
            b.setFlat(True)
            b.setCheckable(True)
            b.setMinimumHeight(38)
            b.setChecked(name == "Cards")
            b.clicked.connect(lambda checked=False, page=name: self.select_workspace(page))
            if name == "Cards":
                b.setObjectName("primary")
            nav.addWidget(b)
            self.nav_buttons[name] = b
        nav.addStretch(1)
        body.addWidget(sidebar)

        self.pages = QStackedWidget()
        cards = QWidget()
        self._build_cards(cards)
        self.pages.addWidget(cards)
        art_page = QWidget()
        self._build_art_page(art_page)
        self.pages.addWidget(art_page)
        for name in self.NAV[2:]:
            page = QWidget()
            self._build_workspace_page(name, page)
            self.workspace_indices[name] = self.pages.addWidget(page)
        # Every list a page shows sorts by the column you click, bar the handful
        # UNSORTED_TABLES names. Done here, once all the pages are built, so the
        # three with hand-built forms (cards, art, packs) are in it as much as
        # the ones loaded from a .ui file.
        for index in range(self.pages.count()):
            for table in self.pages.widget(index).findChildren(QTableWidget):
                allow_sorting(table)
        body.addWidget(self.pages)
        body.setStretchFactor(0, 0)
        body.setStretchFactor(1, 1)
        body.setSizes([185, 1350])
        self.statusBar().showMessage(f"Game files: {self.files.source}")

        self._build_menus()
    def _toggle_workspace(self, expanded):
        self.workspace_sidebar.setVisible(expanded)
        self.workspace_toggle.setToolTip(
            "Collapse workspace navigation" if expanded else "Expand workspace navigation")
        if expanded:
            self.workspace_splitter.setSizes([185, max(1, self.workspace_splitter.width() - 205)])
    def select_workspace(self, name):
        if name not in self.workspace_indices:
            return
        if self.current_workspace == "Cards" and name != "Cards":
            if self.current and not self.apply_card(quiet=True):
                self._set_navigation_active(self.current_workspace)
                return
        if self.current_workspace == "Mod info" and name != "Mod info":
            if not self._apply_mod_info():
                self._set_navigation_active(self.current_workspace)
                return
        if self.current_workspace == "Packs" and name != "Packs":
            if not self._commit_packs():
                self._set_navigation_active(self.current_workspace)
                return
        self.pages.setCurrentIndex(self.workspace_indices[name])
        self.current_workspace = name
        self._set_navigation_active(name)
        if name == "Art" and self.current_art_card is None:
            self.refresh_art_list(select_id=self.current)
        if name in self.NAV[2:]:
            self._refresh_workspace(name)
    def _set_navigation_active(self, name):
        for page_name, button in self.nav_buttons.items():
            button.setChecked(page_name == name)
            button.setObjectName("primary" if page_name == name else "")
            button.style().unpolish(button)
            button.style().polish(button)
    # A page's tabs are forms of their own, one file each, so a tab is read
    # and changed without the rest of the page around it. The page's form
    # keeps the empty tab widget; these fill it, in this order.
    PAGE_TABS = {
        # The campaign's story pages come with the campaign work, in a PR of
        # their own; this page is the map.
        "Campaign": ("campaignTabs", (("campaign_map.ui", "Map"),)),
        "Fusions": ("fusionPages", (("fusions_fusion_list.ui", "Fusion list"),
                                    ("fusions_generic_fusions.ui", "Generic fusions"))),
        "Starter decks": ("starterTabs", (("starter_written_decks.ui", "Written decks"),
                                          ("starter_weighted_pools.ui", "Weighted pools"))),
    }

    def _load_page_tabs(self, name, page):
        """Put each tab's own form into the page's tab widget, before anything
        is wired: the page is then the same object tree as one file would give."""
        wanted = self.PAGE_TABS.get(name)
        if wanted is None:
            return
        book_name, tabs = wanted
        book = page.findChild(QTabWidget, book_name)
        if book is None:
            raise RuntimeError(f"{name} form is missing {book_name!r}")
        loader = QUiLoader()
        for filename, title in tabs:
            path = UI_DIR / filename
            source = QFile(str(path))
            if not source.open(QIODevice.OpenModeFlag.ReadOnly):
                raise RuntimeError(f"Could not open {title} form {path}: {source.errorString()}")
            tab = loader.load(source, book)
            source.close()
            if tab is None:
                raise RuntimeError(f"Could not load {title} form {path}: {loader.errorString()}")
            book.addTab(tab, title)

    def _build_workspace_page(self, name, parent):
        if name == "Packs":
            self._build_packs_page(parent)
            return
        filenames = {"Campaign": "campaign_page.ui", "Fusions": "fusions_page.ui",
                     "Equips": "equips_page.ui", "Rituals": "rituals_page.ui",
                     "Duelists": "duelists_page.ui", "Starter decks": "starter_decks_page.ui",
                     "Limits": "limits_page.ui",
                     "Guardian Stars": "guardian_stars_page.ui",
                     "Mod info": "mod_info_page.ui", "Problems": "problems_page.ui"}
        path = UI_DIR / filenames[name]
        source = QFile(str(path))
        if not source.open(QIODevice.OpenModeFlag.ReadOnly):
            raise RuntimeError(f"Could not open {name} form {path}: {source.errorString()}")
        loader = QUiLoader()
        page = loader.load(source, parent)
        source.close()
        if page is None:
            raise RuntimeError(f"Could not load {name} form {path}: {loader.errorString()}")
        self._load_page_tabs(name, page)
        wrapper = QVBoxLayout(parent)
        wrapper.setContentsMargins(0, 0, 0, 0)
        wrapper.addWidget(page)
        controls = {"page": page}
        self.workspace_forms[name] = page
        self.workspace_controls[name] = controls
        if name == "Rituals":
            splitter=page.findChild(QSplitter,"ritualSplit")
            if splitter is not None:
                splitter.setSizes([330,950])
                splitter.setStretchFactor(0,0)
                splitter.setStretchFactor(1,1)
            recipe_layout=page.findChild(QVBoxLayout,"ritualRecipeLayout")
            if recipe_layout is not None:
                recipe_layout.setAlignment(Qt.AlignmentFlag.AlignTop)
        if name == "Duelists":
            splitter=page.findChild(QSplitter,"duelistSplit")
            if splitter is not None:
                splitter.setChildrenCollapsible(False)
                splitter.setStretchFactor(0,0)
                splitter.setStretchFactor(1,1)
                splitter.setSizes([560,900])

        def get(cls, key):
            item = page.findChild(cls, key)
            if item is None:
                raise RuntimeError(f"{name} form is missing {key!r}")
            return item

        def optional(cls, key):
            return page.findChild(cls, key)

        if name == "Campaign":
            controls.update(tabs=get(QTabWidget, "campaignTabs"))
            # The card look, by a selector naming the frame itself: a bare
            # stylesheet here would paint every child the same.
            for panel in ("mapToolbar", "mapPlacesPanel", "mapPreviewPanel"):
                get(QFrame, panel).setStyleSheet(
                    f"QFrame#{panel} {{ background:#101b2b; border:1px solid #26374c; border-radius:10px; }}")
            self._build_map_tab(page, get, controls)
        elif name == "Fusions":
            fusion_pages = get(QTabWidget, "fusionPages")
            header = get(QWidget, "fusionImageHeader")
            sort_names = ("fusionSortCardAButton", "fusionSortCardBButton",
                          "fusionSortResultButton", "fusionSortAtkButton",
                          "fusionSortDefButton", "fusionSortStateButton")
            sort_headers = [get(QPushButton, control_name) for control_name in sort_names]
            header_layout = header.layout()
            for column, button in enumerate(sort_headers):
                button.setCursor(Qt.CursorShape.PointingHandCursor)
                button.clicked.connect(
                    lambda checked=False, col=column: self._sort_fusions(col))
                button.setMinimumWidth(0)
                button.setMaximumWidth(16777215)
                button.setStyleSheet("text-align:center")
            # Plus/arrow separators take 18 px immediately before the State column
            # in the row delegate; reserve the same space in the header layout.
            if not header.property("fusionStateGapAdded"):
                header_layout.insertSpacing(5, 18)
                header.setProperty("fusionStateGapAdded", True)
            controls.update(search=get(QLineEdit, "fusionSearchEdit"),
                            changed=get(QCheckBox, "fusionChangedCheck"),
                            count=get(QLabel, "fusionCountLabel"), table=get(QTableWidget, "fusionTable"),
                            image_list=get(QListWidget, "fusionImageList"),
                            stack=get(QStackedWidget, "fusionViewStack"),
                            grid=get(QPushButton, "fusionGridButton"),
                            list=get(QPushButton, "fusionListButton"), header=header,
                            sort_headers=sort_headers, sort=(0, True), pages=fusion_pages,
                            search_materials=get(QRadioButton, "fusionSearchMaterialsRadio"),
                            search_results=get(QRadioButton, "fusionSearchResultsRadio"),
                            search_both=get(QRadioButton, "fusionSearchBothRadio"))
            controls["search"].setMaximumWidth(360)
            controls["search_materials"].toggled.connect(self._refresh_fusions)
            controls["search_results"].toggled.connect(self._refresh_fusions)
            controls["search_both"].toggled.connect(self._refresh_fusions)
            table = controls["table"]
            table.setColumnCount(6)
            table.setHorizontalHeaderLabels(["Card A", "Card B", "Result", "ATK", "DEF", "State"])
            view_group = QButtonGroup(self)
            view_group.setExclusive(True)
            view_group.addButton(controls["grid"], 0)
            view_group.addButton(controls["list"], 1)
            controls["view_group"] = view_group
            view_group.idClicked.connect(self._set_fusion_view)
            controls["image_list"].setItemDelegate(
                FusionImageDelegate(self._fusion_card_pixmap, controls["image_list"]))
            self._set_fusion_view(1)
            controls["image_list"].itemDoubleClicked.connect(lambda *_: self._edit_fusion())
            controls["search"].textChanged.connect(self._refresh_fusions)
            controls["changed"].toggled.connect(self._refresh_fusions)
            controls["table"].cellDoubleClicked.connect(lambda *_: self._edit_fusion())
            for key, callback in (("addFusionButton", lambda: self._edit_fusion(add=True)),
                                  ("editFusionButton", self._edit_fusion),
                                  ("removeFusionButton", self._remove_fusions),
                                  ("revertFusionButton", self._revert_fusions)):
                get(QPushButton, key).clicked.connect(callback)
            for listing in (controls["image_list"], table):
                listing.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
            remove_result = QPushButton("Remove recipes of…")
            button = get(QPushButton, "removeFusionButton")
            # Use the named Designer action row, keeping the button with its peers.
            action_layout = get(QPushButton, "revertFusionButton").parentWidget().layout()
            def add_to_row(layout):
                for i in range(layout.count()):
                    item = layout.itemAt(i)
                    if item.widget() is button:
                        layout.insertWidget(i + 1, remove_result)
                        return True
                    if item.layout() and add_to_row(item.layout()):
                        return True
                return False
            add_to_row(action_layout)
            remove_result.clicked.connect(self._remove_fusion_result)
            controls["header_timer"] = QTimer(self)
            controls["header_timer"].setSingleShot(True)
            controls["header_timer"].timeout.connect(self._sync_fusion_header)
            controls["image_list"].viewport().installEventFilter(self)
            controls["image_list"].verticalScrollBar().rangeChanged.connect(
                lambda *_: controls["header_timer"].start(0))
            fusion_pages.currentChanged.connect(lambda *_: controls["header_timer"].start(0))
            self._build_bulk_fusions_page(get(QWidget, "genericFusionTab"), controls)
        elif name == "Equips":
            controls.update(equips=get(QTableWidget, "equipTable"),
                            monsters=get(QTableWidget, "equipMonsterTable"),
                            heading=get(QLabel, "equipHeadingLabel"),
                            preview=get(QLabel, "equipCardPreview"),
                            search=get(QLineEdit, "equipSearchEdit"),
                            monster_search=get(QLineEdit, "equipMonsterSearchEdit"),
                            type=get(QComboBox, "equipTypeCombo"))
            controls["type"].addItems(TYPE_NAMES[:20])
            controls["search"].textChanged.connect(self._filter_equip_cards)
            controls["monster_search"].textChanged.connect(self._filter_equip_monsters)
            controls["equips"].itemSelectionChanged.connect(self._select_equip)
            controls["monsters"].itemChanged.connect(self._toggle_equip_monster)
            controls["monsters"].setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
            controls["monsters"].setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
            for key, callback in (("addMonsterButton", self._add_equip_monster),
                                  ("addTypeButton", lambda: self._equip_by_type(True)),
                                  ("removeTypeButton", lambda: self._equip_by_type(False)),
                                  ("removeMonsterButton", self._remove_equip_monsters),
                                  ("revertEquipButton", self._revert_equip)):
                get(QPushButton, key).clicked.connect(callback)
        elif name == "Rituals":
            controls.update(cards=get(QTableWidget, "ritualCardTable"),
                            preview=get(QLabel, "ritualCardPreview"),
                            card_meta={key:widget for key,widget in (("id",optional(QLineEdit,"ritualMetaId")),
                                       ("name",optional(QLineEdit,"ritualMetaName")),
                                       ("type",optional(QComboBox,"ritualMetaType")),
                                       ("attribute",optional(QComboBox,"ritualMetaAttribute")),
                                       ("level",optional(QSpinBox,"ritualMetaLevel")),
                                       ("atk",optional(QSpinBox,"ritualMetaAtk")),
                                       ("def",optional(QSpinBox,"ritualMetaDef"))) if widget is not None},
                            search=get(QLineEdit, "ritualSearchEdit"),
                            heading=get(QLabel, "ritualRecipeHeading"),
                            tribute_images=[get(QLabel,f"ritualTribute{i}Image") for i in range(1,4)],
                            tribute_fields=[{"id":get(QLineEdit,f"ritualTribute{i}Id"),"atk":get(QSpinBox,f"ritualTribute{i}Atk"),
                                             "def":get(QSpinBox,f"ritualTribute{i}Def"),"type":get(QComboBox,f"ritualTribute{i}Type"),
                                             "attribute":get(QComboBox,f"ritualTribute{i}Attribute"),"level":get(QSpinBox,f"ritualTribute{i}Level")} for i in range(1,4)],
                            tribute_combos=[get(QComboBox,f"ritualTribute{i}Combo") for i in range(1,4)],
                            summon_combo=get(QComboBox,"ritualSummonCombo"),
                            summon_image=get(QLabel,"ritualSummonImage"),
                            summon_fields={"id":get(QLineEdit,"ritualSummonId"),"atk":get(QSpinBox,"ritualSummonAtk"),
                                           "def":get(QSpinBox,"ritualSummonDef"),"type":get(QComboBox,"ritualSummonType"),
                                           "attribute":get(QComboBox,"ritualSummonAttribute"),"level":get(QSpinBox,"ritualSummonLevel")},
                            state=get(QLabel,"ritualStateLabel"))
            self.ritual_pending = {}
            controls["cards"].setColumnCount(4)
            controls["cards"].setHorizontalHeaderLabels(["ID", "Ritual card", "State", "Recipe"])
            for combo in controls["tribute_combos"]+[controls["summon_combo"]]:
                combo.setEditable(True)
                combo.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
                combo.addItem("Choose a card…", None)
                for cid in self.project.monsters():
                    combo.addItem(self.project.card_label(cid), cid)
                combo.currentIndexChanged.connect(self._stage_ritual_selector_recipe)
            for fields in [controls["card_meta"],*controls["tribute_fields"],controls["summon_fields"]]:
                for key in ("type","attribute"):
                    if fields.get(key) is not None:
                        fields[key].addItems(TYPE_NAMES if key == "type" else ATTRIBUTE_NAMES)
                        fields[key].setEnabled(False)
                for key in ("id","name"):
                    if fields.get(key) is not None:fields[key].setReadOnly(True)
                for key in ("level","atk","def"):
                    if fields.get(key) is not None:fields[key].setReadOnly(True)
            controls["cards"].itemSelectionChanged.connect(self._select_ritual)
            controls["search"].textChanged.connect(self._filter_ritual_cards)
            controls["cards"].horizontalHeader().setSectionResizeMode(0,QHeaderView.ResizeMode.Fixed)
            controls["cards"].horizontalHeader().resizeSection(0,58)
            controls["cards"].horizontalHeader().setSectionResizeMode(1,QHeaderView.ResizeMode.Stretch)
            controls["cards"].horizontalHeader().setSectionResizeMode(2,QHeaderView.ResizeMode.Fixed)
            controls["cards"].horizontalHeader().resizeSection(2,90)
            controls["cards"].horizontalHeader().setSectionResizeMode(3,QHeaderView.ResizeMode.Stretch)
            for key, callback in (("editRitualButton", self._apply_ritual_selector_recipe),
                                  ("removeRitualButton", self._remove_ritual),
                                  ("revertRitualButton", self._revert_ritual)):
                get(QPushButton, key).clicked.connect(callback)
        elif name == "Duelists":
            controls.update(duelists=get(QTableWidget, "duelistTable"),
                            search=get(QLineEdit, "duelistSearchEdit"),
                            page=get(QComboBox, "duelistPageCombo"),
                            portrait=get(QLabel, "duelistPortraitPreview"),
                            name=get(QLineEdit, "duelistNameEdit"),
                            id=get(QLineEdit, "duelistIdEdit"),
                            base=get(QComboBox, "duelistBaseCombo"),
                            position=get(QComboBox, "duelistPositionCombo"),
                            pool=get(QComboBox, "duelistPoolCombo"),
                            summary=get(QLabel, "duelistPoolSummary"),
                            table=get(QTableWidget, "duelistPoolTable"),
                            weight=get(QSpinBox, "duelistWeightSpin"))
            controls["remove"] = get(QPushButton, "removeDuelistButton")
            # The deck pool may be forty cards written down instead of weights
            # (fixed_decks.py): its own list and buttons, shown in their place.
            controls.update(weighted_radio=get(QRadioButton, "weightedDeckRadio"),
                            fixed_radio=get(QRadioButton, "fixedDeckRadio"),
                            fixed_table=get(QTableWidget, "fixedDeckTable"),
                            copies=get(QSpinBox, "fixedCopiesSpin"),
                            fixed_hint=get(QLabel, "fixedDeckHint"),
                            mode_row=page.findChild(QHBoxLayout, "deckModeRow"),
                            fixed_actions=page.findChild(QHBoxLayout, "fixedDeckActionsLayout"),
                            pool_actions=page.findChild(QHBoxLayout, "duelistActionsLayout"))
            controls["fixed_table"].setColumnCount(6)
            controls["fixed_table"].itemSelectionChanged.connect(self._select_fixed_card)
            get(QRadioButton, "weightedDeckRadio").toggled.connect(self._switch_deck_mode)
            for key, callback in (("addFixedCardButton", self._add_fixed_card),
                                  ("setFixedCopiesButton", self._set_fixed_copies),
                                  ("removeFixedCardsButton", self._remove_fixed_cards),
                                  ("copyWeightedDeckButton", self._copy_weighted_deck),
                                  ("clearFixedDeckButton", self._clear_fixed_deck),
                                  ("revertFixedDeckButton", self._revert_fixed_deck)):
                get(QPushButton, key).clicked.connect(callback)
            self.fixed_stash = {}        # duelist -> the decks "Weighted" took out
            self.fixed_stash_of = None   # the project they belong to
            grid=controls["duelists"]
            grid.setRowCount(5);grid.setColumnCount(8)
            grid.horizontalHeader().setVisible(False);grid.verticalHeader().setVisible(False)
            grid.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
            grid.verticalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
            grid.setIconSize(QSize(48,48));grid.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
            grid.cellClicked.connect(self._select_duelist)
            controls["page"].addItems([f"Page {i + 1}" for i in range(4)])
            controls["page"].currentIndexChanged.connect(self._refresh_duelists)
            controls["search"].textChanged.connect(self._refresh_duelists)
            controls["base"].addItem("Choose a base…",None)
            for did,base_name in enumerate(DUELIST_NAMES):
                if did:controls["base"].addItem(f"{did:02d} · {base_name}",did)
            controls["position"].addItem("Automatic · first empty",None)
            for slot in range(40,128):controls["position"].addItem(f"Position {slot}",slot)
            controls["pool"].addItems([POOL_LABELS[p] for p in POOLS])
            controls["pool"].currentIndexChanged.connect(self._refresh_duelist_pool)
            controls["table"].itemSelectionChanged.connect(self._select_pool_card)
            get(QPushButton,"duelistPortraitButton").clicked.connect(self._choose_duelist_portrait)
            get(QPushButton,"addDuelistButton").clicked.connect(lambda: self._save_duelist(True))
            get(QPushButton,"applyDuelistButton").clicked.connect(lambda: self._save_duelist(False))
            controls["remove"].clicked.connect(self._remove_duelist)
            for key, callback in (("addPoolCardButton", self._add_pool_card),
                                  ("setPoolWeightButton", self._set_pool_weight),
                                  ("removePoolCardButton", self._remove_pool_cards),
                                  ("normalizePoolButton", self._normalize_pool),
                                  ("revertPoolButton", self._revert_pool)):
                get(QPushButton, key).clicked.connect(callback)
        elif name == "Starter decks":
            self._build_starter_pools(page, get, controls)
            controls.update(decks=get(QTableWidget, "starterDeckTable"),
                            name=get(QLineEdit, "starterNameEdit"),
                            weight=get(QSpinBox, "starterWeightSpin"),
                            cards=get(QTableWidget, "starterCardTable"),
                            copies=get(QSpinBox, "starterCopiesSpin"))
            controls["decks"].itemSelectionChanged.connect(self._select_starter_deck)
            controls["cards"].itemSelectionChanged.connect(self._select_starter_card)
            for key, callback in (("addDeckButton", self._add_starter_deck),
                                  ("removeDeckButton", self._remove_starter_deck),
                                  ("applyDeckButton", self._apply_starter_details),
                                  ("addDeckCardButton", self._add_starter_card),
                                  ("setDeckCopiesButton", self._set_starter_copies),
                                  ("removeDeckCardButton", self._remove_starter_cards)):
                get(QPushButton, key).clicked.connect(callback)
        elif name == "Limits":
            self._build_limits_page(page, get, controls)
        elif name == "Guardian Stars":
            self._build_stars_page(page, get, controls)
        elif name == "Mod info":
            controls.update(id=get(QLineEdit, "modIdEdit"), name=get(QLineEdit, "modNameEdit"),
                            version=get(QLineEdit, "modVersionEdit"), author=get(QLineEdit, "modAuthorEdit"),
                            description=get(QPlainTextEdit, "modDescriptionEdit"),
                            settings=get(QPlainTextEdit, "modSettingsEdit"),
                            other=get(QPlainTextEdit, "modOtherEdit"), status=get(QLabel, "modInfoStatusLabel"),
                            folder=get(QLabel, "modFolderLabel"))
            get(QPushButton, "applyModInfoButton").clicked.connect(self._apply_mod_info)
            get(QPushButton, "previewModJsonButton").clicked.connect(self.preview_manifest)
            self._style_mod_info_page(page, get, controls)
        elif name == "Problems":
            controls.update(table=get(QTableWidget, "problemsTable"),
                            summary=get(QLabel, "problemSummaryLabel"), issues=[])
            get(QPushButton, "checkModButton").clicked.connect(self._refresh_problems)
            controls["table"].cellDoubleClicked.connect(self._open_problem)
            controls["table"].setToolTip("Double-click a line to go to it.")
            controls["table"].setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        for table in page.findChildren(QTableWidget):
            table.setAlternatingRowColors(True)
            table.verticalHeader().setVisible(False)
            table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
            table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        if name == "Problems":
            # After the shared pass above: a level and the page it names are
            # short, and what is left belongs to the message.
            header=controls["table"].horizontalHeader()
            for column,width in ((0,90),(1,280)):
                header.setSectionResizeMode(column,QHeaderView.ResizeMode.Fixed)
                header.resizeSection(column,width)
            header.setSectionResizeMode(2,QHeaderView.ResizeMode.Stretch)
        elif name == "Equips":
            # The shared table setup above stretches every column. Override it
            # here so checkbox and numeric ID columns stay compact.
            equip_header=controls["equips"].horizontalHeader()
            equip_header.setSectionResizeMode(0,QHeaderView.ResizeMode.Fixed)
            equip_header.resizeSection(0,58)
            equip_header.setSectionResizeMode(1,QHeaderView.ResizeMode.Stretch)
            equip_header.setSectionResizeMode(2,QHeaderView.ResizeMode.Fixed)
            equip_header.resizeSection(2,58)
            monster_header=controls["monsters"].horizontalHeader()
            monster_header.setSectionResizeMode(0,QHeaderView.ResizeMode.Fixed)
            monster_header.resizeSection(0,42)
            monster_header.setSectionResizeMode(1,QHeaderView.ResizeMode.Fixed)
            monster_header.resizeSection(1,54)
            for col in (2,3,4):
                monster_header.setSectionResizeMode(col,QHeaderView.ResizeMode.Stretch)
            # Both lists hide what the search box leaves out, and a hidden row
            # is a row number rather than the line in it.
            sort_keeps_filter(controls["equips"],
                              lambda: self._filter_equip_cards(controls["search"].text()))
            sort_keeps_filter(controls["monsters"],
                              lambda: self._filter_equip_monsters(controls["monster_search"].text()))
        elif name == "Rituals":
            # After the shared pass above, which stretches every column.
            card_header=controls["cards"].horizontalHeader()
            for column,width in ((0,58),(1,190),(2,90)):
                card_header.setSectionResizeMode(column,QHeaderView.ResizeMode.Fixed)
                card_header.resizeSection(column,width)
            card_header.setSectionResizeMode(3,QHeaderView.ResizeMode.Stretch)
            sort_keeps_filter(controls["cards"],
                              lambda: self._filter_ritual_cards(controls["search"].text()))
        elif name == "Guardian Stars":
            # The shared pass above hides every vertical header, selects whole
            # rows and stretches every column. The matrix is a grid of cells
            # named on both axes, so it wants none of that.
            matrix = controls["matrix"]
            matrix.verticalHeader().setVisible(True)
            matrix.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectItems)
            matrix.setAlternatingRowColors(False)       # the cells carry their own colour
            matrix.horizontalHeader().setMinimumSectionSize(62)
            matrix.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
            matrix.verticalHeader().setMinimumWidth(140)
            matrix.verticalHeader().setDefaultSectionSize(36)
            star_header = controls["stars"].horizontalHeader()
            for column, width in ((0, 42), (2, 74), (3, 62)):
                star_header.setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
                star_header.resizeSection(column, width)
            star_header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self._refresh_workspace(name)
    def _refresh_workspace(self, name):
        if not hasattr(self, "workspace_controls") or name not in self.workspace_controls:
            return
        c, p = self.workspace_controls[name], self.project
        if name == "Campaign":
            self._refresh_map()
        elif name == "Fusions":
            self._refresh_fusions()
            self._refresh_bulk_fusion_plan()
        elif name == "Equips":
            self._refresh_equips()
        elif name == "Rituals":
            self._refresh_rituals()
        elif name == "Duelists":
            self._refresh_duelists()
        elif name == "Starter decks":
            self._refresh_starter_decks()
            self._refresh_starter_pools()
        elif name == "Limits":
            self._refresh_limits()
        elif name == "Guardian Stars":
            self._refresh_stars()
        elif name == "Packs":
            self._refresh_packs()
        elif name == "Mod info":
            self._refresh_mod_info()
        elif name == "Problems":
            self._refresh_problems()
    def _card_combo(self, parent, selected=None, ids=None):
        combo = QComboBox(parent)
        for cid in sorted(ids if ids is not None else self.project.cards):
            card = self.project.cards.get(cid)
            if card is not None:
                combo.addItem(f"{cid:03d}  {card.name}", cid)
        # Typing a number or a part of a name beats scrolling seven hundred
        # cards, as the Tk form's CardField does (widgets.card_named).
        combo.setEditable(True)
        combo.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        combo.lineEdit().setPlaceholderText("A number, or part of a name…")
        completer = combo.completer()
        if completer is not None:
            completer.setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
            completer.setFilterMode(Qt.MatchFlag.MatchContains)
        if selected is not None:
            index = combo.findData(selected)
            if index >= 0: combo.setCurrentIndex(index)
        else:
            combo.setCurrentIndex(-1)
            combo.clearEditText()
        return combo
    MAP_ZOOM = 2
    MAP_CANVAS = (cm.SCREEN[0] * MAP_ZOOM, cm.SCREEN[1] * MAP_ZOOM)
    MAP_CONDITIONS = ("always", "while the flag is set", "while the flag is clear")
    MAP_CONDITION_KINDS = ("always", "set", "clear")
    MAP_CONFIRM_SCENE = "(enter the place's own scene)"
    MAP_ARROW_GLYPHS = {"up": "▲", "down": "▼", "left": "◀", "right": "▶"}
    MAP_EDGE_COLOURS = {"always": "#4fc36b", "set": "#f2c04c", "clear": "#6fb1ff"}
    MAP_WORLD_BOX = (0, 40, 440, 480)
    MAP_TOWN_BOX = (448, 40, 640, 184)
    MAP_WORLD_CENTRE = (0, 0)
    MAP_WORLD_SPAN = 2800
    MAP_INTS = ("distance", "heading", "pitch", "target_x", "target_z", "marker_x", "marker_y")
    @staticmethod
    def _set_combo_card(combo, cid):
        """Show a card in a card combo by its number."""
        index = combo.findData(cid)
        if index >= 0:
            combo.setCurrentIndex(index)
        else:
            combo.setCurrentText(str(cid))
    def _combo_card_id(self, combo):
        """The card a card combo names, picked or typed: its number, the entry's
        own text, or a name (widgets.card_named). None when it names no card."""
        if not combo.isEditable():
            return combo.currentData()
        text = combo.currentText().strip()
        if not text:
            return None
        for flags in (Qt.MatchFlag.MatchFixedString, Qt.MatchFlag.MatchContains):
            index = combo.findText(text, flags)
            if index >= 0:
                return combo.itemData(index)
        cid = self.project.resolve(int(text) if text.isdigit() else text)
        return cid or None
    def eventFilter(self, watched, event):
        controls = getattr(self, "workspace_controls", {}).get("Fusions", {})
        image_list = controls.get("image_list")
        if image_list is not None and watched is image_list.viewport():
            if event.type() in (QEvent.Type.Resize, QEvent.Type.Show):
                timer = controls.get("header_timer")
                if timer is not None:
                    timer.start(0)
        if event.type() == QEvent.Type.MouseButtonDblClick and event.button() == Qt.MouseButton.LeftButton:
            for side in controls.get("bulk_filters", {}).values():
                for key in ("types", "stars"):
                    listing = side[key]
                    if watched is not listing.viewport():
                        continue
                    point = event.position().toPoint()
                    item = listing.itemAt(point)
                    if item is None:
                        break
                    option = QStyleOptionViewItem()
                    listing.itemDelegate().initStyleOption(option, listing.indexFromItem(item))
                    option.rect = listing.visualItemRect(item)
                    indicator = listing.style().subElementRect(QStyle.SubElement.SE_ItemViewItemCheckIndicator, option, listing)
                    if not indicator.contains(point):
                        item.setCheckState(Qt.CheckState.Unchecked if item.checkState() == Qt.CheckState.Checked
                                           else Qt.CheckState.Checked)
                        return True
        return super().eventFilter(watched, event)
    @staticmethod
    def _populate_checkable_list(listing, names):
        listing.clear()
        if not listing.property("toggleOnDoubleClick"):
            # Once per list, however often it is filled again: the name is a
            # bigger target than the box beside it.
            listing.setProperty("toggleOnDoubleClick", True)
            listing.setSpacing(2)
            listing.itemDoubleClicked.connect(
                lambda item: item.setCheckState(
                    Qt.CheckState.Unchecked if item.checkState() == Qt.CheckState.Checked
                    else Qt.CheckState.Checked))
        for index, name in enumerate(names):
            item = QListWidgetItem(name)
            item.setSizeHint(QSize(0, max(26, listing.fontMetrics().height() + 10)))
            item.setData(Qt.ItemDataRole.UserRole, index)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Unchecked)
            listing.addItem(item)
    @staticmethod
    def _set_layout_widgets_visible(layout, visible):
        """Show/hide all widgets in an advanced-filter layout from Designer."""
        panel = layout.parentWidget()
        if panel is not None and panel.property("bulkAdvancedPanel"):
            panel.setVisible(visible)
            return
        for index in range(layout.count()):
            item = layout.itemAt(index)
            if item.widget() is not None:
                item.widget().setVisible(visible)
            elif item.layout() is not None:
                ModernEditor._set_layout_widgets_visible(item.layout(), visible)
    @staticmethod
    def _checkable_list(names, parent, height=6):
        listing = QListWidget(parent)
        listing.setMaximumHeight(108)
        # Room between the boxes: ticking the one you meant is hard when the
        # rows are a line of text apart.
        listing.setSpacing(3)
        listing.setStyleSheet("QListWidget::item { padding: 3px 2px; }")
        for index, name in enumerate(names):
            item = QListWidgetItem(name)
            item.setData(Qt.ItemDataRole.UserRole, index)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Unchecked)
            listing.addItem(item)
        # The box is a small target; double-clicking the name does the same.
        listing.itemDoubleClicked.connect(
            lambda item: item.setCheckState(
                Qt.CheckState.Unchecked if item.checkState() == Qt.CheckState.Checked
                else Qt.CheckState.Checked))
        return listing
    @staticmethod
    def _checked_items(listing):
        return [listing.item(i) for i in range(listing.count())
                if listing.item(i).checkState() == Qt.CheckState.Checked]
    def _choose_one_card(self,title,ids=None,selected=None):
        dialog=QDialog(self);dialog.setWindowTitle(title);layout=QVBoxLayout(dialog)
        combo=self._card_combo(dialog,selected=selected,ids=ids);layout.addWidget(combo)
        buttons=QHBoxLayout();ok=QPushButton("Select");cancel=QPushButton("Cancel")
        buttons.addWidget(ok);buttons.addWidget(cancel);layout.addLayout(buttons)
        ok.clicked.connect(dialog.accept);cancel.clicked.connect(dialog.reject)
        return self._combo_card_id(combo) if dialog.exec() == QDialog.DialogCode.Accepted else None
    RITUAL_CONDITIONS = (("Specific card", "card", None), ("Monster type", "type", None),
                         ("Fusion group", "fusion_group", None),
                         ("Minimum ATK", "min_attack", (0, 9999, 1000)),
                         ("Minimum DEF", "min_defense", (0, 9999, 1000)),
                         ("Maximum ATK", "max_attack", (0, 9999, 1000)),
                         ("Maximum DEF", "max_defense", (0, 9999, 1000)),
                         ("Minimum level", "min_level", (0, 12, 1)),
                         ("Maximum level", "max_level", (0, 12, 12)),
                         ("DEF > ATK", "defense_gt_attack", None))
    LP_UNSET = "—"      # a field left at the side's own number
    STAR_CELL_COLOURS = {"plus": "#1f4d2c", "minus": "#5a2323", "zero": "#16212f"}
    MOD_INFO_OWNED = ("limits", "guardian_stars", "starter_pools", "story")
    MOD_INFO_RESERVED = frozenset(manifest.INFO_KEYS + manifest.TABLE_KEYS + MOD_INFO_OWNED)
    @staticmethod
    def _select_table_id(table, value, column=0):
        """Select the row whose `column` carries `value` as its id, showing it
        even when a search has hidden it."""
        for row in range(table.rowCount()):
            item = table.item(row, column)
            if item is not None and item.data(Qt.ItemDataRole.UserRole) == value:
                table.setRowHidden(row, False)
                table.selectRow(row)
                table.scrollToItem(item, QAbstractItemView.ScrollHint.PositionAtCenter)
                return True
        return False
    def _build_menus(self):
        file_menu = self.menuBar().addMenu("File")
        entries = (("New mod", self.new_mod, "Ctrl+N"),
                   ("Open mod folder…", self.open_mod, "Ctrl+O"),
                   ("Save", self.save_mod, "Ctrl+S"),
                   ("Save as…", lambda: self.save_mod(choose=True), "Ctrl+Shift+S"))
        for title, callback, shortcut in entries:
            action = file_menu.addAction(title)
            action.setShortcut(shortcut)
            action.triggered.connect(callback)
        file_menu.addSeparator()
        file_menu.addAction("Import a modified game (.bin or SLUS_014.11)…").triggered.connect(self.import_modded_game)
        file_menu.addAction("Convert an old recomp's .ygomods package (one way)…").triggered.connect(self.import_ygomods)
        file_menu.addSeparator()
        game_action = file_menu.addAction("Game files…")
        game_action.triggered.connect(self.choose_game_files)
        file_menu.addSeparator()
        exit_action = file_menu.addAction("Exit")
        exit_action.setShortcut("Ctrl+Q")
        exit_action.triggered.connect(self.close)

        tools_menu = self.menuBar().addMenu("Tools")
        for title, callback in (("Check the mod", self.check_mod),
                                ("Preview mod.json", self.preview_manifest),
                                ("Card text preview", self.preview_card_text),
                                ("Remove every pack of the mod", self._reset_packs)):
            tools_menu.addAction(title).triggered.connect(callback)

        view_menu = self.menuBar().addMenu("View")
        self.dark_action = view_menu.addAction("Dark mode")
        self.dark_action.setCheckable(True)
        self.dark_action.setChecked(settings.load().get("modern_dark", True))
        self.dark_action.toggled.connect(self.toggle_dark)
        self.toggle_dark(self.dark_action.isChecked(), remember=False)
        passwords_action = view_menu.addAction("Show card passwords")
        passwords_action.setCheckable(True)
        passwords_action.setChecked(True)
        passwords_action.toggled.connect(self.toggle_password_visibility)
        refresh_action = view_menu.addAction("Refresh card preview")
        refresh_action.setShortcut("F5")
        refresh_action.triggered.connect(self._render_preview)

        help_menu = self.menuBar().addMenu("Help")
        help_menu.addAction("About").triggered.connect(self.show_about)
    @staticmethod
    def _apply_palette(dark):
        """The window is themed by stylesheet, but a few surfaces are drawn by
        the style from the palette and nothing else: a combo popup's frame, a
        tooltip, a menu's edge. Without this they stay light."""
        application = QApplication.instance()
        if application is None:
            return
        if not dark:
            application.setPalette(application.style().standardPalette())
            return
        palette = QPalette()
        ink, dim = QColor("#e5edf8"), QColor("#7f8ca0")
        for role, colour in ((QPalette.ColorRole.Window, "#0b1220"),
                             (QPalette.ColorRole.Base, "#162337"),
                             (QPalette.ColorRole.AlternateBase, "#142236"),
                             (QPalette.ColorRole.Button, "#162337"),
                             (QPalette.ColorRole.ToolTipBase, "#101b2b"),
                             (QPalette.ColorRole.Highlight, "#1e64df")):
            palette.setColor(role, QColor(colour))
        for role in (QPalette.ColorRole.WindowText, QPalette.ColorRole.Text,
                     QPalette.ColorRole.ButtonText, QPalette.ColorRole.ToolTipText,
                     QPalette.ColorRole.HighlightedText):
            palette.setColor(role, ink)
        for role in (QPalette.ColorRole.WindowText, QPalette.ColorRole.Text,
                     QPalette.ColorRole.ButtonText):
            palette.setColor(QPalette.ColorGroup.Disabled, role, dim)
        application.setPalette(palette)
    def toggle_dark(self, enabled, remember=True):
        # Transform the existing palette, including per-widget styles, so the
        # same layout and semantic accent colours are used in both modes.
        def light_style(style):
            def colour(match):
                value = QColor(match.group())
                hue, saturation, lightness, alpha = value.getHsl()
                return QColor.fromHsl(hue, saturation, 255 - lightness, alpha).name()
            return re.sub(r"#[0-9a-fA-F]{6}\b", colour, style)

        for widget in [self, *self.findChildren(QWidget)]:
            original = widget.property("darkStyle")
            if original is None:
                original = widget.styleSheet()
                widget.setProperty("darkStyle", original)
            widget.setStyleSheet(original if enabled else light_style(original))
        self._apply_palette(enabled)
        self.dark_mode = enabled
        if remember:
            # The lists' state colours are item inks, not styles, so the
            # stylesheet pass above cannot turn them over: draw them again.
            self.refresh_cards(select_id=self.current)
            self.refresh_art_list(select_id=self.current_art_card)
            self._refresh_fusions()
            problem = settings.save("modern_dark", enabled)
            if problem:
                self.statusBar().showMessage(f"Could not remember the theme: {problem}")
    def _report(self, title, message):
        dialog = QDialog(self)
        dialog.setWindowTitle(title)
        dialog.resize(760, 560)
        layout = QVBoxLayout(dialog)
        text = QPlainTextEdit()
        text.setReadOnly(True)
        text.setPlainText(message)
        layout.addWidget(text)
        close = QPushButton("Close")
        close.clicked.connect(dialog.accept)
        layout.addWidget(close)
        dialog.exec()
    def _use_import(self, project, report, source):
        self.project = project
        self.current = None
        self.duelist_selected_slot = 0
        self.frame_cache.clear()
        self.fusion_art_cache.clear()
        self.refresh_cards(select_id=1)
        self.current_art_card = 1
        self.refresh_art_list(select_id=1)
        for name in self.NAV[2:]:
            self._refresh_workspace(name)
        self.dirty = False
        self._mark_dirty()
        self.statusBar().showMessage(f"Imported {source}: save it to write the mod folder.")
        self._report("Import report", "\n".join(report) +
                     "\n\nThe report is saved with the mod as import-report.txt.")
    def import_modded_game(self):
        if not self.confirm_discard():
            return
        path, _ = QFileDialog.getOpenFileName(self, "Modified disc image or SLUS_014.11", "",
            "Game files (*.bin *.iso *.img *.11);;All files (*)")
        if not path:
            return
        source = Path(path)
        try:
            if source.suffix.lower() in (".bin", ".iso", ".img"):
                files, name = disc.load(source), source.stem
            else:
                wa = source.parent / "DATA" / "WA_MRG.MRG"
                if not wa.is_file():
                    wa = source.parent / "WA_MRG.MRG"
                if not wa.is_file():
                    chosen, _ = QFileDialog.getOpenFileName(self, "Modified WA_MRG.MRG", "",
                                                           "Game archive (*.MRG);;All files (*)")
                    if not chosen:
                        return
                    wa = Path(chosen)
                files, name = disc.load_pair(source, wa), source.parent.name
            result = importer.import_modded(self.files, files, importer.slug(name), name)
        except Exception as problem:
            QMessageBox.critical(self, "Import", f"The import stopped: {type(problem).__name__}: {problem}")
            return
        self._use_import(result.project, result.report, source)
    def import_ygomods(self):
        if not self.confirm_discard():
            return
        path, _ = QFileDialog.getOpenFileName(self, "Convert an old recomp's .ygomods package", "",
                                             "Old recomp package (*.ygomods);;All files (*)")
        if not path:
            return
        try:
            project, report = ygomods.import_package(self.retail, self.files.wa, path,
                                                     importer.slug(path), Path(path).stem)
        except Exception as problem:
            QMessageBox.critical(self, "Import", f"The import stopped: {type(problem).__name__}: {problem}")
            return
        self._use_import(project, ["One-way conversion; check the result in the game.", *report], path)
    def mods_dir(self):
        folder = disc.user_dir() / "mods"
        return folder if folder.is_dir() else Path.cwd()
    def choose_game_files(self):
        if not self.confirm_discard():
            return
        path, _ = QFileDialog.getOpenFileName(
            self, "Choose the game's disc image", str(Path.cwd()),
            "Disc images (*.bin *.iso *.img);;All files (*)")
        if not path:
            path = QFileDialog.getExistingDirectory(
                self, "Or choose a folder with SLUS_014.11 and DATA/WA_MRG.MRG", str(Path.cwd()))
        if not path:
            return
        try:
            files = disc.load(path)
            retail = gamedata.load_game(files)
        except Exception as problem:
            QMessageBox.critical(self, "Game files", str(problem))
            return
        self.files = files
        self.retail = retail
        self.project = Project(retail)
        self.current = None
        self.frame_cache.clear()
        self.fusion_art_cache.clear()
        self.dirty = False
        self.refresh_cards(select_id=1)
        self.current_art_card = 1
        self.refresh_art_list(select_id=1)
        for name in self.NAV[2:]: self._refresh_workspace(name)
        self.game_label.setText("●  Game loaded")
        self.statusBar().showMessage(f"Game files: {files.source}")
    def check_mod(self):
        if self.current and not self.apply_card(quiet=True):
            return
        if self.current_workspace == "Mod info" and not self._apply_mod_info(): return
        if self.current_workspace == "Packs" and not self._commit_packs(): return
        issues = validate.validate(self.project)
        errors = validate.errors(issues)
        warnings = [issue for issue in issues if issue.level == "warning"]
        if not issues:
            QMessageBox.information(self, "Check the mod", "No problems found.")
            return
        details = "\n".join(str(issue) for issue in issues[:150])
        if len(issues) > 150:
            details += f"\n… and {len(issues) - 150} more."
        QMessageBox.warning(self, "Mod check",
                            f"{len(errors)} error(s), {len(warnings)} warning(s).\n\n{details}")
    def preview_manifest(self):
        if self.current and not self.apply_card(quiet=True):
            return
        if self.current_workspace == "Mod info" and not self._apply_mod_info():
            return
        dialog = QDialog(self)
        dialog.setWindowTitle("Preview mod.json")
        dialog.resize(760, 620)
        layout = QVBoxLayout(dialog)
        text = QTextEdit()
        text.setReadOnly(True)
        text.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        text.setPlainText(manifest.dumps(manifest.build(self.project)))
        layout.addWidget(text)
        close_button = QPushButton("Close")
        close_button.clicked.connect(dialog.accept)
        layout.addWidget(close_button, alignment=Qt.AlignmentFlag.AlignRight)
        dialog.exec()
    def show_about(self):
        QMessageBox.about(self, "About FM Editor",
                          "FM Editor\n\nMakes mods for the PC port of Yu-Gi-Oh! Forbidden Memories. "
                          "It reads retail tables from your game files and saves only mod changes. "
                          "It never writes to the disc or game folder.")
    def _panel(self, title: str):
        frame = QFrame(objectName="panel")
        frame.setProperty("class", "panel")
        layout = QVBoxLayout(frame)
        layout.setContentsMargins(14, 12, 14, 12)
        layout.setSpacing(10)
        heading = QLabel(title, objectName="heading")
        heading.setProperty("class", "heading")
        layout.addWidget(heading)
        return frame, layout
    def _card_matches(self, cid, text):
        """The card list's search: the card's number exactly, or a part of its
        name, text or notes (model.card_matches, widened to the text and the
        notes). The number is matched whole: "1" finds card 1, not every card
        with a 1 in its id."""
        text = text.casefold().strip()
        if not text:
            return True
        if text == str(cid):
            return True
        card = self.project.cards[cid]
        return text in f"{card.name} {card.description} {self.project.notes.get(cid, '')}".casefold()
    @staticmethod
    def _clear_checkable_list(listing):
        for i in range(listing.count()):
            listing.item(i).setCheckState(Qt.CheckState.Unchecked)
    def _wanted(self, cid):
        card = self.project.cards[cid]
        kind = self.filter.currentText()
        if kind == "Changed" and not self.project.card_changed(cid): return False
        if kind == "Added by the mod" and cid not in self.project.added: return False
        if kind == "With notes" and cid not in self.project.notes: return False
        if kind == "Monsters" and not card.is_monster(): return False
        if kind == "Non-monsters" and card.is_monster(): return False
        if kind in TYPE_NAMES and card.type != TYPE_NAMES.index(kind): return False
        advanced = getattr(self, "advanced_card_filters", {})
        if advanced.get("types") and card.type not in advanced["types"]: return False
        if advanced.get("attributes") and (not card.is_monster() or card.attribute not in advanced["attributes"]): return False
        bounds = advanced.get("bounds", {})
        if not bulk_fusions._within(card.attack, bounds.get("atk_min"), bounds.get("atk_max")): return False
        if not bulk_fusions._within(card.defense, bounds.get("def_min"), bounds.get("def_max")): return False
        if not bulk_fusions._within(card.level, bounds.get("level_min"), bounds.get("level_max")): return False
        return self._card_matches(cid, self.search.text())
    def _state_colour(self, state):
        """The ink a row's state is drawn in, or None for an unchanged row."""
        pair = STATE_COLOURS.get((state or "").casefold())
        if pair is None:
            return None
        return QColor(pair[1] if getattr(self, "dark_mode", True) else pair[0])
    def _tint_state(self, item, state):
        """Colour a row by its state, as the Tk lists' row tags do."""
        colour = self._state_colour(state)
        if colour is not None and item is not None:
            item.setForeground(colour)
    def _card_form_widgets(self):
        """Everything the card form edits, for enabling it with a selection."""
        return [*self.fields.values(), self.description, self.notes, self.key_edit,
                self.drops, self.opponents, self.apply_button, self.revert_button, self.remove_button]
    def _clear_card_form(self):
        """No card selected: an empty form nobody can type into, so an edit
        cannot be made against no card and then dropped (CardsTab.show(None))."""
        for field in self.fields.values():
            if isinstance(field, QLineEdit):
                field.clear()
            elif isinstance(field, QSpinBox):
                field.setValue(field.minimum())
            else:
                field.setCurrentIndex(-1)
        self.card_id.setValue(self.card_id.minimum())     # "—" (setSpecialValueText)
        self.description.clear()
        self.notes.clear()
        self.key_edit.clear()
        self.drops.setChecked(False)
        self.opponents.setChecked(False)
        self.added_panel.setVisible(False)
        self.remove_button.setVisible(False)
        self.base_card_info.clear()
        self.preview_image.setPixmap(QPixmap())
        self.preview_image.clear()
        self.reference_info.setText("Select a card to edit.")
        self.extra_info.clear()
        self.description_status.clear()
        self.validation.clear()
        for widget in self._card_form_widgets():
            widget.setEnabled(False)
    def resizeEvent(self, event):
        super().resizeEvent(event)
        controls = getattr(self, "workspace_controls", {}).get("Fusions")
        if controls:
            self._set_fusion_view(0 if controls["grid"].isChecked() else 1)
            self._sync_fusion_header()
        if self.current: self._render_preview()
    def new_mod(self):
        if not self.confirm_discard(): return
        self.project = Project(self.retail)
        self.fusion_art_cache.clear()
        self.duelist_selected_slot=0
        self.project.source_dir = None
        self.current = None
        self.refresh_cards(select_id=1)
        self.current_art_card = 1
        self.refresh_art_list(select_id=1)
        for name in self.NAV[2:]: self._refresh_workspace(name)
        self.setWindowTitle("FM Editor — New mod")
        self.dirty = False
        self.statusBar().showMessage("New mod · based on retail data")
    def open_mod(self):
        if not self.confirm_discard(): return
        folder = QFileDialog.getExistingDirectory(self, "Open mod folder", str(self.mods_dir()))
        if folder: self.open_mod_path(folder)
    def open_mod_path(self, folder):
        try:
            project, messages = manifest.open_mod(self.retail, folder)
        except (ValueError, OSError) as problem:
            QMessageBox.critical(self, "Could not open mod", str(problem)); return
        self.project = project
        self.duelist_selected_slot=0
        self.current = None
        self.frame_cache.clear()
        self.fusion_art_cache.clear()
        self.dirty = False
        self.refresh_cards(select_id=1)
        self.current_art_card = 1
        self.refresh_art_list(select_id=1)
        for name in self.NAV[2:]: self._refresh_workspace(name)
        self.setWindowTitle(f"{self.project.info.name} — FM Editor")
        self.statusBar().showMessage(f"Opened {folder}" + (f" · {len(messages)} note(s)" if messages else ""))
        if messages:
            self._report("Opened with notes", "\n".join(messages))
    def save_mod(self, choose=False):
        if self.current and not self.apply_card(quiet=True): return
        if self.current_workspace == "Mod info" and not self._apply_mod_info(): return
        if self.current_workspace == "Packs" and not self._commit_packs(): return
        folder = str(self.project.source_dir or "")
        if choose or not folder:
            folder = QFileDialog.getExistingDirectory(self, "Save mod: choose an empty folder or its parent", str(self.mods_dir()))
            if not folder: return
            chosen = Path(folder)
            if chosen.is_dir() and any(chosen.iterdir()) and not (chosen / "mod.json").exists():
                if not KEY_RE.fullmatch(self.project.info.id or ""):
                    QMessageBox.critical(self, "Save mod", "Give the mod a valid ID before creating its folder.")
                    return
                folder = str(chosen / self.project.info.id)
            if (Path(folder) / "mod.json").exists() and Path(folder) != Path(self.project.source_dir or ""):
                answer = QMessageBox.question(self, "Replace mod files?",
                    f"{folder} already contains a mod. Replace its mod.json and matching files?")
                if answer != QMessageBox.StandardButton.Yes: return
        issues = validate.validate(self.project)
        errors = validate.errors(issues)
        if errors:
            answer = QMessageBox.question(self, "Mod validation",
                f"The game loader would reject {len(errors)} issue(s), including:\n\n{errors[0]}\n\nSave anyway?")
            if answer != QMessageBox.StandardButton.Yes: return
        try:
            path = manifest.save_mod(self.project, folder)
        except (ValueError, OSError) as problem:
            QMessageBox.critical(self, "Could not save mod", str(problem)); return
        self.statusBar().showMessage(f"Saved {path}")
        self.dirty = False
        self.setWindowTitle(f"{self.project.info.name} — FM Editor")
    def _mark_dirty(self):
        if not self.dirty:
            self.dirty = True
            self.setWindowTitle("* " + self.project.info.name + " — FM Editor")
    def confirm_discard(self):
        if self.current and not self.apply_card(quiet=True): return False
        if self.current_workspace == "Mod info" and not self._apply_mod_info(): return False
        if self.current_workspace == "Packs" and not self._commit_packs(): return False
        if not self.dirty: return True
        answer = QMessageBox.question(self, "Unsaved changes", "Save changes to this mod before continuing?",
            QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Save)
        if answer == QMessageBox.StandardButton.Cancel: return False
        if answer == QMessageBox.StandardButton.Save:
            self.save_mod()
            return not self.dirty
        return True
    def closeEvent(self, event):
        if self.confirm_discard():
            event.accept()
        else:
            event.ignore()


def main(game=None, mod=None):
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("FM Editor")
    try:
        window = ModernEditor(game, mod)
    except SystemExit as problem:
        return int(problem.code or 0)
    window.show()
    return app.exec()

if __name__ == "__main__":
    raise SystemExit(main())
