"""Fusions in bulk: every pair of a set of cards A with a set of cards B.

Each set is chosen by filters that must all hold (card kind, monster type,
attribute, guardian star, ATK, DEF, level, words of the name or the text,
a list of cards, "is a fusion result"). The plan says, before anything
changes, which pairs would be added, which already fuse and are kept or
replaced, and which are left alone; apply() carries it out and returns what
undo() needs to put it back. No Tk here: bulk_dialog.py is the window.

A pair is unordered, as in the game (Duel_CheckFusion sorts the two ids;
the port's rules are kept under the smaller one), so A+B and B+A are one
pair: a pair both sets make twice is counted once, as a duplicate.

"Already fuses" is what the port would do with the rules the project holds
(src/pc/cards/tables.c Tables_Fusion): the pair's own rule, or, for a card
the mod adds, its base's; a rule of 0 forbids the fusion, so that pair
fuses with nothing and bulk add treats it as free.
"""
from __future__ import annotations

import re
from bisect import bisect_right
from dataclasses import dataclass, field, replace as dc_replace

from .gamedata import TYPE_EQUIP, TYPE_MAGIC, TYPE_RITUAL, TYPE_TRAP

KINDS = ("monster", "magic", "trap", "ritual", "equip")

# What a batch may leave the mod holding. The port keeps the rules in an
# array it sorts once (src/pc/cards/fusion_table.c), so the only true limit is the
# pairs there are: n(n+1)/2 for n cards, 261,003 for the disc's 722.
# Measured (a 32-bit Windows build, MEMORIES_TRACE=mods): a mod.json of all
# 261,003 rules (24.3 MB) loads in about 3 s. The budget is that, with room
# for cards a mod adds; past it a batch is refused. The disc's own table
# (64 KB, a count byte per card) does not bind: a mod carries rules, not it.
RULE_BUDGET = 300_000
BYTES_PER_RULE = 93          # a rule as mod.json writes it, cards by name, on average (measured)
SAMPLE = 300                 # the pairs a preview lists


def kind_of(card) -> str:
    if card.type == TYPE_MAGIC:
        return "magic"
    if card.type == TYPE_TRAP:
        return "trap"
    if card.type == TYPE_RITUAL:
        return "ritual"
    if card.type == TYPE_EQUIP:
        return "equip"
    return "monster"


# --- choosing cards ---------------------------------------------------------------

def parse_cards(project, text: str):
    """Cards a list names: numbers, ranges ("10-20") and names, separated by
    commas, semicolons or new lines. (ids in order, words it cannot place)."""
    ids, unknown = [], []
    for token in re.split(r"[,;\n]+", text or ""):
        token = token.strip()
        if not token:
            continue
        span = re.fullmatch(r"(\d+)\s*-\s*(\d+)", token)
        if span:
            low, high = sorted((int(span.group(1)), int(span.group(2))))
            found = [cid for cid in range(low, high + 1) if cid in project.cards]
            if found:
                ids.extend(found)
            else:
                unknown.append(token)
            continue
        cid = int(token) if token.isdigit() and int(token) in project.cards else project.resolve(token)
        if not cid:
            cid = next((c for c, card in project.cards.items() if card.name.lower() == token.lower()), 0)
        if cid:
            ids.append(cid)
        else:
            unknown.append(token)
    return ids, unknown


@dataclass
class CardFilter:
    """The cards of one side. An empty set or None means "any"; every filter
    given must hold."""
    kinds: set = field(default_factory=set)        # KINDS
    types: set = field(default_factory=set)        # monster types 0-19
    attributes: set = field(default_factory=set)   # 0-5
    stars: set = field(default_factory=set)        # guardian stars 1-10: either of the card's two
    atk_min: int = None
    atk_max: int = None
    def_min: int = None
    def_max: int = None
    level_min: int = None
    level_max: int = None
    name: str = ""                                 # the name holds these letters, any case
    text: str = ""                                 # the card text does
    cards: str = ""                                # a list of cards (parse_cards)
    results_only: bool = False                     # only cards some fusion makes

    def empty(self) -> bool:
        """No filter at all (a field of spaces is none)."""
        return (dc_replace(self, name="", text="", cards="") == CardFilter()
                and not (self.name.strip() or self.text.strip() or self.cards.strip()))

    def select(self, project, results=None):
        """(card ids, in order; list words it cannot place)."""
        listed, unknown = parse_cards(project, self.cards) if self.cards.strip() else (None, [])
        pool = sorted(set(listed)) if listed is not None else sorted(project.cards)
        if self.results_only and results is None:
            results = fusion_results(project)
        name, text = self.name.strip().lower(), " ".join(self.text.split()).lower()
        chosen = []
        for cid in pool:
            card = project.cards[cid]
            if self.kinds and kind_of(card) not in self.kinds:
                continue
            if self.types and card.type not in self.types:
                continue
            if self.attributes and (not card.is_monster() or card.attribute not in self.attributes):
                continue
            if self.stars and not ({card.star1, card.star2} & self.stars):
                continue
            if not _within(card.attack, self.atk_min, self.atk_max) or \
                    not _within(card.defense, self.def_min, self.def_max) or \
                    not _within(card.level, self.level_min, self.level_max):
                continue
            if name and name not in card.name.lower():
                continue
            if text and text not in " ".join(card.description.split()).lower():
                continue
            if self.results_only and cid not in results:
                continue
            chosen.append(cid)
        return chosen, unknown


def _within(value, low, high) -> bool:
    return (low is None or value >= low) and (high is None or value <= high)


def fusion_results(project) -> set:
    """The cards some fusion of the project makes."""
    return {r for r in project.fusions.values() if r}


# --- what the port would do -----------------------------------------------------

def effective(project, a: int, b: int):
    """The pair's result as the port finds it (Tables_Fusion): its own rule,
    then, for an added card, a rule naming the other card with its base,
    then both bases'. None: no rule; 0: forbidden."""
    fusions = project.fusions
    pair = project.pair(a, b)
    if pair in fusions:
        return fusions[pair]
    if not project.added or (a not in project.added and b not in project.added):
        return None
    base_a, base_b = project.base_of(a), project.base_of(b)
    # A copy with its partner's base; of two such, the later rule, which in
    # the mod the editor writes (pairs in order) is the pair that sorts last.
    found = [project.pair(*other) for other in ((a, base_b), (base_a, b)) if other != (a, b)]
    found = [pair for pair in found if pair in fusions]
    if found:
        return fusions[max(found)]
    if base_a != a and base_b != b:
        return fusions.get(project.pair(base_a, base_b))
    return None


# --- the plan ------------------------------------------------------------------------

@dataclass
class BulkSpec:
    a: CardFilter = field(default_factory=CardFilter)
    b: CardFilter = field(default_factory=CardFilter)
    mode: str = "add"                    # "add" or "remove"
    result: int = 0                      # add: the card made; remove: only fusions making it (0: any)
    ladder: str = ""                     # add: the weakest of these cards that beats both materials
    stronger: bool = True                # add: only when the result's ATK beats both materials'
    allow_self: bool = False             # a card with itself
    overwrite: bool = False              # add: replace a pair that already fuses


@dataclass
class Plan:
    mode: str = "add"
    changes: list = field(default_factory=list)    # (pair, before, after): after 0/None takes the fusion away
    kept: int = 0            # already fuses, left as it is (add, skip)
    same: int = 0            # already makes that result
    weaker: int = 0          # the result would not beat both materials
    no_result: int = 0       # no card of the ladder beats both materials
    not_fusing: int = 0      # remove: the pair fuses with nothing (or makes another card)
    only_result: int = 0     # remove: only fusions making this card
    self_pairs: int = 0      # a card with itself, not allowed
    duplicates: int = 0      # B+A of a pair A+B already counted
    pairs: int = 0           # distinct pairs looked at
    a_count: int = 0
    b_count: int = 0
    rules_before: int = 0    # the fusion rules the mod writes now
    rules_after: int = 0
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    samples: list = field(default_factory=list)     # (pair, before, after, what) for the preview: changes first
    kept_samples: list = field(default_factory=list)

    @property
    def added(self) -> int:
        return sum(1 for _, before, _ in self.changes if not before) if self.mode == "add" else 0

    @property
    def replaced(self) -> int:
        return sum(1 for _, before, _ in self.changes if before) if self.mode == "add" else 0

    @property
    def removed(self) -> int:
        return len(self.changes) if self.mode == "remove" else 0

    def ok(self) -> bool:
        return not self.errors and bool(self.changes)

    def summary(self) -> str:
        head = f"A: {self.a_count} cards, B: {self.b_count} cards; {self.pairs} pairs"
        if self.mode == "add":
            parts = [f"{self.added} to add", f"{self.replaced} to replace"]
            if self.kept:
                parts.append(f"{self.kept} already fuse (kept)")
        else:
            parts = [f"{self.removed} to take away"]
            if self.not_fusing:
                parts.append(f"{self.not_fusing} do not " + ("make that card" if self.only_result else "fuse"))
        for count, label in ((self.same, "already make that card"), (self.weaker, "result not stronger"),
                             (self.no_result, "no card of the list beats both"),
                             (self.self_pairs, "card with itself"),
                             (self.duplicates, "B+A duplicates counted once")):
            if count:
                parts.append(f"{count} {label}")
        return head + ": " + ", ".join(parts) + "."

    def headline(self) -> str:
        """What applying does, in a few words (the window's big line)."""
        if self.mode == "add":
            return f"{self.added} to add  ·  {self.replaced} to replace"
        return f"{self.removed} to take away"

    def details(self) -> str:
        """The rest of summary(): the pairs looked at and those left alone."""
        parts = [f"{self.pairs} pairs of {self.a_count} × {self.b_count} cards"]
        if self.mode == "add" and self.kept:
            parts.append(f"{self.kept} already fuse (kept)")
        if self.mode == "remove" and self.not_fusing:
            parts.append(f"{self.not_fusing} do not " + ("make that card" if self.only_result else "fuse"))
        for count, label in ((self.same, "already make that card"), (self.weaker, "result not stronger"),
                             (self.no_result, "no card of the list beats both"),
                             (self.self_pairs, "card with itself"),
                             (self.duplicates, "B+A duplicates counted once")):
            if count:
                parts.append(f"{count} {label}")
        return "  ·  ".join(parts)

    def budget_line(self) -> str:
        size = self.rules_after * BYTES_PER_RULE
        size = f"{size / 1e6:.1f} MB" if size >= 1e6 else f"{round(size / 1e3)} KB"
        return (f"The mod's fusion rules: {self.rules_before} now, {self.rules_after} after "
                f"(about {size} of mod.json; at most {RULE_BUDGET}).")


def rule_count(project) -> int:
    """The fusion rules manifest.build_fusions writes."""
    active = project.active_removes()
    removes = project.removed_results()
    fusions = project.fusions
    count = sum(1 for pair, now in fusions.items() if project.fusion_rule(pair, now, removes))
    count += sum(1 for pair in project.retail.fusions if pair not in fusions
                 and project.fusion_rule(pair, None, removes))
    count += sum(1 for pair in project.fusion_explicit if pair not in fusions and pair not in project.retail.fusions)
    return count + len(active) + project.fusion_remove_all + len(project.kept["fusions"])


def _differs(project, pair, value, removes=frozenset(), explicit=None) -> bool:
    """Whether a pair holding `value` (None: no entry) is a rule of the mod,
    with the removes it has now (restoring every recipe of a removed card
    drops its remove; the count leaves that to the next plan)."""
    return project.fusion_rule(pair, value, removes, explicit)


def plan(project, spec: BulkSpec) -> Plan:
    out = Plan(mode=spec.mode, only_result=spec.result if spec.mode == "remove" else 0)
    results = fusion_results(project) if (spec.a.results_only or spec.b.results_only) else None
    side_a, unknown_a = spec.a.select(project, results)
    side_b, unknown_b = spec.b.select(project, results)
    out.a_count, out.b_count = len(side_a), len(side_b)
    for side, unknown in (("A", unknown_a), ("B", unknown_b)):
        if unknown:
            out.errors.append(f"{side}'s list names no card: {', '.join(unknown[:5])}"
                              f"{' ...' if len(unknown) > 5 else ''}")
    out.rules_before = out.rules_after = rule_count(project)
    if spec.a.empty() and spec.b.empty():
        out.errors.append("choose the cards of A and of B with at least one filter")
    ladder = None
    if spec.mode == "add":
        if spec.ladder.strip():
            listed, unknown = parse_cards(project, spec.ladder)
            if unknown:
                out.errors.append(f"the result list names no card: {', '.join(unknown[:5])}")
            ladder = sorted({cid for cid in listed if project.cards[cid].is_monster()},
                            key=lambda cid: (project.cards[cid].attack, cid))
            if len(ladder) < len(set(listed)):
                out.errors.append("a fusion makes a monster: the result list holds a card that is not one")
            if not ladder:
                out.errors.append("the result list names no monster")
        elif not spec.result:
            out.errors.append("choose the card the pairs make")
        elif spec.result not in project.cards:
            out.errors.append(f"no card {spec.result}")
        elif not project.cards[spec.result].is_monster():
            out.errors.append(f"{project.card_label(spec.result)} is not a monster: a fusion makes a monster")
    elif spec.result and (spec.result not in project.cards or not project.cards[spec.result].is_monster()):
        out.errors.append(f"no fusion makes {project.card_label(spec.result)}: it is not a monster card")
    if out.errors:
        return out

    atk = {cid: card.attack for cid, card in project.cards.items()}
    ladder_atk = [atk[cid] for cid in ladder] if ladder else None
    set_a, set_b = set(side_a), set(side_b)
    simple = not project.added
    fusions = project.fusions
    removes = project.removed_results()
    final = {}          # a recipe of a removed card -> what the plan leaves in it
    delta = 0
    for a in side_a:
        for b in side_b:
            if a == b:
                if not spec.allow_self:
                    out.self_pairs += 1
                    continue
            elif b < a and b in set_a and a in set_b:
                out.duplicates += 1     # the same pair as a' = b, b' = a, taken there
                continue
            pair = (a, b) if a <= b else (b, a)
            out.pairs += 1
            before = fusions.get(pair) if simple else effective(project, a, b)
            if spec.mode == "remove":
                if not before or (spec.result and before != spec.result):
                    out.not_fusing += 1
                    continue
                after = None
                what = "remove"
            else:
                strongest = max(atk[a], atk[b])
                if ladder is not None:
                    at = bisect_right(ladder_atk, strongest)
                    if at == len(ladder):
                        out.no_result += 1
                        continue
                    after = ladder[at]
                else:
                    after = spec.result
                    if spec.stronger and atk[after] <= strongest:
                        out.weaker += 1
                        continue
                if before == after:
                    out.same += 1
                    continue
                if before and not spec.overwrite:
                    out.kept += 1
                    if len(out.kept_samples) < SAMPLE:
                        out.kept_samples.append((pair, before, before, "kept"))
                    continue
                what = "replace" if before else "add"
            out.changes.append((pair, before, after))
            if len(out.samples) < SAMPLE:
                out.samples.append((pair, before, after, what))
            # The rule the pair leaves in the mod, as Project.set_fusion stores it.
            now = fusions.get(pair)
            stored = after if after else (0 if (pair[0] in project.added or pair[1] in project.added) else None)
            delta += (_differs(project, pair, stored, removes, project.explicit_after_edit(pair))
                      - _differs(project, pair, now, removes))
            if removes and project.retail.fusions.get(pair) in removes:
                final[pair] = stored
    # A remove whose every recipe the plan puts back goes (Project.settle_removes),
    # and with it the rules its recipes needed: those not kept for themselves.
    for result in set(project.active_removes()) if final else ():
        recipes = project.retail_recipes(result)
        if any(pair in final for pair in recipes) and \
                all(final.get(pair, fusions.get(pair)) == result for pair in recipes):
            delta -= 1
            for pair in recipes:
                kept = project.explicit_after_edit(pair) if pair in final else pair in project.fusion_explicit
                delta -= 0 if kept else 1
    out.rules_after = out.rules_before + delta
    out.samples += out.kept_samples[:SAMPLE - len(out.samples)]
    if out.rules_after > RULE_BUDGET and out.rules_after > out.rules_before:
        out.errors.append(f"the mod would hold {out.rules_after} fusion rules, past the {RULE_BUDGET} the "
                          "editor lets a mod carry: narrow A or B")
    if spec.mode == "add" and not spec.overwrite and out.kept:
        out.warnings.append(f"{out.kept} pairs already fuse and keep their result (choose \"replace\" to change them)")
    return out


# --- carrying it out -------------------------------------------------------------

MISSING = object()


@dataclass
class Batch:
    """What apply() changed: per pair, the entry before and the entry after
    (MISSING: none) and whether the pair was written as a rule of its own
    (Project.fusion_explicit) before and after; the removes before and
    after. undo() puts back the pairs nobody changed since, and a remove
    the batch dropped by putting the last of its recipes back."""
    project: object
    entries: dict = field(default_factory=dict)
    explicit: dict = field(default_factory=dict)
    removes: tuple = ((), ())
    description: str = ""


def apply(project, the_plan: Plan, description: str = "") -> Batch:
    if the_plan.errors:
        raise ValueError("; ".join(the_plan.errors))
    batch = Batch(project, description=description)
    removes = list(project.fusion_removes)
    explicit = project.fusion_explicit
    for pair, _, after in the_plan.changes:
        before, was = project.fusions.get(pair, MISSING), pair in explicit
        project.set_fusion(pair[0], pair[1], after)
        batch.entries[pair] = (before, project.fusions.get(pair, MISSING))
        if was or pair in explicit:
            batch.explicit[pair] = (was, pair in explicit)
    batch.removes = (removes, list(project.fusion_removes))
    return batch


def undo(project, batch: Batch) -> tuple:
    """(pairs put back, pairs changed since and left as they are)."""
    if batch.project is not project:
        raise ValueError("the batch was made on another mod")
    restored = skipped = 0
    for pair, (before, after) in batch.entries.items():
        current = project.fusions.get(pair, MISSING)
        if not (current is after or (MISSING not in (current, after) and current == after)):
            skipped += 1        # edited since: that edit stays
            continue
        if before is MISSING:
            project.fusions.pop(pair, None)
        else:
            project.fusions[pair] = before
        was, now = batch.explicit.get(pair, (False, False))
        if (pair in project.fusion_explicit) == now:
            (project.fusion_explicit.add if was else project.fusion_explicit.discard)(pair)
        restored += 1
    before, after = batch.removes
    dropped = [result for result in before if result not in after and result not in project.fusion_removes]
    if dropped:
        now = project.fusion_removes
        project.fusion_removes = [r for r in before if r in now or r in dropped] + [r for r in now if r not in before]
    project.settle_removes()
    return restored, skipped
