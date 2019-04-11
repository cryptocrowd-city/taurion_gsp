-- Data for the characters in the game.
CREATE TABLE IF NOT EXISTS `characters` (

  -- The character ID, which is assigned based on libxayagame's AutoIds.
  `id` INTEGER PRIMARY KEY,

  -- The Xaya name that owns this character (and is thus allowed to send
  -- moves for it).
  `owner` TEXT NOT NULL,

  -- The faction (as integer corresponding to the Faction enum in C++).
  -- We need this for querying combat targets, which should be possible to
  -- do without decoding the proto.
  `faction` INTEGER NOT NULL,

  -- Current position of the character on the map.  We need this in the table
  -- so that we can look characters up based on "being near" a given other
  -- coordinate.  Coordinates are in the axial system (as everywhere else
  -- in the backend).
  `x` INTEGER NOT NULL,
  `y` INTEGER NOT NULL,

  -- Movement data for the character that changes frequently and is thus
  -- not part of the big main proto.
  `volatilemv` BLOB NOT NULL,

  -- Current HP data as an encoded HP proto.  This is stored directly in
  -- the database rather than the "proto" BLOB since it is a field that
  -- is changed frequently (e.g. during HP regeneration) and without
  -- any explicit action.  Thus having it separate reduces performance
  -- costs and undo data size.
  `hp` BLOB NOT NULL,

  -- If non-zero, then the number represents for how many more blocks the
  -- character is "locked" at being busy (e.g. prospecting).
  `busy` INTEGER NOT NULL,

  -- Flag indicating if the character is currently moving.  This is set
  -- based on the encoded protocol buffer when updating the table, and is
  -- used so that we can efficiently retrieve only those characters that are
  -- moving when we do move updates.
  `ismoving` INTEGER NOT NULL,

  -- Flag indicating whether or not the character has a combat target.
  -- This is used so we can later efficiently retrieve only those characters
  -- that need to be processed for combat damage.
  `hastarget` INTEGER NOT NULL,

  -- Additional data encoded as a Character protocol buffer.
  `proto` BLOB NOT NULL

);

-- Non-unique indices for the characters table.
CREATE INDEX IF NOT EXISTS `characters_owner` ON `characters` (`owner`);
CREATE INDEX IF NOT EXISTS `characters_pos` ON `characters` (`x`, `y`);
CREATE INDEX IF NOT EXISTS `characters_busy` ON `characters` (`busy`);
CREATE INDEX IF NOT EXISTS `characters_ismoving` ON `characters` (`ismoving`);
CREATE INDEX IF NOT EXISTS `characters_hastarget` ON `characters` (`hastarget`);

-- =============================================================================

-- Tracks the damage lists:  Who damaged some character in the last N blocks,
-- so that we can award fame to them later.
CREATE TABLE IF NOT EXISTS `damage_lists` (

  -- The character being attacked.
  `victim` INTEGER NOT NULL,

  -- The attacking character.
  `attacker` INTEGER NOT NULL,

  -- The block height of the last damage done.
  `height` INTEGER NOT NULL,

  -- For each (victim, attacker) combination, there should only be a single
  -- entry.  For each new round of damage done, we "INSERT OR REPLACE" into
  -- the table, which updates the height for existing rows rather than
  -- adding new ones.
  PRIMARY KEY (`victim`, `attacker`)

);

-- Additional indices we need.  Note that lookups per victim are already
-- possible due to the primary key.
CREATE INDEX IF NOT EXISTS `damage_lists_attacker`
    ON `damage_lists` (`attacker`);
CREATE INDEX IF NOT EXISTS `damage_lists_height` ON `damage_lists` (`height`);

-- =============================================================================

-- Data stored for the Xaya accounts (names) themselves.
CREATE TABLE IF NOT EXISTS `accounts` (

  -- The Xaya p/ name of this account.
  `name` TEXT PRIMARY KEY,

  -- The number of characters killed by the account in total.
  `kills` INTEGER NOT NULL,

  -- The fame of this account.
  `fame` INTEGER NOT NULL

);

-- =============================================================================

-- Data for regions where we already have non-trivial data.  Rows here are
-- only created over time, for regions when the first change is made
-- away from the "default / empty" state.
CREATE TABLE IF NOT EXISTS `regions` (

  -- The region ID as defined by the base map data.  Note that ID 0 is a valid
  -- value for one of the regions.  This ranges up to about 700k, but not
  -- all values are real regions.
  `id` INTEGER PRIMARY KEY,

  -- Additional data encoded as a RegionData protocol buffer.
  `proto` BLOB NOT NULL

);

-- =============================================================================

-- Data about the still available prospecting prizes (so that we can
-- ensure only a certain number can be found).
CREATE TABLE IF NOT EXISTS `prizes` (

  -- Name of the prize (as defined in the game params).
  `name` TEXT PRIMARY KEY,

  -- Number of prizes found from this type already.
  `found` INTEGER NOT NULL

);
