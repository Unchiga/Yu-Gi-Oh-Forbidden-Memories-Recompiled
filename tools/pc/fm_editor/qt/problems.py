"""The Problems page, and going to what a line is about."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class ProblemsMixin:
    def _refresh_problems(self):
        c=self.workspace_controls["Problems"];issues=validate.validate(self.project);table=c["table"]
        c["issues"]=issues
        held=sort_paused(table);table.setRowCount(len(issues))
        for row,issue in enumerate(issues):
            for col,value in enumerate((issue.level.title(),issue.area+" · "+issue.where,issue.message)):
                item=TableItem(str(value))
                item.setData(Qt.ItemDataRole.UserRole,row)      # which issue the line is, once a sort has moved it
                self._tint_state(item,issue.level)      # the level's ink, as the Tk tree's tags draw it
                table.setItem(row,col,item)
        sort_resumed(table,held)
        errors=len(validate.errors(issues));warnings=len(issues)-errors
        c["summary"].setText(f"{errors} error(s) · {warnings} warning(s)" if issues else "No problems found.")
        c["summary"].setStyleSheet("color:#ff8f87" if errors else "color:#f2c04c" if warnings
                                   else "color:#7fd49b")
    def _open_problem(self, row, _column=0):
        """Problems: a double-clicked line goes to what it is about."""
        controls = self.workspace_controls["Problems"]
        issues = controls.get("issues") or []
        # Which issue the line is, rather than which row it sits on: the
        # column headers sort this list like any other.
        item = controls["table"].item(row, 0)
        index = item.data(Qt.ItemDataRole.UserRole) if item is not None else row
        if index is not None and 0 <= index < len(issues):
            self.go_to(issues[index])
    def go_to(self, issue):
        """Show what a validation issue is about, as App.go_to does for the Tk
        window. The pages the modern editor has no form for yet are named
        instead of jumped to."""
        area, target = issue.area, issue.target
        if area == "Starter pools":
            self.select_workspace("Starter decks")
            if self.current_workspace == "Starter decks":
                self.goto_starter_pool(target if isinstance(target, int) else 0)
            return
        if area == "Map":
            self.select_workspace("Campaign")
            if self.current_workspace == "Campaign":
                self.goto_map_place(target if isinstance(target, int) else 0)
            return
        if area not in self.workspace_indices:
            self.statusBar().showMessage(
                f"{area}: {issue.where} — this page is not in the modern editor yet; "
                "its data is under Mod info as JSON.")
            return
        self.select_workspace(area)
        if self.current_workspace != area:      # a pending edit refused to leave its page
            return
        if area == "Cards" and target:
            self.goto_card(target)
        elif area == "Art" and target:
            self.goto_art(target)
        elif area == "Fusions" and target:
            controls = self.workspace_controls["Fusions"]
            controls["changed"].setChecked(False)
            controls["search"].setText(str(target[0]))
        elif area == "Equips" and target:
            controls = self.workspace_controls["Equips"]
            controls["search"].clear()
            self._select_table_id(controls["equips"], target)
        elif area == "Rituals" and target:
            controls = self.workspace_controls["Rituals"]
            controls["search"].clear()
            self._select_table_id(controls["cards"], target)
        elif area == "Duelists" and target:
            self.goto_duelist(*target)
        elif area == "Starter decks" and isinstance(target, int):
            controls = self.workspace_controls["Starter decks"]
            controls["starter_tabs"].setCurrentIndex(0)
            self._select_table_id(controls["decks"], target)
        elif area == "Packs" and isinstance(target, int):
            self.goto_pack(target)
