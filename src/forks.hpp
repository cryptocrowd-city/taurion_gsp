/*
    GSP for the Taurion blockchain game
    Copyright (C) 2020  Autonomous Worlds Ltd

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

#ifndef PXD_FORKS_HPP
#define PXD_FORKS_HPP

#include <xayagame/gamelogic.hpp>

namespace pxd
{

/**
 * Hardforks that are done on the Taurion game world.
 */
enum class Fork
{

  /**
   * Test fork that does nothing, but is used in unit tests and such
   * for the fork system itself.
   */
  Dummy,

  /**
   * Fork on the 0.3 competition rules to fix issues with blocked spawn
   * areas (from abused starter packs):  New characters will be spawned
   * inside the starter-city building, rather than outside.  Also, vehicles
   * of the same faction are no longer hard obstacles, but instead just
   * slow down movement drastically (but can be passed with enough patience).
   */
  UnblockSpawns,

};

/**
 * Helper class that exposes the state of forks on the network with
 * respect to the current block height and/or block time.
 */
class ForkHandler
{

private:

  /** The chain we are running on.  */
  const xaya::Chain chain;

  /** The block height this is for.  */
  const unsigned height;

public:

  explicit ForkHandler (const xaya::Chain c, const unsigned h)
    : chain(c), height(h)
  {}

  ForkHandler () = delete;
  ForkHandler (const ForkHandler&) = delete;
  void operator= (const ForkHandler&) = delete;

  /**
   * Returns true if the given fork should be considered active.
   */
  bool IsActive (Fork f) const;

};

} // namespace pxd

#endif // PXD_FORKS_HPP