#include "character.hpp"

#include "dbtest.hpp"

#include "proto/character.pb.h"

#include <gtest/gtest.h>

namespace pxd
{
namespace
{

class CharacterTests : public DBTestWithSchema
{

protected:

  /** CharacterTable instance for tests.  */
  CharacterTable tbl;

  CharacterTests ()
    : tbl(db)
  {}

};

TEST_F (CharacterTests, Creation)
{
  const HexCoord pos(5, -2);

  auto c = tbl.CreateNew  ("domob", "abc", Faction::RED);
  c->SetPosition (pos);
  const auto id1 = c->GetId ();
  c->MutableHP ().set_armour (10);
  c.reset ();

  c = tbl.CreateNew ("domob", u8"äöü", Faction::GREEN);
  const auto id2 = c->GetId ();
  c->MutableProto ().mutable_movement ();
  c.reset ();

  auto res = tbl.QueryAll ();

  ASSERT_TRUE (res.Step ());
  c = tbl.GetFromResult (res);
  ASSERT_EQ (c->GetId (), id1);
  EXPECT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetName (), "abc");
  EXPECT_EQ (c->GetFaction (), Faction::RED);
  EXPECT_EQ (c->GetPosition (), pos);
  EXPECT_EQ (c->GetHP ().armour (), 10);
  EXPECT_FALSE (c->GetProto ().has_movement ());

  ASSERT_TRUE (res.Step ());
  c = tbl.GetFromResult (res);
  ASSERT_EQ (c->GetId (), id2);
  EXPECT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetName (), u8"äöü");
  EXPECT_EQ (c->GetFaction (), Faction::GREEN);
  EXPECT_TRUE (c->GetProto ().has_movement ());

  ASSERT_FALSE (res.Step ());
}

TEST_F (CharacterTests, ModificationWithProto)
{
  const HexCoord pos(-2, 5);

  tbl.CreateNew ("domob", "foo", Faction::RED);

  auto res = tbl.QueryAll ();
  ASSERT_TRUE (res.Step ());
  auto c = tbl.GetFromResult (res);
  EXPECT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetPosition (), HexCoord (0, 0));
  EXPECT_EQ (c->GetPartialStep (), 0);
  EXPECT_FALSE (c->GetHP ().has_shield ());
  EXPECT_FALSE (c->GetProto ().has_movement ());
  ASSERT_FALSE (res.Step ());

  c->SetOwner ("andy");
  c->SetPosition (pos);
  c->SetPartialStep (10);
  c->MutableHP ().set_shield (5);
  c->MutableProto ().mutable_movement ();
  c.reset ();

  res = tbl.QueryAll ();
  ASSERT_TRUE (res.Step ());
  c = tbl.GetFromResult (res);
  EXPECT_EQ (c->GetOwner (), "andy");
  EXPECT_EQ (c->GetName (), "foo");
  EXPECT_EQ (c->GetFaction (), Faction::RED);
  EXPECT_EQ (c->GetPosition (), pos);
  EXPECT_EQ (c->GetPartialStep (), 10);
  EXPECT_EQ (c->GetHP ().shield (), 5);
  EXPECT_TRUE (c->GetProto ().has_movement ());
  ASSERT_FALSE (res.Step ());
}

TEST_F (CharacterTests, ModificationFieldsOnly)
{
  const HexCoord pos(-2, 5);

  tbl.CreateNew ("domob", "foo", Faction::RED);

  auto c = tbl.GetById (1);
  ASSERT_TRUE (c != nullptr);
  c->SetOwner ("andy");
  c->SetPosition (pos);
  c->SetPartialStep (42);
  c->MutableHP ().set_shield (5);
  c.reset ();

  c = tbl.GetById (1);
  ASSERT_TRUE (c != nullptr);
  EXPECT_EQ (c->GetName (), "foo");
  EXPECT_EQ (c->GetOwner (), "andy");
  EXPECT_EQ (c->GetFaction (), Faction::RED);
  EXPECT_EQ (c->GetPosition (), pos);
  EXPECT_EQ (c->GetPartialStep (), 42);
  EXPECT_EQ (c->GetHP ().shield (), 5);
}

TEST_F (CharacterTests, EmptyNameNotAllowed)
{
  EXPECT_DEATH ({
    tbl.CreateNew ("domob", "", Faction::RED);
  }, "name");
}

using CharacterTableTests = CharacterTests;

TEST_F (CharacterTableTests, GetById)
{
  const auto id1 = tbl.CreateNew ("domob", "abc", Faction::RED)->GetId ();
  const auto id2 = tbl.CreateNew ("domob", "foo", Faction::RED)->GetId ();

  CHECK (tbl.GetById (500) == nullptr);
  CHECK_EQ (tbl.GetById (id1)->GetName (), "abc");
  CHECK_EQ (tbl.GetById (id2)->GetName (), "foo");
}

TEST_F (CharacterTableTests, QueryForOwner)
{
  tbl.CreateNew ("domob", "abc", Faction::RED);
  tbl.CreateNew ("domob", "foo", Faction::RED);
  tbl.CreateNew ("andy", "test", Faction::RED);

  auto res = tbl.QueryForOwner ("domob");
  ASSERT_TRUE (res.Step ());
  EXPECT_EQ (tbl.GetFromResult (res)->GetName (), "abc");
  ASSERT_TRUE (res.Step ());
  EXPECT_EQ (tbl.GetFromResult (res)->GetName (), "foo");
  ASSERT_FALSE (res.Step ());

  res = tbl.QueryForOwner ("not there");
  ASSERT_FALSE (res.Step ());
}

TEST_F (CharacterTableTests, QueryMoving)
{
  tbl.CreateNew ("domob", "foo", Faction::RED);
  tbl.CreateNew ("domob", "bar", Faction::RED)
    ->MutableProto ().mutable_movement ();

  auto res = tbl.QueryMoving ();
  ASSERT_TRUE (res.Step ());
  EXPECT_EQ (tbl.GetFromResult (res)->GetName (), "bar");
  ASSERT_FALSE (res.Step ());
}

TEST_F (CharacterTableTests, IsValidName)
{
  tbl.CreateNew ("domob", "abc", Faction::RED);

  EXPECT_FALSE (tbl.IsValidName (""));
  EXPECT_FALSE (tbl.IsValidName ("abc"));
  EXPECT_TRUE (tbl.IsValidName ("foo"));
}

} // anonymous namespace
} // namespace pxd
