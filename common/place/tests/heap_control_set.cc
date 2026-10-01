#include "gtest/gtest.h"
#include "heap_control_set.h"

USING_NEXTPNR_NAMESPACE

TEST(HeapControlSetTest, ExclusiveGroupReopensAfterLastResidentLeaves)
{
    HeapControlSetState group;
    EXPECT_TRUE(group.check(7));
    group.bind(7);
    group.bind(7);
    EXPECT_TRUE(group.check(7));
    EXPECT_FALSE(group.check(9));
    group.unbind(7);
    EXPECT_EQ(group.count, 1);
    EXPECT_TRUE(group.members.count(7));
    EXPECT_FALSE(group.check(9));
    group.unbind(7);
    EXPECT_EQ(group.count, 0);
    EXPECT_TRUE(group.members.empty());
    EXPECT_TRUE(group.check(9));
}

TEST(HeapControlSetTest, MixedSetEvictionPreservesOtherResidents)
{
    HeapControlSetState group;
    group.bind(7);
    group.bind(7);
    group.bind(9);
    EXPECT_FALSE(group.check(7));
    EXPECT_FALSE(group.check(9));
    group.unbind(7);
    EXPECT_EQ(group.count, 2);
    EXPECT_TRUE(group.members.count(7));
    EXPECT_TRUE(group.members.count(9));
    group.unbind(9);
    EXPECT_EQ(group.count, 1);
    EXPECT_FALSE(group.members.count(9));
    EXPECT_TRUE(group.check(7));
    group.unbind(7);
    EXPECT_EQ(group.count, 0);
    EXPECT_TRUE(group.members.empty());
}

TEST(HeapControlSetTest, ReplacingAnEvictedSetDoesNotLoseItsNeighbour)
{
    HeapControlSetState group;
    group.bind(7);
    group.bind(9);
    group.unbind(7);
    group.bind(11);
    EXPECT_FALSE(group.members.count(7));
    EXPECT_TRUE(group.members.count(9));
    EXPECT_TRUE(group.members.count(11));
    EXPECT_EQ(group.count, 2);
    group.unbind(11);
    EXPECT_TRUE(group.check(9));
}

TEST(HeapControlSetTest, UnknownEvictionFailsWithoutChangingResidents)
{
    HeapControlSetState group;
    group.bind(7);
    EXPECT_THROW(group.unbind(9), assertion_failure);
    EXPECT_EQ(group.count, 1);
    EXPECT_TRUE(group.check(7));
}
