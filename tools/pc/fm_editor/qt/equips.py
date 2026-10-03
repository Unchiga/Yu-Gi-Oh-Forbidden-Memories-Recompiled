"""The Equips page."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class EquipsMixin:
    def _refresh_equips(self):
        c=self.workspace_controls.get("Equips")
        if not c:return
        self._loading_workspace=True
        equips=c["equips"]; held=sort_paused(equips); equips.setRowCount(0)
        cards=sorted(set(self.project.equip_cards())|set(self.project.equips))
        for cid in cards:
            if cid not in self.project.cards:continue
            row=equips.rowCount(); equips.insertRow(row)
            changed=self.project.equips.get(cid,set())!=self.project.equip_baseline(cid)
            for col,value in enumerate((f"{cid:03d}",self.project.cards[cid].name,
                                        len(self.project.equips.get(cid,set())))):
                item=TableItem(str(value))
                if col==0:item.setData(Qt.ItemDataRole.UserRole,cid)
                self._tint_state(item,"changed" if changed else "")
                equips.setItem(row,col,item)
        sort_resumed(equips,held)
        if getattr(self,"equip_current",None) is not None:
            for row in range(equips.rowCount()):
                if equips.item(row,0).data(Qt.ItemDataRole.UserRole)==self.equip_current:
                    equips.selectRow(row);break
        if equips.currentRow()<0 and equips.rowCount():
            equips.selectRow(0)
            self.equip_current=equips.item(0,0).data(Qt.ItemDataRole.UserRole)
        self._loading_workspace=False
        self._filter_equip_cards(c["search"].text())
        self._fill_equip_monsters()
    def _select_equip(self):
        if self._loading_workspace:
            return
        c=self.workspace_controls["Equips"]; row=c["equips"].currentRow()
        self.equip_current=c["equips"].item(row,0).data(Qt.ItemDataRole.UserRole) if row>=0 else None
        self._fill_equip_monsters()
    def _filter_equip_cards(self, text):
        c=self.workspace_controls.get("Equips")
        if not c:return
        table=c["equips"]; query=text.strip()
        current_row=table.currentRow()
        for row in range(table.rowCount()):
            cid=table.item(row,0).data(Qt.ItemDataRole.UserRole) if table.item(row,0) else None
            table.setRowHidden(row,cid is None or not self._card_matches(cid,query))
        if current_row>=0 and table.isRowHidden(current_row):
            table.clearSelection()
            self.equip_current=None
            self._fill_equip_monsters()
    def _filter_equip_monsters(self, text):
        c=self.workspace_controls.get("Equips")
        if not c:return
        table=c["monsters"]; query=text.strip()
        for row in range(table.rowCount()):
            cid=table.item(row,0).data(Qt.ItemDataRole.UserRole) if table.item(row,0) else None
            table.setRowHidden(row,cid is None or not self._card_matches(cid,query))
    def _fill_equip_monsters(self):
        c=self.workspace_controls.get("Equips")
        if not c:return
        table=c["monsters"]; table.blockSignals(True); held=sort_paused(table); table.setRowCount(0)
        cid=getattr(self,"equip_current",None)
        c["heading"].setText(f"{self.project.card_label(cid)} can equip:" if cid in self.project.cards else "Select an equip card")
        preview=c["preview"]
        if cid in self.project.cards and self.files is not None:
            try:
                pixmap=_card_image(self.project,self.files.wa,cid,self.frame_cache)
                preview.setPixmap(pixmap.scaled(preview.size(),Qt.AspectRatioMode.KeepAspectRatio,
                                                Qt.TransformationMode.FastTransformation))
                preview.setText("")
            except (OSError,ValueError,IndexError):
                preview.setPixmap(QPixmap())
                preview.setText("Card preview unavailable")
        else:
            preview.setPixmap(QPixmap())
            preview.setText("Select an equip card")
        if cid in self.project.cards:
            now=self.project.equips.get(cid,set()); baseline=self.project.equip_baseline(cid)
            for monster in self.project.monsters():
                row=table.rowCount(); table.insertRow(row)
                check=TableItem()
                check.setFlags(Qt.ItemFlag.ItemIsEnabled|Qt.ItemFlag.ItemIsSelectable|Qt.ItemFlag.ItemIsUserCheckable)
                check.setCheckState(Qt.CheckState.Checked if monster in now else Qt.CheckState.Unchecked)
                check.setData(Qt.ItemDataRole.UserRole,monster); table.setItem(row,0,check)
                state=("" if monster in baseline and monster in now else "added" if monster in now
                       else "removed" if monster in baseline else "")
                values=(f"{monster:03d}",self.project.cards[monster].name,TYPE_NAMES[self.project.cards[monster].type],
                        state.title() or ("Stock" if monster in now else ""))
                self._tint_state(check,state)
                for col,value in enumerate(values,1):
                    item=TableItem(str(value));self._tint_state(item,state)
                    table.setItem(row,col,item)
        sort_resumed(table,held)
        table.blockSignals(False)
        self._filter_equip_monsters(c["monster_search"].text())
    def _toggle_equip_monster(self,item):
        if self._loading_workspace or item.column()!=0:return
        equip=getattr(self,"equip_current",None)
        if equip not in self.project.cards:return
        monster=item.data(Qt.ItemDataRole.UserRole)
        allowed=self.project.equips.setdefault(equip,set())
        if item.checkState()==Qt.CheckState.Checked:allowed.add(monster)
        else:allowed.discard(monster)
        self._mark_dirty(); self._refresh_equips()
    def _add_equip_monster(self):
        equip=getattr(self,"equip_current",None)
        if equip not in self.project.cards:return
        cid=self._choose_one_card("Add monster",[i for i in self.project.monsters()])
        if cid:self.project.equips.setdefault(equip,set()).add(cid);self._mark_dirty();self._refresh_equips()
    def _equip_by_type(self,allow):
        equip=getattr(self,"equip_current",None)
        if equip not in self.project.cards:return
        typ=self.workspace_controls["Equips"]["type"].currentIndex()
        members={cid for cid,card in self.project.cards.items() if card.type==typ}
        now=self.project.equips.setdefault(equip,set())
        now.update(members) if allow else now.difference_update(members)
        self._mark_dirty();self._refresh_equips()
    def _remove_equip_monsters(self):
        equip=getattr(self,"equip_current",None)
        if equip not in self.project.cards:return
        for row in sorted({i.row() for i in self.workspace_controls['Equips']['monsters'].selectedItems()}):
            item=self.workspace_controls['Equips']['monsters'].item(row,0)
            if item:self.project.equips.setdefault(equip,set()).discard(item.data(Qt.ItemDataRole.UserRole))
        self._mark_dirty();self._refresh_equips()
    def _revert_equip(self):
        equip=getattr(self,"equip_current",None)
        if equip in self.project.cards:self.project.equips[equip]=self.project.equip_baseline(equip);self._mark_dirty();self._refresh_equips()
