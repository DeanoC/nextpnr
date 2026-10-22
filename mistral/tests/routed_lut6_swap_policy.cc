#include "gtest/gtest.h"
#include "routed_lut6_swap_policy.h"
#include <cstdint>

using routed_lut6_swap_policy::symmetric;
TEST(RoutedLut6SwapPolicy, RejectsInvalidPinsAndAsymmetricTables)
{
    EXPECT_FALSE(symmetric(0,0,0));EXPECT_FALSE(symmetric(0,6,1));EXPECT_FALSE(symmetric(0,1,99));
    for(unsigned first=0;first<6;++first) for(unsigned second=0;second<6;++second) if(first!=second) {
        uint64_t mask=0;
        for(unsigned row=0;row<64;++row) if((row>>first)&1) mask|=uint64_t(1)<<row;
        EXPECT_FALSE(symmetric(mask,first,second));
    }
}
TEST(RoutedLut6SwapPolicy, ProvesEveryOrAndParityTransposition)
{
    uint64_t parity=0;
    for(unsigned row=0;row<64;++row) if(__builtin_parity(row)) parity|=uint64_t(1)<<row;
    for(auto mask:{~uint64_t(1),uint64_t(1)<<63,parity})
        for(unsigned first=0;first<6;++first) for(unsigned second=first+1;second<6;++second)
            EXPECT_TRUE(symmetric(mask,first,second));
}
TEST(RoutedLut6SwapPolicy, IndependentRandomPairTruthProofAndCorruption)
{
    uint64_t random=0x5f673a899c42ed01ULL;
    for(unsigned first=0;first<6;++first) for(unsigned second=first+1;second<6;++second) {
        for(unsigned trial=0;trial<32;++trial) {
            uint64_t mask=0;
            for(unsigned row=0;row<64;++row) {
                unsigned values[6];for(unsigned p=0;p<6;++p) values[p]=(row>>p)&1;
                unsigned temporary=values[first];values[first]=values[second];values[second]=temporary;
                unsigned swapped=0;for(unsigned p=0;p<6;++p)swapped|=values[p]<<p;
                if(row>swapped)continue;
                random^=random<<13;random^=random>>7;random^=random<<17;
                if(random&1)mask|=(uint64_t(1)<<row)|(uint64_t(1)<<swapped);
            }
            ASSERT_TRUE(symmetric(mask,first,second));
            EXPECT_FALSE(symmetric(mask^(uint64_t(1)<<(1u<<first)),first,second));
        }
    }
}
