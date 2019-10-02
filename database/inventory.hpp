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

#ifndef DATABASE_INVENTORY_HPP
#define DATABASE_INVENTORY_HPP

#include "coord.hpp"
#include "database.hpp"
#include "lazyproto.hpp"

#include "proto/inventory.pb.h"

#include <google/protobuf/map.h>

#include <cstdint>
#include <string>

namespace pxd
{

/**
 * The maximum valid value for an item quantity.  If a move contains a number
 * larger than this, it is considered invalid.  This is consensus relevant.
 * Through this limit, we ensure that values are "sane" and avoid potential
 * overflows when working with them.
 *
 * But this is not only applied to moves, but checked in general for any
 * item quantity.  So it should really be the total supply limit of anything
 * in the game.
 *
 * A value of one billion allows multiplication with another value in that
 * range (e.g. cargo per item or price per unit) without overflowing 64 bits.
 */
static constexpr int64_t MAX_ITEM_QUANTITY = 1'000'000'000;

/**
 * The maximum value any "dual variables" for item quantities can have.
 * These are things that are multiplied with them, for instance per-unit
 * value/weight or cost.  By limiting this value, we ensure that the product
 * can always be safely computed in 64 bits.
 */
static constexpr int64_t MAX_ITEM_DUAL = 1'000'000'000;

/**
 * Wrapper class around the state of an inventory.  This is what game-logic
 * code should use rather than plain Inventory protos.
 */
class Inventory
{

private:

  /** The underlying data as proto.  */
  LazyProto<proto::Inventory> data;

public:

  /** Type for the quantity of an item.  */
  using QuantityT = int64_t;

  /**
   * Constructs an instance representing an empty inventory (that can then
   * be modified, for instance).
   */
  Inventory ();

  /**
   * Constructs an instance wrapping the given proto data.
   */
  explicit Inventory (LazyProto<proto::Inventory>&& d);

  /**
   * Sets the contained inventory from the given proto.
   */
  Inventory& operator= (LazyProto<proto::Inventory>&& d);

  Inventory (const Inventory&) = delete;
  void operator= (const Inventory&) = delete;

  /**
   * Returns the fungible inventory items as protobuf maps.  This can be
   * used to iterate over all non-zero fungible items (e.g. to construct
   * the JSON state for it).
   */
  const google::protobuf::Map<std::string, google::protobuf::uint64>&
  GetFungible () const
  {
    return data.Get ().fungible ();
  }

  /**
   * Returns the number of fungible items with the given key in the inventory.
   * Returns zero for non-existant items.
   */
  QuantityT GetFungibleCount (const std::string& type) const;

  /**
   * Sets the number of fungible items with the given key in the inventory.
   */
  void SetFungibleCount (const std::string& type, QuantityT count);

  /**
   * Returns true if the inventory data has been modified (and thus needs to
   * be saved back to the database).
   */
  bool
  IsDirty () const
  {
    return data.IsDirty ();
  }

  /**
   * Returns true if the inventory is empty.  Note that this forces the
   * proto to get parsed if it hasn't yet been.
   */
  bool IsEmpty () const;

  /**
   * Gives access to the underlying lazy proto for binding purposes.
   */
  const LazyProto<proto::Inventory>&
  GetProtoForBinding () const
  {
    return data;
  }

  /**
   * Computes the product of a quantity value with a dual value.  Both
   * must be within the limits, or else the function CHECK-fails.  They may
   * be signed, though.
   */
  static int64_t Product (QuantityT amount, int64_t dual);

};

/**
 * Database result type for rows from the ground_loot table.
 */
struct GroundLootResult : public ResultWithCoord
{
  RESULT_COLUMN (proto::Inventory, inventory, 1);
};

/**
 * Wrapper class around the loot on the ground at a certain location.
 *
 * Instantiations of this class should be made through GroundLootTable.
 */
class GroundLoot
{

private:

  /** Database this belongs to.  */
  Database& db;

  /** The coordinate of this loot tile.  */
  HexCoord coord;

  /** The associated loot.  */
  Inventory inventory;

  /**
   * Constructs an instance with empty inventory.
   */
  explicit GroundLoot (Database& d, const HexCoord& pos);

  /**
   * Constructs an instance based on an existing DB result.
   */
  explicit GroundLoot (Database& d,
                       const Database::Result<GroundLootResult>& res);

  friend class GroundLootTable;

public:

  /**
   * In the destructor, potential updates to the database are made if the
   * data has been modified.
   */
  ~GroundLoot ();

  GroundLoot () = delete;
  GroundLoot (const GroundLoot&) = delete;
  void operator= (const GroundLoot&) = delete;

  const HexCoord&
  GetPosition () const
  {
    return coord;
  }

  const Inventory&
  GetInventory () const
  {
    return inventory;
  }

  Inventory&
  GetInventory ()
  {
    return inventory;
  }

};

/**
 * Utility class to query the ground-loot table and obtain GroundLoot instances
 * from it accordingly.
 */
class GroundLootTable
{

private:

  /** The Database reference for this instance.  */
  Database& db;

public:

  /** Movable handle to a ground-loot instance.  */
  using Handle = std::unique_ptr<GroundLoot>;

  explicit GroundLootTable (Database& d)
    : db(d)
  {}

  GroundLootTable () = delete;
  GroundLootTable (const GroundLootTable&) = delete;
  void operator= (const GroundLootTable&) = delete;

  /**
   * Returns a handle for the instance based on a Database::Result.
   */
  Handle GetFromResult (const Database::Result<GroundLootResult>& res);

  /**
   * Returns a handle for the loot instance at the given coordinate.  If there
   * is not yet any loot, returns a handle for a "newly constructed"
   * entry.
   */
  Handle GetByCoord (const HexCoord& coord);

  /**
   * Queries the database for all non-empty piles of loot on the ground.
   */
  Database::Result<GroundLootResult> QueryNonEmpty ();

};

} // namespace pxd

#endif // DATABASE_INVENTORY_HPP
