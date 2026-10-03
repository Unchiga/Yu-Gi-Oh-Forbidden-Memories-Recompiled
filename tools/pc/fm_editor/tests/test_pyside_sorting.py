"""Clicking a column header sorts the list under it, on every page that has one."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import unittest
from unittest import mock
from types import SimpleNamespace
try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication, QTableWidget
    from fm_editor.pyside_app import ModernEditor
    from fm_editor.qt.common import UNSORTED_TABLES, TableItem, _sort_number
except ImportError:
    QApplication = None
from fm_editor.tests.test_data import fixture


@unittest.skipIf(QApplication is None, 'PySide6 is not installed')
class SortingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qt = QApplication.instance() or QApplication([])

    def setUp(self):
        f = fixture()
        with mock.patch.object(ModernEditor, '_load_game',
                               return_value=SimpleNamespace(wa=f.wa, source='synthetic')), \
             mock.patch('fm_editor.pyside_app.gamedata.load_game', return_value=f.game()), \
             mock.patch.object(ModernEditor, '_render_preview'):
            self.window = ModernEditor()
        self.render = mock.patch.object(self.window, '_render_preview')
        self.render.start()
        self.addCleanup(self.render.stop)
        self.addCleanup(self.window.deleteLater)

    def tables(self):
        """Every table in every page, named."""
        for index in range(self.window.pages.count()):
            for table in self.window.pages.widget(index).findChildren(QTableWidget):
                yield table.objectName() or '(unnamed)', table

    @staticmethod
    def column(table, index):
        return [table.item(row, index).text() for row in range(table.rowCount())
                if table.item(row, index) is not None]

    @staticmethod
    def click(table, column, order=Qt.SortOrder.AscendingOrder):
        """Sort as a click on the column header does."""
        table.sortByColumn(column, order)

    def test_every_list_sorts_by_the_column_you_click(self):
        found = [name for name, table in self.tables() if name not in UNSORTED_TABLES]
        self.assertGreater(len(found), 10, found)
        for name, table in self.tables():
            if name in UNSORTED_TABLES:
                continue
            with self.subTest(table=name):
                self.assertTrue(table.isSortingEnabled())
                self.assertTrue(table.horizontalHeader().sectionsClickable())
                self.assertTrue(table.horizontalHeader().isSortIndicatorShown())

    def test_a_grid_or_a_matrix_is_not_a_list_and_does_not_sort(self):
        held = [name for name, table in self.tables() if name in UNSORTED_TABLES]
        self.assertIn('duelistTable', held)        # the grid of portraits
        self.assertIn('matrixTable', held)         # named on both axes
        self.assertIn('duelistLpTable', held)      # its cells hold live spin boxes
        for name, table in self.tables():
            if name in UNSORTED_TABLES:
                with self.subTest(table=name):
                    self.assertFalse(table.isSortingEnabled())

    def test_the_drop_list_sorts_by_name_and_by_weight(self):
        w = self.window
        w.select_workspace('Duelists')
        drops = w.workspace_controls['Duelists']['table']
        self.assertTrue(self.column(drops, 1), 'the duelist has no drop list to sort')
        self.click(drops, 1)
        names = self.column(drops, 1)
        self.assertEqual(names, sorted(names, key=str.casefold))
        self.click(drops, 3, Qt.SortOrder.DescendingOrder)
        weights = [int(text) for text in self.column(drops, 3)]
        self.assertEqual(weights, sorted(weights, reverse=True))

    def test_a_number_column_sorts_as_numbers_rather_than_as_text(self):
        w = self.window
        w.project.cards[1].attack = 900
        w.project.cards[2].attack = 1500
        w.project.cards[3].attack = 80
        w.refresh_cards()
        self.click(w.listing, 3, Qt.SortOrder.AscendingOrder)
        attacks = [int(text) for text in self.column(w.listing, 3)]
        self.assertEqual(attacks, sorted(attacks))
        # The text sort this replaces would have put 1500 before 900 and 80.
        self.assertLess(attacks.index(900), attacks.index(1500))

    def test_a_refill_keeps_the_sort_and_every_row_keeps_its_own_cells(self):
        w = self.window
        w.select_workspace('Duelists')
        drops = w.workspace_controls['Duelists']['table']
        self.click(drops, 3, Qt.SortOrder.DescendingOrder)
        pairs = dict(zip(self.column(drops, 0), self.column(drops, 1)))
        self.assertTrue(pairs)
        w._refresh_duelist_pool()
        weights = [int(text) for text in self.column(drops, 3)]
        self.assertEqual(weights, sorted(weights, reverse=True))
        self.assertEqual(dict(zip(self.column(drops, 0), self.column(drops, 1))), pairs)

    def test_the_line_a_sorted_list_selects_is_the_one_it_was_on(self):
        w = self.window
        w.select_workspace('Rituals')
        cards = w.workspace_controls['Rituals']['cards']
        if cards.rowCount() < 2:
            self.skipTest('the fixture has no rituals to sort')
        chosen = cards.item(1, 0).data(Qt.ItemDataRole.UserRole)
        w.ritual_current = chosen
        self.click(cards, 1, Qt.SortOrder.DescendingOrder)
        w._refresh_rituals()
        self.assertEqual(w.ritual_current, chosen)
        row = cards.currentRow()
        self.assertEqual(cards.item(row, 0).data(Qt.ItemDataRole.UserRole), chosen)

    def test_a_search_still_hides_the_rows_it_hid_after_a_sort(self):
        w = self.window
        w.select_workspace('Equips')
        controls = w.workspace_controls['Equips']
        table = controls['equips']
        if table.rowCount() < 2:
            self.skipTest('the fixture has no equip cards to sort')
        wanted = table.item(table.rowCount() - 1, 1).text()
        controls['search'].setText(wanted)
        shown = {row for row in range(table.rowCount()) if not table.isRowHidden(row)}
        self.assertTrue(shown)
        self.click(table, 1, Qt.SortOrder.DescendingOrder)
        self.qt.processEvents()        # the filter runs a turn after the sort
        for row in range(table.rowCount()):
            hidden = table.isRowHidden(row)
            matches = wanted.casefold() in table.item(row, 1).text().casefold()
            self.assertEqual(hidden, not matches, table.item(row, 1).text())

    def test_a_problem_opens_the_line_it_is_about_and_not_the_row_it_sits_on(self):
        w = self.window
        # Named after the page is up: leaving the card form behind writes the
        # card it was showing back, name and all.
        w.select_workspace('Problems')
        w.project.cards[1].name = ''        # two problems, a row each
        w.project.cards[2].name = ''
        w._refresh_problems()
        table = w.workspace_controls['Problems']['table']
        issues = w.workspace_controls['Problems']['issues']
        self.assertEqual(len(issues), 2)
        self.click(table, 1, Qt.SortOrder.DescendingOrder)
        self.assertEqual(table.item(0, 1).text().strip(), 'Cards · 2')     # the sort moved it up
        with mock.patch.object(ModernEditor, 'go_to') as went:
            w._open_problem(0)
        index = table.item(0, 0).data(Qt.ItemDataRole.UserRole)
        went.assert_called_once_with(issues[index])


@unittest.skipIf(QApplication is None, 'PySide6 is not installed')
class TableItemTest(unittest.TestCase):
    def test_what_reads_as_a_number_sorts_as_one(self):
        for text, number in (('1500', 1500.0), ('003', 3.0), ('12.50%', 12.5),
                             ('+500', 500.0), ('-200', -200.0), (' 7 ', 7.0)):
            self.assertEqual(_sort_number(text), number, text)
        for text in ('', 'Blue-eyes', '—', '1 / 40', 'Stock'):
            self.assertIsNone(_sort_number(text), text)

    def test_numbers_before_text_and_ticks_before_blanks(self):
        self.assertLess(TableItem('900'), TableItem('1500'))
        self.assertLess(TableItem('003'), TableItem('012'))
        self.assertLess(TableItem('Ameba'), TableItem('bat'))       # case does not matter
        ticked, blank = TableItem(''), TableItem('')
        ticked.setCheckState(Qt.CheckState.Checked)
        blank.setCheckState(Qt.CheckState.Unchecked)
        self.assertLess(blank, ticked)
        self.assertFalse(ticked < blank)


if __name__ == '__main__':
    unittest.main()
