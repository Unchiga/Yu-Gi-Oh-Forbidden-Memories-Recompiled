"""The Packs page (a port of packs_tab.PacksTab)."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class PacksMixin:
    def _build_packs_page(self, parent):
        ui_path=UI_DIR/"packs_page.ui";source=QFile(str(ui_path))
        if not source.open(QIODevice.OpenModeFlag.ReadOnly):raise RuntimeError(f"Could not open Packs form {ui_path}: {source.errorString()}")
        loader=QUiLoader();page=loader.load(source,parent);source.close()
        if page is None:raise RuntimeError(f"Could not load Packs form {ui_path}: {loader.errorString()}")
        wrapper=QVBoxLayout(parent);wrapper.setContentsMargins(0,0,0,0);wrapper.addWidget(page)
        c={"page":page};self.workspace_forms["Packs"]=page;self.workspace_controls["Packs"]=c
        def get(cls,key):
            item=page.findChild(cls,key)
            if item is None:raise RuntimeError(f"Packs form is missing {key!r}")
            return item
        specs={"list":(QListWidget,"packsList"),"name":(QLineEdit,"packNameEdit"),"description":(QPlainTextEdit,"packDescriptionEdit"),"price":(QSpinBox,"packPriceSpin"),"count":(QSpinBox,"packCountSpin"),"stock":(QSpinBox,"packStockSpin"),"infinite":(QCheckBox,"packInfiniteStockCheck"),"identity":(QLabel,"packIdentityValue"),"problem":(QLabel,"packValidationLabel"),"image":(QLabel,"packImagePreview"),"image_scale":(QComboBox,"packImageScaleCombo"),"contents":(QTableWidget,"packContentsTable"),"total":(QLabel,"packTotalWeightLabel"),"tier":(QComboBox,"packTierCombo"),"weight":(QSpinBox,"packWeightSpin"),"count_label":(QLabel,"packDescriptionCount"),"list_title":(QLabel,"packListTitle"),"status":(QLabel,"packsStatusLabel"),"sim_count":(QSpinBox,"packSimulationCountSpin"),"sim_results":(QTableWidget,"packSimulationResultsTable")}
        for k,(cls,n) in specs.items():c[k]=get(cls,n)
        # The advanced forms are a window of their own: five forms do not fit
        # the settings column, and the page keeps the shape of the template.
        c["advanced_tabs"]=QTabWidget()
        self._build_pack_advanced(c["advanced_tabs"],c)
        c["pack_index"]=-1;c["simulation"]=None
        t=c["contents"];t.setColumnCount(6);t.setHorizontalHeaderLabels(["#","Card","Tier","Weight","Chance",""]);t.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows);t.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection);t.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers);t.verticalHeader().setVisible(False);t.horizontalHeader().setSectionResizeMode(1,QHeaderView.ResizeMode.Stretch)
        for col,width in ((0,34),(2,112),(3,80),(4,68),(5,36)):
            t.horizontalHeader().setSectionResizeMode(col,QHeaderView.ResizeMode.Fixed);t.setColumnWidth(col,width)
        # A row carries the card's own picture, and its tier and weight as
        # boxes of its own: tall enough for both.
        t.verticalHeader().setDefaultSectionSize(44)
        t.setIconSize(QSize(30,42))
        t.horizontalHeader().setMinimumSectionSize(32)
        t.setColumnWidth(1,160)
        side=art.SIZES["art"]
        c["image_scale"].addItems([f"{n}× ({side[0]*n}×{side[1]*n})" for n in (1,2,4)])
        c["pack_art"]={}
        c["filling"]=False
        c["description"].textChanged.connect(self._count_pack_description)
        # The list follows the name as it is typed: there is no Apply button
        # to make it catch up, and the pack being renamed is the one shown.
        c["name"].textChanged.connect(self._rename_pack_row)
        c["list"].currentRowChanged.connect(self._select_pack);t.itemSelectionChanged.connect(self._select_pack_content)
        actions=(("addPackButton",self._add_pack),("duplicatePackButton",self._duplicate_pack),("removePackButton",self._remove_pack),("movePackUpButton",lambda:self._move_pack(-1)),("movePackDownButton",lambda:self._move_pack(1)),("addPackCardButton",self._add_pack_card),("addFilteredPackCardsButton",self._add_filtered_pack_cards),("removePackCardsButton",self._remove_pack_cards),("setPackWeightButton",self._set_pack_weight),("setPackTierButton",self._set_pack_tier),("addPackTierButton",self._add_pack_tier),("importPackImageButton",self._import_pack_image),("exportPackImageButton",self._export_pack_image),("revertPackImageButton",self._revert_pack_image),("toggleAdvancedPackButton",self._toggle_pack_advanced),("openShopSettingsButton",self._edit_pack_shop),("simulatePackButton",self._simulate_pack),("viewPackResultsButton",self._show_pack_results))
        for name,fn in actions:get(QPushButton,name).clicked.connect(fn)
        c["image_scale"].currentIndexChanged.connect(lambda *_:self._refresh_pack_image())
        c["simulation_timer"]=QTimer(self);c["simulation_timer"].setSingleShot(True);c["simulation_timer"].timeout.connect(self._pack_simulation_step)
        c["sim_results"].hide()
        get(QToolButton,"packContentsHelpButton").clicked.connect(self._explain_pack_weights)
        # The card actions wrap rather than run off the panel.
        host=get(QWidget,"contentsActionsHost")
        flow=FlowLayout(spacing=8)
        flow.setContentsMargins(0,0,0,0)
        for name in ("addPackCardButton","addFilteredPackCardsButton"):flow.addWidget(get(QPushButton,name))
        flow.addWidget(get(QWidget,"packSetControls"))
        for name in ("addPackTierButton","removePackCardsButton"):flow.addWidget(get(QPushButton,name))
        host.setLayout(flow)
        self._style_packs_page(page, get, c)
        self._refresh_packs()
    def _style_packs_page(self, page, get, c):
        """The page's own look and proportions: the panels as cards, the
        headings, and the slack to the columns rather than the headings."""
        panels = get(QSplitter, "packsSplitter")
        page_layout = page.layout()
        for index in range(page_layout.count()):
            page_layout.setStretch(index, 1 if page_layout.itemAt(index).widget() is panels else 0)
        panels.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        for label in ("pageTitleLabel", "pageSummaryLabel", "packsStatusLabel"):
            get(QLabel, label).setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Minimum)
        get(QLabel, "pageTitleLabel").setStyleSheet("font-size:20px;font-weight:650;color:#f3f7fc")
        for title in ("packListTitle", "packSettingsTitle", "packContentsTitle",
                      "shopSettingsTitle", "simulationTitle"):
            get(QLabel, title).setStyleSheet("font-size:16px;font-weight:650;color:#f3f7fc")
        for muted in ("pageSummaryLabel", "packsStatusLabel", "packIdentityValue", "packTotalWeightLabel",
                      "shopSettingsHint", "simulationHint", "packPriceHint"):
            get(QLabel, muted).setStyleSheet("color:#9aacc4")
        get(QLabel, "packValidationLabel").setStyleSheet("color:#ff7777")
        for panel in ("packListPanel", "packSettingsPanel", "packContentsPanel",
                      "shopSettingsPanel", "simulationPanel"):
            get(QFrame, panel).setStyleSheet(
                f"QFrame#{panel} {{ background:#101b2b; border:1px solid #26374c; border-radius:10px; }}")
        get(QLabel, "packImagePreview").setStyleSheet(
            "background:#16212f;border:1px solid #26374c;border-radius:8px;color:#7f8ca0")
        # A pack reads as a card of its own: the cover, then the name over
        # its numbers.
        c["list"].setIconSize(QSize(52, 74))
        c["list"].setSpacing(4)
        for muted in ("packDescriptionCount",):
            get(QLabel, muted).setStyleSheet("color:#9aacc4")
        help_button = get(QToolButton, "packContentsHelpButton")
        help_button.setStyleSheet(
            "QToolButton { border:1px solid #30455e; border-radius:9px; background:transparent;"
            " color:#9aacc4; padding:0; min-width:18px; min-height:18px; }"
            "QToolButton:hover { color:#e5edf8; border-color:#4a6a90; }")
        help_button.setToolTip("What the weights and the chances mean")
        for name in ("simulatePackButton",):
            get(QPushButton, name).setStyleSheet(
                "background:#216cf1;border-color:#216cf1;color:white;font-weight:600;padding:7px 14px")
        scroll = get(QScrollArea, "packSettingsScroll")
        # As needed, not off: off clipped the form's right-hand side away with
        # no way to reach it when the panel was narrow.
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        # These three add up to less than the splitter has at the window's own
        # 1280: asking for more makes the panels overlap rather than scroll.
        get(QFrame, "packListPanel").setMinimumWidth(306)
        get(QFrame, "packSettingsPanel").setMinimumWidth(348)
        get(QFrame, "packContentsPanel").setMinimumWidth(418)
        for index, stretch in enumerate((3, 4, 6)):
            panels.setStretchFactor(index, stretch)
        panels.setSizes([340, 470, 740])
        # The actions under the contents wrap (FlowLayout): on one line they
        # were wider than the window, and the panel beside them was drawn over.
        for key, width in (("tier", 130), ("weight", 110)):
            c[key].setMinimumWidth(0)
            c[key].setMaximumWidth(width)
        for name in ("addPackCardButton", "addFilteredPackCardsButton", "setPackTierButton",
                     "addPackTierButton", "setPackWeightButton", "removePackCardsButton"):
            button = get(QPushButton, name)
            button.setMinimumWidth(0)
            button.setStyleSheet("padding:7px 9px")
        # The four beside "Add pack" are their glyph and nothing else.
        for name in ("duplicatePackButton", "removePackButton", "movePackUpButton", "movePackDownButton"):
            square = get(QPushButton, name)
            square.setFixedWidth(38)
            square.setStyleSheet("padding:7px 0")
        get(QPushButton, "addPackButton").setMinimumWidth(0)
        get(QPushButton, "toggleAdvancedPackButton").setStyleSheet(
            "padding:5px 9px;color:#9aacc4;background:transparent;border:1px solid #2a3d55")
    def _pack_entry(self):
        c=self.workspace_controls.get("Packs",{});i=c.get("pack_index",-1)
        return self.project.packs[i] if 0<=i<len(self.project.packs) and isinstance(self.project.packs[i],dict) else None
    def _pack_read(self):
        e=self._pack_entry()
        return packmath.read_pack(e,validate.pack_resolver(self.project),self.project.info.id,self.workspace_controls["Packs"].get("pack_index",0)) if e else (None,[])
    def _refresh_packs(self):
        c=self.workspace_controls.get("Packs")
        if not c:return
        listing=c["list"];listing.blockSignals(True);listing.clear();taken=set()
        c["list_title"].setText(f"Pack list ({len(self.project.packs)})")
        for i,e in enumerate(self.project.packs):
            pack,notes=packmath.read_pack(e,validate.pack_resolver(self.project),self.project.info.id,i,taken)
            if pack:taken.add(pack.id)
            stock="∞" if pack and pack.stock<0 else str(pack.stock) if pack else "—"
            listing.addItem(f"{i+1}   {e.get('name','Pack')}\n      {pack.price if pack else '—'}      "
                            f"{pack.count if pack else '—'} cards      Stock: {stock}")
            listing.item(i).setData(Qt.ItemDataRole.UserRole,i)
            image_data=self._pack_image_bytes(e)
            if image_data:
                thumb=QImage.fromData(image_data)
                if not thumb.isNull():listing.item(i).setIcon(QIcon(QPixmap.fromImage(thumb).scaled(QSize(52,74),Qt.AspectRatioMode.KeepAspectRatio,Qt.TransformationMode.SmoothTransformation)))
            if not pack:listing.item(i).setForeground(QColor("#ff7777"))
            elif notes:listing.item(i).setForeground(QColor("#f2c04c"))
        c["pack_index"]=min(max(0,c.get("pack_index",0)),len(self.project.packs)-1) if self.project.packs else -1
        listing.setCurrentRow(c["pack_index"]);listing.blockSignals(False)
        external=self.project.packs_file is not None
        # Before the form is filled: what it has to say about the picture is
        # the later word, and must not be wiped by this line.
        c["status"].setText(f'"packs" points to {self.project.packs_file}; that external file is read-only here.' if external else "")
        self._fill_pack()
        for name in ("addPackButton","duplicatePackButton","removePackButton","movePackUpButton","movePackDownButton","addPackCardButton","addFilteredPackCardsButton","removePackCardsButton","setPackWeightButton","setPackTierButton","addPackTierButton","importPackImageButton"):
            b=c["page"].findChild(QPushButton,name)
            if b:b.setEnabled(not external)
        for key in ("name","description","price","count","stock","infinite","contents","tier","weight"):
            c[key].setEnabled(not external)
    def _fill_pack(self):
        c=self.workspace_controls["Packs"];e=self._pack_entry();t=c["contents"]
        c["filling"]=True
        try:self._fill_pack_form(c,e,t)
        finally:c["filling"]=False
    def _fill_pack_form(self,c,e,t):
        t.setRowCount(0)
        if not e:
            c["name"].clear();c["description"].clear();c["price"].setValue(packmath.DEFAULT_PRICE);c["count"].setValue(packmath.DEFAULT_COUNT);c["stock"].setValue(0);c["infinite"].setChecked(True);c["identity"].setText("No pack: Add pack makes one");c["problem"].clear();c["total"].setText("Total weight: 0");c["image"].setPixmap(QPixmap());c["image"].setText("No image")
            self._fill_pack_advanced(None,None);c["adv_baseline"]=self._pack_advanced_state();return
        c["name"].setText(str(e.get("name","")));c["description"].setPlainText(str(e.get("description","")))
        price=e.get("price",packmath.DEFAULT_PRICE);price=e.get("cost",{}).get("starchips",price) if isinstance(e.get("cost"),dict) else price;c["price"].setValue(price if isinstance(price,int) else packmath.DEFAULT_PRICE);c["count"].setValue(e.get("count",packmath.default_count(e)))
        stock=e.get("stock",-1);c["infinite"].setChecked(stock in (-1,None));c["stock"].setValue(0 if stock in (-1,None) else stock);c["identity"].setText(f"{self.project.info.id}:{packmath.pack_id(e)}")
        pack,notes=self._pack_read();c["problem"].setText(next((m for level,m in notes if level=="error"),notes[0][1] if notes else ""));chances=packmath.card_chances(pack) if pack else {};resolve=validate.pack_resolver(self.project);total=0
        tiers=packmath.tiers_of(e)
        held=c["tier"].currentText()
        c["tier"].clear()
        for name,_ in tiers:c["tier"].addItem(name,name)
        if held:c["tier"].setCurrentText(held)
        if c["tier"].currentIndex()<0 and tiers:c["tier"].setCurrentIndex(0)
        for ti,(tier_name,tier) in enumerate(tiers):
            pool=packmath.pool_items(tier.get("cards",[]));total+=sum(w for _,w in pool)
            for k,(ref,weight) in enumerate(pool):
                cid=resolve(ref);card=self.project.cards.get(cid);chance=chances.get((ti,cid),0) if pack and cid>0 else 0;r=t.rowCount();t.insertRow(r)
                for col,val in enumerate((r+1,card.name if card else str(ref),"","",f"{chance*100:.2f}%")):
                    cell=TableItem(str(val))
                    if col==0:cell.setData(Qt.ItemDataRole.UserRole,(ti,k,cid))
                    if col==1:
                        cell.setToolTip(f"{cid:03d}  {card.name}" if card else f"{ref}: no card the editor knows")
                        picture=self._pack_card_icon(cid) if card else None
                        if picture is not None:cell.setIcon(picture)
                        if not card:self._tint_state(cell,"removed")
                    t.setItem(r,col,cell)
                # The tier and the weight are the row's own boxes, as a pack
                # is read column by column rather than card by card.
                t.setCellWidget(r,2,self._pack_row_tier_box([name for name,_ in tiers],ti,r))
                t.setCellWidget(r,3,self._pack_row_spin(weight,0,packmath.WEIGHT_TOTAL_MAX,
                                                        lambda value,row=r:self._set_pack_row_weight(row,value)))
                t.setCellWidget(r,5,self._pack_row_remove(r))
        c["total"].setText(f"Total weight: {total:,}")
        self._fill_pack_advanced(e,pack)
        c["adv_baseline"]=self._pack_advanced_state()
        self._refresh_pack_image()
    def _rename_pack_row(self, text):
        """The chosen row's first line, as the name is typed. The rest of the
        row (its price, cards and stock) waits for the pack to be stored."""
        c = self.workspace_controls.get("Packs")
        if not c or c.get("filling"):
            return
        row = c["pack_index"]
        item = c["list"].item(row) if 0 <= row < c["list"].count() else None
        if item is None:
            return
        lines = item.text().split("\n")
        lines[0] = f"{row + 1}   {text.strip() or '(unnamed)'}"
        item.setText("\n".join(lines))

    def _pack_image_bytes(self, entry):
        """A pack's own picture: the mod's files, or the folder it was opened
        from (PacksTab.image_bytes). None where it names none."""
        path = entry.get("image") if isinstance(entry, dict) else None
        if not isinstance(path, str) or not path:
            return None
        blob = self.project.files.get(path)
        if blob is None and self.project.source_dir:
            try:
                blob = (Path(self.project.source_dir) / path).read_bytes()
            except OSError:
                blob = None
        return blob

    def _refresh_pack_image(self):
        c=self.workspace_controls.get("Packs")
        if not c:return
        e=self._pack_entry()
        def nothing():
            c["image"].setPixmap(QPixmap());c["image"].setText("No image");self._set_pack_image_buttons(False)
        if not e:nothing();return
        path=e.get("image");blob=self._pack_image_bytes(e)
        zoom=max(1,c["image_scale"].currentIndex()*2)
        picture,own=None,False
        if blob:
            try:picture,own=pngio.decode(blob),True
            except pngio.PngError as problem:c["status"].setText(f"{path}: {problem}")
        elif isinstance(path,str):
            c["status"].setText(f"{path} is not in the mod folder: the game shows the cover.")
        if picture is None:
            # No picture of its own: the cover card's art stands in, as the
            # game draws it (PacksTab.show_picture).
            cover=e.get("cover") if isinstance(e.get("cover"),(int,str)) else None
            cid=self.project.resolve(cover) if cover is not None else 0
            if cid and self.files is not None:
                try:picture=art.in_game(self.project,self.files.wa,cid,"art",1)
                except (OSError,ValueError,pngio.PngError):picture=None
        name=c["name"].text().strip() or str(e.get("name",""))
        try:drawn=self._pack_picture(picture,name,zoom)
        except (OSError,ValueError,pngio.PngError):drawn=None
        if drawn is not None and not drawn.isNull():
            c["image"].setText("");c["image"].setPixmap(QPixmap.fromImage(drawn))
            self._set_pack_image_buttons(own);return
        nothing()
    def _pack_plate_inks(self, name):
        """The game's name plate for a pack (PacksTab.plate_inks); None where
        the system has no serif face to set it in."""
        cache = self.workspace_controls["Packs"]
        if "serif" not in cache:
            path = packmath.serif_path()
            try:
                cache["serif"] = ttf.Font(path) if path else None
            except ttf.FontError:
                cache["serif"] = None
        if cache["serif"] is None:
            return None
        return packmath.name_plate_inks(name, cache["serif"])
    def _pack_picture(self, picture, name, zoom):
        """The picture over the card's gold with the name plate under it, as
        the big card shows a pack (PacksTab.card_picture)."""
        width, height = art.SIZES["art"][0] * zoom, (art.SIZES["art"][1] + 2 + 14) * zoom
        out = bytearray(bytes(art.GOLD) + b"\xff") * (width * height)
        if picture is not None:
            shaped = pngio.resample(picture, art.SIZES["art"][0] * zoom, art.SIZES["art"][1] * zoom,
                                    pngio.middle(picture, *art.SIZES["art"]))
            for y in range(shaped.height):
                start = y * width * 4
                out[start:start + shaped.width * 4] = shaped.rgba[y * shaped.width * 4:(y + 1) * shaped.width * 4]
        inks = self._pack_plate_inks(name)
        if inks is not None:
            plate = pngio.scale_nearest(art.plate_image(inks, background=art.GOLD), zoom)
            top, left = (art.SIZES["art"][1] + 2) * zoom, 3 * zoom
            for y in range(plate.height):
                start = ((top + y) * width + left) * 4
                out[start:start + plate.width * 4] = plate.rgba[y * plate.width * 4:(y + 1) * plate.width * 4]
        return _qimage(width, height, bytes(out))
    def _set_pack_image_buttons(self,own):
        """Export needs a picture to write, Revert one to take away."""
        e=self._pack_entry();page=self.workspace_controls["Packs"]["page"]
        for name,on in (("exportPackImageButton",own),
                        ("revertPackImageButton",bool(e) and isinstance(e.get("image"),str))):
            button=page.findChild(QPushButton,name)
            if button:button.setEnabled(bool(on))
    def _build_pack_advanced(self, tabs, c):
        """The five forms behind "Advanced…": what a pack carries that the
        plain fields have no room for. Built here rather than in the form, so
        the page itself stays the shape of the template."""
        v = {}
        c["adv"] = v

        def add_tab(page, title):
            """Each page scrolls: a hint at its foot is never cut away."""
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setFrameShape(QFrame.Shape.NoFrame)
            scroll.setWidget(page)
            tabs.addTab(scroll, title)

        def row(grid, r, label, widget, hint=None):
            grid.addWidget(QLabel(label), r, 0, Qt.AlignmentFlag.AlignLeft)
            grid.addWidget(widget, r, 1)
            if hint:
                note = QLabel(hint)
                note.setWordWrap(True)
                note.setStyleSheet("color:#9aacc4")
                grid.addWidget(note, r, 2)
            return widget

        def line(width=170):
            edit = QLineEdit()
            edit.setMaximumWidth(width)
            return edit

        def pick(values, width=150):
            box = QComboBox()
            box.addItems(list(values))
            box.setMaximumWidth(width)
            return box

        # Tiers ---------------------------------------------------------------
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(6, 6, 6, 6)
        table = QTableWidget(0, 8)
        table.setObjectName("packTierTable")      # UNSORTED_TABLES names it
        table.setHorizontalHeaderLabels(["Tier", "Odds", "Share", "Label", "Colour", "Sound", "Reveal", "Cards"])
        table.verticalHeader().setVisible(False)
        table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        table.doubleClicked.connect(self._edit_pack_tier)
        v["tiers"] = table
        layout.addWidget(table)
        actions = QHBoxLayout()
        for text, call in (("Add tier", self._add_pack_tier), ("Edit…", self._edit_pack_tier),
                           ("Remove", self._remove_pack_tier), ("Up", lambda: self._move_pack_tier(-1)),
                           ("Down", lambda: self._move_pack_tier(1))):
            button = QPushButton(text)
            button.setStyleSheet("padding:6px 10px")
            button.clicked.connect(call)
            actions.addWidget(button)
        note = QLabel("written in order of rarity, commonest first")
        note.setStyleSheet("color:#9aacc4")
        actions.addWidget(note)
        actions.addStretch(1)
        layout.addLayout(actions)
        add_tab(page, "Tiers")

        # Slots ---------------------------------------------------------------
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(6, 6, 6, 6)
        v["use_slots"] = QCheckBox("A rule for each slot (else every slot is dealt by the tiers' odds)")
        v["use_slots"].clicked.connect(self._toggle_pack_slots)
        layout.addWidget(v["use_slots"])
        slots = QTableWidget(0, 2)
        slots.setObjectName("packSlotTable")      # likewise: slot 1 is slot 1
        slots.setHorizontalHeaderLabels(["#", "Rule"])
        slots.verticalHeader().setVisible(False)
        slots.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        slots.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        slots.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        slots.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        slots.setColumnWidth(0, 40)
        slots.doubleClicked.connect(self._edit_pack_slot)
        v["slots"] = slots
        layout.addWidget(slots)
        actions = QHBoxLayout()
        for text, call in (("Add slot", self._add_pack_slot), ("Edit…", self._edit_pack_slot),
                           ("Remove", self._remove_pack_slot), ("Up", lambda: self._move_pack_slot(-1)),
                           ("Down", lambda: self._move_pack_slot(1))):
            button = QPushButton(text)
            button.setStyleSheet("padding:6px 10px")
            button.clicked.connect(call)
            actions.addWidget(button)
        actions.addStretch(1)
        layout.addLayout(actions)
        add_tab(page, "Slots")

        # Dealing -------------------------------------------------------------
        page = QWidget()
        grid = QGridLayout(page)
        grid.setContentsMargins(6, 6, 6, 6)
        grid.setColumnStretch(2, 1)
        rows = (("guarantee", "Guarantee", "tier=n: at least n of that tier or rarer in every pack"),
                ("pity", "Pity", "tier=n: the n-th pack in a row without it has one"),
                ("max_copies", "Max copies", "not dealt once the player holds this many (chest and deck)"),
                ("stock", "Stock", "purchases a save may make; empty for no limit"),
                ("cost_cards", "Cost in cards", "card=copies taken from the chest: names or numbers"),
                ("order", "Order", "place in the list; empty for as declared"),
                ("shop", "Shops", "shop ids, comma between; empty for every shop"),
                ("cover", "Cover", "the card whose art stands in without a picture"))
        for r, (key, label, hint) in enumerate(rows):
            v[key] = row(grid, r, label, line(), hint)
        r = len(rows)
        v["duplicates"] = row(grid, r, "Duplicates", pick(("allow", "unique_in_pack")))
        v["reveal"] = row(grid, r + 1, "Reveal", pick(packmath.REVEALS))
        v["include_added"] = QCheckBox("Include cards mods add")
        grid.addWidget(v["include_added"], r + 2, 1)
        v["when_nothing_left"] = row(
            grid, r + 3, "All owned", pick((PACK_SHOPS_RULE,) + packmath.NOTHING_LEFT),
            "with Max copies, when the player holds that many of every card: refuse (BUY is refused, "
            "ALL OWNED) or sell anyway (empty slots); (shop's): Shop settings' rule")
        grid.setRowStretch(r + 4, 1)
        add_tab(page, "Dealing")

        # Unlock --------------------------------------------------------------
        page = QWidget()
        grid = QGridLayout(page)
        grid.setContentsMargins(6, 6, 6, 6)
        grid.setColumnStretch(2, 1)
        beat = QComboBox()
        beat.setEditable(True)
        beat.addItems([""] + list(DUELIST_NAMES[1:]))
        beat.setMaximumWidth(220)
        v["beat"] = row(grid, 0, "Beat", beat)
        for r, (key, label, hint) in enumerate((("wins", "Wins", "against Beat, or in all without it"),
                                                ("story", "Story flag", "a campaign story flag: 0x6E0 + n is the "
                                                                        "n-th duelist beaten in the campaign"),
                                                ("copies", "Copies", "of the card below (1 by default)"),
                                                ("starchips_spent", "Starchips spent", "on packs, by this save"),
                                                ("packs_opened", "Packs opened", "in all, by this save"),
                                                ("opened", "Opened", "pack=n: that pack opened n times")), start=1):
            v[key] = row(grid, r, label, line(), hint)
        v["unlock_card"] = row(grid, 7, "Card", self._card_combo(page))
        v["locked"] = row(grid, 8, "While locked", pick(("hidden", "shown")))
        grid.setRowStretch(9, 1)
        add_tab(page, "Unlock")

        # Password and sounds -------------------------------------------------
        page = QWidget()
        grid = QGridLayout(page)
        grid.setContentsMargins(6, 6, 6, 6)
        grid.setColumnStretch(2, 1)
        v["password"] = row(grid, 0, "Password", line(120))
        v["password_note"] = QLabel()
        v["password_note"].setWordWrap(True)
        v["password_note"].setStyleSheet("color:#9aacc4")
        grid.addWidget(v["password_note"], 0, 2)
        v["once"] = QCheckBox("Once a save")
        grid.addWidget(v["once"], 1, 1)
        v["listed"] = row(grid, 2, "In the list", pick(PACK_LISTED, 120))
        sounds = QLabel("Sound ids (empty: the Password screen's)")
        sounds.setStyleSheet("color:#9aacc4")
        grid.addWidget(sounds, 3, 0, 1, 3)
        strip = QHBoxLayout()
        for key in packmath.SOUND_KEYS:
            strip.addWidget(QLabel(f"{key} ({packmath.DEFAULT_SOUNDS[key]})"))
            v["sound_" + key] = line(64)
            strip.addWidget(v["sound_" + key])
        strip.addStretch(1)
        grid.addLayout(strip, 4, 0, 1, 3)
        grid.setRowStretch(5, 1)
        add_tab(page, "Password and sounds")
    def _fill_pack_advanced(self, entry, pack):
        """The five forms from the pack (PacksTab._fill's advanced half)."""
        c = self.workspace_controls["Packs"]
        v = c.get("adv")
        if not v:
            return
        v["tiers"].setRowCount(0)
        v["slots"].setRowCount(0)
        if entry is None:
            return
        resolve = validate.pack_resolver(self.project)
        total = sum(t.odds for t in pack.tiers) if pack else 0
        for name, tier in packmath.tiers_of(entry):
            odds = tier.get("odds", 1)
            r = v["tiers"].rowCount()
            v["tiers"].insertRow(r)
            share = f"{odds / total * 100:.1f}%" if total and isinstance(odds, int) else ""
            for col, value in enumerate((name, odds, share, tier.get("label", ""), tier.get("color", ""),
                                         tier.get("sound", ""), tier.get("reveal", ""),
                                         len(packmath.pool_items(tier.get("cards", []))))):
                v["tiers"].setItem(r, col, TableItem(str(value)))
        slots = entry.get("slots")
        v["use_slots"].setChecked(isinstance(slots, list))
        for s, rule in enumerate(slots if isinstance(slots, list) else []):
            r = v["slots"].rowCount()
            v["slots"].insertRow(r)
            v["slots"].setItem(r, 0, TableItem(str(s + 1)))
            v["slots"].setItem(r, 1, TableItem(json.dumps(rule, ensure_ascii=False)))
        v["guarantee"].setText(_pairs_text(entry.get("guarantee")))
        v["pity"].setText(_pairs_text(entry.get("pity")))
        v["max_copies"].setText(str(entry.get("max_copies", "")))
        v["stock"].setText(str(entry.get("stock", "")))
        cost = entry.get("cost") if isinstance(entry.get("cost"), dict) else {}
        v["cost_cards"].setText(_pairs_text(cost.get("cards")))
        v["order"].setText(str(entry.get("order", "")))
        shop = entry.get("shop")
        v["shop"].setText(", ".join(shop) if isinstance(shop, list) else shop if isinstance(shop, str) else "")
        v["cover"].setText(str(entry.get("cover", "")))
        v["duplicates"].setCurrentText(entry.get("duplicates", "allow"))
        v["reveal"].setCurrentText(entry.get("reveal", "flip"))
        v["include_added"].setChecked(packmath.json_bool(entry.get("include_added_cards"), True))
        rule = entry.get("when_nothing_left", PACK_SHOPS_RULE)
        v["when_nothing_left"].setCurrentText(rule if isinstance(rule, str) else json.dumps(rule))
        unlock = entry.get("unlock") if isinstance(entry.get("unlock"), dict) else {}
        v["beat"].setCurrentText(str(unlock.get("beat", "")))
        for key in ("wins", "story", "copies", "starchips_spent", "packs_opened"):
            v[key].setText(str(unlock.get(key, "")))
        v["opened"].setText(_pairs_text(unlock.get("opened")))
        card = resolve(unlock["card"]) if "card" in unlock else 0
        if card > 0:
            self._set_combo_card(v["unlock_card"], card)
        else:
            v["unlock_card"].setCurrentText(str(unlock["card"]) if "card" in unlock else "")
        v["locked"].setCurrentText(entry.get("locked", "hidden"))
        password = packmath.password_bits(entry.get("password")) if "password" in entry else None
        v["password"].setText(f"{password:08X}" if password is not None else str(entry.get("password", "")))
        v["once"].setChecked(packmath.json_bool(entry.get("once"), False))
        listed = entry.get("listed")
        v["listed"].setCurrentText(PACK_LISTED[0] if listed is None else
                                   PACK_LISTED[1] if listed else PACK_LISTED[2])
        sounds = entry.get("sounds") if isinstance(entry.get("sounds"), dict) else {}
        for key in packmath.SOUND_KEYS:
            v["sound_" + key].setText(str(sounds.get(key, "")))
        v["password_note"].setText(self._pack_password_clash(password))
    def _pack_password_clash(self, password) -> str:
        """What a pack's password means, and whether a card has it already
        (PacksTab.password_clash)."""
        if password is None:
            return "A pack with a password is sold by it, and is not in the list unless it says."
        for cid in sorted(self.project.cards):
            text = self.project.password(cid)
            if text and text.isdigit() and int(text, 16) == password:
                return f"{self.project.card_label(cid)} has this password: the card comes first."
        return "Typed on the Password screen, it sells this pack."
    def _pack_advanced_state(self) -> dict:
        """What the advanced forms say, for the baseline a commit compares
        against: a field still showing what was written leaves its key alone."""
        v = self.workspace_controls["Packs"].get("adv")
        if not v:
            return {}
        state = {}
        for key, widget in v.items():
            if isinstance(widget, QLineEdit):
                state[key] = widget.text()
            elif isinstance(widget, QComboBox):
                state[key] = widget.currentText()
            elif isinstance(widget, QCheckBox):
                state[key] = widget.isChecked()
        return state
    def _store_pack_advanced(self, entry, new):
        """The advanced forms into the pack (PacksTab.store's advanced half):
        only what the form says differently, so a key the mod wrote its own
        way is left as written. ValueError, and nothing stored, for a field
        the game could not read."""
        c = self.workspace_controls["Packs"]
        v = c.get("adv")
        if not v:
            return
        now, base = self._pack_advanced_state(), c.get("adv_baseline", {})

        def changed(*keys):
            return any(now.get(key) != base.get(key) for key in keys)

        def put(key, value, container=new):
            if value in (None, "", {}, []):
                container.pop(key, None)
            else:
                container[key] = value

        if changed("guarantee"):
            put("guarantee", _parse_pairs(v["guarantee"].text(), "Guarantee"))
        if changed("pity"):
            put("pity", _parse_pairs(v["pity"].text(), "Pity"))
        if changed("max_copies"):
            put("max_copies", _whole(v["max_copies"].text(), "Max copies", 1, 250))
        if changed("stock"):
            put("stock", _whole(v["stock"].text(), "Stock", 1, 999999))
        if changed("order"):
            put("order", _whole(v["order"].text(), "Order", -1000000, 1000000))
        if changed("cost_cards"):
            cost = new.get("cost") if isinstance(new.get("cost"), dict) else {}
            put("cards", _parse_pairs(v["cost_cards"].text(), "Cost in cards"), cost)
            put("cost", cost)
        if changed("shop"):
            shops = [s.strip() for s in v["shop"].text().split(",") if s.strip()]
            put("shop", shops[0] if len(shops) == 1 and isinstance(entry.get("shop"), str) else shops)
        if changed("cover"):
            cover = v["cover"].text().strip()
            if cover:
                cid = self.project.resolve(cover) or (int(cover.split(" ", 1)[0])
                                                      if cover.split(" ", 1)[0].isdigit() else 0)
                put("cover", self.project.ref(cid) if cid in self.project.cards else cover)
            else:
                new.pop("cover", None)
        if changed("duplicates"):
            new["duplicates"] = v["duplicates"].currentText() or "allow"
        if changed("reveal"):
            new["reveal"] = v["reveal"].currentText() or "flip"
        if changed("include_added"):
            new["include_added_cards"] = bool(v["include_added"].isChecked())
        if changed("when_nothing_left"):
            rule = v["when_nothing_left"].currentText()
            put("when_nothing_left", rule if rule in packmath.NOTHING_LEFT else None)
        unlock_keys = ("beat", "wins", "story", "copies", "starchips_spent", "packs_opened", "opened", "unlock_card")
        if changed(*unlock_keys):
            unlock = copy.deepcopy(entry.get("unlock")) if isinstance(entry.get("unlock"), dict) else {}

            def as_written(key, text, container):
                if key in container and str(container[key]) == text:
                    return container[key]
                return text

            if changed("beat"):
                put("beat", as_written("beat", v["beat"].currentText().strip(), unlock), unlock)
            for key, low, high in (("wins", 0, 65535), ("story", 0, 0xFFFF), ("copies", 0, 250),
                                   ("starchips_spent", 0, 999999999), ("packs_opened", 0, 999999999)):
                if not changed(key):
                    continue
                text = v[key].text().strip()
                number = int(text, 16) if text.lower().startswith("0x") and key == "story" else None
                put(key, number if number is not None else
                    _whole(text, key.replace("_", " ").capitalize(), low, high), unlock)
            if changed("opened"):
                put("opened", _parse_pairs(v["opened"].text(), "Opened"), unlock)
            if changed("unlock_card"):
                card = self._combo_card_id(v["unlock_card"])
                typed = v["unlock_card"].currentText().strip()
                if card and "card" in unlock and self.project.resolve(unlock["card"]) == card:
                    put("card", unlock["card"], unlock)
                else:
                    put("card", self.project.ref(card) if card else typed, unlock)
            put("unlock", unlock)
        if changed("locked"):
            new["locked"] = v["locked"].currentText() or "hidden"
        if changed("password"):
            password = v["password"].text().strip()
            if password and (not password.isdigit() or len(password) > 8):
                raise ValueError("a password is up to 8 digits")
            if password and "password" in entry and \
                    packmath.password_bits(entry["password"]) == int(password.zfill(8), 16):
                put("password", entry["password"])
            else:
                put("password", password.zfill(8) if password else "")
        if changed("once"):
            new["once"] = bool(v["once"].isChecked())
        if changed("listed"):
            listed = v["listed"].currentText()
            if listed == PACK_LISTED[0]:
                new.pop("listed", None)
            else:
                new["listed"] = listed == PACK_LISTED[1]
        sound_keys = ["sound_" + key for key in packmath.SOUND_KEYS]
        if changed(*sound_keys):
            sounds = copy.deepcopy(entry.get("sounds")) if isinstance(entry.get("sounds"), dict) else {}
            for key in packmath.SOUND_KEYS:
                if changed("sound_" + key):
                    put(key, _whole(v["sound_" + key].text(), f"Sound {key}", 0, 0xFFFF), sounds)
            put("sounds", sounds)
    def _pack_form_dialog(self, title, build, ok):
        """A small form that stays open while what it says is refused: `build`
        fills it, `ok` returns a complaint or None (the Tk FormDialog)."""
        dialog = QDialog(self)
        dialog.setWindowTitle(title)
        layout = QVBoxLayout(dialog)
        body = QGridLayout()
        layout.addLayout(body)
        build(body)
        problem = QLabel()
        problem.setWordWrap(True)
        problem.setStyleSheet("color:#ff8f87")
        layout.addWidget(problem)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        layout.addWidget(buttons)
        buttons.rejected.connect(dialog.reject)

        def accept():
            said = ok()
            if said:
                problem.setText(said)
            else:
                dialog.accept()

        buttons.accepted.connect(accept)
        return dialog.exec() == QDialog.DialogCode.Accepted
    def _pack_tier_dialog(self, title, name, tier, on_ok):
        fields = {}

        def build(body):
            rows = (("name", "Name", name), ("odds", "Odds", tier.get("odds", 1)),
                    ("label", "Label", tier.get("label", "")), ("color", "Colour", tier.get("color", "")),
                    ("sound", "Sound", tier.get("sound", "")))
            for r, (key, label, value) in enumerate(rows):
                body.addWidget(QLabel(label), r, 0)
                fields[key] = QLineEdit(str(value))
                fields[key].setMaximumWidth(180)
                body.addWidget(fields[key], r, 1)
            body.addWidget(QLabel("Reveal"), 5, 0)
            fields["reveal"] = QComboBox()
            fields["reveal"].addItems([""] + list(packmath.REVEALS))
            fields["reveal"].setCurrentText(tier.get("reveal", ""))
            body.addWidget(fields["reveal"], 5, 1)
            note = QLabel("Odds: its weight when a slot deals by the tiers' odds (0: only slots and guarantees "
                          "reach it). Label: what a card of it says when it turns over (\"ULTRA RARE!\"). "
                          "Colour: the game's text colour, 0-15. Sound: a sound effect id of the game's.")
            note.setWordWrap(True)
            note.setStyleSheet("color:#9aacc4")
            body.addWidget(note, 6, 0, 1, 2)

        def ok():
            new_name = fields["name"].text().strip()
            if not packmath.KEY_RE.match(new_name):
                return "a tier's name is 1-63 letters, digits, '_' or '-'"
            try:
                odds = _whole(fields["odds"].text(), "Odds", 0, packmath.WEIGHT_TOTAL_MAX, 1)
                color = _whole(fields["color"].text(), "Colour", 0, 15)
                sound = _whole(fields["sound"].text(), "Sound", 0, 0xFFFF)
            except ValueError as problem:
                return str(problem)
            return on_ok(new_name, {"odds": odds, "label": fields["label"].text().strip(),
                                    "color": color, "sound": sound,
                                    "reveal": fields["reveal"].currentText()})

        self._pack_form_dialog(title, build, ok)
    @staticmethod
    def _write_pack_tier(tier: dict, values: dict):
        for key, value in values.items():
            if value in (None, "") or (key == "odds" and value == 1):
                tier.pop(key, None)
            else:
                tier[key] = value
    def _selected_pack_tier(self):
        table = self.workspace_controls["Packs"]["adv"]["tiers"]
        row = table.currentRow()
        return row if row >= 0 else None
    def _add_pack_tier(self):
        entry = self._pack_entry()
        if entry is None or not self._commit_packs():
            return

        def done(name, values):
            if name in dict(packmath.tiers_of(entry)):
                return f"the pack has a tier \"{name}\""
            packmath.ensure_tiers(entry)
            tier = {"cards": []}
            self._write_pack_tier(tier, values)
            entry["tiers"][name] = tier
            self._mark_dirty()
            self._refresh_packs()
            return None

        self._pack_tier_dialog("Add tier", "rare", {}, done)
    def _edit_pack_tier(self):
        entry, index = self._pack_entry(), self._selected_pack_tier()
        if entry is None or index is None or not self._commit_packs():
            return
        name, tier = packmath.tiers_of(entry)[index]

        def done(new_name, values):
            if new_name != name and new_name in dict(packmath.tiers_of(entry)):
                return f"the pack has a tier \"{new_name}\""
            packmath.ensure_tiers(entry)
            self._write_pack_tier(entry["tiers"][name], values)
            if new_name != name:
                entry["tiers"] = {new_name if k == name else k: v for k, v in entry["tiers"].items()}
                self._rename_pack_tier(entry, name, new_name)
            self._mark_dirty()
            self._refresh_packs()
            return None

        self._pack_tier_dialog("Tier", name, tier, done)
    @staticmethod
    def _rename_pack_tier(entry, old, new):
        """A tier renamed: the slots, guarantee and pity that name it too."""
        for key in ("guarantee", "pity"):
            if isinstance(entry.get(key), dict) and old in entry[key]:
                entry[key] = {new if k == old else k: v for k, v in entry[key].items()}
        for s, slot in enumerate(entry.get("slots") or []):
            if slot == old:
                entry["slots"][s] = new
            elif isinstance(slot, dict) and isinstance(slot.get("tiers"), dict) and old in slot["tiers"]:
                slot["tiers"] = {new if k == old else k: v for k, v in slot["tiers"].items()}
    def _remove_pack_tier(self):
        entry, index = self._pack_entry(), self._selected_pack_tier()
        if entry is None or index is None or not isinstance(entry.get("tiers"), dict):
            return
        name = packmath.tiers_of(entry)[index][0]
        if QMessageBox.question(self, "Remove tier", f"Remove the tier {name} and its cards?",
                                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                                QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
            return
        del entry["tiers"][name]
        self._mark_dirty()
        self._refresh_packs()
    def _move_pack_tier(self, step):
        entry, index = self._pack_entry(), self._selected_pack_tier()
        if entry is None or index is None or not isinstance(entry.get("tiers"), dict):
            return
        items = list(entry["tiers"].items())
        other = index + step
        if not 0 <= other < len(items):
            return
        items[index], items[other] = items[other], items[index]
        entry["tiers"] = dict(items)
        self._mark_dirty()
        self._refresh_packs()
        self.workspace_controls["Packs"]["adv"]["tiers"].selectRow(other)
    def _toggle_pack_slots(self):
        """Slots of its own, or every slot dealt by the tiers' odds."""
        entry = self._pack_entry()
        c = self.workspace_controls["Packs"]
        if entry is None:
            return
        if c["adv"]["use_slots"].isChecked():
            first = packmath.tiers_of(entry)[0][0]
            count = entry.get("count", packmath.DEFAULT_COUNT)
            entry["slots"] = [first] * (count if isinstance(count, int) and count > 0 else packmath.DEFAULT_COUNT)
            entry.pop("count", None)
        else:
            slots = entry.pop("slots", None)
            if isinstance(slots, list) and len(slots) != packmath.DEFAULT_COUNT:
                entry["count"] = len(slots)
        self._mark_dirty()
        self._refresh_packs()
    def _pack_slot_dialog(self, title, rule, on_ok):
        entry = self._pack_entry()
        tiers = [name for name, _ in packmath.tiers_of(entry)]
        fields = {}

        def build(body):
            kind = ("card" if isinstance(rule, dict) and "card" in rule else
                    "pool" if isinstance(rule, dict) and "cards" in rule else
                    "mix" if isinstance(rule, dict) else "tier")
            fields["kind"] = {}
            group = QButtonGroup(self)
            for r, (key, label) in enumerate((("tier", "A tier"), ("mix", "Tiers by weight"),
                                              ("pool", "Its own cards"), ("card", "Always a card"))):
                button = QRadioButton(label)
                button.setChecked(key == kind)
                group.addButton(button)
                fields["kind"][key] = button
                body.addWidget(button, r, 0)
            fields["group"] = group
            fields["tier"] = QComboBox()
            fields["tier"].addItems(tiers)
            fields["tier"].setCurrentText(rule if isinstance(rule, str) and rule in tiers else
                                          tiers[0] if tiers else "")
            body.addWidget(fields["tier"], 0, 1)
            fields["mix"] = QLineEdit(_pairs_text(rule.get("tiers")) if kind == "mix" else "")
            body.addWidget(fields["mix"], 1, 1)
            pool = packmath.pool_items(rule.get("cards")) if kind == "pool" else []
            fields["pool"] = QLineEdit(", ".join(f"{r}={w}" for r, w in pool))
            body.addWidget(fields["pool"], 2, 1)
            fields["card"] = self._card_combo(None)
            body.addWidget(fields["card"], 3, 1)
            if kind == "card":
                cid = self.project.resolve(rule["card"])
                if cid:
                    self._set_combo_card(fields["card"], cid)
                else:
                    fields["card"].setCurrentText(str(rule["card"]))
            note = QLabel("Tiers by weight: tier=weight, comma between. Its own cards: card=weight (names or "
                          "numbers). A slot of its own cards or a fixed card is never dealt again for a "
                          "guarantee or the pity.")
            note.setWordWrap(True)
            note.setStyleSheet("color:#9aacc4")
            body.addWidget(note, 4, 0, 1, 2)

        def ok():
            kind = next(key for key, button in fields["kind"].items() if button.isChecked())
            try:
                if kind == "tier":
                    value = fields["tier"].currentText()
                elif kind == "mix":
                    value = {"tiers": _parse_pairs(fields["mix"].text(), "Tiers by weight")}
                elif kind == "pool":
                    items = []
                    for ref, weight in _parse_pairs(fields["pool"].text(), "Its own cards").items():
                        cid = self.project.resolve(ref)
                        items.append((self.project.ref(cid) if cid else ref, weight))
                    value = {"cards": packmath.pool_value(items)}
                else:
                    cid = self._combo_card_id(fields["card"])
                    if not cid:
                        return "choose the card"
                    value = {"card": self.project.ref(cid)}
            except ValueError as problem:
                return str(problem)
            on_ok(value)
            return None

        self._pack_form_dialog(title, build, ok)
    def _selected_pack_slot(self):
        table = self.workspace_controls["Packs"]["adv"]["slots"]
        row = table.currentRow()
        return row if row >= 0 else None
    def _add_pack_slot(self):
        entry = self._pack_entry()
        if entry is None or not self._commit_packs():
            return

        def done(value):
            if not isinstance(entry.get("slots"), list):
                entry["slots"] = []
            entry["slots"].append(value)
            entry.pop("count", None)
            self._mark_dirty()
            self._refresh_packs()

        self._pack_slot_dialog("Add slot", packmath.tiers_of(entry)[0][0], done)
    def _edit_pack_slot(self):
        entry, index = self._pack_entry(), self._selected_pack_slot()
        if entry is None or index is None or not isinstance(entry.get("slots"), list) \
                or not self._commit_packs():
            return

        def done(value):
            entry["slots"][index] = value
            self._mark_dirty()
            self._refresh_packs()

        self._pack_slot_dialog("Slot", entry["slots"][index], done)
    def _remove_pack_slot(self):
        entry, index = self._pack_entry(), self._selected_pack_slot()
        if entry is None or index is None or not isinstance(entry.get("slots"), list):
            return
        entry["slots"].pop(index)
        if not entry["slots"]:
            del entry["slots"]
        entry.pop("count", None)
        self._mark_dirty()
        self._refresh_packs()
    def _move_pack_slot(self, step):
        entry, index = self._pack_entry(), self._selected_pack_slot()
        if entry is None or index is None or not isinstance(entry.get("slots"), list):
            return
        other = index + step
        if 0 <= other < len(entry["slots"]):
            entry["slots"][index], entry["slots"][other] = entry["slots"][other], entry["slots"][index]
            self._mark_dirty()
            self._refresh_packs()
            self.workspace_controls["Packs"]["adv"]["slots"].selectRow(other)
    def _card_filter_dialog(self, title, accept="Add"):
        """Pick cards by what they are rather than by name alone: the Tk
        FilterPanel, which the bulk dialogs share. [] when nothing is chosen."""
        dialog = QDialog(self)
        dialog.setWindowTitle(title)
        dialog.resize(640, 620)
        outer = QVBoxLayout(dialog)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        body = QWidget()
        grid = QGridLayout(body)
        scroll.setWidget(body)
        outer.addWidget(scroll, 1)
        fields = {}
        row = 0

        def boxes(label, names, key, columns=5):
            nonlocal row
            grid.addWidget(QLabel(label), row, 0, 1, columns)
            row += 1
            made = []
            for n, name in enumerate(names):
                box = QCheckBox(str(name))
                grid.addWidget(box, row + n // columns, n % columns)
                made.append(box)
            row += (len(names) + columns - 1) // columns
            fields[key] = made

        def text(label, key, placeholder=""):
            nonlocal row
            grid.addWidget(QLabel(label), row, 0)
            edit = QLineEdit()
            edit.setPlaceholderText(placeholder)
            grid.addWidget(edit, row, 1, 1, 4)
            fields[key] = edit
            row += 1

        boxes("Kind", [kind.capitalize() for kind in bulk_fusions.KINDS], "kinds", 4)
        boxes("Attribute", ATTRIBUTE_NAMES[:6], "attributes", 6)
        boxes("Guardian star", STAR_NAMES[1:11], "stars", 5)
        boxes("Monster type", TYPE_NAMES[:20], "types", 5)
        grid.addWidget(QLabel("Level"), row, 0)
        fields["level_min"] = QSpinBox()
        fields["level_max"] = QSpinBox()
        for key, value in (("level_min", 0), ("level_max", 12)):
            fields[key].setRange(0, 12)
            fields[key].setValue(value)
        grid.addWidget(fields["level_min"], row, 1)
        grid.addWidget(QLabel("to"), row, 2)
        grid.addWidget(fields["level_max"], row, 3)
        row += 1
        for label, low, high in (("ATK", "atk_min", "atk_max"), ("DEF", "def_min", "def_max")):
            grid.addWidget(QLabel(label), row, 0)
            for n, key in enumerate((low, high)):
                fields[key] = QLineEdit()
                fields[key].setPlaceholderText("any")
                grid.addWidget(fields[key], row, 1 + n * 2)
            grid.addWidget(QLabel("to"), row, 2)
            row += 1
        text("Name holds", "name")
        text("Text holds", "text")
        text("Cards", "cards", "names or numbers, comma between")
        grid.setRowStretch(row, 1)

        count = QLabel()
        count.setStyleSheet("color:#9aacc4")
        outer.addWidget(count)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        add = buttons.addButton(accept, QDialogButtonBox.ButtonRole.AcceptRole)
        outer.addWidget(buttons)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        chosen = {"cards": []}

        def read():
            numbers = {}
            for key in ("atk_min", "atk_max", "def_min", "def_max"):
                value = fields[key].text().strip()
                numbers[key] = int(value) if value.isdigit() else None
            low, high = fields["level_min"].value(), fields["level_max"].value()
            return bulk_fusions.CardFilter(
                kinds={kind for kind, box in zip(bulk_fusions.KINDS, fields["kinds"]) if box.isChecked()},
                attributes={n for n, box in enumerate(fields["attributes"]) if box.isChecked()},
                stars={n + 1 for n, box in enumerate(fields["stars"]) if box.isChecked()},
                types={n for n, box in enumerate(fields["types"]) if box.isChecked()},
                level_min=low or None, level_max=high if high < 12 else None,
                name=fields["name"].text(), text=fields["text"].text(), cards=fields["cards"].text(),
                **numbers)

        def changed():
            try:
                cards, unknown = read().select(self.project)
            except ValueError as problem:
                count.setText(str(problem))
                add.setEnabled(False)
                return
            chosen["cards"] = cards
            add.setEnabled(bool(cards))
            said = f"{len(cards):,} card(s) match"
            if unknown:
                said += f" — not placed: {', '.join(str(x) for x in unknown[:4])}"
            count.setText(said)

        for widget in fields.values():
            for one in (widget if isinstance(widget, list) else [widget]):
                if isinstance(one, QCheckBox):
                    one.toggled.connect(changed)
                elif isinstance(one, QSpinBox):
                    one.valueChanged.connect(changed)
                else:
                    one.textChanged.connect(changed)
        changed()
        return chosen["cards"] if dialog.exec() == QDialog.DialogCode.Accepted else []
    def _pack_card_icon(self, cid):
        """The card's own picture for a contents row, kept once drawn."""
        cache = self.workspace_controls["Packs"].setdefault("pack_art", {})
        key = (cid, id(self.project))
        if key not in cache:
            icon = None
            if self.files is not None and cid in self.project.cards:
                try:
                    icon = QIcon(_card_image(self.project, self.files.wa, cid, self.frame_cache))
                except (OSError, ValueError, KeyError, art.pngio.PngError):
                    icon = None
            cache[key] = icon
        return cache[key]
    def _pack_row_spin(self, value, low, high, changed):
        box = QSpinBox()
        box.setRange(low, high)
        box.setValue(value)
        box.setButtonSymbols(QSpinBox.ButtonSymbols.UpDownArrows)
        box.setStyleSheet("padding:2px 4px")
        box.valueChanged.connect(changed)
        return box
    def _pack_row_tier_box(self, names, current, row):
        """A row's tier, named rather than numbered."""
        box = QComboBox()
        box.addItems(names)
        box.setCurrentIndex(current)
        box.setStyleSheet("padding:2px 4px")
        box.currentIndexChanged.connect(lambda place, r=row: self._set_pack_row_tier(r, place))
        return box
    def _pack_row_remove(self, row):
        button = QToolButton()
        button.setText("\u2715")
        button.setToolTip("Take this card out of the pack")
        button.setStyleSheet("QToolButton { border:0; background:transparent; color:#9aacc4; padding:4px; }"
                             "QToolButton:hover { color:#ff8f87; }")
        button.clicked.connect(lambda *_, r=row: self._remove_pack_row(r))
        return button
    def _pack_row_key(self, row):
        """(tier, place, card) of a contents row."""
        item = self.workspace_controls["Packs"]["contents"].item(row, 0)
        return item.data(Qt.ItemDataRole.UserRole) if item else None
    def _set_pack_row_weight(self, row, value):
        c = self.workspace_controls["Packs"]
        key = self._pack_row_key(row)
        if c["filling"] or key is None:
            return
        entry = self._pack_entry()
        tier_index, place, _cid = key
        name = packmath.tiers_of(entry)[tier_index][0]
        items = packmath.tier_pool(entry, name)
        if not 0 <= place < len(items):
            return
        items[place] = (items[place][0], value)
        packmath.set_tier_pool(entry, name, items)
        self._mark_dirty()
        self._refresh_packs()
    def _set_pack_row_tier(self, row, value):
        """Move one card into the tier at that place in the pack's list."""
        c = self.workspace_controls["Packs"]
        key = self._pack_row_key(row)
        if c["filling"] or key is None:
            return
        entry = self._pack_entry()
        tiers = packmath.tiers_of(entry)
        target = value
        tier_index, place, _cid = key
        if not 0 <= target < len(tiers) or target == tier_index:
            return
        pools = {name: packmath.tier_pool(entry, name) for name, _ in tiers}
        moving = pools[tiers[tier_index][0]].pop(place)
        pools[tiers[target][0]].append(moving)
        for name, items in pools.items():
            packmath.set_tier_pool(entry, name, items)
        self._mark_dirty()
        self._refresh_packs()
    def _remove_pack_row(self, row):
        entry = self._pack_entry()
        key = self._pack_row_key(row)
        if entry is None or key is None:
            return
        tier_index, place, _cid = key
        name = packmath.tiers_of(entry)[tier_index][0]
        items = packmath.tier_pool(entry, name)
        if 0 <= place < len(items):
            del items[place]
            packmath.set_tier_pool(entry, name, items)
            self._mark_dirty()
            self._refresh_packs()
    def _count_pack_description(self):
        """What the game has room for, counted as it counts it: bytes of UTF-8."""
        c = self.workspace_controls["Packs"]
        used = len(c["description"].toPlainText().encode("utf-8"))
        limit = packmath.DESCRIPTION_BYTES
        c["count_label"].setText(f"{used} / {limit}")
        c["count_label"].setStyleSheet("color:#ff8f87" if used > limit else "color:#9aacc4")
    def _explain_pack_weights(self):
        QMessageBox.information(
            self, "Weights and chances",
            "A tier is dealt its own number of the pack's cards, and inside a tier a card is "
            "drawn against the weights: a card of weight 100 among 1000 is drawn one time in "
            "ten.\n\nChance is what the game would give that card over a whole pack, the "
            "tier's share of the cards included. Total weight is every tier's weights added "
            "up, so it is the chances' denominator only where a pack has one tier.")
    def _commit_packs(self):
        c=self.workspace_controls.get("Packs");e=self._pack_entry()
        if not c or not e or self.project.packs_file is not None:return True
        try:
            name=c["name"].text().strip()
            if not name:raise ValueError("A pack needs a name.")
            if len(name)>packmath.NAME_LETTERS:raise ValueError(f"Pack name is limited to {packmath.NAME_LETTERS} characters.")
            # Renaming must not move the pack's identity: a save counts what
            # the player opened and bought by it (PacksTab.store).
            old_id=packmath.pack_id(e)
            new=copy.deepcopy(e);new["name"]=name
            if "id" not in e and packmath.slug(name)!=old_id:new["id"]=old_id
            desc=c["description"].toPlainText().strip()
            if desc:new["description"]=desc
            else:new.pop("description",None)
            price=c["price"].value()
            if isinstance(new.get("cost"),dict) and "starchips" in new["cost"]:new["cost"]["starchips"]=price
            else:new["price"]=price
            count=c["count"].value()
            if count==packmath.default_count(new):new.pop("count",None)
            else:new["count"]=count
            self._store_pack_advanced(e,new)
            # After the advanced forms: "stock" is one of Dealing's fields, so
            # setting it before them would be written over from that tab.
            if c["infinite"].isChecked():new.pop("stock",None)
            else:new["stock"]=max(1,c["stock"].value())
        except (ValueError,json.JSONDecodeError) as problem:c["problem"].setText(str(problem));return False
        if packmath.minimize(new)!=packmath.minimize(e):e.clear();e.update(packmath.minimize(new));self._mark_dirty()
        c["adv_baseline"]=self._pack_advanced_state()
        c["problem"].clear();return True
    def _select_pack(self,row):
        if row<0:return
        c=self.workspace_controls["Packs"]
        if not self._commit_packs():c["list"].setCurrentRow(c["pack_index"]);return
        c["pack_index"]=row;self._fill_pack()
    def _apply_pack(self):
        if self._commit_packs():self._refresh_packs();self.workspace_controls["Packs"]["status"].setText("Changes applied to this mod.")
    def _add_pack(self):
        if not self._commit_packs():return
        self.project.packs.append(packmath.new_pack(f"Pack {len(self.project.packs)+1}",{packmath.pack_id(x) for x in self.project.packs}));self.workspace_controls["Packs"]["pack_index"]=len(self.project.packs)-1;self._mark_dirty();self._refresh_packs()
    def _duplicate_pack(self):
        e=self._pack_entry()
        if e is None or not self._commit_packs():return
        n=copy.deepcopy(e);n["name"]=(str(e.get("name","Pack"))+" copy")[:packmath.NAME_LETTERS];n.pop("id",None);c=self.workspace_controls["Packs"];c["pack_index"]+=1;self.project.packs.insert(c["pack_index"],n);self._mark_dirty();self._refresh_packs()
    def _remove_pack(self):
        e=self._pack_entry();c=self.workspace_controls["Packs"]
        if e is None:return
        if QMessageBox.question(self,"Remove pack",f"Remove {e.get('name','this pack')}?",QMessageBox.StandardButton.Yes|QMessageBox.StandardButton.No,QMessageBox.StandardButton.No)==QMessageBox.StandardButton.Yes:self.project.packs.pop(c["pack_index"]);c["pack_index"]=max(0,c["pack_index"]-1);self._mark_dirty();self._refresh_packs()
    def _move_pack(self,step):
        c=self.workspace_controls["Packs"];i=c["pack_index"];j=i+step
        if 0<=j<len(self.project.packs) and self._commit_packs():self.project.packs[i],self.project.packs[j]=self.project.packs[j],self.project.packs[i];c["pack_index"]=j;self._mark_dirty();self._refresh_packs()
    def _reset_packs(self):
        if QMessageBox.question(self,"Reset packs","Remove all custom packs and shop settings?",QMessageBox.StandardButton.Yes|QMessageBox.StandardButton.No,QMessageBox.StandardButton.No)==QMessageBox.StandardButton.Yes:self.project.packs=[];self.project.pack_shop=None;self.workspace_controls["Packs"]["pack_index"]=-1;self._mark_dirty();self._refresh_packs()
    def _choose_pack_cards(self):
        dialog=QDialog(self);dialog.setWindowTitle("Add filtered cards");dialog.resize(500,600);layout=QVBoxLayout(dialog);search=QLineEdit();search.setPlaceholderText("Search by card name or ID…");listing=QListWidget();listing.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        for cid,card in sorted(self.project.cards.items()):item=QListWidgetItem(f"{cid:03d}  {card.name}");item.setData(Qt.ItemDataRole.UserRole,cid);listing.addItem(item)
        search.textChanged.connect(lambda text:[listing.item(i).setHidden(text.casefold() not in listing.item(i).text().casefold()) for i in range(listing.count())]);layout.addWidget(search);layout.addWidget(listing);buttons=QDialogButtonBox(QDialogButtonBox.StandardButton.Ok|QDialogButtonBox.StandardButton.Cancel);layout.addWidget(buttons);buttons.accepted.connect(dialog.accept);buttons.rejected.connect(dialog.reject)
        return [int(i.data(Qt.ItemDataRole.UserRole)) for i in listing.selectedItems()] if dialog.exec()==QDialog.DialogCode.Accepted else []
    def _insert_pack_cards(self,ids):
        e=self._pack_entry();c=self.workspace_controls["Packs"]
        if not e:return
        tiers=packmath.tiers_of(e);place=min(max(0,c["tier"].currentIndex()),len(tiers)-1)
        tier=tiers[place][0];items=packmath.tier_pool(e,tier);present={self.project.resolve(ref):i for i,(ref,_) in enumerate(items)}
        for cid in ids:
            if cid in present:items[present[cid]]=(items[present[cid]][0],c["weight"].value())
            else:items.append((self.project.ref(cid),c["weight"].value()))
        packmath.set_tier_pool(e,tier,items);self._mark_dirty();self._refresh_packs()
    def _add_pack_card(self):
        if not self._commit_packs():return
        cid=self._choose_one_card("Add a card to the pack")
        if cid:self._insert_pack_cards([cid])
    def _add_filtered_pack_cards(self):
        if not self._commit_packs():return
        ids=self._card_filter_dialog("Add filtered cards")
        if ids:self._insert_pack_cards(ids)
    def _selected_pack_items(self):
        t=self.workspace_controls["Packs"]["contents"];return [t.item(r,0).data(Qt.ItemDataRole.UserRole) for r in sorted({i.row() for i in t.selectedItems()})]
    def _remove_pack_cards(self):
        e=self._pack_entry();selected=self._selected_pack_items()
        if not e or not selected:return
        by={}
        for ti,k,cid in selected:by.setdefault(ti,set()).add(k)
        tiers=packmath.tiers_of(e)
        for ti,indices in by.items():name=tiers[ti][0];items=packmath.tier_pool(e,name);packmath.set_tier_pool(e,name,[v for i,v in enumerate(items) if i not in indices])
        self._mark_dirty();self._refresh_packs()
    def _select_pack_content(self):
        selected=self._selected_pack_items()
        if selected:self.workspace_controls["Packs"]["tier"].setCurrentIndex(selected[0][0])
    def _set_pack_weight(self):
        e=self._pack_entry();selected=self._selected_pack_items()
        if not e or not selected:return
        by={}
        for ti,k,cid in selected:by.setdefault(ti,set()).add(k)
        tiers=packmath.tiers_of(e);weight=self.workspace_controls["Packs"]["weight"].value()
        for ti,indices in by.items():
            name=tiers[ti][0];items=packmath.tier_pool(e,name)
            for i,(ref,old) in enumerate(items):
                if i in indices:items[i]=(ref,weight)
            packmath.set_tier_pool(e,name,items)
        self._mark_dirty();self._refresh_packs()
    def _set_pack_tier(self):
        """Move the selected cards into the tier the box names (PacksTab.set_tier)."""
        e=self._pack_entry();selected=self._selected_pack_items();c=self.workspace_controls["Packs"]
        if not e or not selected:return
        tiers=packmath.tiers_of(e)
        place=c["tier"].currentIndex()
        if not 0<=place<len(tiers):return
        target=tiers[place][0]
        by={}
        for ti,k,cid in selected:by.setdefault(ti,set()).add(k)
        pools={name:packmath.tier_pool(e,name) for name,_ in tiers}
        moving=[]
        for ti,indices in by.items():
            name=tiers[ti][0]
            if name==target:continue
            moving+=[item for i,item in enumerate(pools[name]) if i in indices]
            pools[name]=[item for i,item in enumerate(pools[name]) if i not in indices]
        if not moving:return
        pools.setdefault(target,[])
        pools[target]+=moving
        for name,items in pools.items():packmath.set_tier_pool(e,name,items)
        self._mark_dirty();self._refresh_packs()
    def _toggle_pack_advanced(self):
        """The five forms behind the plain fields, in a window of their own."""
        c = self.workspace_controls["Packs"]
        dialog = c.get("advanced_dialog")
        if dialog is None:
            dialog = QDialog(self)
            dialog.setWindowTitle("Advanced pack settings")
            dialog.resize(760, 520)
            layout = QVBoxLayout(dialog)
            layout.addWidget(c["advanced_tabs"])
            # The same rule as a page's lists: every table is offered sorting,
            # and the ones UNSORTED_TABLES names decline it. A tier's row and a
            # slot's row are the order the game reads them in, so both decline.
            for table in dialog.findChildren(QTableWidget):
                allow_sorting(table)
            line = QHBoxLayout()
            apply_button = QPushButton("Apply")
            apply_button.clicked.connect(self._apply_pack)
            line.addWidget(apply_button)
            kept = QLabel("Unknown keys of a pack stay as written.")
            kept.setStyleSheet("color:#9aacc4")
            line.addWidget(kept)
            line.addStretch(1)
            close = QPushButton("Close")
            close.clicked.connect(dialog.hide)
            line.addWidget(close)
            layout.addLayout(line)
            c["advanced_dialog"] = dialog
        dialog.show()
        dialog.raise_()
    def _edit_pack_shop(self):
        """The shop's own rules, the ones that are not a pack's
        (PacksTab.shop_settings)."""
        if not self._commit_packs():
            return
        rules = copy.deepcopy(self.project.pack_shop) if isinstance(self.project.pack_shop, dict) else {}
        fields, shown = {}, {}

        def build(body):
            def pick(key, values, default):
                box = QComboBox()
                box.addItems(list(values))
                box.setCurrentText(str(rules.get(key, default)))
                fields[key] = box
                return box

            def note(text, r):
                label = QLabel(text)
                label.setWordWrap(True)
                label.setStyleSheet("color:#9aacc4")
                body.addWidget(label, r, 0, 1, 2)

            body.addWidget(QLabel("Password screen"), 0, 0)
            body.addWidget(pick("password", packmath.SHOP_PASSWORD, "both"), 0, 1)
            note("both: passwords and packs (triangle); packs_only: the screen opens on the packs; "
                 "password_only: no triangle (a pack's own password still sells it)", 1)
            body.addWidget(QLabel("Random numbers"), 2, 0)
            body.addWidget(pick("rng", ("game", "save"), "game"), 2, 1)
            note("save: a pack dealt from the save, so reloading it deals the same cards", 3)
            body.addWidget(QLabel("Music"), 4, 0)
            fields["music"] = QLineEdit(str(rules.get("music", packmath.DEFAULT_MUSIC)))
            fields["music"].setMaximumWidth(110)
            body.addWidget(fields["music"], 4, 1)
            body.addWidget(QLabel("All owned"), 5, 0)
            body.addWidget(pick("when_nothing_left", packmath.NOTHING_LEFT, "refuse"), 5, 1)
            note("for a pack of Max copies that says nothing: refuse it (ALL OWNED) or sell it with empty "
                 "slots when the player holds that many of every card", 6)
            note("Shops: one a line, id | name | unlock as JSON (optional)", 7)
            fields["shops"] = QPlainTextEdit()
            fields["shops"].setMinimumHeight(110)
            lines = []
            for shop in rules.get("shops", []) if isinstance(rules.get("shops"), list) else []:
                if isinstance(shop, dict):
                    text = f"{shop.get('id', '')} | {shop.get('name', '')}"
                    if "unlock" in shop:
                        text += " | " + json.dumps(shop["unlock"], ensure_ascii=False)
                    lines.append(text)
            fields["shops"].setPlainText("\n".join(lines))
            body.addWidget(fields["shops"], 8, 0, 1, 2)
            note("Needs a restart of the game. Not yet in the game (the keys are kept for them): "
                 + ", ".join(packmath.NOT_YET) + ". A shop's other keys (\"where\" and any the editor has "
                 "no field for) stay as written.", 9)
            shown.update({key: widget.currentText() for key, widget in fields.items()
                          if isinstance(widget, QComboBox)})
            shown["music"] = fields["music"].text()
            shown["shops"] = fields["shops"].toPlainText()

        def ok():
            new = dict(rules)
            # A field still showing what was written leaves the key as it was.
            for key in ("password", "rng", "when_nothing_left"):
                if fields[key].currentText() != shown[key]:
                    new[key] = fields[key].currentText()
            if fields["music"].text() != shown["music"]:
                try:
                    new["music"] = _whole(fields["music"].text(), "Music", 0, 0xFFFF, packmath.DEFAULT_MUSIC)
                except ValueError as problem:
                    return str(problem)
            if fields["shops"].toPlainText() != shown["shops"]:
                written = {}
                for shop in rules.get("shops") if isinstance(rules.get("shops"), list) else []:
                    if isinstance(shop, dict) and isinstance(shop.get("id"), str):
                        written.setdefault(shop["id"], shop)
                shops = []
                for n, text in enumerate(fields["shops"].toPlainText().splitlines(), start=1):
                    if not text.strip():
                        continue
                    parts = [part.strip() for part in text.split("|", 2)]
                    if not packmath.KEY_RE.match(parts[0]):
                        return f"shop line {n}: an id is 1-63 letters, digits, '_' or '-'"
                    # The shop as written, its "where" and unknown keys kept.
                    shop = copy.deepcopy(written.get(parts[0], {}))
                    shop["id"] = parts[0]
                    if len(parts) > 1 and parts[1]:
                        shop["name"] = parts[1]
                    else:
                        shop.pop("name", None)      # the game names it by its id
                    if len(parts) > 2 and parts[2]:
                        try:
                            shop["unlock"] = json.loads(parts[2])
                        except json.JSONDecodeError as problem:
                            return f"shop line {n}: the unlock is not JSON ({problem.msg})"
                    else:
                        shop.pop("unlock", None)
                    shops.append({"id": shop.pop("id"), **shop})
                if len({shop["id"] for shop in shops}) > packmath.SHOPS_MAX:
                    return f"at most {packmath.SHOPS_MAX} shops"
                new["shops"] = shops
            self.project.pack_shop = packmath.minimize_rules(new)
            self._mark_dirty()
            self._refresh_packs()
            return None

        self._pack_form_dialog("Shop settings", build, ok)
    def _simulate_pack(self):
        if not self._commit_packs():return
        pack,notes=self._pack_read()
        if pack is None:QMessageBox.warning(self,"Simulation",next((m for level,m in notes if level=="error"),"Invalid pack"));return
        c=self.workspace_controls["Packs"]
        c["simulation_engine"]=packmath.Simulator(pack,1)
        c["simulation_target"]=c["sim_count"].value()
        c["simulation"]=None
        c["status"].setText(f"Simulating 0 of {c['simulation_target']:,} packs…")
        c["simulation_timer"].start(0)
    def _pack_simulation_step(self):
        c=self.workspace_controls["Packs"];engine=c.get("simulation_engine")
        if engine is None:return
        remaining=c["simulation_target"]-engine.opened
        engine.step(min(remaining,max(1,500//max(1,engine.pack.count))))
        if engine.opened<c["simulation_target"]:
            c["status"].setText(f"Simulating {engine.opened:,} of {c['simulation_target']:,} packs…")
            c["simulation_timer"].start(1);return
        c["simulation"]=engine.result();c["simulation_engine"]=None
        c["status"].setText(f"Simulated {c['simulation'].packs:,} packs.")
        self._show_pack_results()
    def _show_pack_results(self):
        c=self.workspace_controls["Packs"];result=c.get("simulation")
        if result is None:QMessageBox.information(self,"Simulation","Simulate a pack first.");return
        table=c["sim_results"]
        if table.isVisible():
            table.hide();return
        table.setColumnCount(4);table.setHorizontalHeaderLabels(["Card","Copies","Per pack","Chance"]);held=sort_paused(table);table.setRowCount(0)
        dealt=sum(result.tiers.values()) or 1
        for cid,n in sorted(result.cards.items(),key=lambda x:(-x[1],x[0])):
            r=table.rowCount();table.insertRow(r)
            for col,val in enumerate((self.project.card_label(cid),n,f"{n/result.packs:.3f}",f"{n/dealt*100:.2f}%")):table.setItem(r,col,TableItem(str(val)))
        sort_resumed(table,held)
        table.horizontalHeader().setSectionResizeMode(0,QHeaderView.ResizeMode.Stretch)
        table.show()
    def _pack_image_shared(self,name,entry) -> bool:
        """Whether a pack other than `entry` names this picture too
        (PacksTab.image_shared): one it shares must not be thrown away."""
        return any(isinstance(other,dict) and other is not entry and other.get("image")==name
                   for other in self.project.packs)
    def _free_pack_image_name(self,pid,entry=None) -> str:
        """packs/<id>.png, or -2, -3… when another pack names that already."""
        name,n=f"packs/{pid}.png",2
        while self._pack_image_shared(name,entry):
            name,n=f"packs/{pid}-{n}.png",n+1
        return name
    def _import_pack_image(self):
        e=self._pack_entry()
        if not e:return
        path,_=QFileDialog.getOpenFileName(self,"Import pack image","","PNG images (*.png)")
        if not path:return
        # As PacksTab.use_file has it: cut to the pack's shape, made opaque
        # (a pixel under half alpha is a hole in the card) and no larger than
        # the game's 4x. Storing the file as it came leaves the game a
        # picture it cannot draw.
        try:image,notes=art.normalize(pngio.read(path),"art")
        except (OSError,ValueError,pngio.PngError) as error:QMessageBox.warning(self,"Pack image",str(error));return
        old=e.get("image")
        keep=isinstance(old,str) and old in self.project.files and not self._pack_image_shared(old,e)
        name=old if keep else self._free_pack_image_name(packmath.pack_id(e),e)
        if isinstance(old,str) and old!=name and not self._pack_image_shared(old,e):
            self.project.files.pop(old,None)      # nothing else names it now
        self.project.files[name]=pngio.encode(image);e["image"]=name
        self._mark_dirty();self._refresh_packs()
        self.workspace_controls["Packs"]["status"].setText(
            f"Pack picture from {Path(path).name}"+(": "+"; ".join(notes) if notes else ""))
    def _export_pack_image(self):
        e=self._pack_entry()
        if not e:return
        path=e.get("image");blob=self.project.files.get(path) if isinstance(path,str) else None
        if blob is None and isinstance(path,str) and self.project.source_dir:
            try:blob=(Path(self.project.source_dir)/path).read_bytes()
            except OSError:pass
        if not blob:QMessageBox.information(self,"Pack image","This pack has no image.");return
        target,_=QFileDialog.getSaveFileName(self,"Export pack image",f"{packmath.pack_id(e)}.png","PNG images (*.png)")
        if target:Path(target).write_bytes(blob)
    def _revert_pack_image(self):
        e=self._pack_entry()
        if not e or "image" not in e:return
        name=e.pop("image")
        # The file goes with it unless another pack still names it; without
        # this the mod folder keeps a picture nothing draws (PacksTab.revert_png).
        if isinstance(name,str) and not self._pack_image_shared(name,e):
            self.project.files.pop(name,None)
        self._mark_dirty();self._refresh_packs()
    def goto_pack(self, index):
        """Packs: the pack a validation line is about (PacksTab.goto)."""
        controls = self.workspace_controls.get("Packs")
        if not controls:
            return
        if isinstance(index, int) and 0 <= index < controls["list"].count():
            controls["list"].setCurrentRow(index)
    def _update_art_pack_status(self, state=None):
        label = getattr(self, "art_pack_status", None)
        if label is None:
            return
        state = state or art.state(self.project)
        folder = self.project.other.get("textures")
        if not isinstance(folder, str) or not folder:
            label.setText("Texture pack: none configured")
        elif state.problem:
            label.setText(f"Texture pack: {folder} · could not read manifest ({state.problem})")
        elif state.entries is None:
            label.setText(f"Texture pack: {folder} · manifest not loaded")
        else:
            card_entries = set(state.owned.values()) | set(state.gated)
            gated_count = len(state.gated)
            label.setText(
                f"Texture pack loaded: {folder} · {len(state.entries)} entries · "
                f"{len(card_entries)} card images recognized ({gated_count} setting-gated)")
