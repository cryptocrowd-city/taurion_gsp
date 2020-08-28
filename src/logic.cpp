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

#include "logic.hpp"

#include "buildings.hpp"
#include "combat.hpp"
#include "dynobstacles.hpp"
#include "mining.hpp"
#include "movement.hpp"
#include "moveprocessor.hpp"
#include "ongoings.hpp"

#include "database/account.hpp"
#include "database/building.hpp"
#include "database/moneysupply.hpp"
#include "database/schema.hpp"

#include <glog/logging.h>

namespace pxd
{

sqlite3_stmt*
SQLiteGameDatabase::PrepareStatement (const std::string& sql)
{
  return db.Prepare (sql);
}

Database::IdT
SQLiteGameDatabase::GetNextId ()
{
  return game.Ids ("pxd").GetNext ();
}

const BaseMap&
PXLogic::GetBaseMap ()
{
  if (map == nullptr)
    {
      const auto chain = GetChain ();
      VLOG (1)
          << "Constructing BaseMap instance for chain "
          << static_cast<int> (chain);
      map = std::make_unique<BaseMap> (chain);
    }

  CHECK (map != nullptr);
  return *map;
}

void
PXLogic::UpdateState (Database& db, xaya::Random& rnd,
                      const xaya::Chain chain, const BaseMap& map,
                      const Json::Value& blockData)
{
  const auto& blockMeta = blockData["block"];
  CHECK (blockMeta.isObject ());
  const auto& heightVal = blockMeta["height"];
  CHECK (heightVal.isUInt64 ());
  const unsigned height = heightVal.asUInt64 ();
  const auto& timestampVal = blockMeta["timestamp"];
  CHECK (timestampVal.isInt64 ());
  const int64_t timestamp = timestampVal.asInt64 ();

  Context ctx(chain, map, height, timestamp);

  FameUpdater fame(db, ctx);
  UpdateState (db, fame, rnd, ctx, blockData);
}

void
PXLogic::UpdateState (Database& db, FameUpdater& fame, xaya::Random& rnd,
                      const Context& ctx, const Json::Value& blockData)
{
  fame.GetDamageLists ().RemoveOld (
      ctx.RoConfig ()->params ().damage_list_blocks ());

  AllHpUpdates (db, fame, rnd, ctx);
  ProcessAllOngoings (db, rnd, ctx);

  DynObstacles dyn(db, ctx);
  MoveProcessor mvProc(db, dyn, rnd, ctx);
  mvProc.ProcessAdmin (blockData["admin"]);
  mvProc.ProcessAll (blockData["moves"]);

  ProcessAllMining (db, rnd, ctx);
  ProcessAllMovement (db, dyn, ctx);

  /* Entering buildings should be after moves and movement, so that players
     enter as soon as possible (perhaps in the same instant the move for it
     gets confirmed).  It should be before combat targets, so that players
     entering a building won't be attacked any more.  */
  ProcessEnterBuildings (db, dyn, ctx);

  FindCombatTargets (db, rnd, ctx);

#ifdef ENABLE_SLOW_ASSERTS
  ValidateStateSlow (db, ctx);
#endif // ENABLE_SLOW_ASSERTS
}

void
PXLogic::SetupSchema (xaya::SQLiteDatabase& db)
{
  SetupDatabaseSchema (*db);
}

void
PXLogic::GetInitialStateBlock (unsigned& height,
                               std::string& hashHex) const
{
  const xaya::Chain chain = GetChain ();
  switch (chain)
    {
    case xaya::Chain::MAIN:
      height = 2'000'000;
      hashHex
          = "1e4949abe599a40af54b650f21cb83e97ee054c4b172515877f340e2beb550b5";
      break;

    case xaya::Chain::TEST:
      height = 112'000;
      hashHex
          = "9c5b83a5caaf7f4ce17cc1f38fdb1ed3e3e3e98e43d23d19a4810767d7df38b9";
      break;

    case xaya::Chain::REGTEST:
      height = 0;
      hashHex
          = "6f750b36d22f1dc3d0a6e483af45301022646dfc3b3ba2187865f5a7d6d83ab1";
      break;

    default:
      LOG (FATAL) << "Unexpected chain: " << xaya::ChainToString (chain);
    }
}

void
PXLogic::InitialiseState (xaya::SQLiteDatabase& db)
{
  SQLiteGameDatabase dbObj(db, *this);

  InitialiseBuildings (dbObj, GetChain ());

  MoneySupply ms(dbObj);
  ms.InitialiseDatabase ();

  /* The initialisation uses up some auto IDs, namely for placed buildings.
     We start "regular" IDs at a later value to avoid shifting them always
     when we tweak initialisation, and thus having to potentially update test
     data and other stuff.  */
  Ids ("pxd").ReserveUpTo (1'000);

  /* FIXME: Hack for testing, should not be released in a production
     version (obviously).  */
  {
    BuildingInventoriesTable invs(dbObj);
    auto h = invs.Get (5, "acoloss");
    auto& inv = h->GetInventory ();

    for (const auto& itm : RoConfig (GetChain ())->fungible_items ())
      {
        /* 100 million units of each ore.  */
        if (itm.second.has_refines ())
          inv.AddFungibleCount (itm.first, 100'000'000);
        /* One of each blueprint.  */
        if (itm.second.with_blueprint ())
          inv.AddFungibleCount (itm.first + " bpo", 1);
        /* Two of each upgrade.  */
        if (itm.second.has_fitment ())
          inv.AddFungibleCount (itm.first, 2);
        /* Two of each vehicle.  */
        if (itm.second.has_vehicle ())
          inv.AddFungibleCount (itm.first, 2);
        /* 100 of each artefact.  */
        if (itm.second.has_reveng ())
          inv.AddFungibleCount (itm.first, 100);
      }
  }
}

void
PXLogic::UpdateState (xaya::SQLiteDatabase& db, const Json::Value& blockData)
{
  SQLiteGameDatabase dbObj(db, *this);
  UpdateState (dbObj, GetContext ().GetRandom (),
               GetChain (), GetBaseMap (), blockData);
}

Json::Value
PXLogic::GetStateAsJson (const xaya::SQLiteDatabase& db)
{
  SQLiteGameDatabase dbObj(const_cast<xaya::SQLiteDatabase&> (db), *this);
  const Context ctx(GetChain (), GetBaseMap (),
                    Context::NO_HEIGHT, Context::NO_TIMESTAMP);
  GameStateJson gsj(dbObj, ctx);

  return gsj.FullState ();
}

Json::Value
PXLogic::GetCustomStateData (xaya::Game& game, const JsonStateFromRawDb& cb)
{
  return SQLiteGame::GetCustomStateData (game, "data",
      [this, &cb] (const xaya::SQLiteDatabase& db, const xaya::uint256& hash,
                   const unsigned height)
        {
          SQLiteGameDatabase dbObj(const_cast<xaya::SQLiteDatabase&> (db),
                                   *this);
          return cb (dbObj, hash, height);
        });
}

Json::Value
PXLogic::GetCustomStateData (xaya::Game& game,
                             const JsonStateFromDatabaseWithBlock& cb)
{
  return GetCustomStateData (game,
    [this, &cb] (Database& db, const xaya::uint256& hash, const unsigned height)
        {
          const Context ctx(GetChain (), GetBaseMap (),
                            Context::NO_HEIGHT, Context::NO_TIMESTAMP);
          GameStateJson gsj(db, ctx);
          return cb (gsj, hash, height);
        });
}

Json::Value
PXLogic::GetCustomStateData (xaya::Game& game, const JsonStateFromDatabase& cb)
{
  return GetCustomStateData (game,
    [&cb] (GameStateJson& gsj, const xaya::uint256& hash, const unsigned height)
    {
      return cb (gsj);
    });
}

namespace
{

/**
 * Verifies that each character's and building's faction in the database
 * matches the owner's faction.
 */
void
ValidateCharacterBuildingFactions (Database& db)
{
  std::unordered_map<std::string, Faction> accountFactions;
  {
    AccountsTable accounts(db);
    auto res = accounts.QueryInitialised ();
    while (res.Step ())
      {
        auto a = accounts.GetFromResult (res);
        const auto f = a->GetFaction ();
        CHECK (f != Faction::INVALID && f != Faction::ANCIENT)
            << "Account " << a->GetName () << " has invalid faction";
        auto insert = accountFactions.emplace (a->GetName (), f);
        CHECK (insert.second) << "Duplicate account name " << a->GetName ();
      }
  }

  {
    CharacterTable characters(db);
    auto res = characters.QueryAll ();
    while (res.Step ())
      {
        auto h = characters.GetFromResult (res);
        const auto mit = accountFactions.find (h->GetOwner ());
        CHECK (mit != accountFactions.end ())
            << "Character " << h->GetId ()
            << " owned by uninitialised account " << h->GetOwner ();
        CHECK (h->GetFaction () == mit->second)
            << "Faction mismatch between character " << h->GetId ()
            << " and owner account " << h->GetOwner ();
      }
  }

  {
    BuildingsTable buildings(db);
    auto res = buildings.QueryAll ();
    while (res.Step ())
      {
        auto h = buildings.GetFromResult (res);
        if (h->GetFaction () == Faction::ANCIENT)
          continue;
        const auto mit = accountFactions.find (h->GetOwner ());
        CHECK (mit != accountFactions.end ())
            << "Building " << h->GetId ()
            << " owned by uninitialised account " << h->GetOwner ();
        CHECK (h->GetFaction () == mit->second)
            << "Faction mismatch between building " << h->GetId ()
            << " and owner account " << h->GetOwner ();
      }
  }
}

/**
 * Verifies that each account has at most the maximum allowed number of
 * characters in the database.
 */
void
ValidateCharacterLimit (Database& db, const Context& ctx)
{
  CharacterTable characters(db);
  AccountsTable accounts(db);

  auto res = accounts.QueryInitialised ();
  while (res.Step ())
    {
      auto a = accounts.GetFromResult (res);
      CHECK_LE (characters.CountForOwner (a->GetName ()),
                ctx.RoConfig ()->params ().character_limit ())
          << "Account " << a->GetName () << " has too many characters";
    }
}

/**
 * Verifies that characters are only inside buildings they can be in,
 * i.e. ancient or matching their faction.
 */
void
ValidateCharactersInBuildings (Database& db)
{
  BuildingsTable buildings(db);
  CharacterTable characters(db);

  auto res = characters.QueryAll ();
  while (res.Step ())
    {
      auto c = characters.GetFromResult (res);
      if (!c->IsInBuilding ())
        continue;

      const auto id = c->GetBuildingId ();
      auto b = buildings.GetById (id);
      CHECK (b != nullptr)
          << "Character " << c->GetId ()
          << " is in non-existant building " << id;

      if (b->GetFaction () == Faction::ANCIENT)
        continue;
      CHECK (c->GetFaction () == b->GetFaction ())
          << "Character " << c->GetId ()
          << " is in building " << id
          << " of opposing faction";
    }
}

/**
 * Verifies that all "in building" inventories have an existing
 * building and account association.  No inventories may be inside a foundation.
 */
void
ValidateBuildingInventories (Database& db)
{
  BuildingInventoriesTable inv(db);
  AccountsTable accounts(db);
  BuildingsTable buildings(db);

  {
    auto res = inv.QueryAll ();
    while (res.Step ())
      {
        auto h = inv.GetFromResult (res);
        auto b = buildings.GetById (h->GetBuildingId ());
        CHECK (b != nullptr)
            << "Inventory for non-existant building " << h->GetBuildingId ();
        CHECK (!b->GetProto ().foundation ())
            << "Inventory for " << h->GetAccount ()
            << " in foundation " << h->GetBuildingId ();
        CHECK (accounts.GetByName (h->GetAccount ()) != nullptr)
            << "Inventory for non-existant account " << h->GetAccount ();
      }
  }

  {
    auto res = buildings.QueryAll ();
    while (res.Step ())
      {
        auto b = buildings.GetFromResult (res);
        const auto& pb = b->GetProto ();
        CHECK (pb.foundation () || !pb.has_construction_inventory ())
            << "Building " << b->GetId ()
            << " is not a foundation but has construction inventory";
      }
  }
}

/**
 * Verifies that the links between characters/buildings and ongoing
 * operations are all valid.
 */
void
ValidateOngoingsLinks (Database& db)
{
  BuildingsTable buildings(db);
  CharacterTable characters(db);
  OngoingsTable ongoings(db);

  {
    auto res = ongoings.QueryAll ();
    while (res.Step ())
      {
        auto op = ongoings.GetFromResult (res);
        const auto bId = op->GetBuildingId ();
        const auto cId = op->GetCharacterId ();

        if (bId != Database::EMPTY_ID)
          {
            auto b = buildings.GetById (bId);
            CHECK (b != nullptr)
                << "Operation " << op->GetId ()
                << " refers to non-existing building " << bId;
            CHECK_EQ (b->GetProto ().ongoing_construction (), op->GetId ())
                << "Building " << bId
                << " does not refer back to ongoing " << op->GetId ();
          }

        if (cId != Database::EMPTY_ID)
          {
            auto c = characters.GetById (cId);
            CHECK (c != nullptr)
                << "Operation " << op->GetId ()
                << " refers to non-existing character " << cId;
            CHECK_EQ (c->GetProto ().ongoing (), op->GetId ())
                << "Character " << cId
                << " does not refer back to ongoing " << op->GetId ();
          }
      }
  }

  {
    auto res = characters.QueryAll ();
    while (res.Step ())
      {
        auto c = characters.GetFromResult (res);
        if (!c->IsBusy ())
          continue;

        const auto opId = c->GetProto ().ongoing ();
        const auto op = ongoings.GetById (opId);
        CHECK (op != nullptr)
            << "Character " << c->GetId ()
            << " has non-existing ongoing operation " << opId;
        CHECK_EQ (op->GetCharacterId (), c->GetId ())
            << "Operation " << opId
            << " does not refer back to character " << c->GetId ();
      }
  }

  {
    auto res = buildings.QueryAll ();
    while (res.Step ())
      {
        auto b = buildings.GetFromResult (res);
        if (!b->GetProto ().has_ongoing_construction ())
          continue;

        const auto opId = b->GetProto ().ongoing_construction ();
        const auto op = ongoings.GetById (opId);
        CHECK (op != nullptr)
            << "Building " << b->GetId ()
            << " has non-existing ongoing operation " << opId;
        CHECK_EQ (op->GetBuildingId (), b->GetId ())
            << "Operation " << opId
            << " does not refer back to building " << b->GetId ();
      }
  }
}

} // anonymous namespace

void
PXLogic::ValidateStateSlow (Database& db, const Context& ctx)
{
  LOG (INFO) << "Performing slow validation of the game-state database...";
  ValidateCharacterBuildingFactions (db);
  ValidateCharacterLimit (db, ctx);
  ValidateCharactersInBuildings (db);
  ValidateBuildingInventories (db);
  ValidateOngoingsLinks (db);
}

} // namespace pxd
