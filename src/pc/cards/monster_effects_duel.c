/* Monster effects in a duel (monster_effects_duel.h). */
#define D_8009B178_AS_SIDE_ARRAY
#include "monster_effects_duel.h"
#include "cards.h"
#include "game/card_constants.h"
#include "game/display_object.h"
#include "game/display_object_core.h"
#include "game/display_object_work_slots.h"
#include "game/duel_action_lock.h"
#include "game/duel_apply_card_object_flags.h"
#include "game/duel_card.h"
#include "game/duel_card_record_lifecycle.h"
#include "game/duel_check_quit_input.h"
#include "game/duel_effect.h"
#include "game/duel_effect_request.h"
#include "game/duel_grid.h"
#include "game/duel_magic_effect_dispatch.h"
#include "game/duel_scene_field_actions.h"
#include "game/duel_scene_state.h"
#include "game/duel_selection_layout.h"
#include "game/duel_side_state.h"
#include "game/duel_trap_resolution.h"
#include "game/sound.h"
#include "pc/debug/log.h"
#include "pc/mods/events.h"
#include "pc/mods/mods.h"
#include "tables.h"
#include <string.h>

/* The mods' event numbers what happened as the cards' "when" does. */
typedef char MonsterEffects_events_match
    [MEMORIES_MONSTER_SUMMON == MONSTER_WHEN_SUMMON &&
             MEMORIES_MONSTER_FLIP == MONSTER_WHEN_FLIP &&
             MEMORIES_MONSTER_DRAW == MONSTER_WHEN_DRAW &&
             MEMORIES_MONSTER_COMBAT == MONSTER_WHEN_COMBAT &&
             MEMORIES_MONSTER_DESTROYED == MONSTER_WHEN_DESTROYED &&
             MEMORIES_MONSTER_DESTROY_OPPONENT == MONSTER_WHEN_DESTROY_OPPONENT
         ? 1
         : -1];

/* The record of the trap that sprang; the field phase's frame timer
 * (src/unmatched.h). */
extern u8 D_8009B1B8;
extern u16 D_8009B162;
extern s8 D_8009B1B0[2];
extern s8 D_8009B208;
extern s8 D_8009B209;

#define S gMonsterEffects
#define PHASE_STARTUP 1
#define PHASE_HAND 4
#define PHASE_FIELD 5
#define PHASE_BATTLE 9
#define PAUSE_FRAMES 24
#define CHAIN_MAX 200   /* a guard: no field settles after this many in a row */
#define SE_HEAL 0x14    /* the LP recovery cards' sound */
#define SE_DAMAGE 0x1C  /* the direct damage cards' */
#define SE_BOOST 0x0C   /* a card put down */
#define SE_FLIP 0x0B    /* a card turned over */
#define CRUSH_CARD 661  /* whose removal a "destroy" plays */
#define SPLASH_HEAL 5   /* the LP recovery cards' effect request */
#define SPLASH_DAMAGE 6 /* the direct damage cards' */

/* The splashes' configs in the WA effect bank (src/overlays/duel_effects
 * effect_5.c, effect_6.c: D_8015B450 and D_8015B30C, at these addresses in
 * the NA bank), one per card in their families. The number each shows is
 * a word of its own entry (DuelEffect5Config.number, an s32 at +0x1C;
 * DuelEffect6Config.column_value, an s16 at +0x1C). */
#define HEAL_CONFIGS 0x8015B4BCu
#define HEAL_CONFIG_SIZE 0x24
#define DAMAGE_CONFIGS 0x8015B3D8u
#define DAMAGE_CONFIG_SIZE 0x1E
#define CONFIG_NUMBER 0x1C
static const short heal_steps[] = {
    DUEL_MOOYAN_CURRY_RECOVERY, DUEL_RED_MEDICINE_RECOVERY,
    DUEL_GOBLINS_SECRET_REMEDY_RECOVERY, DUEL_SOUL_OF_THE_PURE_RECOVERY,
    DUEL_DIAN_KETO_RECOVERY};
static const short damage_steps[] = {
    DUEL_SPARKS_DAMAGE, DUEL_HINOTAMA_DAMAGE, DUEL_FINAL_FLAME_DAMAGE,
    DUEL_OOKAZI_DAMAGE, DUEL_TREMENDOUS_FIRE_DAMAGE};

static int monster_zone(int record) {
  return record >= 0 && record < MONSTER_RECORDS && record % 15 >= 5 &&
         record % 15 < 10;
}
static int owner(int record) { return record / 15; }
static int phase(void) {
  return gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK;
}

static void trace(const char *what, int card, int record,
                  const MonsterEffect *effect) {
  LOG(LOG_DUEL_EFFECTS, "monster effects: %s card %d record %d side %d: %s %s",
      what, card, record, owner(record), MonsterEffect_WhenNames[effect->when],
      MonsterEffect_DoNames[effect->action]);
}

static void queue(int card, int record, int when) {
  const MonsterEffect *effects;
  int n = Cards_MonsterEffects(card, &effects), i;
  for (i = 0; i < n; i++) {
    if (effects[i].when != when)
      continue;
    if (S.count == MONSTER_QUEUE_MAX) {
      LOG(LOG_DUEL_EFFECTS,
          "monster effects: queue full: card %d's %s effect dropped", card,
          MonsterEffect_WhenNames[when]);
      return;
    }
    S.queue[S.count].card = (short)card;
    S.queue[S.count].record = (unsigned char)record;
    S.queue[S.count].effect = (unsigned char)i;
    S.count++;
    trace("queued", card, record, &effects[i]);
  }
}

void MonsterEffects_CardPlayed(int card, int side) {
  const MonsterEffect *effects;
  int n = Cards_CardEffects(card, &effects), i;
  /* The first monster-zone record of the owner's side (records are 15 a
   * side: owner()) stands in as the source, giving the shared target and
   * LP helpers the owning side. CardEffects_Read rejects the targets that
   * would take this made-up source for a real monster. */
  int record = side * 15 + 5;
  for (i = 0; i < n; i++) {
    if (S.count == MONSTER_QUEUE_MAX) {
      LOG(LOG_DUEL_EFFECTS,
          "card effects: queue full: card %d's effect dropped", card);
      return;
    }
    S.queue[S.count].card = (short)card;
    S.queue[S.count].record = (unsigned char)record;
    S.queue[S.count].effect = (unsigned char)(i | MONSTER_TRIGGER_CARD_EFFECT);
    S.count++;
  }
}

void MonsterEffects_TrapPlayed(int card, int side) {
  MonsterEffects_CardPlayed(card, side);
}

void MonsterEffects_AttackTrapPlayed(int card, int side) {
  const MonsterEffect *effects;
  /* Its effects run within the battle: hold the battle's own triggers
   * until they are done, and cancel it if they took a participant. */
  if (Cards_CardEffects(card, &effects))
    S.trap_pending = 1;
  MonsterEffects_CardPlayed(card, side);
}

void MonsterEffects_TrapPresented(int card, int side) {
  /* The same presentation serves the battle's attack traps and the
   * traps that answer a card put down or a magic card. */
  if (phase() == PHASE_BATTLE)
    MonsterEffects_AttackTrapPlayed(card, side);
  else
    MonsterEffects_CardPlayed(card, side);
}

void MonsterEffects_TrackBattleParticipants(int attacker, int defender) {
  S.trap_battle_record[0] = (short)attacker;
  S.trap_battle_record[1] = (short)defender;
}

int MonsterEffects_BattleAbortMask(void) {
  int aborted = S.battle_abort;
  S.battle_abort = 0;
  return aborted;
}

void MonsterEffects_Removed(int record) {
  int i;
  if (!S.trap_pending || phase() != PHASE_BATTLE)
    return;
  for (i = 0; i < 2; i++) {
    if (record == S.trap_battle_record[i])
      S.battle_abort |= 1 << i;
  }
}

static int trap_participant(int record) {
  int i;
  if (!S.trap_pending || phase() != PHASE_BATTLE)
    return 0;
  for (i = 0; i < 2; i++) {
    if (record == S.trap_battle_record[i] && !(S.battle_abort & (1 << i)))
      return 1;
  }
  return 0;
}

static int finish_attack_trap(void) {
  if (!S.trap_pending)
    return 0;
  /* Battle setup deactivates both field records while their separate
   * battle objects are shown. Only an explicit removal cancels combat. */
  S.trap_pending = 0;
  if (!S.battle_abort)
    MonsterEffects_Battle();
  return S.count || S.battle_life_count;
}

/* MEMORIES_EVENT_MONSTER, before: whether a code mod took the occasion
 * (the card's own effects are then skipped). */
static int announce(int card, int record, int when, unsigned phase) {
  MemoriesModEvent event = {MEMORIES_EVENT_MONSTER, 0, 0, 0, 0, 0, 0};
  event.phase = phase;
  event.a = card;
  event.b = record;
  event.c = when;
  Mods_Dispatch(&event);
  return event.handled;
}

/* Something happened to the monster `card` at `record`: the mods hear of
 * it, then its own effects for it are queued. */
static void occur(int card, int record, int when) {
  int taken = announce(card, record, when, MEMORIES_BEFORE);
  if (!taken)
    queue(card, record, when);
  announce(card, record, when, MEMORIES_AFTER);
}

/* Whether `effect` of the card at `source` reaches the monster `card` at
 * `target`. */
static int reaches(const MonsterEffect *effect, int source, int target,
                   int card) {
  switch (effect->target) {
  case MONSTER_TARGET_SELF:
    if (target != source)
      return 0;
    break;
  case MONSTER_TARGET_OWN:
    if (owner(target) != owner(source))
      return 0;
    break;
  case MONSTER_TARGET_OTHERS:
    if (owner(target) != owner(source) || target == source)
      return 0;
    break;
  case MONSTER_TARGET_OPPONENT:
    if (owner(target) == owner(source))
      return 0;
    break;
  case MONSTER_TARGET_ALL:
    break;
  case MONSTER_TARGET_BATTLE:
    /* The monster that attacked the flipped one, if it is still there. */
    if (source != S.defender || target != S.attacker ||
        D_801A7AD8[target].card_id != S.attacker_card)
      return 0;
    break;
  default:
    return 0;
  }
  /* A type past 127 is stored as a negative byte; only -1 (0xFF) is "any". */
  if (effect->type != -1 && Cards_Type(card) != (unsigned char)effect->type)
    return 0;
  if (effect->attribute >= 0 && Cards_Attribute(card) != effect->attribute)
    return 0;
  return 1;
}

/* How many times `effect` of the card at `source` is made: 1, or with a
 * "for_each" the face-up monsters it counts as the field was last looked
 * at (the battle's two as face up once it begins), the card too. */
static int times(const MonsterEffect *effect, int source) {
  int record, n = 0;
  if (effect->each == MONSTER_EACH_NONE)
    return 1;
  for (record = 0; record < MONSTER_RECORDS; record++) {
    if (!monster_zone(record) || !S.card[record] || !S.face_up[record])
      continue;
    if (effect->each == MONSTER_EACH_OWN && owner(record) != owner(source))
      continue;
    if (effect->each == MONSTER_EACH_OPPONENT && owner(record) == owner(source))
      continue;
    if (effect->each_type != -1 &&
        Cards_Type(S.card[record]) != (unsigned char)effect->each_type)
      continue;
    if (effect->each_attribute >= 0 &&
        Cards_Attribute(S.card[record]) != effect->each_attribute)
      continue;
    n++;
  }
  return n;
}

static int clamp(int value, int low, int high) {
  return value < low ? low : value > high ? high : value;
}

/* A spell/trap has no source monster. Its supported LP targets name the
 * owner or the other duelist explicitly; monster effects retain their
 * original owner/heal and opponent/damage meanings. */
static int life_side(const MonsterEffect *effect, int side, int card_effect) {
  if (!card_effect)
    return effect->action == MONSTER_DO_HEAL ? side : side ^ 1;
  return effect->target == MONSTER_TARGET_OPPONENT ? side ^ 1 : side;
}

/* A lasting boost: the modifiers equips use (both stats share
 * stat_modifier; defense_modifier is DEF's own on top), kept within what
 * their 16 bits and the stat cap leave room for. */
static void boost(DuelCardRecord *card, int attack, int defense) {
  int room = 2 * Tables_StatCapEither(), had_defense;
  if (room > TABLES_LIMIT_STAT_MAX)
    room = TABLES_LIMIT_STAT_MAX;
  had_defense = card->stat_modifier + card->defense_modifier;
  card->stat_modifier = (s16)clamp(card->stat_modifier + attack, -room, room);
  card->defense_modifier = (s16)clamp(
      clamp(had_defense + defense, -room, room) - card->stat_modifier,
      -TABLES_LIMIT_STAT_MAX, TABLES_LIMIT_STAT_MAX);
}

static void lend_turn(int side) {
  D_8009B1D5 = (u8)side;
  D_8009B1C8 = &D_800E9FF0[side];
  D_8009B22C = D_800907D8 + side * DUEL_FIELD_SIDE_GRID_SLOT_COUNT;
}

static void change_life(int side, int amount, int sound) {
  DuelSideState *state = &D_800E9FF0[side];
  int life = state->life_points.signed_value;
  if (amount > 0) {
    int ceiling = state->max_life_points > life ? state->max_life_points : life;
    state->life_points.unsigned_value =
        (u16)(life + amount > ceiling ? ceiling : life + amount);
    if (sound)
      SD_SEPlayFull(SE_HEAL);
  } else {
    life = Mods_DamageLife(side, life, -amount, 1);
    state->life_points.unsigned_value = (u16)(life < 0 ? 0 : life);
    if (sound)
      SD_SEPlayFull(SE_DAMAGE);
  }
}

static unsigned char *splash_number(int id, int config) {
  return (unsigned char *)(uintptr_t)(id == SPLASH_HEAL
                                          ? HEAL_CONFIGS +
                                                config * HEAL_CONFIG_SIZE
                                          : DAMAGE_CONFIGS +
                                                config * DAMAGE_CONFIG_SIZE) +
         CONFIG_NUMBER;
}

static int read_number(int id, const unsigned char *number) {
  if (id == SPLASH_HEAL) {
    int value;
    memcpy(&value, number, sizeof(value));
    return value;
  } else {
    short value;
    memcpy(&value, number, sizeof(value));
    return value;
  }
}

static void write_number(int id, unsigned char *number, int value) {
  if (id == SPLASH_HEAL) {
    memcpy(number, &value, sizeof(value));
  } else {
    short narrow = (short)value;
    memcpy(number, &narrow, sizeof(narrow));
  }
}

/* The borrowed config gets its own number back (unless the bank was
 * delivered again since, with its own). */
static void end_splash(void) {
  unsigned char *number;
  if (!S.splash_id)
    return;
  number = splash_number(S.splash_id, S.splash_config);
  if (read_number(S.splash_id, number) == S.splash_shown)
    write_number(S.splash_id, number, S.splash_saved);
  S.splash_id = 0;
}

/* The splash Dian Keto or Hinotama shows, with `amount` (a heal above 0,
 * damage below) for its number: the config of the family's card with the
 * largest amount up to it, as the size of the burst. The scene waits for
 * it (DuelScene_Update). 0 when no request was free. */
static int start_splash(int amount) {
  int id = amount > 0 ? SPLASH_HEAL : SPLASH_DAMAGE,
      size = amount > 0 ? amount : -amount, config = 0, saved;
  const short *steps = id == SPLASH_HEAL ? heal_steps : damage_steps;
  unsigned char *number;
  DuelEffectRequest *request;
  end_splash();
  while (config + 1 < 5 && steps[config + 1] <= size)
    config++;
  number = splash_number(id, config);
  saved = read_number(id, number);
  /* Not the retail bank's table (a mod's bank): no splash. */
  if (saved != steps[config] && saved != -steps[config])
    return 0;
  request = DuelEffect_CreateRequest(id);
  if (!request)
    return 0;
  request->field_00 = 0xA0;
  request->field_02 = 0x78;
  request->field_1A = (s16)config;
  if (size > 32767)
    size = 32767;
  S.splash_id = (unsigned char)id;
  S.splash_config = (unsigned char)config;
  S.splash_saved = saved;
  S.splash_shown = saved < 0 ? -size : size;
  write_number(id, number, S.splash_shown);
  SD_SEPlayFull(id == SPLASH_HEAL ? SE_HEAL : SE_DAMAGE);
  return 1;
}

/* A heal or damage effect: its splash first, then the LP change, as the
 * retail cards make it. */
static int life_effect(int side, int amount) {
  if (!start_splash(amount)) {
    change_life(side, amount, 1);
    S.pause = PAUSE_FRAMES;
    return 1;
  }
  S.life_pending = 1;
  S.life_side = (unsigned char)side;
  S.life_amount = amount;
  return 1;
}

/* One effect off the queue: 1 when the duel waits for it. */
static int resolve(void) {
  MonsterTrigger trigger = S.queue[0];
  const MonsterEffect *effects, *effect;
  int card_effect = trigger.effect & MONSTER_TRIGGER_CARD_EFFECT,
      index = trigger.effect & ~MONSTER_TRIGGER_CARD_EFFECT;
  int n = card_effect ? Cards_CardEffects(trigger.card, &effects)
                      : Cards_MonsterEffects(trigger.card, &effects);
  int side = owner(trigger.record), record, hit = 0, count;
  memmove(S.queue, S.queue + 1, (size_t)(--S.count) * sizeof(S.queue[0]));
  if (index >= n)
    return 0;
  effect = &effects[index];
  trace(card_effect ? "resolving card" : "resolving", trigger.card,
        trigger.record, effect);
  S.chain++;
  count = times(effect, trigger.record);
  if (effect->each)
    LOG(LOG_DUEL_EFFECTS, "monster effects: for each: %d counted", count);
  switch (effect->action) {
  case MONSTER_DO_MAGIC:
    /* Played as its owner would play it: on the other side's turn the
     * turn is lent to the owner for as long as the effect runs. */
    if (side != D_8009B1D5) {
      S.swapped = 1;
      S.saved_turn = D_8009B1D5;
      lend_turn(side);
    }
    S.running = 1;
    S.magic_card = effect->card;
    DuelEffect_StartRetailCardEffect(effect->card, 0);
    return 1;
  case MONSTER_DO_BOOST:
    if (!count)
      return 0;
    for (record = 0; record < MONSTER_RECORDS; record++) {
      DuelCardRecord *card = &D_801A7AD8[record];
      if (!monster_zone(record) || !(card->flags & DUEL_CARD_FLAG_OCCUPIED))
        continue;
      if (!reaches(effect, trigger.record, record, card->card_id))
        continue;
      boost(card, effect->attack * count, effect->defense * count);
      hit = 1;
    }
    if (!hit)
      return 0;
    SD_SEPlayFull(SE_BOOST);
    break;
  case MONSTER_DO_HEAL:
    return count ? life_effect(life_side(effect, side, card_effect),
                               effect->amount * count)
                 : 0;
  case MONSTER_DO_DAMAGE:
    return count ? life_effect(life_side(effect, side, card_effect),
                               -effect->amount * count)
                 : 0;
  case MONSTER_DO_DESTROY:
    /* Crush Card's removal, on the monsters chosen here
     * (MonsterEffects_RemovalTakes): it takes the other side's, as
     * its owner plays it. */
    S.destroy_mask = 0;
    for (record = 0; record < MONSTER_RECORDS; record++) {
      DuelCardRecord *card = &D_801A7AD8[record];
      if (!monster_zone(record) || (!(card->flags & DUEL_CARD_FLAG_OCCUPIED) &&
                                    !trap_participant(record)))
        continue;
      if (!reaches(effect, trigger.record, record, card->card_id))
        continue;
      /* Crush Card only walks the opposing physical row.  A data
       * effect which explicitly names its owner (or all monsters)
       * removes that row here. Lifted battle participants have no
       * field object for Crush Card to walk, so remove them here too;
       * the retail sequencer still presents the opposing row below. */
      if (trap_participant(record) || (card_effect && owner(record) == side)) {
        DuelCard_RemoveFromField(card);
        hit = 1;
      } else {
        S.destroy_mask |= 1u << record;
      }
    }
    if (!S.destroy_mask) {
      if (!hit)
        return 0;
      SD_SEPlayFull(SE_BOOST);
      S.pause = PAUSE_FRAMES;
      return 1;
    }
    if (side != D_8009B1D5) {
      S.swapped = 1;
      S.saved_turn = D_8009B1D5;
      lend_turn(side);
    }
    S.running = 1;
    S.magic_card = CRUSH_CARD;
    DuelEffect_StartRetailCardEffect(CRUSH_CARD, 0);
    return 1;
  }
  S.pause = PAUSE_FRAMES;
  return 1;
}

/* The field as it is now, against the last look: what happened since. */
static void look(void) {
  int record, side = D_8009B1D5, turns = D_800E9FF0[side].rank.turns_taken;
  if (S.flipped) {
    /* Flipped by the battle just over: its flip resolves first,
     * whether or not the battle destroyed it. */
    occur(S.flipped, S.defender, MONSTER_WHEN_FLIP);
    S.flipped = 0;
  }
  for (record = 0; record < MONSTER_RECORDS; record++) {
    const DuelCardRecord *card = &D_801A7AD8[record];
    int now = 0, up = 0, before = S.card[record];
    if (!monster_zone(record))
      continue;
    if (card->flags & DUEL_CARD_FLAG_OCCUPIED && Cards_Valid(card->card_id)) {
      now = card->card_id;
      up = !(card->flags & DUEL_CARD_FLAG_FACE_DOWN);
    }
    if (before && now != before && !S.placed[record] && !S.ritual) {
      occur(before, record, MONSTER_WHEN_DESTROYED);
    }
    if (now && up && (now != before || S.placed[record])) {
      /* A face-down play is no summon: the card's "flip" fires
       * when it is attacked instead (MonsterEffects_Battle). One
       * turned face up any other way -- attacking, Swords -- fires
       * nothing. */
      occur(now, record, MONSTER_WHEN_SUMMON);
    }
    if (now != before || S.placed[record]) {
      S.attacked_summon[record] = 0;
      if (!now)
        S.attacked_turn[record] = 0;
    }
    S.card[record] = (short)now;
    S.face_up[record] = (unsigned char)up;
    S.placed[record] = 0;
    S.battle_attack[record] = S.battle_defense[record] = 0;
  }
  S.ritual = 0;
  /* A battle's winner, still there, whose foe it destroyed. */
  for (record = 0; record < 2; record++) {
    int winner = S.fight[record], loser = S.fight[record ^ 1];
    if (monster_zone(winner) && monster_zone(loser) &&
        S.card[winner] == S.fight_card[record] &&
        S.card[loser] != S.fight_card[record ^ 1])
      occur(S.fight_card[record], winner, MONSTER_WHEN_DESTROY_OPPONENT);
  }
  S.fight[0] = S.fight[1] = 0;
  S.trap_battle_record[0] = -1;
  S.trap_battle_record[1] = -1;
  if (side != S.turn_side || turns != S.turn_count) {
    S.turn_side = (unsigned char)side;
    S.turn_count = (unsigned char)turns;
    memset(S.attacked_turn, 0, sizeof(S.attacked_turn));
    for (record = 15 * side + 5; record < 15 * side + 10; record++) {
      if (S.card[record] && S.face_up[record])
        occur(S.card[record], record, MONSTER_WHEN_DRAW);
    }
  }
}

static void reset(void) {
  end_splash();
  memset(&S, 0, sizeof(S));
  S.trap_battle_record[0] = -1;
  S.trap_battle_record[1] = -1;
}

int MonsterEffects_Update(void) {
  int now = phase(), waiting;
  if (now == PHASE_STARTUP) {
    if (S.ready)
      reset();
    return 0;
  }
  if (S.running == 1) {
    /* The first handler is done (most only clear the flags); the
     * second is the effect, as DuelScene_UpdateCardUse runs them, for
     * the same card (gDuel_wEffectCardID is set only when the retail
     * start ran, not when a code mod handled it). */
    S.running = 2;
    DuelEffect_StartRetailCardEffect(S.magic_card, 1);
    return 1;
  }
  if (S.running == 2) {
    S.running = 0;
    S.destroy_mask = 0;
    if (S.swapped) {
      S.swapped = 0;
      lend_turn(S.saved_turn);
    }
  }
  /* Past a splash: the scene ran no step while it was on screen. */
  end_splash();
  if (S.life_pending) {
    S.life_pending = 0;
    change_life(S.life_side, S.life_amount, 0);
  }
  /* A replacement trap only suppresses the battle where it sprang.  Do
   * not leave that marker behind after its queue, magic handler, splash or
   * pause has drained. */
  if (!S.count && !S.running && !S.life_pending && !S.pause)
    finish_attack_trap();
  if (S.pause) {
    S.pause--;
    return 1;
  }
  /* The battle that just began waits for them (MonsterEffects_Battle). */
  if (S.battle_life_count) {
    int side = S.battle_life_side[0], amount = S.battle_life[0];
    S.battle_life_count--;
    memmove(S.battle_life, S.battle_life + 1,
            S.battle_life_count * sizeof(S.battle_life[0]));
    memmove(S.battle_life_side, S.battle_life_side + 1, S.battle_life_count);
    return life_effect(side, amount);
  }
  /* Battle immunity (API 19): prevent battle destruction during battle or
   * return */
  if (now == PHASE_BATTLE || now == 12) {
    int att_rec = S.trap_battle_record[0];
    int def_rec = S.trap_battle_record[1];
    if (monster_zone(att_rec) && D_8009B1B0[0] == -1 &&
        MonsterEffects_HasBattleImmunity(att_rec)) {
      D_8009B1B0[0] = 1;
    }
    if (monster_zone(def_rec) && D_8009B1B0[1] == -1 &&
        MonsterEffects_HasBattleImmunity(def_rec)) {
      D_8009B1B0[1] = 1;
    }
    if (monster_zone(D_8009B208) &&
        MonsterEffects_HasBattleImmunity(D_8009B208)) {
      D_8009B208 = -1;
    }
    if (monster_zone(D_8009B209) &&
        MonsterEffects_HasBattleImmunity(D_8009B209)) {
      D_8009B209 = -1;
    }
  }
  /* Only as a hand or field phase begins, before its first step: within
   * one the field is not settled (the hand lifts a field monster for a
   * fusion frames before placement takes it). Every way back to them --
   * a card put down, a battle, a card's effect, a new turn -- starts one. */
  if (((now != PHASE_HAND && now != PHASE_FIELD) && !S.count) ||
      ((gDuel_wSceneStateFlags & DUEL_SCENE_FLAG_INITIALIZED) &&
       !(now == PHASE_BATTLE && S.count)) ||
      gDuel_bEffectState || gDuel_wCardEffectFlags || gDuel_bQuitDialogState)
    return 0;
  /* A side out of LP: the field phase ends the duel; no more effects. */
  if (!D_800E9FF0[0].life_points.signed_value ||
      !D_800E9FF0[1].life_points.signed_value) {
    S.count = 0;
    if (now == PHASE_HAND)
      gDuel_wSceneStateFlags = PHASE_FIELD;
    return 0;
  }
  if (!S.ready) {
    reset();
    S.ready = 1;
    S.turn_side = (unsigned char)D_8009B1D5;
    S.turn_count = D_800E9FF0[D_8009B1D5].rank.turns_taken;
    look();
    S.count = 0;
    return 0;
  }
  /* A trap springs in battle. Its queued card effects may run there, but
   * that is not a stable field boundary, so do not infer summons,
   * destruction or draw triggers from this intermediate board. */
  if (now != PHASE_HAND && now != PHASE_FIELD) {
    waiting = 0;
    while (S.count && !waiting)
      waiting = resolve();
    if (!waiting && !S.count && finish_attack_trap())
      return 1;
    return waiting;
  }
  look();
  /* A code mod may have started a card effect of its own for what it
   * heard: the duel waits for it, and the field is looked at again. */
  if (gDuel_wCardEffectFlags)
    return 1;
  if (S.count && S.chain >= CHAIN_MAX) {
    LOG(LOG_DUEL_EFFECTS,
        "monster effects: %d effects in a row: the rest are dropped", S.chain);
    S.count = 0;
  }
  /* One that finds nothing to do (a boost or destroy with no monster
   * left) gives way to the next at once: past this look the phase goes
   * on, and the rest would wait for the player's next move. */
  waiting = 0;
  while (S.count && !waiting)
    waiting = resolve();
  if (!waiting && !S.count)
    S.chain = 0;
  return waiting;
}

int MonsterEffects_PlayFaceUp(int card) {
  const MonsterEffect *effects;
  int n = Cards_MonsterEffects(card, &effects), i, summon = 0, flip = 0,
      seen = 0;
  for (i = 0; i < n; i++) {
    summon |= effects[i].when == MONSTER_WHEN_SUMMON;
    flip |= effects[i].when == MONSTER_WHEN_FLIP;
    seen |= effects[i].when == MONSTER_WHEN_DRAW ||
            effects[i].when == MONSTER_WHEN_FACE_UP;
  }
  return summon || (seen && !flip);
}

int MonsterEffects_RemovalTakes(int record) {
  if (!S.destroy_mask)
    return -1;
  return record >= 0 && record < MONSTER_RECORDS &&
         (S.destroy_mask >> record & 1);
}

void MonsterEffects_FilterTargets(unsigned *objects) {
  int i, n = 0;
  if (!S.destroy_mask)
    return;
  for (i = 0; objects[i]; i++) {
    const DisplayObject *object = (const DisplayObject *)(uintptr_t)objects[i];
    if (object->field_6A < MONSTER_RECORDS &&
        (S.destroy_mask >> object->field_6A & 1))
      objects[n++] = objects[i];
  }
  objects[n] = 0;
}

void MonsterEffects_Placed(int record, int equip) {
  /* A card put back on its own zone (the 0x4000 placement, which an equip
   * and a fusion onto a field monster both take) is a new card only when
   * it is another card: the fusion's result, not the equipped monster. */
  if (monster_zone(record) &&
      (!equip || D_801A7AD8[record].card_id != S.card[record]))
    S.placed[record] = 1;
}

void MonsterEffects_EffectStarted(int ritual) {
  if (ritual)
    S.ritual = 1;
}

void MonsterEffects_Battle(void) {
  const DisplayObject *attacker = (const DisplayObject *)D_800E9EF0[0];
  const DisplayObject *defender = (const DisplayObject *)D_800E9EF0[1];
  int records[2], i, played = 0;
  if (!S.ready || D_8009B22A || S.trap_pending || !attacker)
    return;
  records[0] = attacker->field_6A;
  records[1] = defender ? defender->field_6A : -1;
  if (monster_zone(records[1])) {
    S.attacked_summon[records[1]]++;
    S.attacked_turn[records[1]]++;
    S.attacked_duel[records[1]]++;
  }
  /* A face-down defender is flipped by the battle: as Yu-Gi-Oh! has it,
   * its flip resolves after the damage is worked out (with the card as
   * it is), at the next look, before what the battle destroyed. */
  S.flipped = 0;
  if (monster_zone(records[1]) && monster_zone(records[0]) &&
      (D_8009B178[1] & DUEL_CARD_FLAG_FACE_DOWN)) {
    S.flipped = D_801A7AD8[records[1]].card_id;
    S.defender = (unsigned char)records[1];
    S.attacker = (unsigned char)records[0];
    S.attacker_card = D_801A7AD8[records[0]].card_id;
  }
  /* Who won it is seen at the next look (DESTROY_OPPONENT). */
  for (i = 0; i < 2; i++) {
    S.fight[i] = (unsigned char)(monster_zone(records[i]) ? records[i] : 0);
    S.fight_card[i] =
        (short)(monster_zone(records[i]) ? D_801A7AD8[records[i]].card_id : 0);
  }
  /* Face up from here: their face_up boosts count in the battle, and
   * they are counted for a "for_each". */
  for (i = 0; i < 2; i++)
    if (monster_zone(records[i]) &&
        D_801A7AD8[records[i]].card_id == S.card[records[i]])
      S.face_up[records[i]] = 1;
  for (i = 0; i < 2; i++) {
    const MonsterEffect *effects;
    int record = records[i], other = records[i ^ 1], card, n, e, count;
    if (!monster_zone(record))
      continue;
    card = D_801A7AD8[record].card_id;
    n = announce(card, record, MONSTER_WHEN_COMBAT, MEMORIES_BEFORE)
            ? 0
            : Cards_MonsterEffects(card, &effects);
    for (e = 0; e < n; e++) {
      const MonsterEffect *effect = &effects[e];
      if (effect->when != MONSTER_WHEN_COMBAT)
        continue;

      trace("battle", card, record, effect);
      count = times(effect, record);
      if (!count)
        continue;
      played = 1;
      if (effect->action == MONSTER_DO_HEAL ||
          effect->action == MONSTER_DO_DAMAGE) {
        /* Made with its splash at the next frames, before the
         * battle goes on (MonsterEffects_Update). */
        int amount = (effect->action == MONSTER_DO_HEAL ? effect->amount
                                                        : -effect->amount) *
                     count;
        int side = effect->action == MONSTER_DO_HEAL ? owner(record)
                                                     : owner(record) ^ 1;
        if (S.battle_life_count <
            (int)(sizeof(S.battle_life) / sizeof(S.battle_life[0]))) {
          S.battle_life_side[S.battle_life_count] = (unsigned char)side;
          S.battle_life[S.battle_life_count++] =
              (short)clamp(amount, -32767, 32767);
        } else {
          change_life(side, amount, 0);
        }
      } else if (effect->action == MONSTER_DO_BOOST) {
        int to = effect->target == MONSTER_TARGET_BATTLE ? other : record;
        if (!monster_zone(to) ||
            !reaches(&(MonsterEffect){.target = MONSTER_TARGET_ALL,
                                      .type = effect->type,
                                      .attribute = effect->attribute},
                     record, to, D_801A7AD8[to].card_id))
          continue;
        S.battle_attack[to] =
            (short)clamp(S.battle_attack[to] + effect->attack * count,
                         -TABLES_LIMIT_STAT_MAX, TABLES_LIMIT_STAT_MAX);
        S.battle_defense[to] =
            (short)clamp(S.battle_defense[to] + effect->defense * count,
                         -TABLES_LIMIT_STAT_MAX, TABLES_LIMIT_STAT_MAX);
      }
    }
    announce(card, record, MONSTER_WHEN_COMBAT, MEMORIES_AFTER);
  }
  if (played)
    SD_SEPlayFull(SE_BOOST);
}

static int s_evaluating_stats = 0;

static int evaluate_condition(int record, const MonsterCondition *cond) {
  int r;
  switch (cond->type) {
  case MONSTER_COND_NAMED_CARD: {
    int count = 0;
    for (r = 0; r < MONSTER_RECORDS; r++) {
      if (!monster_zone(r) || !S.card[r] || !S.face_up[r])
        continue;
      if (cond->whose == MONSTER_EACH_OWN && owner(r) != owner(record))
        continue;
      if (cond->whose == MONSTER_EACH_OPPONENT && owner(r) == owner(record))
        continue;
      if (S.card[r] == cond->card)
        count++;
    }
    return count > 0;
  }
  case MONSTER_COND_FIRST_ATTACKED:

  case MONSTER_COND_DESTROYED_AFTER_2_ATTACKS:
  case MONSTER_COND_DESTROYED_AFTER_3_ATTACKS: {
    int now = phase(), in_battle;
    int target_count = 1;
    int count = 0;

    if (cond->type == MONSTER_COND_DESTROYED_AFTER_2_ATTACKS)
      target_count = 2;
    else if (cond->type == MONSTER_COND_DESTROYED_AFTER_3_ATTACKS)
      target_count = 3;

    /* Must be in combat and the monster must be the defender being attacked.
     * If the monster attacks, the condition is false and the effect does not
     * apply. */
    in_battle = (now == PHASE_BATTLE || now == 12);
    if (!in_battle)
      return 0;

    if (record != S.trap_battle_record[1])
      return 0;

    if (cond->reset == MONSTER_RESET_SUMMON)
      count = S.attacked_summon[record];
    else if (cond->reset == MONSTER_RESET_TURN)
      count = S.attacked_turn[record];
    else if (cond->reset == MONSTER_RESET_DUEL)
      count = S.attacked_duel[record];

    return count <= target_count && count > 0;
  }
  case MONSTER_COND_STAT_THRESHOLD: {
    int attack = 0, defense = 0, val, packed;
    DuelCardRecord *card = &D_801A7AD8[record];

    if (cond->stat == MONSTER_STAT_LEVEL) {
      val = Cards_Level(card->card_id);
    } else {
      s_evaluating_stats = 1;
      packed = Duel_CalcCardStats(card);
      s_evaluating_stats = 0;
      attack = packed & 0xFFFF;
      defense = (unsigned int)packed >> 16;
      val = (cond->stat == MONSTER_STAT_DEF) ? defense : attack;
    }

    switch (cond->op) {
    case MONSTER_OP_LT:
      return val < cond->value;
    case MONSTER_OP_LE:
      return val <= cond->value;
    case MONSTER_OP_GT:
      return val > cond->value;
    case MONSTER_OP_GE:
      return val >= cond->value;
    default:
      return 0;
    }
  }
  case MONSTER_COND_LOWEST_ATK: {
    int my_atk;
    s_evaluating_stats = 1;
    my_atk = Duel_CalcCardStats(&D_801A7AD8[record]) & 0xFFFF;
    for (r = 0; r < MONSTER_RECORDS; r++) {
      int other_atk;
      if (r == record || !monster_zone(r) || !S.card[r] || !S.face_up[r])
        continue;
      other_atk = Duel_CalcCardStats(&D_801A7AD8[r]) & 0xFFFF;
      if (other_atk <= my_atk) {
        s_evaluating_stats = 0;
        return 0;
      }
    }
    s_evaluating_stats = 0;
    return 1;
  }
  case MONSTER_COND_HIGHEST_ATK: {
    int my_atk;
    s_evaluating_stats = 1;
    my_atk = Duel_CalcCardStats(&D_801A7AD8[record]) & 0xFFFF;
    for (r = 0; r < MONSTER_RECORDS; r++) {
      int other_atk;
      if (r == record || !monster_zone(r) || !S.card[r] || !S.face_up[r])
        continue;
      other_atk = Duel_CalcCardStats(&D_801A7AD8[r]) & 0xFFFF;
      if (other_atk >= my_atk) {
        s_evaluating_stats = 0;
        return 0;
      }
    }
    s_evaluating_stats = 0;
    return 1;
  }
  default:
    return 1;
  }
}

int MonsterEffects_ConditionsMet(int record, const MonsterEffect *effect) {
  int i;
  if (!effect->cond_count)
    return 1;
  if (effect->cond_mode == MONSTER_COND_MODE_ANY) {
    for (i = 0; i < effect->cond_count; i++) {
      if (evaluate_condition(record, &effect->conditions[i]))
        return 1;
    }
    return 0;
  }
  for (i = 0; i < effect->cond_count; i++) {
    if (!evaluate_condition(record, &effect->conditions[i]))
      return 0;
  }
  return 1;
}

int MonsterEffects_HasBattleImmunity(int record) {
  const MonsterEffect *effects;
  int n, e;
  if (!monster_zone(record) || !S.card[record] || !S.face_up[record])
    return 0;
  n = Cards_MonsterEffects(S.card[record], &effects);
  for (e = 0; e < n; e++) {
    if (effects[e].when != MONSTER_WHEN_FACE_UP)
      continue;
    if (effects[e].action != MONSTER_DO_BATTLE_IMMUNITY)
      continue;
    if (MonsterEffects_ConditionsMet(record, &effects[e]))
      return 1;
  }
  return 0;
}

void MonsterEffects_Stats(const void *pointer, int *attack, int *defense) {
  const DuelCardRecord *card = pointer;
  int target = (int)(card - D_801A7AD8), source;
  if (!S.ready || !monster_zone(target) || s_evaluating_stats)
    return;
  s_evaluating_stats = 1;
  if (phase() == PHASE_BATTLE) {
    *attack += S.battle_attack[target];
    *defense += S.battle_defense[target];
  }
  for (source = 0; source < MONSTER_RECORDS; source++) {
    const MonsterEffect *effects;
    int n, e;
    if (!S.card[source] || !S.face_up[source])
      continue;
    n = Cards_MonsterEffects(S.card[source], &effects);
    for (e = 0; e < n; e++) {
      int count;
      if (effects[e].when != MONSTER_WHEN_FACE_UP ||
          !reaches(&effects[e], source, target, card->card_id))
        continue;
      if (!MonsterEffects_ConditionsMet(source, &effects[e]))
        continue;
      /* Counted as it is asked: the boost follows the field. */
      count = times(&effects[e], source);
      *attack += effects[e].attack * count;
      *defense += effects[e].defense * count;
    }
  }
  s_evaluating_stats = 0;
}
