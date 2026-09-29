#include "gtest/gtest.h"
#include "local_remap_policy.h"

using namespace local_remap_policy;

TEST(LocalRemapComposition, ExhaustiveTwoInputTablesAndInvertedIntermediate)
{
    for (unsigned inner = 0; inner < 16; ++inner)
        for (unsigned outer = 0; outer < 16; ++outer)
            for (bool inverted : {false, true}) {
                auto c = compose(inner, {{0, false}, {1, false}}, outer,
                                 {{INTERMEDIATE, inverted}, {2, false}});
                ASSERT_TRUE(c.valid);
                EXPECT_EQ(c.signals, (std::vector<int>{0, 1, 2}));
                for (unsigned row = 0; row < 8; ++row) {
                    unsigned mid = ((inner >> (row & 3)) & 1) ^ inverted;
                    bool expected = (outer >> (mid | ((row >> 2) << 1))) & 1;
                    EXPECT_EQ((c.mask >> row) & 1, expected);
                }
            }
}

TEST(LocalRemapComposition, ConstantsAliasesAndInputInversion)
{
    // inner = !a XOR 1 = a; outer = inner AND !a = 0.
    auto c = compose(0x6, {{7, true}, {ONE, false}}, 0x8,
                     {{INTERMEDIATE, false}, {7, true}});
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.signals, (std::vector<int>{7}));
    EXPECT_EQ(c.mask, 0u);
    // Both outer inputs alias the intermediate, with opposite polarities.
    auto duplicate = compose(0x8, {{0, false}, {ZERO, true}}, 0xe,
                            {{INTERMEDIATE, false}, {INTERMEDIATE, true}});
    ASSERT_TRUE(duplicate.valid);
    EXPECT_EQ(duplicate.mask, 3u);
}

TEST(LocalRemapComposition, SixInputBit63AndBounds)
{
    auto c = compose(0x8000000000000000ULL,
                     {{0,false},{1,false},{2,false},{3,false},{4,false},{5,false}},
                     0x8, {{INTERMEDIATE,false},{ONE,false}});
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.mask, 0x8000000000000000ULL);
    EXPECT_FALSE(compose(0x8, {{0,false},{1,false}}, 0,
                         {{INTERMEDIATE,false},{2,false},{3,false},{4,false},{5,false},{6,false}}).valid);
    EXPECT_FALSE(compose(0, {{INTERMEDIATE,false},{0,false}}, 0,
                         {{INTERMEDIATE,false},{1,false}}).valid);
    EXPECT_FALSE(compose(0, {{0,false},{1,false}}, 0, {{0,false},{1,false}}).valid);
}
