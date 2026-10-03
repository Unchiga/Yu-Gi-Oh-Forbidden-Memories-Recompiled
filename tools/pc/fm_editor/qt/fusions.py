"""The Fusions page and its bulk dialog (bulk_dialog.py)."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class FusionsMixin:
    def _set_fusion_view(self, mode):
        controls = self.workspace_controls.get("Fusions")
        if not controls:
            return
        image_list = controls["image_list"]
        controls["stack"].setCurrentWidget(image_list)
        grid = int(mode) == 0
        image_list.setViewMode(QListView.ViewMode.IconMode if grid else QListView.ViewMode.ListMode)
        image_list.setFlow(QListView.Flow.LeftToRight if grid else QListView.Flow.TopToBottom)
        image_list.setWrapping(grid)
        image_list.setResizeMode(QListView.ResizeMode.Adjust)
        image_list.setMovement(QListView.Movement.Static)
        image_list.setSpacing(10 if grid else 4)
        image_list.setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        image_list.verticalScrollBar().setSingleStep(96 if grid else 52)
        if grid:
            width = max(420, (image_list.viewport().width() - 28) // 3)
            image_list.setGridSize(QSize(width, 190))
        else:
            image_list.setGridSize(QSize())
        controls["header"].setVisible(not grid)
        image_list.doItemsLayout()
        self._sync_fusion_header()
    def _sync_fusion_header(self):
        """Match the Designer header columns to the image-row delegate geometry."""
        controls = self.workspace_controls.get("Fusions")
        if not controls:
            return
        image_list = controls["image_list"]
        header = controls["header"]
        layout = header.layout()
        if layout is None:
            return

        row_width = max(1, image_list.viewport().width())
        # Reproduce FusionImageDelegate.paint()'s column widths exactly.
        name_width = max(210, row_width - 478) // 3
        widths = (80 + name_width, 80 + name_width, 52 + name_width, 70, 70, 84)

        # The header sits outside the list viewport, so account for the frame and
        # vertical scrollbar that reduce the actual row width.
        viewport_inset = max(0, header.width() - row_width)
        margins = layout.contentsMargins()
        layout.setContentsMargins(12, margins.top(), 12 + viewport_inset,
                                  margins.bottom())
        for column, width in enumerate(widths):
            controls["sort_headers"][column].setFixedWidth(width)
    def _refresh_fusions(self, *_):
        c = self.workspace_controls.get("Fusions")
        if not c: return
        table, image_list, p = c["table"], c["image_list"], self.project
        query = c["search"].text().strip().casefold()
        changed_only = c["changed"].isChecked()
        rows = []
        named = {pair for pair in p.own_fusion_pairs()[0] if all(cid in p.cards for cid in pair)}
        removes = set(p.active_removes())
        own = {pair: p.own_fusion(pair, removes) for pair in named}
        pairs = set(p.fusions) | set(p.retail.fusions) | p.fusion_explicit | {
            pair for pair, result in own.items() if result is not None}
        outcomes = {}
        for pair in sorted(pairs):
            status = "own list" if own.get(pair) is not None else p.fusion_status(pair)
            result = own[pair] if status == "own list" else p.fusions.get(pair)
            outcomes[pair] = result
            a, b = p.card_label(pair[0]), p.card_label(pair[1])
            retail = p.retail.fusions.get(pair)
            outcome = p.card_label(result) if result else (
                f"No fusion (retail: {p.card_label(retail)})" if retail else "No fusion (forbidden)")
            if status == "own list":
                outcome += " (a card’s own fusions list)"
            if changed_only and status in ("", "glitch"):
                continue
            if query:
                materials_match = any(query in text.casefold() for text in (a, b))
                result_match = query in outcome.casefold()
                if not (materials_match or result_match if c["search_both"].isChecked() else
                        result_match if c["search_results"].isChecked() else materials_match):
                    continue
            result_card = p.cards.get(result)
            rows.append((pair, a, b, outcome, status or "Stock",
                         result_card.attack if result_card else 0,
                         result_card.defense if result_card else 0))
        sort_column, ascending = c.get("sort", (0, True))
        sort_keys = (lambda row: row[0][0], lambda row: row[0][1],
                     lambda row: outcomes[row[0]] or 0,
                     lambda row: row[5], lambda row: row[6], lambda row: row[4].casefold())
        rows.sort(key=sort_keys[sort_column], reverse=not ascending)
        self._update_fusion_sort_headers()
        selected_pairs = [item.data(Qt.ItemDataRole.UserRole)
                          for item in image_list.selectedItems()]
        total = len(rows)
        rows = rows[:3000]
        table.setRowCount(len(rows))
        image_list.clear()
        for i, (pair, a, b, result, status, atk, defense) in enumerate(rows):
            for col, value in enumerate((a, b, result, atk, defense, status)):
                item = TableItem(str(value))
                if col == 0: item.setData(Qt.ItemDataRole.UserRole, pair)
                self._tint_state(item, status)
                table.setItem(i, col, item)
            outcome_id = outcomes[pair]
            row_item = QListWidgetItem(f"{a} + {b} → {result} · {status}")
            self._tint_state(row_item, status)
            row_item.setData(Qt.ItemDataRole.UserRole, pair)
            row_item.setData(int(Qt.ItemDataRole.UserRole) + 1, outcome_id)
            row_item.setData(int(Qt.ItemDataRole.UserRole) + 2, (a, b, result))
            row_item.setData(int(Qt.ItemDataRole.UserRole) + 3, status)
            row_item.setData(int(Qt.ItemDataRole.UserRole) + 4, atk)
            row_item.setData(int(Qt.ItemDataRole.UserRole) + 5, defense)
            ink = self._state_colour(status)
            row_item.setData(int(Qt.ItemDataRole.UserRole) + 6, ink.name() if ink else None)
            image_list.addItem(row_item)
            if pair in selected_pairs:
                row_item.setSelected(True)
        c["count"].setText(f"{total:,} fusion rules" + (" (first 3,000 shown; search to narrow)" if total > 3000 else ""))
    def _sort_fusions(self, column):
        controls = self.workspace_controls.get("Fusions")
        if not controls:
            return
        old_column, ascending = controls.get("sort", (0, True))
        controls["sort"] = (column, not ascending if column == old_column else True)
        self._refresh_fusions()
    def _update_fusion_sort_headers(self):
        controls = self.workspace_controls.get("Fusions")
        if not controls:
            return
        column, ascending = controls.get("sort", (0, True))
        titles = ("Card A", "Card B", "Fusion result", "ATK", "DEF", "State")
        for index, button in enumerate(controls["sort_headers"]):
            arrow = (" ↑" if ascending else " ↓") if index == column else ""
            button.setText(titles[index] + arrow)
    def _build_bulk_fusions_page(self, page, controls):
        """Bind the Generic Fusions Designer form to the fusion planner."""
        def get(cls, name):
            item = page.findChild(cls, name)
            if item is None:
                raise RuntimeError(f"Fusions Designer form is missing {name!r}")
            return item

        def bind_filter(side):
            cap = side.upper()
            types = get(QListWidget, f"material{cap}TypeList")
            stars = get(QListWidget, f"material{cap}StarList")
            self._populate_checkable_list(types, TYPE_NAMES[:20])
            self._populate_checkable_list(stars, guardian_stars.choices(self.project.other.get("guardian_stars"))[1:])
            for listing in (types, stars):
                listing.setSpacing(3)
                listing.viewport().installEventFilter(self)
            group = get(QGroupBox, f"material{cap}Group")
            advanced = get(QLayout, f"material{cap}AdvancedLayout")
            outer = group.layout()
            position = next(i for i in range(outer.count()) if outer.itemAt(i).layout() is advanced)
            advanced = outer.takeAt(position).layout()
            advanced.setParent(None)
            advanced_panel = QWidget(group)
            advanced_panel.setProperty("bulkAdvancedPanel", True)
            advanced_panel.setLayout(advanced)
            outer.insertWidget(position, advanced_panel)
            kinds = {}
            extras = QHBoxLayout()
            for kind in bulk_fusions.KINDS:
                box = QCheckBox(kind.capitalize())
                box.toggled.connect(self._schedule_bulk_fusion_plan)
                extras.addWidget(box)
                kinds[kind] = box
            group.layout().addLayout(extras)
            actions = QHBoxLayout()
            clear = QPushButton("Clear filters")
            listing = QPushButton("List matches…")
            actions.addWidget(clear)
            actions.addWidget(listing)
            group.layout().addLayout(actions)
            clear.clicked.connect(lambda: self._clear_bulk_filter(side))
            listing.clicked.connect(lambda: self._list_bulk_matches(side))
            return {
                "group": get(QGroupBox, f"material{cap}Group"),
                "kinds": kinds, "types": types, "stars": stars,
                "attributes": [get(QCheckBox, f"material{cap}Attribute{i}") for i in range(6)],
                "bounds": {
                    "atk_min": get(QLineEdit, f"material{cap}AtkMin"),
                    "atk_max": get(QLineEdit, f"material{cap}AtkMax"),
                    "def_min": get(QLineEdit, f"material{cap}DefMin"),
                    "def_max": get(QLineEdit, f"material{cap}DefMax"),
                },
                "name": get(QLineEdit, f"material{cap}Name"),
                "text": get(QLineEdit, f"material{cap}Text"),
                "cards": get(QLineEdit, f"material{cap}Cards"),
                "results_only": get(QCheckBox, f"material{cap}ResultsOnly"),
                "count": get(QLabel, f"material{cap}CountLabel"),
                "level_any": get(QRadioButton, f"material{cap}LevelAny"),
                "level_exact_radio": get(QRadioButton, f"material{cap}LevelExactRadio"),
                "level_range_radio": get(QRadioButton, f"material{cap}LevelRangeRadio"),
                "level_exact": get(QSpinBox, f"material{cap}LevelExact"),
                "level_min": get(QSpinBox, f"material{cap}LevelMin"),
                "level_max": get(QSpinBox, f"material{cap}LevelMax"),
                "advanced_toggle": get(QPushButton, f"material{cap}AdvancedToggle"),
                "advanced": get(QLayout, f"material{cap}AdvancedLayout"),
            }

        controls["bulk_filters"] = {"a": bind_filter("a"), "b": bind_filter("b")}
        get(QPushButton, "copyAToBButton").clicked.connect(lambda: self._copy_bulk_filter("a", "b"))
        get(QPushButton, "copyBToAButton").clicked.connect(lambda: self._copy_bulk_filter("b", "a"))

        choose_card = get(QRadioButton, "chooseFusionCardRadio")
        use_ladder = get(QRadioButton, "fusionLadderRadio")
        result_picker = get(QPushButton, "chooseFusionResultButton")
        ladder = get(QLineEdit, "fusionLadderCandidates")
        stronger = get(QCheckBox, "fusionStrongerCheck")
        allow_self = get(QCheckBox, "fusionAllowSelfCheck")
        overwrite = get(QCheckBox, "fusionOverwriteCheck")
        intro = get(QLabel, "genericFusionIntroLabel")
        intro.setStyleSheet("color:#9aacc4")
        summary = get(QLabel, "genericFusionSummaryLabel")
        summary.setStyleSheet("font-size:16px; font-weight:650")
        errors = get(QLabel, "genericFusionErrorsLabel")
        errors.setStyleSheet("color:#ff9292")
        warnings = get(QLabel, "genericFusionWarningsLabel")
        warnings.setStyleSheet("color:#9aacc4")
        for key in ("materialACountLabel", "materialBCountLabel"):
            get(QLabel, key).setStyleSheet("color:#9aacc4")
        preview = get(QTableWidget, "genericFusionPlanTable")
        preview.setAlternatingRowColors(True)
        preview.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        preview.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        preview.verticalHeader().setVisible(False)
        preview.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        apply = get(QPushButton, "applyBulkFusionsButton")
        apply.setObjectName("primary")
        undo = get(QPushButton, "undoBulkFusionsButton")
        splitter = get(QSplitter, "genericFusionSplitter")
        splitter.setChildrenCollapsible(False)
        splitter.setStretchFactor(0, 2)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([1400, 700])
        left_scroll = get(QScrollArea, "genericFusionLeftScroll")
        left_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        controls["bulk"] = {"choose_card": choose_card, "use_ladder": use_ladder,
                            "result_button": result_picker, "result_id": 0, "ladder": ladder,
                            "stronger": stronger, "allow_self": allow_self,
                            "overwrite": overwrite, "summary": summary, "errors": errors,
                            "warnings": warnings, "preview": preview, "apply": apply,
                            "undo": undo, "batch": None, "result_project": self.project}
        mode = QComboBox()
        mode.addItem("Add fusions", "add")
        mode.addItem("Take fusions away", "remove")
        reset_result = QPushButton("Clear result restriction")
        page.layout().insertWidget(0, mode)
        page.layout().insertWidget(1, reset_result)
        controls["bulk"]["mode"] = mode
        controls["bulk"]["reset_result"] = reset_result
        mode.currentIndexChanged.connect(self._bulk_mode_changed)
        reset_result.clicked.connect(self._clear_bulk_result)
        self._bulk_mode_changed()
        result_picker.clicked.connect(self._choose_bulk_result)
        for side in controls["bulk_filters"].values():
            for widget in side["attributes"]:
                widget.stateChanged.connect(self._schedule_bulk_fusion_plan)
            for widget in (side["name"], side["text"], side["cards"], *side["bounds"].values()):
                widget.textChanged.connect(self._schedule_bulk_fusion_plan)
            for widget in (side["types"], side["stars"]):
                widget.itemChanged.connect(self._schedule_bulk_fusion_plan)
            side["results_only"].stateChanged.connect(self._schedule_bulk_fusion_plan)
            for widget in (side["level_any"], side["level_exact_radio"], side["level_range_radio"]):
                widget.toggled.connect(self._schedule_bulk_fusion_plan)
            for widget in (side["level_exact"], side["level_min"], side["level_max"]):
                widget.valueChanged.connect(self._schedule_bulk_fusion_plan)
            self._set_layout_widgets_visible(side["advanced"], side["advanced_toggle"].isChecked())
            side["advanced_toggle"].toggled.connect(
                lambda visible, layout=side["advanced"]: self._set_layout_widgets_visible(layout, visible))
        ladder.textChanged.connect(self._schedule_bulk_fusion_plan)
        for widget in (choose_card, use_ladder, stronger, allow_self, overwrite):
            widget.toggled.connect(self._schedule_bulk_fusion_plan)
        apply.clicked.connect(lambda _checked=False: self._apply_bulk_fusions())
        undo.clicked.connect(self._undo_bulk_fusions)

        timer = QTimer(self)
        timer.setSingleShot(True)
        timer.setInterval(300)
        timer.timeout.connect(self._refresh_bulk_fusion_plan)
        controls["bulk_timer"] = timer
        self._refresh_bulk_fusion_plan()
    def _choose_bulk_result(self):
        controls = self.workspace_controls.get("Fusions")
        if not controls or "bulk" not in controls:
            return
        bulk = controls["bulk"]
        dialog = QDialog(self)
        dialog.setWindowTitle("Choose fusion result")
        dialog.setMinimumSize(420, 460)
        dialog.resize(480, 560)
        layout = QVBoxLayout(dialog)
        search = QLineEdit(dialog)
        search.setPlaceholderText("Search cards by name or ID…")
        listing = QListWidget(dialog)
        listing.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        current_row = -1
        for cid in sorted(self.project.cards):
            card = self.project.cards[cid]
            item = QListWidgetItem(f"{cid:03d}  {card.name}")
            item.setData(Qt.ItemDataRole.UserRole, cid)
            listing.addItem(item)
            if cid == bulk["result_id"]:
                current_row = listing.count() - 1
        if current_row >= 0:
            listing.setCurrentRow(current_row)

        def filter_results(text):
            query = text.strip().casefold()
            for row in range(listing.count()):
                item = listing.item(row)
                item.setHidden(query not in item.text().casefold())
            current = listing.currentItem()
            if current is None or current.isHidden():
                first = next((listing.item(row) for row in range(listing.count())
                              if not listing.item(row).isHidden()), None)
                listing.setCurrentItem(first) if first is not None else listing.setCurrentRow(-1)

        search.textChanged.connect(filter_results)
        search.returnPressed.connect(dialog.accept)
        layout.addWidget(search)
        layout.addWidget(listing, 1)
        buttons = QHBoxLayout()
        buttons.addStretch(1)
        cancel = QPushButton("Cancel", dialog)
        choose = QPushButton("Choose", dialog)
        choose.setObjectName("primary")
        buttons.addWidget(cancel)
        buttons.addWidget(choose)
        layout.addLayout(buttons)
        cancel.clicked.connect(dialog.reject)
        choose.clicked.connect(dialog.accept)
        listing.itemDoubleClicked.connect(lambda *_: dialog.accept())
        search.setFocus()
        if dialog.exec() == QDialog.DialogCode.Accepted and listing.currentItem() is not None:
            item = listing.currentItem()
            bulk["result_id"] = int(item.data(Qt.ItemDataRole.UserRole))
            bulk["result_button"].setText(item.text())
            bulk["choose_card"].setChecked(True)
            self._schedule_bulk_fusion_plan()
    def _read_bulk_filter(self, controls):
        numbers = {}
        for key, edit in controls["bounds"].items():
            value = edit.text().strip()
            try:
                numbers[key] = int(value) if value else None
            except ValueError:
                raise ValueError(f"{controls['group'].title()}: {key.replace('_', ' ')} must be a whole number") from None
        for label, low, high in (("ATK", "atk_min", "atk_max"), ("DEF", "def_min", "def_max")):
            if numbers[low] is not None and numbers[high] is not None and numbers[low] > numbers[high]:
                raise ValueError(f"{controls['group'].title()}: {label} minimum exceeds maximum")
        if controls["level_exact_radio"].isChecked():
            numbers["level_min"] = numbers["level_max"] = controls["level_exact"].value()
        elif controls["level_range_radio"].isChecked():
            numbers["level_min"] = controls["level_min"].value()
            numbers["level_max"] = controls["level_max"].value()
            if numbers["level_min"] > numbers["level_max"]:
                raise ValueError(f"{controls['group'].title()}: Level minimum exceeds maximum")
        else:
            numbers["level_min"] = numbers["level_max"] = None
        card_filter = bulk_fusions.CardFilter(
            kinds={kind for kind, box in controls["kinds"].items() if box.isChecked()},
            types={item.data(Qt.ItemDataRole.UserRole) for item in self._checked_items(controls["types"])},
            attributes={i for i, widget in enumerate(controls["attributes"]) if widget.isChecked()},
            stars={item.data(Qt.ItemDataRole.UserRole) + 1 for item in self._checked_items(controls["stars"])},
            name=controls["name"].text(), text=controls["text"].text(), cards=controls["cards"].text(),
            results_only=controls["results_only"].isChecked(), **numbers)
        return card_filter
    def _copy_bulk_filter(self, source, destination):
        controls = self.workspace_controls["Fusions"]["bulk_filters"]
        src, dst = controls[source], controls[destination]
        for kind, box in src["kinds"].items():
            dst["kinds"][kind].setChecked(box.isChecked())
        for mine, theirs in zip(dst["attributes"], src["attributes"]):
            mine.setChecked(theirs.isChecked())
        for key in ("name", "text", "cards"):
            dst[key].setText(src[key].text())
        for key in src["bounds"]:
            dst["bounds"][key].setText(src["bounds"][key].text())
        dst["level_any"].setChecked(src["level_any"].isChecked())
        dst["level_exact_radio"].setChecked(src["level_exact_radio"].isChecked())
        dst["level_range_radio"].setChecked(src["level_range_radio"].isChecked())
        dst["level_exact"].setValue(src["level_exact"].value())
        dst["level_min"].setValue(src["level_min"].value())
        dst["level_max"].setValue(src["level_max"].value())
        dst["results_only"].setChecked(src["results_only"].isChecked())
        for key in ("types", "stars"):
            for i in range(src[key].count()):
                dst[key].item(i).setCheckState(src[key].item(i).checkState())
    def _schedule_bulk_fusion_plan(self, *_):
        controls = self.workspace_controls.get("Fusions")
        if controls and "bulk_timer" in controls:
            controls["bulk_timer"].start()
    def _refresh_bulk_fusion_plan(self):
        controls = self.workspace_controls.get("Fusions")
        if not controls or "bulk" not in controls:
            return
        bulk = controls["bulk"]
        names = guardian_stars.choices(self.project.other.get("guardian_stars"))[1:]
        for side in controls["bulk_filters"].values():
            listing = side["stars"]
            if [listing.item(i).text() for i in range(listing.count())] != names:
                selected = {item.data(Qt.ItemDataRole.UserRole) for item in self._checked_items(listing)}
                listing.blockSignals(True)
                self._populate_checkable_list(listing, names)
                for i in selected:
                    if i < listing.count(): listing.item(i).setCheckState(Qt.CheckState.Checked)
                listing.blockSignals(False)
        if bulk["result_project"] is not self.project:
            bulk["result_id"] = 0
            bulk["result_button"].setText("Choose a result…")
            bulk["result_project"] = self.project
            bulk["batch"] = None
            bulk["undo"].setEnabled(False)
        try:
            a = self._read_bulk_filter(controls["bulk_filters"]["a"])
            b = self._read_bulk_filter(controls["bulk_filters"]["b"])
            ladder_mode = bulk["mode"].currentData() == "add" and bulk["use_ladder"].isChecked()
            spec = bulk_fusions.BulkSpec(
                a=a, b=b, mode=bulk["mode"].currentData(), result=0 if ladder_mode else int(bulk["result_id"] or 0),
                ladder=bulk["ladder"].text() if ladder_mode else "",
                stronger=bulk["stronger"].isChecked(), allow_self=bulk["allow_self"].isChecked(),
                overwrite=bulk["overwrite"].isChecked())
            plan = bulk_fusions.plan(self.project, spec)
        except ValueError as problem:
            bulk["summary"].setText("Preview needs valid criteria")
            bulk["errors"].setText(str(problem))
            bulk["errors"].setVisible(True)
            bulk["warnings"].clear()
            bulk["warnings"].setVisible(False)
            bulk["preview"].setRowCount(0)
            # Keep Apply clickable so users get the specific validation reason
            # in a dialog instead of a button that appears unresponsive.
            bulk["apply"].setEnabled(True)
            return
        for key, spec_filter in (("a", a), ("b", b)):
            results = bulk_fusions.fusion_results(self.project) if (a.results_only or b.results_only) else None
            chosen, _unknown = spec_filter.select(self.project, results)
            controls["bulk_filters"][key]["count"].setText(f"{len(chosen):,} cards match")
        bulk["summary"].setText(
            f"A {plan.a_count} · B {plan.b_count} · {plan.pairs:,} pairs  |  "
            f"+{plan.added} new · {plan.replaced} replaced · {plan.removed} removed · {plan.kept} kept  |  "
            f"{plan.rules_after:,} mod rules")
        bulk["summary"].setToolTip(plan.summary() + "\n" + plan.budget_line())
        bulk["errors"].setText("\n".join(plan.errors))
        bulk["errors"].setVisible(bool(plan.errors))
        bulk["warnings"].setText("\n".join(plan.warnings))
        bulk["warnings"].setVisible(bool(plan.warnings))
        table = bulk["preview"]
        held = sort_paused(table)
        table.setRowCount(0)
        label = lambda cid: self.project.card_label(cid) if cid else ("(forbidden)" if cid == 0 else "(none)")
        for pair, before, after, action in plan.samples[:bulk_fusions.SAMPLE]:
            row = table.rowCount()
            table.insertRow(row)
            after_label = "(no fusion)" if action == "remove" else label(after)
            values = (self.project.card_label(pair[0]), self.project.card_label(pair[1]), label(before),
                      after_label, action)
            for column, value in enumerate(values):
                table.setItem(row, column, TableItem(value))
        sort_resumed(table, held)
        bulk["apply"].setEnabled(True)
        bulk["apply"].setToolTip("Review the validation message before applying." if plan.errors
                                 else "Apply the previewed fusion changes to this mod.")
    def _apply_bulk_fusions(self):
        controls = self.workspace_controls.get("Fusions")
        if not controls:
            return
        self._refresh_bulk_fusion_plan()
        bulk = controls["bulk"]
        try:
            a = self._read_bulk_filter(controls["bulk_filters"]["a"])
            b = self._read_bulk_filter(controls["bulk_filters"]["b"])
            ladder_mode = bulk["mode"].currentData() == "add" and bulk["use_ladder"].isChecked()
            plan = bulk_fusions.plan(self.project, bulk_fusions.BulkSpec(
                a=a, b=b, mode=bulk["mode"].currentData(), result=0 if ladder_mode else int(bulk["result_id"] or 0),
                ladder=bulk["ladder"].text() if ladder_mode else "",
                stronger=bulk["stronger"].isChecked(), allow_self=bulk["allow_self"].isChecked(),
                overwrite=bulk["overwrite"].isChecked()))
        except ValueError as problem:
            QMessageBox.warning(self, "Bulk fusions", str(problem))
            return
        if not plan.ok():
            QMessageBox.warning(self, "Bulk fusions", "\n".join(plan.errors) or "There are no fusion changes to apply.")
            return
        answer = QMessageBox.question(self, "Apply bulk fusions",
            plan.summary() + "\n\n" + plan.budget_line() +
            f"\n\nApply this batch ({plan.added} new, {plan.replaced} replaced, {plan.removed} removed)?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        bulk["batch"] = bulk_fusions.apply(self.project, plan, f"bulk {plan.mode} fusions")
        bulk["undo"].setEnabled(True)
        self._mark_dirty()
        self._refresh_fusions()
        self._refresh_bulk_fusion_plan()
        self.statusBar().showMessage(
            f"Applied {plan.added:,} new, {plan.replaced:,} replaced, and {plan.removed:,} removed fusion rules. Save the mod to keep them.",
            10000)
    def _undo_bulk_fusions(self):
        controls = self.workspace_controls.get("Fusions")
        if not controls or "bulk" not in controls:
            return
        bulk = controls["bulk"]
        batch = bulk.get("batch")
        if batch is None or batch.project is not self.project:
            bulk["undo"].setEnabled(False)
            return
        restored, skipped = bulk_fusions.undo(self.project, batch)
        bulk["batch"] = None
        bulk["undo"].setEnabled(False)
        self._mark_dirty()
        self._refresh_fusions()
        self._refresh_bulk_fusion_plan()
        message = f"{restored} fusion rule(s) restored."
        if skipped:
            message += f" {skipped} rule(s) edited since the batch were left unchanged."
        self.statusBar().showMessage(message, 8000)
    def _selected_fusion_pairs(self):
        controls = self.workspace_controls["Fusions"]
        if controls["stack"].currentIndex() == 1:
            return [item.data(Qt.ItemDataRole.UserRole) for item in controls["image_list"].selectedItems()]
        table = controls["table"]
        return [table.item(row, 0).data(Qt.ItemDataRole.UserRole)
                for row in sorted({i.row() for i in table.selectedItems()}) if table.item(row, 0)]
    def _edit_fusion(self, *_args, add=False):
        pairs = self._selected_fusion_pairs()
        pair = None if add else (pairs[0] if pairs else None)
        if not add and pair is None: return
        old_result = None
        if pair:
            own = self.project.own_fusion(pair, set(self.project.active_removes()))
            old_result = own if own is not None else self.project.fusions.get(pair)
        dialog = QDialog(self); dialog.setWindowTitle("Add fusion" if add else "Edit fusion")
        form = QFormLayout(dialog)
        a = self._card_combo(dialog, pair[0] if pair else None)
        b = self._card_combo(dialog, pair[1] if pair else None)
        result = self._card_combo(dialog, old_result)
        form.addRow("Card A", a); form.addRow("Card B", b); form.addRow("Result", result)
        actions = QHBoxLayout(); ok = QPushButton("Apply"); cancel = QPushButton("Cancel")
        actions.addWidget(ok); actions.addWidget(cancel); form.addRow(actions)
        ok.clicked.connect(dialog.accept); cancel.clicked.connect(dialog.reject)
        problem = QLabel(); problem.setWordWrap(True); problem.setStyleSheet("color:#ff7777")
        form.addRow(problem)
        while True:
            if not dialog.exec(): return
            new_pair = (self._combo_card_id(a), self._combo_card_id(b))
            new_result = self._combo_card_id(result)
            if all(new_pair) and new_result:
                break
            # Say which field is empty instead of closing with nothing done.
            problem.setText("Name three cards (a number, a name, or pick one from the list).")
        if pair and self.project.pair(*new_pair) != pair:
            self.project.set_fusion(pair[0],pair[1],None)      # the fusion moved to other cards
        self.project.set_fusion(*new_pair,new_result)
        self._mark_dirty()
        if add:
            # Show the row that was just made, whatever the list is filtered to.
            self.workspace_controls["Fusions"]["changed"].setChecked(False)
            self.workspace_controls["Fusions"]["search"].setText(self.project.cards[new_pair[0]].name)
        self._refresh_fusions()
    def _remove_fusions(self):
        pairs = self._selected_fusion_pairs()
        if not pairs: return
        for a,b in pairs: self.project.set_fusion(a,b,None)
        self._mark_dirty(); self._refresh_fusions()
    def _revert_fusions(self):
        pairs = self._selected_fusion_pairs()
        if not pairs: return
        for pair in pairs: self.project.revert_fusion(pair)
        self._mark_dirty(); self._refresh_fusions()
    def _remove_fusion_result(self):
        # The selected pair's result is the likely one, as the Tk dialog fills it.
        pairs = self._selected_fusion_pairs()
        chosen = self.project.fusions.get(pairs[0]) if pairs else None
        cid = self._choose_one_card("Remove all disc recipes of…", selected=chosen)
        if cid is None:
            return
        if not self.project.retail_recipes(cid):
            QMessageBox.information(self, "Remove disc recipes", "No recipe on the disc makes this card.")
            return
        answer = QMessageBox.question(self, "Remove disc recipes",
            f"Remove every disc recipe of {self.project.card_label(cid)}?\n"
            "The mod’s own fusions remain. Revert a pair to restore that recipe.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No)
        if answer == QMessageBox.StandardButton.Yes:
            self.project.remove_recipes(cid)
            self._mark_dirty()
            self._refresh_fusions()
            self._refresh_bulk_fusion_plan()
    def _bulk_mode_changed(self, *_):
        bulk = self.workspace_controls["Fusions"]["bulk"]
        adding = bulk["mode"].currentData() == "add"
        for key in ("choose_card", "use_ladder", "ladder", "stronger", "overwrite"):
            bulk[key].setEnabled(adding)
        bulk["reset_result"].setVisible(not adding)
        bulk["apply"].setText("Apply…" if adding else "Take away…")
        if not bulk["result_id"]:
            bulk["result_button"].setText("Choose a result…" if adding else "Any result (click to restrict)…")
        self._schedule_bulk_fusion_plan()
    def _clear_bulk_result(self):
        self.workspace_controls["Fusions"]["bulk"]["result_id"] = 0
        self._bulk_mode_changed()
    def _clear_bulk_filter(self, side):
        c = self.workspace_controls["Fusions"]["bulk_filters"][side]
        for box in [*c["kinds"].values(), *c["attributes"], c["results_only"]]:
            box.setChecked(False)
        for key in ("types", "stars"):
            self._clear_checkable_list(c[key])
        for edit in [c["name"], c["text"], c["cards"], *c["bounds"].values()]:
            edit.clear()
        c["level_any"].setChecked(True)
        self._schedule_bulk_fusion_plan()
    def _list_bulk_matches(self, side):
        c = self.workspace_controls["Fusions"]["bulk_filters"][side]
        try:
            chosen, unknown = self._read_bulk_filter(c).select(self.project)
        except ValueError as problem:
            QMessageBox.warning(self, "Material filter", str(problem))
            return
        self._report("Matching material cards", "\n".join(self.project.card_label(cid) for cid in sorted(chosen))
                     + ("\nUnknown cards: " + str(unknown) if unknown else ""))
