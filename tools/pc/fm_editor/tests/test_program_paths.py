"""Where the editor looks beside its program, and the port's user directory,
on each platform (disc.program_dir, disc.user_dir, disc.candidates), and the
mods folders the game reads (validate.mod_folders, chosen_folder)."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fm_editor import disc, validate

APP = "/Users/p/Games/fm-editor.app/Contents/MacOS/fm-editor"


class ProgramDir(unittest.TestCase):
    def run_as(self, platform, executable):
        patches = [mock.patch.object(sys, "platform", platform), mock.patch.object(sys, "executable", executable),
                   mock.patch.object(sys, "frozen", True, create=True)]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        return disc.program_dir()

    def test_mac_app_bundle_is_the_folder_holding_the_app(self):
        self.assertEqual(self.run_as("darwin", APP), Path(APP).resolve().parents[3])

    def test_mac_frozen_outside_a_bundle_is_the_executable_folder(self):
        self.assertEqual(self.run_as("darwin", "/opt/fm/fm-editor"), Path("/opt/fm").resolve())

    def test_mac_bundle_shape_needs_the_app_suffix(self):
        self.assertEqual(self.run_as("darwin", "/x/Foo/Contents/MacOS/fm-editor"), Path("/x/Foo/Contents/MacOS").resolve())

    def test_windows_and_linux_frozen_are_the_executable_folder(self):
        self.assertEqual(self.run_as("win32", "C:/g/fm-editor.exe"), Path("C:/g/fm-editor.exe").resolve().parent)
        self.assertEqual(self.run_as("linux", "/g/fm-editor"), Path("/g/fm-editor").resolve().parent)

    def test_linux_even_with_bundle_shaped_path_is_unchanged(self):
        self.assertEqual(self.run_as("linux", APP), Path(APP).resolve().parent)

    def test_source_run_is_the_script_folder(self):
        for platform in ("darwin", "linux", "win32"):
            with mock.patch.object(sys, "platform", platform), mock.patch.object(sys, "argv", ["/src/fm_editor/__main__.py"]):
                self.assertEqual(disc.program_dir(), Path("/src/fm_editor/__main__.py").resolve().parent)

    def test_candidates_find_game_beside_the_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            beside = Path(tmp).resolve()
            executable = str(beside / "fm-editor.app" / "Contents" / "MacOS" / "fm-editor")
            env = {"HOME": tmp, "MEMORIES_USER_DIR": str(beside / "user")}
            with mock.patch.dict(os.environ, env), mock.patch.object(sys, "platform", "darwin"), \
                    mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "executable", executable):
                found = disc.candidates()
        self.assertIn(beside / "game", found)
        self.assertIn(beside, found)
        self.assertNotIn(Path(executable).parent / "game", found)

    def test_mods_are_found_beside_the_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            beside = Path(tmp).resolve()
            (beside / "mods" / "one").mkdir(parents=True)
            (beside / "mods" / "one" / "mod.json").write_text("{}")
            executable = str(beside / "fm-editor.app" / "Contents" / "MacOS" / "fm-editor")
            env = {"HOME": tmp, "MEMORIES_USER_DIR": str(beside / "user")}
            with mock.patch.dict(os.environ, env), mock.patch.object(sys, "platform", "darwin"), \
                    mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "executable", executable):
                self.assertEqual(validate.shipped_folder(), beside / "mods")


class UserDir(unittest.TestCase):
    def setUp(self):
        saved = dict(os.environ)
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(saved)))
        for name in ("MEMORIES_USER_DIR", "XDG_DATA_HOME"):
            os.environ.pop(name, None)
        os.environ["HOME"] = "/home/p"
        os.environ["USERPROFILE"] = "C:/Users/p"

    def where(self, platform):
        with mock.patch.object(sys, "platform", platform), \
                mock.patch.object(disc.os.path, "expanduser", lambda p: os.environ["HOME"] if p == "~" else p):
            return disc.user_dir()

    def test_named_directory_wins_everywhere(self):
        os.environ["MEMORIES_USER_DIR"] = "/named"
        for platform in ("darwin", "linux", "win32"):
            self.assertEqual(self.where(platform), Path("/named"))

    def test_darwin_is_application_support(self):
        self.assertEqual(self.where("darwin"), Path("/home/p/Library/Application Support/YFM Re-Decomp"))

    def test_darwin_ignores_xdg(self):
        os.environ["XDG_DATA_HOME"] = "/xdg"
        self.assertEqual(self.where("darwin"), Path("/home/p/Library/Application Support/YFM Re-Decomp"))

    def test_linux_is_local_share_or_absolute_xdg(self):
        self.assertEqual(self.where("linux"), Path("/home/p/.local/share/YFM Re-Decomp"))
        os.environ["XDG_DATA_HOME"] = "/xdg"
        self.assertEqual(self.where("linux"), Path("/xdg/YFM Re-Decomp"))
        os.environ["XDG_DATA_HOME"] = "relative"
        self.assertEqual(self.where("linux"), Path("/home/p/.local/share/YFM Re-Decomp"))

    def test_windows_is_documents_my_games(self):
        self.assertEqual(self.where("win32").parts[-2:], ("My Games", "YFM Re-Decomp"))


class ChosenFolder(unittest.TestCase):
    """validate.chosen_folder and mod_folders: the player's mods folder, which
    the game reads between the release's mods/ and the user directory's
    (mods.c Mods_Folder, notes/modding.md "Your own mods folder")."""

    def setUp(self):
        saved = dict(os.environ)
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(saved)))
        for name in ("MEMORIES_MODS_FOLDER", "MEMORIES_MODS_DIR"):
            os.environ.pop(name, None)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.user = self.root / "user"
        self.user.mkdir()
        os.environ["MEMORIES_USER_DIR"] = str(self.user)
        self.program = self.root / "program"
        self.program.mkdir()
        patch = mock.patch.object(disc, "program_dir", lambda: self.program)
        patch.start()
        self.addCleanup(patch.stop)

    def sidecar(self, data):
        (self.user / "mods-folder.txt").write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))

    def on(self, platform):
        patch = mock.patch.object(sys, "platform", platform)
        patch.start()
        self.addCleanup(patch.stop)

    def mod(self, folder, name, mod_id=None):
        (folder / name).mkdir(parents=True, exist_ok=True)
        (folder / name / "mod.json").write_text('{"id": "%s", "name": "%s"}' % (mod_id or name, name), encoding="utf-8")

    def test_none_without_file_or_variable(self):
        self.assertIsNone(validate.chosen_folder())

    def test_sidecar_names_it(self):
        self.sidecar("/packs/mine\n")
        self.assertEqual(validate.chosen_folder(), Path("/packs/mine"))

    def test_first_line_that_says_anything(self):
        self.sidecar("\n  \r\n\t\n/packs/first\n/packs/second\n")
        self.assertEqual(validate.chosen_folder(), Path("/packs/first"))

    def test_bom_whitespace_and_crlf_are_dropped(self):
        self.sidecar(b"\xef\xbb\xbf  \t/packs/mine \t\r\n/other\r\n")
        self.assertEqual(validate.chosen_folder(), Path("/packs/mine"))

    def test_quotes_are_dropped_with_the_space_inside_them(self):
        self.sidecar('"/packs/with space "  \r\n')
        self.assertEqual(validate.chosen_folder(), Path("/packs/with space"))

    def test_one_quote_is_part_of_the_name(self):
        self.sidecar('"/packs/mine')
        self.assertIn('"', str(validate.chosen_folder()))

    def test_nothing_but_quotes_or_space_is_none(self):
        for text in ("", "   \r\n", '""', '" "\n'):
            self.sidecar(text)
            self.assertIsNone(validate.chosen_folder(), repr(text))

    def test_trailing_separators_are_dropped_but_a_root_stays(self):
        self.on("win32")
        self.sidecar("D:\\Mods\\Pack\\\\\n")
        self.assertEqual(validate.chosen_folder(), Path("D:/Mods/Pack"))
        self.sidecar("D:\\\n")
        self.assertEqual(str(validate.chosen_folder()).replace("\\", "/"), "D:/")

    def test_backslash_is_a_separator_on_windows_only(self):
        self.on("win32")
        self.sidecar('"C:\\Users\\p\\My Mods\\"')
        self.assertEqual(validate.chosen_folder(), Path("C:/Users/p/My Mods"))
        self.on("linux")
        self.sidecar("/a\\b\n")
        self.assertEqual(validate.chosen_folder(), Path("/a\\b"))

    def test_relative_path_is_from_the_program_directory(self):
        self.sidecar("mods-extra\n")
        self.assertEqual(validate.chosen_folder(), self.program / "mods-extra")
        self.on("win32")
        self.sidecar("..\\sibling/\n")
        self.assertEqual(validate.chosen_folder(), self.program / ".." / "sibling")

    def test_a_drive_letter_is_absolute_on_windows_only(self):
        self.sidecar("C:/Mods\n")
        self.on("win32")
        self.assertEqual(validate.chosen_folder(), Path("C:/Mods"))
        self.on("linux")
        self.assertIn("program", str(validate.chosen_folder()))   # a file name with a colon, under the program

    def test_a_name_too_long_for_the_game_is_none(self):
        self.sidecar("/" + "a" * 1023)
        self.assertIsNone(validate.chosen_folder())
        self.sidecar("/" + "a" * 1021)   # the game's longest line: fgets keeps 1023 bytes
        self.assertIsNotNone(validate.chosen_folder())
        os.environ["MEMORIES_MODS_FOLDER"] = "/" + "a" * 1023
        self.assertIsNone(validate.chosen_folder())

    def test_variable_wins_over_the_file(self):
        self.sidecar("/from/file")
        os.environ["MEMORIES_MODS_FOLDER"] = '  "/from/env/"  '
        self.assertEqual(validate.chosen_folder(), Path("/from/env"))

    def test_variable_set_to_nothing_says_there_is_none(self):
        self.sidecar("/from/file")
        for empty in ("", "  ", '""'):
            os.environ["MEMORIES_MODS_FOLDER"] = empty
            self.assertIsNone(validate.chosen_folder(), repr(empty))

    def test_variable_relative_is_from_the_program_directory(self):
        os.environ["MEMORIES_MODS_FOLDER"] = "extra"
        self.assertEqual(validate.chosen_folder(), self.program / "extra")

    def test_order_is_shipped_then_chosen_then_user(self):
        shipped, chosen = self.root / "shipped", self.root / "pack"
        self.sidecar(str(chosen))
        with mock.patch.object(validate, "shipped_folder", lambda: shipped):
            self.assertEqual(validate.mod_folders(), [shipped, chosen, self.user / "mods"])

    def test_no_chosen_folder_leaves_the_old_order(self):
        shipped = self.root / "shipped"
        with mock.patch.object(validate, "shipped_folder", lambda: shipped):
            self.assertEqual(validate.mod_folders(), [shipped, self.user / "mods"])
        with mock.patch.object(validate, "shipped_folder", lambda: None):
            self.assertEqual(validate.mod_folders(), [self.user / "mods"])

    def test_chosen_without_shipped(self):
        chosen = self.root / "pack"
        os.environ["MEMORIES_MODS_FOLDER"] = str(chosen)
        with mock.patch.object(validate, "shipped_folder", lambda: None):
            self.assertEqual(validate.mod_folders(), [chosen, self.user / "mods"])

    def test_mods_dir_is_the_only_folder_and_wins(self):
        self.sidecar(str(self.root / "pack"))
        os.environ["MEMORIES_MODS_FOLDER"] = str(self.root / "pack")
        os.environ["MEMORIES_MODS_DIR"] = str(self.root / "only")
        with mock.patch.object(validate, "shipped_folder", lambda: self.root / "shipped"):
            self.assertEqual(validate.mod_folders(), [self.root / "only"])

    def test_empty_mods_dir_is_not_set(self):
        os.environ["MEMORIES_MODS_DIR"] = ""
        os.environ["MEMORIES_MODS_FOLDER"] = str(self.root / "pack")
        with mock.patch.object(validate, "shipped_folder", lambda: None):
            self.assertEqual(validate.mod_folders(), [self.root / "pack", self.user / "mods"])

    def test_the_games_own_folders_are_left_out_as_the_game_does(self):
        shipped = self.program / "mods"
        for own in (shipped, shipped / "inner", self.user, self.user / "mods", self.user / "downloads"):
            os.environ["MEMORIES_MODS_FOLDER"] = str(own)
            with mock.patch.object(validate, "shipped_folder", lambda: shipped):
                self.assertEqual(validate.mod_folders(), [shipped, self.user / "mods"], str(own))

    def test_the_mod_being_edited_is_still_added_last(self):
        chosen, elsewhere = self.root / "pack", self.root / "elsewhere"
        elsewhere.mkdir()
        os.environ["MEMORIES_MODS_FOLDER"] = str(chosen)
        with mock.patch.object(validate, "shipped_folder", lambda: None):
            project = mock.Mock(source_dir=elsewhere / "mine")
            self.assertEqual(validate.mod_folders(project), [chosen, self.user / "mods", elsewhere])
            project = mock.Mock(source_dir=chosen / "mine")
            self.assertEqual(validate.mod_folders(project), [chosen, self.user / "mods"])

    def test_chosen_mods_are_installed_and_the_user_folder_wins_an_id(self):
        from fm_editor import overlaps
        shipped, chosen = self.root / "shipped", self.root / "pack"
        self.mod(shipped, "base", "same")
        self.mod(chosen, "pack-only")
        self.mod(chosen, "dup", "same")
        self.mod(self.user / "mods", "dup", "same")
        os.environ["MEMORIES_MODS_FOLDER"] = str(chosen)
        with mock.patch.object(validate, "shipped_folder", lambda: shipped):
            found = {m.id: Path(m.directory) for m in overlaps.installed(validate.mod_folders())}
            self.assertEqual(found["pack-only"], chosen / "pack-only")
            self.assertEqual(found["same"], self.user / "mods" / "dup")
            os.environ["MEMORIES_MODS_FOLDER"] = ""
            self.assertNotIn("pack-only", {m.id for m in overlaps.installed(validate.mod_folders())})


if __name__ == "__main__":
    unittest.main()
