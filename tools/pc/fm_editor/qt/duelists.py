"""The Duelists page, its pools and its fixed decks (fixed_deck_view)."""
from __future__ import annotations

from .common import *      # noqa: F401,F403
from .common import (_qimage, _line_count, _pairs_text, _parse_pairs, _whole,
                     _card_image)      # noqa: F401


class DuelistsMixin:
    @staticmethod
    def _put_rows(table, rows):
        held = sort_paused(table)
        table.setRowCount(0)
        table.setRowCount(len(rows))
        for row, values in enumerate(rows):
            for col, value in enumerate(values):
                table.setItem(row, col, TableItem(str(value)))
        sort_resumed(table, held)
    def _duelist_roster_source(self):
        """Read inline, file-backed, or folder duelist definitions without losing their source form."""
        cached=getattr(self,"_duelist_entries_cache",None)
        if cached is not None and getattr(self,"_duelist_entries_cache_project",None) is self.project:
            return cached
        raw=self.project.other.get("duelists")
        entries=[];sources={};mode="inline";path=None
        if isinstance(raw,list):
            entries=[dict(e) for e in raw if isinstance(e,dict)]
            mode="inline"
        elif isinstance(raw,str):
            path=raw;mode="file"
            source=Path(self.project.source_dir or "") / raw
            try:
                blob=self.project.files.get(raw)
                loaded=json.loads(blob.decode("utf-8-sig") if blob is not None else source.read_text(encoding="utf-8-sig"))
                if isinstance(loaded,list):entries=[dict(e) for e in loaded if isinstance(e,dict)]
            except (OSError,ValueError):
                pass
        else:
            # New projects use the framework's one-duelist-per-file format,
            # even before the `duelists/` directory exists on disk.
            mode="folder"
            folder=Path(self.project.source_dir or "") / "duelists"
            files={file.relative_to(self.project.source_dir).as_posix():file for file in folder.glob("*.json")} if self.project.source_dir and folder.is_dir() else {}
            for relative,blob in self.project.files.items():
                if relative.startswith("duelists/") and relative.endswith(".json"):
                    files.setdefault(relative,folder/Path(relative).name)
            for relative,file in sorted(files.items()):
                try:
                    blob=self.project.files.get(relative)
                    loaded=json.loads(blob.decode("utf-8-sig") if blob is not None else file.read_text(encoding="utf-8-sig"))
                    entry=loaded
                    if isinstance(entry,dict):
                        # In the folder format, the filename is the ID; an
                        # `id` property in the JSON is ignored by the game.
                        entry["id"]=file.stem;entries.append(entry);sources[file.stem]=relative
                except (OSError,ValueError):
                    continue
        result=(entries,mode,path,sources)
        self._duelist_entries_cache=result;self._duelist_entries_cache_project=self.project
        return result
    @staticmethod
    def _replaces_slot(entry, slot) -> bool:
        """Whether this entry replaces that duelist, however it names one: by
        number or by name in any case, as the port reads it (duelist_named).
        Matching the name alone added a second entry for the same duelist."""
        from ..model import duelist_named
        value=entry.get("replace")
        if value is None or isinstance(value,bool):
            return False
        if isinstance(value,int):
            return value==slot
        return duelist_named(value)==slot

    @staticmethod
    def _duelist_slug(text):
        import re
        slug=re.sub(r"[^a-z0-9]+","-",str(text).lower()).strip("-")
        return slug or "new-duelist"
    def _duelist_layout(self):
        entries,mode,path,sources=self._duelist_roster_source()
        # Slot 000 is Deck Build, not an opponent: preserve the empty grid
        # position but never expose it as an editable duelist.
        slots={i:{"slot":i,"name":DUELIST_NAMES[i],"base":i,"kind":"stock","entry":None}
               for i in range(1,min(40,len(DUELIST_NAMES)))}
        # Replacements occupy their retail positions and do not take an added slot.
        for entry in entries:
            if "replace" not in entry:continue
            from ..model import duelist_named
            d=duelist_named(entry.get("replace"))
            if 0<d<40:
                slots[d]={"slot":d,"name":str(entry.get("name") or DUELIST_NAMES[d]),"base":d,
                          "kind":"replacement","entry":entry}
        # Reserve all explicit positions first, matching the framework's placement pass.
        added=[]
        for entry in entries:
            if "replace" in entry:continue
            slot=entry.get("slot")
            if isinstance(slot,int) and not isinstance(slot,bool) and 40<=slot<128 and slot not in slots:
                slots[slot]={"slot":slot,"name":str(entry.get("name") or entry.get("id") or "Duelist"),
                             "base":self._duelist_base_index(entry.get("copy")),"kind":"added","entry":entry}
            else:added.append(entry)
        for entry in added:
            free=next((slot for slot in range(40,128) if slot not in slots),None)
            if free is None:break
            slots[free]={"slot":free,"name":str(entry.get("name") or entry.get("id") or "Duelist"),
                         "base":self._duelist_base_index(entry.get("copy")),"kind":"added","entry":entry}
        self._duelist_entries_cache=(entries,mode,path,sources);self._duelist_entries_cache_project=self.project
        return slots
    @staticmethod
    def _duelist_base_index(value):
        from ..model import duelist_named
        if value is None:return 1
        if isinstance(value,int) and 1<=value<len(DUELIST_NAMES):return value
        found=duelist_named(value)
        return found if found>0 else 1
    def _duelist_portrait_pixmap(self, record):
        entry=record.get("entry") or {}
        portrait=entry.get("portrait")
        if portrait:
            blob=self.project.files.get(portrait)
            if blob is None and self.project.source_dir:
                try:blob=(Path(self.project.source_dir)/portrait).read_bytes()
                except OSError:blob=None
            if blob:
                pix=QPixmap()
                if pix.loadFromData(blob):return pix
        base=int(record.get("base",1))
        if not 0<=base<40:return QPixmap()
        # Free Duel portraits can also be replaced by texture-pack entries
        # targeting their WA_MRG.MRG records.
        try:
            texture_state=art.state(self.project)
            pack_entries=texture_state.entries
            if pack_entries is None:
                manifest_path=f"{art.pack_dir(self.project)}/manifest.json"
                manifest_blob=self.project.files.get(manifest_path)
                if manifest_blob:
                    pack_entries=json.loads(manifest_blob.decode("utf-8-sig"))
            portrait_offset=0xF55000+base*0x980
            for texture in pack_entries or ():
                if (not isinstance(texture,dict) or
                        str(texture.get("archive","")).upper()!="WA_MRG.MRG" or
                        not art.contained(texture.get("file"))):
                    continue
                try:offset=int(str(texture.get("offset","-1")),0)
                except ValueError:continue
                if offset!=portrait_offset:continue
                relative=f"{art.pack_dir(self.project)}/{texture['file']}"
                blob=self.project.files.get(relative)
                if blob is None:
                    folder=texture_state.folder or self.project.source_dir
                    if folder is not None:
                        blob=(Path(folder)/relative).read_bytes()
                if blob:
                    pix=QPixmap()
                    if pix.loadFromData(blob):return pix
                break
        except (OSError,ValueError,TypeError,json.JSONDecodeError):
            pass
        try:
            wa=self.files.wa;offset=0xF55000+base*0x980
            palette=image_extract.read_palette(wa,offset+0x900,64)
            palette.extend([0]*(256-len(palette)))
            width,height,rgba=image_extract.decode(wa,offset,24,48,8,palette)
            return QPixmap.fromImage(_qimage(width,height,rgba))
        except (IndexError,ValueError,struct.error):
            return QPixmap()
    def _refresh_duelists(self,*_):
        c=self.workspace_controls["Duelists"];grid=c["duelists"]
        self.duelist_slots=self._duelist_layout()
        if not hasattr(self,"duelist_selected_slot"):self.duelist_selected_slot=1
        page=c["page"].currentIndex();query=c["search"].text().strip().casefold()
        grid.clearContents();grid.setRowCount(5);grid.setColumnCount(8)
        for cell in range(40):
            slot=page*40+cell;row,col=divmod(cell,8);record=self.duelist_slots.get(slot)
            item=TableItem("");item.setData(Qt.ItemDataRole.UserRole,slot)
            if slot==0:item.setFlags(Qt.ItemFlag.NoItemFlags)
            grid.setItem(row,col,item)
            if record is None:
                continue
            if query and query not in record["name"].casefold() and query not in str(slot) and query not in f"{slot:03d}":
                continue
            tile=QWidget();tile.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents,True)
            tile_layout=QVBoxLayout(tile);tile_layout.setContentsMargins(2,2,2,2);tile_layout.setSpacing(1)
            portrait=QLabel();portrait.setFixedSize(54,54);portrait.setAlignment(Qt.AlignmentFlag.AlignCenter)
            pix=self._duelist_portrait_pixmap(record)
            if not pix.isNull():portrait.setPixmap(pix.scaled(portrait.size(),Qt.AspectRatioMode.KeepAspectRatio,Qt.TransformationMode.FastTransformation))
            else:portrait.setText("—")
            label=QLabel(f"{slot:03d} · {record['name']}");label.setAlignment(Qt.AlignmentFlag.AlignCenter);label.setWordWrap(True)
            # A duelist the mod has touched reads as touched, as the Tk list's
            # "changed"/"fixed" column says.
            base=int(record.get("base",0) or 0)
            state=""
            if 0<=base<len(self.project.pools):
                if fixed_decks.deck_of(self.project,base) is not None:state="fixed"
                elif any({c:w for c,w in self.project.pools[base][pool].items() if w}
                         !=self.project.retail.pools[base][pool] for pool in POOLS):state="changed"
            if record["kind"]=="added":state="added"
            if state:
                ink=self._state_colour("added" if state in ("added","fixed") else state)
                if ink is not None:label.setStyleSheet(f"color:{ink.name()}")
            label.setToolTip(record["name"]+(f"  ({state})" if state else ""))
            tile_layout.addWidget(portrait,0,Qt.AlignmentFlag.AlignHCenter);tile_layout.addWidget(label)
            grid.setCellWidget(row,col,tile)
        current=getattr(self,"duelist_selected_slot",1)
        cell=current-page*40
        if not 0<=cell<40 or current==0:
            cell=1 if page==0 else 0
            self.duelist_selected_slot=page*40+cell
        grid.setCurrentCell(cell//8,cell%8)
        self._select_duelist(grid.currentRow(),grid.currentColumn(),refresh_grid=False)
    def _selected_duelist(self):
        slot=getattr(self,"duelist_selected_slot",0)
        record=getattr(self,"duelist_slots",{}).get(slot)
        return (record or {}).get("base",1)
    def _select_duelist(self,row=None,column=None,refresh_grid=True):
        c=self.workspace_controls["Duelists"]
        if row is None or row<0:row=c["duelists"].currentRow()
        if column is None or column<0:column=c["duelists"].currentColumn()
        if row<0 or column<0:return
        slot=c["page"].currentIndex()*40+row*8+column
        if slot==0:return
        if slot != getattr(self,"duelist_selected_slot",None):
            self._duelist_portrait_pending=None
        self.duelist_selected_slot=slot
        self.duelist_current=self._selected_duelist()
        record=self.duelist_slots.get(slot)
        c["remove"].setEnabled(bool(record and record["kind"]=="added"))
        c["base"].setEnabled(record is None or record["kind"]=="added")
        if record:
            c["name"].setText(record["name"])
            entry=record.get("entry") or {}
            c["id"].setText(str(entry.get("id") or ""))
            c["id"].setReadOnly(record["kind"] in ("added","replacement"))
            base_index=max(1,int(record.get("base",1)))
            c["base"].setCurrentIndex(max(0,c["base"].findData(base_index)))
            wanted=entry.get("slot") if record["kind"]=="added" else None
            c["position"].setCurrentIndex(max(0,c["position"].findData(wanted)))
            pix=self._duelist_portrait_pixmap(record)
            c["portrait"].setPixmap(pix.scaled(c["portrait"].size(),Qt.AspectRatioMode.KeepAspectRatio,Qt.TransformationMode.SmoothTransformation) if not pix.isNull() else QPixmap())
            c["portrait"].setText("" if not pix.isNull() else "Portrait")
        else:
            c["name"].clear();c["id"].clear();c["id"].setReadOnly(False)
            c["portrait"].setPixmap(QPixmap());c["portrait"].setText("Portrait")
            c["base"].setCurrentIndex(0);c["position"].setCurrentIndex(max(0,c["position"].findData(slot if slot>=40 else None)))
        self._refresh_duelist_pool()
    def _choose_duelist_portrait(self):
        path,_=QFileDialog.getOpenFileName(self,"Choose duelist portrait","","PNG images (*.png)")
        if not path:return
        image=QImage(path)
        if image.isNull():
            QMessageBox.warning(self,"Invalid portrait","Choose a readable PNG image.");return
        self._duelist_portrait_pending=Path(path).read_bytes()
        pix=QPixmap.fromImage(image)
        label=self.workspace_controls["Duelists"]["portrait"]
        label.setPixmap(pix.scaled(label.size(),Qt.AspectRatioMode.KeepAspectRatio,Qt.TransformationMode.SmoothTransformation));label.setText("")
    def _store_duelist_entries(self,entries):
        _,mode,path,sources=getattr(self,"_duelist_entries_cache",( [],"inline",None,{}))
        if mode=="inline":
            self.project.other["duelists"]=entries
        elif mode=="file":
            self.project.files[path]=json.dumps(entries,ensure_ascii=False,indent=4).encode("utf-8")+b"\n"
        else:
            for entry in entries:
                duel_id=str(entry.get("id") or "")
                relative=sources.get(duel_id,f"duelists/{duel_id}.json")
                body=dict(entry)
                # The folder format derives IDs from the filename and expects
                # only duelist properties (copy/name/slot/portrait) in JSON.
                body.pop("id",None)
                self.project.files[relative]=json.dumps(body,ensure_ascii=False,indent=4).encode("utf-8")+b"\n"
    def _save_duelist(self,adding=False):
        c=self.workspace_controls["Duelists"]
        name=c["name"].text().strip()
        if not name:
            QMessageBox.warning(self,"Duelist name required","Enter a name for this duelist.");return
        entries,mode,path,sources=self._duelist_roster_source();self._duelist_entries_cache=(entries,mode,path,sources)
        slot=getattr(self,"duelist_selected_slot",None);record=self.duelist_slots.get(slot) if slot is not None else None
        raw_id=c["id"].text().strip()
        # An id the mod already wrote stays exactly as written: slugging it
        # here renamed "Dark_Simon" to "dark-simon" and left its roster, deck,
        # drop and portrait files behind under the old one.
        written=str(((record or {}).get("entry") or {}).get("id") or "")
        duelist_id=raw_id if raw_id and raw_id==written else self._duelist_slug(raw_id or name)
        existing=next((e for e in entries if str(e.get("id","")).casefold()==duelist_id.casefold()),None)
        if adding:
            if existing:
                QMessageBox.warning(self,"ID already used",f"The ID {duelist_id!r} is already in use.");return
            base=c["base"].currentData()
            if base is None:
                QMessageBox.warning(self,"Choose a base","Choose a stock duelist to copy.");return
            chosen_slot=c["position"].currentData()
            if chosen_slot is not None:
                if chosen_slot in self.duelist_slots:
                    QMessageBox.warning(self,"Position occupied",f"Grid position {chosen_slot} is occupied. Choose another position or Automatic.");return
            entry={"id":duelist_id,"copy":int(base),"name":name}
            if chosen_slot is not None:entry["slot"]=int(chosen_slot)
            entries.append(entry)
            target_id=duelist_id
            slot=chosen_slot
        else:
            if record is None:
                QMessageBox.warning(self,"Choose a duelist","Select a duelist to edit, or use Add duelist for an empty position.");return
            if record["kind"] in ("added","replacement"):
                entry=record["entry"]
                old_id=str(entry.get("id") or "")
                if duelist_id!=old_id and existing:
                    QMessageBox.warning(self,"ID already used",f"The ID {duelist_id!r} is already in use.");return
                entry["id"]=duelist_id;entry["name"]=name
                if record["kind"]=="added":
                    base=c["base"].currentData()
                    if base is None:
                        QMessageBox.warning(self,"Choose a base","Choose a stock duelist to copy.");return
                    entry["copy"]=int(base)
                    chosen_slot=c["position"].currentData()
                    if chosen_slot is not None and chosen_slot in self.duelist_slots and chosen_slot!=slot:
                        QMessageBox.warning(self,"Position occupied",f"Grid position {chosen_slot} is occupied.");return
                    if chosen_slot is None:entry.pop("slot",None)
                    else:entry["slot"]=int(chosen_slot)
                target_id=duelist_id
            else:
                if slot == 0 and record["kind"] == "stock":
                    QMessageBox.warning(self,"Reserved position","Position 000 is reserved for Deck Build and cannot be replaced.");return
                entry=next((e for e in entries if self._replaces_slot(e,slot)),None)
                if entry is not None:
                    entry["name"]=name;entry["id"]=duelist_id
                else:
                    entry={"id":duelist_id,"replace":DUELIST_NAMES[slot],"name":name};entries.append(entry)
                target_id=duelist_id
        portrait=getattr(self,"_duelist_portrait_pending",None)
        if portrait:
            rel=f"portraits/{target_id}.png";self.project.files[rel]=portrait
            entry["portrait"]=rel
            self._duelist_portrait_pending=None
        if mode=="folder" and entry.get("id") and entry.get("id") not in sources:
            sources[entry["id"]]=f"duelists/{entry['id']}.json"
        self._duelist_entries_cache=(entries,mode,path,sources);self._duelist_entries_cache_project=self.project
        self._store_duelist_entries(entries)
        self._mark_dirty()
        self.duelist_slots=self._duelist_layout()
        slot=next((s for s,r in self.duelist_slots.items()
                   if (r.get("entry") or {}).get("id")==target_id),slot)
        if slot is not None:
            self.duelist_selected_slot=slot
            if c["search"].text() and c["search"].text().casefold() not in name.casefold():
                c["search"].clear()
            c["page"].setCurrentIndex(slot//40)
        self._refresh_duelists()
        self.statusBar().showMessage(f"Applied edits to {name}. Use Save to write them to the mod.",8000)
    def _remove_duelist(self):
        c=self.workspace_controls["Duelists"]
        slot=getattr(self,"duelist_selected_slot",None)
        record=self.duelist_slots.get(slot) if slot is not None else None
        if not record or record.get("kind")!="added":
            QMessageBox.information(self,"Remove duelist","Select a custom-added duelist to remove. Retail duelists and replacements cannot be removed.")
            return
        name=record["name"]
        answer=QMessageBox.question(self,"Remove duelist",f"Remove {name} from the roster? Its duelists, deck, drop, and portrait files will be omitted when you save the mod.")
        if answer!=QMessageBox.StandardButton.Yes:return
        entries,mode,path,sources=self._duelist_roster_source()
        entry=record["entry"];duelist_id=str(entry.get("id") or "")
        entries=[item for item in entries if item is not entry]
        if mode=="folder":
            roster_path=sources.pop(duelist_id,f"duelists/{duelist_id}.json")
            self.project.removed_files.add(roster_path)
            for related in (f"decks/{duelist_id}.json",f"drops/{duelist_id}.json"):
                self.project.removed_files.add(related)
            portrait=entry.get("portrait") or f"portraits/{duelist_id}.png"
            still_used=any((other.get("portrait") or f"portraits/{other.get('id','')}.png")==portrait for other in entries)
            if not still_used:self.project.removed_files.add(portrait)
            for relative in (roster_path,f"decks/{duelist_id}.json",f"drops/{duelist_id}.json",portrait):
                self.project.files.pop(relative,None)
        self._duelist_entries_cache=(entries,mode,path,sources)
        self._duelist_entries_cache_project=self.project
        self._store_duelist_entries(entries)
        self._duelist_portrait_pending=None
        self._mark_dirty()
        self._refresh_duelists()
    def _selected_pool_name(self):
        return POOLS[self.workspace_controls["Duelists"]["pool"].currentIndex()]
    def _refresh_duelist_pool(self,*_):
        if "Duelists" not in self.workspace_controls:return
        c=self.workspace_controls["Duelists"];d=self._selected_duelist();name=self._selected_pool_name()
        record=getattr(self,"duelist_slots",{}).get(getattr(self,"duelist_selected_slot",d))
        is_added=bool(record and record.get("kind")=="added")
        pool=self.project.pools[d][name];retail=self.project.retail.pools[d][name]
        title=record["name"] if record else DUELIST_NAMES[d]
        if self._refresh_fixed_deck():
            return                      # the forty written down are what shows
        total=sum(pool.values())
        c["summary"].setText(f"{title} · {POOL_LABELS[name]} · {total}/{POOL_TOTAL} weight" + (" · inherited from base" if is_added else ""))
        c["summary"].setStyleSheet("color:#7fd49b" if total==POOL_TOTAL else "color:#ff7777")
        for button_name in ("addPoolCardButton","setPoolWeightButton","removePoolCardButton","normalizePoolButton","revertPoolButton"):
            button=c["page"].findChild(QPushButton,button_name)
            if button:button.setEnabled(not is_added)
        rows=[]
        for cid in sorted(set(pool)|set(retail),key=lambda i:(-pool.get(i,0),i)):
            weight,before=pool.get(cid,0),retail.get(cid,0)
            if not weight and not before:continue
            card=self.project.cards.get(cid)
            state="" if weight==before else "added" if not before else "removed" if not weight else "changed"
            rows.append((cid,card.name if card else "?",TYPE_NAMES[card.type] if card else "",weight,
                         f"{weight*100/POOL_TOTAL:.2f}%",before,state.title()))
        table=c["table"];held=sort_paused(table);table.setRowCount(len(rows))
        for i,row in enumerate(rows):
            for col,value in enumerate(row):
                item=TableItem(str(value))
                if col==0:item.setData(Qt.ItemDataRole.UserRole,row[0])
                self._tint_state(item,row[6].lower())
                table.setItem(i,col,item)
        sort_resumed(table,held)
    def _fixed_deck(self):
        """The duelist's fixed deck, while the deck pool is the one shown."""
        if self._selected_pool_name() != "deck":
            return None
        return fixed_decks.deck_of(self.project, self._selected_duelist())
    def _show_fixed_widgets(self, layout, visible):
        for index in range(layout.count()):
            item = layout.itemAt(index)
            if item.widget() is not None:
                item.widget().setVisible(visible)
    def _refresh_fixed_deck(self):
        """Which of the two the detail pane shows, and the fixed deck's rows."""
        c = self.workspace_controls.get("Duelists")
        if not c or "fixed_table" not in c:
            return False
        if self.project is not self.fixed_stash_of:
            self.fixed_stash, self.fixed_stash_of = {}, self.project
        on_deck = self._selected_pool_name() == "deck"
        self._show_fixed_widgets(c["mode_row"], on_deck)
        deck = self._fixed_deck()
        c["weighted_radio"].blockSignals(True)
        c["fixed_radio"].setChecked(deck is not None)
        c["weighted_radio"].setChecked(deck is None)
        c["weighted_radio"].blockSignals(False)
        fixed = deck is not None
        c["table"].setVisible(not fixed)
        self._show_fixed_widgets(c["pool_actions"], not fixed)
        c["fixed_table"].setVisible(fixed)
        c["fixed_hint"].setVisible(fixed)
        self._show_fixed_widgets(c["fixed_actions"], fixed)
        if not fixed:
            return False
        table = c["fixed_table"]
        table.blockSignals(True)
        held = sort_paused(table)
        table.setRowCount(0)
        weights = self.project.pools[self._selected_duelist()]["deck"]
        for cid in sorted(deck.cards):
            copies = deck.cards[cid]
            card = self.project.cards.get(cid)
            notes = []
            if card is None:
                notes.append("no such card")
            if copies > DECK_COPY_LIMIT:
                notes.append(f"more than the weighted deal's {DECK_COPY_LIMIT}")
            weight = weights.get(cid, 0)
            row = table.rowCount()
            table.insertRow(row)
            kind = TYPE_NAMES[card.type] if card and 0 <= card.type < len(TYPE_NAMES) else ""
            for column, value in enumerate((cid, card.name if card else "?", kind, copies,
                                            f"{weight * 100 / POOL_TOTAL:.2f}%" if weight else "",
                                            ", ".join(notes))):
                item = TableItem(str(value))
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, cid)
                self._tint_state(item, "removed" if card is None else "")
                table.setItem(row, column, item)
        # A card the editor could not place keeps its row, so its copies are
        # not quietly lost.
        for name, copies in deck.kept.items():
            row = table.rowCount()
            table.insertRow(row)
            for column, value in enumerate(("", name, "", copies, "",
                                            "no such card; the deck is left out")):
                item = TableItem(str(value))
                if column == 0:
                    item.setData(Qt.ItemDataRole.UserRole, f"kept:{name}")
                self._tint_state(item, "removed")
                table.setItem(row, column, item)
        sort_resumed(table, held)
        table.blockSignals(False)
        total = deck.total()
        good = total == DECK_SIZE and not deck.kept
        title = DUELIST_NAMES[self._selected_duelist()]
        c["summary"].setText(f"{title} · fixed deck · {total} / {DECK_SIZE} cards")
        c["summary"].setStyleSheet("color:#7fd49b" if good else "color:#ff7777")
        return True
    def _switch_deck_mode(self, *_):
        c = self.workspace_controls["Duelists"]
        if self._selected_pool_name() != "deck":
            return
        duelist = self._selected_duelist()
        if c["fixed_radio"].isChecked():
            if fixed_decks.deck_of(self.project, duelist) is None:
                if self.fixed_stash.get(duelist):
                    fixed_decks.restore(self.project, self.fixed_stash.pop(duelist))
                else:
                    # A start that deals: the forty the weighted deck most
                    # likely deals. Clear starts from nothing.
                    fixed_decks.set_deck(self.project, duelist,
                                         fixed_decks.most_likely(self.project.pools[duelist]["deck"]))
        else:
            taken = fixed_decks.remove(self.project, duelist)
            if taken:
                self.fixed_stash[duelist] = taken
        self._fixed_edited()
    def _fixed_edited(self):
        deck = self._fixed_deck()
        if deck:
            for cid in [c for c, n in deck.cards.items() if not n]:
                del deck.cards[cid]
        self._mark_dirty()
        self._refresh_duelists()
    def _selected_fixed_rows(self):
        table = self.workspace_controls["Duelists"]["fixed_table"]
        return [table.item(row, 0).data(Qt.ItemDataRole.UserRole)
                for row in sorted({i.row() for i in table.selectedItems()}) if table.item(row, 0)]
    def _select_fixed_card(self):
        deck = self._fixed_deck()
        chosen = self._selected_fixed_rows()
        if deck and len(chosen) == 1 and isinstance(chosen[0], int):
            self.workspace_controls["Duelists"]["copies"].setValue(deck.cards.get(chosen[0], 0))
    def _add_fixed_card(self):
        deck = self._fixed_deck()
        if deck is None:
            return
        cid = self._choose_one_card("Card to add to the fixed deck")
        if not cid:
            return
        copies = self.workspace_controls["Duelists"]["copies"].value()
        deck.cards[cid] = copies if 0 < copies <= DECK_SIZE else 1
        self._fixed_edited()
    def _set_fixed_copies(self):
        deck = self._fixed_deck()
        if deck is None:
            return
        copies = self.workspace_controls["Duelists"]["copies"].value()
        for cid in self._selected_fixed_rows():
            if isinstance(cid, int):
                deck.cards[cid] = copies
        self._fixed_edited()
    def _remove_fixed_cards(self):
        deck = self._fixed_deck()
        if deck is None:
            return
        for cid in self._selected_fixed_rows():
            if isinstance(cid, str) and cid.startswith("kept:"):
                deck.kept.pop(cid[5:], None)
            else:
                deck.cards.pop(cid, None)
        self._fixed_edited()
    def _copy_weighted_deck(self):
        deck = self._fixed_deck()
        if deck is None:
            return
        if deck.cards:
            answer = QMessageBox.question(
                self, "Fixed deck",
                f"Replace the {deck.total()} cards of this fixed deck with the {DECK_SIZE} the "
                "weighted deck most likely deals?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
        duelist = self._selected_duelist()
        fixed_decks.set_deck(self.project, duelist,
                             fixed_decks.most_likely(self.project.pools[duelist]["deck"]))
        self._fixed_edited()
    def _clear_fixed_deck(self):
        deck = self._fixed_deck()
        if deck is None:
            return
        deck.cards, deck.kept = {}, {}
        self._fixed_edited()
    def _revert_fixed_deck(self):
        """Back to the disc's: no fixed deck, and the weighted deck retail's."""
        duelist = self._selected_duelist()
        answer = QMessageBox.question(
            self, "Revert to retail",
            f"Deal {DUELIST_NAMES[duelist]} the disc's weighted deck again? "
            "The fixed deck and the weighted deck's edits are taken out.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        fixed_decks.remove(self.project, duelist)
        self.fixed_stash.pop(duelist, None)
        self.project.revert_pool(duelist, "deck")
        self._fixed_edited()
    def _select_pool_card(self):
        c=self.workspace_controls["Duelists"];row=c["table"].currentRow()
        if row>=0:c["weight"].setValue(int(c["table"].item(row,3).text()))
    def _add_pool_card(self):
        cid=self._choose_one_card("Add card to pool")
        if cid:
            d=self._selected_duelist();pool=self.project.pools[d][self._selected_pool_name()]
            pool[cid]=max(1,self.workspace_controls["Duelists"]["weight"].value())
            self._mark_dirty();self._refresh_duelists()
            return cid
    def _selected_pool_cards(self):
        table=self.workspace_controls["Duelists"]["table"]
        return [table.item(row,0).data(Qt.ItemDataRole.UserRole)
                for row in sorted({x.row() for x in table.selectedItems()}) if table.item(row,0)]
    def _set_pool_weight(self):
        pool=self.project.pools[self._selected_duelist()][self._selected_pool_name()]
        weight=self.workspace_controls["Duelists"]["weight"].value()
        for cid in self._selected_pool_cards():
            if weight:pool[cid]=weight
            else:pool.pop(cid,None)
        self._mark_dirty();self._refresh_duelist_pool()
    def _remove_pool_cards(self):
        pool=self.project.pools[self._selected_duelist()][self._selected_pool_name()]
        for cid in self._selected_pool_cards():pool.pop(cid,None)
        self._mark_dirty();self._refresh_duelist_pool()
    def _normalize_pool(self):
        d=self._selected_duelist();pool=self._selected_pool_name()
        self.project.pools[d][pool]=poolmath.normalize(self.project.pools[d][pool])
        self._mark_dirty();self._refresh_duelist_pool()
    def _revert_pool(self):
        d=self._selected_duelist();pool=self._selected_pool_name()
        self.project.revert_pool(d,pool)
        self._mark_dirty();self._refresh_duelists()
    @property
    def limits_duelists(self):
        return getattr(self, "_limits_duelists", {})
    def _fill_limit_duelists(self):
        c = self.workspace_controls["Limits"]
        table = c["table"]
        table.blockSignals(True)
        table.setRowCount(0)
        c["rows"] = {}
        table.setIconSize(QSize(34, 34))
        for index, name in enumerate(self._limit_names()):
            player, opponent = self.limits_duelists.get(name, (None, None))
            row = table.rowCount()
            table.insertRow(row)
            number = TableItem("—" if name == "all" else f"{index:02d}")
            number.setData(Qt.ItemDataRole.UserRole, name)
            table.setItem(row, 0, number)
            label = TableItem("All duelists" if name == "all" else name)
            if name != "all":
                base = DUELIST_NAMES.index(name) if name in DUELIST_NAMES else 0
                if base:
                    picture = self._duelist_portrait_pixmap({"base": base})
                    if not picture.isNull():
                        label.setIcon(picture)
            table.setItem(row, 1, label)
            for column, stored in ((2, player), (3, opponent)):
                spin = QSpinBox()
                spin.setRange(0, limits.LIFE_POINTS_MAX)
                spin.setSpecialValueText(self.LP_UNSET)
                spin.setValue(stored or 0)
                spin.valueChanged.connect(self._limits_edited)
                table.setCellWidget(row, column, spin)
                c["rows"].setdefault(name, {})[column] = spin
        table.blockSignals(False)
        header = table.horizontalHeader()
        for column, width in ((0, 54), (2, 130), (3, 130)):
            header.setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
            header.resizeSection(column, width)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
    def _set_duelist_lp(self, side):
        c = self.workspace_controls["Limits"]
        column = 2 if side == "player" else 3
        name = c["scope"].currentData()
        value = c["value"].value()
        # "All duelists" is the "all" row: one entry the game reads for every
        # duelist without a row of its own, rather than forty of the same number.
        if name not in c["rows"]:
            return
        c["rows"][name][column].setValue(value)
        self._limits_edited()
    def _reset_duelist_lp(self):
        c = self.workspace_controls["Limits"]
        self.limits_filling = True
        try:
            for spins in c["rows"].values():
                for spin in spins.values():
                    spin.setValue(0)
        finally:
            self.limits_filling = False
        self._limits_edited()
    def goto_duelist(self, duelist, pool):
        controls = self.workspace_controls["Duelists"]
        controls["search"].clear()
        slot = next((s for s, record in sorted(self._duelist_layout().items())
                     if record.get("base") == duelist), None)
        if slot is None:
            return
        self.duelist_selected_slot = slot
        controls["page"].setCurrentIndex(slot // 40)
        if pool in POOLS:
            controls["pool"].setCurrentIndex(POOLS.index(pool))
        self._refresh_duelists()
