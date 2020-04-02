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

#ifndef PROTO_ROITEMS_HPP
#define PROTO_ROITEMS_HPP

#include "config.pb.h"

#include <string>

namespace pxd
{

/**
 * Looks up and returns the configuration data for the given type of item
 * (or null if there is no such item).  This automatically "constructs" some
 * things (e.g. blueprints, tech levels) instead of just looking data up
 * in the real roconfig proto.  It should always be used instead of a direct
 * access for items.
 */
const proto::ItemData* RoItemDataOrNull (const std::string& item);

/**
 * Looks up item data, asserting that the item exists.
 */
const proto::ItemData& RoItemData (const std::string& item);

} // namespace pxd

#endif // PROTO_ROITEMS_HPP