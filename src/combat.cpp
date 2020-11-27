/*
    GSP for the Taurion blockchain game
    Copyright (C) 2019-2020  Autonomous Worlds Ltd

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "combat.hpp"

#include "modifier.hpp"

#include "database/account.hpp"
#include "database/building.hpp"
#include "database/character.hpp"
#include "database/dex.hpp"
#include "database/fighter.hpp"
#include "database/ongoing.hpp"
#include "database/region.hpp"
#include "database/target.hpp"
#include "hexagonal/coord.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace pxd
{

namespace
{

/**
 * Chance (in percent) that an inventory position inside a destroyed building
 * will drop on the ground instead of being destroyed.
 */
constexpr unsigned BUILDING_INVENTORY_DROP_PERCENT = 30;

/**
 * Chance (in percent) that an equipped fitment of a destroyed character
 * will be dropped as loot rather than destroyed.
 */
constexpr unsigned EQUIPPED_FITMENT_DROP_PERCENT = 20;

/**
 * Modifications to combat-related stats.
 */
struct CombatModifier
{

  /** Modification of combat damage.  */
  StatModifier damage;

  /** Modifiction of range.  */
  StatModifier range;

  /** Modification of hit chance for attacks of this fighter.  */
  StatModifier hitChance;

  CombatModifier () = default;
  CombatModifier (CombatModifier&&) = default;

  CombatModifier (const CombatModifier&) = delete;
  void operator= (const CombatModifier&) = delete;

};

/**
 * Computes the modifier to apply for a given entity (composed of base
 * modifiers, low-HP boosts and effects).
 */
void
ComputeModifier (const CombatEntity& f, CombatModifier& mod)
{
  mod.damage = StatModifier ();
  mod.range = StatModifier ();
  mod.hitChance = StatModifier ();

  const auto& cd = f.GetCombatData ();
  const auto& hp = f.GetHP ();
  const auto& maxHp = f.GetRegenData ().max_hp ();

  for (const auto& b : cd.low_hp_boosts ())
    {
      /* hp / max > p / 100 iff 100 hp > p max */
      if (100 * hp.armour () > b.max_hp_percent () * maxHp.armour ())
        continue;

      mod.damage += b.damage ();
      mod.range += b.range ();
    }

  const auto& eff = f.GetEffects ();
  mod.range += eff.range ();
  mod.hitChance += cd.hit_chance_modifier ();
  mod.hitChance += eff.hit_chance ();
}

} // anonymous namespace

TargetKey::TargetKey (const proto::TargetId& id)
{
  CHECK (id.has_id ());
  first = id.type ();
  second = id.id ();
}

proto::TargetId
TargetKey::ToProto () const
{
  proto::TargetId res;
  res.set_type (first);
  res.set_id (second);
  return res;
}

/* ************************************************************************** */

namespace
{

/**
 * Wrapper around ProcessL1Targets, which filters out targets in a no-combat
 * safe zone as well as the fighter itself.  If enemies is true, it will look
 * for enemies and not friendlies; if enemies is false, the other way round.
 * If the fighter is affected by a mentecon, it will instead just ignore the
 * enemies flag and always look for enemies and friendlies alike.
 */
void
ProcessCombatTargets (const TargetFinder& targets, const Context& ctx,
                      const CombatEntity& f,
                      const HexCoord& centre, const HexCoord::IntT range,
                      const bool enemies,
                      const TargetFinder::ProcessingFcn& cb)
{
  const TargetKey myId(f.GetIdAsTarget ());

  const bool mentecon = f.GetEffects ().mentecon ();
  const bool lookForEnemies = enemies || mentecon;
  const bool lookForFriendlies = !enemies || mentecon;

  targets.ProcessL1Targets (centre, range, f.GetFaction (),
                            lookForEnemies, lookForFriendlies,
    [&] (const HexCoord& c, const proto::TargetId& id)
    {
      if (ctx.Map ().SafeZones ().IsNoCombat (c))
        {
          VLOG (2)
              << "Ignoring fighter in no-combat zone:\n"
              << id.DebugString ();
          return;
        }

      if (myId == TargetKey (id))
        return;

      cb (c, id);
    });
}

/**
 * Helper class for performing target finding.  It holds some general
 * context, and also manages parallel processing.
 */
class TargetFindingProcessor
{

private:

  class TargetingResult;

  BuildingsTable buildings;
  CharacterTable characters;
  FighterTable fighters;
  const TargetFinder targets;

  xaya::Random& rnd;
  const Context& ctx;

  /**
   * Runs target finding for the normal attacks, setting (or clearing)
   * the target field in the result.
   */
  void SelectNormalTarget (const CombatModifier& mod,
                           TargetingResult& res) const;

  /**
   * Runs target finding for friendlies in range of a friendly attack, if any.
   * This sets (or unsets) the friendly-targets flag in the result.
   */
  void SelectFriendlyTargets (const CombatModifier& mod,
                              TargetingResult& res) const;

  /**
   * Runs target selection for one fighter entity.  This does most of the
   * processing, but does not modify the handle.  Instead it returns
   * the associated TargetingResult.
   */
  TargetingResult SelectTarget (FighterTable::Handle f);

  /**
   * Applies changes specified in a TargetingResult to the contained handle,
   * and then destroys (writes to the database) the handle.
   */
  void Finalise (TargetingResult res);

public:

  TargetFindingProcessor (Database& db, xaya::Random& r, const Context& c)
    : buildings(db), characters(db),
      fighters(buildings, characters),
      targets(db),
      rnd(r), ctx(c)
  {}

  /**
   * Runs all processing.
   */
  void ProcessAll ();

};

/**
 * Helper struct that holds the result of target finding for a particular
 * potential attacker.  It is used to pass the result from the processor
 * thread back to when it will be executed in a single thread.
 */
struct TargetFindingProcessor::TargetingResult
{

  /**
   * The underlying fighter handle, which needs to be modified still
   * according to the update.
   */
  FighterTable::Handle f;

  /** The fighter's ID.  We use this to sort them for processing.  */
  TargetKey id;

  /**
   * Array of closest enemy targets found, if any.  From this list, we will
   * randomly pick one as "the" target.
   */
  std::vector<proto::TargetId> enemyTargets;

  /** Whether or not there is a friendly target in range.  */
  bool hasFriendlyTarget;

  TargetingResult () = default;

  explicit TargetingResult (FighterTable::Handle h)
    : f(std::move (h)), id(f->GetIdAsTarget ())
  {}

  TargetingResult (TargetingResult&&) = default;
  TargetingResult& operator= (TargetingResult&&) = default;

  TargetingResult (const TargetingResult&) = delete;
  void operator= (const TargetingResult&) = delete;

};

void
TargetFindingProcessor::SelectNormalTarget (const CombatModifier& mod,
                                            TargetingResult& res) const
{
  CHECK (res.enemyTargets.empty ());
  const HexCoord pos = res.f->GetCombatPosition ();

  HexCoord::IntT range = res.f->GetAttackRange (false);
  if (range == CombatEntity::NO_ATTACKS)
    {
      VLOG (1) << "Fighter at " << pos << " has no attacks";
      return;
    }
  CHECK_GE (range, 0);
  range = mod.range (range);

  HexCoord::IntT closestRange;

  ProcessCombatTargets (targets, ctx, *res.f, pos, range, true,
    [&] (const HexCoord& c, const proto::TargetId& id)
    {
      const HexCoord::IntT curDist = HexCoord::DistanceL1 (pos, c);
      if (res.enemyTargets.empty () || curDist < closestRange)
        {
          closestRange = curDist;
          res.enemyTargets = {id};
          return;
        }

      if (curDist == closestRange)
        {
          res.enemyTargets.push_back (id);
          return;
        }

      CHECK_GT (curDist, closestRange);
    });

  if (res.enemyTargets.empty ())
    VLOG (1) << "Found no targets around " << pos;
  else
    VLOG (1)
        << "Found " << res.enemyTargets.size () << " targets in closest range "
        << closestRange << " around " << pos;
}

void
TargetFindingProcessor::SelectFriendlyTargets (const CombatModifier& mod,
                                               TargetingResult& res) const
{
  const HexCoord pos = res.f->GetCombatPosition ();

  HexCoord::IntT range = res.f->GetAttackRange (true);
  if (range == CombatEntity::NO_ATTACKS)
    {
      VLOG (2) << "Fighter at " << pos << " has no friendly attacks";
      res.hasFriendlyTarget = false;
      return;
    }
  CHECK_GE (range, 0);
  range = mod.range (range);

  res.hasFriendlyTarget = false;
  ProcessCombatTargets (targets, ctx, *res.f, pos, range, false,
    [&] (const HexCoord& c, const proto::TargetId& id)
    {
      res.hasFriendlyTarget = true;
    });

  if (res.hasFriendlyTarget)
    VLOG (1)
        << "Found at least one friendly target in range for "
        << res.f->GetIdAsTarget ().DebugString ();
  else
    VLOG (1)
        << "No friendlies in range for "
        << res.f->GetIdAsTarget ().DebugString ();
}

TargetFindingProcessor::TargetingResult
TargetFindingProcessor::SelectTarget (FighterTable::Handle f)
{
  TargetingResult res(std::move (f));

  if (ctx.Map ().SafeZones ().IsNoCombat (res.f->GetCombatPosition ()))
    {
      VLOG (1)
          << "Not selecting targets for fighter in no-combat zone:\n"
          << res.f->GetIdAsTarget ().DebugString ();
      CHECK (res.enemyTargets.empty ());
      res.hasFriendlyTarget = false;
      return res;
    }

  CombatModifier mod;
  ComputeModifier (*res.f, mod);

  SelectNormalTarget (mod, res);
  SelectFriendlyTargets (mod, res);

  return res;
}

void
TargetFindingProcessor::Finalise (TargetingResult res)
{
  if (res.enemyTargets.empty ())
    res.f->ClearTarget ();
  else
    {
      const unsigned ind = rnd.NextInt (res.enemyTargets.size ());
      res.f->SetTarget (res.enemyTargets[ind]);
    }

  res.f->SetFriendlyTargets (res.hasFriendlyTarget);
}

void
TargetFindingProcessor::ProcessAll ()
{
  fighters.ProcessWithAttacks ([this] (FighterTable::Handle f)
    {
      Finalise (SelectTarget (std::move (f)));
    });
}

} // anonymous namespace

void
FindCombatTargets (Database& db, xaya::Random& rnd, const Context& ctx)
{
  TargetFindingProcessor proc(db, rnd, ctx);
  proc.ProcessAll ();
}

/* ************************************************************************** */

unsigned
BaseHitChance (const proto::CombatData& target,
               const proto::Attack::Damage& dmg)
{
  if (!target.has_target_size () || !dmg.has_weapon_size ())
    return 100;

  if (target.target_size () >= dmg.weapon_size ())
    return 100;

  CHECK_GT (target.target_size (), 0);
  CHECK_GT (dmg.weapon_size (), 0);

  return (target.target_size () * 100) / dmg.weapon_size ();
}

namespace
{

/**
 * Helper class to perform the damage-dealing processing step.
 */
class DamageProcessor
{

private:

  DamageLists& dl;
  xaya::Random& rnd;
  const Context& ctx;

  BuildingsTable buildings;
  CharacterTable characters;
  FighterTable fighters;
  TargetFinder targets;

  /**
   * Modifiers to combat stats for all fighters that will deal damage.  This
   * is filled in (e.g. from their low-HP boosts) before actual damaging starts,
   * and is used to make the damaging independent of processing order.  This is
   * especially important so that HP changes do not influence low-HP boosts.
   */
  std::map<TargetKey, CombatModifier> modifiers;

  /**
   * Combat effects that are being applied by this round of damage to
   * the given targets.  This is accumulated here so that the original
   * effects are unaffected, and only later written back to the fighters
   * after all damaging is done.  This ensures that we do not take current
   * changes into effect right now in a messy way, e.g. for self-destruct
   * rounds (which do not rely on "modifiers" but recompute them).
   */
  std::map<TargetKey, proto::CombatEffects> newEffects;

  /**
   * For each target that was attacked with a gain_hp attack, we store all
   * attackers and how many HP they drained.  We give them those HP back
   * only later, after processing all damage and kills (i.e. HP you gained
   * in one round do not prevent you from dying in that round).  Also, if
   * a single target was drained by more than one attacker and ends up with
   * no HP left, noone gets any of them.
   *
   * DealDamage fills this in whenever it processes an attack that has
   * gain_hp set.
   *
   * This system ensures that processing is independent of the order in which
   * the individual attackers are handled; if two people drained the same
   * target and it ends up without HP (so that the order might have mattered),
   * then noone gets any.
   */
  std::map<TargetKey, std::map<TargetKey, proto::HP>> gainHpDrained;

  /**
   * The list of dead targets.  We use this to avoid giving out fame for
   * kills of already-dead targets in later rounds of self-destruct.  The list
   * being built up during a round of damage is a temporary, that gets put
   * here only after the round.
   */
  std::set<TargetKey> alreadyDead;

  /**
   * Performs a random roll to determine the damage a particular attack does.
   * The min/max damage is modified according to the stats modifier.
   */
  unsigned RollAttackDamage (const proto::Attack::Damage& attack,
                             const StatModifier& mod);

  /**
   * Checks (possibly with a random roll) whether or not an attack is supposed
   * to hit the given target.
   */
  bool AttackHitsTarget (const CombatEntity& target,
                         const proto::Attack::Damage& attack,
                         const StatModifier& attackerHitMod);

  /**
   * Applies a fixed given amount of damage to a given attack target.  Adds
   * the target into newDead if it is now dead.  This is a more low-level
   * variant that does not handle gain_hp.  Returns the damage actually
   * done to the target's shield and armour.
   */
  proto::HP ApplyDamage (unsigned dmg, const CombatEntity& attacker,
                         const proto::Attack::Damage& pb,
                         const CombatModifier& attackerMod,
                         CombatEntity& target, std::set<TargetKey>& newDead);

  /**
   * Applies a fixed amount of damage to a given target.  This is the
   * high-level variant that also handles gain_hp and is used for real attacks,
   * but not self-destruct damage.
   */
  void ApplyDamage (unsigned dmg, const CombatEntity& attacker,
                    const proto::Attack& attack,
                    const CombatModifier& attackerMod,
                    CombatEntity& target, std::set<TargetKey>& newDead);

  /**
   * Applies combat effects (non-damage) to a target.  They are not saved
   * directly to the target for now, but accumulated in newEffects.
   */
  void ApplyEffects (const proto::Attack& attack, const CombatEntity& target);

  /**
   * Deals damage for one fighter with a target to the respective target
   * (or any AoE targets).  Only processes attacks with gain_hp equal to
   * the argument value passed in.
   */
  void DealDamage (FighterTable::Handle f, bool forGainHp,
                   std::set<TargetKey>& newDead);

  /**
   * Processes all damage the given fighter does due to self-destruct
   * abilities when killed.
   */
  void ProcessSelfDestructs (FighterTable::Handle f,
                             std::set<TargetKey>& newDead);

public:

  explicit DamageProcessor (Database& db, DamageLists& lst,
                            xaya::Random& r, const Context& c)
    : dl(lst), rnd(r), ctx(c),
      buildings(db), characters(db),
      fighters(buildings, characters),
      targets(db)
  {}

  /**
   * Runs the full damage processing step.
   */
  void Process ();

  /**
   * Returns the list of killed fighters.
   */
  const std::set<TargetKey>&
  GetDead () const
  {
    return alreadyDead;
  }

};

unsigned
DamageProcessor::RollAttackDamage (const proto::Attack::Damage& dmg,
                                   const StatModifier& mod)
{
  const auto minDmg = mod (dmg.min ());
  const auto maxDmg = mod (dmg.max ());

  CHECK_LE (minDmg, maxDmg);
  const auto n = maxDmg - minDmg + 1;
  return minDmg + rnd.NextInt (n);
}

bool
DamageProcessor::AttackHitsTarget (const CombatEntity& target,
                                   const proto::Attack::Damage& attack,
                                   const StatModifier& attackerHitMod)
{
  int chance = BaseHitChance (target.GetCombatData (), attack);
  chance = attackerHitMod (chance);

  /* Do not do a random roll at all if the chance is fully 0 or 100.  */
  if (chance <= 0)
    return false;
  if (chance >= 100)
    return true;

  return rnd.ProbabilityRoll (chance, 100);
}

/**
 * Computes the damage done vs shield and armour, given the total
 * damage roll and the remaining shield and armour of the target.
 * The Damage proto is used for the shield/armour damage percentages
 * (if there are any).
 */
proto::HP
ComputeDamage (unsigned dmg, const proto::Attack::Damage& pb,
               const proto::HP& hp)
{
  proto::HP done;

  const unsigned shieldPercent
      = (pb.has_shield_percent () ? pb.shield_percent () : 100);
  const unsigned armourPercent
      = (pb.has_armour_percent () ? pb.armour_percent () : 100);

  /* To take the shield vs armour percentages into account, we first
     multiply the base damage with the corresponding fraction, then deduct
     it from the shield, and then divide the damage done by the fraction again
     to determine how much base damage (if any) is left to apply to the armour.
     There we do the same.

     In the integer math, we always round down; this ensures that we will
     never get more than the original base damage as "damage done".  */

  const unsigned availableForShield = (dmg * shieldPercent) / 100;
  done.set_shield (std::min (availableForShield, hp.shield ()));

  /* If we did not yet exhaust the shield, do not try to damage the armour
     even if some "base damage" is left.  This can happen for instance if
     the shield damage was discounted heavily by the shield percent.  */
  CHECK_LE (done.shield (), hp.shield ());
  if (done.shield () < hp.shield ())
    return done;

  if (done.shield () > 0)
    {
      const unsigned baseDoneShield = (done.shield () * 100) / shieldPercent;
      CHECK_LE (baseDoneShield, dmg);
      dmg -= baseDoneShield;
    }

  const unsigned availableForArmour = (dmg * armourPercent) / 100;
  done.set_armour (std::min (availableForArmour, hp.armour ()));

  if (done.armour () > 0)
    {
      const unsigned baseDoneArmour = (done.armour () * 100) / armourPercent;
      CHECK_LE (baseDoneArmour, dmg);
    }

  return done;
}

proto::HP
DamageProcessor::ApplyDamage (unsigned dmg, const CombatEntity& attacker,
                              const proto::Attack::Damage& pb,
                              const CombatModifier& attackerMod,
                              CombatEntity& target,
                              std::set<TargetKey>& newDead)
{
  CHECK (!ctx.Map ().SafeZones ().IsNoCombat (target.GetCombatPosition ()));

  /* If the target is already dead from a previous rounds of self-destructs,
     do nothing (not even roll random for hit/miss).  */
  const auto targetId = target.GetIdAsTarget ();
  const TargetKey targetKey(targetId);
  if (alreadyDead.count (targetKey) > 0)
    {
      VLOG (1)
          << "Target is already dead from before:\n" << targetId.DebugString ();
      return proto::HP ();
    }

  /* Check if we hit or miss.  */
  if (!AttackHitsTarget (target, pb, attackerMod.hitChance))
    {
      VLOG (1) << "Attack misses target:\n" << targetId.DebugString ();
      return proto::HP ();
    }

  /* Compute the modified damage.  If no damage remains, exit early and
     do not update the damage lists.  */
  const auto& targetData = target.GetCombatData ();
  const StatModifier recvDamage(targetData.received_damage_modifier ());
  const auto updatedDamage = recvDamage (dmg);
  CHECK_GE (updatedDamage, 0);
  if (updatedDamage != dmg)
    {
      VLOG (1)
          << "Damage modifier for " << targetId.DebugString ()
          << " changed " << dmg << " to " << updatedDamage;
      dmg = updatedDamage;
    }
  if (dmg == 0)
    {
      VLOG (1) << "No damage done to target:\n" << targetId.DebugString ();
      return proto::HP ();
    }

  VLOG (1)
      << "Dealing " << dmg << " damage to target:\n" << targetId.DebugString ();

  const auto attackerId = attacker.GetIdAsTarget ();
  if (attackerId.type () == proto::TargetId::TYPE_CHARACTER
        && targetId.type () == proto::TargetId::TYPE_CHARACTER)
    dl.AddEntry (targetId.id (), attackerId.id ());

  auto& hp = target.MutableHP ();
  const auto done = ComputeDamage (dmg, pb, hp);

  hp.set_shield (hp.shield () - done.shield ());
  hp.set_armour (hp.armour () - done.armour ());

  VLOG (1) << "Total damage done: " << (done.shield () + done.armour ());
  VLOG (1) << "Remaining total HP: " << (hp.armour () + hp.shield ());
  if (done.shield () + done.armour () > 0 && hp.armour () + hp.shield () == 0)
    {
      /* Regenerated partial HP are ignored (i.e. you die even with 999/1000
         partial HP).  Just make sure that the partial HP are not full yet
         due to some bug.  */
      CHECK_LT (hp.mhp ().shield (), 1'000);
      CHECK_LT (hp.mhp ().armour (), 1'000);
      CHECK (newDead.insert (targetKey).second)
          << "Target is already dead:\n" << targetId.DebugString ();
    }

  return done;
}

void
DamageProcessor::ApplyDamage (const unsigned dmg, const CombatEntity& attacker,
                              const proto::Attack& attack,
                              const CombatModifier& attackerMod,
                              CombatEntity& target,
                              std::set<TargetKey>& newDead)
{
  const auto done = ApplyDamage (dmg, attacker, attack.damage (), attackerMod,
                                 target, newDead);

  /* If this is a gain_hp attack, record the drained HP in the map of
     drain attacks done so we can later process the potential HP gains
     for the attackers.  */
  if (attack.gain_hp ())
    {
      const TargetKey targetId(target.GetIdAsTarget ());
      const TargetKey attackerId(attacker.GetIdAsTarget ());

      auto& drained = gainHpDrained[targetId][attackerId];
      drained.set_armour (drained.armour () + done.armour ());
      drained.set_shield (drained.shield () + done.shield ());
    }
}

void
DamageProcessor::ApplyEffects (const proto::Attack& attack,
                               const CombatEntity& target)
{
  CHECK (!ctx.Map ().SafeZones ().IsNoCombat (target.GetCombatPosition ()));

  if (!attack.has_effects ())
    return;

  const auto targetId = target.GetIdAsTarget ();
  VLOG (1) << "Applying combat effects to " << targetId.DebugString ();

  const auto& attackEffects = attack.effects ();
  auto& targetEffects = newEffects[targetId];

  if (attackEffects.has_speed ())
    *targetEffects.mutable_speed () += attackEffects.speed ();
  if (attackEffects.has_range ())
    *targetEffects.mutable_range () += attackEffects.range ();
  if (attackEffects.has_hit_chance ())
    *targetEffects.mutable_hit_chance () += attackEffects.hit_chance ();
  if (attackEffects.has_shield_regen ())
    *targetEffects.mutable_shield_regen () += attackEffects.shield_regen ();
  if (attackEffects.mentecon ())
    targetEffects.set_mentecon (true);
}

void
DamageProcessor::DealDamage (FighterTable::Handle f, const bool forGainHp,
                             std::set<TargetKey>& newDead)
{
  const auto& cd = f->GetCombatData ();
  const auto& pos = f->GetCombatPosition ();
  CHECK (!ctx.Map ().SafeZones ().IsNoCombat (pos));

  /* If the fighter has friendly attacks and friendlies in range, it may
     happen that we get here without it having a proper target.  This needs
     to be handled fine.  (In this situation, only friendly attacks will need
     to be processed in the end, which only have area and no range.)  */
  const bool hasTarget = f->HasTarget ();
  HexCoord targetPos;
  HexCoord::IntT targetDist = std::numeric_limits<HexCoord::IntT>::max ();
  if (hasTarget)
    {
      FighterTable::Handle tf = fighters.GetForTarget (f->GetTarget ());
      targetPos = tf->GetCombatPosition ();
      targetDist = HexCoord::DistanceL1 (pos, targetPos);
    }
  else
    CHECK (f->HasFriendlyTargets ());

  const auto& mod = modifiers.at (f->GetIdAsTarget ());

  for (const auto& attack : cd.attacks ())
    {
      if (attack.gain_hp () != forGainHp)
        continue;

      /* If this is not a centred-on-attacker AoE attack, check that
         the target is actually within range of this attack (and that
         we actually have a target).  */
      if (attack.has_range ())
        {
          if (!hasTarget)
            continue;
          if (targetDist > static_cast<int> (mod.range (attack.range ())))
            continue;
        }

      unsigned dmg = 0;
      if (attack.has_damage ())
        dmg = RollAttackDamage (attack.damage (), mod.damage);

      if (attack.has_area ())
        {
          HexCoord centre;
          if (attack.has_range ())
            {
              CHECK (hasTarget);
              centre = targetPos;
            }
          else
            centre = pos;

          ProcessCombatTargets (targets, ctx,
                                *f, centre, mod.range (attack.area ()),
                                !attack.friendlies (),
            [&] (const HexCoord& c, const proto::TargetId& id)
            {
              auto t = fighters.GetForTarget (id);
              ApplyDamage (dmg, *f, attack, mod, *t, newDead);
              ApplyEffects (attack, *t);
            });
        }
      else
        {
          CHECK (hasTarget);
          CHECK (!attack.friendlies ());
          auto t = fighters.GetForTarget (f->GetTarget ());
          ApplyDamage (dmg, *f, attack, mod, *t, newDead);
          ApplyEffects (attack, *t);
        }
    }
}

void
DamageProcessor::ProcessSelfDestructs (FighterTable::Handle f,
                                       std::set<TargetKey>& newDead)
{
  const auto& pos = f->GetCombatPosition ();
  CHECK (!ctx.Map ().SafeZones ().IsNoCombat (pos));

  /* The killed fighter should have zero HP left, and thus also should get
     all low-HP boosts now.  */
  CHECK_EQ (f->GetHP ().armour (), 0);
  CHECK_EQ (f->GetHP ().shield (), 0);
  CombatModifier mod;
  ComputeModifier (*f, mod);

  for (const auto& sd : f->GetCombatData ().self_destructs ())
    {
      const auto dmg = RollAttackDamage (sd.damage (), mod.damage);
      VLOG (1)
          << "Dealing " << dmg
          << " of damage for self-destruct of "
          << f->GetIdAsTarget ().DebugString ();

      ProcessCombatTargets (targets, ctx,
                            *f, pos, mod.range (sd.area ()),
                            true,
        [&] (const HexCoord& c, const proto::TargetId& id)
        {
          auto t = fighters.GetForTarget (id);
          ApplyDamage (dmg, *f, sd.damage (), mod, *t, newDead);
        });
    }
}

void
DamageProcessor::Process ()
{
  modifiers.clear ();
  fighters.ProcessWithTarget ([&] (FighterTable::Handle f)
    {
      CombatModifier mod;
      ComputeModifier (*f, mod);
      CHECK (modifiers.emplace (f->GetIdAsTarget (), std::move (mod)).second);
    });

  std::set<TargetKey> newDead;

  /* We first process all attacks with gain_hp, and only later all without.
     This ensures that normal attacks against shields do not remove the HP
     first before they can be drained by a syphon.  */
  fighters.ProcessWithTarget ([&] (FighterTable::Handle f)
    {
      DealDamage (std::move (f), true, newDead);
    });

  /* Reconcile the set of HP gained by attackers now (before normal attacks
     may bring shields down to zero when they aren't yet, for instance).  */
  std::map<TargetKey, proto::HP> gainedHp;
  for (const auto& targetEntry : gainHpDrained)
    {
      CHECK (!targetEntry.second.empty ());

      const auto tf = fighters.GetForTarget (targetEntry.first.ToProto ());
      const auto& tHp = tf->GetHP ();

      for (const auto& attackEntry : targetEntry.second)
        {
          /* While most of the code here is written to support both armour
             and shield drains, we only actually need shield in the game
             (for the syphon fitment).  Supporting both types also leads to
             more issues with processing order, as the order may e.g. determine
             the split between shield and armour for a general attack.
             Thus we disallow this for simplicity (but we could probably
             work out some rules that make it work).  */
          CHECK_EQ (attackEntry.second.armour (), 0)
              << "Armour drain is not supported";
          CHECK_GT (attackEntry.second.shield (), 0);

          proto::HP gained;

          /* The attacker only gains HP if either noone else drained the
             target in question, or there are HP left (so everyone can indeed
             get what they drained).  */
          if (tHp.armour () > 0 || targetEntry.second.size () == 1)
            gained.set_armour (attackEntry.second.armour ());
          if (tHp.shield () > 0 || targetEntry.second.size () == 1)
            gained.set_shield (attackEntry.second.shield ());

          if (gained.armour () > 0 || gained.shield () > 0)
            {
              auto& gainedEntry = gainedHp[attackEntry.first];
              gainedEntry.set_armour (gainedEntry.armour () + gained.armour ());
              gainedEntry.set_shield (gainedEntry.shield () + gained.shield ());
              VLOG (2)
                  << "Fighter " << attackEntry.first.ToProto ().DebugString ()
                  << " gained HP from "
                  << targetEntry.first.ToProto ().DebugString ()
                  << ":\n" << gained.DebugString ();
            }
        }
    }

  fighters.ProcessWithTarget ([&] (FighterTable::Handle f)
    {
      DealDamage (std::move (f), false, newDead);
    });

  /* After applying the base damage, we process all self-destruct actions
     of kills.  This may lead to more damage and more kills, so we have
     to process as many "rounds" of self-destructs as necessary before
     no new kills are added.  */
  while (!newDead.empty ())
    {
      /* The way we merge in the new elements here is not optimal, as one
         could use a proper merge of sorted ranges instead.  But it is quite
         straight-forward and easy to read, and most likely not performance
         critical anyway.  */
      for (const auto& n : newDead)
        CHECK (alreadyDead.insert (n).second)
            << "Target was already dead before:\n"
            << n.ToProto ().DebugString ();

      const auto toProcess = std::move (newDead);
      CHECK (newDead.empty ());

      for (const auto& d : toProcess)
        ProcessSelfDestructs (fighters.GetForTarget (d.ToProto ()), newDead);
    }

  /* Credit gained HP to everyone who is not dead.  */
  for (const auto& entry : gainedHp)
    {
      if (alreadyDead.count (entry.first) > 0)
        {
          VLOG (1)
              << "Fighter " << entry.first.ToProto ().DebugString ()
              << " was killed, not crediting gained HP";
          continue;
        }

      VLOG (1)
          << "Fighter " << entry.first.ToProto ().DebugString ()
          << " gained HP:\n" << entry.second.DebugString ();

      const auto f = fighters.GetForTarget (entry.first.ToProto ());
      const auto& maxHp = f->GetRegenData ().max_hp ();
      auto& hp = f->MutableHP ();
      hp.set_armour (std::min (hp.armour () + entry.second.armour (),
                               maxHp.armour ()));
      hp.set_shield (std::min (hp.shield () + entry.second.shield (),
                               maxHp.shield ()));
    }

  /* Update combat effects on fighters (clear all previous effects in the
     database, and put back in those that are accumulated in newEffects).

     Conceptually, target finding, waiting for the new block, and then
     applying damaging is "one thing".  Swapping over the effects is done
     here, so it is right after that whole "combat block" for the rest
     of processing (e.g. movement or regeneration) and also the next
     combat block.  */
  fighters.ClearAllEffects ();
  for (auto& entry : newEffects)
    {
      auto f = fighters.GetForTarget (entry.first.ToProto ());
      f->MutableEffects () = std::move (entry.second);
    }
}

} // anonymous namespace

std::set<TargetKey>
DealCombatDamage (Database& db, DamageLists& dl,
                  xaya::Random& rnd, const Context& ctx)
{
  DamageProcessor proc(db, dl, rnd, ctx);
  proc.Process ();
  return proc.GetDead ();
}

/* ************************************************************************** */

namespace
{

/**
 * Utility class that handles processing of killed characters and buildings.
 */
class KillProcessor
{

private:

  xaya::Random& rnd;
  const Context& ctx;

  DamageLists& damageLists;
  GroundLootTable& loot;

  AccountsTable accounts;
  BuildingsTable buildings;
  BuildingInventoriesTable inventories;
  CharacterTable characters;
  DexOrderTable orders;
  OngoingsTable ongoings;
  RegionsTable regions;

  /**
   * Deletes a character from the database in all tables.  Takes ownership
   * of and destructs the handle to it.
   */
  void
  DeleteCharacter (CharacterTable::Handle h)
  {
    const auto id = h->GetId ();
    h.reset ();
    damageLists.RemoveCharacter (id);
    ongoings.DeleteForCharacter (id);
    characters.DeleteById (id);
  }

public:

  explicit KillProcessor (Database& db, DamageLists& dl, GroundLootTable& l,
                          xaya::Random& r, const Context& c)
    : rnd(r), ctx(c), damageLists(dl), loot(l),
      accounts(db), buildings(db), inventories(db), characters(db),
      orders(db), ongoings(db), regions(db, ctx.Height ())
  {}

  KillProcessor () = delete;
  KillProcessor (const KillProcessor&) = delete;
  void operator= (const KillProcessor&) = delete;

  /**
   * Processes everything for a character killed in combat.
   */
  void ProcessCharacter (const Database::IdT id);

  /**
   * Processes everything for a building that has been destroyed.
   */
  void ProcessBuilding (const Database::IdT id);

};

void
KillProcessor::ProcessCharacter (const Database::IdT id)
{
  auto c = characters.GetById (id);
  const auto& pb = c->GetProto ();
  const auto& pos = c->GetPosition ();

  /* If the character was prospecting some region, cancel that
     operation and mark the region as not being prospected.  */
  if (c->IsBusy ())
    {
      const auto op = ongoings.GetById (pb.ongoing ());
      CHECK (op != nullptr);
      if (op->GetProto ().has_prospection ())
        {
          const auto regionId = ctx.Map ().Regions ().GetRegionId (pos);
          LOG (INFO)
              << "Killed character " << id
              << " was prospecting region " << regionId
              << ", cancelling";

          auto r = regions.GetById (regionId);
          CHECK_EQ (r->GetProto ().prospecting_character (), id);
          r->MutableProto ().clear_prospecting_character ();
        }
    }

  /* If the character has an inventory, drop everything they had
     on the ground.  Equipped fitments have a chance to survive and
     be dropped as well.  The vehicle itself is always destroyed.  */
  Inventory inv;
  inv += c->GetInventory ();
  for (const auto& f : pb.fitments ())
    if (rnd.ProbabilityRoll (EQUIPPED_FITMENT_DROP_PERCENT, 100))
      inv.AddFungibleCount (f, 1);
  if (!inv.IsEmpty ())
    {
      LOG (INFO)
          << "Killed character " << id
          << " has non-empty inventory/fitments, dropping loot at " << pos;

      auto ground = loot.GetByCoord (pos);
      auto& groundInv = ground->GetInventory ();
      for (const auto& entry : inv.GetFungible ())
        {
          VLOG (1)
              << "Dropping " << entry.second << " of " << entry.first;
          groundInv.AddFungibleCount (entry.first, entry.second);
        }
    }

  DeleteCharacter (std::move (c));
}

void
KillProcessor::ProcessBuilding (const Database::IdT id)
{
  /* Some of the buildings inventory will be dropped on the floor, so we
     need to compute a "combined inventory" of everything that is inside
     the building (all account inventories in the building plus the
     inventories of all characters inside).

     Cubits reserved in open bids will be refunded to their owners,
     and items reserved in asks will be added to the building inventory.

     In addition to that, we destroy all characters inside the building.  */

  Inventory totalInv;

  {
    auto res = inventories.QueryForBuilding (id);
    while (res.Step ())
      totalInv += inventories.GetFromResult (res)->GetInventory ();
  }

  {
    auto res = characters.QueryForBuilding (id);
    while (res.Step ())
      {
        auto c = characters.GetFromResult (res);
        totalInv += c->GetInventory ();
        const auto& pb = c->GetProto ();
        /* Normally the character always has a vehicle, but in some tests
           this might not be set up.  */
        if (pb.has_vehicle ())
          totalInv.AddFungibleCount (pb.vehicle (), 1);
        for (const auto& f : pb.fitments ())
          totalInv.AddFungibleCount (f, 1);
        DeleteCharacter (std::move (c));
      }
  }

  {
    auto res = ongoings.QueryForBuilding (id);
    while (res.Step ())
      {
        auto op = ongoings.GetFromResult (res);

        if (op->GetProto ().has_blueprint_copy ())
          {
            const auto& type
                = op->GetProto ().blueprint_copy ().original_type ();
            totalInv.AddFungibleCount (type, 1);
            continue;
          }

        if (op->GetProto ().has_item_construction ())
          {
            const auto& c = op->GetProto ().item_construction ();
            if (c.has_original_type ())
              totalInv.AddFungibleCount (c.original_type (), 1);
            continue;
          }
      }
  }

  for (const auto& entry : orders.GetReservedCoins (id))
    {
      auto a = accounts.GetByName (entry.first);
      CHECK (a != nullptr);
      a->AddBalance (entry.second);
      VLOG (1)
          << "Refunded " << entry.second << " coins to " << entry.first
          << " for open bids in destroyed building " << id;
    }
  for (const auto& entry : orders.GetReservedQuantities (id))
    totalInv += entry.second;

  auto b = buildings.GetById (id);
  CHECK (b != nullptr) << "Killed non-existant building " << id;
  if (b->GetProto ().has_construction_inventory ())
    totalInv += Inventory (b->GetProto ().construction_inventory ());

  /* The underlying proto map does not have a well-defined order.  Since the
     random rolls depend on the other, make sure to explicitly sort the
     the list of inventory positions.  */
  const auto& protoInvMap = totalInv.GetFungible ();
  const std::map<std::string, Quantity> invItems (protoInvMap.begin (),
                                                  protoInvMap.end ());

  auto lootHandle = loot.GetByCoord (b->GetCentre ());
  b.reset ();

  for (const auto& entry : invItems)
    {
      CHECK_GT (entry.second, 0);
      if (!rnd.ProbabilityRoll (BUILDING_INVENTORY_DROP_PERCENT, 100))
        {
          VLOG (1)
              << "Not dropping " << entry.second << " " << entry.first
              << " from destroyed building " << id;
          continue;
        }

      VLOG (1)
          << "Dropping " << entry.second << " " << entry.first
          << " from destroyed building " << id
          << " at " << lootHandle->GetPosition ();
      lootHandle->GetInventory ().AddFungibleCount (entry.first, entry.second);
    }

  inventories.RemoveBuilding (id);
  ongoings.DeleteForBuilding (id);
  orders.DeleteForBuilding (id);
  buildings.DeleteById (id);
}

} // anonymous namespace

void
ProcessKills (Database& db, DamageLists& dl, GroundLootTable& loot,
              const std::set<TargetKey>& dead,
              xaya::Random& rnd, const Context& ctx)
{
  KillProcessor proc(db, dl, loot, rnd, ctx);

  for (const auto& id : dead)
    switch (id.first)
      {
      case proto::TargetId::TYPE_CHARACTER:
        proc.ProcessCharacter (id.second);
        break;

      case proto::TargetId::TYPE_BUILDING:
        proc.ProcessBuilding (id.second);
        break;

      default:
        LOG (FATAL)
            << "Invalid target type killed: " << static_cast<int> (id.first);
      }
}

/* ************************************************************************** */

namespace
{

/**
 * Performs the regeneration logic for one type of HP (armour or shield).
 * Returns the new "full" and "milli" HP value in the output variables,
 * and true iff something changed.
 */
bool
RegenerateHpType (const unsigned max, const unsigned mhpRate,
                  const unsigned oldCur, const unsigned oldMilli,
                  unsigned& newCur, unsigned& newMilli)
{
  CHECK (oldCur < max || (oldCur == max && oldMilli == 0));

  newMilli = oldMilli + mhpRate;
  newCur = oldCur + newMilli / 1'000;
  newMilli %= 1'000;

  if (newCur >= max)
    {
      newCur = max;
      newMilli = 0;
    }

  CHECK (newCur > oldCur || (newCur == oldCur && newMilli >= oldMilli));
  return newCur != oldCur || newMilli != oldMilli;
}

/**
 * Applies HP regeneration (if any) to a given fighter.
 */
void
RegenerateFighterHP (FighterTable::Handle f)
{
  const auto& regen = f->GetRegenData ();
  const auto& hp = f->GetHP ();

  unsigned cur, milli;

  if (RegenerateHpType (
          regen.max_hp ().armour (), regen.regeneration_mhp ().armour (),
          hp.armour (), hp.mhp ().armour (), cur, milli))
    {
      f->MutableHP ().set_armour (cur);
      f->MutableHP ().mutable_mhp ()->set_armour (milli);
    }

  const StatModifier shieldRegenMod(f->GetEffects ().shield_regen ());
  const unsigned shieldRate
      = shieldRegenMod (regen.regeneration_mhp ().shield ());

  if (RegenerateHpType (
          regen.max_hp ().shield (), shieldRate,
          hp.shield (), hp.mhp ().shield (), cur, milli))
    {
      f->MutableHP ().set_shield (cur);
      f->MutableHP ().mutable_mhp ()->set_shield (milli);
    }
}

} // anonymous namespace

void
RegenerateHP (Database& db)
{
  BuildingsTable buildings(db);
  CharacterTable characters(db);
  FighterTable fighters(buildings, characters);

  fighters.ProcessForRegen ([] (FighterTable::Handle f)
    {
      RegenerateFighterHP (std::move (f));
    });
}

/* ************************************************************************** */

void
AllHpUpdates (Database& db, FameUpdater& fame, xaya::Random& rnd,
              const Context& ctx)
{
  const auto dead = DealCombatDamage (db, fame.GetDamageLists (), rnd, ctx);

  for (const auto& id : dead)
    fame.UpdateForKill (id.ToProto ());

  GroundLootTable loot(db);
  ProcessKills (db, fame.GetDamageLists (), loot, dead, rnd, ctx);

  RegenerateHP (db);
}

} // namespace pxd
