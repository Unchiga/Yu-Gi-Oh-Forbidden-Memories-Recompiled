"""The Rituals page."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class RitualsMixin:
    def _refresh_rituals(self):
        c=self.workspace_controls["Rituals"]; cards=c["cards"]
        previous=getattr(self,"ritual_current",None)
        self._loading_workspace=True
        held=sort_paused(cards)
        cards.setRowCount(0)
        rituals=sorted(set(self.project.ritual_cards())|set(self.project.rituals)|set(self.project.retail.rituals))
        for ritual in rituals:
            if ritual not in self.project.cards:continue
            row=cards.rowCount();cards.insertRow(row)
            id_item=TableItem(f"{ritual:03d}")
            id_item.setData(Qt.ItemDataRole.UserRole,ritual)
            cards.setItem(row,0,id_item)
            cards.setItem(row,1,TableItem(self.project.cards[ritual].name))
            state=self.project.ritual_status(ritual).title() or "Stock"
            status_item=TableItem(state)
            self._tint_state(status_item,self.project.ritual_status(ritual))
            cards.setItem(row,2,status_item)
            cards.setItem(row,3,TableItem(self._ritual_recipe_text(ritual)))
        sort_resumed(cards,held)
        if previous is not None:
            self._select_table_id(cards,previous)
        if cards.currentRow()<0 and cards.rowCount():
            cards.selectRow(0)
        self.ritual_current=self._selected_ritual()
        self._loading_workspace=False
        self._filter_ritual_cards(c["search"].text())
        self._refresh_ritual_detail()
    def _select_ritual(self):
        if self._loading_workspace:return
        self.ritual_current=self._selected_ritual()
        self._refresh_ritual_detail()
    def _filter_ritual_cards(self,text):
        c=self.workspace_controls.get("Rituals")
        if not c:return
        cards=c["cards"];query=text.strip().casefold()
        for row in range(cards.rowCount()):
            name=cards.item(row,1).text() if cards.item(row,1) else ""
            cards.setRowHidden(row,query not in name.casefold())
        row=cards.currentRow()
        if row>=0 and cards.isRowHidden(row):
            cards.clearSelection();self.ritual_current=None;self._refresh_ritual_detail()
    def _refresh_ritual_detail(self):
        c=self.workspace_controls["Rituals"]; ritual=getattr(self,"ritual_current",None)
        for image,fields in zip(c["tribute_images"],c["tribute_fields"]):
            image.setPixmap(QPixmap());image.setText("—")
            self._set_ritual_fields(fields,None)
        c["summon_image"].setPixmap(QPixmap());c["summon_image"].setText("—")
        self._set_ritual_fields(c["summon_fields"],None)
        if ritual not in self.project.cards:
            c["heading"].setText("Select a ritual card")
            c["preview"].setPixmap(QPixmap());c["preview"].setText("Select a ritual card")
            self._set_ritual_fields(c["card_meta"],None)
            for fields in c["tribute_fields"]+[c["summon_fields"]]:self._set_ritual_fields(fields,None)
            c["state"].setText("State: —")
            return
        c["heading"].setText(f"Ritual Recipe (Tributes → Result) · {self.project.card_label(ritual)}")
        self._set_ritual_fields(c["card_meta"],ritual)
        if self.files is not None:
            try:
                image=_card_image(self.project,self.files.wa,ritual,self.frame_cache)
                c["preview"].setPixmap(image.scaled(c["preview"].size(),Qt.AspectRatioMode.KeepAspectRatio,
                                                      Qt.TransformationMode.FastTransformation))
                c["preview"].setText("")
            except (OSError,ValueError,IndexError):
                c["preview"].setPixmap(QPixmap());c["preview"].setText("Card preview unavailable")
        now=self.project.rituals.get(ritual)
        state=self.project.ritual_status(ritual).title()
        recipe=self.ritual_pending.get(ritual,now or (None,None,None,None))
        for combo,cid in zip(c["tribute_combos"]+[c["summon_combo"]],recipe):
            combo.blockSignals(True)
            combo.setCurrentIndex(max(0,combo.findData(cid)))
            combo.blockSignals(False)
        for index,cid in enumerate(recipe[:3]):
            if not cid:continue
            self._set_ritual_fields(c["tribute_fields"][index],cid)
            if self.files is not None:
                try:
                    image=_card_image(self.project,self.files.wa,cid,self.frame_cache)
                    c["tribute_images"][index].setPixmap(image.scaled(
                        c["tribute_images"][index].size(),Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.FastTransformation))
                    c["tribute_images"][index].setText("")
                except (OSError,ValueError,IndexError):
                    c["tribute_images"][index].setText("Preview unavailable")
        summoned=recipe[3]
        if summoned:
            self._set_ritual_fields(c["summon_fields"],summoned)
            if self.files is not None:
                try:
                    image=_card_image(self.project,self.files.wa,summoned,self.frame_cache)
                    c["summon_image"].setPixmap(image.scaled(
                        c["summon_image"].size(),Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.FastTransformation))
                    c["summon_image"].setText("")
                except (OSError,ValueError,IndexError):
                    c["summon_image"].setText("Preview unavailable")
        pending=self.ritual_pending.get(ritual)
        pending_changed=pending is not None and pending!=now
        conditions=self.project.ritual_requirements.get(ritual)
        for combo in c["tribute_combos"]:combo.setEnabled(not conditions)
        if conditions:
            c["state"].setText("State: " + (state or "Stock")
                               + "  ·  conditions: " + self._ritual_recipe_text(ritual)
                               + "   (Edit recipe… to change them)")
        else:
            c["state"].setText("State: Pending" if pending_changed else f"State: {state or 'Stock'}")
    def _ritual_tribute_text(self, requirement) -> str:
        """One tribute's conditions in a line, as the Tk list writes them."""
        p = self.project
        parts = []
        if requirement.get("card"):
            parts.append(p.card_label(requirement["card"]))
        if requirement.get("type") is not None:
            parts.append(str(requirement["type"]))
        if requirement.get("fusion_group") is not None:
            parts.append(f'Group: {requirement["fusion_group"]}')
        for key, text in (("min_attack", "ATK ≥ {}"), ("min_defense", "DEF ≥ {}"),
                          ("max_attack", "ATK ≤ {}"), ("max_defense", "DEF ≤ {}"),
                          ("min_level", "Level ≥ {}"), ("max_level", "Level ≤ {}")):
            if requirement.get(key) is not None:
                parts.append(text.format(requirement[key]))
        if requirement.get("defense_gt_attack"):
            parts.append("DEF > ATK")
        return " & ".join(parts) or "-"
    def _ritual_recipe_text(self, ritual) -> str:
        """The whole recipe in a line: its three tributes and what it summons.
        An added copy without one of its own shows its base's."""
        p = self.project
        conditions = p.ritual_requirements.get(ritual)
        recipe = p.rituals.get(ritual)
        if recipe is None and ritual in p.added and not conditions:
            recipe = p.rituals.get(p.base_of(ritual))
        recipe = recipe or (None, None, None, None)
        if conditions:
            tributes = [self._ritual_tribute_text(r) for r in conditions]
        else:
            tributes = [p.card_label(c) if c else "-" for c in recipe[:3]]
        return " + ".join(tributes) + " → " + (p.card_label(recipe[3]) if recipe[3] else "-")
    def _stage_ritual_selector_recipe(self, *_):
        ritual=getattr(self,"ritual_current",None)
        if ritual not in self.project.cards:return
        c=self.workspace_controls["Rituals"]
        values=tuple(combo.currentData() for combo in c["tribute_combos"]+[c["summon_combo"]])
        self.ritual_pending[ritual]=values
        self._refresh_ritual_detail()
    def _apply_ritual_selector_recipe(self, *_):
        ritual=getattr(self,"ritual_current",None)
        if ritual not in self.project.cards:return
        values=self.ritual_pending.get(ritual)
        if values is None:return
        if not all(values):
            self.statusBar().showMessage("Choose all three materials and a summoned card before applying the recipe",5000)
            return
        current=self.project.rituals.get(ritual)
        if current!=values or ritual in self.project.ritual_requirements:
            self.project.rituals[ritual]=values
            # Named cards are a traditional recipe: the conditions it had go.
            self.project.ritual_requirements.pop(ritual,None)
            self._mark_dirty()
        self.ritual_pending.pop(ritual,None)
        self._refresh_rituals()
    def _set_ritual_fields(self,fields,cid):
        card=self.project.cards.get(cid) if cid is not None else None
        if fields.get("id") is not None:fields["id"].setText(f"{cid:03d}" if card is not None else "")

        for key,choices,value in (("type",TYPE_NAMES,card.type if card else -1),
                                  ("attribute",ATTRIBUTE_NAMES,card.attribute if card else -1)):
            box=fields.get(key)
            if box is not None:
                if value>=0 and value<len(choices):box.setCurrentIndex(value)
                else:box.setCurrentIndex(-1)
        for key,value in (("level",card.level if card else 0),("atk",card.attack if card else 0),
                          ("def",card.defense if card else 0)):
            if fields.get(key) is not None:fields[key].setValue(value)
    def _ritual_card_summary(self,cid):
        card=self.project.cards.get(cid)
        if card is None:return "Card data unavailable"
        card_type=TYPE_NAMES[card.type] if 0<=card.type<len(TYPE_NAMES) else "Unknown type"
        lines=[f"ID {cid:03d}  ·  {card_type}"]
        if card.is_monster():
            attribute=ATTRIBUTE_NAMES[card.attribute] if 0<=card.attribute<len(ATTRIBUTE_NAMES) else "Unknown"
            star1=STAR_NAMES[card.star1] if 0<=card.star1<len(STAR_NAMES) else "Unknown"
            star2=STAR_NAMES[card.star2] if 0<=card.star2<len(STAR_NAMES) else "Unknown"
            lines.extend((f"{attribute}  ·  Level {card.level}",
                          f"ATK {card.attack}  ·  DEF {card.defense}",
                          f"Stars: {star1} / {star2}"))
        return "\n".join(lines)
    def _selected_ritual(self):
        table=self.workspace_controls["Rituals"]["cards"];row=table.currentRow()
        if row<0 or table.isRowHidden(row):return None
        item=table.item(row,0) if row>=0 else None
        return item.data(Qt.ItemDataRole.UserRole) if item else None
    def _edit_ritual(self, *_):
        """The recipe: three tributes, each either a named card or a set of
        conditions the monster on the field must meet, and what it summons
        (a port of tabs.RitualsTab.edit)."""
        ritual = self._selected_ritual()
        if ritual is None:
            return
        p = self.project
        # An added copy starts from its base's recipe, as the game reads it.
        recipe = (p.rituals.get(ritual) or p.retail.rituals.get(ritual)
                  or p.rituals.get(p.base_of(ritual)) or (0, 0, 0, 0))
        saved = p.ritual_requirements.get(ritual)
        requirements = [dict(r) for r in saved] if saved else \
            [{"card": recipe[i]} if recipe[i] else {} for i in range(3)]

        dialog = QDialog(self)
        dialog.setWindowTitle(f"Ritual recipe · {p.card_label(ritual)}")
        dialog.resize(720, 620)
        layout = QVBoxLayout(dialog)
        heading = QLabel(p.card_label(ritual))
        heading.setStyleSheet("font-size:16px;font-weight:650;color:#f3f7fc")
        layout.addWidget(heading)
        note = QLabel("Each tribute is a monster on the field. Name a card for the disc's own kind of "
                      "recipe, or give conditions it must meet instead.")
        note.setWordWrap(True)
        note.setStyleSheet("color:#9aacc4")
        layout.addWidget(note)
        tabs = QTabWidget()
        layout.addWidget(tabs, 1)
        panels = []
        for index in range(3):
            holder = QWidget()
            QVBoxLayout(holder)
            tabs.addTab(holder, f"Tribute {index + 1}")
            panels.append(holder)
        problem = QLabel()
        problem.setWordWrap(True)
        problem.setStyleSheet("color:#ff7777")

        def editors_of(index):
            return panels[index].property("editors") or {}

        def render(index):
            holder = panels[index]
            old = holder.layout()
            while old.count():
                item = old.takeAt(0)
                if item.widget() is not None:
                    item.widget().deleteLater()
            editors = {}
            requirement = requirements[index]
            for label, key, number in self.RITUAL_CONDITIONS:
                if key not in requirement:
                    continue
                row = QHBoxLayout()
                caption = QLabel(label)
                caption.setMinimumWidth(130)
                row.addWidget(caption)
                if key == "card":
                    field = self._card_combo(holder, requirement[key] or None, ids=p.monsters())
                    row.addWidget(field, 1)
                    editors[key] = field
                elif key in ("type", "fusion_group"):
                    field = QComboBox()
                    field.addItems(list(TYPE_NAMES[:20] if key == "type" else FUSION_GROUPS))
                    found = field.findText(str(requirement[key]))
                    field.setCurrentIndex(found if found >= 0 else 0)
                    row.addWidget(field, 1)
                    editors[key] = field
                elif number is not None:
                    low, high, _start = number
                    field = QSpinBox()
                    field.setRange(low, high)
                    field.setValue(int(requirement[key]) if str(requirement[key]).lstrip("-").isdigit() else low)
                    field.setMaximumWidth(120)
                    row.addWidget(field)
                    row.addStretch(1)
                    editors[key] = field
                else:
                    held = QLabel("Required")
                    held.setStyleSheet("color:#9aacc4")
                    row.addWidget(held)
                    row.addStretch(1)
                remove = QPushButton("Remove")
                remove.setEnabled(len(requirement) > 1)
                remove.clicked.connect(lambda _checked=False, n=index, k=key: drop(n, k))
                row.addWidget(remove)
                holder.layout().addLayout(row)
            holder.setProperty("editors", editors)
            add = QPushButton("+ Add condition")
            add.clicked.connect(lambda _checked=False, n=index, button=add: offer(n, button))
            holder.layout().addWidget(add, 0, Qt.AlignmentFlag.AlignLeft)
            holder.layout().addStretch(1)
            tabs.setTabText(index, f"Tribute {index + 1}"
                            + (f"  ·  {self._ritual_tribute_text(requirement)}"[:34]
                               if requirement else "  ·  (empty)"))

        def collect(index):
            """What the tribute's boxes say, into its requirement."""
            requirement = requirements[index]
            for key, field in editors_of(index).items():
                if key == "card":
                    requirement[key] = self._combo_card_id(field) or 0
                elif key in ("type", "fusion_group"):
                    requirement[key] = field.currentText()
                else:
                    requirement[key] = field.value()

        def drop(index, key):
            collect(index)
            if len(requirements[index]) <= 1:
                return
            requirements[index].pop(key, None)
            render(index)

        def offer(index, button):
            collect(index)
            menu = QMenu(dialog)
            for label, key, _number in self.RITUAL_CONDITIONS:
                action = menu.addAction(label)
                action.setEnabled(key not in requirements[index])
                action.triggered.connect(lambda _checked=False, n=index, k=key: add_condition(n, k))
            menu.exec(button.mapToGlobal(button.rect().bottomLeft()))

        def add_condition(index, key):
            requirement = requirements[index]
            if key in requirement:
                return
            if key == "card":
                chosen = self._choose_one_card("Specific ritual tribute", ids=p.monsters())
                if not chosen:
                    return
                requirement[key] = chosen
            elif key == "type":
                requirement[key] = TYPE_NAMES[0]
            elif key == "fusion_group":
                requirement[key] = FUSION_GROUPS[0]
            elif key == "defense_gt_attack":
                requirement[key] = True
            else:
                requirement[key] = next(n[2] for _l, k, n in self.RITUAL_CONDITIONS if k == key)
            render(index)

        for index in range(3):
            render(index)
        summon_row = QHBoxLayout()
        summon_label = QLabel("Summons")
        summon_label.setMinimumWidth(130)
        summon_row.addWidget(summon_label)
        summon = self._card_combo(dialog, recipe[3] or None, ids=p.monsters())
        summon_row.addWidget(summon, 1)
        layout.addLayout(summon_row)
        layout.addWidget(problem)
        buttons = QHBoxLayout()
        buttons.addStretch(1)
        save = QPushButton("Save")
        save.setStyleSheet("background:#216cf1;border-color:#216cf1;color:white;font-weight:600")
        cancel = QPushButton("Cancel")
        buttons.addWidget(cancel)
        buttons.addWidget(save)
        layout.addLayout(buttons)
        cancel.clicked.connect(dialog.reject)

        def fail(index, text):
            problem.setText(f"Tribute {index + 1}: {text}")
            tabs.setCurrentIndex(index)

        def accept():
            for index in range(3):
                collect(index)
            for index, requirement in enumerate(requirements):
                if not requirement:
                    return fail(index, "needs at least one condition.")
                if "card" in requirement and not requirement["card"]:
                    return fail(index, "name a card, or remove the Specific card condition.")
                for low, high, what in (("min_attack", "max_attack", "ATK"),
                                        ("min_defense", "max_defense", "DEF"),
                                        ("min_level", "max_level", "level")):
                    if requirement.get(low) is not None and requirement.get(high) is not None \
                            and requirement[low] > requirement[high]:
                        return fail(index, f"the minimum {what} is past the maximum.")
            summoned = self._combo_card_id(summon)
            if not summoned:
                problem.setText("Summons must name a monster.")
                return
            dialog.accept()

        save.clicked.connect(accept)
        if not dialog.exec():
            return
        shown = [requirement.get("card", 0) for requirement in requirements]
        p.rituals[ritual] = tuple(shown + [self._combo_card_id(summon)])
        # Three named cards and nothing else is the disc's own kind of recipe,
        # which needs no conditions written beside it.
        if all(set(requirement) == {"card"} for requirement in requirements):
            p.ritual_requirements.pop(ritual, None)
        else:
            p.ritual_requirements[ritual] = [dict(r) for r in requirements]
        self.ritual_pending.pop(ritual, None)
        self._mark_dirty()
        self._refresh_rituals()
    def _remove_ritual(self):
        ritual=self._selected_ritual()
        if ritual is not None:
            self.ritual_pending.pop(ritual,None)
            self.project.rituals.pop(ritual,None)
            self.project.ritual_requirements.pop(ritual,None)
            self._mark_dirty();self._refresh_rituals()
    def _revert_ritual(self):
        ritual=self._selected_ritual()
        if ritual is None:return
        self.ritual_pending.pop(ritual,None)
        self.project.revert_ritual(ritual)      # clears its conditions as well
        self._mark_dirty();self._refresh_rituals()
