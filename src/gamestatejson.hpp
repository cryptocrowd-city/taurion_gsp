/*
    GSP for the Taurion blockchain game
    Copyright (C) 2019  Autonomous Worlds Ltd

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

#ifndef PXD_GAMESTATEJSON_HPP
#define PXD_GAMESTATEJSON_HPP

#include "params.hpp"

#include "database/damagelists.hpp"
#include "database/database.hpp"
#include "mapdata/basemap.hpp"

#include <json/json.h>

namespace pxd
{

/**
 * Utility class that handles construction of game-state JSON.
 */
class GameStateJson
{

private:

  /** Database to read from.  */
  Database& db;

  /** Damage lists accessor (for adding the attackers to a character JSON).  */
  const DamageLists dl;

  /** Game parameters.  */
  const Params& params;

  /** Basemap instance that can be used.  */
  const BaseMap& map;

  /**
   * Extracts all results from the Database::Result instance, converts them
   * to JSON, and returns a JSON array.
   */
  template <typename T, typename R>
    Json::Value ResultsAsArray (T& tbl, Database::Result<R> res) const;

public:

  explicit GameStateJson (Database& d, const Params& p, const BaseMap& m)
    : db(d), dl(db), params(p), map(m)
  {}

  GameStateJson () = delete;
  GameStateJson (const GameStateJson&) = delete;
  void operator= (const GameStateJson&) = delete;

  /**
   * Converts a state instance (like a Character or Region) to the corresponding
   * JSON value in the game state.
   */
  template <typename T>
    Json::Value Convert (const T& val) const;

  /**
   * Returns the JSON data representing all accounts in the game state.
   */
  Json::Value Accounts ();

  /**
   * Returns the JSON data representing all characters in the game state.
   */
  Json::Value Characters ();

  /**
   * Returns the JSON data representing all ground loot.
   */
  Json::Value GroundLoot ();

  /**
   * Returns the JSON data representing all regions in the game state.
   */
  Json::Value Regions ();

  /**
   * Returns the JSON data representing the available and found prizes
   * for prospecting.
   */
  Json::Value PrizeStats ();

  /**
   * Returns the full game state JSON for the given Database handle.  The full
   * game state as JSON should mainly be used for debugging and testing, not
   * in production.  For that, more targeted RPC results should be used.
   */
  Json::Value FullState ();

};

} // namespace pxd

#endif // PXD_GAMESTATEJSON_HPP
