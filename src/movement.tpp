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

/* Inline implementation code for movement.hpp.  */

namespace pxd
{

PathFinder::DistanceT
MovementEdgeWeight (const BaseMap& map, const Faction f,
                    const HexCoord& from, const HexCoord& to)
{
  const auto baseWeight = map.GetEdgeWeight (from, to);
  if (baseWeight == PathFinder::NO_CONNECTION)
    return PathFinder::NO_CONNECTION;

  /* Starter zones are obstacles to other factions, but allow 3x
     faster movement to the matching faction.  */
  const auto toStarter = map.SafeZones ().StarterFor (to);
  if (toStarter == Faction::INVALID)
    return baseWeight;
  if (toStarter == f)
    return baseWeight / 3;
  return PathFinder::NO_CONNECTION;
}

} // namespace pxd
