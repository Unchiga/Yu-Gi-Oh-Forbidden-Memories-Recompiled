"""The Cards tab's Monster effects list and the dialog that makes or
changes one (monster_effects.py has what the game takes)."""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk

from . import card_icons, manifest, monster_effects as fx
from .icon_choice import IconChoice
from .gamedata import ATTRIBUTE_NAMES, TYPE_MAGIC, TYPE_NAMES
from .widgets import CardField, FormDialog, px, scrolled_tree

ANY = "Any"


class EffectsBox(ttk.LabelFrame):
    """A monster's effects: a row each, in the order they resolve, and the
    buttons that change them. Each change is stored at once (on_change)."""

    def __init__(self, master, app, on_change):
        super().__init__(master, text="Monster effects", padding=6)
        self.app, self.on_change = app, on_change
        self.cid = None
        self.effects = []
        frame, self.tree = scrolled_tree(self, [("when", "When"), ("what", "Does")], [110, 420], 4, horizontal=True)
        frame.grid(row=0, column=0, columnspan=6, sticky="we")
        self.list_frame = frame         # shown once there is an effect: an empty list took a quarter of the form
        self.columnconfigure(5, weight=1)
        self.tree.bind("<Double-1>", lambda e: self.edit())
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.show_buttons())
        self.buttons = {}
        for column, (key, text, command) in enumerate((("add", "Add...", self.add), ("edit", "Edit...", self.edit),
                                                        ("remove", "Remove", self.remove),
                                                        ("up", "Up", lambda: self.move(-1)),
                                                        ("down", "Down", lambda: self.move(1)))):
            self.buttons[key] = ttk.Button(self, text=text, command=command, width=8 if column < 3 else 5)
            self.buttons[key].grid(row=1, column=column, sticky="w", pady=(4, 0), padx=(0, 4))
        self.note = ttk.Label(self, style="Hint.TLabel", wraplength=px(self, 480), justify="left")
        self.note.grid(row=2, column=0, columnspan=6, sticky="w", pady=(4, 0))
        # A disc card with no effects: "monster_effects": [] takes away any
        # an earlier mod gives it (cards.c), left out leaves them.
        self.none = tk.BooleanVar(self)
        self.none_box = ttk.Checkbutton(self, text="Force no effects", variable=self.none,
                                        command=self.store)
        self.none_box.grid(row=3, column=0, columnspan=6, sticky="w", pady=(4, 0))

    @property
    def project(self):
        return self.app.project

    def show(self, cid):
        """The card's list (an added card's base's, until it has its own)."""
        self.cid = cid
        if cid is None or self.project is None or cid not in self.project.cards:
            self.effects = []
        else:
            effects, _ = self.project.monster_effects_of(cid)
            # An entry that is not an object stays as written (the game leaves it out).
            self.effects = [dict(e) if isinstance(e, dict) else e for e in effects]
        retail = cid is not None and self.project is not None and cid in self.project.cards and \
            cid not in self.project.added
        extra = self.project.card_extra.get(cid, {}) if retail else {}
        self.none.set(retail and extra.get("monster_effects") == [])
        if retail and not self.effects:
            self.none_box.grid()
        else:
            self.none_box.grid_remove()
        self.fill()
        # No effect: Add... and a line, not an empty list and greyed buttons.
        if self.effects:
            self.list_frame.grid()
            for key in ("edit", "remove", "up", "down"):
                self.buttons[key].grid()
        else:
            self.list_frame.grid_remove()
            for key in ("edit", "remove", "up", "down"):
                self.buttons[key].grid_remove()
        if not self.effects and cid is not None:
            self.note.configure(text="No effects")
        else:
            # Only what is wrong with the list; nothing when it is fine.
            problems = fx.problems(self.effects, self.project.resolve if self.project else None,
                                   self.attribute_choice if self.project else None)
            self.note.configure(text="\n".join(problems))
        if self.note.cget("text"):
            self.note.grid()
        else:
            self.note.grid_remove()

    def label(self, cid):
        card = self.project.cards.get(cid) if self.project else None
        return card.name if card else f"#{cid}"

    def attribute_choice(self, value):
        return manifest._attribute_choice(value, self.project.other.get("attributes"))

    def fill(self):
        self.tree.delete(*self.tree.get_children())
        for n, effect in enumerate(self.effects):
            normalized = fx.normalize(effect, self.project.resolve, self.attribute_choice)
            if normalized is None:
                when = effect.get("when", "?") if isinstance(effect, dict) else "?"
                self.tree.insert("", "end", iid=str(n), values=(when, "(not one the game takes) " +
                                                                fx.describe_raw(effect)))
                continue
            self.tree.insert("", "end", iid=str(n), values=(fx.when_label(fx._name(fx.WHEN, effect.get("when"))),
                                                            fx.describe(normalized, self.label)))
        self.show_buttons()

    def selected(self):
        chosen = self.tree.selection()
        return int(chosen[0]) if chosen else None

    def show_buttons(self):
        n = self.selected()
        enabled = self.cid is not None
        self.buttons["add"].state(["!disabled"] if enabled and len(self.effects) < fx.MAX_EFFECTS else ["disabled"])
        for key in ("edit", "remove"):
            self.buttons[key].state(["!disabled"] if n is not None else ["disabled"])
        self.buttons["up"].state(["!disabled"] if n else ["disabled"])
        self.buttons["down"].state(["!disabled"] if n is not None and n < len(self.effects) - 1 else ["disabled"])

    def store(self, select=None):
        if self.cid is None:
            return
        self.project.set_monster_effects(self.cid, self.effects, keep_empty=self.none.get())
        self.on_change(self.cid)
        self.show(self.cid)
        if select is not None and str(select) in self.tree.get_children():
            self.tree.selection_set(str(select))

    def add(self):
        if self.cid is None:
            return
        dialog = EffectDialog(self, self.project, None, self.label, self.app, effects=self.effects, index=None)
        self.wait_window(dialog)
        if dialog.result is not None:
            self.effects.append(dialog.result)
            self.store(len(self.effects) - 1)

    def edit(self):
        n = self.selected()
        if n is None:
            return
        dialog = EffectDialog(self, self.project, self.effects[n], self.label, self.app, effects=self.effects, index=n)
        self.wait_window(dialog)
        if dialog.result is not None:
            self.effects[n] = dialog.result
            self.store(n)

    def remove(self):
        n = self.selected()
        if n is None:
            return
        del self.effects[n]
        # An added card with none left has none: not its base's again.
        self.store(min(n, len(self.effects) - 1) if self.effects else None)

    def move(self, step):
        n = self.selected()
        if n is None or not 0 <= n + step < len(self.effects):
            return
        self.effects[n], self.effects[n + step] = self.effects[n + step], self.effects[n]
        self.store(n + step)


class ConditionDialog(FormDialog):
    """A dialog to create or edit a single condition for a monster effect."""

    def __init__(self, master, project, condition, label, app=None):
        self.project, self.label, self.app = project, label, app
        self.result = None
        self.start = condition or {"type": "named_card", "card": 1, "whose": "all"}
        super().__init__(master, "Condition" if condition is None else "Change condition",
                         self.build, self.read)

    def build(self, dialog, body):
        dialog.minsize(px(dialog, 620), 0)
        dialog.resizable(True, False)
        body.columnconfigure(1, weight=1)
        c = self.start
        self.vars = {
            "type": tk.StringVar(dialog),
            "whose": tk.StringVar(dialog),
            "stat": tk.StringVar(dialog),
            "op": tk.StringVar(dialog),
            "value": tk.StringVar(dialog),
            "presence": tk.StringVar(dialog),
            "scope": tk.StringVar(dialog),
            "type_mode": tk.StringVar(dialog),
            "card_type": tk.StringVar(dialog),
            "attribute": tk.StringVar(dialog),
            "level": tk.StringVar(dialog),
            "field": tk.StringVar(dialog),
            "field_status": tk.StringVar(dialog),
        }
        self.rows = {}
        row = 0

        def line(key, label, widget):
            nonlocal row
            caption = ttk.Label(body, text=label)
            caption.grid(row=row, column=0, sticky="w", pady=2, padx=(0, 8))
            widget.grid(row=row, column=1, sticky="we", pady=2)
            self.rows[key] = (caption, widget)
            row += 1
            return widget

        self.cond_types = list(fx.CONDITIONS)
        self.cond_labels = list(fx.CONDITION_LABELS)

        line("type", "Condition", ttk.Combobox(body, textvariable=self.vars["type"],
                                              values=self.cond_labels, state="readonly", width=48))
        self.type_hint = ttk.Label(body, style="Hint.TLabel", wraplength=px(body, 580), justify="left")
        self.type_hint.grid(row=row, column=0, columnspan=2, sticky="w", pady=(0, 4))
        row += 1

        self.card_field = CardField(body, lambda: self.project, width=38)
        line("card", "Named card", self.card_field)
        line("whose", "On field", ttk.Combobox(body, textvariable=self.vars["whose"],
                                             values=list(fx.EACH_LABELS), state="readonly", width=48))

        line("presence", "Presence", ttk.Combobox(body, textvariable=self.vars["presence"],
                                                values=list(fx.PRESENCE_LABELS), state="readonly", width=48))

        line("stat", "Compare stat", ttk.Combobox(body, textvariable=self.vars["stat"],
                                                 values=list(fx.STAT_LABELS), state="readonly", width=48))
        line("op", "Operator", ttk.Combobox(body, textvariable=self.vars["op"],
                                            values=list(fx.OP_LABELS), state="readonly", width=48))
        self.val_spin = ttk.Spinbox(body, textvariable=self.vars["value"],
                                    from_=0, to=fx.AMOUNT_MAX, increment=100, width=12)
        line("value", "Threshold", self.val_spin)

        self.level_spin = ttk.Spinbox(body, textvariable=self.vars["level"],
                                      from_=1, to=12, increment=1, width=12)
        line("level", "Level", self.level_spin)

        line("type_mode", "Requirement", ttk.Combobox(body, textvariable=self.vars["type_mode"],
                                                     values=list(fx.TYPE_MODE_LABELS), state="readonly", width=48))

        type_choices = list(TYPE_NAMES[:TYPE_MAGIC])
        line("card_type", "Monster type", ttk.Combobox(body, textvariable=self.vars["card_type"],
                                                      values=type_choices, state="readonly", width=48))

        if self.project and "attributes" in self.project.other:
            attr_names, _ = manifest._attribute_registry(self.project.other.get("attributes"))
            attr_choices = list(attr_names.keys())
        else:
            attr_choices = list(ATTRIBUTE_NAMES)
        line("attribute", "Attribute", ttk.Combobox(body, textvariable=self.vars["attribute"],
                                                   values=attr_choices, state="readonly", width=48))

        line("scope", "Scope", ttk.Combobox(body, textvariable=self.vars["scope"],
                                            values=list(fx.SCOPE_LABELS), state="readonly", width=48))

        line("field", "Field spell", ttk.Combobox(body, textvariable=self.vars["field"],
                                                 values=list(fx.FIELD_SPELL_LABELS), state="readonly", width=48))
        line("field_status", "Status", ttk.Combobox(body, textvariable=self.vars["field_status"],
                                                   values=list(fx.FIELD_STATUS_LABELS), state="readonly", width=48))

        c_type = c.get("type", "named_card")

        if c_type in ("lower_lp", "lp_lower", "fewer_lp", "lp_fewer", "lower_life", "lp_less", "player_lp_lower_than_opponent", "player_lp_lower"):
            c_type = "lower_lp"
        elif c_type in ("higher_lp", "lp_higher", "more_lp", "lp_more", "higher_life", "lp_greater", "player_lp_higher_than_opponent", "player_lp_higher"):
            c_type = "higher_lp"
        elif c_type in ("control_monster_stat", "monster_stat", "not_control_stat", "not_control_monster_stat"):
            c_type = "control_stat"
        elif c_type in ("control_monster_level", "monster_level", "not_control_level", "not_control_monster_level"):
            c_type = "control_level"
        elif c_type in ("control_monster_type", "monster_type", "not_control_type", "not_control_monster_type",
                        "type_present", "monster_type_present", "not_type_present", "not_monster_type_present"):
            c_type = "control_type"
        elif c_type in ("control_monster_attribute", "monster_attribute", "not_control_attribute", "not_control_monster_attribute",
                        "attribute_present", "monster_attribute_present", "not_attribute_present", "not_monster_attribute_present"):
            c_type = "control_attribute"
        elif c_type in ("field_spell", "field", "terrain", "field_magic", "not_field_spell", "not_field", "not_terrain"):
            c_type = "field_spell"

        if c_type in self.cond_types:
            self.vars["type"].set(self.cond_labels[self.cond_types.index(c_type)])
        else:
            self.vars["type"].set(self.cond_labels[0])

        if c.get("card"):
            self.card_field.set(c["card"])
        else:
            self.card_field.set(1)

        whose = c.get("whose") or c.get("field")
        if not whose:
            whose = "all" if c_type == "named_card" else "own"
        self.vars["whose"].set(fx.EACH_LABELS[fx.EACH.index(whose)] if whose in fx.EACH else (fx.EACH_LABELS[2] if c_type == "named_card" else fx.EACH_LABELS[0]))

        pres = c.get("presence", "control")
        if str(c.get("type", "")).startswith("not_"):
            pres = "not_control"
        self.vars["presence"].set(fx.PRESENCE_LABELS[fx.PRESENCE.index(pres)] if pres in fx.PRESENCE else fx.PRESENCE_LABELS[0])

        sc = c.get("scope", "own")
        self.vars["scope"].set(fx.SCOPE_LABELS[fx.SCOPE.index(sc)] if sc in fx.SCOPE else fx.SCOPE_LABELS[0])

        t_mode = c.get("mode", "at_least_one")
        self.vars["type_mode"].set(fx.TYPE_MODE_LABELS[fx.TYPE_MODES.index(t_mode)] if t_mode in fx.TYPE_MODES else fx.TYPE_MODE_LABELS[0])

        t_name = c.get("type") or c.get("card_type", type_choices[0])
        self.vars["card_type"].set(t_name if t_name in type_choices else type_choices[0])

        a_name = c.get("attribute", attr_choices[0])
        self.vars["attribute"].set(a_name if a_name in attr_choices else attr_choices[0])

        self.vars["level"].set(str(c.get("level") if "level" in c else c.get("value", 7)))

        stat = c.get("stat", "atk")
        self.vars["stat"].set("DEF" if stat == "def" else "ATK")

        op = c.get("comparison") or c.get("op", ">=" if c_type != "control_level" else "==")
        if op == "=":
            op = "=="
        self.vars["op"].set(fx.OP_LABELS[fx.OPS.index(op)] if op in fx.OPS else fx.OP_LABELS[3])

        self.vars["value"].set(str(c.get("value", 1500)))

        fld = c.get("field") or c.get("field_spell") or c.get("terrain") or "any"
        if isinstance(fld, int) and 330 <= fld <= 335:
            fld = fx.FIELD_SPELLS[fld - 330 + 1]
        elif isinstance(fld, int) and 0 <= fld < len(fx.FIELD_SPELLS):
            fld = fx.FIELD_SPELLS[fld]
        elif isinstance(fld, str):
            fld_lower = fld.lower()
            if fld_lower in ("sea", "ocean"):
                fld = "umi"
            elif fld_lower in ("meadow", "grassland"):
                fld = "sogen"
            elif fld_lower in ("dark", "darkness"):
                fld = "yami"
        fld_idx = fx.FIELD_SPELLS.index(fld) if fld in fx.FIELD_SPELLS else 0
        self.vars["field"].set(fx.FIELD_SPELL_LABELS[fld_idx])

        f_status = c.get("presence") or c.get("status") or ("not_active" if str(c.get("type", "")).startswith("not_") else "active")
        if f_status in ("not_active", "inactive", "not_control", "not", "0", False):
            self.vars["field_status"].set(fx.FIELD_STATUS_LABELS[1])
        else:
            self.vars["field_status"].set(fx.FIELD_STATUS_LABELS[0])

        self.vars["type"].trace_add("write", lambda *_: self.show_rows())
        self.show_rows()

    def chosen(self, key, names, labels):
        text = self.vars[key].get()
        if text in labels:
            return names[labels.index(text)]
        if text in names:
            return text
        return None

    def show_rows(self):
        c_type = self.chosen("type", self.cond_types, self.cond_labels) or self.cond_types[0]
        self.type_hint.configure(text=fx.CONDITION_HINTS[fx.CONDITIONS.index(c_type)])
        shown = {"type"}
        if c_type == "named_card":
            shown |= {"card", "whose"}
        elif c_type == "stat_threshold":
            shown |= {"stat", "op", "value"}
        elif c_type == "control_stat":
            shown |= {"presence", "stat", "op", "value", "scope"}
        elif c_type == "control_level":
            shown |= {"presence", "op", "level", "scope"}
        elif c_type == "control_type":
            shown |= {"whose", "presence", "type_mode", "card_type", "scope"}
        elif c_type == "control_attribute":
            shown |= {"whose", "presence", "attribute", "scope"}
        elif c_type == "field_spell":
            shown |= {"field", "field_status"}
        elif c_type in ("lowest_atk", "highest_atk", "fewer_monsters", "more_monsters", "lower_lp", "higher_lp"):
            pass

        for key, widgets in self.rows.items():
            for widget in widgets:
                widget.grid() if key in shown else widget.grid_remove()

    def read(self, dialog):
        c_type = self.chosen("type", self.cond_types, self.cond_labels)
        if not c_type:
            return "Choose a condition type."
        out = {"type": c_type}
        if c_type == "named_card":
            cid = self.card_field.get()
            if not cid:
                return "Choose a card."
            out["card"] = cid
            whose = self.chosen("whose", fx.EACH, fx.EACH_LABELS)
            if whose and whose != "all":
                out["whose"] = whose
        elif c_type == "stat_threshold":
            stat = self.chosen("stat", fx.STATS, fx.STAT_LABELS) or "atk"
            out["stat"] = stat
            op = self.chosen("op", fx.OPS, fx.OP_LABELS) or ">="
            out["comparison"] = op
            try:
                val = int(self.vars["value"].get().strip())
            except ValueError:
                return "Threshold must be a whole number."
            if not 0 <= val <= fx.AMOUNT_MAX:
                return f"Threshold must be 0 to {fx.AMOUNT_MAX}."
            out["value"] = val
        elif c_type == "control_stat":
            pres = self.chosen("presence", fx.PRESENCE, fx.PRESENCE_LABELS) or "control"
            if pres == "not_control":
                out["presence"] = "not_control"
            stat = self.chosen("stat", fx.STATS, fx.STAT_LABELS) or "atk"
            out["stat"] = stat
            op = self.chosen("op", fx.OPS, fx.OP_LABELS) or ">="
            out["comparison"] = op
            try:
                val = int(self.vars["value"].get().strip())
            except ValueError:
                return "Threshold must be a whole number."
            if not 0 <= val <= fx.AMOUNT_MAX:
                return f"Threshold must be 0 to {fx.AMOUNT_MAX}."
            out["value"] = val
            sc = self.chosen("scope", fx.SCOPE, fx.SCOPE_LABELS) or "own"
            if sc == "other":
                out["scope"] = "other"
        elif c_type == "control_level":
            pres = self.chosen("presence", fx.PRESENCE, fx.PRESENCE_LABELS) or "control"
            if pres == "not_control":
                out["presence"] = "not_control"
            op = self.chosen("op", fx.OPS, fx.OP_LABELS) or "=="
            out["comparison"] = op
            try:
                lvl = int(self.vars["level"].get().strip())
            except ValueError:
                return "Level must be a number."
            if not 1 <= lvl <= 12:
                return "Level must be 1 to 12."
            out["level"] = lvl
            sc = self.chosen("scope", fx.SCOPE, fx.SCOPE_LABELS) or "own"
            if sc == "other":
                out["scope"] = "other"
        elif c_type == "control_type":
            whose = self.chosen("whose", fx.EACH, fx.EACH_LABELS) or "own"
            if whose and whose != "own":
                out["whose"] = whose
            pres = self.chosen("presence", fx.PRESENCE, fx.PRESENCE_LABELS) or "control"
            if pres == "not_control":
                out["presence"] = "not_control"
            mode = self.chosen("type_mode", fx.TYPE_MODES, fx.TYPE_MODE_LABELS) or "at_least_one"
            if mode == "only":
                out["mode"] = "only"
            t = self.vars["card_type"].get()
            if not t:
                return "Choose a monster type."
            out["card_type"] = t
            sc = self.chosen("scope", fx.SCOPE, fx.SCOPE_LABELS) or "own"
            if sc == "other":
                out["scope"] = "other"
        elif c_type == "control_attribute":
            whose = self.chosen("whose", fx.EACH, fx.EACH_LABELS) or "own"
            if whose and whose != "own":
                out["whose"] = whose
            pres = self.chosen("presence", fx.PRESENCE, fx.PRESENCE_LABELS) or "control"
            if pres == "not_control":
                out["presence"] = "not_control"
            attr = self.vars["attribute"].get()
            if not attr:
                return "Choose an attribute."
            out["attribute"] = attr
            sc = self.chosen("scope", fx.SCOPE, fx.SCOPE_LABELS) or "own"
            if sc == "other":
                out["scope"] = "other"
        elif c_type == "field_spell":
            fld = self.chosen("field", fx.FIELD_SPELLS, fx.FIELD_SPELL_LABELS) or "any"
            out["field"] = fld
            st = self.chosen("field_status", fx.FIELD_STATUS, fx.FIELD_STATUS_LABELS) or "active"
            out["presence"] = st
        elif c_type in ("lowest_atk", "highest_atk", "fewer_monsters", "more_monsters", "lower_lp", "higher_lp"):
            pass
        self.result = out
        return None


class EffectDialog(FormDialog):
    """One effect: when, what, and what that needs. result is the entry
    as the game reads it, or None when cancelled."""

    def __init__(self, master, project, effect, label, app=None, effects=None, index=None):
        """app: the window, whose game files give the lists' icons (none without)."""
        self.project, self.label, self.app = project, label, app
        self.effects = effects or []
        self.index = index
        self.result = None
        effect = fx.normalize(effect, project.resolve, self.attribute_choice) if effect else None
        self.start = effect or {"when": "summon", "do": "boost", "target": "self", "attack": 500}
        self.conditions = [dict(c) for c in (effect.get("conditions") or
                           ([effect["condition"]] if effect and "condition" in effect else []))] if effect else []
        self.attack_mod = "always"
        self.attack_reset = "turn"
        if effect and effect.get("do") == "battle_immunity":
            self.attack_mod = effect.get("modifier", "always")
            self.attack_reset = effect.get("reset", "turn")
            for i, c in enumerate(self.conditions):
                if c.get("type") in fx.ATTACK_CONDITIONS:
                    t = c.get("type")
                    if t in ("first_attacked", "after_one_attack", "destroyed_after_1_attack"):
                        self.attack_mod = "first_attacked"
                    elif t in ("second_attacked", "destroyed_after_2_attacks", "destroyed_after_two_attacks", "can_only_be_destroyed_after_two_attacks"):
                        self.attack_mod = "destroyed_after_2_attacks"
                    elif t in ("third_attacked", "destroyed_after_3_attacks", "destroyed_after_three_attacks", "can_only_be_destroyed_after_three_attacks"):
                        self.attack_mod = "destroyed_after_3_attacks"
                    if "reset" in c:
                        self.attack_reset = c["reset"]
                    del self.conditions[i]
                    break
        elif effect and effect.get("do") == "piercing":
            self.attack_mod = effect.get("modifier", "always")
        super().__init__(master, "Monster effect" if effect is None else "Change the monster effect", self.build,
                         self.read)

    def magic_label(self, cid):
        return f"{cid} {self.label(cid)}"

    def attribute_choice(self, value):
        return manifest._attribute_choice(value, self.project.other.get("attributes"))

    def custom_attribute_labels(self):
        names, declared = manifest._attribute_registry(self.project.other.get("attributes"))
        return {(f"{names[ident]} ({ident})" if names[ident] == ANY else names[ident]): ident
                for ident in sorted(declared)}

    def attribute_names(self):
        names, declared = manifest._attribute_registry(self.project.other.get("attributes"))
        labels = self.custom_attribute_labels()
        retail = [next((label for label, ident in labels.items() if ident == ident_), names[ident_])
                  for ident_ in range(len(ATTRIBUTE_NAMES))]
        return [ANY] + retail + [label for label, ident in labels.items() if ident >= len(ATTRIBUTE_NAMES)]

    def attribute_label(self, value):
        names, declared = manifest._attribute_registry(self.project.other.get("attributes"))
        if type(value) is int and value in declared:
            return next(label for label, ident in self.custom_attribute_labels().items() if ident == value)
        return names[value] if type(value) is int and 0 <= value < len(ATTRIBUTE_NAMES) else value

    def attribute_value(self, label):
        return self.custom_attribute_labels().get(label, label)

    def build(self, dialog, body):
        dialog.minsize(px(dialog, 680), 0)
        dialog.resizable(True, False)
        body.columnconfigure(1, weight=1)
        e = self.start
        self.vars = {k: tk.StringVar(dialog) for k in ("when", "do", "card", "target", "type", "attribute", "attack",
                                                 "defense", "amount", "each", "each_type", "each_attribute",
                                                 "cond_mode", "modifier", "modifier_reset",
                                                 "attacker_type", "attacker_attribute", "attacker_stat",
                                                 "attacker_stat_op", "attacker_amount",
                                                 "attacker_level_op", "attacker_level")}
        self.rows = {}
        row = 0

        def line(key, label, widget):
            nonlocal row
            caption = ttk.Label(body, text=label)
            caption.grid(row=row, column=0, sticky="w", pady=2, padx=(0, 8))
            widget.grid(row=row, column=1, sticky="we", pady=2)
            self.rows[key] = (caption, widget)
            row += 1
            return widget

        line("when", "When", ttk.Combobox(body, textvariable=self.vars["when"], values=list(fx.WHEN_LABELS),
                                          state="readonly", width=48))
        self.when_hint = ttk.Label(body, style="Hint.TLabel", wraplength=px(body, 640), justify="left")
        self.when_hint.grid(row=row, column=0, columnspan=2, sticky="w", pady=(0, 4))
        row += 1
        self.do_box = line("do", "Does", ttk.Combobox(body, textvariable=self.vars["do"], state="readonly", width=48))
        line("card", "Magic card", self.choice(body, "card", [self.magic_label(c) for c in fx.MAGIC], lambda v: 0x14))
        self.target_box = line("target", "Whose", ttk.Combobox(body, textvariable=self.vars["target"],
                                                               state="readonly", width=48))
        line("type", "Only type", self.choice(body, "type", [ANY] + TYPE_NAMES[:TYPE_MAGIC],
                                              lambda v: TYPE_NAMES.index(v) if v in TYPE_NAMES else None))
        line("attribute", "Only attribute", self.choice(body, "attribute", self.attribute_names(), self.attribute_icon))
        pictures = card_icons.photos(self.app, body, 2) if self.app is not None else {}
        for key, label in (("attack", "ATK"), ("defense", "DEF")):
            line(key, label, ttk.Spinbox(body, textvariable=self.vars[key], from_=-fx.BOOST_MAX, to=fx.BOOST_MAX,
                                         increment=100, width=12))
            if pictures.get(key):       # the sword and the shield, as the Cards tab's
                self.rows[key][0].configure(image=pictures[key], compound="left")
        line("amount", "LP", ttk.Spinbox(body, textvariable=self.vars["amount"], from_=1, to=fx.AMOUNT_MAX,
                                         increment=100, width=12))
        # "for_each": the number above made once per face-up monster counted.
        line("each", "For each", ttk.Combobox(body, textvariable=self.vars["each"], state="readonly", width=48,
                                              values=[fx.NO_EACH] + list(fx.EACH_LABELS)))
        line("each_type", "Counting type", self.choice(body, "each_type", [ANY] + TYPE_NAMES[:TYPE_MAGIC],
                                                       lambda v: TYPE_NAMES.index(v) if v in TYPE_NAMES else None))
        line("each_attribute", "Counting attribute", self.choice(body, "each_attribute", self.attribute_names(),
                                                                 self.attribute_icon))

        # Modifiers field sits right on top of conditions (API 19)
        line("modifier", "Modifiers", ttk.Combobox(body, textvariable=self.vars["modifier"],
                                                   values=list(fx.MODIFIER_LABELS), state="readonly", width=48))
        line("modifier_reset", "Resets", ttk.Combobox(body, textvariable=self.vars["modifier_reset"],
                                                      values=list(fx.RESET_LABELS), state="readonly", width=48))
        line("attacker_type", "Attacker type", self.choice(body, "attacker_type", [ANY] + TYPE_NAMES[:TYPE_MAGIC],
                                                           lambda v: TYPE_NAMES.index(v) if v in TYPE_NAMES else None))
        line("attacker_attribute", "Attacker attribute", self.choice(body, "attacker_attribute", self.attribute_names(),
                                                                     self.attribute_icon))
        line("attacker_stat", "Attacker stat", ttk.Combobox(body, textvariable=self.vars["attacker_stat"],
                                                            values=["None", "ATK", "DEF"], state="readonly", width=48))
        line("attacker_stat_op", "Stat comparison", ttk.Combobox(body, textvariable=self.vars["attacker_stat_op"],
                                                                 values=list(fx.OP_LABELS), state="readonly", width=48))
        line("attacker_amount", "Stat threshold", ttk.Spinbox(body, textvariable=self.vars["attacker_amount"], from_=0, to=fx.AMOUNT_MAX,
                                                           increment=100, width=12))
        line("attacker_level_op", "Level comparison", ttk.Combobox(body, textvariable=self.vars["attacker_level_op"],
                                                                   values=list(fx.OP_LABELS), state="readonly", width=48))
        line("attacker_level", "Attacker level", ttk.Spinbox(body, textvariable=self.vars["attacker_level"], from_=0, to=12,
                                                                increment=1, width=12))

        # Conditions section (API 19)
        self.cond_frame = ttk.LabelFrame(body, text="Conditions (optional)", padding=6)
        self.cond_frame.columnconfigure(0, weight=1)
        c_frame, self.cond_tree = scrolled_tree(self.cond_frame, [("type", "Type"), ("details", "Condition")], [160, 460], 3, horizontal=True)
        c_frame.grid(row=0, column=0, columnspan=3, sticky="we")
        self.cond_tree.bind("<Double-1>", lambda ev: self.edit_condition())
        self.cond_tree.bind("<<TreeviewSelect>>", lambda ev: self.show_condition_buttons())

        self.cond_buttons = {}
        for col, (key, text, cmd) in enumerate((("add", "Add...", self.add_condition),
                                                 ("edit", "Edit...", self.edit_condition),
                                                 ("remove", "Remove", self.remove_condition))):
            btn = ttk.Button(self.cond_frame, text=text, command=cmd, width=8)
            btn.grid(row=1, column=col, sticky="w", pady=(4, 0), padx=(0, 4))
            self.cond_buttons[key] = btn

        mode_row = ttk.Frame(self.cond_frame)
        mode_row.grid(row=2, column=0, columnspan=3, sticky="we", pady=(4, 0))
        ttk.Label(mode_row, text="Mode:").pack(side="left", padx=(0, 4))
        ttk.Combobox(mode_row, textvariable=self.vars["cond_mode"],
                     values=list(fx.COND_MODE_LABELS), state="readonly", width=46).pack(side="left", fill="x", expand=True)

        self.cond_frame.grid(row=row, column=0, columnspan=2, sticky="we", pady=(6, 2))
        row += 1

        self.vars["when"].set(fx.when_label(e["when"]))
        self.vars["do"].set(fx.DO_LABELS[fx.DO.index(e["do"])])
        self.vars["card"].set(self.magic_label(e.get("card", 337)))
        target = e.get("target", fx.default_target(e["when"], e["do"]))
        self.vars["target"].set(fx.TARGET_LABELS[fx.TARGET.index(target)])
        self.vars["type"].set(e.get("type", ANY))
        self.vars["attribute"].set(self.attribute_label(e.get("attribute", ANY)))
        self.vars["attack"].set(e.get("attack", 0))
        self.vars["defense"].set(e.get("defense", 0))
        self.vars["amount"].set(e.get("amount", 500))
        each = e.get("for_each")
        self.vars["each"].set(fx.EACH_LABELS[fx.EACH.index(each["whose"])] if each else fx.NO_EACH)
        self.vars["each_type"].set((each or {}).get("type", ANY))
        self.vars["each_attribute"].set(self.attribute_label((each or {}).get("attribute", ANY)))

        if e.get("do") == "piercing":
            mod_idx = fx.PIERCING_MODIFIERS.index(self.attack_mod) if self.attack_mod in fx.PIERCING_MODIFIERS else 0
            self.vars["modifier"].set(fx.PIERCING_MODIFIER_LABELS[mod_idx])
        else:
            mod_idx = fx.MODIFIERS.index(self.attack_mod) if self.attack_mod in fx.MODIFIERS else 0
            self.vars["modifier"].set(fx.MODIFIER_LABELS[mod_idx])
        rst = self.attack_reset
        if rst == "summon":
            rst = "copy"
        rst_idx = fx.RESET.index(rst) if rst in fx.RESET else 0
        self.vars["modifier_reset"].set(fx.RESET_LABELS[rst_idx])

        t_init = e.get("defender_type") or e.get("target_type") or e.get("attacker_type", ANY)
        self.vars["attacker_type"].set(t_init)
        a_init = e.get("defender_attribute") or e.get("target_attribute") or e.get("attacker_attribute", ANY)
        self.vars["attacker_attribute"].set(self.attribute_label(a_init))
        stat_val = e.get("defender_stat") or e.get("target_stat") or e.get("attacker_stat", "")
        self.vars["attacker_stat"].set(stat_val.upper() if stat_val in ("atk", "def") else ("ATK" if self.attack_mod in ("attacked_by_stat", "attacks_stat") else "None"))
        stat_op = e.get("defender_stat_op") or e.get("target_stat_op") or e.get("attacker_stat_op") or e.get("comparison") or e.get("op", ">")
        self.vars["attacker_stat_op"].set(fx.OP_LABELS[fx.OPS.index(stat_op)] if stat_op in fx.OPS else fx.OP_LABELS[2])
        amt_init = e.get("defender_amount") if "defender_amount" in e else (e.get("target_amount") if "target_amount" in e else e.get("attacker_amount", 1500))
        self.vars["attacker_amount"].set(str(amt_init))
        level_op = e.get("defender_level_op") or e.get("target_level_op") or e.get("attacker_level_op") or (e.get("comparison") if self.attack_mod in ("attacked_by_level", "attacks_level") else None) or e.get("op", ">")
        self.vars["attacker_level_op"].set(fx.OP_LABELS[fx.OPS.index(level_op)] if level_op in fx.OPS else fx.OP_LABELS[2])
        lvl_init = e.get("defender_level") if "defender_level" in e else (e.get("target_level") if "target_level" in e else e.get("attacker_level", 4 if self.attack_mod in ("attacked_by_level", "attacks_level") else 0))
        self.vars["attacker_level"].set(str(lvl_init))

        cond_mode = e.get("condition_mode", "all")
        self.vars["cond_mode"].set(fx.COND_MODE_LABELS[fx.COND_MODES.index(cond_mode)] if cond_mode in fx.COND_MODES else fx.COND_MODE_LABELS[0])

        self.fill_conditions()

        for key in ("when", "do", "each", "modifier"):
            self.vars[key].trace_add("write", lambda *_: self.show_rows())
        self.show_rows()

    def fill_conditions(self):
        self.cond_tree.delete(*self.cond_tree.get_children())
        for idx, cond in enumerate(self.conditions):
            c_type = cond.get("type", "")
            lbl = fx.CONDITION_LABELS[fx.CONDITIONS.index(c_type)] if c_type in fx.CONDITIONS else c_type
            details = fx.condition_words(cond, self.label)
            self.cond_tree.insert("", "end", iid=str(idx), values=(lbl, details))
        self.show_condition_buttons()

    def show_condition_buttons(self):
        sel = self.cond_tree.selection()
        idx = int(sel[0]) if sel else None
        self.cond_buttons["add"].state(["!disabled"] if len(self.conditions) < fx.MAX_CONDITIONS else ["disabled"])
        self.cond_buttons["edit"].state(["!disabled"] if idx is not None else ["disabled"])
        self.cond_buttons["remove"].state(["!disabled"] if idx is not None else ["disabled"])

    def add_condition(self):
        if len(self.conditions) >= fx.MAX_CONDITIONS:
            return
        dlg = ConditionDialog(self, self.project, None, self.label, self.app)
        self.wait_window(dlg)
        if dlg.result is not None:
            self.conditions.append(dlg.result)
            self.fill_conditions()

    def edit_condition(self):
        sel = self.cond_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        dlg = ConditionDialog(self, self.project, self.conditions[idx], self.label, self.app)
        self.wait_window(dlg)
        if dlg.result is not None:
            self.conditions[idx] = dlg.result
            self.fill_conditions()

    def remove_condition(self):
        sel = self.cond_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        del self.conditions[idx]
        self.fill_conditions()

    def choice(self, body, key, values, icon_of):
        """A list with the game's icons beside its choices (icon_choice.py),
        or a plain one without a window to take them from."""
        if self.app is None:
            return ttk.Combobox(body, textvariable=self.vars[key], state="readonly", width=48, values=values)
        return IconChoice(body, self.app, self.vars[key], values, icon_of, width=48)

    def attribute_icon(self, value):
        ident = self.attribute_choice(value)
        if not 0 <= ident < len(ATTRIBUTE_NAMES):
            return None
        return card_icons.photos(self.app, self).get("attribute", {}).get(ident)

    def chosen(self, key, names, labels):
        text = self.vars[key].get()
        return names[labels.index(text)] if text in labels else None

    def show_rows(self):
        when = self.chosen("when", fx.WHEN, fx.WHEN_LABELS) or "summon"
        self.when_hint.configure(text=fx.WHEN_HINTS[fx.WHEN.index(when)])
        actions = fx.actions(when)
        self.do_box.configure(values=[fx.DO_LABELS[fx.DO.index(a)] for a in actions])
        do = self.chosen("do", fx.DO, fx.DO_LABELS)
        if do not in actions:
            do = actions[0]
            self.vars["do"].set(fx.DO_LABELS[fx.DO.index(do)])
            return      # the trace calls again
        targets = fx.targets(when, do if do in fx.FILTERED else "boost")
        self.target_box.configure(values=[fx.TARGET_LABELS[fx.TARGET.index(t)] for t in targets])
        if self.chosen("target", fx.TARGET, fx.TARGET_LABELS) not in targets:
            self.vars["target"].set(fx.TARGET_LABELS[fx.TARGET.index(targets[0])])
        shown = {"when", "do"} | {"magic": {"card"}, "boost": {"target", "type", "attribute", "attack", "defense"},
                                  "heal": {"amount"}, "damage": {"amount"},
                                  "destroy": {"target", "type", "attribute"},
                                  "battle_immunity": set(),
                                  "piercing": set()}[do]
        if do == "battle_immunity":
            self.rows["modifier"][1].configure(values=list(fx.MODIFIER_LABELS))
            self.rows["attacker_type"][0].configure(text="Attacker type")
            self.rows["attacker_attribute"][0].configure(text="Attacker attribute")
            self.rows["attacker_stat"][0].configure(text="Attacker stat")
            self.rows["attacker_level"][0].configure(text="Attacker level")
            shown.add("modifier")
            mod = self.chosen("modifier", fx.MODIFIERS, fx.MODIFIER_LABELS)
            if mod in fx.ATTACK_COUNT_MODIFIERS:
                shown.add("modifier_reset")
            elif mod == "attacked_by_type":
                shown.add("attacker_type")
            elif mod == "attacked_by_attribute":
                shown.add("attacker_attribute")
            elif mod == "attacked_by_stat":
                shown |= {"attacker_stat", "attacker_stat_op", "attacker_amount"}
            elif mod == "attacked_by_level":
                shown |= {"attacker_level_op", "attacker_level"}
            elif mod == "attacked_by_monster":
                shown |= {"attacker_type", "attacker_attribute", "attacker_stat", "attacker_stat_op", "attacker_amount", "attacker_level_op", "attacker_level"}
        elif do == "piercing":
            self.rows["modifier"][1].configure(values=list(fx.PIERCING_MODIFIER_LABELS))
            self.rows["attacker_type"][0].configure(text="Defender type")
            self.rows["attacker_attribute"][0].configure(text="Defender attribute")
            self.rows["attacker_stat"][0].configure(text="Defender stat")
            self.rows["attacker_level"][0].configure(text="Defender level")
            shown.add("modifier")
            mod = self.chosen("modifier", fx.PIERCING_MODIFIERS, fx.PIERCING_MODIFIER_LABELS)
            if mod == "attacks_type":
                shown.add("attacker_type")
            elif mod == "attacks_attribute":
                shown.add("attacker_attribute")
            elif mod == "attacks_stat":
                shown |= {"attacker_stat", "attacker_stat_op", "attacker_amount"}
            elif mod == "attacks_level":
                shown |= {"attacker_level_op", "attacker_level"}
            elif mod == "attacks_monster":
                shown |= {"attacker_type", "attacker_attribute", "attacker_stat", "attacker_stat_op", "attacker_amount", "attacker_level_op", "attacker_level"}
        if do in fx.EACH_DO:
            shown.add("each")
            if self.vars["each"].get() in fx.EACH_LABELS:
                shown |= {"each_type", "each_attribute"}
        for key, widgets in self.rows.items():
            for widget in widgets:
                widget.grid() if key in shown else widget.grid_remove()

        if when == "face_up":
            self.cond_frame.grid()
        else:
            self.cond_frame.grid_remove()

    def read(self, dialog):
        when = self.chosen("when", fx.WHEN, fx.WHEN_LABELS)
        do = self.chosen("do", fx.DO, fx.DO_LABELS)
        if not when or not do:
            return "Choose when and what."
        effect = {"when": when, "do": do}
        all_conds = []
        if do == "battle_immunity":
            effect["target"] = "self"
            mod = self.chosen("modifier", fx.MODIFIERS, fx.MODIFIER_LABELS) or "always"
            if mod in fx.ATTACK_COUNT_MODIFIERS:
                if fx.monster_has_attack_condition(self.effects, self.index):
                    return "A monster can only have one condition related to a number of attacks."
                effect["modifier"] = mod
                rst = self.chosen("modifier_reset", fx.RESET, fx.RESET_LABELS) or "turn"
                effect["reset"] = rst
            elif mod == "attacked_by_type":
                effect["modifier"] = mod
                t = self.vars["attacker_type"].get()
                if not t or t == ANY:
                    return "Choose a monster type."
                effect["attacker_type"] = t
            elif mod == "attacked_by_attribute":
                effect["modifier"] = mod
                a = self.vars["attacker_attribute"].get()
                if not a or a == ANY:
                    return "Choose an attribute."
                effect["attacker_attribute"] = self.attribute_value(a)
            elif mod == "attacked_by_stat":
                effect["modifier"] = mod
                s = self.vars["attacker_stat"].get().lower()
                if s not in ("atk", "def"):
                    s = "atk"
                effect["attacker_stat"] = s
                sop = self.chosen("attacker_stat_op", fx.OPS, fx.OP_LABELS) or ">"
                effect["attacker_stat_op"] = sop
                try:
                    amt = int(self.vars["attacker_amount"].get().strip())
                except ValueError:
                    return "Stat threshold must be a whole number."
                if not 0 <= amt <= fx.AMOUNT_MAX:
                    return f"Stat threshold must be 0 to {fx.AMOUNT_MAX}."
                effect["attacker_amount"] = amt
            elif mod == "attacked_by_level":
                effect["modifier"] = mod
                lop = self.chosen("attacker_level_op", fx.OPS, fx.OP_LABELS) or ">"
                effect["attacker_level_op"] = lop
                try:
                    lvl = int(self.vars["attacker_level"].get().strip())
                except ValueError:
                    return "Level must be a number."
                if not 1 <= lvl <= 12:
                    return "Level must be 1 to 12."
                effect["attacker_level"] = lvl
            elif mod == "attacked_by_monster":
                effect["modifier"] = mod
                t = self.vars["attacker_type"].get()
                if t and t != ANY:
                    effect["attacker_type"] = t
                a = self.vars["attacker_attribute"].get()
                if a and a != ANY:
                    effect["attacker_attribute"] = self.attribute_value(a)
                s = self.vars["attacker_stat"].get()
                if s in ("ATK", "DEF"):
                    effect["attacker_stat"] = s.lower()
                    sop = self.chosen("attacker_stat_op", fx.OPS, fx.OP_LABELS) or ">"
                    effect["attacker_stat_op"] = sop
                    try:
                        amt = int(self.vars["attacker_amount"].get().strip())
                    except ValueError:
                        return "Stat threshold must be a whole number."
                    if not 0 <= amt <= fx.AMOUNT_MAX:
                        return f"Stat threshold must be 0 to {fx.AMOUNT_MAX}."
                    effect["attacker_amount"] = amt
                lvl_str = self.vars["attacker_level"].get().strip()
                if lvl_str and lvl_str != "0":
                    try:
                        lvl = int(lvl_str)
                    except ValueError:
                        return "Level must be a number."
                    if not 1 <= lvl <= 12:
                        return "Level must be 1 to 12."
                    lop = self.chosen("attacker_level_op", fx.OPS, fx.OP_LABELS) or ">"
                    effect["attacker_level_op"] = lop
                    effect["attacker_level"] = lvl
                if not any(k in effect for k in ("attacker_type", "attacker_attribute", "attacker_stat", "attacker_level")):
                    return "Specify at least one attacker criteria (Type, Attribute, Stat, or Level)."
            all_conds = [dict(c) for c in self.conditions]
        elif do == "piercing":
            effect["target"] = "self"
            mod = self.chosen("modifier", fx.PIERCING_MODIFIERS, fx.PIERCING_MODIFIER_LABELS) or "always"
            if mod == "attacks_type":
                effect["modifier"] = mod
                t = self.vars["attacker_type"].get()
                if not t or t == ANY:
                    return "Choose a monster type."
                effect["defender_type"] = t
            elif mod == "attacks_attribute":
                effect["modifier"] = mod
                a = self.vars["attacker_attribute"].get()
                if not a or a == ANY:
                    return "Choose an attribute."
                effect["defender_attribute"] = self.attribute_value(a)
            elif mod == "attacks_stat":
                effect["modifier"] = mod
                s = self.vars["attacker_stat"].get().lower()
                if s not in ("atk", "def"):
                    s = "atk"
                effect["defender_stat"] = s
                sop = self.chosen("attacker_stat_op", fx.OPS, fx.OP_LABELS) or ">"
                effect["defender_stat_op"] = sop
                try:
                    amt = int(self.vars["attacker_amount"].get().strip())
                except ValueError:
                    return "Stat threshold must be a whole number."
                if not 0 <= amt <= fx.AMOUNT_MAX:
                    return f"Stat threshold must be 0 to {fx.AMOUNT_MAX}."
                effect["defender_amount"] = amt
            elif mod == "attacks_level":
                effect["modifier"] = mod
                lop = self.chosen("attacker_level_op", fx.OPS, fx.OP_LABELS) or ">"
                effect["defender_level_op"] = lop
                try:
                    lvl = int(self.vars["attacker_level"].get().strip())
                except ValueError:
                    return "Level must be a number."
                if not 1 <= lvl <= 12:
                    return "Level must be 1 to 12."
                effect["defender_level"] = lvl
            elif mod == "attacks_monster":
                effect["modifier"] = mod
                t = self.vars["attacker_type"].get()
                if t and t != ANY:
                    effect["defender_type"] = t
                a = self.vars["attacker_attribute"].get()
                if a and a != ANY:
                    effect["defender_attribute"] = self.attribute_value(a)
                s = self.vars["attacker_stat"].get()
                if s in ("ATK", "DEF"):
                    effect["defender_stat"] = s.lower()
                    sop = self.chosen("attacker_stat_op", fx.OPS, fx.OP_LABELS) or ">"
                    effect["defender_stat_op"] = sop
                    try:
                        amt = int(self.vars["attacker_amount"].get().strip())
                    except ValueError:
                        return "Stat threshold must be a whole number."
                    if not 0 <= amt <= fx.AMOUNT_MAX:
                        return f"Stat threshold must be 0 to {fx.AMOUNT_MAX}."
                    effect["defender_amount"] = amt
                lvl_str = self.vars["attacker_level"].get().strip()
                if lvl_str and lvl_str != "0":
                    try:
                        lvl = int(lvl_str)
                    except ValueError:
                        return "Level must be a number."
                    if not 1 <= lvl <= 12:
                        return "Level must be 1 to 12."
                    lop = self.chosen("attacker_level_op", fx.OPS, fx.OP_LABELS) or ">"
                    effect["defender_level_op"] = lop
                    effect["defender_level"] = lvl
                if not any(k in effect for k in ("defender_type", "defender_attribute", "defender_stat", "defender_level")):
                    return "Specify at least one target criteria (Type, Attribute, Stat, or Level)."
            all_conds = [dict(c) for c in self.conditions]
        elif do == "magic":
            text = self.vars["card"].get()
            effect["card"] = int(text.split(" ", 1)[0]) if text[:1].isdigit() else 0
        elif do in fx.FILTERED:
            effect["target"] = self.chosen("target", fx.TARGET, fx.TARGET_LABELS)
            for key in ("attack", "defense") if do == "boost" else ():
                text = self.vars[key].get().strip() or "0"
                try:
                    value = int(text)
                except ValueError:
                    return "ATK and DEF are whole numbers (negative takes away)."
                if abs(value) > fx.BOOST_MAX:
                    return f"ATK and DEF are -{fx.BOOST_MAX} to {fx.BOOST_MAX}."
                if value:
                    effect[key] = value
            if do == "boost" and "attack" not in effect and "defense" not in effect:
                return "A boost needs ATK or DEF."
            for key in ("type", "attribute"):
                if self.vars[key].get() not in ("", ANY):
                    effect[key] = self.attribute_value(self.vars[key].get()) if key == "attribute" else self.vars[key].get()
        else:
            try:
                effect["amount"] = int(self.vars["amount"].get().strip())
            except ValueError:
                return "LP is a whole number."
            if not 0 < effect["amount"] <= fx.AMOUNT_MAX:
                return f"LP is 1 to {fx.AMOUNT_MAX}."
        whose = self.chosen("each", fx.EACH, fx.EACH_LABELS)
        if whose and do in fx.EACH_DO:
            effect["for_each"] = {"whose": whose}
            for key in ("type", "attribute"):
                if self.vars["each_" + key].get() not in ("", ANY):
                    value = self.vars["each_" + key].get()
                    effect["for_each"][key] = self.attribute_value(value) if key == "attribute" else value

        if when == "face_up":
            if do not in ("battle_immunity", "piercing"):
                all_conds = [dict(c) for c in self.conditions]
            if all_conds:
                effect["conditions"] = all_conds
                mode_lbl = self.vars["cond_mode"].get()
                mode = fx.COND_MODES[fx.COND_MODE_LABELS.index(mode_lbl)] if mode_lbl in fx.COND_MODE_LABELS else "all"
                if mode != "all":
                    effect["condition_mode"] = mode

        effect = fx.normalize(effect, self.project.resolve, self.attribute_choice)
        if effect is None:
            return "The game cannot do that."
        self.result = effect
        return None
