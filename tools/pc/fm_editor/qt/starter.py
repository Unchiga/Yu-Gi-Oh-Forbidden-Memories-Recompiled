"""The Starter decks page: the written decks and the weighted pools."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class StarterMixin:
    def _current_starter(self):
        index=getattr(self,"starter_current",None)
        return self.project.starter[index] if index is not None and 0<=index<len(self.project.starter) else None
    def _build_starter_pools(self, page, get, controls):
        controls.update(pools=get(QTableWidget, "starterPoolTable"),
                        pool_cards=get(QTableWidget, "starterPoolCardTable"),
                        pool_name=get(QLineEdit, "starterPoolNameEdit"),
                        pool_draws=get(QSpinBox, "starterPoolDrawSpin"),
                        pool_weight=get(QSpinBox, "starterPoolWeightSpin"),
                        pool_total=get(QLabel, "starterPoolDrawTotal"),
                        pool_heading=get(QLabel, "starterPoolHeading"),
                        pool_summary=get(QLabel, "starterPoolSummary"),
                        pool_status=get(QLabel, "starterPoolStatus"),
                        starter_tabs=get(QTabWidget, "starterTabs"))
        self.starter_pool_current = 0
        self.starter_pools_filling = False
        controls["pools"].setColumnCount(5)
        controls["pool_cards"].setColumnCount(6)
        controls["pools"].itemSelectionChanged.connect(self._select_starter_pool)
        controls["pool_cards"].itemSelectionChanged.connect(self._select_starter_pool_card)
        controls["pool_draws"].valueChanged.connect(self._set_starter_pool_draws)
        controls["pool_name"].editingFinished.connect(self._set_starter_pool_name)
        for key, callback in (("addStarterPoolButton", self._add_starter_pool),
                              ("removeStarterPoolButton", self._remove_starter_pool),
                              ("moveStarterPoolUpButton", lambda: self._move_starter_pool(-1)),
                              ("moveStarterPoolDownButton", lambda: self._move_starter_pool(1)),
                              ("addPoolWeightButton", self._add_starter_pool_card),
                              ("addFilteredPoolCardsButton", self._add_filtered_starter_pool_cards),
                              ("setPoolWeightValueButton", self._set_starter_pool_weight),
                              ("removePoolWeightButton", self._remove_starter_pool_cards)):
            get(QPushButton, key).clicked.connect(callback)
        for muted in ("starterPoolsHint", "starterPoolSummary", "starterPoolStatus", "starterPoolDrawTotal"):
            get(QLabel, muted).setStyleSheet("color:#9aacc4")
        for title in ("starterPoolListHeading", "starterPoolHeading"):
            get(QLabel, title).setStyleSheet("font-size:16px;font-weight:650;color:#f3f7fc")
        splitter = get(QSplitter, "starterPoolsSplit")
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 6)
        splitter.setSizes([460, 820])
        # Both tabs: the slack belongs to the panels, not to the hint above
        # them, or a QVBoxLayout shares it out evenly.
        for layout_name, grows in (("starterPoolsLayout", "starterPoolsSplit"),
                                   ("starterWrittenLayout", "starterSplit")):
            layout = page.findChild(QVBoxLayout, layout_name)
            if layout is None:
                continue
            for index in range(layout.count()):
                item = layout.itemAt(index)
                layout.setStretch(index, 1 if item.widget() is get(QSplitter, grows) else 0)
        get(QLabel, "starterPoolsHint").setSizePolicy(QSizePolicy.Policy.Preferred,
                                                      QSizePolicy.Policy.Minimum)
        for name in ("starterPoolsSplit", "starterSplit"):
            get(QSplitter, name).setSizePolicy(QSizePolicy.Policy.Expanding,
                                               QSizePolicy.Policy.Expanding)
    def _starter_pools(self):
        """The pools the tab shows: the mod's own, or the disc's where it
        offers none, so the page says what a new game would draw either way."""
        mine = starter_pools.state(self.project)
        if mine:
            self.starter_pools_stock = False
            return mine
        stock = getattr(self, "starter_pools_retail", None)
        if stock is None:
            stock = starter_pools.retail(self.files.wa if self.files is not None else b"")
            # Rows that would not deal a deck are no rows at all: an archive
            # without them reads as seven empty ones.
            if starter_pools.retail_drawn(stock) != starter_pools.DRAWS:
                stock = []
            self.starter_pools_retail = stock
        self.starter_pools_stock = bool(stock)
        return stock
    def _starter_pool(self):
        pools = self._starter_pools()
        index = getattr(self, "starter_pool_current", 0)
        return pools[index] if 0 <= index < len(pools) else None
    def _starter_pools_editable(self) -> bool:
        """The disc's rows are shown, not edited: Add a pool starts the mod's."""
        return not getattr(self, "starter_pools_stock", False)
    def _starter_pools_own(self):
        """The mod's own pools, whichever the page is showing: an edit writes
        the mod's set, never the disc's rows."""
        return starter_pools.state(self.project)
    def _refresh_starter_pools(self):
        c = self.workspace_controls.get("Starter decks")
        if not c or "pools" not in c:
            return
        pools = self._starter_pools()
        table = c["pools"]
        table.blockSignals(True)
        held = sort_paused(table)
        table.setRowCount(0)
        for index, pool in enumerate(pools):
            row = table.rowCount()
            table.insertRow(row)
            for column, value in enumerate((index + 1, pool.name or "(unnamed)", pool.draws,
                                            pool.count(), pool.total())):
                item = TableItem(str(value))
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, index)
                self._tint_state(item, "removed" if pool.kept else "")
                table.setItem(row, column, item)
        sort_resumed(table, held)
        header = table.horizontalHeader()
        for column, width in ((0, 40), (2, 64), (3, 64), (4, 80)):
            header.setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
            header.resizeSection(column, width)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        if pools:
            index = min(getattr(self, "starter_pool_current", 0), len(pools) - 1)
            self.starter_pool_current = index
            self._select_table_id(table, index)
        else:
            self.starter_pool_current = 0
        table.blockSignals(False)
        stock = not self._starter_pools_editable()
        drawn = starter_pools.retail_drawn(pools) if stock else starter_pools.drawn(self.project)
        if not pools:
            c["pool_total"].setText("No pools: a new game takes a written deck, or the disc's own rows.")
        elif stock:
            c["pool_total"].setText(f"The disc's own rows: {drawn} of {starter_pools.DRAWS} cards drawn in all. "
                                    "This mod weights none of its own — Add a pool starts a set that is "
                                    "read in their place.")
        elif drawn == starter_pools.DRAWS:
            c["pool_total"].setText(f"{drawn} of {starter_pools.DRAWS} cards drawn in all")
        else:
            c["pool_total"].setText(f"{drawn} of {starter_pools.DRAWS} cards drawn in all — the pools must "
                                    "draw forty, or the game reads the disc's own rows")
        # The disc's rows are shown as they are: everything but Add a pool is
        # off until the mod weights its own.
        for name in ("removeStarterPoolButton", "moveStarterPoolUpButton", "moveStarterPoolDownButton",
                     "addPoolWeightButton", "addFilteredPoolCardsButton", "setPoolWeightValueButton",
                     "removePoolWeightButton"):
            form = self.workspace_forms.get("Starter decks")
            button = form.findChild(QPushButton, name) if form is not None else None
            if button is not None:
                button.setEnabled(not stock)
        self._fill_starter_pool_cards()
    def _select_starter_pool(self):
        table = self.workspace_controls["Starter decks"]["pools"]
        row = table.currentRow()
        if row < 0:
            return
        index = table.item(row, 0).data(Qt.ItemDataRole.UserRole)
        if index is not None:
            self.starter_pool_current = index
            self._fill_starter_pool_cards()
    def _fill_starter_pool_cards(self):
        c = self.workspace_controls["Starter decks"]
        pool = self._starter_pool()
        table = c["pool_cards"]
        table.blockSignals(True)
        table.setRowCount(0)
        self.starter_pools_filling = True
        try:
            c["pool_name"].setText(pool.name or "" if pool else "")
            c["pool_draws"].setValue(pool.draws if pool else 0)
        finally:
            self.starter_pools_filling = False
        editable = pool is not None and self._starter_pools_editable()
        c["pool_draws"].setEnabled(editable)
        c["pool_name"].setEnabled(editable)
        c["pool_weight"].setEnabled(editable)
        if pool is None:
            table.blockSignals(False)
            c["pool_heading"].setText("Cards in this pool")
            c["pool_summary"].setText("")
            self._report_starter_pools()
            return
        total = pool.total() or 1
        held = sort_paused(table)
        for cid, weight in sorted(pool.cards.items()):
            card = self.project.cards.get(cid)
            row = table.rowCount()
            table.insertRow(row)
            values = (cid, card.name if card else "?",
                      TYPE_NAMES[card.type] if card and 0 <= card.type < len(TYPE_NAMES) else "",
                      weight, f"{weight * 100 / total:.2f}%" if weight else "", "")
            for column, value in enumerate(values):
                item = TableItem(str(value))
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, cid)
                table.setItem(row, column, item)
        # A card the editor could not place keeps its row, so a pool somebody
        # else wrote is not quietly thrown away.
        for name, weight in pool.kept.items():
            row = table.rowCount()
            table.insertRow(row)
            for column, value in enumerate(("", name, "", weight, "",
                                            "no such card; the game leaves it out")):
                item = TableItem(str(value))
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, f"kept:{name}")
                self._tint_state(item, "removed")
                table.setItem(row, column, item)
        sort_resumed(table, held)
        table.blockSignals(False)
        header = table.horizontalHeader()
        for column, width in ((0, 56), (2, 110), (3, 80), (4, 80)):
            header.setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
            header.resizeSection(column, width)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(5, QHeaderView.ResizeMode.Stretch)
        c["pool_heading"].setText(f"Pool {self.starter_pool_current + 1}"
                                  + (f" · {pool.name}" if pool.name else ""))
        c["pool_summary"].setText(f"{pool.count()} cards · weight {pool.total()}")
        self._report_starter_pools()
    def _report_starter_pools(self):
        c = self.workspace_controls["Starter decks"]
        issues = []
        starter_pools.check(self.project, issues)
        c["pool_status"].setText("\n".join(f"{i.level}: {i.where}: {i.message}" for i in issues[:6]))
        c["pool_status"].setStyleSheet("color:#ff7777" if any(i.level == "error" for i in issues)
                                       else "color:#f2c04c" if issues else "color:#9aacc4")
    def _starter_pools_edited(self):
        starter_pools.store(self.project)
        self._mark_dirty()
        self._refresh_starter_pools()
    def _add_starter_pool(self):
        pools = self._starter_pools_own()
        pools.append(starter_pools.Pool(draws=0))
        self.starter_pool_current = len(pools) - 1
        self._starter_pools_edited()
    def _remove_starter_pool(self):
        pools = self._starter_pools_own()
        index = getattr(self, "starter_pool_current", 0)
        if not 0 <= index < len(pools):
            return
        pools.pop(index)
        self.starter_pool_current = max(0, index - 1)
        self._starter_pools_edited()
    def _move_starter_pool(self, step):
        pools = self._starter_pools_own()
        index = getattr(self, "starter_pool_current", 0)
        other = index + step
        if not (0 <= index < len(pools) and 0 <= other < len(pools)):
            return
        pools[index], pools[other] = pools[other], pools[index]
        self.starter_pool_current = other
        self._starter_pools_edited()
    def _set_starter_pool_name(self):
        pool = self._starter_pool()
        if pool is None or self.starter_pools_filling:
            return
        name = self.workspace_controls["Starter decks"]["pool_name"].text().strip()
        if (pool.name or "") == name:
            return
        pool.name = name or None
        self._starter_pools_edited()
    def _set_starter_pool_draws(self, value):
        pool = self._starter_pool()
        if pool is None or self.starter_pools_filling or pool.draws == value:
            return
        pool.draws = value
        self._starter_pools_edited()
    def _selected_starter_pool_rows(self):
        table = self.workspace_controls["Starter decks"]["pool_cards"]
        return [table.item(row, 0).data(Qt.ItemDataRole.UserRole)
                for row in sorted({i.row() for i in table.selectedItems()}) if table.item(row, 0)]
    def _select_starter_pool_card(self):
        pool = self._starter_pool()
        chosen = self._selected_starter_pool_rows()
        if pool is not None and len(chosen) == 1 and isinstance(chosen[0], int):
            self.workspace_controls["Starter decks"]["pool_weight"].setValue(pool.cards.get(chosen[0], 0))
    def _weight_starter_pool_cards(self, ids):
        pool = self._starter_pool()
        if pool is None or not ids:
            return
        weight = self.workspace_controls["Starter decks"]["pool_weight"].value()
        for cid in ids:
            pool.cards[cid] = weight or 1
        self._starter_pools_edited()
    def _add_starter_pool_card(self):
        if self._starter_pool() is None:
            return
        cid = self._choose_one_card("Card to weight in this pool")
        if cid:
            self._weight_starter_pool_cards([cid])
    def _add_filtered_starter_pool_cards(self):
        if self._starter_pool() is None:
            return
        self._weight_starter_pool_cards(self._choose_pack_cards())
    def _set_starter_pool_weight(self):
        pool = self._starter_pool()
        if pool is None:
            return
        weight = self.workspace_controls["Starter decks"]["pool_weight"].value()
        for cid in self._selected_starter_pool_rows():
            if isinstance(cid, int):
                pool.cards[cid] = weight
            elif isinstance(cid, str) and cid.startswith("kept:"):
                pool.kept[cid[5:]] = weight
        self._starter_pools_edited()
    def _remove_starter_pool_cards(self):
        pool = self._starter_pool()
        if pool is None:
            return
        for cid in self._selected_starter_pool_rows():
            if isinstance(cid, str) and cid.startswith("kept:"):
                pool.kept.pop(cid[5:], None)
            else:
                pool.cards.pop(cid, None)
        self._starter_pools_edited()
    def _refresh_starter_decks(self):
        c=self.workspace_controls["Starter decks"];table=c["decks"]
        table.blockSignals(True);held=sort_paused(table);table.setRowCount(0)
        for index,deck in enumerate(self.project.starter):
            row=table.rowCount();table.insertRow(row)
            values=(index+1,deck.name or "(unnamed)",deck.weight,f"{deck.total()}/{DECK_SIZE}")
            for col,value in enumerate(values):
                item=TableItem(str(value))
                if col==0:item.setData(Qt.ItemDataRole.UserRole,index)
                table.setItem(row,col,item)
        sort_resumed(table,held)
        if self.project.starter:
            self.starter_current=min(getattr(self,"starter_current",None) or 0,len(self.project.starter)-1)
            self._select_table_id(table,self.starter_current)
        else:self.starter_current=None
        table.blockSignals(False);self._fill_starter_cards()
    def _select_starter_deck(self):
        table=self.workspace_controls["Starter decks"]["decks"];row=table.currentRow()
        if row>=0:
            self.starter_current=table.item(row,0).data(Qt.ItemDataRole.UserRole)
            self._fill_starter_cards()
    def _fill_starter_cards(self):
        c=self.workspace_controls["Starter decks"];deck=self._current_starter();table=c["cards"]
        table.setRowCount(0)
        c["name"].setText(deck.name if deck else "")
        c["weight"].setValue(deck.weight if deck else 0)
        if not deck:return
        rows=[]
        for cid,copies in sorted(deck.cards.items()):
            card=self.project.cards.get(cid)
            warnings=[]
            if copies>DECK_COPY_LIMIT:warnings.append(f"Over {DECK_COPY_LIMIT} copies")
            if exodia_piece(cid) and copies>1:warnings.append("Exodia piece")
            rows.append((cid,card.name if card else "?",TYPE_NAMES[card.type] if card else "",copies,", ".join(warnings)))
        for label,copies in deck.kept.items():rows.append(("",label,"",copies,"Kept as written"))
        held=sort_paused(table);table.setRowCount(len(rows))
        for i,row in enumerate(rows):
            for col,value in enumerate(row):
                item=TableItem(str(value))
                if col==0:item.setData(Qt.ItemDataRole.UserRole,row[0])
                table.setItem(i,col,item)
        sort_resumed(table,held)
    def _select_starter_card(self):
        c=self.workspace_controls["Starter decks"];row=c["cards"].currentRow()
        if row>=0 and c["cards"].item(row,0).data(Qt.ItemDataRole.UserRole):
            c["copies"].setValue(int(c["cards"].item(row,3).text()))
    def _add_starter_deck(self):
        self.project.starter.append(StarterDeck(name=f"Deck {len(self.project.starter)+1}"))
        self.starter_current=len(self.project.starter)-1;self._mark_dirty();self._refresh_starter_decks()
    def _remove_starter_deck(self):
        deck=self._current_starter()
        if deck is None:return
        if QMessageBox.question(self,"Remove starter deck",f"Remove {deck.name or 'this deck'}?")!=QMessageBox.StandardButton.Yes:return
        self.project.starter.pop(self.starter_current)
        self.starter_current=max(0,self.starter_current-1) if self.project.starter else None
        self._mark_dirty();self._refresh_starter_decks()
    def _apply_starter_details(self):
        deck=self._current_starter()
        if deck is None:return
        deck.name=self.workspace_controls["Starter decks"]["name"].text().strip()
        deck.weight=self.workspace_controls["Starter decks"]["weight"].value()
        self._mark_dirty();self._refresh_starter_decks()
    def _add_starter_card(self):
        deck=self._current_starter()
        if deck is None:return
        cid=self._choose_one_card("Add card to starter deck")
        if cid:
            deck.cards[cid]=max(1,self.workspace_controls["Starter decks"]["copies"].value())
            self._mark_dirty();self._fill_starter_cards();self._refresh_starter_decks()
    def _set_starter_copies(self):
        deck=self._current_starter()
        if deck is None:return
        copies=self.workspace_controls["Starter decks"]["copies"].value()
        table=self.workspace_controls["Starter decks"]["cards"]
        for row in sorted({x.row() for x in table.selectedItems()}):
            cid=table.item(row,0).data(Qt.ItemDataRole.UserRole)
            if cid:
                if copies:deck.cards[cid]=copies
                else:deck.cards.pop(cid,None)
        self._mark_dirty();self._fill_starter_cards();self._refresh_starter_decks()
    def _remove_starter_cards(self):
        deck=self._current_starter()
        if deck is None:return
        table=self.workspace_controls["Starter decks"]["cards"]
        for row in sorted({x.row() for x in table.selectedItems()}):
            cid=table.item(row,0).data(Qt.ItemDataRole.UserRole)
            if cid:deck.cards.pop(cid,None)
        self._mark_dirty();self._fill_starter_cards();self._refresh_starter_decks()
    def goto_starter_pool(self, index):
        """Starter decks: the Weighted pools tab, showing one pool."""
        controls = self.workspace_controls.get("Starter decks")
        if not controls or "pools" not in controls:
            return
        controls["starter_tabs"].setCurrentIndex(1)
        table = controls["pools"]
        if isinstance(index, int) and 0 <= index < table.rowCount():
            self.starter_pool_current = index
            table.selectRow(index)
            table.scrollToItem(table.item(index, 0))
