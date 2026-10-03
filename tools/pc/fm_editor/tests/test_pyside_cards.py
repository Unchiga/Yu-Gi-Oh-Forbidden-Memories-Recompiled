"""Cards parity and no-op round trips using the optional Qt frontend."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import tempfile
from pathlib import Path
import unittest
from unittest import mock
from types import SimpleNamespace
try:
    from PySide6.QtWidgets import QApplication
    from fm_editor.pyside_app import ModernEditor
except ImportError:
    QApplication = None
from fm_editor import manifest, gamedata
from fm_editor.gamedata import DUELIST_NAMES
try:
    from PySide6.QtWidgets import QGridLayout, QPlainTextEdit
except ImportError:
    QGridLayout = QPlainTextEdit = None


def Qt_UserRole():
    from PySide6.QtCore import Qt
    return Qt.ItemDataRole.UserRole
from fm_editor.tests.test_data import fixture

@unittest.skipIf(QApplication is None, 'PySide6 is not installed')
class CardsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qt = QApplication.instance() or QApplication([])

    def setUp(self):
        f = fixture()
        with mock.patch.object(ModernEditor, '_load_game', return_value=SimpleNamespace(wa=f.wa, source='synthetic')), mock.patch('fm_editor.pyside_app.gamedata.load_game', return_value=f.game()), mock.patch.object(ModernEditor, '_render_preview'):
            self.window = ModernEditor()
        self.render = mock.patch.object(self.window, '_render_preview')
        self.render.start()
        self.addCleanup(self.render.stop)
        self.addCleanup(self.window.deleteLater)

    def test_browse_all_cards_preserves_manifest(self):
        w = self.window
        w.project.cards[1].type = gamedata.TYPE_TRAP
        w.project.cards[1].attribute = 7
        before = manifest.build(w.project)
        for cid in w.project.cards:
            w.show_card(cid)
            self.assertTrue(w.apply_card(quiet=True))
        self.assertEqual(before, manifest.build(w.project))
        self.assertFalse(w.dirty)

    def test_custom_star_is_preserved(self):
        w = self.window
        w.project.other['guardian_stars'] = {'stars': [{'id': 11, 'name': 'Fire'}]}
        w.project.cards[1].star1 = 11
        w.show_card(1)
        self.assertEqual(w.fields['star1'].currentIndex(), 11)
        self.assertIn('Fire', w.fields['star1'].currentText())
        w.apply_card(quiet=True)
        self.assertEqual(w.project.cards[1].star1, 11)

    def test_added_card_reverts_without_removing_identity(self):
        w = self.window
        cid = w.project.add_card(1)
        key = w.project.added[cid].key
        w.project.cards[cid].attack = 4000
        w.show_card(cid)
        w.revert_card()
        self.assertEqual(w.project.cards[cid].attack, w.project.cards[1].attack)
        self.assertEqual(w.project.added[cid].key, key)

    def test_apply_unchanged_validates(self):
        w = self.window
        w.show_card(1)
        with mock.patch('fm_editor.pyside_app.validate.validate_card', return_value=[SimpleNamespace(message='Existing issue')]):
            w.apply_card()
        self.assertEqual(w.validation.text(), 'Existing issue')

    def test_add_without_selection(self):
        w = self.window
        w.show_card(None)
        with mock.patch.object(w, '_choose_one_card', return_value=1):
            w.add_card()
        self.assertEqual(len(w.project.added), 1)

    def test_text_preview_renders_and_follows_card(self):
        from PySide6.QtWidgets import QLabel
        w = self.window
        from fm_editor.tests.test_card_text import synthetic_wa
        w.files.wa = synthetic_wa()
        w.preview_card_text()
        dialog = w.text_preview_dialog
        images = [label for label in dialog.findChildren(QLabel) if not label.pixmap().isNull()]
        self.assertTrue(images)
        w.show_card(2)
        self.assertFalse(images[0].pixmap().isNull())
        dialog.close()
        self.assertIsNone(w.text_preview_dialog)

    def test_save_round_trip_matches_backend(self):
        w = self.window
        w.project.other['guardian_stars'] = {'stars': [{'id': 11, 'name': 'Fire'}]}
        w.project.cards[1].star1 = 11
        w.project.cards[2].type = gamedata.TYPE_TRAP
        w.project.cards[2].attribute = 7
        with tempfile.TemporaryDirectory() as tmp:
            original, baseline, edited = [Path(tmp) / name for name in ('original', 'baseline', 'edited')]
            manifest.save_mod(w.project, original)
            expected, _ = manifest.open_mod(w.retail, original)
            manifest.save_mod(expected, baseline)
            w.open_mod_path(str(original))
            for cid in (1, 2, 601, 722):
                w.show_card(cid)
                w.apply_card(quiet=True)
            w.project.source_dir = edited
            w.save_mod()
            self.assertEqual((baseline / 'mod.json').read_bytes(), (edited / 'mod.json').read_bytes())
            expected_files = {p.relative_to(baseline): p.read_bytes() for p in baseline.rglob('*') if p.is_file()}
            actual_files = {p.relative_to(edited): p.read_bytes() for p in edited.rglob('*') if p.is_file()}
            self.assertEqual(expected_files, actual_files)

    def test_menu_parity(self):
        menus = {action.text(): action.menu() for action in self.window.menuBar().actions()}
        expected = {
            'File': ['New mod', 'Open mod folder', 'Save', 'Save as', 'Import a modified game',
                     "Convert an old recomp's", 'Game files', 'Exit'],
            'Tools': ['Check the mod', 'Preview mod.json', 'Card text preview'],
            'View': ['Dark mode'], 'Help': ['About'],
        }
        for name, entries in expected.items():
            labels = [action.text() for action in menus[name].actions()]
            for entry in entries:
                self.assertTrue(any(label.startswith(entry) for label in labels), (name, entry))

    def test_theme_toggle_remembers_choice(self):
        w = self.window
        with mock.patch('fm_editor.pyside_app.settings.save', return_value=None) as save:
            w.toggle_dark(True)
            dark = w.styleSheet()
            w.toggle_dark(False)
            self.assertNotEqual(w.styleSheet(), dark)
            save.assert_called_with('modern_dark', False)
            w.toggle_dark(True)
            self.assertEqual(w.styleSheet(), dark)
        self.assertFalse(w.dirty)

    def test_import_actions_replace_project_and_mark_dirty(self):
        from fm_editor.model import Project
        for package in (False, True):
            w = self.window
            project = Project(w.retail)
            with mock.patch.object(w, 'confirm_discard', return_value=True), mock.patch.object(w, '_report'), \
                 mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=('example.ygomods' if package else 'example.bin', '')), \
                 mock.patch('fm_editor.pyside_app.disc.load'), \
                 mock.patch('fm_editor.pyside_app.importer.import_modded', return_value=SimpleNamespace(project=project, report=['Imported'])), \
                 mock.patch('fm_editor.pyside_app.ygomods.import_package', return_value=(project, ['Converted'])):
                (w.import_ygomods if package else w.import_modded_game)()
            self.assertIs(w.project, project)
            self.assertTrue(w.dirty)

    def test_save_as_creates_mod_subfolder(self):
        w = self.window
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp)
            (parent / 'existing-mod').mkdir()
            with mock.patch('fm_editor.pyside_app.QFileDialog.getExistingDirectory', return_value=tmp):
                w.save_mod(choose=True)
            self.assertTrue((parent / w.project.info.id / 'mod.json').is_file())
            self.assertFalse((parent / 'mod.json').exists())

    def test_open_starter_mod_after_empty_project(self):
        from fm_editor.model import StarterDeck
        w = self.window
        self.assertIsNone(w.starter_current)
        w.project.starter.append(StarterDeck())
        w._refresh_starter_decks()
        self.assertEqual(w.starter_current, 0)

    def test_art_details_and_type_column(self):
        w = self.window
        w.refresh_art_list(select_id=1)
        self.assertEqual(w.art_table.horizontalHeaderItem(2).text(), 'Type')
        self.assertIn('Texture pack:', w.art_pack_status.text())
        cid = w.project.add_card(1)
        w.show_art_card(cid)
        self.assertIn('inherits', w.art_instructions.text())
        self.assertIn('Restart', w.art_instructions.text())

    def test_art_preview_preserves_shape_and_clears_errors(self):
        from PySide6.QtCore import QSize
        from fm_editor import pngio
        w = self.window
        label = w.art_previews['art']['game']
        image = pngio.Image(100, 50, bytes([255, 0, 0, 255]) * 5000)
        w._show_art_image(label, image, 1, '', QSize(240, 226))
        self.assertEqual((label.pixmap().width(), label.pixmap().height()), (240, 120))
        w.current_art_card = 1
        # A render the editor cannot make clears that part and says so.
        with mock.patch('fm_editor.pyside_app.art.in_game', side_effect=ValueError('Broken image')):
            w.show_art_card(1)
        self.assertTrue(label.pixmap().isNull())
        self.assertIn('Broken image', w.art_status.text())
        with mock.patch('fm_editor.pyside_app.art.disc_image', side_effect=ValueError('Bad disc')):
            w.show_art_card(2)
        for previews in w.art_previews.values():
            for preview in previews.values():
                self.assertTrue(preview.pixmap().isNull())

    def test_art_export_sanitizes_filename(self):
        from fm_editor import pngio
        w = self.window
        w.project.cards[1].name = 'Card / with : unsafe ? characters'
        image = pngio.Image(1, 1, bytes([0, 0, 0, 255]))
        with mock.patch('fm_editor.pyside_app.QFileDialog.getSaveFileName', return_value=('', '')) as chooser:
            w._save_art_image(1, 'art', image, 'mod')
        self.assertEqual(chooser.call_args.args[2], '0001-card-with-unsafe-characters-mod.png')

    def test_art_import_warns_on_crop(self):
        from fm_editor import pngio
        w = self.window
        w.current_art_card = 1
        image = pngio.Image(1, 1, bytes([0, 0, 0, 255]))
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=('crop.png', '')), \
             mock.patch('fm_editor.pyside_app.art.pngio.read', return_value=image), \
             mock.patch('fm_editor.pyside_app.art.set_image', return_value=['shape was cropped']), \
             mock.patch('fm_editor.pyside_app.QMessageBox.warning') as warning:
            w.import_art('art')
        warning.assert_called_once()
        self.assertIn('102:96', warning.call_args.args[2])
        self.assertTrue(w.dirty)

    def test_fusion_effective_results_and_forbidden_pairs(self):
        from PySide6.QtCore import Qt
        w = self.window
        p = w.project
        p.set_fusion(1, 2, None)
        p.card_extra[4] = {'fusions': [{'with': 5, 'result': 6}]}
        p._own_pairs = None
        p.fusion_explicit.add((7, 8))
        p.fusions.pop((7, 8), None)
        w._refresh_fusions()
        listing = w.workspace_controls['Fusions']['image_list']
        rows = {listing.item(i).data(Qt.ItemDataRole.UserRole): listing.item(i) for i in range(listing.count())}
        self.assertIsNone(rows[(1, 2)].data(int(Qt.ItemDataRole.UserRole) + 1))
        self.assertIn('No fusion', rows[(1, 2)].text())
        self.assertEqual(rows[(4, 5)].data(int(Qt.ItemDataRole.UserRole) + 1), 6)
        self.assertIn('own list', rows[(4, 5)].text())
        self.assertIn((7, 8), rows)

    def test_fusion_multiselect_and_empty_actions(self):
        from PySide6.QtWidgets import QAbstractItemView
        w = self.window
        c = w.workspace_controls['Fusions']
        listing = c['image_list']
        self.assertEqual(listing.selectionMode(), QAbstractItemView.SelectionMode.ExtendedSelection)
        listing.clearSelection()
        w._remove_fusions()
        w._revert_fusions()
        self.assertFalse(w.dirty)
        for i in (0, 1): listing.item(i).setSelected(True)
        pairs = w._selected_fusion_pairs()
        self.assertEqual(len(pairs), 2)
        w._remove_fusions()
        for pair in pairs: self.assertFalse(w.project.fusions.get(pair))

    def test_bulk_filters_all_kinds_and_custom_stars(self):
        from PySide6.QtCore import Qt
        w = self.window
        c = w.workspace_controls['Fusions']
        a = c['bulk_filters']['a']
        self.assertEqual(len(w._read_bulk_filter(a).select(w.project)[0]), 722)
        a['kinds']['magic'].setChecked(True)
        matches = w._read_bulk_filter(a).select(w.project)[0]
        self.assertTrue(matches)
        self.assertTrue(all(w.project.cards[cid].type == gamedata.TYPE_MAGIC for cid in matches))
        w._copy_bulk_filter('a', 'b')
        self.assertTrue(c['bulk_filters']['b']['kinds']['magic'].isChecked())
        w._clear_bulk_filter('a')
        w.project.other['guardian_stars'] = {'stars': [{'id': 11, 'name': 'Fire'}]}
        w.project.cards[1].star1 = 11
        w._refresh_bulk_fusion_plan()
        self.assertIn('Fire', a['stars'].item(10).text())
        a['stars'].item(10).setCheckState(Qt.CheckState.Checked)
        self.assertEqual(w._read_bulk_filter(a).select(w.project)[0], [1])
        with mock.patch.object(w, '_report') as report:
            w._list_bulk_matches('a')
        self.assertIn(w.project.card_label(1), report.call_args.args[1])

    def test_bulk_remove_and_add_undo(self):
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        c = w.workspace_controls['Fusions']
        bulk = c['bulk']
        c['bulk_filters']['a']['cards'].setText('1')
        c['bulk_filters']['b']['cards'].setText('2')
        before = manifest.build(w.project)
        bulk['mode'].setCurrentIndex(1)
        with mock.patch('fm_editor.pyside_app.QMessageBox.question', return_value=QMessageBox.StandardButton.Yes):
            w._apply_bulk_fusions()
        self.assertFalse(w.project.fusions.get((1, 2)))
        w._undo_bulk_fusions()
        self.assertEqual(manifest.build(w.project), before)
        bulk['mode'].setCurrentIndex(0)
        bulk['result_id'] = 4
        bulk['stronger'].setChecked(False)
        bulk['overwrite'].setChecked(True)
        with mock.patch('fm_editor.pyside_app.QMessageBox.question', return_value=QMessageBox.StandardButton.Yes):
            w._apply_bulk_fusions()
        self.assertEqual(w.project.fusions[(1, 2)], 4)
        w._undo_bulk_fusions()
        self.assertEqual(manifest.build(w.project), before)

    def test_remove_disc_recipes_by_result(self):
        from PySide6.QtWidgets import QMessageBox, QPushButton
        w = self.window
        self.assertTrue(any(b.text() == 'Remove recipes of…' for b in w.findChildren(QPushButton)))
        with mock.patch.object(w, '_choose_one_card', return_value=3), \
             mock.patch('fm_editor.pyside_app.QMessageBox.question', return_value=QMessageBox.StandardButton.Yes):
            w._remove_fusion_result()
        self.assertIn(3, w.project.active_removes())
        self.assertFalse(w.project.fusions.get((1, 2)))

    def test_fusion_display_limit(self):
        w = self.window
        for a in range(1, 61):
            for b in range(61, 121):
                w.project.fusions[(a, b)] = 3
        w._refresh_fusions()
        c = w.workspace_controls['Fusions']
        self.assertEqual(c['image_list'].count(), 3000)
        self.assertEqual(c['table'].rowCount(), 3000)
        self.assertIn('first 3,000', c['count'].text())

    def test_fusion_filter_double_click_and_collapsed_layout(self):
        from PySide6.QtCore import Qt, QPoint
        from PySide6.QtTest import QTest
        w = self.window
        w.select_workspace('Fusions')
        c = w.workspace_controls['Fusions']
        c['pages'].setCurrentIndex(1)
        w.show()
        self.qt.processEvents()
        for side in c['bulk_filters'].values():
            self.assertTrue(side['advanced'].parentWidget().isHidden())
            for key in ('types', 'stars'):
                listing = side[key]
                item = listing.item(0)
                rect = listing.visualItemRect(item)
                self.assertGreaterEqual(rect.height(), 24)
                point = QPoint(rect.left() + 70, rect.center().y())
                QTest.mouseDClick(listing.viewport(), Qt.MouseButton.LeftButton, pos=point)
                self.assertEqual(item.checkState(), Qt.CheckState.Checked)
                QTest.mouseDClick(listing.viewport(), Qt.MouseButton.LeftButton, pos=point)
                self.assertEqual(item.checkState(), Qt.CheckState.Unchecked)
        w.hide()

    def test_fusion_header_tracks_sidebar_resize(self):
        w = self.window
        w.show()
        w.select_workspace('Fusions')
        c = w.workspace_controls['Fusions']
        c['pages'].setCurrentIndex(0)
        for _ in range(3): self.qt.processEvents()
        before = c['sort_headers'][0].width()
        w._toggle_workspace(False)
        for _ in range(3): self.qt.processEvents()
        width = c['image_list'].viewport().width()
        self.assertEqual(c['sort_headers'][0].width(), 80 + max(210, width - 478) // 3)
        self.assertNotEqual(before, c['sort_headers'][0].width())
        w.hide()

    def test_no_selection_clears_and_disables_the_card_form(self):
        """A filter with no results must not leave the last card's data in a
        form that takes edits and drops them (CardsTab.show(None))."""
        w = self.window
        self.assertEqual(w.current, 1)
        w.search.setText('zzz-no-such-card-zzz')
        self.assertEqual(w.listing.rowCount(), 0)
        self.assertIsNone(w.current)
        self.assertEqual(w.card_id.text(), '—')
        self.assertEqual(w.fields['name'].text(), '')
        self.assertEqual(w.description.toPlainText(), '')
        self.assertEqual(w.notes.toPlainText(), '')
        self.assertIn('Select a card', w.reference_info.text())
        for widget in w._card_form_widgets():
            self.assertFalse(widget.isEnabled(), widget.objectName())
        self.assertTrue(w.add_button.isEnabled())       # a card may still be added
        w.search.clear()
        self.assertEqual(w.current, 1)
        self.assertEqual(w.fields['name'].text(), 'Blue Dragon')
        for widget in w._card_form_widgets():
            self.assertTrue(widget.isEnabled(), widget.objectName())

    def test_search_matches_the_card_number_whole(self):
        """The number is a whole match, as model.card_matches has it: a search
        for "1" is card 1, not every card with a 1 in its number."""
        from PySide6.QtCore import Qt
        w = self.window
        # Names and text without digits, so only the number column can match.
        for card in w.project.cards.values():
            card.name, card.description = 'a nameless card', ''
        w.search.setText('1')
        self.assertEqual([w.listing.item(r, 0).data(Qt.ItemDataRole.UserRole)
                          for r in range(w.listing.rowCount())], [1])
        w.search.setText('22')
        self.assertEqual([w.listing.item(r, 0).data(Qt.ItemDataRole.UserRole)
                          for r in range(w.listing.rowCount())], [22])
        # The name and the card text are still matched on any part of them.
        w.project.cards[5].description = 'a poem about rain'
        w.search.setText('about rain')
        self.assertEqual([w.listing.item(r, 0).data(Qt.ItemDataRole.UserRole)
                          for r in range(w.listing.rowCount())], [5])

    def test_card_text_box_is_fixed_pitch(self):
        """The game wraps at 20 letters a line; the columns must be countable."""
        self.assertTrue(self.window.description.fontInfo().fixedPitch())

    def test_goto_card_ignores_the_list_filters(self):
        w = self.window
        w.search.setText('Blue')
        w.filter.setCurrentText('Changed')
        w.goto_card(300)
        self.assertEqual(w.current, 300)
        self.assertEqual(w.search.text(), '')
        self.assertEqual(w.filter.currentText(), 'All cards')

    def test_problem_lines_go_to_what_they_are_about(self):
        w = self.window
        w.project.pools[2]['deck'] = {999999: 2048}
        w.select_workspace('Problems')
        w._refresh_problems()
        issues = w.workspace_controls['Problems']['issues']
        self.assertTrue(issues)
        row = next(i for i, issue in enumerate(issues) if issue.area == 'Duelists')
        w._open_problem(row)
        self.assertEqual(w.current_workspace, 'Duelists')
        self.assertEqual(w.workspace_controls['Duelists']['pool'].currentText(), 'Deck')
        # Cards, Fusions and Rituals reach their own page and selection too.
        w.go_to(SimpleNamespace(area='Cards', target=42, where='card 42', level='error'))
        self.assertEqual((w.current_workspace, w.current), ('Cards', 42))
        w.go_to(SimpleNamespace(area='Fusions', target=(5, 6), where='5+6', level='error'))
        self.assertEqual(w.current_workspace, 'Fusions')
        self.assertEqual(w.workspace_controls['Fusions']['search'].text(), '5')
        # Packs has a page of its own now, so its problems reach it.
        w.go_to(SimpleNamespace(area='Packs', target=None, where='pack 1', level='error'))
        self.assertEqual(w.current_workspace, 'Packs')
        # An area with no page still says so rather than doing nothing.
        w.select_workspace('Fusions')
        w.go_to(SimpleNamespace(area='Nowhere', target=None, where='x', level='error'))
        self.assertEqual(w.current_workspace, 'Fusions')
        self.assertIn('not in the modern editor yet', w.statusBar().currentMessage())

    def test_art_search_matches_the_card_number_whole(self):
        """The Art list searches as the card list does (model.card_matches)."""
        from PySide6.QtCore import Qt
        w = self.window
        w.select_workspace('Art')
        for card in w.project.cards.values():
            card.name, card.description = 'a nameless card', ''
        w.refresh_art_list()
        for query, expected in (('1', [1]), ('22', [22])):
            w.art_search.setText(query)
            self.assertEqual([w.art_table.item(r, 0).data(Qt.ItemDataRole.UserRole)
                              for r in range(w.art_table.rowCount())], expected)

    def test_goto_art_ignores_the_list_filters(self):
        w = self.window
        w.select_workspace('Art')
        w.art_search.setText('nothing-matches-this')
        w.art_filter.setCurrentText('Added by the mod')
        w.goto_art(300)
        self.assertEqual(w.current_art_card, 300)
        self.assertEqual(w.art_search.text(), '')
        self.assertEqual(w.art_filter.currentText(), 'All cards')

    def test_card_combos_take_a_number_or_a_name(self):
        """A card is typed, not hunted down a list of seven hundred
        (widgets.card_named)."""
        w = self.window
        combo = w._card_combo(None)
        self.assertTrue(combo.isEditable())
        for typed, expected in (('003  Kuriboh', 3), ('Kuriboh', 3), ('3', 3),
                                ('kurib', 3), ('', None), ('zzzz', None)):
            combo.setCurrentText(typed)
            self.assertEqual(w._combo_card_id(combo), expected, typed)

    def test_fusion_search_covers_materials_and_results_by_default(self):
        """The Tk list matched card A, card B or the result; "Both" is what
        that is here, so it is what the page opens on."""
        w = self.window
        w.select_workspace('Fusions')
        c = w.workspace_controls['Fusions']
        self.assertTrue(c['search_both'].isChecked())
        c['search'].setText('Kuriboh')
        both = c['table'].rowCount()
        c['search_materials'].setChecked(True)
        self.assertLess(c['table'].rowCount(), both)    # narrowing is still offered

    def test_rows_are_coloured_by_their_state(self):
        from PySide6.QtCore import Qt
        w = self.window
        w.project.set_notes(5, 'a note')
        w.project.cards[7].name = 'edited'
        w.refresh_cards()
        inks = {}
        for row in range(w.listing.rowCount()):
            cid = w.listing.item(row, 0).data(Qt.ItemDataRole.UserRole)
            if cid in (5, 7):
                inks[cid] = w.listing.item(row, 0).foreground().color().name()
        self.assertEqual(inks[5], '#f2c04c')        # notes
        self.assertEqual(inks[7], '#8ab4f8')        # changed
        # The retail table's "glitch" fusions are the brown rows the hint names.
        # The shown list paints itself, so the ink reaches it through the
        # delegate's own role, not the item's foreground.
        w.select_workspace('Fusions')
        listing = w.workspace_controls['Fusions']['image_list']
        ink_role = int(Qt.ItemDataRole.UserRole) + 6
        state_role = int(Qt.ItemDataRole.UserRole) + 3
        def glitch_ink():
            return next(listing.item(i).data(ink_role) for i in range(listing.count())
                        if listing.item(i).data(state_role) == 'glitch')
        self.assertEqual(glitch_ink(), '#e0ae78')
        # Item inks are not styles, so the theme has to redraw them itself.
        with mock.patch('fm_editor.pyside_app.settings.save', return_value=None):
            w.toggle_dark(False)
        listing = w.workspace_controls['Fusions']['image_list']
        self.assertEqual(glitch_ink(), '#865e3c')

    def test_remove_disc_recipes_offers_the_selected_result(self):
        w = self.window
        w.select_workspace('Fusions')
        c = w.workspace_controls['Fusions']
        c['search'].clear()
        c['image_list'].item(0).setSelected(True)
        pair = w._selected_fusion_pairs()[0]
        expected = w.project.fusions.get(pair)
        seen = {}
        def capture(title, ids=None, selected=None):
            seen['selected'] = selected
            return None
        with mock.patch.object(type(w), '_choose_one_card', side_effect=capture):
            w._remove_fusion_result()
        self.assertEqual(seen['selected'], expected)

    def test_the_card_list_shows_all_six_columns(self):
        """The middle column used to crowd the list until DEF and State fell off
        its right edge."""
        w = self.window
        w.show()
        w.resize(1520, 1000)
        for _ in range(6):
            self.qt.processEvents()
        widths = [w.listing.columnWidth(i) for i in range(w.listing.columnCount())]
        self.assertEqual(len(widths), 6)
        self.assertLessEqual(sum(widths), w.listing.viewport().width())
        self.assertFalse(w.listing.horizontalScrollBar().isVisible())
        self.assertGreater(widths[1], 40)       # the name keeps room of its own
        self.assertFalse(w.listing.wordWrap())  # and elides instead of doubling the row
        heights = {w.listing.rowHeight(r) for r in range(min(20, w.listing.rowCount()))}
        self.assertEqual(len(heights), 1)
        w.hide()

    def test_the_card_form_never_scrolls_sideways(self):
        from PySide6.QtWidgets import QScrollArea
        w = self.window
        w.show()
        for width in (1280, 1520, 1920):
            w.resize(width, 1000)
            for _ in range(6):
                self.qt.processEvents()
            scroll = w.cards_form.findChild(QScrollArea, 'cardDataScroll')
            self.assertLessEqual(scroll.widget().minimumSizeHint().width(),
                                 scroll.viewport().width(), width)
            self.assertFalse(scroll.horizontalScrollBar().isVisible(), width)
        w.hide()

    def test_a_combo_popup_is_dark_all_the_way_to_its_frame(self):
        """The popup is its own window: the stylesheet reaches the list, the
        palette reaches the frame around it."""
        w = self.window
        w.show()
        w.resize(1520, 1000)
        for _ in range(5):
            self.qt.processEvents()
        w.frame_box.showPopup()
        for _ in range(5):
            self.qt.processEvents()
        image = w.frame_box.view().parentWidget().grab().toImage()
        light = 0
        for y in range(image.height()):
            across = sum(1 for x in range(image.width())
                         if image.pixelColor(x, y).lightness() > 200)
            if across > image.width() * 0.8:
                light += 1
        w.frame_box.hidePopup()
        w.hide()
        self.assertEqual(light, 0)

    # --- Limits -----------------------------------------------------------

    def limits_page(self):
        self.window.select_workspace('Limits')
        return self.window.workspace_controls['Limits']

    def test_limits_builds_a_field_for_every_limit(self):
        from fm_editor import limits
        c = self.limits_page()
        self.assertEqual(set(c['limits']), {key for key, *_ in limits.ALL_FIELDS})
        for key, _label, _retail, low, high, _storage in limits.ALL_FIELDS:
            spin = c['limits'][key]
            self.assertEqual((spin.minimum(), spin.maximum()), (low - 1, high), key)
            self.assertEqual(spin.value(), low - 1, key)      # unset to start with

    def test_a_limit_is_written_through_at_once(self):
        w = self.window
        c = self.limits_page()
        w.dirty = False
        c['limits']['stats'].setValue(30000)
        self.assertEqual(w.project.other['limits'], {'stats': 30000})
        self.assertTrue(w.dirty)
        c['limits']['two_player.step'].setValue(250)        # a nested key
        self.assertEqual(w.project.other['limits'],
                         {'stats': 30000, 'two_player': {'step': 250}})
        c['limits']['stats'].setValue(c['limits']['stats'].minimum())
        self.assertEqual(w.project.other['limits'], {'two_player': {'step': 250}})

    def test_life_points_collapses_to_its_short_form(self):
        w = self.window
        c = self.limits_page()
        c['limits']['life_points.start'].setValue(16000)
        self.assertEqual(w.project.other['limits'], {'life_points': 16000})

    def test_per_duelist_lp_takes_both_shapes(self):
        w = self.window
        c = self.limits_page()
        c['rows']['Heishin'][3].setValue(20000)             # the duelist's side only
        self.assertEqual(w.project.other['limits']['life_points']['duelists'],
                         {'Heishin': 20000})
        c['rows']['Seto'][2].setValue(8000)
        c['rows']['Seto'][3].setValue(12000)
        self.assertEqual(w.project.other['limits']['life_points']['duelists']['Seto'],
                         {'player': 8000, 'opponent': 12000})

    def test_setting_lp_for_all_duelists_writes_the_wildcard(self):
        w = self.window
        c = self.limits_page()
        c['value'].setValue(7000)
        c['scope'].setCurrentIndex(0)                       # "All duelists"
        w._set_duelist_lp('player')
        self.assertEqual(w.project.other['limits']['life_points']['duelists'],
                         {'all': {'player': 7000}})
        c['scope'].setCurrentIndex(c['scope'].findData('Mai Valentine'))
        c['value'].setValue(11000)
        w._set_duelist_lp('opponent')
        self.assertEqual(w.project.other['limits']['life_points']['duelists']['Mai Valentine'], 11000)

    def test_resetting_clears_the_limits_key(self):
        w = self.window
        c = self.limits_page()
        c['limits']['stats'].setValue(30000)
        c['rows']['Teana'][2].setValue(5000)
        w._reset_all_limits()
        self.assertNotIn('limits', w.project.other)

    def test_revert_puts_back_what_the_mod_was_opened_with(self):
        w = self.window
        w.project.other['limits'] = {'stats': 12345}
        w.limits_opened = None
        c = self.limits_page()
        self.assertEqual(c['limits']['stats'].value(), 12345)
        c['limits']['stats'].setValue(30000)
        w._revert_limits()
        self.assertEqual(w.project.other['limits'], {'stats': 12345})

    def test_a_duelist_the_editor_does_not_know_keeps_its_row(self):
        w = self.window
        w.project.other['limits'] = {'life_points': {'duelists': {'My Custom Guy': 5000}}}
        w.limits_opened = None
        c = self.limits_page()
        self.assertIn('My Custom Guy', c['rows'])
        self.assertEqual(c['rows']['My Custom Guy'][3].value(), 5000)
        self.assertEqual(w.project.other['limits']['life_points']['duelists'],
                         {'My Custom Guy': 5000})

    def test_limits_reports_what_the_loader_would_refuse(self):
        w = self.window
        w.project.other['limits'] = {'stats': 99999, 'nonsense': 1}
        c = self.limits_page()
        self.assertIn('nonsense', c['status'].text())
        self.assertFalse(w._apply_limits())                 # an error is reported, not applied over
        w.project.other['limits'] = {'stats': 30000}
        w._refresh_limits()
        self.assertTrue(w._apply_limits())

    def test_the_limits_form_never_scrolls_sideways(self):
        from PySide6.QtWidgets import QScrollArea
        w = self.window
        self.limits_page()
        w.show()
        for width in (1280, 1520, 1920):
            w.resize(width, 1000)
            for _ in range(6):
                self.qt.processEvents()
            scroll = w.workspace_forms['Limits'].findChild(QScrollArea, 'limitSettingsScroll')
            self.assertLessEqual(scroll.widget().minimumSizeHint().width(),
                                 scroll.viewport().width(), width)
            self.assertFalse(scroll.horizontalScrollBar().isVisible(), width)
        w.hide()

    def test_no_page_hands_its_slack_to_a_label(self):
        """A page's panels take the height, not its heading and summary: with
        every item at stretch 0 a QVBoxLayout shares the slack out evenly."""
        from PySide6.QtWidgets import QLabel
        w = self.window
        w.show()
        w.resize(1920, 1040)
        stretched = {}
        for name in w.NAV:
            w.select_workspace(name)
            for _ in range(6):
                self.qt.processEvents()
            page = w.workspace_forms.get(name)
            layout = page.layout() if page is not None else None
            if layout is None:
                continue
            for index in range(layout.count()):
                item = layout.itemAt(index)
                label = item.widget()
                if isinstance(label, QLabel) and \
                        item.geometry().height() > label.sizeHint().height() + 12:
                    stretched.setdefault(name, []).append(label.objectName())
        w.hide()
        self.assertEqual(stretched, {})

    def test_the_limits_table_gives_its_spin_boxes_room(self):
        w = self.window
        c = self.limits_page()
        w.show()
        w.resize(1920, 1040)
        for _ in range(6):
            self.qt.processEvents()
        spin = c['table'].cellWidget(0, 2)
        self.assertGreaterEqual(spin.height(), spin.sizeHint().height())
        self.assertGreater(c['table'].height() // c['table'].rowHeight(0), 12)
        w.hide()

    # --- Guardian Stars ---------------------------------------------------

    def stars_page(self):
        self.window.select_workspace('Guardian Stars')
        return self.window.workspace_controls['Guardian Stars']

    def test_the_stars_and_the_matrix_are_listed(self):
        from fm_editor import guardian_stars as gs
        w = self.window
        c = self.stars_page()
        self.assertEqual(c['stars'].rowCount(), gs.RETAIL_COUNT)
        self.assertEqual(c['list_title'].text(), f'Guardian stars ({gs.RETAIL_COUNT})')
        self.assertEqual([c['stars'].item(0, i).text() for i in range(3)], ['1', 'Mars', "disc's"])
        self.assertEqual((c['matrix'].rowCount(), c['matrix'].columnCount()),
                         (gs.RETAIL_COUNT, gs.RETAIL_COUNT))
        # The disc's cycle: Mars beats Jupiter.
        self.assertEqual(w.stars_model.grid[1][2], gs.retail_matchup(1, 2))
        self.assertEqual(c['matrix'].item(0, 1).text(), f'{gs.retail_matchup(1, 2):+}')

    def test_a_matchup_cell_and_its_mirror(self):
        w = self.window
        c = self.stars_page()
        w.dirty = False
        w._pick_star_cell(0, 2)                     # Mars attacking Saturn
        self.assertIn('Mars attacking Saturn', c['cell_label'].text())
        c['value'].setValue(1234)
        w._set_star_cell()
        self.assertEqual(w.stars_model.grid[1][3], 1234)
        self.assertEqual(w.stars_model.grid[3][1], -1234)       # mirrored
        self.assertTrue(w.dirty)
        self.assertIn('matchups', w.project.other['guardian_stars'])
        c['mirror'].setChecked(False)
        w._set_star_cell(7)
        self.assertEqual(w.stars_model.grid[1][3], 7)
        self.assertEqual(w.stars_model.grid[3][1], -1234)       # left alone

    def test_the_default_bonus_moves_the_cycles_with_it(self):
        from fm_editor import guardian_stars as gs
        w = self.window
        c = self.stars_page()
        self.assertEqual(w.stars_model.grid[1][2], gs.RETAIL_BONUS)
        c['bonus'].setValue(800)
        w._set_star_default()
        self.assertEqual(w.stars_model.default_bonus, 800)
        self.assertEqual(w.stars_model.grid[1][2], 800)

    def test_renaming_a_star_writes_only_that_star(self):
        w = self.window
        c = self.stars_page()
        w._select_star_row(5)
        self.assertEqual(w.stars_selected, 5)
        c['name'].setText('Poseidon')
        w._set_star_name()
        self.assertEqual(w.stars_model.name(5), 'Poseidon')
        self.assertEqual(w.project.other['guardian_stars']['stars'], [{'id': 5, 'name': 'Poseidon'}])

    def test_the_name_list_sets_every_star_at_once(self):
        w = self.window
        c = self.stars_page()
        names = ['Ares', 'Zeus', 'Cronos', 'Ouranos', 'Hades',
                 'Poseidon', 'Hermes', 'Selene', 'Helios', 'Aphrodite']
        c['names'].setText(', '.join(names))
        w._set_star_names()
        self.assertEqual([w.stars_model.name(i) for i in range(1, 11)], names)
        w._reset_star_names()
        self.assertEqual(w.stars_model.name(1), 'Mars')
        self.assertNotIn('guardian_stars', w.project.other)

    def test_adding_duplicating_and_removing_a_star(self):
        from fm_editor import guardian_stars as gs
        w = self.window
        c = self.stars_page()
        w._select_star_row(1)
        c['name'].setText('Ares')
        w._set_star_name()
        star = w._add_star()
        self.assertEqual(star, gs.RETAIL_COUNT + 1)
        self.assertEqual(w.stars_model.count, gs.RETAIL_COUNT + 1)
        w._select_star_row(1)
        w._duplicate_star()                         # a copy of Ares
        self.assertEqual(w.stars_model.count, gs.RETAIL_COUNT + 2)
        self.assertEqual(w.stars_model.name(gs.RETAIL_COUNT + 2), 'Ares')
        w.stars_selected = gs.RETAIL_COUNT + 2
        w._remove_star()
        self.assertEqual(w.stars_model.count, gs.RETAIL_COUNT + 1)

    def test_a_star_icon_is_imported_and_removed(self):
        import tempfile
        from pathlib import Path
        from fm_editor import pngio
        w = self.window
        c = self.stars_page()
        w._select_star_row(5)
        image = pngio.Image(16, 16, bytes([200, 40, 40, 255]) * 256)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'star.png'
            pngio.write(str(path), image)
            w.use_star_icon(5, str(path))
        self.assertEqual(w.stars_model.stars[5].icon, 'icons/star-5.png')
        self.assertIn('icons/star-5.png', w.project.files)
        self.assertFalse(c['icon'].pixmap().isNull())
        self.assertEqual(c['stars'].item(4, 2).text(), "mod's")
        w._remove_star_icon()
        self.assertNotIn('icons/star-5.png', w.project.files)

    def test_the_table_presets(self):
        w = self.window
        self.stars_page()
        w._stars_preset_clear()
        self.assertTrue(w.stars_model.replace)
        self.assertEqual(w.stars_model.grid[1][2], 0)
        w._stars_preset_retail()
        self.assertFalse(w.stars_model.replace)
        self.assertNotEqual(w.stars_model.grid[1][2], 0)

    def test_an_untouched_section_is_not_rewritten(self):
        """A mod's own "beats"/"mirror" shape survives a visit to the page."""
        w = self.window
        written = {'stars': [{'id': 11, 'name': 'Fire', 'beats': ['Mars']}]}
        w.project.other['guardian_stars'] = written
        self.stars_page()
        w.dirty = False
        w._commit_stars()
        self.assertEqual(w.project.other['guardian_stars'], written)
        self.assertFalse(w.dirty)

    def test_stars_by_rule_applies_and_undoes(self):
        from PySide6.QtWidgets import QDialog, QPushButton, QCheckBox, QComboBox, QMessageBox
        w = self.window
        self.stars_page()
        opened = {}
        with mock.patch.object(QDialog, 'exec', lambda self: opened.setdefault('d', self) and 0):
            w._open_star_rules()
        dialog = opened['d']
        next(b for b in dialog.findChildren(QCheckBox) if b.text() == 'Every monster').setChecked(True)
        mapping = [b for b in dialog.findChildren(QComboBox)
                   if b.count() and b.itemText(0) == '(none)']
        self.assertEqual(len(mapping), 6)           # one per attribute
        mapping[0].setCurrentIndex(3)               # Light cards get star 3
        before = {cid: (card.star1, card.star2) for cid, card in w.project.cards.items()}
        apply_button = next(b for b in dialog.findChildren(QPushButton) if b.text().startswith('Apply'))
        undo_button = next(b for b in dialog.findChildren(QPushButton) if b.text().startswith('Undo'))
        self.assertTrue(apply_button.isEnabled())
        with mock.patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            apply_button.click()
        changed = [cid for cid, card in w.project.cards.items()
                   if (card.star1, card.star2) != before[cid]]
        self.assertTrue(changed)
        self.assertTrue(undo_button.isEnabled())
        with mock.patch.object(QMessageBox, 'information'):
            undo_button.click()
        self.assertEqual([cid for cid, card in w.project.cards.items()
                          if (card.star1, card.star2) != before[cid]], [])

    def test_guardian_stars_reports_and_reverts(self):
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        w.project.other['guardian_stars'] = {'nonsense': 1}
        c = self.stars_page()
        self.assertIn('nonsense', c['status'].text())
        self.assertFalse(w._apply_stars())
        with mock.patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            w._revert_stars()
        self.assertNotIn('guardian_stars', w.project.other)

    def test_the_matrix_is_a_grid_not_a_row_list(self):
        """The shared table pass hides vertical headers, selects whole rows and
        stretches columns; the matrix is named on both axes and wants none of it."""
        w = self.window
        c = self.stars_page()
        w.show()
        w.resize(1920, 1040)
        for _ in range(8):
            self.qt.processEvents()
        matrix = c['matrix']
        self.assertTrue(matrix.verticalHeader().isVisible())
        self.assertGreaterEqual(matrix.verticalHeader().width(), 120)
        self.assertEqual(matrix.verticalHeaderItem(0).text(), '1 Mars')
        self.assertEqual(matrix.horizontalHeaderItem(0).text(), 'Mars')
        widths = [matrix.columnWidth(i) for i in range(matrix.columnCount())]
        self.assertTrue(all(width >= 62 for width in widths), widths)
        self.assertEqual(matrix.item(0, 1).text(), '+500')      # not elided to "..."
        self.assertEqual(matrix.selectionBehavior(),
                         matrix.SelectionBehavior.SelectItems)
        w.hide()

    def test_a_star_takes_its_symbol_from_the_disc(self):
        """A star with no icon of its own falls back to the disc\'s symbol.
        The synthetic fixture leaves that sheet clear, so stand a tile in it;
        the pixels themselves are pinned in test_guardian_stars.DiscIconTest."""
        import struct
        from fm_editor import guardian_stars as gs
        w = self.window
        sheet = bytearray(w.files.wa)
        struct.pack_into('<16H', sheet, gs.ICON_CLUT, *([0] + [0xFFFF] * 15))
        for x, y in gs.ICON_SPOTS:
            start = gs.ICON_SHEET + y * gs.ICON_STRIDE + x // 2
            for row in range(gs.ICON_SIDE):
                at = start + row * gs.ICON_STRIDE
                sheet[at:at + gs.ICON_SIDE // 2] = bytes([0x11] * (gs.ICON_SIDE // 2))
        w.files = SimpleNamespace(wa=bytes(sheet), source=w.files.source)
        c = self.stars_page()
        for star in range(1, gs.RETAIL_COUNT + 1):
            self.assertIsNotNone(w._star_icon_pixmap(star), star)
        self.assertTrue(all(not c['stars'].item(row, 1).icon().isNull()
                            for row in range(gs.RETAIL_COUNT)))
        self.assertFalse(c['icon'].pixmap().isNull())
        self.assertTrue(all(not c['matrix'].horizontalHeaderItem(i).icon().isNull()
                            for i in range(gs.RETAIL_COUNT)))
        # Only the disc's ten have one; a star past them does not.
        self.assertIsNone(gs.disc_icon(bytes(sheet), gs.RETAIL_COUNT + 1))
        self.assertIsNone(gs.disc_icon(bytes(sheet), 0))
        self.assertIsNone(gs.disc_icon(None, 1))

    # --- Packs ------------------------------------------------------------

    def packs_page(self):
        self.window.select_workspace('Packs')
        return self.window.workspace_controls['Packs']

    def test_a_pack_is_added_and_its_fields_are_kept(self):
        from fm_editor import packs as packmath
        w = self.window
        c = self.packs_page()
        self.assertEqual(c['list'].count(), 0)
        w._add_pack()
        self.assertEqual(len(w.project.packs), 1)
        self.assertEqual(c['list'].count(), 1)
        c['name'].setText('Starter Pack')
        c['price'].setValue(5000)          # the spin must reach past Qt's default 99
        c['count'].setValue(9)
        self.assertTrue(w._commit_packs())
        entry = w.project.packs[0]
        self.assertEqual(entry['name'], 'Starter Pack')
        self.assertEqual(entry['price'], 5000)
        self.assertEqual(entry['count'], 9)
        self.assertIn(packmath.pack_id(entry), c['identity'].text())

    def test_the_basic_stock_field_is_not_eaten_by_the_advanced_tab(self):
        """"stock" is one of the Dealing tab's keys, so the order the two are
        applied in decides whether the plain field does anything."""
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        c['infinite'].setChecked(False)
        c['stock'].setValue(7)
        self.assertTrue(w._commit_packs())
        self.assertEqual(w.project.packs[0].get('stock'), 7)
        c['infinite'].setChecked(True)
        self.assertTrue(w._commit_packs())
        self.assertNotIn('stock', w.project.packs[0])

    def test_cards_weights_and_chances(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        w._commit_packs()
        c['weight'].setValue(10)
        w._insert_pack_cards([1, 2, 3, 4, 5])
        self.assertEqual(c['contents'].rowCount(), 5)
        self.assertEqual(c['total'].text(), 'Total weight: 50')
        self.assertEqual([c['contents'].item(r, 4).text() for r in range(5)], ['20.00%'] * 5)
        c['contents'].selectRow(0)
        c['weight'].setValue(90)
        w._set_pack_weight()
        pool = w.project.packs[0]['cards']
        self.assertEqual(sorted(pool.values()), [10, 10, 10, 10, 90])

    def add_tier(self, name, **values):
        """Fill the tier dialog in and accept it."""
        w = self.window
        held = {}

        def fill(window, title, tier_name, tier, on_ok):
            held['said'] = on_ok(name, {'odds': values.get('odds', 1), 'label': values.get('label', ''),
                                        'color': values.get('color'), 'sound': values.get('sound'),
                                        'reveal': values.get('reveal', '')})

        with mock.patch.object(type(w), '_pack_tier_dialog', fill):
            w._add_pack_tier()
        return held.get('said')

    def test_tiers_are_made_and_cards_moved_between_them(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        w._commit_packs()
        c['weight'].setValue(10)
        w._insert_pack_cards([1, 2, 3])
        self.add_tier('rare')
        self.assertIn('rare', w.project.packs[0]['tiers'])
        c['contents'].clearSelection()
        c['contents'].selectRow(0)
        c['tier'].setCurrentText('rare')
        w._set_pack_tier()
        tiers = w.project.packs[0]['tiers']
        self.assertEqual(len(tiers['rare']['cards']), 1)
        self.assertEqual(len(tiers['cards']['cards']), 2)

    def test_duplicate_remove_and_move_packs(self):
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('First')
        w._commit_packs()
        w._duplicate_pack()
        self.assertEqual(len(w.project.packs), 2)
        self.assertEqual([p['name'] for p in w.project.packs], ['First', 'First copy'])
        w._move_pack(-1)
        self.assertEqual([p['name'] for p in w.project.packs], ['First copy', 'First'])
        with mock.patch.object(QMessageBox, 'question',
                               return_value=QMessageBox.StandardButton.Yes):
            w._remove_pack()
        self.assertEqual(len(w.project.packs), 1)

    def test_the_dealing_form_writes_what_the_game_reads(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        c['adv']['duplicates'].setCurrentText('unique_in_pack')
        c['adv']['max_copies'].setText('3')
        c['adv']['guarantee'].setText('cards=2')
        self.assertTrue(w._commit_packs())
        pack = w.project.packs[0]
        self.assertEqual(pack['duplicates'], 'unique_in_pack')
        self.assertEqual(pack['max_copies'], 3)
        self.assertEqual(pack['guarantee'], {'cards': 2})
        # What the game could not read is refused, and nothing is stored.
        c['adv']['max_copies'].setText('nonsense')
        self.assertFalse(w._commit_packs())
        self.assertIn('Max copies', c['problem'].text())
        self.assertEqual(w.project.packs[0]['max_copies'], 3)

    def test_an_untouched_advanced_field_leaves_its_key_as_written(self):
        """A pack a mod wrote its own way is not rewritten by being looked at."""
        w = self.window
        c = self.packs_page()
        w.project.packs.append({'id': 'mine', 'name': 'Mine', 'cover': 2,
                                'unlock': {'beat': 'Heishin', 'wins': 3}})
        w._refresh_packs()
        c['list'].setCurrentRow(0)
        self.assertTrue(w._commit_packs())
        self.assertEqual(w.project.packs[0]['cover'], 2)        # still a number
        self.assertEqual(w.project.packs[0]['unlock'], {'beat': 'Heishin', 'wins': 3})

    def test_the_unlock_and_sound_forms_round_trip(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['adv']['wins'].setText('5')
        c['adv']['story'].setText('0x6E2')
        c['adv']['opened'].setText('starter=2')
        c['adv']['locked'].setCurrentText('shown')
        c['adv']['password'].setText('12345678')
        c['adv']['once'].setChecked(True)
        c['adv']['sound_buy'].setText('40')
        self.assertTrue(w._commit_packs())
        pack = w.project.packs[0]
        self.assertEqual(pack['unlock']['wins'], 5)
        self.assertEqual(pack['unlock']['story'], 0x6E2)
        self.assertEqual(pack['unlock']['opened'], {'starter': 2})
        self.assertEqual(pack['locked'], 'shown')
        self.assertTrue(pack['once'])
        self.assertEqual(pack['sounds']['buy'], 40)
        w._fill_pack()
        self.assertEqual(c['adv']['wins'].text(), '5')
        self.assertEqual(c['adv']['sound_buy'].text(), '40')

    def test_simulating_a_pack_reports_every_card(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        w._commit_packs()
        c['weight'].setValue(10)
        w._insert_pack_cards([1, 2, 3, 4, 5])
        c['sim_count'].setValue(200)
        w._simulate_pack()
        for _ in range(900):
            self.qt.processEvents()
            if c.get('simulation'):
                break
        result = c['simulation']
        self.assertIsNotNone(result)
        self.assertEqual(result.packs, 200)
        self.assertEqual(sum(result.cards.values()), sum(result.tiers.values()))
        w._show_pack_results()
        table = c['sim_results']
        # One row a card, each filled in: the rows used to be inserted and then
        # every card written into the last of them.
        self.assertEqual(table.rowCount(), len(result.cards))
        for row in range(table.rowCount()):
            self.assertTrue(all(table.item(row, col) for col in range(4)), row)

    def test_an_external_packs_file_is_read_only(self):
        from PySide6.QtWidgets import QPushButton
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        w._commit_packs()
        before = dict(w.project.packs[0])
        w.project.packs_file = 'packs.json'
        w._refresh_packs()
        self.assertIn('packs.json', c['status'].text())
        self.assertFalse(c['name'].isEnabled())
        self.assertFalse(c['page'].findChild(QPushButton, 'addPackButton').isEnabled())
        c['name'].setText('Changed')
        self.assertTrue(w._commit_packs())
        self.assertEqual(w.project.packs[0], before)
        w.project.packs_file = None

    # --- Rituals ----------------------------------------------------------

    def rituals_page(self):
        self.window.select_workspace('Rituals')
        return self.window.workspace_controls['Rituals']

    def a_ritual(self, conditions=None):
        """Select the first retail ritual, optionally giving it conditions."""
        w = self.window
        c = self.rituals_page()
        ritual = sorted(w.project.retail.rituals)[0]
        if conditions is not None:
            w.project.ritual_requirements[ritual] = conditions
        w.ritual_current = ritual
        w._refresh_rituals()
        row = next(i for i in range(c['cards'].rowCount())
                   if c['cards'].item(i, 0).data(Qt_UserRole()) == ritual)
        c['cards'].selectRow(row)
        return ritual, c, row

    def edit_ritual(self, mutate):
        """Open the recipe dialog, let `mutate` touch it, then press Save."""
        from PySide6.QtWidgets import QDialog, QPushButton
        def fake_exec(dialog):
            mutate(dialog)
            next(b for b in dialog.findChildren(QPushButton) if b.text() == 'Save').click()
            return dialog.result()
        with mock.patch.object(QDialog, 'exec', fake_exec):
            self.window._edit_ritual()

    def test_removing_a_ritual_takes_its_conditions_with_it(self):
        w = self.window
        ritual, _c, _row = self.a_ritual([{'min_attack': 1500}, {}, {}])
        w._remove_ritual()
        self.assertNotIn(ritual, w.project.rituals)
        self.assertNotIn(ritual, w.project.ritual_requirements)

    def test_reverting_a_ritual_takes_its_conditions_with_it(self):
        w = self.window
        ritual, _c, _row = self.a_ritual([{'min_attack': 1500}, {}, {}])
        w._revert_ritual()
        self.assertEqual(w.project.rituals[ritual], w.project.retail.rituals[ritual])
        self.assertNotIn(ritual, w.project.ritual_requirements)

    def test_a_plain_recipe_replaces_the_conditions_it_is_applied_over(self):
        w = self.window
        ritual, _c, _row = self.a_ritual([{'min_attack': 1500}, {}, {}])
        w.ritual_pending[ritual] = (1, 2, 3, 4)
        w._apply_ritual_selector_recipe()
        self.assertEqual(w.project.rituals[ritual], (1, 2, 3, 4))
        self.assertNotIn(ritual, w.project.ritual_requirements)

    def test_a_conditional_recipe_shows_as_changed_and_locks_the_combos(self):
        w = self.window
        ritual, c, row = self.a_ritual([{'min_attack': 1500}, {'type': 'Dragon'}, {'card': 1}])
        # model.ritual_status counts conditions as a change; the page used to
        # read the recipe alone and call it Stock.
        self.assertEqual(w.project.ritual_status(ritual), 'changed')
        self.assertEqual(c['cards'].item(row, 2).text(), 'Changed')
        self.assertIn('ATK ≥ 1500', c['cards'].item(row, 3).text())
        self.assertIn('Dragon', c['cards'].item(row, 3).text())
        # The three boxes cannot say what the conditions say, so they are shut.
        self.assertTrue(all(not box.isEnabled() for box in c['tribute_combos']))
        self.assertIn('Edit recipe', c['state'].text())

    def test_the_recipe_dialog_edits_conditions(self):
        from PySide6.QtWidgets import QSpinBox
        w = self.window
        ritual, _c, _row = self.a_ritual([{'min_attack': 1500}, {'type': 'Dragon'}, {'card': 1}])
        w.dirty = False
        self.edit_ritual(lambda d: d.findChildren(QSpinBox)[0].setValue(2200))
        self.assertTrue(w.dirty)
        self.assertEqual(w.project.ritual_requirements[ritual],
                         [{'min_attack': 2200}, {'type': 'Dragon'}, {'card': 1}])
        # The cards it shows are the named ones; a condition-only tribute is 0.
        self.assertEqual(w.project.rituals[ritual][:3], (0, 0, 1))

    def test_three_named_cards_need_no_conditions_written(self):
        w = self.window
        ritual, _c, _row = self.a_ritual([{'card': 1}, {'card': 2}, {'card': 3}])
        w.project.rituals[ritual] = (1, 2, 3, 500)
        self.edit_ritual(lambda d: None)
        self.assertEqual(w.project.rituals[ritual], (1, 2, 3, 500))
        self.assertNotIn(ritual, w.project.ritual_requirements)

    def test_the_recipe_dialog_refuses_what_the_game_could_not_read(self):
        from PySide6.QtWidgets import QSpinBox, QLabel
        w = self.window
        ritual, _c, _row = self.a_ritual([{}, {'card': 2}, {'card': 3}])
        before = dict(w.project.rituals)
        self.edit_ritual(lambda d: None)                    # a tribute with nothing in it
        self.assertEqual(w.project.rituals, before)
        # and a minimum past its maximum
        self.a_ritual([{'min_attack': 1000, 'max_attack': 2000}, {'card': 2}, {'card': 3}])
        before = dict(w.project.rituals)
        def crossed(dialog):
            spins = dialog.findChildren(QSpinBox)
            spins[0].setValue(3000)
            spins[1].setValue(100)
        self.edit_ritual(crossed)
        self.assertEqual(w.project.rituals, before)

    def test_the_recipe_column_reads_every_kind_of_condition(self):
        w = self.window
        ritual, c, _row = self.a_ritual([{'min_level': 5, 'max_level': 8},
                                         {'defense_gt_attack': True}, {'card': 1}])
        row = next(i for i in range(c['cards'].rowCount())
                   if c['cards'].item(i, 0).data(Qt_UserRole()) == ritual)
        text = c['cards'].item(row, 3).text()
        for part in ('Level ≥ 5', 'Level ≤ 8', 'DEF > ATK', 'Blue Dragon'):
            self.assertIn(part, text)

    # --- Equips -----------------------------------------------------------

    def equips_page(self):
        self.window.select_workspace('Equips')
        return self.window.workspace_controls['Equips']

    def test_the_equips_page_opens_on_a_card(self):
        w = self.window
        c = self.equips_page()
        self.assertGreater(c['equips'].rowCount(), 0)
        self.assertEqual(c['equips'].currentRow(), 0)
        self.assertIsNotNone(w.equip_current)
        self.assertEqual(c['monsters'].rowCount(), len(w.project.monsters()))

    def test_the_equip_searches_match_a_card_number(self):
        w = self.window
        c = self.equips_page()
        equip = w.equip_current
        # Names and text without digits, so only the number column can match.
        for card in w.project.cards.values():
            card.name, card.description = 'a nameless card', ''
        w.project.cards[equip].name = 'Dragon Capture Jar'
        w._refresh_equips()
        c['search'].setText(str(equip))
        shown = [r for r in range(c['equips'].rowCount()) if not c['equips'].isRowHidden(r)]
        self.assertEqual(len(shown), 1)
        c['search'].setText('Capture')
        shown = [r for r in range(c['equips'].rowCount()) if not c['equips'].isRowHidden(r)]
        self.assertEqual(len(shown), 1)
        c['search'].clear()
        monster = w.project.monsters()[0]
        c['monster_search'].setText(str(monster))
        shown = [r for r in range(c['monsters'].rowCount()) if not c['monsters'].isRowHidden(r)]
        self.assertEqual(len(shown), 1)
        c['monster_search'].clear()

    def test_equip_rows_are_coloured_by_their_state(self):
        w = self.window
        c = self.equips_page()
        equip = w.equip_current
        baseline = w.project.equip_baseline(equip)
        self.assertTrue(baseline)
        # Keep one of the disc's and allow one it never allowed.
        w.project.equips[equip] = set(list(baseline)[:1]) | {max(w.project.monsters())}
        w._refresh_equips()
        row = next(r for r in range(c['equips'].rowCount())
                   if c['equips'].item(r, 0).data(Qt_UserRole()) == equip)
        self.assertEqual(c['equips'].item(row, 0).foreground().color().name(), '#8ab4f8')
        c['equips'].selectRow(row)
        w._select_equip()
        monsters = c['monsters']
        added = next(r for r in range(monsters.rowCount()) if monsters.item(r, 4).text() == 'Added')
        removed = next(r for r in range(monsters.rowCount()) if monsters.item(r, 4).text() == 'Removed')
        self.assertEqual(monsters.item(added, 1).foreground().color().name(), '#7fd49b')
        self.assertEqual(monsters.item(removed, 1).foreground().color().name(), '#ff8f87')

    def test_equips_are_edited_by_tick_type_and_revert(self):
        from PySide6.QtCore import Qt
        w = self.window
        c = self.equips_page()
        equip = w.equip_current
        monsters = c['monsters']
        row = next(r for r in range(monsters.rowCount())
                   if monsters.item(r, 0).data(Qt_UserRole()) not in w.project.equips[equip])
        monster = monsters.item(row, 0).data(Qt_UserRole())
        w.dirty = False
        monsters.item(row, 0).setCheckState(Qt.CheckState.Checked)
        self.assertIn(monster, w.project.equips[equip])
        self.assertTrue(w.dirty)
        c['type'].setCurrentIndex(0)
        w._equip_by_type(True)
        self.assertTrue(any(w.project.cards[m].type == 0 for m in w.project.equips[equip]))
        w._equip_by_type(False)
        self.assertFalse(any(w.project.cards[m].type == 0 for m in w.project.equips[equip]))
        w._revert_equip()
        self.assertEqual(w.project.equips[equip], w.project.equip_baseline(equip))

    # --- Duelists ---------------------------------------------------------

    def duelists_page(self):
        self.window.select_workspace('Duelists')
        return self.window.workspace_controls['Duelists']

    def duelist_name_label(self, slot):
        from PySide6.QtWidgets import QLabel
        c = self.window.workspace_controls['Duelists']
        cell = slot - c['page'].currentIndex() * 40
        tile = c['duelists'].cellWidget(cell // 8, cell % 8)
        return next(l for l in tile.findChildren(QLabel) if '·' in l.text())

    def test_a_duelist_can_be_dealt_a_fixed_deck(self):
        from fm_editor import fixed_decks
        from fm_editor.gamedata import DECK_SIZE
        w = self.window
        c = self.duelists_page()
        duelist = w._selected_duelist()
        self.assertEqual(w._selected_pool_name(), 'deck')
        self.assertIsNone(fixed_decks.deck_of(w.project, duelist))
        w.dirty = False
        c['fixed_radio'].setChecked(True)
        deck = fixed_decks.deck_of(w.project, duelist)
        self.assertIsNotNone(deck)
        # It starts from the forty the weighted deck most likely deals.
        self.assertEqual(deck.total(), DECK_SIZE)
        self.assertTrue(w.dirty)
        self.assertGreater(c['fixed_table'].rowCount(), 0)
        self.assertIn('fixed deck', c['summary'].text())

    def test_switching_back_to_weighted_keeps_the_fixed_deck_to_hand(self):
        from fm_editor import fixed_decks
        w = self.window
        c = self.duelists_page()
        duelist = w._selected_duelist()
        c['fixed_radio'].setChecked(True)
        kept = dict(fixed_decks.deck_of(w.project, duelist).cards)
        c['weighted_radio'].setChecked(True)
        self.assertIsNone(fixed_decks.deck_of(w.project, duelist))
        self.assertIn(duelist, w.fixed_stash)
        c['fixed_radio'].setChecked(True)
        self.assertEqual(fixed_decks.deck_of(w.project, duelist).cards, kept)

    def test_the_fixed_deck_cards_are_edited(self):
        from fm_editor import fixed_decks
        from fm_editor.gamedata import DECK_SIZE
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        c = self.duelists_page()
        duelist = w._selected_duelist()
        c['fixed_radio'].setChecked(True)
        table = c['fixed_table']
        cid = table.item(0, 0).data(Qt_UserRole())
        table.selectRow(0)
        self.assertEqual(c['copies'].value(), fixed_decks.deck_of(w.project, duelist).cards[cid])
        c['copies'].setValue(7)
        w._set_fixed_copies()
        self.assertEqual(fixed_decks.deck_of(w.project, duelist).cards[cid], 7)
        table.selectRow(0)
        w._remove_fixed_cards()
        self.assertNotIn(cid, fixed_decks.deck_of(w.project, duelist).cards)
        with mock.patch.object(QMessageBox, 'question',
                               return_value=QMessageBox.StandardButton.Yes):
            w._copy_weighted_deck()
        self.assertEqual(fixed_decks.deck_of(w.project, duelist).total(), DECK_SIZE)
        w._clear_fixed_deck()
        self.assertEqual(fixed_decks.deck_of(w.project, duelist).total(), 0)

    def test_reverting_takes_the_fixed_deck_and_the_pool_edits(self):
        from fm_editor import fixed_decks
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        c = self.duelists_page()
        duelist = w._selected_duelist()
        pool = w.project.pools[duelist]['deck']
        pool[sorted(pool)[0]] += 5
        c['fixed_radio'].setChecked(True)
        with mock.patch.object(QMessageBox, 'question',
                               return_value=QMessageBox.StandardButton.Yes):
            w._revert_fixed_deck()
        self.assertIsNone(fixed_decks.deck_of(w.project, duelist))
        self.assertEqual(w.project.pools[duelist]['deck'],
                         w.project.retail.pools[duelist]['deck'])

    def test_a_card_the_deck_names_but_the_game_lacks_keeps_its_row(self):
        from fm_editor import fixed_decks
        w = self.window
        c = self.duelists_page()
        duelist = w._selected_duelist()
        c['fixed_radio'].setChecked(True)
        deck = fixed_decks.deck_of(w.project, duelist)
        deck.kept = {'No Such Card': 3}
        w._refresh_duelist_pool()
        rows = [c['fixed_table'].item(r, 1).text() for r in range(c['fixed_table'].rowCount())]
        self.assertIn('No Such Card', rows)
        row = rows.index('No Such Card')
        self.assertIn('no such card', c['fixed_table'].item(row, 5).text())

    def test_the_pool_list_and_the_grid_show_what_changed(self):
        from fm_editor import fixed_decks
        w = self.window
        c = self.duelists_page()
        duelist = w._selected_duelist()
        pool = w.project.pools[duelist]['deck']
        card = sorted(pool)[0]
        pool[card] += 5
        w._refresh_duelist_pool()
        row = next(r for r in range(c['table'].rowCount())
                   if c['table'].item(r, 0).data(Qt_UserRole()) == card)
        self.assertEqual(c['table'].item(row, 6).text(), 'Changed')
        self.assertEqual(c['table'].item(row, 0).foreground().color().name(), '#8ab4f8')
        # The total is no longer 2048, and says so.
        self.assertIn('#ff7777', c['summary'].styleSheet())
        # The grid marks the duelist too.
        w._refresh_duelists()
        self.assertIn('changed', self.duelist_name_label(duelist).toolTip())
        fixed_decks.set_deck(w.project, duelist,
                             fixed_decks.most_likely(w.project.pools[duelist]['deck']))
        w._refresh_duelists()
        self.assertIn('fixed', self.duelist_name_label(duelist).toolTip())

    # --- Starter decks and the retail pools --------------------------------

    def starter_page(self):
        self.window.select_workspace('Starter decks')
        return self.window.workspace_controls['Starter decks']

    def test_the_written_decks_still_work(self):
        from fm_editor.gamedata import DECK_SIZE
        w = self.window
        c = self.starter_page()
        self.assertEqual([c['starter_tabs'].tabText(i) for i in range(c['starter_tabs'].count())],
                         ['Written decks', 'Weighted pools'])
        w._add_starter_deck()
        self.assertEqual(len(w.project.starter), 1)
        c['name'].setText('Spellbinder')
        c['weight'].setValue(3)
        w._apply_starter_details()
        self.assertEqual(w.project.starter[0].name, 'Spellbinder')
        self.assertEqual(w.project.starter[0].weight, 3)
        self.assertIn(f'/{DECK_SIZE}', c['decks'].item(0, 3).text())

    def test_a_mod_with_no_pools_of_its_own_says_so(self):
        """Nothing is weighted until the mod writes a pool: the tab says what
        a new game would do instead."""
        c = self.starter_page()
        self.assertEqual(c['pools'].rowCount(), 0)
        self.assertIn('No pools', c['pool_total'].text())

    # --- Mod info ----------------------------------------------------------

    def mod_info_page(self):
        self.window.select_workspace('Mod info')
        return self.window.workspace_controls['Mod info']

    def test_the_other_box_leaves_the_editor_s_own_keys_alone(self):
        """The box shows every key but the ones a page of the editor's own
        writes, and applying it keeps those as that page left them."""
        import json
        w = self.window
        w.project.other['limits'] = {'hand': 7}
        w.project.other['guardian_stars'] = {'names': {}}
        w.project.other['starter_pools'] = [{'draws': 40, 'cards': {'1': 1}}]
        w.project.other['story'] = {'scene': 1}
        w.project.other['textures'] = ['art/']
        c = self.mod_info_page()
        shown = json.loads(c['other'].toPlainText())
        self.assertEqual(sorted(shown), ['textures'])
        # Applying the box as it stands must not drop what it never showed.
        c['other'].setPlainText(json.dumps({'textures': ['art/', 'ui/']}))
        self.assertTrue(w._apply_mod_info())
        self.assertEqual(w.project.other['limits'], {'hand': 7})
        self.assertEqual(w.project.other['guardian_stars'], {'names': {}})
        self.assertEqual(w.project.other['starter_pools'], [{'draws': 40, 'cards': {'1': 1}}])
        self.assertEqual(w.project.other['story'], {'scene': 1})
        self.assertEqual(w.project.other['textures'], ['art/', 'ui/'])

    def test_a_key_the_editor_writes_is_refused_rather_than_lost(self):
        """Typing one in the box would be written over when the manifest is
        built, so say which page writes it instead."""
        import json
        w = self.window
        c = self.mod_info_page()
        before = dict(w.project.other)
        for key in ('cards', 'starter', 'packs', 'pack_shop', 'limits', 'starter_pools'):
            c['other'].setPlainText(json.dumps({key: []}))
            self.assertFalse(w._apply_mod_info(), key)
            self.assertIn(key, c['status'].text())
            self.assertEqual(dict(w.project.other), before)

    def test_the_page_says_where_the_mod_is(self):
        import tempfile
        from pathlib import Path
        w = self.window
        c = self.mod_info_page()
        self.assertEqual(c['folder'].text(), 'Not saved yet')
        with tempfile.TemporaryDirectory() as folder:
            w.project.source_dir = Path(folder)
            w._refresh_mod_info()
            self.assertIn(folder, c['folder'].text())

    def test_a_refused_apply_is_said_once(self):
        """The message goes when the page is shown again, as the Tk tab's."""
        w = self.window
        c = self.mod_info_page()
        c['id'].setText('not a valid id!')
        self.assertFalse(w._apply_mod_info())
        self.assertTrue(c['status'].text())
        c['id'].setText('my-mod')
        w._refresh_mod_info()
        self.assertEqual(c['status'].text(), '')

    def test_the_identity_fields_round_trip(self):
        w = self.window
        c = self.mod_info_page()
        c['id'].setText('deck-mod')
        c['name'].setText('Deck Mod')
        c['version'].setText(' 2.1 ')
        c['author'].setText('Somebody')
        c['description'].setPlainText('A mod.')
        self.assertTrue(w._apply_mod_info())
        info = w.project.info
        self.assertEqual((info.id, info.name, info.version, info.author, info.description),
                         ('deck-mod', 'Deck Mod', '2.1', 'Somebody', 'A mod.'))
        w._refresh_mod_info()
        self.assertEqual(c['version'].text(), '2.1')

    # --- Problems ----------------------------------------------------------

    def problems_page(self):
        self.window.select_workspace('Problems')
        return self.window.workspace_controls['Problems']

    def pool_fixture(self):
        """Pools that draw too few cards: an error and a warning apiece."""
        w = self.window
        card = sorted(w.project.cards)[0]
        w.project.other['starter_pools'] = [
            {'name': 'Weak', 'draws': 16, 'cards': {w.project.ref(card): 100}},
            {'name': 'Odd', 'draws': 9, 'cards': {'No Such Card At All': 5}},
        ]
        w.project.starter_pool_state = None

    def test_every_line_is_drawn_in_its_level_s_ink(self):
        self.pool_fixture()
        c = self.problems_page()
        self.assertTrue(c['issues'])
        for row, issue in enumerate(c['issues']):
            colour = c['table'].item(row, 0).foreground().color().name()
            self.assertEqual(colour, '#ff8f87' if issue.level == 'error' else '#f2c04c',
                             issue.message)

    def test_the_summary_counts_errors_and_warnings(self):
        from fm_editor import validate
        self.pool_fixture()
        c = self.problems_page()
        errors = len(validate.errors(c['issues']))
        self.assertIn(f'{errors} error(s)', c['summary'].text())
        self.assertIn(f'{len(c["issues"]) - errors} warning(s)', c['summary'].text())
        self.assertIn('#ff8f87' if errors else '#f2c04c', c['summary'].styleSheet())

    def test_a_clean_mod_says_so_in_green(self):
        c = self.problems_page()
        self.window._refresh_problems()
        if c['issues']:
            self.skipTest('this fixture has problems of its own')
        self.assertEqual(c['summary'].text(), 'No problems found.')
        self.assertIn('#7fd49b', c['summary'].styleSheet())

    def test_a_starter_pool_line_goes_to_its_pool(self):
        """"Starter pools" is the Starter decks page's second tab, not a page
        of its own: it used to be reported as missing from the editor."""
        self.pool_fixture()
        w = self.window
        c = self.problems_page()
        row = next(r for r, i in enumerate(c['issues'])
                   if i.area == 'Starter pools' and i.target == 1)
        w._open_problem(row)
        self.assertEqual(w.current_workspace, 'Starter decks')
        starter = w.workspace_controls['Starter decks']
        self.assertEqual(starter['starter_tabs'].currentIndex(), 1)
        self.assertEqual(starter['pools'].currentRow(), 1)

    def test_a_packs_line_goes_to_its_pack(self):
        from fm_editor.validate import Issue
        w = self.window
        self.window.select_workspace('Packs')
        w._add_pack()
        w._add_pack()
        w.go_to(Issue('error', 'Packs', 'pack 1', 'something', 1))
        self.assertEqual(w.current_workspace, 'Packs')
        self.assertEqual(w.workspace_controls['Packs']['list'].currentRow(), 1)

    def test_a_double_click_goes_to_the_line_it_is_on(self):
        self.pool_fixture()
        w = self.window
        c = self.problems_page()
        row = next(r for r, i in enumerate(c['issues']) if i.area == 'Starter pools')
        c['table'].cellDoubleClicked.emit(row, 0)
        self.assertEqual(w.current_workspace, 'Starter decks')

    def test_a_pack_with_a_cover_shows_it_in_the_list(self):
        """The cover is drawn as the row's icon; nothing else in the suite
        reaches that line, and a missing import went unseen there."""
        from PySide6.QtGui import QImage
        from PySide6.QtCore import QBuffer, QByteArray
        w = self.window
        c = self.packs_page()
        w._add_pack()
        entry = w._pack_entry()
        image = QImage(34, 52, QImage.Format.Format_RGB32)
        image.fill(0x204080)
        blob = QByteArray()
        buffer = QBuffer(blob)
        buffer.open(QBuffer.OpenModeFlag.WriteOnly)
        image.save(buffer, 'PNG')
        buffer.close()
        w.project.files['packs/cover.png'] = bytes(blob)
        entry['image'] = 'packs/cover.png'
        w._refresh_packs()
        self.assertFalse(c['list'].item(0).icon().isNull())

    def pack_png(self, width=204, height=192, alpha=False):
        """A PNG on disk for the import dialog to return."""
        import tempfile
        from fm_editor import pngio
        pixels = bytearray()
        for _ in range(width * height):
            pixels += bytes((0x20, 0x40, 0x80, 0x00 if alpha else 0xFF))
        image = pngio.Image(width, height, bytes(pixels))
        handle = tempfile.NamedTemporaryFile(suffix='.png', delete=False)
        handle.write(pngio.encode(image))
        handle.close()
        self.addCleanup(lambda: Path(handle.name).unlink(missing_ok=True))
        return handle.name

    def test_an_imported_cover_is_cut_to_the_pack_s_shape(self):
        """art.normalize's, not the file as it came: the game cannot draw a
        picture of the wrong shape, and a hole in it is a hole in the card."""
        from fm_editor import pngio, art
        w = self.window
        c = self.packs_page()
        w._add_pack()
        path = self.pack_png(300, 100, alpha=True)
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(path, '')):
            w._import_pack_image()
        stored = pngio.decode(w.project.files[w._pack_entry()['image']])
        shape = art.SIZES['art']
        self.assertAlmostEqual(stored.width / stored.height, shape[0] / shape[1], places=2)
        self.assertTrue(stored.opaque())
        self.assertLessEqual(stored.width, shape[0] * art.MAX_SCALE)
        self.assertIn('Pack picture from', c['status'].text())

    def test_reverting_a_cover_takes_its_file_with_it(self):
        w = self.window
        self.packs_page()
        w._add_pack()
        path = self.pack_png()
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(path, '')):
            w._import_pack_image()
        name = w._pack_entry()['image']
        self.assertIn(name, w.project.files)
        w._revert_pack_image()
        self.assertNotIn('image', w._pack_entry())
        self.assertNotIn(name, w.project.files)

    def test_a_cover_two_packs_share_is_not_taken_from_the_other(self):
        """A duplicated pack names the original's picture: reverting one must
        leave the other's showing."""
        w = self.window
        c = self.packs_page()
        w._add_pack()
        path = self.pack_png()
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(path, '')):
            w._import_pack_image()
        name = w._pack_entry()['image']
        w._duplicate_pack()
        self.assertEqual(w._pack_entry()['image'], name)
        w._revert_pack_image()
        self.assertIn(name, w.project.files)
        c['list'].setCurrentRow(0)
        self.assertEqual(w._pack_entry()['image'], name)

    def test_a_second_cover_does_not_leave_the_first_behind(self):
        """Re-importing reuses the pack's own name rather than piling up
        pictures nothing draws."""
        w = self.window
        self.packs_page()
        w._add_pack()
        first, second = self.pack_png(), self.pack_png(160, 150)
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(first, '')):
            w._import_pack_image()
        before = w._pack_entry()['image']
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(second, '')):
            w._import_pack_image()
        self.assertEqual(w._pack_entry()['image'], before)
        self.assertEqual([k for k in w.project.files if k.startswith('packs/')], [before])

    def test_the_cover_zoom_box_changes_what_is_drawn(self):
        """It was wired to the preview but the preview ignored it."""
        from fm_editor import art
        w = self.window
        c = self.packs_page()
        w._add_pack()
        side = art.SIZES['art']
        path = self.pack_png(side[0] * 4, side[1] * 4)
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(path, '')):
            w._import_pack_image()
        sizes = []
        for index in range(c['image_scale'].count()):
            c['image_scale'].setCurrentIndex(index)
            sizes.append(c['image'].pixmap().width())
        self.assertEqual(sizes, [side[0], side[0] * 2, side[0] * 4])

    def test_export_and_revert_follow_the_picture(self):
        from PySide6.QtWidgets import QPushButton
        w = self.window
        c = self.packs_page()
        w._add_pack()
        page = c['page']
        export = page.findChild(QPushButton, 'exportPackImageButton')
        revert = page.findChild(QPushButton, 'revertPackImageButton')
        w._refresh_pack_image()
        self.assertFalse(export.isEnabled())
        self.assertFalse(revert.isEnabled())
        path = self.pack_png()
        with mock.patch('fm_editor.pyside_app.QFileDialog.getOpenFileName', return_value=(path, '')):
            w._import_pack_image()
        self.assertTrue(export.isEnabled())
        self.assertTrue(revert.isEnabled())
        w._revert_pack_image()
        self.assertFalse(export.isEnabled())
        self.assertFalse(revert.isEnabled())

    def test_the_three_panels_fit_the_window_they_demand(self):
        """They asked for 1270 of a splitter that holds 1079 at the window's
        own minimum, so the settings panel was drawn over the contents."""
        from PySide6.QtWidgets import QSplitter
        w = self.window
        c = self.packs_page()
        w.resize(w.minimumWidth(), w.minimumHeight())
        w.show()
        self.addCleanup(w.hide)
        self.qt.processEvents()
        panels = c['page'].findChild(QSplitter, 'packsSplitter')
        for i in range(panels.count()):
            panel = panels.widget(i)
            # Wide enough for what is in it, and inside the splitter: a panel
            # past the right edge is drawn over the one beside it.
            self.assertGreaterEqual(panel.width(), panel.minimumSizeHint().width(),
                                    panel.objectName())
            self.assertLessEqual(panel.x() + panel.width(), panels.width(),
                                 panel.objectName())

    # --- the contents rows, as the template draws them ----------------------

    def pack_with_cards(self, ids=(1, 2, 3)):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['weight'].setValue(10)
        w._insert_pack_cards(list(ids))
        return c

    def test_a_row_carries_its_own_weight_box(self):
        from PySide6.QtWidgets import QSpinBox
        from fm_editor import packs as packmath
        w = self.window
        c = self.pack_with_cards()
        box = c['contents'].cellWidget(0, 3)
        self.assertIsInstance(box, QSpinBox)
        self.assertEqual(box.value(), 10)
        box.setValue(250)
        pool = dict(packmath.tier_pool(w.project.packs[0], 'cards'))
        self.assertEqual(pool[w.project.ref(1)], 250)
        self.assertEqual(c['contents'].cellWidget(1, 3).value(), 10)   # only that one

    def test_a_row_carries_its_own_tier_box(self):
        from PySide6.QtWidgets import QComboBox
        w = self.window
        c = self.pack_with_cards()
        self.add_tier('rare')
        box = c['contents'].cellWidget(0, 2)
        # The tier is named in the row, not numbered.
        self.assertIsInstance(box, QComboBox)
        self.assertEqual([box.itemText(i) for i in range(box.count())], ['cards', 'rare'])
        self.assertEqual(box.currentText(), 'cards')
        box.setCurrentText('rare')
        tiers = w.project.packs[0]['tiers']
        self.assertEqual(len(tiers['rare']['cards']), 1)
        self.assertEqual(len(tiers['cards']['cards']), 2)

    def test_a_row_is_taken_out_by_its_own_cross(self):
        from fm_editor import packs as packmath
        w = self.window
        c = self.pack_with_cards()
        self.assertEqual(c['contents'].rowCount(), 3)
        c['contents'].cellWidget(1, 5).click()
        self.assertEqual(c['contents'].rowCount(), 2)
        left = [ref for ref, _ in packmath.tier_pool(w.project.packs[0], 'cards')]
        self.assertEqual(left, [w.project.ref(1), w.project.ref(3)])

    def test_the_description_is_counted_in_the_bytes_the_game_keeps(self):
        from fm_editor import packs as packmath
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['description'].setPlainText('é' * 10)           # two bytes apiece
        self.assertEqual(c['count_label'].text(), f'20 / {packmath.DESCRIPTION_BYTES}')
        self.assertNotIn('#ff8f87', c['count_label'].styleSheet())
        c['description'].setPlainText('x' * (packmath.DESCRIPTION_BYTES + 1))
        self.assertIn('#ff8f87', c['count_label'].styleSheet())

    def test_the_list_says_how_many_packs_it_holds(self):
        w = self.window
        c = self.packs_page()
        self.assertEqual(c['list_title'].text(), 'Pack list (0)')
        w._add_pack()
        w._add_pack()
        self.assertEqual(c['list_title'].text(), 'Pack list (2)')

    def test_a_pack_without_a_picture_is_drawn_with_its_name_plate(self):
        """The preview is the pack as the game draws it, not the raw file."""
        from fm_editor import art
        w = self.window
        c = self.packs_page()
        w._add_pack()
        c['name'].setText('Starter')
        w._commit_packs()
        w._refresh_pack_image()
        drawn = c['image'].pixmap()
        self.assertFalse(drawn.isNull())
        # art over gold, and the plate's two pixels of gap and fourteen rows
        self.assertEqual((drawn.width(), drawn.height()),
                         (art.SIZES['art'][0], art.SIZES['art'][1] + 2 + 14))

    # --- what the advanced forms and the dialogs do --------------------------

    def test_a_tier_is_renamed_and_what_names_it_follows(self):
        """Its guarantee, its pity and the slots that name it (rename_tier)."""
        w = self.window
        c = self.packs_page()
        w._add_pack()
        w._insert_pack_cards([1, 2])
        self.add_tier('rare', odds=4, label='RARE!', reveal='quick')
        entry = w.project.packs[0]
        entry['guarantee'] = {'rare': 1}
        entry['pity'] = {'rare': 5}
        entry['slots'] = ['cards', 'rare']
        w._refresh_packs()
        tiers = c['adv']['tiers']
        self.assertEqual(tiers.rowCount(), 2)
        self.assertEqual(tiers.item(1, 0).text(), 'rare')
        self.assertEqual(tiers.item(1, 1).text(), '4')
        self.assertEqual(tiers.item(1, 3).text(), 'RARE!')
        tiers.selectRow(1)

        def fill(window, title, name, tier, on_ok):
            self.assertEqual(name, 'rare')
            self.assertEqual(tier.get('odds'), 4)
            on_ok('ultra', {'odds': 9, 'label': 'ULTRA!', 'color': None, 'sound': None, 'reveal': ''})

        with mock.patch.object(type(w), '_pack_tier_dialog', fill):
            w._edit_pack_tier()
        entry = w.project.packs[0]
        self.assertIn('ultra', entry['tiers'])
        self.assertNotIn('rare', entry['tiers'])
        self.assertEqual(entry['tiers']['ultra']['odds'], 9)
        self.assertEqual(entry['guarantee'], {'ultra': 1})
        self.assertEqual(entry['pity'], {'ultra': 5})
        self.assertEqual(entry['slots'], ['cards', 'ultra'])

    def test_a_tier_is_removed_and_reordered(self):
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        c = self.packs_page()
        w._add_pack()
        w._insert_pack_cards([1])
        self.add_tier('rare')
        tiers = c['adv']['tiers']
        self.assertEqual([tiers.item(r, 0).text() for r in range(2)], ['cards', 'rare'])
        tiers.selectRow(1)
        w._move_pack_tier(-1)
        self.assertEqual(list(w.project.packs[0]['tiers']), ['rare', 'cards'])
        c['adv']['tiers'].selectRow(0)
        with mock.patch.object(QMessageBox, 'question',
                               return_value=QMessageBox.StandardButton.Yes):
            w._remove_pack_tier()
        self.assertEqual(list(w.project.packs[0]['tiers']), ['cards'])

    def test_slots_are_turned_on_and_given_rules(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        w._insert_pack_cards([1])
        self.add_tier('rare')
        c['adv']['use_slots'].setChecked(True)
        w._toggle_pack_slots()
        entry = w.project.packs[0]
        self.assertEqual(entry['slots'], ['cards'] * 5)       # one for each of its cards
        self.assertNotIn('count', entry)
        self.assertEqual(c['adv']['slots'].rowCount(), 5)
        c['adv']['slots'].selectRow(4)

        def fill(window, title, rule, on_ok):
            on_ok({'tiers': {'cards': 3, 'rare': 1}})

        with mock.patch.object(type(w), '_pack_slot_dialog', fill):
            w._edit_pack_slot()
        self.assertEqual(w.project.packs[0]['slots'][4], {'tiers': {'cards': 3, 'rare': 1}})
        c['adv']['slots'].selectRow(4)
        w._remove_pack_slot()
        self.assertEqual(len(w.project.packs[0]['slots']), 4)
        # Turning them off again keeps the count they stood for.
        c['adv']['use_slots'].setChecked(False)
        w._toggle_pack_slots()
        self.assertNotIn('slots', w.project.packs[0])
        self.assertEqual(w.project.packs[0]['count'], 4)

    def test_the_shop_settings_form_writes_the_shop_s_own_rules(self):
        w = self.window
        self.packs_page()
        held = {}

        def run(window, title, build, ok):
            body = QGridLayout()
            build(body)
            held['said'] = ok()
            held['body'] = body

        with mock.patch.object(type(w), '_pack_form_dialog', run):
            w._edit_pack_shop()
        self.assertIsNone(held['said'])
        # Nothing was touched, so nothing is written.
        self.assertIn(w.project.pack_shop, (None, {}))

    def test_the_shop_settings_form_reads_its_shops_back(self):
        w = self.window
        self.packs_page()
        w.project.pack_shop = {'shops': [{'id': 'card_shop', 'name': 'Cards', 'where': 'town'}]}
        seen = {}

        def run(window, title, build, ok):
            body = QGridLayout()
            build(body)
            seen['before'] = window.workspace_controls['Packs']
            # The form's own widgets are the ones build() made; find the box.
            for i in range(body.count()):
                widget = body.itemAt(i).widget()
                if isinstance(widget, QPlainTextEdit):
                    seen['text'] = widget.toPlainText()
                    widget.setPlainText('card_shop | Cards\npack_shop | Packs')
            held = ok()
            seen['said'] = held

        with mock.patch.object(type(w), '_pack_form_dialog', run):
            w._edit_pack_shop()
        self.assertEqual(seen['text'], 'card_shop | Cards')
        self.assertIsNone(seen['said'])
        shops = w.project.pack_shop['shops']
        self.assertEqual([shop['id'] for shop in shops], ['card_shop', 'pack_shop'])
        self.assertEqual(shops[0]['where'], 'town')      # a key the form has no field for

    def test_a_shop_line_the_game_could_not_read_is_refused(self):
        w = self.window
        self.packs_page()
        said = {}

        def run(window, title, build, ok):
            body = QGridLayout()
            build(body)
            for i in range(body.count()):
                widget = body.itemAt(i).widget()
                if isinstance(widget, QPlainTextEdit):
                    widget.setPlainText('not a valid id!')
            said['text'] = ok()

        with mock.patch.object(type(w), '_pack_form_dialog', run):
            w._edit_pack_shop()
        self.assertIn('an id is', said['text'])
        self.assertIn(w.project.pack_shop, (None, {}))

    def test_add_filtered_picks_cards_by_what_they_are(self):
        """Not by name alone: the filter the bulk dialogs use."""
        from fm_editor import bulk_fusions, gamedata
        w = self.window
        c = self.packs_page()
        w._add_pack()
        monsters = [cid for cid, card in w.project.cards.items() if card.is_monster()]
        self.assertTrue(monsters)
        chosen = bulk_fusions.CardFilter(kinds={'monster'}).select(w.project)[0]
        self.assertEqual(sorted(chosen), sorted(monsters))
        with mock.patch.object(type(w), '_card_filter_dialog', lambda self, *a, **k: chosen[:3]):
            w._add_filtered_pack_cards()
        self.assertEqual(c['contents'].rowCount(), 3)

    # --- the Campaign page while the story tabs are away ---------------------

    def test_the_campaign_page_is_the_map_for_now(self):
        """The scene editor, the timeline and the story assets come with the
        campaign work, in a PR of their own. This page is the map."""
        w = self.window
        w.select_workspace('Campaign')
        c = w.workspace_controls['Campaign']
        self.assertEqual([c['tabs'].tabText(i) for i in range(c['tabs'].count())],
                         ['Map'])

    def test_a_mod_s_story_key_is_left_alone(self):
        """No page writes "story": the editor keeps it as the mod wrote it."""
        w = self.window
        w.project.other['story'] = {'scenes': [{'id': 'intro'}]}
        w.select_workspace('Campaign')
        w.select_workspace('Cards')
        self.assertEqual(w.project.other['story'], {'scenes': [{'id': 'intro'}]})
        self.assertIn('story', manifest.build(w.project))

    def test_a_map_problem_still_goes_to_its_place(self):
        from fm_editor.validate import Issue
        w = self.window
        w.go_to(Issue('warning', 'Map', 'place 3', 'something', 3))
        self.assertEqual(w.current_workspace, 'Campaign')
        c = w.workspace_controls['Campaign']
        self.assertEqual(c['tabs'].tabText(c['tabs'].currentIndex()), 'Map')

    def test_the_packs_page_has_no_bottom_button_row(self):
        """Apply is what leaving a pack does; the other two were each other."""
        from PySide6.QtWidgets import QPushButton
        c = self.packs_page()
        for name in ('applyPacksButton', 'resetAllPacksButton', 'revertAllPacksButton'):
            self.assertIsNone(c['page'].findChild(QPushButton, name), name)

    def test_removing_every_pack_is_still_reachable(self):
        """It lost its button, so the Tools menu keeps it."""
        w = self.window
        titles = [action.text() for menu in w.menuBar().actions()
                  if menu.menu() for action in menu.menu().actions()]
        self.assertIn('Remove every pack of the mod', titles)

    def test_a_pack_still_commits_without_an_apply_button(self):
        w = self.window
        c = self.packs_page()
        w._add_pack()
        w._add_pack()
        c['list'].setCurrentRow(0)
        c['name'].setText('Renamed')
        c['list'].setCurrentRow(1)          # leaving the pack writes it
        self.assertEqual(w.project.packs[0]['name'], 'Renamed')

    def test_the_disc_s_own_pools_stand_in_for_a_mod_that_weights_none(self):
        """With the disc's rows to hand the page shows them, read only."""
        from PySide6.QtWidgets import QPushButton
        from fm_editor import starter_pools
        w = self.window
        c = self.starter_page()
        rows = [starter_pools.Pool(name=f"Row {n + 1}", draws=draws, cards={1: 2048})
                for n, draws in enumerate((16, 16, 4, 1, 1, 1, 1))]
        w.starter_pools_retail = rows
        w._refresh_starter_pools()
        self.assertFalse(w._starter_pools_editable())
        self.assertEqual(c['pools'].rowCount(), 7)
        self.assertIn("The disc's own rows", c['pool_total'].text())
        form = w.workspace_forms['Starter decks']
        self.assertFalse(form.findChild(QPushButton, 'removeStarterPoolButton').isEnabled())
        self.assertTrue(form.findChild(QPushButton, 'addStarterPoolButton').isEnabled())
        self.assertFalse(c['pool_draws'].isEnabled())
        # They are the disc's, so nothing of them is written to the mod.
        self.assertNotIn('starter_pools', w.project.other)

    def test_adding_a_pool_takes_the_page_off_the_disc_s_rows(self):
        from fm_editor import starter_pools
        w = self.window
        c = self.starter_page()
        w.starter_pools_retail = [starter_pools.Pool(name='Row', draws=40, cards={1: 2048})]
        w._refresh_starter_pools()
        self.assertFalse(w._starter_pools_editable())
        w._add_starter_pool()
        self.assertTrue(w._starter_pools_editable())
        self.assertEqual(c['pools'].rowCount(), 1)
        self.assertIn('starter_pools', w.project.other)

    # --- what a review found: ids, files and the duelists' own ---------------

    def test_an_id_the_mod_wrote_is_not_reslugged(self):
        """"Dark_Simon" stayed "Dark_Simon": slugging it renamed the duelist
        and left its roster, deck, drop and portrait files behind."""
        import json
        w = self.window
        w.project.files['duelists/Dark_Simon.json'] = json.dumps(
            {'copy': 1, 'name': 'Dark Simon'}).encode()
        w._duelist_entries_cache = None
        w.select_workspace('Duelists')
        c = w.workspace_controls['Duelists']
        slot = next(s for s, r in w.duelist_slots.items()
                    if (r.get('entry') or {}).get('id') == 'Dark_Simon')
        w.duelist_selected_slot = slot
        c['name'].setText('Dark Simon the Second')
        c['id'].setText('Dark_Simon')
        c['base'].setCurrentIndex(1)
        w._save_duelist()
        entries = w._duelist_roster_source()[0]
        self.assertEqual([e.get('id') for e in entries], ['Dark_Simon'])
        self.assertEqual(sorted(k for k in w.project.files if k.startswith('duelists/')),
                         ['duelists/Dark_Simon.json'])

    def test_a_replace_is_matched_however_it_names_its_duelist(self):
        """By number or by name in any case: matching the name alone wrote a
        second entry for the same duelist."""
        w = self.window
        self.assertTrue(w._replaces_slot({'replace': 2}, 2))
        self.assertTrue(w._replaces_slot({'replace': '2'}, 2))
        self.assertTrue(w._replaces_slot({'replace': DUELIST_NAMES[2].upper()}, 2))
        self.assertTrue(w._replaces_slot({'replace': DUELIST_NAMES[2].lower()}, 2))
        self.assertFalse(w._replaces_slot({'replace': 3}, 2))
        self.assertFalse(w._replaces_slot({'copy': 2}, 2))
        self.assertFalse(w._replaces_slot({'replace': True}, 2))

    def test_removing_a_duelist_does_not_raise(self):
        from PySide6.QtWidgets import QMessageBox
        w = self.window
        w.select_workspace('Duelists')
        c = w.workspace_controls['Duelists']
        c['name'].setText('Tester')
        c['base'].setCurrentIndex(1)
        w._save_duelist(adding=True)
        self.assertIn('tester', [e.get('id') for e in w._duelist_roster_source()[0]])
        with mock.patch.object(QMessageBox, 'question',
                               return_value=QMessageBox.StandardButton.Yes):
            w._remove_duelist()
        self.assertNotIn('tester', [e.get('id') for e in w._duelist_roster_source()[0]])

    # --- the Art page's three renders ---------------------------------------

    def test_the_art_page_shows_both_renders_at_once(self):
        """Disc, the console's and Internal side by side, as the Tk window
        has them, rather than one scale at a time behind a button."""
        from PySide6.QtWidgets import QPushButton
        from fm_editor.qt.common import ART_INTERNAL
        w = self.window
        w.select_workspace('Art')
        page = w.art_previews['art']['disc'].window()
        for part, stem in (('art', 'picture'), ('thumbnail', 'thumbnail'), ('title', 'title')):
            panes = set(w.art_previews[part])
            self.assertEqual(panes, {'disc', 'game', 'mod'} | ({'internal'} if part in ART_INTERNAL else set()),
                             part)
            # The scale buttons are gone with it.
            for scale in (1, 2, 4):
                self.assertIsNone(page.findChild(QPushButton, f'{stem}Scale{scale}Button'),
                                  f'{stem} {scale}x')
        self.assertFalse(hasattr(w, 'art_scales'))

    def test_the_internal_pane_draws_what_the_console_averages_away(self):
        from fm_editor.qt.common import ART_INTERNAL
        from fm_editor import art
        w = self.window
        w.select_workspace('Art')
        cid = sorted(w.project.cards)[0]
        w.show_art_card(cid)
        for part, factor in ART_INTERNAL.items():
            console = art.in_game(w.project, w.files.wa, cid, part, 1)
            internal = art.in_game(w.project, w.files.wa, cid, part, factor)
            self.assertEqual(internal.width, console.width * factor, part)
            self.assertFalse(w.art_previews[part]['internal'].pixmap().isNull(), part)
            self.assertIn(f'Internal {factor}', w.art_info[part]['internal'].text())
            self.assertIn("console's render", w.art_info[part]['game'].text())

    def test_opening_a_mod_with_packs_shows_its_first_pack(self):
        """min(-1, 0) left the list with a row and nothing chosen, so every
        field of a mod that plainly has a pack read as empty."""
        w = self.window
        c = self.packs_page()
        w.project.packs.append({'name': 'Dragons', 'cards': {'1': 100}})
        c['pack_index'] = -1                      # as a freshly built page has it
        w._refresh_packs()
        self.assertEqual(c['pack_index'], 0)
        self.assertEqual(c['list'].currentRow(), 0)
        self.assertEqual(c['name'].text(), 'Dragons')

    def test_a_cover_on_disk_is_drawn_without_being_in_project_files(self):
        """A mod opened from a folder keeps its PNG there, not in memory."""
        import tempfile
        from pathlib import Path
        w = self.window
        c = self.packs_page()
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'packs').mkdir()
            Path(folder, 'packs/cover.png').write_bytes(
                Path(self.pack_png()).read_bytes())
            w.project.source_dir = Path(folder)
            w.project.packs.append({'name': 'From disk', 'cards': {'1': 100},
                                    'image': 'packs/cover.png'})
            w._refresh_packs()
            self.assertNotIn('packs/cover.png', w.project.files)
            self.assertIsNotNone(w._pack_image_bytes(w._pack_entry()))
            self.assertFalse(c['list'].item(0).icon().isNull())
            self.assertFalse(c['image'].pixmap().isNull())

    def test_the_list_follows_the_name_as_it_is_typed(self):
        """There is no Apply button to make it catch up."""
        w = self.window
        c = self.packs_page()
        w._add_pack()
        self.assertIn('Pack 1', c['list'].item(0).text())
        c['name'].setText('Dragons!')
        self.assertIn('Dragons!', c['list'].item(0).text().splitlines()[0])
        c['name'].clear()
        self.assertIn('(unnamed)', c['list'].item(0).text().splitlines()[0])
