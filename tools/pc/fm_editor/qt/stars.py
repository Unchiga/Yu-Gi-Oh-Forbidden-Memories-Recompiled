"""The Guardian Stars page (a port of guardian_stars_tab.GuardianStarsTab)."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class StarsMixin:
    def _build_stars_page(self, page, get, controls):
        table = get(QTableWidget, "starTable")
        matrix = get(QTableWidget, "matrixTable")
        value = get(QSpinBox, "matrixValueSpin")
        value.setRange(-guardian_stars.BONUS_MAX, guardian_stars.BONUS_MAX)
        bonus = get(QSpinBox, "defaultBonusSpin")
        bonus.setRange(-guardian_stars.BONUS_MAX, guardian_stars.BONUS_MAX)
        bonus.setValue(guardian_stars.RETAIL_BONUS)
        palette = get(QComboBox, "starPaletteCombo")
        palette.addItem("game — the disc's stars' 16 colours", "game")
        palette.addItem("own — the PNG's own colours", "own")
        choice = get(QComboBox, "starChoiceCombo")
        for key, text in (("ask", "ask — the SELECT A GUARDIAN STAR box (the disc's)"),
                          ("first", "first — always the first star"),
                          ("best", "best — the one the matchups favour")):
            choice.addItem(text, key)
        default_action = get(QComboBox, "matrixDefaultCombo")
        default_action.addItem("Default…", None)
        default_action.addItem("+ default", 1)
        default_action.addItem("− default", -1)
        controls.update(stars=table, matrix=matrix, value=value, bonus=bonus,
                        palette=palette, choice=choice, default_action=default_action,
                        name=get(QLineEdit, "starNameEdit"),
                        languages=get(QLineEdit, "starLanguagesEdit"),
                        names=get(QLineEdit, "starNamesEdit"),
                        icon=get(QLabel, "starIconPreview"), star_id=get(QLabel, "starIdValue"),
                        cell_label=get(QLabel, "matrixCellLabel"),
                        mirror=get(QCheckBox, "matrixMirrorCheck"),
                        status=get(QLabel, "starsStatusLabel"),
                        list_title=get(QLabel, "starListTitle"),
                        advanced=get(QCheckBox, "starsAdvancedCheck"))
        self.stars_model = guardian_stars.Stars()
        self.stars_selected = 1
        self.stars_cell = None
        self.stars_filling = False
        self.stars_icons = {}

        advanced_panel = get(QWidget, "starAdvancedPanel")
        advanced_panel.setVisible(False)
        controls["advanced"].toggled.connect(advanced_panel.setVisible)
        table.setColumnCount(4)
        table.itemSelectionChanged.connect(self._pick_star)
        matrix.horizontalHeader().setVisible(True)
        matrix.verticalHeader().setVisible(True)
        matrix.cellClicked.connect(self._pick_star_cell)

        for title in ("pageTitleLabel", "starListTitle", "selectedStarTitle", "matrixTitle", "defaultsTitle"):
            get(QLabel, title).setStyleSheet(
                "font-size:20px;font-weight:650;color:#f3f7fc" if title == "pageTitleLabel"
                else "font-size:16px;font-weight:650;color:#f3f7fc")
        for muted in ("pageSummaryLabel", "defaultBonusHint", "starNamesHint", "matrixCellLabel",
                      "starsStatusLabel"):
            get(QLabel, muted).setStyleSheet("color:#9aacc4")
        for panel in ("starListPanel", "selectedStarPanel", "matrixPanel", "defaultsPanel"):
            get(QFrame, panel).setStyleSheet(
                f"QFrame#{panel} {{ background:#101b2b; border:1px solid #26374c; border-radius:10px; }}")
        get(QLabel, "starIconPreview").setStyleSheet(
            "background:#16212f;border:1px solid #26374c;border-radius:8px;color:#7f8ca0")
        get(QPushButton, "applyStarsButton").setStyleSheet(
            "background:#216cf1;border-color:#216cf1;color:white;font-weight:600")
        panels = get(QSplitter, "starsSplitter")
        page_layout = page.layout()
        for index in range(page_layout.count()):
            page_layout.setStretch(index, 1 if page_layout.itemAt(index).widget() is panels else 0)
        panels.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        for label in ("pageTitleLabel", "pageSummaryLabel", "starsStatusLabel"):
            get(QLabel, label).setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Minimum)
        panels.setStretchFactor(0, 3)
        panels.setStretchFactor(1, 7)
        panels.setSizes([430, 1050])
        get(QWidget, "starsLeftColumn").setMinimumWidth(380)

        controls["name"].editingFinished.connect(self._set_star_name)
        controls["languages"].editingFinished.connect(self._set_star_name)
        palette.currentIndexChanged.connect(self._set_star_palette)
        choice.currentIndexChanged.connect(self._set_star_choice)
        bonus.editingFinished.connect(self._set_star_default)
        controls["names"].editingFinished.connect(self._set_star_names)
        value.editingFinished.connect(self._set_star_cell)
        get(QPushButton, "matrixSetButton").clicked.connect(self._set_star_cell)
        get(QPushButton, "matrixClearButton").clicked.connect(lambda: self._set_star_cell(0))
        default_action.activated.connect(self._apply_star_default_action)
        for key, slot in (("addStarButton", self._add_star), ("duplicateStarButton", self._duplicate_star),
                          ("removeStarButton", self._remove_star),
                          ("importIconButton", self._import_star_icon),
                          ("removeIconButton", self._remove_star_icon),
                          ("retailCyclesButton", self._stars_preset_retail),
                          ("clearAllButton", self._stars_preset_clear),
                          ("starRulesButton", self._open_star_rules),
                          ("resetNamesButton", self._reset_star_names),
                          ("resetAllStarsButton", self._stars_preset_clear),
                          ("revertStarsButton", self._revert_stars),
                          ("applyStarsButton", self._apply_stars)):
            get(QPushButton, key).clicked.connect(slot)
    def _stars_default(self) -> int:
        model = self.stars_model
        return guardian_stars.RETAIL_BONUS if model.default_bonus is None else model.default_bonus
    def _star_card_counts(self) -> dict:
        counts = {}
        for card in self.project.cards.values():
            if card.is_monster():
                for star in (card.star1, card.star2):
                    counts[star] = counts.get(star, 0) + 1
        return counts
    def _refresh_stars(self):
        c = self.workspace_controls.get("Guardian Stars")
        if not c or self.project is None:
            return
        self.stars_model = guardian_stars.read(self.project.other.get("guardian_stars"))
        self.stars_icons = {}
        self.stars_cell = None
        self.stars_filling = True
        try:
            c["bonus"].setValue(self._stars_default())
            c["choice"].setCurrentIndex(max(0, c["choice"].findData(self.stars_model.choice or "ask")))
            c["names"].setText(", ".join(self.stars_model.name(star)
                                         for star in range(1, self.stars_model.count + 1)))
        finally:
            self.stars_filling = False
        self._fill_stars()
    def _fill_stars(self):
        c = self.workspace_controls["Guardian Stars"]
        model, table = self.stars_model, c["stars"]
        counts = self._star_card_counts()
        table.blockSignals(True)
        held = sort_paused(table)
        table.setRowCount(0)
        for star in range(1, model.count + 1):
            entry = model.stars.get(star)
            row = table.rowCount()
            table.insertRow(row)
            state = "added" if star > guardian_stars.RETAIL_COUNT else "changed" if entry else ""
            icon = "mod's" if entry and entry.icon else \
                ("disc's" if star <= guardian_stars.RETAIL_COUNT else "plain")
            for column, text in enumerate((str(star), model.name(star), icon, str(counts.get(star, 0)))):
                item = TableItem(text)
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, star)
                self._tint_state(item, state)
                table.setItem(row, column, item)
            picture = self._star_icon_pixmap(star)
            if picture is not None:
                table.item(row, 1).setIcon(picture)
        sort_resumed(table, held)
        table.blockSignals(False)
        header = table.horizontalHeader()
        for column, width in ((0, 42), (2, 74), (3, 62)):
            header.setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
            header.resizeSection(column, width)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        c["list_title"].setText(f"Guardian stars ({model.count})")
        self._select_star_row(self.stars_selected)
        self._draw_star_matrix()
        self._stars_report()
    def _select_star_row(self, star):
        table = self.workspace_controls["Guardian Stars"]["stars"]
        for row in range(table.rowCount()):
            if table.item(row, 0).data(Qt.ItemDataRole.UserRole) == star:
                table.blockSignals(True)
                table.selectRow(row)
                table.blockSignals(False)
                self.stars_selected = star      # the form edits this star from now on
                break
        self._show_star(self.stars_selected)
    def _pick_star(self):
        table = self.workspace_controls["Guardian Stars"]["stars"]
        row = table.currentRow()
        if row < 0:
            return
        star = table.item(row, 0).data(Qt.ItemDataRole.UserRole)
        if star is not None:
            self.stars_selected = star
            self._show_star(star)
    def _show_star(self, star):
        c = self.workspace_controls["Guardian Stars"]
        model = self.stars_model
        entry = model.stars.get(star)
        name = entry.name if entry else None
        self.stars_filling = True
        try:
            c["name"].setText(name if isinstance(name, str) else guardian_stars.display_name(name, star))
            c["languages"].setText(", ".join(f"{k}={v}" for k, v in name.items() if isinstance(v, str))
                                   if isinstance(name, dict) else "")
            c["palette"].setCurrentIndex(max(0, c["palette"].findData(
                entry.palette if entry and entry.palette else "game")))
            c["star_id"].setText(str(star))
        finally:
            self.stars_filling = False
        picture = self._star_icon_pixmap(star, 64)
        if picture is None:
            c["icon"].setPixmap(QPixmap())
            c["icon"].setText("disc's" if star <= guardian_stars.RETAIL_COUNT else "no icon")
        else:
            c["icon"].setPixmap(picture)
            c["icon"].setText("")
    def _forget_star_icon(self, star):
        """Both cached shapes of a star's icon: the mod's and the disc's."""
        for cached in (True, False):
            self.stars_icons.pop((star, cached), None)
    def _star_icon_pixmap(self, star, side=24):
        """A star's icon: the mod's PNG where it has one, else the disc's own
        symbol out of the boot sheet (guardian_stars.disc_icon)."""
        entry = self.stars_model.stars.get(star)
        key = (star, bool(entry and entry.icon))
        if key not in self.stars_icons:
            image = None
            if entry and entry.icon:
                try:
                    if entry.icon in self.project.files:
                        image = pngio.decode(self.project.files[entry.icon])
                    elif self.project.source_dir:
                        image = pngio.read(Path(self.project.source_dir) / entry.icon)
                except (OSError, pngio.PngError, ValueError):
                    image = None
            if image is None and self.files is not None:
                try:
                    found = guardian_stars.disc_icon(self.files.wa, star)
                except (struct.error, IndexError, ValueError):
                    found = None
                if found is not None:
                    image = pngio.Image(found[0], found[1], found[2])
            self.stars_icons[key] = image
        image = self.stars_icons[key]
        if image is None:
            return None
        if (image.width, image.height) != (side, side):
            image = pngio.resample(image, side, side)
        return QPixmap.fromImage(_qimage(image.width, image.height, image.rgba))
    def _draw_star_matrix(self):
        c = self.workspace_controls["Guardian Stars"]
        model, matrix = self.stars_model, c["matrix"]
        n = model.count
        matrix.blockSignals(True)
        matrix.setRowCount(n)
        matrix.setColumnCount(n)
        names = [model.name(star) for star in range(1, n + 1)]
        matrix.setHorizontalHeaderLabels(names)
        matrix.setVerticalHeaderLabels([f"{star} {names[star - 1]}" for star in range(1, n + 1)])
        for star in range(1, n + 1):
            picture = self._star_icon_pixmap(star, 20)
            for header in (matrix.horizontalHeaderItem(star - 1), matrix.verticalHeaderItem(star - 1)):
                if picture is not None and header is not None:
                    header.setIcon(picture)
        for a in range(1, n + 1):
            for d in range(1, n + 1):
                bonus = model.grid[a][d]
                item = TableItem(f"{bonus:+}" if bonus else "-")
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                kind = "plus" if bonus > 0 else "minus" if bonus < 0 else "zero"
                item.setBackground(QColor(self.STAR_CELL_COLOURS[kind]))
                if bonus and bonus != guardian_stars.retail_matchup(a, d):
                    font = item.font()
                    font.setBold(True)                  # changed from the disc's table
                    item.setFont(font)
                matrix.setItem(a - 1, d - 1, item)
        matrix.blockSignals(False)
        # Stretch, so the grid fills its panel however many stars there are;
        # setDefaultSectionSize would only reach columns made after it.
        matrix.horizontalHeader().setMinimumSectionSize(62)
        matrix.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        matrix.verticalHeader().setDefaultSectionSize(36)
        for row in range(n):
            matrix.setRowHeight(row, 36)
        if self.stars_cell:
            a, d = self.stars_cell
            if a <= n and d <= n:
                matrix.setCurrentCell(a - 1, d - 1)
    def _pick_star_cell(self, row, column):
        c = self.workspace_controls["Guardian Stars"]
        model = self.stars_model
        a, d = row + 1, column + 1
        if not (1 <= a <= model.count and 1 <= d <= model.count):
            return
        self.stars_cell = (a, d)
        c["cell_label"].setText(f"{model.name(a)} attacking {model.name(d)}:")
        self.stars_filling = True
        try:
            c["value"].setValue(model.grid[a][d])
        finally:
            self.stars_filling = False
    def _set_star_cell(self, bonus=None):
        c = self.workspace_controls["Guardian Stars"]
        if not self.stars_cell or self.stars_filling:
            return
        if bonus is None or bonus is False:
            bonus = c["value"].value()
        a, d = self.stars_cell
        self.stars_model.grid[a][d] = bonus
        if c["mirror"].isChecked() and a != d:
            self.stars_model.grid[d][a] = -bonus
        self.stars_filling = True
        try:
            c["value"].setValue(bonus)
        finally:
            self.stars_filling = False
        self._draw_star_matrix()
        self._commit_stars()
    def _apply_star_default_action(self, index):
        c = self.workspace_controls["Guardian Stars"]
        sign = c["default_action"].itemData(index)
        c["default_action"].setCurrentIndex(0)
        if sign:
            self._set_star_cell(sign * self._stars_default())
    def _star_entry(self, star):
        return self.stars_model.stars.setdefault(star, guardian_stars.Star(star))
    def _tidy_star(self, star):
        """A disc star the mod says nothing of any more is not declared."""
        entry = self.stars_model.stars.get(star)
        if entry and star <= guardian_stars.RETAIL_COUNT and entry.name in (None, "", {}) \
                and not entry.icon and not entry.palette and not entry.extra:
            del self.stars_model.stars[star]
    def _set_star_name(self):
        if self.stars_filling:
            return
        c = self.workspace_controls["Guardian Stars"]
        star = self.stars_selected
        text = c["name"].text().strip()
        languages = {}
        for part in c["languages"].text().split(","):
            if "=" in part:
                key, _, written = part.partition("=")
                if key.strip() and written.strip():
                    languages[key.strip()] = written.strip()
        default = guardian_stars.display_name(None, star)
        if languages:
            if text:
                languages["default"] = text
            name = languages
        else:
            name = text if text and text != default else None
        entry = self.stars_model.stars.get(star)
        if (entry.name if entry else None) == name:
            return
        if name is None and star > guardian_stars.RETAIL_COUNT:
            self._star_entry(star).name = None
        elif name is not None or entry:
            self._star_entry(star).name = name
        self._tidy_star(star)
        self._fill_stars()
        self._commit_stars()
    def _set_star_palette(self):
        if self.stars_filling:
            return
        c = self.workspace_controls["Guardian Stars"]
        entry = self._star_entry(self.stars_selected)
        entry.palette = None if c["palette"].currentData() == "game" else c["palette"].currentData()
        self._tidy_star(self.stars_selected)
        self._commit_stars()
    def _set_star_choice(self):
        if self.stars_filling:
            return
        c = self.workspace_controls["Guardian Stars"]
        picked = c["choice"].currentData()
        self.stars_model.choice = None if picked == "ask" else picked
        self._commit_stars()
    def _set_star_default(self):
        if self.stars_filling:
            return
        c = self.workspace_controls["Guardian Stars"]
        bonus = c["bonus"].value()
        if bonus == guardian_stars.RETAIL_BONUS:
            bonus = None
        if bonus == self.stars_model.default_bonus:
            return
        self.stars_model.set_default(bonus)
        self._draw_star_matrix()
        self._commit_stars()
    def _set_star_names(self):
        """The whole list at once, star 1 first."""
        if self.stars_filling:
            return
        c = self.workspace_controls["Guardian Stars"]
        written = [part.strip() for part in c["names"].text().split(",")]
        for star in range(1, self.stars_model.count + 1):
            if star > len(written):
                break
            text = written[star - 1]
            default = guardian_stars.display_name(None, star)
            entry = self.stars_model.stars.get(star)
            if isinstance(entry.name if entry else None, dict):
                continue            # a name by language is left to its own box
            name = text if text and text != default else None
            if name is None and star <= guardian_stars.RETAIL_COUNT:
                if entry:
                    entry.name = None
                    self._tidy_star(star)
            else:
                self._star_entry(star).name = name
        self._fill_stars()
        self._commit_stars()
    def _reset_star_names(self):
        for star, entry in list(self.stars_model.stars.items()):
            entry.name = None
            self._tidy_star(star)
        self._refresh_stars_names_box()
        self._fill_stars()
        self._commit_stars()
    def _refresh_stars_names_box(self):
        c = self.workspace_controls["Guardian Stars"]
        self.stars_filling = True
        try:
            c["names"].setText(", ".join(self.stars_model.name(star)
                                         for star in range(1, self.stars_model.count + 1)))
        finally:
            self.stars_filling = False
    def _add_star(self):
        star = self.stars_model.add_star()
        if not star:
            QMessageBox.information(self, "Guardian Stars",
                                    "All 15 stars are there: a card holds its stars in 4 bits, so a "
                                    "sixteenth would need a wider card record.")
            return star
        self.stars_selected = star
        self._refresh_stars_names_box()
        self._fill_stars()
        self._commit_stars()
        return star
    def _duplicate_star(self):
        source = self.stars_model.stars.get(self.stars_selected)
        star = self._add_star()
        if not star:
            return
        if source is not None:
            entry = self._star_entry(star)
            entry.name = copy.deepcopy(source.name)
            entry.icon = source.icon
            entry.palette = source.palette
            entry.extra = copy.deepcopy(source.extra)
            self._refresh_stars_names_box()
            self._fill_stars()
            self._commit_stars()
    def _remove_star(self):
        star = self.stars_selected
        if not star or star not in self.stars_model.stars:
            return
        in_use = self._star_card_counts().get(star, 0)
        if star > guardian_stars.RETAIL_COUNT and in_use:
            answer = QMessageBox.question(
                self, "Guardian Stars",
                f"{in_use} card stars are {self.stars_model.name(star)}; they keep the number {star}, "
                "which the game then warns about. Remove it?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
        if self.stars_model.stars[star].icon:
            self.project.files.pop(self.stars_model.stars[star].icon, None)
        self.stars_model.remove_star(star)
        self._forget_star_icon(star)
        self.stars_selected = min(star, self.stars_model.count)
        self._refresh_stars_names_box()
        self._fill_stars()
        self._commit_stars()
    def use_star_icon(self, star, path) -> bool:
        """The PNG at `path` as `star`'s icon (the import without the dialog)."""
        data = Path(path).read_bytes()
        pngio.decode(data)              # a PNG it can read, or PngError
        name = f"icons/star-{star}.png"
        self.project.files[name] = data
        self._star_entry(star).icon = name
        self._forget_star_icon(star)
        self._fill_stars()
        self._commit_stars()
        return True
    def _import_star_icon(self):
        star = self.stars_selected
        if not star:
            return
        path, _ = QFileDialog.getOpenFileName(self, "A guardian star's icon", "",
                                              "PNG images (*.png);;All files (*)")
        if not path:
            return
        try:
            self.use_star_icon(star, path)
        except (OSError, pngio.PngError, ValueError) as problem:
            QMessageBox.critical(self, "Guardian Stars", f"Could not read {path}: {problem}")
    def _remove_star_icon(self):
        star = self.stars_selected
        entry = self.stars_model.stars.get(star) if star else None
        if not entry or not entry.icon:
            return
        self.project.files.pop(entry.icon, None)
        entry.icon = None
        self._forget_star_icon(star)
        self._tidy_star(star)
        self._fill_stars()
        self._commit_stars()
    def _stars_preset_retail(self):
        self.stars_model.preset_retail()
        self._draw_star_matrix()
        self._commit_stars()
    def _stars_preset_clear(self):
        self.stars_model.preset_clear()
        self._draw_star_matrix()
        self._commit_stars()
    def _revert_stars(self):
        answer = QMessageBox.question(
            self, "Guardian Stars",
            "Put every star, name, icon and matchup back as the disc has them?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        for entry in self.stars_model.stars.values():
            if entry.icon:
                self.project.files.pop(entry.icon, None)
        self.project.other.pop("guardian_stars", None)
        self._mark_dirty()
        self._refresh_stars()
    def _open_star_rules(self):
        """Set many cards' stars at once: the chosen cards get a star from
        their attribute or their monster type (star_rules.py)."""
        from .. import star_rules
        dialog = QDialog(self)
        dialog.setWindowTitle("Set cards' stars by rule")
        dialog.resize(760, 640)
        layout = QVBoxLayout(dialog)

        top = QFormLayout()
        source = QComboBox()
        for key, text in (("attribute", "the card's attribute"), ("type", "the card's monster type"),
                          ("star", "nothing: one star for every card")):
            source.addItem(text, key)
        which = QComboBox()
        for key, text in (("first", "the first star"), ("second", "the second star"),
                          ("both", "both stars")):
            which.addItem(text, key)
        top.addRow("Pick the star from", source)
        top.addRow("Set", which)
        layout.addLayout(top)

        # The cards: a filter, as the Tk dialog's panel gives. An empty one is
        # refused by star_rules.plan, so at least one of these must be set.
        chooser = QGroupBox("Cards (every filter given must hold)")
        chooser_layout = QHBoxLayout(chooser)
        types = self._checkable_list(TYPE_NAMES[:20], chooser, height=6)
        attributes = self._checkable_list(ATTRIBUTE_NAMES, chooser, height=6)
        for listing, title in ((types, "Monster type"), (attributes, "Attribute")):
            column = QVBoxLayout()
            caption = QLabel(title)
            caption.setStyleSheet("color:#9aacc4")
            column.addWidget(caption)
            column.addWidget(listing)
            holder = QWidget()
            holder.setLayout(column)
            chooser_layout.addWidget(holder)
        cards_column = QVBoxLayout()
        cards_caption = QLabel("Cards (numbers, 10-20, names)")
        cards_caption.setStyleSheet("color:#9aacc4")
        cards = QLineEdit()
        all_monsters = QCheckBox("Every monster")
        cards_column.addWidget(cards_caption)
        cards_column.addWidget(cards)
        cards_column.addWidget(all_monsters)
        cards_column.addStretch(1)
        cards_holder = QWidget()
        cards_holder.setLayout(cards_column)
        chooser_layout.addWidget(cards_holder)
        layout.addWidget(chooser)

        mapping_box = QGroupBox("The star each one gets")
        mapping_layout = QFormLayout(mapping_box)
        layout.addWidget(mapping_box)
        combos = {}

        def star_combo():
            box = QComboBox()
            box.addItem("(none)", 0)
            for number in range(1, self.stars_model.count + 1):
                box.addItem(f"{number} {self.stars_model.name(number)}", number)
            box.currentIndexChanged.connect(lambda *_: show())
            return box

        def rebuild_mapping():
            while mapping_layout.rowCount():
                mapping_layout.removeRow(0)
            combos.clear()
            kind = source.currentData()
            if kind == "star":
                keys = [(None, "Every chosen card")]
            elif kind == "attribute":
                keys = list(enumerate(ATTRIBUTE_NAMES))
            else:
                keys = list(enumerate(TYPE_NAMES[:20]))
            for key, title in keys:
                box = star_combo()
                combos[key] = box
                mapping_layout.addRow(title, box)

        preview = QPlainTextEdit()
        preview.setReadOnly(True)
        layout.addWidget(preview, 1)
        buttons = QHBoxLayout()
        apply_button = QPushButton("Apply…")
        apply_button.setStyleSheet("background:#216cf1;border-color:#216cf1;color:white;font-weight:600")
        undo_button = QPushButton("Undo last batch")
        undo_button.setEnabled(getattr(self, "star_rules_batch", None) is not None)
        close = QPushButton("Close")
        buttons.addWidget(undo_button)
        buttons.addStretch(1)
        buttons.addWidget(apply_button)
        buttons.addWidget(close)
        layout.addLayout(buttons)
        close.clicked.connect(dialog.accept)

        def spec():
            card_filter = bulk_fusions.CardFilter(
                kinds={"monster"} if all_monsters.isChecked() else set(),
                types={item.data(Qt.ItemDataRole.UserRole) for item in self._checked_items(types)},
                attributes={item.data(Qt.ItemDataRole.UserRole) for item in self._checked_items(attributes)},
                cards=cards.text())
            mapping = {key: box.currentData() for key, box in combos.items() if box.currentData()}
            return star_rules.RuleSpec(filter=card_filter, which=which.currentData(),
                                       source=source.currentData(), mapping=mapping)

        def show(*_):
            try:
                plan = star_rules.plan(self.project, spec(), self.stars_model.count)
            except (ValueError, KeyError) as problem:
                preview.setPlainText(str(problem))
                apply_button.setEnabled(False)
                return None
            if plan.errors:
                preview.setPlainText("\n".join(plan.errors))
                apply_button.setEnabled(False)
                return plan
            lines = [plan.summary(), ""]
            for cid, before, after in plan.changes[:star_rules.SAMPLE]:
                lines.append(f"{self.project.card_label(cid)}:  {before[0]}, {before[1]}"
                             f"  →  {after[0]}, {after[1]}")
            if len(plan.changes) > star_rules.SAMPLE:
                lines.append(f"… and {len(plan.changes) - star_rules.SAMPLE} more")
            preview.setPlainText("\n".join(lines))
            apply_button.setEnabled(plan.ok())
            return plan

        def run():
            plan = show()
            if plan is None or not plan.ok():
                return
            answer = QMessageBox.question(
                dialog, "Set stars by rule",
                f"{plan.summary()}\n\nChange {len(plan.changes)} cards? "
                "\"Undo last batch\" puts them back.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
            self.star_rules_batch = star_rules.apply(self.project, plan, "stars by rule")
            undo_button.setEnabled(True)
            self._mark_dirty()
            self.refresh_cards(select_id=self.current)
            self._fill_stars()
            show()

        def undo():
            batch = getattr(self, "star_rules_batch", None)
            if batch is None:
                return
            restored, skipped = star_rules.undo(self.project, batch)
            self.star_rules_batch = None
            undo_button.setEnabled(False)
            self._mark_dirty()
            self.refresh_cards(select_id=self.current)
            self._fill_stars()
            show()
            message = f"{restored} cards put back."
            if skipped:
                message += f" {skipped} changed since are left as they are."
            QMessageBox.information(dialog, "Set stars by rule", message)

        source.currentIndexChanged.connect(lambda *_: (rebuild_mapping(), show()))
        which.currentIndexChanged.connect(show)
        cards.textChanged.connect(show)
        all_monsters.toggled.connect(show)
        for listing in (types, attributes):
            listing.itemChanged.connect(show)
        apply_button.clicked.connect(run)
        undo_button.clicked.connect(undo)
        rebuild_mapping()
        show()
        dialog.exec()
    def _commit_stars(self):
        """The model into the project, as GuardianStarsTab.commit does."""
        if self.project is None:
            return True
        before = self.project.other.get("guardian_stars")
        after = self.stars_model.build()
        # Untouched, a section as the mod wrote it ("beats", "mirror", stars by
        # name) stays so, rather than being rewritten as the model's matchups.
        if after == guardian_stars.read(before).build():
            after = before
        if after is None:
            self.project.other.pop("guardian_stars", None)
        else:
            self.project.other["guardian_stars"] = after
        if after != before:
            self._mark_dirty()
        self._stars_report()
        return True
    def _stars_report(self):
        c = self.workspace_controls["Guardian Stars"]
        problems = guardian_stars.check(self.project.other.get("guardian_stars"),
                                        card_stars=self._star_card_counts()) if self.project else []
        c["status"].setText("\n".join(f"{level}: {where}: {message}"
                                      for level, where, message in problems[:8]))
        c["status"].setStyleSheet("color:#ff7777" if any(p[0] == "error" for p in problems)
                                  else "color:#f2c04c" if problems else "color:#9aacc4")
        return problems
    def _apply_stars(self):
        self._commit_stars()
        problems = self._stars_report()
        errors = [p for p in problems if p[0] == "error"]
        c = self.workspace_controls["Guardian Stars"]
        if not problems:
            c["status"].setText("Guardian stars applied; the loader would take all of them.")
            c["status"].setStyleSheet("color:#9aacc4")
        return not errors
