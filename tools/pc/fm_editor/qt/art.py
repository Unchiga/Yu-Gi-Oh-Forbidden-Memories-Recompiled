"""The Art page."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class ArtMixin:
    def _build_art_page(self, parent):
        ui_path = UI_DIR / "arts_page.ui"
        ui_file = QFile(str(ui_path))
        if not ui_file.open(QIODevice.OpenModeFlag.ReadOnly):
            raise RuntimeError(f"Could not open Art Designer form {ui_path}: {ui_file.errorString()}")
        loader = QUiLoader()
        page = loader.load(ui_file, parent)
        ui_file.close()
        if page is None:
            raise RuntimeError(f"Could not load Art Designer form {ui_path}: {loader.errorString()}")
        layout = QVBoxLayout(parent)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(page)
        self.art_form = page

        def widget(widget_type, name):
            found = page.findChild(widget_type, name)
            if found is None:
                raise RuntimeError(f"Art Designer form is missing widget {name!r}")
            return found

        self.art_search = widget(QLineEdit, "artSearchEdit")
        self.art_filter = widget(QComboBox, "artFilterCombo")
        self.art_table = widget(QTableWidget, "artCardTable")
        splitter = widget(QSplitter, "artsSplitter")
        splitter.setStretchFactor(0, 2)
        splitter.setStretchFactor(1, 8)
        splitter.setSizes([320, 1150])
        self.art_count = widget(QLabel, "artCountLabel")
        self.art_status = widget(QLabel, "artStatusLabel")
        # The compact Art form has a single title/status pair; older forms
        # exposed separate labels for the selected card, instructions, and pack.
        # Use the compact title as the heading and treat the extra labels as
        # optional so either Designer layout can be loaded.
        self.art_heading = (page.findChild(QLabel, "selectedArtCardLabel")
                            or page.findChild(QLabel, "artworkTitle"))
        if self.art_heading is None:
            raise RuntimeError("Art Designer form is missing its artwork title label")
        self.art_pack_status = page.findChild(QLabel, "texturePackStatusLabel")
        self.art_instructions = page.findChild(QLabel, "artInstructionsLabel")
        self.art_previews = {}
        self.art_info = {}
        # Four panes a part now (the disc's, the console's, Internal, the
        # mod's): small enough that the last is not drawn off the page.
        parts_scroll = page.findChild(QScrollArea, "artPartsScroll")
        if parts_scroll is not None:
            # Four panes and the buttons do not fit a narrow window: the page
            # scrolls to them rather than drawing them off its edge.
            parts_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        canvas_sizes = {"art": QSize(166, 156), "thumbnail": QSize(132, 106),
                        "title": QSize(222, 33)}
        for part, stem in (("art", "picture"), ("thumbnail", "thumbnail"), ("title", "title")):
            # Designer forms may use a named QWidget wrapper or place the named
            # category layout directly in the scroll area. Keep the UI flexible:
            # only style a wrapper when one exists; the category contents are
            # found by their stable object names below either way.
            section = page.findChild(QWidget, stem + "Tab")
            if section is not None:
                section.setObjectName("artSectionCard")
            elif page.findChild(QVBoxLayout, stem + "Layout") is None:
                raise RuntimeError(
                    f"Art Designer form is missing the {stem!r} category layout"
                )
            widget(QLabel, stem + "HelpLabel").setObjectName("artPartHeading")
            self.art_previews[part] = {
                "disc": widget(QLabel, stem + "DiscPreview"),
                "game": widget(QLabel, stem + "GamePreview"),
                "mod": widget(QLabel, stem + "ModPreview"),
            }
            self.art_info[part] = {
                "disc": widget(QLabel, stem + "DiscInfo"),
                "game": widget(QLabel, stem + "GameInfo"),
                "mod": widget(QLabel, stem + "ModInfo"),
            }
            # The game draws a picture and a thumbnail larger at Internal 2x
            # and 4x; a pane of its own puts that beside the console's, as the
            # Tk window does, rather than behind a scale button.
            if part in ART_INTERNAL:
                self.art_previews[part]["internal"] = widget(QLabel, stem + "InternalPreview")
                self.art_info[part]["internal"] = widget(QLabel, stem + "InternalInfo")
            for preview in self.art_previews[part].values():
                preview.setFixedSize(canvas_sizes[part])
                preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
                preview.setObjectName("artPreviewImage")
            tiles = ["DiscGroup", "GameGroup", "ModGroup"]
            if part in ART_INTERNAL:
                tiles.append("InternalGroup")
            for suffix in tiles:
                widget(QWidget, stem + suffix).setObjectName("artPreviewTile")
            button_names = {
                "import": f"import{stem.title()}Button",
                "export_disc": f"export{stem.title()}DiscButton",
                "export_mod": f"export{stem.title()}ModButton",
                "revert": f"revert{stem.title()}Button",
            }
            for action, handler in (("import", self.import_art), ("export_disc", self.export_art_disc),
                                    ("export_mod", self.export_art_mod), ("revert", self.revert_art)):
                button = widget(QPushButton, button_names[action])
                button.clicked.connect(lambda checked=False, p=part, fn=handler: fn(p))
                if action == "import":
                    button.setObjectName("primary")
                if action in ("export_mod", "revert"):
                    button.setEnabled(False)
                self.art_info[part][action] = button

        self.art_filter.addItems(["All cards", "Art of the mod", "Added by the mod"])
        self.art_table.setColumnCount(4)
        self.art_table.setHorizontalHeaderLabels(["ID", "Name", "Type", "Art status"])
        self.art_table.setColumnWidth(0, 62)
        self.art_table.setColumnWidth(1, 220)
        self.art_table.setColumnWidth(2, 110)
        self.art_table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        self.art_table.verticalHeader().setVisible(False)
        self.art_search.textChanged.connect(self.refresh_art_list)
        self.art_filter.currentTextChanged.connect(self.refresh_art_list)
        self.art_table.itemSelectionChanged.connect(self.select_art_card)
    def refresh_art_list(self, *_args, select_id=None):
        if not hasattr(self, "art_table") or self.project is None:
            return
        selected = self.current_art_card if select_id is None else select_id
        query = self.art_search.text().casefold().strip()
        filter_name = self.art_filter.currentText()
        changed = art.changed_cards(self.project)
        state = art.state(self.project)
        pack_cards = {cid for cid, _part in state.gated}
        pack_cards.update(cid for cid, _part in state.owned.values())
        visible_art_cards = changed | pack_cards
        self._update_art_pack_status(state)
        self.art_table.blockSignals(True)
        held = sort_paused(self.art_table)
        self.art_table.setRowCount(0)
        for cid, card in sorted(self.project.cards.items()):
            if filter_name == "Art of the mod" and cid not in visible_art_cards:
                continue
            if filter_name == "Added by the mod" and cid not in self.project.added:
                continue
            if not self._card_matches(cid, query):     # the same search as the card list
                continue
            row = self.art_table.rowCount()
            self.art_table.insertRow(row)
            part_names = []
            for part in art.PARTS:
                if (cid, part) in state.images:
                    part_names.append(art.LABELS[part])
                elif (cid, part) in state.gated:
                    part_names.append(art.LABELS[part] + " · pack")
            values = (f"{cid:03d}", card.name, TYPE_NAMES[card.type], ", ".join(part_names) if part_names else "Stock")
            for column, value in enumerate(values):
                item = TableItem(value)
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, cid)
                self._tint_state(item, "changed" if part_names else "")
                self.art_table.setItem(row, column, item)
        sort_resumed(self.art_table, held)
        for row in range(self.art_table.rowCount()):
            if self.art_table.item(row, 0).data(Qt.ItemDataRole.UserRole) == selected:
                self.art_table.selectRow(row)
                break
        self.art_count.setText(f"{self.art_table.rowCount()} cards")
        self.art_table.blockSignals(False)
        row = self.art_table.currentRow()
        if row >= 0:
            self.show_art_card(self.art_table.item(row, 0).data(Qt.ItemDataRole.UserRole))
        else:
            self.show_art_card(None)
    def select_art_card(self):
        row = self.art_table.currentRow()
        cid = self.art_table.item(row, 0).data(Qt.ItemDataRole.UserRole) if row >= 0 else None
        self.show_art_card(cid)
    @staticmethod
    def _show_art_image(label, image, scale, empty_text, canvas_size):
        if image is None:
            label.setPixmap(QPixmap())
            label.setText(empty_text)
            return
        qimage = _qimage(image.width, image.height, image.rgba)
        target = QSize(canvas_size.width(), canvas_size.height())
        # All cards in a category occupy the same preview area. Scaling is
        # applied before this display fit, so 1x/2x/4x still renders different
        # game output while the surrounding layout stays stable.
        qimage = qimage.scaled(target, Qt.AspectRatioMode.KeepAspectRatio,
                               Qt.TransformationMode.FastTransformation)
        label.setText("")
        label.setPixmap(QPixmap.fromImage(qimage))
    def _clear_art_part(self, part, message, kinds=None):
        for kind in (kinds if kinds is not None else self.art_previews[part]):
            self.art_previews[part][kind].setPixmap(QPixmap())
            self.art_previews[part][kind].setText(message)
            self.art_info[part][kind].clear()
    def show_art_card(self, cid):
        self.current_art_card = cid
        if cid is None or cid not in self.project.cards or self.files is None:
            self.art_heading.setText("Select a card")
            for part, previews in self.art_previews.items():
                for kind, label in previews.items():
                    label.setPixmap(QPixmap())
                    label.setText("Choose a card" if kind != "mod" else "No replacement")
                for kind, label in self.art_info[part].items():
                    if isinstance(label, QLabel):
                        label.setText("")
            for info in self.art_info.values():
                for action in ("export_mod", "revert"):
                    info[action].setEnabled(False)
            if self.art_instructions is not None:      # optional in older Designer forms
                self.art_instructions.clear()
            self.art_status.setText("Choose a card to view its original art and mod replacements.")
            return

        card = self.project.cards[cid]
        base = self.project.base_of(cid)
        self.art_heading.setText(self.project.card_label(cid))
        if cid in self.project.added:
            detail = (f"Copy based on {self.project.card_label(base)}. Without its own artwork it inherits its base’s. "
                      "Its picture and thumbnail PNGs are saved with the added card and converted to 102×96 and "
                      "40×32 when the game starts; they gain no extra detail at Internal 2×/4×. Restart the game after changes.")
        else:
            detail = ("Retail picture and thumbnail replacements are stored in the mod texture pack, up to 4×. "
                      "The game averages them down at 1×; Internal 2×/4× retains higher-resolution detail. "
                      "Added copies without their own artwork inherit these replacements.")
        if self.art_instructions is not None:
            self.art_instructions.setText(detail + " Name plates use a title PNG with dark ink on white.")
        notes = []
        for part in art.PARTS:
            try:
                disc_image = art.disc_image(self.files.wa, base, part)
                if part == "title":
                    disc_image = art.plate_image(art.disc_plate_inks(self.files.wa, base), background=art.GOLD)
                game_image = art.in_game(self.project, self.files.wa, cid, part, 1)
                internal = ART_INTERNAL.get(part)
                internal_image = (art.in_game(self.project, self.files.wa, cid, part, internal)
                                  if internal else None)
                mod_image = art.replacement_image(self.project, cid, part)
                gated = art.gated_image(self.project, cid, part) if mod_image is None and part != "title" else None
                if gated is not None:
                    mod_image = gated[0]
                if part == "title" and mod_image is not None:
                    mod_image = art.plate_image(art.plate_inks(mod_image), background=art.GOLD)
                _, where = art.shown_image(self.project, self.files.wa, cid, part)
                canvas = {"art": QSize(166, 156), "thumbnail": QSize(132, 106),
                          "title": QSize(222, 33)}[part]
                self._show_art_image(self.art_previews[part]["disc"], disc_image,
                                     {"art": 2, "thumbnail": 4, "title": 4}[part], "No disc image",
                                     canvas)
                self._show_art_image(self.art_previews[part]["game"], game_image,
                                     1, "Not available", canvas)
                self._show_art_image(self.art_previews[part]["mod"], mod_image,
                                     1, "No mod replacement", canvas)
                if internal:
                    self._show_art_image(self.art_previews[part]["internal"], internal_image,
                                         internal, "Not available", canvas)
                self.art_info[part]["disc"].setText("Original asset from the game data")
                def measured(image):
                    return f"{image.width} × {image.height} px" if image is not None else "not available"

                self.art_info[part]["game"].setText(
                    f"The console's render · {measured(game_image)} · shown from {where}.")
                if internal:
                    self.art_info[part]["internal"].setText(
                        f"Internal {internal}× · {measured(internal_image)} · the detail the "
                        "console's render averages away.")
                current_art_state = art.state(self.project)
                replacement = current_art_state.images.get((cid, part))
                self.art_info[part]["mod"].setText(art.describe(self.project, cid, part))
                self.art_info[part]["export_mod"].setEnabled(
                    replacement is not None or (cid, part) in current_art_state.gated)
                self.art_info[part]["revert"].setEnabled(replacement is not None)
            except (OSError, ValueError, art.pngio.PngError) as problem:
                self._clear_art_part(part, "Preview unavailable")
                notes.append(f"{art.LABELS[part]}: {problem}")
                state = art.state(self.project)
                self.art_info[part]["revert"].setEnabled((cid, part) in state.images)
                self.art_info[part]["export_mod"].setEnabled(
                    (cid, part) in state.images or (cid, part) in state.gated)
        self.art_status.setText("  ".join(notes) if notes else "Preview updated from the game files and current mod data.")
    def import_art(self, part):
        cid = self.current_art_card
        if cid is None:
            return
        path, _ = QFileDialog.getOpenFileName(self, f"Import {art.LABELS[part]}", "", "PNG images (*.png)")
        if not path:
            return
        try:
            image = art.pngio.read(path)
            original_size = image.size
            notes = art.set_image(self.project, cid, part, image)
        except (OSError, ValueError, art.pngio.PngError) as problem:
            QMessageBox.critical(self, "Could not import artwork", str(problem))
            return
        self.fusion_art_cache.clear()
        self._mark_dirty()
        self.refresh_art_list(select_id=cid)
        self._refresh_fusions()
        details = f"Imported {Path(path).name} ({original_size[0]}×{original_size[1]})."
        if notes:
            details += " " + " ".join(notes)
        self.art_status.setText(details)
        self._render_preview()
        if any("shape" in note for note in notes):
            width, height = art.SIZES[part]
            QMessageBox.warning(self, "Artwork cropped",
                f"The picture is not the {art.LABELS[part].lower()}’s shape ({width}:{height}), "
                "so only its middle is kept. Crop it yourself to choose the part.")
    def export_art_disc(self, part):
        cid = self.current_art_card
        if cid is None:
            return
        base = self.project.base_of(cid)
        try:
            image = (art.plate_image(art.disc_plate_inks(self.files.wa, base)) if part == "title"
                     else art.disc_image(self.files.wa, base, part))
            self._save_art_image(cid, part, image, "disc")
        except (OSError, ValueError, art.pngio.PngError) as problem:
            QMessageBox.critical(self, "Could not export artwork", str(problem))
    def export_art_mod(self, part):
        cid = self.current_art_card
        if cid is None:
            return
        try:
            image = art.replacement_image(self.project, cid, part)
            if image is None and part != "title":
                gated = art.gated_image(self.project, cid, part)
                image = gated[0] if gated is not None else None
            if image is not None:
                self._save_art_image(cid, part, image, "mod")
        except (OSError, ValueError, art.pngio.PngError) as problem:
            QMessageBox.critical(self, "Could not export artwork", str(problem))
    def _save_art_image(self, cid, part, image, source):
        if image is None:
            return
        safe_name = re.sub(r"[^a-z0-9]+", "-", self.project.cards[cid].name.lower()).strip("-") or "card"
        suffix = {"art": "", "thumbnail": ".small", "title": ".title"}[part]
        default = f"{cid:04d}-{safe_name}{suffix}-{source}.png"
        path, _ = QFileDialog.getSaveFileName(self, "Export artwork", default, "PNG images (*.png)")
        if path:
            art.pngio.write(path, image)
            self.art_status.setText(f"Exported {path}")
    def revert_art(self, part):
        cid = self.current_art_card
        if cid is None:
            return
        if (cid, part) not in art.state(self.project).images:
            return
        art.revert(self.project, cid, part)
        self.fusion_art_cache.clear()
        self._mark_dirty()
        self.refresh_art_list(select_id=cid)
        self._refresh_fusions()
        self._render_preview()
    def goto_art(self, cid):
        self.art_filter.setCurrentText("All cards")
        self.art_search.clear()
        self.refresh_art_list(select_id=cid)
