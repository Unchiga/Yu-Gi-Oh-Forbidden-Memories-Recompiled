"""model_oracle.py's own logic on made-up games (CTest pc_model_oracle_check):
no game and no disc needed.

The record-to-card mapping must follow model.h's gaps as the loader's
inverse (test_mod_models.py, disc_record) does; a game's files must split
into its battles by frame; and the comparison must name the first frame two
battles differ at, a battle that ran longer, and calls that differ, while
the coverage counts an image only when its slot held the disc's bytes,
hashed before its module ran, and its module was called there on both
sides."""
from array import array
import hashlib
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/pc"))
import model_oracle as oracle  # noqa: E402

failures = []


def check(what, ok, detail=""):
    print(f"{'ok' if ok else 'FAILED'}: {what}" + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        failures.append(what)


def disc_record(card):
    """test_mod_models.py's card -> record, the loader's bias written out."""
    record = card - 1
    return record - ((record >= 721) + 50 * (record >= 700) + 50 * (record >= 350))


def mapping():
    check("621 records", oracle.RECORDS == 621, oracle.RECORDS)
    check("each record's card maps back to it",
          all(disc_record(oracle.record_card(r)) == r for r in range(oracle.RECORDS)))
    check("the gaps' neighbours", [oracle.record_card(r) for r in (0, 299, 300, 599, 600, 619, 620)] ==
          [1, 300, 351, 650, 701, 720, 722])
    check("records parsed", oracle.parse_records("34, 3-5,34") == [34, 3, 4, 5])
    check("all records by default", oracle.parse_records(None) == list(range(621)))
    try:
        oracle.parse_records("620-621")
        check("a record past the last refused", False)
    except ValueError:
        check("a record past the last refused", True)
    data = oracle.Battle(300, 1, "defender").properties()
    check("D_800EF658's two records", data == bytes.fromhex("5f01 0000 0000 00 01 5f01 0100 0000 00 01"), data.hex())


def write_game(folder, hashes, log):
    folder.mkdir(parents=True)
    (folder / "hashes.txt").write_text("".join(f"{frame} {value:016x}\n" for frame, value in hashes))
    (folder / "game.log").write_text("\n".join(log) + "\n")


def line(frame, kind, command, module=None, slot="A"):
    text = f"[t 1 us frame {frame} vb {2 * frame} model] {kind} slot={'AB'.index(slot)} command={command}"
    return text + (f" module {module} in {slot}: interpreter" if module is not None else "")


def traces(work):
    folder = work / "game"
    write_game(folder, [(f, 0x100 + f) for f in range(0, 30)], [
        "memories-pc: control: listening",
        line(3, "variant", 1, 418, "A"), line(4, "variant", -1, 418, "A"), line(5, "variant", -1, 418, "A"),
        line(6, "variant", 0, 559, "B"),
        "memories-pc: model variant module 559 (slot B) cannot run; using the stand-in",
        line(16, "variant", 0), line(17, "variant", -1)])   # an older build: no module, no slot
    first = {"start": 2, "end": 10}
    second = {"start": 15, "end": 20}
    oracle.read_traces(folder, [first, second])
    check("a battle's hashes are the frames after its start to its end",
          list(first["hashes"]) == [0x100 + f for f in range(3, 11)], list(first["hashes"]))
    check("its setup calls, by offset", first["calls"] == [(1, "variant", "A", 418, 1), (4, "variant", "B", 559, 0)],
          first["calls"])
    check("its updates counted", first["updates"] == {"variant A": 2}, first["updates"])
    check("a module that cannot run, with its battle", first["cannot_run"] == ["variant 559 in B"]
          and second["cannot_run"] == [], (first["cannot_run"], second["cannot_run"]))
    check("an older build's line: the active slot, no module", second["calls"] == [(1, "variant", "A", None, 0)],
          second["calls"])
    return first


def comparing(first):
    same = dict(first, side="subject", battle="0/0/tie", start=2)
    other = dict(same, side="reference")
    check("the same battle twice is identical", oracle.compare(same, other)["status"] == "identical")
    changed = dict(other, hashes=array("Q", list(first["hashes"])[:5] + [1, 2, 3]))
    verdict = oracle.compare(same, changed)
    check("the first differing frame", verdict["status"] == "differs" and verdict["first_differing_frame"] == 6,
          verdict)
    longer = dict(other, hashes=array("Q", list(first["hashes"]) + [9]))
    check("a battle that ran on differs past the shorter one's end",
          oracle.compare(same, longer).get("first_differing_frame") == len(first["hashes"]) + 1)
    calls = dict(other, calls=[(1, "variant", "A", 418, 2)] + first["calls"][1:])
    verdict = oracle.compare(same, calls)
    check("calls that differ with the same pictures", verdict["status"] == "differs"
          and "first_differing_frame" not in verdict and "call 1" in verdict["why"], verdict)
    check("an error is neither", oracle.compare(same, dict(other, error="GameHung: no reply"))["status"] == "error")
    idle = dict(other, calls=[], updates={})
    verdict = oracle.compare(dict(same, calls=[], updates={}), idle)
    check("two runs where no module ran are an error, not a match",
          verdict["status"] == "error" and "slot A" in verdict["why"], verdict)
    only_a = [call for call in first["calls"] if call[2] == "A"]
    check("a tie without slot B's module is an error",
          oracle.compare(dict(same, calls=only_a), dict(other, calls=only_a))["status"] == "error")
    check("an attacker's win runs slot A's module only",
          oracle.compare(dict(same, calls=only_a, battle="0/0/attacker"),
                         dict(other, calls=only_a, battle="0/0/attacker"))["status"] == "identical")
    check("a battle without its frame hashes is an error, not a match",
          oracle.compare(dict(same, hashes=array("Q")), dict(other, hashes=array("Q")))["status"] == "error" and
          oracle.compare(same, dict(other, hashes=array("Q", [0] * len(first["hashes"]))))["status"] == "error")


class FakeDisc:
    battle_variant, battle_primary = oracle.Disc.battle_variant, oracle.Disc.battle_primary

    def variant(self, record, alternative, slot):
        return f"{record % 2} {alternative} {slot}".encode()   # records 0 and 2 share their images

    def primary(self, record, slot):
        return b"primary"


def covering():
    disc = FakeDisc()
    battles = [oracle.Battle(r, a, "tie") for r in (0, 1, 2) for a in (0, 1)]

    def ran(battle, slots="AB", images=None):
        return {"side": "x", "images": images or {s: oracle.sha(disc.variant(battle.record, battle.alternative, s))
                                                   for s in "AB"},
                "primary_images": {s: "" for s in "AB"}, "captured": {"A": 0, "B": 0},
                "calls": [(1, "variant", s, 1, 0) for s in slots]}

    subject = {b.key: ran(b) for b in battles}
    reference = dict(subject)
    report = oracle.coverage(battles, subject, reference, disc)
    check("distinct images counted once", report["variant"]["A"]["distinct"] == 4 and
          report["variant"]["A"]["covered"] == 4, report["variant"])
    only_a = oracle.Battle(1, 1, "tie")
    reference = dict(subject, **{only_a.key: ran(only_a, "A")})
    report = oracle.coverage(battles, subject, reference, disc)
    check("an image whose module was not called in one side is missing",
          report["variant"]["B"]["missing"] == ["record 1 stage 10"] and report["variant"]["A"]["missing"] == [],
          report["variant"])
    wrong = ran(only_a, images={"A": hashlib.sha256(b"other").hexdigest(),
                                "B": oracle.sha(disc.variant(1, 1, "B"))})
    report = oracle.coverage(battles, dict(subject, **{only_a.key: wrong}), subject, disc)
    check("a slot that held other bytes is named and not covered",
          report["mismatched"] == ["1/1/tie slot A (x): not the disc's stage 9 image"] and
          report["variant"]["A"]["missing"] == ["record 1 stage 9"], report)
    attacks = [oracle.Battle(r, a, "attacker") for r in (0, 1, 2) for a in (0, 1)]
    won = {b.key: ran(b, "A") for b in attacks}
    report = oracle.coverage(attacks, won, won, disc)
    check("attacker-only battles ask for no slot B image", report["variant"]["B"] ==
          {"distinct": 0, "covered": 0, "missing": []} and report["variant"]["A"]["covered"] == 4, report["variant"])
    late = dict(ran(only_a), captured={"A": 0, "B": 1})
    report = oracle.coverage(battles, dict(subject, **{only_a.key: late}), subject, disc)
    check("an image hashed only after its module ran does not count (a module may write into it)",
          report["mismatched"] == ["1/1/tie slot B (x): its image was not hashed before its module ran"] and
          report["variant"]["B"]["missing"] == ["record 1 stage 10"], report)


with tempfile.TemporaryDirectory() as temporary:
    mapping()
    comparing(traces(Path(temporary)))
    covering()
print(f"model oracle check: {'FAILED ' + ', '.join(failures) if failures else 'all passed'}")
raise SystemExit(1 if failures else 0)
