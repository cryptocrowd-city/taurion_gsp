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

#include "gamestatejson.hpp"

#include "buildings.hpp"
#include "jsonutils.hpp"
#include "protoutils.hpp"

#include "database/account.hpp"
#include "database/building.hpp"
#include "database/character.hpp"
#include "database/faction.hpp"
#include "database/inventory.hpp"
#include "database/prizes.hpp"
#include "database/region.hpp"
#include "hexagonal/pathfinder.hpp"
#include "proto/character.pb.h"

namespace pxd
{

namespace
{

/**
 * Converts a TargetId proto to its JSON gamestate form.
 */
Json::Value
TargetIdToJson (const proto::TargetId& target)
{
  Json::Value res(Json::objectValue);
  res["id"] = IntToJson (target.id ());

  switch (target.type ())
    {
    case proto::TargetId::TYPE_CHARACTER:
      res["type"] = "character";
      break;
    case proto::TargetId::TYPE_BUILDING:
      res["type"] = "building";
      break;
    default:
      LOG (FATAL) << "Invalid target type: " << target.type ();
    }

  return res;
}

/**
 * Converts an HP proto to a JSON form.
 */
Json::Value
HpProtoToJson (const proto::HP& hp)
{
  Json::Value res(Json::objectValue);
  res["armour"] = IntToJson (hp.armour ());

  const int baseShield = hp.shield ();
  if (hp.shield_mhp () == 0)
    res["shield"] = baseShield;
  else
    res["shield"] = baseShield + hp.shield_mhp () / 1000.0;

  return res;
}

/**
 * Computes the "movement" sub-object for a Character's JSON state.
 */
Json::Value
GetMovementJsonObject (const Character& c)
{
  const auto& pb = c.GetProto ();
  Json::Value res(Json::objectValue);

  const auto& volMv = c.GetVolatileMv ();
  if (volMv.has_partial_step ())
    res["partialstep"] = IntToJson (volMv.partial_step ());
  if (volMv.has_blocked_turns ())
    res["blockedturns"] = IntToJson (volMv.blocked_turns ());

  if (pb.has_movement ())
    {
      const auto& mvProto = pb.movement ();

      if (mvProto.has_chosen_speed ())
        res["chosenspeed"] = mvProto.chosen_speed ();

      Json::Value wp(Json::arrayValue);
      for (const auto& entry : mvProto.waypoints ())
        wp.append (CoordToJson (CoordFromProto (entry)));
      if (wp.size () > 0)
        res["waypoints"] = wp;

      /* The precomputed path is processed (rather than just translated from
         proto to JSON):  We strip off already visited points from it, and
         we "shift" it by one so that the points represent destinations
         and it is easier to understand.  */
      Json::Value path(Json::arrayValue);
      bool foundPosition = false;
      for (const auto& s : mvProto.steps ())
        {
          const HexCoord from = CoordFromProto (s);
          if (from == c.GetPosition ())
            {
              CHECK (!foundPosition);
              foundPosition = true;
            }
          else if (foundPosition)
            path.append (CoordToJson (from));
        }
      CHECK (foundPosition || mvProto.steps_size () == 0);
      if (foundPosition)
        {
          CHECK (wp.size () > 0);
          path.append (wp[0]);
          res["steps"] = path;
        }
    }

  return res;
}

/**
 * Computes the "combat" sub-object for a Character's JSON state.
 */
Json::Value
GetCombatJsonObject (const Character& c, const DamageLists& dl)
{
  Json::Value res(Json::objectValue);

  const auto& pb = c.GetProto ();
  if (pb.has_target ())
    res["target"] = TargetIdToJson (pb.target ());

  Json::Value attacks(Json::arrayValue);
  for (const auto& attack : pb.combat_data ().attacks ())
    {
      Json::Value obj(Json::objectValue);
      obj["range"] = IntToJson (attack.range ());
      obj["area"] = attack.area ();
      obj["mindamage"] = IntToJson (attack.min_damage ());
      obj["maxdamage"] = IntToJson (attack.max_damage ());
      attacks.append (obj);
    }
  if (!attacks.empty ())
    res["attacks"] = attacks;

  const auto& regen = c.GetRegenData ();
  Json::Value hp(Json::objectValue);
  hp["max"] = HpProtoToJson (regen.max_hp ());
  hp["current"] = HpProtoToJson (c.GetHP ());
  hp["regeneration"] = regen.shield_regeneration_mhp () / 1000.0;
  res["hp"] = hp;

  Json::Value attackers(Json::arrayValue);
  for (const auto id : dl.GetAttackers (c.GetId ()))
    attackers.append (IntToJson (id));
  if (!attackers.empty ())
    res["attackers"] = attackers;

  return res;
}

/**
 * Constructs the JSON state object for a character's busy state.  Returns
 * JSON null if the character is not busy.
 */
Json::Value
GetBusyJsonObject (const BaseMap& map, const Character& c)
{
  const auto busyBlocks = c.GetBusy ();
  if (busyBlocks == 0)
    return Json::Value ();

  Json::Value res(Json::objectValue);
  res["blocks"] = IntToJson (busyBlocks);

  const auto& pb = c.GetProto ();
  switch (pb.busy_case ())
    {
    case proto::Character::kProspection:
      res["operation"] = "prospecting";
      res["region"] = IntToJson (map.Regions ().GetRegionId (c.GetPosition ()));
      break;

    default:
      LOG (FATAL) << "Unexpected busy state for character: " << pb.busy_case ();
    }

  return res;
}

/**
 * Constructs the JSON representation of a character's cargo space.
 */
Json::Value
GetCargoSpaceJsonObject (const Character& c)
{
  const auto used = c.UsedCargoSpace ();

  Json::Value res(Json::objectValue);
  res["total"] = IntToJson (c.GetProto ().cargo_space ());
  res["used"] = IntToJson (used);
  res["free"] = IntToJson (c.GetProto ().cargo_space () - used);

  return res;
}

/**
 * Constructs the JSON representation of the mining data of a character.
 */
Json::Value
GetMiningJsonObject (const BaseMap& map, const Character& c)
{
  if (!c.GetProto ().has_mining ())
    return Json::Value ();
  const auto& pb = c.GetProto ().mining ();

  Json::Value rate(Json::objectValue);
  rate["min"] = IntToJson (pb.rate ().min ());
  rate["max"] = IntToJson (pb.rate ().max ());

  Json::Value res(Json::objectValue);
  res["rate"] = rate;
  res["active"] = pb.active ();
  if (pb.active ())
    res["region"] = IntToJson (map.Regions ().GetRegionId (c.GetPosition ()));

  return res;
}

} // anonymous namespace

template <>
  Json::Value
  GameStateJson::Convert<Inventory> (const Inventory& inv) const
{
  Json::Value fungible(Json::objectValue);
  for (const auto& entry : inv.GetFungible ())
    fungible[entry.first] = IntToJson (entry.second);

  Json::Value res(Json::objectValue);
  res["fungible"] = fungible;

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Character> (const Character& c) const
{
  Json::Value res(Json::objectValue);
  res["id"] = IntToJson (c.GetId ());
  res["owner"] = c.GetOwner ();
  res["faction"] = FactionToString (c.GetFaction ());
  res["position"] = CoordToJson (c.GetPosition ());
  res["combat"] = GetCombatJsonObject (c, dl);
  res["speed"] = c.GetProto ().speed ();
  res["inventory"] = Convert (c.GetInventory ());
  res["cargospace"] = GetCargoSpaceJsonObject (c);

  const Json::Value mv = GetMovementJsonObject (c);
  if (!mv.empty ())
    res["movement"] = mv;

  const Json::Value busy = GetBusyJsonObject (map, c);
  if (!busy.isNull ())
    res["busy"] = busy;

  const Json::Value mining = GetMiningJsonObject (map, c);
  if (!mining.isNull ())
    res["mining"] = mining;

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Account> (const Account& a) const
{
  Json::Value res(Json::objectValue);
  res["name"] = a.GetName ();
  res["faction"] = FactionToString (a.GetFaction ());
  res["kills"] = IntToJson (a.GetKills ());
  res["fame"] = IntToJson (a.GetFame ());
  res["banked"] = Convert (a.GetBanked ());
  res["bankingpoints"] = IntToJson (a.GetBankingPoints ());

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Building> (const Building& b) const
{
  Json::Value res(Json::objectValue);
  res["id"] = IntToJson (b.GetId ());
  res["type"] = b.GetType ();
  res["faction"] = FactionToString (b.GetFaction ());
  if (b.GetFaction () != Faction::ANCIENT)
    res["owner"] = b.GetOwner ();
  res["centre"] = CoordToJson (b.GetCentre ());

  const auto& pb = b.GetProto ();
  res["rotationsteps"] = IntToJson (pb.shape_trafo ().rotation_steps ());

  Json::Value tiles(Json::arrayValue);
  for (const auto& c : GetBuildingShape (b))
    tiles.append (CoordToJson (c));
  res["tiles"] = tiles;

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<pxd::GroundLoot> (const pxd::GroundLoot& loot) const
{
  Json::Value res(Json::objectValue);
  res["position"] = CoordToJson (loot.GetPosition ());
  res["inventory"] = Convert (loot.GetInventory ());

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Region> (const Region& r) const
{
  const auto& pb = r.GetProto ();

  Json::Value res(Json::objectValue);
  res["id"] = r.GetId ();

  Json::Value prospection(Json::objectValue);
  if (pb.has_prospecting_character ())
    prospection["inprogress"] = IntToJson (pb.prospecting_character ());
  if (pb.has_prospection ())
    {
      prospection["name"] = pb.prospection ().name ();
      prospection["height"] = pb.prospection ().height ();
    }

  if (!prospection.empty ())
    res["prospection"] = prospection;

  if (pb.has_prospection ())
    {
      Json::Value resource(Json::objectValue);
      resource["type"] = pb.prospection ().resource ();
      resource["amount"] = IntToJson (r.GetResourceLeft ());

      res["resource"] = resource;
    }

  return res;
}

template <typename T, typename R>
  Json::Value
  GameStateJson::ResultsAsArray (T& tbl, Database::Result<R> res) const
{
  Json::Value arr(Json::arrayValue);

  while (res.Step ())
    {
      const auto h = tbl.GetFromResult (res);
      arr.append (Convert (*h));
    }

  return arr;
}

Json::Value
GameStateJson::PrizeStats ()
{
  Prizes prizeTable(db);

  Json::Value res(Json::objectValue);
  for (const auto& p : params.ProspectingPrizes ())
    {
      Json::Value cur(Json::objectValue);
      cur["number"] = p.number;
      cur["probability"] = p.probability;

      const unsigned found = prizeTable.GetFound (p.name);
      CHECK_LE (found, p.number);

      cur["found"] = found;
      cur["available"] = p.number - found;

      res[p.name] = cur;
    }

  return res;
}

Json::Value
GameStateJson::Accounts ()
{
  AccountsTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryInitialised ());
}

Json::Value
GameStateJson::Buildings ()
{
  BuildingsTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryAll ());
}

Json::Value
GameStateJson::Characters ()
{
  CharacterTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryAll ());
}

Json::Value
GameStateJson::GroundLoot ()
{
  GroundLootTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryNonEmpty ());
}

Json::Value
GameStateJson::Regions (const unsigned h)
{
  RegionsTable tbl(db, RegionsTable::HEIGHT_READONLY);
  return ResultsAsArray (tbl, tbl.QueryModifiedSince (h));
}

Json::Value
GameStateJson::FullState ()
{
  Json::Value res(Json::objectValue);

  res["accounts"] = Accounts ();
  res["buildings"] = Buildings ();
  res["characters"] = Characters ();
  res["groundloot"] = GroundLoot ();
  res["regions"] = Regions (0);
  res["prizes"] = PrizeStats ();

  return res;
}

Json::Value
GameStateJson::BootstrapData ()
{
  Json::Value res(Json::objectValue);
  res["regions"] = Regions (0);

  return res;
}

} // namespace pxd
