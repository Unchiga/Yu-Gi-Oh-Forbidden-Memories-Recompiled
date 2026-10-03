"""The Campaign page's Map (a port of map_tab.MapTab)."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class MapMixin:
    def _fusion_card_pixmap(self, cid):
        if cid is None or self.files is None:
            return None
        try:
            if cid not in self.fusion_art_cache:
                image = art.in_game(self.project, self.files.wa, cid, "art", 1)
                if image is None:
                    return None
                pixmap = QPixmap.fromImage(_qimage(image.width, image.height, image.rgba))
                self.fusion_art_cache[cid] = pixmap.scaled(
                    QSize(56, 58), Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.FastTransformation)
            return self.fusion_art_cache[cid]
        except (IndexError, KeyError, ValueError):
            return None
    def _build_map_tab(self, page, get, controls):
        # The Designer form holds a plain QLabel where the drawing goes; swap in
        # the canvas that reports its mouse. Take the layout by its own name:
        # the wrapper from parentWidget().layout() may not outlive the call.
        designer_canvas = get(QLabel, "mapCanvas")
        holder = page.findChild(QVBoxLayout, "mapPreviewLayout")
        canvas = MapCanvas(designer_canvas.parentWidget())
        canvas.setObjectName("mapCanvasWidget")
        canvas.setMinimumSize(cm.SCREEN[0], cm.SCREEN[1])
        canvas.setAlignment(Qt.AlignmentFlag.AlignCenter)
        canvas.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        canvas.setStyleSheet("background:#000000;border:1px solid #26374c;border-radius:6px")
        holder.replaceWidget(designer_canvas, canvas)
        designer_canvas.setParent(None)
        designer_canvas.deleteLater()
        canvas.on_press = self._map_press
        canvas.on_move = self._map_move
        canvas.on_release = self._map_release

        exit_tabs = [get(QPushButton, f"mapExitTab{n + 1}") for n in range(cm.EXITS)]
        tab_group = QButtonGroup(self)
        tab_group.setExclusive(True)
        for n, button in enumerate(exit_tabs):
            tab_group.addButton(button, n)
        tab_group.idClicked.connect(self._map_select_exit)
        directions = {name: get(QPushButton, f"mapExit{name.capitalize()}Button")
                      for name, _bit in cm.DIRECTIONS}
        m = {
            "canvas": canvas, "screen": get(QRadioButton, "mapScreenRadio"),
            "overview": get(QRadioButton, "mapOverviewRadio"),
            "package": get(QComboBox, "mapPackageCombo"),
            "context": get(QLabel, "mapContextLabel"), "search": get(QLineEdit, "mapSearchEdit"),
            "places": get(QTableWidget, "mapPlaceTable"), "hint": get(QLabel, "mapHintLabel"),
            "caption": get(QLabel, "mapCaptionLabel"), "problems": get(QLabel, "mapProblemsLabel"),
            "marker_note": get(QLabel, "mapMarkerNote"),
            "confirm": get(QComboBox, "mapConfirmCombo"), "gate": get(QCheckBox, "mapGateCheck"),
            "exit_tabs": exit_tabs, "exit_group": tab_group, "directions": directions,
            "used": get(QCheckBox, "mapExitUsedCheck"),
            "destination": get(QComboBox, "mapExitDestinationCombo"),
            "condition": get(QComboBox, "mapExitConditionCombo"),
            "arrow": get(QComboBox, "mapExitArrowCombo"),
            "groups": [get(QGroupBox, key) for key in ("mapCameraGroup", "mapMarkerGroup",
                                                       "mapConfirmGroup", "mapExitsGroup")],
        }
        for key, name in (("distance", "mapDistanceSpin"), ("heading", "mapHeadingSpin"),
                          ("pitch", "mapPitchSpin"), ("target_x", "mapTargetXSpin"),
                          ("target_z", "mapTargetZSpin"), ("marker_x", "mapMarkerXSpin"),
                          ("marker_y", "mapMarkerYSpin"), ("flag", "mapExitFlagSpin"),
                          ("steps", "mapExitFramesSpin"), ("x", "mapExitXSpin"), ("y", "mapExitYSpin")):
            spin = get(QSpinBox, name)
            spin.setRange(*{"flag": (0, 0x7FFF), "steps": (0, 255)}.get(key, (-32768, 32767)))
            # Three of these sit in a row inside a narrow panel: a six-digit box
            # is as wide as any value here needs, and the panel never has to
            # scroll sideways for them.
            spin.setMaximumWidth(84)
            spin.setMinimumWidth(56)
            spin.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
            m[key] = spin
        controls["map"] = m
        self.map_index = 0
        self.map_exit = 0
        self.map_filling = False
        self.map_drag = None
        self.map_preview = None        # the place as a drag is leaving it
        self.map_references = {}       # camera -> a picture the player chose, this session
        self.map_pictures = {}         # rendered map_view pictures, by key
        self.map_problem = ""
        self.map_dialog = None

        m["package"].addItems([cm.PACKAGE_LABELS[name] for name, _ in cm.PACKAGES])
        m["condition"].addItems(self.MAP_CONDITIONS)
        m["arrow"].addItems([f"{i}: {name}" for i, name in enumerate(cm.ARROWS)])
        m["places"].setColumnCount(3)
        for title in ("mapPlacesTitle", "mapPreviewTitle"):
            get(QLabel, title).setStyleSheet("font-size:16px;font-weight:650;color:#f3f7fc")
        for muted in ("mapContextLabel", "mapHintLabel", "mapCaptionLabel", "mapMarkerNote",
                      "mapToolbarSeparator", "mapPackageLabel"):
            get(QLabel, muted).setStyleSheet("color:#9aacc4")
        m["problems"].setStyleSheet("color:#f2c04c")
        # The form is a narrow column: nothing in it may demand more width than
        # the panel has, or the whole page grows a sideways scrollbar.
        scroll = get(QScrollArea, "mapFormScroll")
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setMinimumWidth(410)
        for box in (m["package"], m["confirm"], m["destination"], m["condition"], m["arrow"]):
            # Without this a combo asks for its longest entry ("13: Card Shop /
            # Old Card Shop"), which is wider than the column.
            box.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
            box.setMinimumContentsLength(8)
            box.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        for layout_name in ("mapCameraLayout", "mapMarkerLayout", "mapExitGrid"):
            grid = page.findChild(QGridLayout, layout_name)
            if grid is not None:
                for column in range(grid.columnCount()):
                    grid.setColumnStretch(column, 0 if column % 2 == 0 else 1)
        # The pad's glyphs need room the shared 13px button padding does not leave.
        for button in directions.values():
            button.setFixedSize(30, 26)
            button.setStyleSheet("padding:0px;font-size:13px")
        for button in m["exit_tabs"]:
            button.setMinimumWidth(0)
            button.setStyleSheet("padding:6px 4px")
        splitter = get(QSplitter, "mapSplitter")
        for index, stretch in enumerate((3, 6, 4)):
            splitter.setStretchFactor(index, stretch)
        splitter.setSizes([320, 690, 450])

        m["screen"].toggled.connect(lambda *_: self._draw_map())
        m["package"].currentIndexChanged.connect(self._map_package_changed)
        m["search"].textChanged.connect(self._fill_map_places)
        get(QPushButton, "mapSearchButton").clicked.connect(self._fill_map_places)
        m["places"].itemSelectionChanged.connect(self._map_picked)
        get(QPushButton, "mapReferenceButton").clicked.connect(self._choose_map_reference)
        get(QPushButton, "mapPicturesButton").clicked.connect(self._show_map_pictures)
        get(QPushButton, "resetPlaceButton").clicked.connect(self._reset_map_place)
        get(QPushButton, "resetAllButton").clicked.connect(self._reset_map_all)
        for key in self.MAP_INTS + ("flag", "steps", "x", "y"):
            m[key].valueChanged.connect(self._map_edited)
        for box in (m["confirm"], m["destination"], m["condition"], m["arrow"]):
            box.currentIndexChanged.connect(self._map_edited)
        for box in (m["gate"], m["used"]):
            box.toggled.connect(self._map_edited)
        for button in directions.values():
            button.setCheckable(True)
            button.toggled.connect(self._map_edited)
    @property
    def map_state(self):
        return cm.state(self.project) if self.project is not None else None
    def _map_available(self):
        return self.project is not None and cm.available(self.project)
    def _map_location(self):
        return self.map_preview or self.map_state.locations[self.map_index]
    def _map_version(self):
        return map_art.state(self.project).version
    def _map_strips(self):
        return {p: image for p in map_art.STRIP_PALETTES
                if (image := map_art.strip_override(self.project, p)) is not None}
    def _map_camera(self, index):
        loc = self.map_state.locations[index]
        return loc.distance, loc.heading, loc.pitch, loc.target_x, loc.target_z
    def _refresh_map(self):
        m = self.workspace_controls.get("Campaign", {}).get("map")
        if not m:
            return
        self.map_pictures.clear()
        available = self._map_available()
        for group in m["groups"]:
            group.setEnabled(available)
        self._fill_map_places()
        if not available:
            m["canvas"].setPixmap(QPixmap())
            m["context"].setText("")
            m["caption"].setText("The game files hold no campaign map (the overworld packages of WA_MRG.MRG): "
                                 "choose the retail disc under File > Game files.")
            return
        names = [cm.label(self.project, i) for i in range(cm.COUNT)]
        self.map_filling = True
        try:
            m["confirm"].clear()
            m["confirm"].addItem(self.MAP_CONFIRM_SCENE, 0)
            for index, label in enumerate(names):
                if index:
                    m["confirm"].addItem(label, index)
            m["destination"].clear()
            for index, label in enumerate(names):
                m["destination"].addItem(label, index)
        finally:
            self.map_filling = False
        self._select_map_place(min(self.map_index, cm.COUNT - 1))
    def _fill_map_places(self, *_):
        m = self.workspace_controls["Campaign"]["map"]
        table, query = m["places"], m["search"].text().casefold().strip()
        table.blockSignals(True)
        held = sort_paused(table)
        table.setRowCount(0)
        if self._map_available():
            for index in range(cm.COUNT):
                name = cm.name(self.project, index)
                area = "Town" if index >= cm.TOWN_FIRST else "World"
                if query and query not in name.casefold() and query != str(index):
                    continue
                row = table.rowCount()
                table.insertRow(row)
                for column, value in enumerate((index, name, area)):
                    item = TableItem(str(value))
                    if column == 0:
                        item.setData(Qt.ItemDataRole.UserRole, index)
                    self._tint_state(item, "changed" if cm.changed(self.project, index) else "")
                    table.setItem(row, column, item)
            m["hint"].setText("Drag an arrow or the marker on the screen, or a place in the overview. Saving "
                              "writes \"data\" patches of both overworld packages (before and after the coup); "
                              "the game needs a restart to read them.")
        sort_resumed(table, held)
        table.blockSignals(False)
        self._select_map_row(self.map_index)
    def _select_map_row(self, index):
        table = self.workspace_controls["Campaign"]["map"]["places"]
        for row in range(table.rowCount()):
            if table.item(row, 0).data(Qt.ItemDataRole.UserRole) == index:
                table.blockSignals(True)
                table.selectRow(row)
                table.blockSignals(False)
                table.scrollToItem(table.item(row, 0))
                return
    def _map_picked(self):
        table = self.workspace_controls["Campaign"]["map"]["places"]
        row = table.currentRow()
        if row < 0:
            return
        index = table.item(row, 0).data(Qt.ItemDataRole.UserRole)
        if index is not None and index != self.map_index:
            self._select_map_place(index)
    def _select_map_place(self, index):
        self.map_index = index
        self._select_map_row(index)
        self._fill_map_form()
        self._draw_map()
    def goto_map_place(self, index):
        if isinstance(index, int) and 0 <= index < cm.COUNT:
            tabs = self.workspace_controls["Campaign"]["tabs"]
            for tab in range(tabs.count()):
                if tabs.tabText(tab) == "Map":
                    tabs.setCurrentIndex(tab)
                    break
            self._select_map_place(index)
    def _map_select_exit(self, number):
        self.map_exit = number
        self._fill_map_form()
    def _fill_map_form(self):
        if not self._map_available():
            return
        m = self.workspace_controls["Campaign"]["map"]
        loc = self.map_state.locations[self.map_index]
        town = self.map_index >= cm.TOWN_FIRST
        self.map_filling = True
        try:
            m["context"].setText(cm.label(self.project, self.map_index) +
                                 ("  (town)" if town else "  (world map)"))
            for key in self.MAP_INTS:
                m[key].setValue(getattr(loc, key))
            m["gate"].setChecked(bool(loc.gate))
            m["confirm"].setCurrentIndex(max(0, m["confirm"].findData(
                loc.confirm if loc.confirm < cm.COUNT else 0)))
            for box in (m["marker_x"], m["marker_y"]):
                box.setEnabled(town)
            m["marker_note"].setText("" if town else "The world map draws no marker; these are kept as "
                                                     "the disc has them.")
            e = loc.exits[self.map_exit]
            m["used"].setChecked(e.used)
            m["destination"].setEnabled(e.used)
            if e.destination < cm.COUNT:
                m["destination"].setCurrentIndex(max(0, m["destination"].findData(e.destination)))
            for name, bit in cm.DIRECTIONS:
                m["directions"][name].setChecked(bool(e.buttons & bit))
            kind, flag = cm.condition_parts(e.condition)
            m["condition"].setCurrentIndex(self.MAP_CONDITION_KINDS.index(kind))
            m["flag"].setValue(flag)
            m["steps"].setValue(e.steps)
            m["arrow"].setCurrentIndex(e.arrow if e.arrow < len(cm.ARROWS) else 0)
            m["x"].setValue(e.x)
            m["y"].setValue(e.y)
            self._map_exit_titles()
        finally:
            self.map_filling = False
        self._show_map_problems()
    def _map_exit_titles(self):
        m = self.workspace_controls["Campaign"]["map"]
        loc = self.map_state.locations[self.map_index]
        for n, e in enumerate(loc.exits):
            button = m["exit_tabs"][n]
            glyphs = "".join(self.MAP_ARROW_GLYPHS[d] for d in cm.direction_names(e.buttons))
            if e.used:
                where = cm.name(self.project, e.destination).split(" / ")[0] if e.destination < cm.COUNT else "?"
                # The Tk tab titles carry the direction and the destination; the
                # segmented buttons have room for the glyphs, the rest hovers.
                button.setText(f"{n + 1} {glyphs or '•'}")
                button.setToolTip(f"Exit {n + 1} {glyphs} to {where}".replace("  ", " "))
            else:
                button.setText(str(n + 1))
                button.setToolTip(f"Exit {n + 1}: not used")
            button.setChecked(n == self.map_exit)
            button.setStyleSheet("" if not e.used else "font-weight:600")
    def _map_edited(self, *_):
        """A field changed: store the form in the place, as map_tab.edited does."""
        if self.map_filling or not self._map_available():
            return
        m = self.workspace_controls["Campaign"]["map"]
        loc = self.map_state.locations[self.map_index].copy()
        for key in self.MAP_INTS:
            setattr(loc, key, m[key].value())
        loc.gate = (loc.gate or 1) if m["gate"].isChecked() else 0
        loc.confirm = m["confirm"].currentData() or 0
        e = loc.exits[self.map_exit]
        was_used = e.used
        refill = False
        if not m["used"].isChecked():
            e.destination = cm.NO_EXIT
            refill = was_used
        else:
            picked = m["destination"].currentData()
            if picked is not None:
                e.destination = picked
            elif not was_used:
                e.destination = 0
            if not was_used:
                refill = True
        buttons = e.buttons & ~cm.DIRECTION_BITS
        for name, bit in cm.DIRECTIONS:
            if m["directions"][name].isChecked():
                buttons |= bit
        e.buttons = buttons
        e.condition = cm.condition_value(self.MAP_CONDITION_KINDS[m["condition"].currentIndex()],
                                         m["flag"].value())
        # A newly used exit with no length gets the game's usual one.
        e.steps = m["steps"].value() or (16 if refill and e.used else 0)
        e.x, e.y = m["x"].value(), m["y"].value()
        e.arrow = m["arrow"].currentIndex()
        self._store_map(loc)
        if refill:
            QTimer.singleShot(0, self._fill_map_form)
    def _store_map(self, loc):
        if loc == self.map_state.locations[self.map_index]:
            return
        self.map_state.locations[self.map_index] = loc
        self._map_exit_titles()
        self._fill_map_places()
        self._mark_dirty()
        self._draw_map()
        self._show_map_problems()
    def _show_map_problems(self):
        m = self.workspace_controls["Campaign"]["map"]
        issues = []
        cm.check(self.project, issues)
        mine = [i for i in issues if i.target == self.map_index]
        m["problems"].setText("\n".join(f"{i.level}: {i.message}" for i in mine[:6]))
    def _reset_map_place(self):
        if not self._map_available() or not cm.changed(self.project, self.map_index):
            return
        cm.reset(self.project, self.map_index)
        self._mark_dirty()
        self._fill_map_places()
        self._fill_map_form()
        self._draw_map()
    def _reset_map_all(self):
        if not self._map_available() or not cm.any_changed(self.project):
            return
        answer = QMessageBox.question(self, "Map", "Put every place back as the disc has it?",
                                      QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                                      QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        cm.reset_all(self.project)
        self._mark_dirty()
        self._fill_map_places()
        self._select_map_place(self.map_index)
    def _map_picture(self, key, make):
        """A map_view picture, drawn once; None when the disc's model cannot be
        read (said in the caption)."""
        if key not in self.map_pictures:
            try:
                self.map_pictures[key] = make()
            except Exception as problem:     # a disc whose map the renderer cannot read
                self.map_pictures[key] = None
                self.map_problem = f"The map could not be drawn: {problem}"
        return self.map_pictures[key]
    def _map_background(self, index):
        """(image, where it comes from) for the place's screen, or None."""
        from .. import map_view
        camera = self._map_camera(index)
        if camera in self.map_references:
            return self.map_references[camera], "your reference picture"
        if self.files is None:
            return None
        sector = self._map_package_sector()
        picture = self._map_picture(
            ("view", sector, camera, index < cm.TOWN_FIRST, self._map_version()),
            lambda: map_view.render(map_view.model(self.files.wa, sector), camera,
                                    spotlight=index < cm.TOWN_FIRST,
                                    overrides=map_art.texture_overrides(self.project, self._map_package_name())))
        if picture is None:
            return None
        label = self.workspace_controls["Campaign"]["map"]["package"].currentText()
        return picture, f"drawn from the disc's map model, {label}"
    @staticmethod
    def _map_pixmap(image, zoom=1):
        picture = _qimage(image.width, image.height, image.rgba)
        if zoom != 1:
            picture = picture.scaled(image.width * zoom, image.height * zoom,
                                     Qt.AspectRatioMode.IgnoreAspectRatio,
                                     Qt.TransformationMode.FastTransformation)
        return QPixmap.fromImage(picture)
    def _draw_map(self):
        m = self.workspace_controls.get("Campaign", {}).get("map")
        if not m:
            return
        canvas = m["canvas"]
        canvas.targets = []
        surface = QPixmap(*self.MAP_CANVAS)
        surface.fill(QColor("#000000"))
        if not self._map_available():
            canvas.set_surface(surface)
            return
        painter = QPainter(surface)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        try:
            if m["overview"].isChecked():
                self._draw_map_overview(painter, canvas, m)
            else:
                self._draw_map_screen(painter, canvas, m)
        finally:
            painter.end()
        canvas.set_surface(surface)
    def _draw_map_screen(self, painter, canvas, m):
        zoom = self.MAP_ZOOM
        data = self.map_state.retail
        loc = self._map_location()
        strips = self._map_strips()
        found = self._map_background(self.map_index)
        if found is not None:
            picture, where = found
            if picture.size != cm.SCREEN:
                picture = pngio.resample(picture, *cm.SCREEN)
            painter.drawPixmap(0, 0, self._map_pixmap(picture, zoom))
        else:
            where = self.map_problem or "no picture of the map"
            where += "; Reference picture… takes a screenshot of the game"
            painter.setPen(QColor("#1c2430"))
            for x in range(0, self.MAP_CANVAS[0], 40):
                painter.drawLine(x, 0, x, self.MAP_CANVAS[1])
            for y in range(0, self.MAP_CANVAS[1], 40):
                painter.drawLine(0, y, self.MAP_CANVAS[0], y)
        panel = cm.sprite_image(data, *cm.PANEL, strips)
        if panel:
            image, left, top = panel
            painter.drawPixmap((cm.PANEL_AT[0] + left) * zoom, (cm.PANEL_AT[1] + top) * zoom,
                               self._map_pixmap(image, zoom))
        painter.setPen(QColor("#ffffff"))
        title = QFont(painter.font())
        title.setPointSize(13)
        painter.setFont(title)
        painter.drawText(QRect(0, 30 * zoom - 16, self.MAP_CANVAS[0], 32),
                         Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignVCenter,
                         cm.name(self.project, self.map_index).split(" / ")[0])
        small = QFont(painter.font())
        small.setPointSize(9)
        painter.setFont(small)
        labels = {}
        for n, e in enumerate(loc.exits):
            if e.used:
                where_to = cm.name(self.project, e.destination).split(" / ")[0] if e.destination < cm.COUNT else "?"
                text = f"{n + 1}: {where_to}"
                if e.condition:
                    kind, flag = cm.condition_parts(e.condition)
                    text += f" ({'' if kind == 'set' else 'not '}{flag})"
                labels.setdefault((e.x, e.y), []).append((n, text))
        for n, e in enumerate(loc.exits):
            if not e.used:
                continue
            arrow = cm.arrow_image(data, e.arrow, strips) if e.arrow < 8 else None
            if arrow:
                image, left, top = arrow
                at = QRect((e.x + left) * zoom, (e.y + top) * zoom, image.width * zoom, image.height * zoom)
                painter.drawPixmap(at.topLeft(), self._map_pixmap(image, zoom))
            else:
                at = QRect(e.x * zoom - 10, e.y * zoom - 10, 20, 20)
                painter.setPen(QColor("#ff5050"))
                painter.drawRect(at)
            canvas.targets.append(("exit", n, at))
            group = labels.get((e.x, e.y), [])
            if not group or group[0][0] != n:
                continue        # one label for the exits that share a place
            text = "\n".join(t for _, t in group)
            painter.setPen(QColor("#ffffff"))
            if e.x > 200:
                box = QRect(e.x * zoom - 16 - 300, e.y * zoom - 24, 300, 48)
                flags = Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            elif e.x < 120:
                box = QRect(e.x * zoom + 16, e.y * zoom - 24, 300, 48)
                flags = Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
            else:
                box = QRect(e.x * zoom - 150, e.y * zoom + 16, 300, 48)
                flags = Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop
            painter.drawText(box, int(flags), text)
        if self.map_index >= cm.TOWN_FIRST:
            marker = cm.sprite_image(data, *cm.MARKER, strips)
            if marker:
                image, left, top = marker
                at = QRect((loc.marker_x + left) * zoom, (loc.marker_y + top) * zoom,
                           image.width * zoom, image.height * zoom)
                painter.drawPixmap(at.topLeft(), self._map_pixmap(image, zoom))
                canvas.targets.append(("marker", None, at))
        m["caption"].setText(f"The screen at this place, {zoom}x ({where}). Arrows are drawn with the exit's "
                             "picture at its x, y; the conditions shown are the flags each needs.")
    def _map_world_frame(self):
        x0, y0, x1, y1 = self.MAP_WORLD_BOX
        scale = min(x1 - x0, y1 - y0) / self.MAP_WORLD_SPAN
        return self.MAP_WORLD_CENTRE[0], self.MAP_WORLD_CENTRE[1], scale, (x0 + x1) / 2, (y0 + y1) / 2
    def _map_node(self, index):
        loc = self.map_preview if (self.map_preview and index == self.map_index) \
            else self.map_state.locations[index]
        if index < cm.TOWN_FIRST:
            cx, cz, scale, ox, oy = self._map_world_frame()
            # Turned as the world's cameras mostly look: -x up the picture, +z right.
            return ox + (loc.target_z - cz) * scale, oy + (loc.target_x - cx) * scale
        x0, y0, x1, y1 = self.MAP_TOWN_BOX
        return (x0 + loc.marker_x * (x1 - x0) / cm.SCREEN[0],
                y0 + loc.marker_y * (y1 - y0) / cm.SCREEN[1])
    def _draw_map_overview(self, painter, canvas, m):
        from .. import map_view
        x0, y0, x1, y1 = self.MAP_WORLD_BOX
        tx0, ty0, tx1, ty1 = self.MAP_TOWN_BOX
        sector = self._map_package_sector()
        if self.files is not None:
            version = self._map_version()
            overrides = map_art.texture_overrides(self.project, self._map_package_name())
            top = self._map_picture(("top", sector, version), lambda: map_view.render_top(
                map_view.model(self.files.wa, sector), self.MAP_WORLD_CENTRE, self.MAP_WORLD_SPAN,
                (x1 - x0, y1 - y0), overrides=overrides))
            if top is not None:
                painter.drawPixmap(x0, y0, self._map_pixmap(top))
            camera = self._map_camera(cm.TOWN_FIRST)
            view = self._map_picture(("view", sector, camera, False, version), lambda: map_view.render(
                map_view.model(self.files.wa, sector), camera, overrides=overrides))
            if view is not None:
                painter.drawPixmap(tx0, ty0, self._map_pixmap(pngio.resample(view, tx1 - tx0, ty1 - ty0)))
        small = QFont(painter.font())
        small.setPointSize(9)
        painter.setFont(small)
        painter.setPen(QColor("#3b4b5e"))
        painter.drawRect(QRect(tx0, ty0, tx1 - tx0, ty1 - ty0))
        painter.setPen(QColor("#c4c8cd"))
        painter.drawText(QRect(tx0, ty0 - 20, 300, 18), int(Qt.AlignmentFlag.AlignLeft |
                         Qt.AlignmentFlag.AlignBottom), "The town (place 10's camera)")
        painter.drawText(QRect(x0 + 4, y0 - 20, 440, 18), int(Qt.AlignmentFlag.AlignLeft |
                         Qt.AlignmentFlag.AlignBottom),
                         "The world map from above: each site where its camera looks")
        for k, (kind, text) in enumerate((("always", "always"), ("set", "while a flag is set"),
                                          ("clear", "while a flag is clear"))):
            y = 204 + k * 16
            painter.setPen(QPen(QColor(self.MAP_EDGE_COLOURS[kind]), 2))
            painter.drawLine(tx0, y, tx0 + 24, y)
            painter.setPen(QColor("#c4c8cd"))
            painter.drawText(QRect(tx0 + 30, y - 9, 200, 18), int(Qt.AlignmentFlag.AlignLeft |
                             Qt.AlignmentFlag.AlignVCenter), text)
        pen = QPen(QColor("#c4c8cd"), 2)
        pen.setStyle(Qt.PenStyle.DashLine)
        painter.setPen(pen)
        painter.drawLine(tx0, 204 + 48, tx0 + 24, 204 + 48)
        painter.setPen(QColor("#c4c8cd"))
        painter.drawText(QRect(tx0 + 30, 204 + 48 - 9, 200, 18), int(Qt.AlignmentFlag.AlignLeft |
                         Qt.AlignmentFlag.AlignVCenter), "Confirm")
        locations = self.map_state.locations
        for index in range(cm.COUNT):
            ax, ay = self._map_node(index)
            for what, destination, condition in cm.edges(locations, index):
                if destination == index or what == "cancel":
                    continue
                bx, by = self._map_node(destination)
                kind = cm.condition_parts(condition)[0]
                width = 3 if self.map_index in (index, destination) else 1
                # A little to the side, so a way back does not cover the way there.
                dx, dy = by - ay, ax - bx
                length = max((dx * dx + dy * dy) ** 0.5, 1)
                sx, sy = dx / length * 3, dy / length * 3
                pen = QPen(QColor(self.MAP_EDGE_COLOURS[kind]), width)
                if what == "confirm":
                    pen.setStyle(Qt.PenStyle.DashLine)
                painter.setPen(pen)
                painter.drawLine(QPoint(round(ax + sx), round(ay + sy)), QPoint(round(bx + sx), round(by + sy)))
                self._draw_map_arrowhead(painter, ax + sx, ay + sy, bx + sx, by + sy,
                                         QColor(self.MAP_EDGE_COLOURS[kind]))
        for index in range(cm.COUNT):
            x, y = self._map_node(index)
            selected = index == self.map_index
            fill = "#f2c04c" if selected else ("#8ab4f8" if index >= cm.TOWN_FIRST else "#e8eaed")
            painter.setPen(QColor("#000000"))
            painter.setBrush(QColor(fill))
            painter.drawEllipse(QPoint(round(x), round(y)), 6, 6)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            canvas.targets.append(("place", index, QRect(round(x) - 8, round(y) - 8, 16, 16)))
            right = x > (tx0 + tx1) / 2 if index >= cm.TOWN_FIRST else x > (x0 + x1) / 2 + 60
            name = cm.name(self.project, index).split(" / ")[0] if index < cm.TOWN_FIRST else ""
            label = f"{index} {name}".strip()
            bold = QFont(painter.font())
            bold.setBold(selected)
            painter.setFont(bold)
            if index >= cm.TOWN_FIRST:
                painter.setPen(QColor("#c4c8cd"))
                painter.drawText(QRect(tx0, 284 + (index - cm.TOWN_FIRST) * 15 - 8, 240, 16),
                                 int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter),
                                 f"{index} {cm.name(self.project, index).split(' / ')[0]}")
            box = QRect(round(x) - 9 - 220, round(y) - 9, 220, 18) if right else \
                QRect(round(x) + 9, round(y) - 9, 220, 18)
            flags = int((Qt.AlignmentFlag.AlignRight if right else Qt.AlignmentFlag.AlignLeft) |
                        Qt.AlignmentFlag.AlignVCenter)
            painter.setPen(QColor("#000000"))       # an outline, so a name reads over the terrain
            for ox, oy in ((1, 1), (-1, -1), (1, -1), (-1, 1)):
                painter.drawText(box.translated(ox, oy), flags, label)
            painter.setPen(QColor("#ffffff"))
            painter.drawText(box, flags, label)
        painter.setFont(small)
        m["caption"].setText("Every place and where its exits lead (thicker: to and from the selected one). "
                             "Drag a world site to move where its camera looks, a town place to move its "
                             "marker; click one to edit it. Cancel in the town, once the tournament is over, "
                             "always leads back to Metropolis (not drawn).")
    @staticmethod
    def _draw_map_arrowhead(painter, ax, ay, bx, by, colour):
        import math
        angle = math.atan2(by - ay, bx - ax)
        painter.setBrush(colour)
        painter.setPen(colour)
        tip = QPoint(round(bx), round(by))
        wings = [QPoint(round(bx - 10 * math.cos(angle - 0.35)), round(by - 10 * math.sin(angle - 0.35))),
                 QPoint(round(bx - 10 * math.cos(angle + 0.35)), round(by - 10 * math.sin(angle + 0.35)))]
        painter.drawPolygon([tip, *wings])
        painter.setBrush(Qt.BrushStyle.NoBrush)
    def _map_press(self, point):
        if not self._map_available():
            return
        m = self.workspace_controls["Campaign"]["map"]
        found = m["canvas"].hit(point)
        if found is None:
            self.map_drag = None
            return
        kind, n = found
        if kind == "place" and n != self.map_index:
            self._select_map_place(n)
            found = ("place", n)
        self.map_drag = (found, point, False)
    def _map_move(self, point):
        if not self.map_drag:
            return
        (kind, n), start, _moved = self.map_drag
        dx, dy = point.x() - start.x(), point.y() - start.y()
        if not dx and not dy:
            return
        loc = self.map_state.locations[self.map_index].copy()
        if kind == "exit":
            loc.exits[n].x += round(dx / self.MAP_ZOOM)
            loc.exits[n].y += round(dy / self.MAP_ZOOM)
        elif kind == "marker":
            loc.marker_x += round(dx / self.MAP_ZOOM)
            loc.marker_y += round(dy / self.MAP_ZOOM)
        elif kind == "place":
            if n < cm.TOWN_FIRST:
                _, _, scale, _, _ = self._map_world_frame()
                loc.target_z += round(dx / scale)
                loc.target_x += round(dy / scale)
            else:
                x0, y0, x1, y1 = self.MAP_TOWN_BOX
                loc.marker_x += round(dx * cm.SCREEN[0] / (x1 - x0))
                loc.marker_y += round(dy * cm.SCREEN[1] / (y1 - y0))
        self.map_preview = loc
        self.map_drag = ((kind, n), point, True)
        self._draw_map()
    def _map_release(self, _point):
        if not self.map_drag:
            return
        _found, _start, moved = self.map_drag
        self.map_drag = None
        loc, self.map_preview = self.map_preview, None
        if not moved or loc is None:
            self._draw_map()
            return
        self._store_map(loc)
        self._fill_map_form()
    def _choose_map_reference(self):
        if not self._map_available():
            return
        path, _ = QFileDialog.getOpenFileName(self, "A screenshot of this place in the game", "",
                                              "PNG images (*.png);;All files (*)")
        if not path:
            return
        try:
            picture = pngio.read(path)
        except (OSError, pngio.PngError) as problem:
            QMessageBox.critical(self, "Map", f"Could not read {path}: {problem}")
            return
        self.map_references[self._map_camera(self.map_index)] = picture
        self._draw_map()
    def _show_map_pictures(self):
        """Map pictures: the sprites and the terrain textures, as the Tk
        window's MapPictures window has them."""
        if not self._map_available():
            return
        if self.map_dialog is not None and self.map_dialog.isVisible():
            self._refresh_map_pictures()
            self.map_dialog.raise_()
            return
        dialog = QDialog(self)
        self.map_dialog = dialog
        dialog.setWindowTitle("Map pictures")
        dialog.resize(720, 620)
        layout = QVBoxLayout(dialog)
        d = {}

        sprites = QGroupBox("Sprites (the map's strip: name panel, marker, arrows)")
        sprite_layout = QVBoxLayout(sprites)
        previews = QHBoxLayout()
        sprite_layout.addLayout(previews)
        d["previews"] = previews
        row = QHBoxLayout()
        d["sprite"] = QComboBox()
        d["sprite"].addItems([s[0] for s in map_art.SPRITES])
        row.addWidget(d["sprite"], 1)
        import_one = QPushButton("Import picture…")
        row.addWidget(import_one)
        sprite_layout.addLayout(row)
        row = QHBoxLayout()
        for label, slot in (("Export sprites…", self._export_map_sprites),
                            ("Import sprites…", self._import_map_sprites),
                            ("Revert sprites", self._revert_map_sprites)):
            button = QPushButton(label)
            button.clicked.connect(slot)
            row.addWidget(button)
        row.addStretch(1)
        sprite_layout.addLayout(row)
        note = QLabel(
            "A picture of one sprite goes into every frame of its animation (the marker's 16, an arrow's 10), "
            "so it keeps its motion; an arrow and its mirror share their cells. Export sprites writes the strip "
            "through each palette (sprites-p0.png to p3.png, 256x256, or the mod's at its size) to paint over; "
            "Import sprites takes those files back. Up to 4x: Internal 2x and 4x draw the detail, the console's "
            "resolution averages it down.")
        note.setWordWrap(True)
        note.setStyleSheet("color:#9aacc4")
        sprite_layout.addWidget(note)
        layout.addWidget(sprites)

        terrain = QGroupBox("Terrain textures")
        terrain_layout = QVBoxLayout(terrain)
        row = QHBoxLayout()
        row.addWidget(QLabel("Map"))
        d["package"] = QComboBox()
        d["package"].addItems([cm.PACKAGE_LABELS[name] for name, _ in cm.PACKAGES])
        row.addWidget(d["package"])
        d["count"] = QLabel()
        d["count"].setStyleSheet("color:#9aacc4")
        row.addWidget(d["count"])
        row.addStretch(1)
        terrain_layout.addLayout(row)
        row = QHBoxLayout()
        for label, slot in (("Export textures…", self._export_map_textures),
                            ("Import textures…", self._import_map_textures),
                            ("Revert textures", self._revert_map_textures)):
            button = QPushButton(label)
            button.clicked.connect(slot)
            row.addWidget(button)
        row.addStretch(1)
        terrain_layout.addLayout(row)
        note = QLabel(
            "The terrain is a 3D model with tiled textures (most are drawn in many places), so it is repainted "
            "a texture at a time: Export writes each texture as the map draws it (textureNN-PPPP.png, one per "
            "palette it is drawn with), Import takes the files of a folder with those names. The map before the "
            "coup and the one after are two models with textures of their own.")
        note.setWordWrap(True)
        note.setStyleSheet("color:#9aacc4")
        terrain_layout.addWidget(note)
        layout.addWidget(terrain)

        d["status"] = QLabel()
        d["status"].setWordWrap(True)
        layout.addWidget(d["status"])
        layout.addStretch(1)
        close = QPushButton("Close")
        close.clicked.connect(dialog.accept)
        row = QHBoxLayout()
        row.addStretch(1)
        row.addWidget(close)
        layout.addLayout(row)

        self.map_dialog_controls = d
        import_one.clicked.connect(self._import_map_sprite)
        d["package"].currentIndexChanged.connect(self._refresh_map_pictures)
        self._refresh_map_pictures()
        dialog.show()
    def _refresh_map_pictures(self, *_):
        if self.map_dialog is None or not self._map_available():
            return
        d = self.map_dialog_controls
        previews = d["previews"]
        while previews.count():
            item = previews.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        data = self.map_state.retail
        strips = self._map_strips()
        shown = [("Marker", cm.MARKER), ("Name panel", cm.PANEL)] + \
            [(name, (2, i)) for i, name in enumerate(cm.ARROWS)
             if name in ("right", "down", "up", "up-right")]
        for label, (animation, variant) in shown:
            found = cm.sprite_image(data, animation, variant, strips)
            if found is None:
                continue
            image = found[0]
            column = QVBoxLayout()
            caption = QLabel(label)
            caption.setStyleSheet("color:#9aacc4")
            caption.setAlignment(Qt.AlignmentFlag.AlignHCenter)
            picture = QLabel()
            picture.setPixmap(self._map_pixmap(image, 1 if image.width > 64 else 2))
            picture.setAlignment(Qt.AlignmentFlag.AlignCenter)
            column.addWidget(caption)
            column.addWidget(picture)
            holder = QWidget()
            holder.setLayout(column)
            previews.addWidget(holder)
        previews.addStretch(1)
        package = self._map_dialog_package()
        total = len(map_art.package_textures(data, package))
        mine = sum(1 for t in map_art.state(self.project).textures if t.package == package)
        d["count"].setText(f"{total} textures, {mine} replaced by the mod" if total else
                           "the disc's map model could not be read")
    def _map_art_done(self, notes, what):
        self.map_pictures.clear()
        self._mark_dirty()
        self._draw_map()
        self._refresh_map_pictures()
        self.map_dialog_controls["status"].setText(
            what + ("\n" + "\n".join(notes[:8]) if notes else ""))
    def _import_map_sprite(self):
        label = self.map_dialog_controls["sprite"].currentText()
        _name, animation, variant = next(s for s in map_art.SPRITES if s[0] == label)
        path, _ = QFileDialog.getOpenFileName(self, f"A picture for the {label.lower()}", "",
                                              "PNG images (*.png);;All files (*)")
        if not path:
            return
        try:
            notes = map_art.set_sprite(self.project, animation, variant, pngio.read(path))
        except (OSError, pngio.PngError, ValueError) as problem:
            QMessageBox.critical(self, "Map pictures", f"Could not use {path}: {problem}")
            return
        self._map_art_done(notes, f"{label}: {path}")
    def _export_map_sprites(self):
        folder = QFileDialog.getExistingDirectory(self, "A folder for the sprites")
        if folder:
            written = map_art.export_sprites(self.project, folder)
            self.map_dialog_controls["status"].setText(f"Wrote {len(written)} files to {folder}")
    def _import_map_sprites(self):
        folder = QFileDialog.getExistingDirectory(self, "The folder with sprites-p0.png to p3.png")
        if not folder:
            return
        try:
            notes = map_art.import_sprites(self.project, folder)
        except (OSError, pngio.PngError) as problem:
            QMessageBox.critical(self, "Map pictures", str(problem))
            return
        self._map_art_done(notes, "Nothing named sprites-p0.png to p3.png there." if not notes else "Imported:")
    def _revert_map_sprites(self):
        map_art.revert_sprites(self.project)
        self._map_art_done([], "The sprites are the disc's again.")
    def _export_map_textures(self):
        folder = QFileDialog.getExistingDirectory(self, "A folder for the textures")
        if folder:
            written = map_art.export_textures(self.project, self._map_dialog_package(), folder)
            self.map_dialog_controls["status"].setText(f"Wrote {len(written)} textures to {folder}")
    def _import_map_textures(self):
        folder = QFileDialog.getExistingDirectory(self, "The folder with the textures")
        if not folder:
            return
        try:
            notes = map_art.import_textures(self.project, self._map_dialog_package(), folder)
        except (OSError, pngio.PngError) as problem:
            QMessageBox.critical(self, "Map pictures", str(problem))
            return
        self._map_art_done(notes, "No texture of this map is named there." if not notes else "Imported:")
    def _revert_map_textures(self):
        package = self._map_dialog_package()
        map_art.revert_textures(self.project, package)
        self._map_art_done([], f"The textures of the map {cm.PACKAGE_LABELS[package]} are the disc's again.")

    def _map_package_name(self):
        label = self.workspace_controls["Campaign"]["map"]["package"].currentText()
        return next((name for name, _ in cm.PACKAGES if cm.PACKAGE_LABELS[name] == label), cm.PACKAGES[0][0])
    def _map_package_sector(self):
        name = self._map_package_name()
        return next(sector for other, sector in cm.PACKAGES if other == name)
    def _map_package_changed(self, *_):
        self.map_pictures.clear()
        self._draw_map()
    def _map_dialog_package(self):
        label = self.map_dialog_controls["package"].currentText()
        return next(name for name, _ in cm.PACKAGES if cm.PACKAGE_LABELS[name] == label)
