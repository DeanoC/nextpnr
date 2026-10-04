// Legality rules for the secondary ALM registers (--mistral-ff4) and the second LAB clock (--mistral-clkb).
// The rules come from Quartus 17.0.2 oracles (mistral/tests/lab_ff4): TCLK_SEL/TCLR_SEL drive FF0+FF3 and
// BCLK_SEL/BCLR_SEL drive FF1+FF2, and a half using both registers owns all three of its outputs.
#include <memory>

#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {
// LAB X30 Y20 is a plain LAB on 5CSEBA6U23I7; X34 Y20 is an MLAB.
constexpr int lab_x = 30, lab_y = 20, mlab_x = 34;

struct Lab
{
    std::unique_ptr<Context> ctx;
    int serial = 0;

    explicit Lab(bool clkb = false)
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        args.lab_clkb = clkb;
        ctx = std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
    }

    NetInfo *net(const std::string &name)
    {
        IdString id = ctx->id(name);
        if (ctx->nets.count(id))
            return ctx->nets.at(id).get();
        return ctx->createNet(id);
    }

    // A flip-flop with the given control nets ("" = unused) and data net.
    CellInfo *ff(const std::string &clk, const std::string &data, const std::string &aclr = "",
                 const std::string &ena = "", bool clk_inv = false, const std::string &sload = "")
    {
        auto cell = ctx->createCell(ctx->idf("ff%d", serial++), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, net(clk));
        if (clk_inv)
            cell->pin_data[id_CLK].state = PIN_INV;
        cell->connectPort(id_DATAIN, net(data));
        cell->connectPort(id_Q, net(cell->name.str(ctx.get()) + "$q"));
        if (!aclr.empty())
            cell->connectPort(id_ACLR, net(aclr));
        else
            cell->pin_data[id_ACLR].state = PIN_1;
        if (!ena.empty())
            cell->connectPort(id_ENA, net(ena));
        else
            cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        if (!sload.empty()) {
            cell->connectPort(id_SLOAD, net(sload));
            cell->connectPort(id_SDATA, net(sload + "$sdata"));
        } else {
            cell->pin_data[id_SLOAD].state = PIN_0;
        }
        ctx->assign_ff_info(cell);
        return cell;
    }

    // A clock on the dedicated network (driven by a clock buffer), which reaches the LAB on CLKIN, not DATAIN
    std::string global_clock(const std::string &name)
    {
        auto buf = ctx->createCell(ctx->id(name + "$buf"), id_MISTRAL_CLKBUF);
        buf->addInput(id_A);
        buf->addOutput(id_Q);
        buf->connectPort(id_A, net(name + "$pad"));
        buf->connectPort(id_Q, net(name));
        return name;
    }

    CellInfo *lut(const std::string &a, const std::string &b, const std::string &out)
    {
        auto cell = ctx->createCell(ctx->idf("lut%d", serial++), id_MISTRAL_ALUT2);
        cell->addInput(id_A);
        cell->addInput(id_B);
        cell->addOutput(id_Q);
        cell->connectPort(id_A, net(a));
        cell->connectPort(id_B, net(b));
        cell->connectPort(id_Q, net(out));
        ctx->assign_comb_info(cell);
        return cell;
    }

    void place_ff(CellInfo *cell, int alm, int slot, int x = lab_x)
    {
        ctx->bindBel(ctx->getBelByLocation(Loc(x, lab_y, alm * 6 + 2 + slot)), cell, STRENGTH_STRONG);
    }

    void place_lut(CellInfo *cell, int alm, int half)
    {
        ctx->bindBel(ctx->getBelByLocation(Loc(lab_x, lab_y, alm * 6 + half)), cell, STRENGTH_STRONG);
    }

    uint32_t lab(int x = lab_x) const
    {
        return ctx->bel_data(ctx->getBelByLocation(Loc(x, lab_y, 2))).lab_data.lab;
    }

    bool alm_legal(int alm = 0, int x = lab_x) const { return ctx->is_alm_legal(lab(x), alm); }
    bool lab_legal(int x = lab_x) const { return ctx->is_lab_ctrlset_legal(lab(x)); }
};
} // namespace

TEST(LabFf4, DefaultModelRejectsSecondaryRegisters)
{
    Lab t;
    t.place_ff(t.ff("clk", "d0"), 0, 0);
    t.place_ff(t.ff("clk", "d1"), 0, 1);
    EXPECT_FALSE(t.alm_legal());
    t.ctx->lab_ff4 = true;
    EXPECT_TRUE(t.alm_legal());
    EXPECT_TRUE(t.lab_legal());
}

TEST(LabFf4, FourFabricRegistersFitOneAlm)
{
    // Quartus case loc4: two route-throughs and two packed registers
    Lab t;
    t.ctx->lab_ff4 = true;
    for (int slot = 0; slot < 4; slot++)
        t.place_ff(t.ff("clk", "d" + std::to_string(slot)), 0, slot);
    EXPECT_TRUE(t.alm_legal());
    EXPECT_TRUE(t.lab_legal());
}

TEST(LabFf4, ClockGroupsAreFf0Ff3AndFf1Ff2)
{
    // Quartus accepts abba and rejects aabb/abab (g_* oracles)
    for (const char *spec : {"abba", "aabb", "abab", "aaab"}) {
        Lab t(true);
        t.ctx->lab_ff4 = true;
        for (int slot = 0; slot < 4; slot++)
            t.place_ff(t.ff(spec[slot] == 'a' ? "clka" : "clkb", "d" + std::to_string(slot)), 0, slot);
        EXPECT_EQ(t.alm_legal(), std::string(spec) == "abba") << spec;
    }
}

TEST(LabFf4, EdgeAndEnableFollowTheClockGroup)
{
    Lab t(true);
    t.ctx->lab_ff4 = true;
    t.place_ff(t.ff("clk", "d0"), 0, 0);
    t.place_ff(t.ff("clk", "d3", "", "", true), 0, 3);
    EXPECT_FALSE(t.alm_legal());
    Lab u(true);
    u.ctx->lab_ff4 = true;
    u.place_ff(u.ff("clk", "d0", "", "e0"), 0, 0);
    u.place_ff(u.ff("clk", "d1", "", "e1"), 0, 1);
    u.place_ff(u.ff("clk", "d2", "", "e1"), 0, 2);
    u.place_ff(u.ff("clk", "d3", "", "e0"), 0, 3);
    EXPECT_TRUE(u.alm_legal());
    EXPECT_TRUE(u.lab_legal());
}

TEST(LabFf4, ClearGroupsAreFf0Ff3AndFf1Ff2)
{
    Lab t;
    t.ctx->lab_ff4 = true;
    t.place_ff(t.ff("clk", "d0", "rst0"), 0, 0);
    t.place_ff(t.ff("clk", "d3"), 0, 3); // open clear would follow TCLR_SEL onto rst0
    EXPECT_FALSE(t.alm_legal());
    Lab u;
    u.ctx->lab_ff4 = true;
    u.place_ff(u.ff("clk", "d0", "rst0"), 0, 0);
    u.place_ff(u.ff("clk", "d1", "rst1"), 0, 1);
    u.place_ff(u.ff("clk", "d2", "rst1"), 0, 2);
    u.place_ff(u.ff("clk", "d3", "rst0"), 0, 3);
    EXPECT_TRUE(u.alm_legal());
    // Two fabric clears and a dedicated clock fit the LAB (Quartus r_abba)
    EXPECT_TRUE(u.lab_legal());
}

TEST(LabFf4, LutWithOtherFanoutCannotShareAFullHalf)
{
    // Quartus rejects lutfan and lutfan_local
    Lab t;
    t.ctx->lab_ff4 = true;
    t.place_lut(t.lut("a", "b", "f"), 0, 0);
    t.place_ff(t.ff("clk", "f"), 0, 0);
    t.place_ff(t.ff("clk", "c"), 0, 1);
    EXPECT_TRUE(t.alm_legal());
    auto other = t.ff("clk", "f");
    t.place_ff(other, 1, 0);
    EXPECT_FALSE(t.alm_legal());
    // The rule follows the netlist: moving the other sink away does not free the half, disconnecting it does
    t.ctx->unbindBel(other->bel);
    EXPECT_FALSE(t.alm_legal());
    other->disconnectPort(id_DATAIN);
    EXPECT_TRUE(t.alm_legal());
}

TEST(LabFf4, SyncLoadKeepsItsHalfToOneRegister)
{
    Lab t;
    t.ctx->lab_ff4 = true;
    t.place_ff(t.ff("clk", "d0", "", "", false, "ld"), 0, 0);
    t.place_ff(t.ff("clk", "d1", "", "", false, "ld"), 0, 1);
    EXPECT_FALSE(t.alm_legal());
}

TEST(LabFf4, MlabsKeepTheTwoRegisterModel)
{
    Lab t;
    t.ctx->lab_ff4 = true;
    t.place_ff(t.ff("clk", "d0"), 0, 0, mlab_x);
    t.place_ff(t.ff("clk", "d1"), 0, 1, mlab_x);
    EXPECT_FALSE(t.alm_legal(0, mlab_x));
}

TEST(LabFf4, SecondClockNeedsClkb)
{
    for (bool clkb : {false, true}) {
        Lab t(clkb);
        t.ctx->lab_ff4 = true;
        t.place_ff(t.ff("clka", "d0"), 0, 0);
        t.place_ff(t.ff("clkb", "d1"), 1, 0);
        EXPECT_EQ(t.lab_legal(), clkb);
        Lab u(clkb);
        u.ctx->lab_ff4 = true;
        u.place_ff(u.ff("clk", "d0"), 0, 0);
        u.place_ff(u.ff("clk", "d1", "", "", true), 1, 0);
        EXPECT_EQ(u.lab_legal(), clkb);
    }
    // At most two clock signals per LAB (Quartus clk3_alm)
    Lab t(true);
    t.ctx->lab_ff4 = true;
    t.place_ff(t.ff("clka", "d0"), 0, 0);
    t.place_ff(t.ff("clkb", "d1"), 1, 0);
    t.place_ff(t.ff("clkc", "d2"), 2, 0);
    EXPECT_FALSE(t.lab_legal());
}

TEST(LabFf4, FabricClockStaysOnClkaWithoutClkb)
{
    // Three enables need DATAIN[2], DATAIN[3] and DATAIN[0]. A fabric clock also needs DATAIN[0] on CLKA.
    // CLKB would free DATAIN[0], but that pip is absent unless --mistral-clkb is on; reserving it asserted.
    auto place = [](Lab &t) {
        t.ctx->lab_ff4 = true;
        t.place_ff(t.ff("clk", "d0", "", "e0"), 0, 0);
        t.place_ff(t.ff("clk", "d1", "", "e1"), 1, 0);
        t.place_ff(t.ff("clk", "d2", "", "e2"), 2, 0);
    };
    Lab t;
    place(t);
    EXPECT_FALSE(t.lab_legal());
    t.ctx->assign_control_sets(t.lab());
    Lab u(true);
    place(u);
    EXPECT_TRUE(u.lab_legal());
    u.ctx->assign_control_sets(u.lab());
}

TEST(LabFf4, AtMostThreeClockEnablePairs)
{
    Lab t(true);
    t.ctx->lab_ff4 = true;
    t.global_clock("clka");
    t.global_clock("clkb");
    t.place_ff(t.ff("clka", "d0", "", "e0"), 0, 0);
    t.place_ff(t.ff("clka", "d1", "", "e1"), 1, 0);
    t.place_ff(t.ff("clkb", "d2", "", "e2"), 2, 0);
    EXPECT_TRUE(t.lab_legal()); // Quartus ena3_alm
    t.place_ff(t.ff("clkb", "d3"), 3, 0);
    EXPECT_FALSE(t.lab_legal());
    // Fabric clocks take DATAIN[0] (CLKA) and DATAIN[1] (CLKB); the third enable then has no DATAIN line
    Lab u(true);
    u.ctx->lab_ff4 = true;
    u.place_ff(u.ff("clka", "d0", "", "e0"), 0, 0);
    u.place_ff(u.ff("clka", "d1", "", "e1"), 1, 0);
    EXPECT_TRUE(u.lab_legal());
    u.place_ff(u.ff("clkb", "d2", "", "e2"), 2, 0);
    EXPECT_FALSE(u.lab_legal());
}
