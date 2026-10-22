/* Physical RBF proof for post-route LUT moves. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
#include "cyclonev.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

USING_NEXTPNR_NAMESPACE
namespace {
using CV = mistral::CycloneV;

class RoutedMovePhysical : public ::testing::TestWithParam<std::tuple<bool, int>>
{
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t lab = 0;
    int serial = 0;
    const std::array<IdString, 6> logical{id_A, id_B, id_C, id_D, id_E, id_F};

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        // Choose an ordinary tile of the requested physical type. All fixture
        // cells fit in this one tile, avoiding an incidental long-route test.
        bool found = false;
        int best = std::numeric_limits<int>::max();
        for (uint32_t i = 0; i < ctx->labs.size(); ++i) {
            if (ctx->labs[i].is_mlab != std::get<0>(GetParam()))
                continue;
            auto loc = ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);
            int distance = std::abs(loc.x - 24) + std::abs(loc.y - 3);
            if (distance < best) {
                best = distance;
                lab = i;
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }

    BelId bel(int alm, int half) { return ctx->labs.at(lab).alms.at(alm).lut_bels.at(half); }

    CellInfo *lut(int alm, int half, int width, uint64_t init)
    {
        const std::array<IdString, 5> types{
                id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
        auto *cell = ctx->createCell(ctx->idf("physical_lut%d", serial++), types.at(width - 2));
        cell->params[id_LUT] = Property(int64_t(init), 1 << width);
        for (int i = 0; i < width; ++i) {
            cell->addInput(logical[i]);
            cell->pin_data[logical[i]].state = PIN_SIG;
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", ctx->nameOf(cell))));
        ctx->assign_comb_info(cell);
        ctx->assign_default_pinmap(cell);
        ctx->bindBel(bel(alm, half), cell, STRENGTH_WEAK);
        return cell;
    }

    CellInfo *driver(int alm)
    {
        auto *cell = lut(alm, 0, 2, 8);
        cell->pin_data[id_A].state = PIN_0;
        cell->pin_data[id_B].state = PIN_1;
        return cell;
    }

    void feed(CellInfo *cell, int width, int first_driver_alm, unsigned inversions)
    {
        for (int i = 0; i < width; ++i) {
            auto *source = driver(first_driver_alm + i);
            cell->connectPort(logical[i], source->getPort(id_Q));
            cell->pin_data[logical[i]].state = ((inversions >> i) & 1) ? PIN_INV : PIN_SIG;
        }
    }

    void output_sink(CellInfo *source)
    {
        auto *sink = lut(7, 0, 2, 8);
        sink->connectPort(id_A, source->getPort(id_Q));
        sink->pin_data[id_B].state = PIN_1;
    }

    bool route_fixture()
    {
        ctx->assignArchInfo();
        for (uint8_t alm = 0; alm < 10; ++alm)
            ctx->assign_alm_lut_inputs(lab, alm);
        for (const auto &entry : ctx->cells)
            if (!ctx->isBelLocationValid(entry.second->bel))
                return false;
        int visits = 2000000;
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            if (net->driver.cell && !critical_local_route_net(ctx.get(), net, visits))
                return false;
        }
        ctx->configure_bitstream();
        return true;
    }

    std::unique_ptr<CV> roundtrip()
    {
        std::vector<uint8_t> bytes;
        ctx->cyclonev->rbf_save(bytes);
        auto decoded = std::unique_ptr<CV>(CV::get_model("5CSEBA6U23I7"));
        if (!decoded || bytes.empty())
            return nullptr;
        decoded->rbf_load(bytes.data(), uint32_t(bytes.size()));
        return decoded;
    }

    bool emitted_mask(const CV &decoded, BelId site, uint64_t &mask)
    {
        const auto &location = ctx->bel_data(site).lab_data;
        CV::bmux_setting_t setting;
        if (!decoded.bmux_get(ctx->labs.at(location.lab).is_mlab ? CV::MLAB : CV::LAB,
                              site.pos, CV::LUT_MASK, location.alm, setting) ||
            setting.type != CV::MT_RAM || setting.r.size() != 8)
            return false;
        mask = 0;
        for (unsigned i = 0; i < 8; ++i)
            mask |= uint64_t(setting.r[i]) << (8 * i);
        return true;
    }

    void expect_function(const CV &decoded, CellInfo *cell, int width, uint64_t init, unsigned inversions)
    {
        const auto &location = ctx->bel_data(cell->bel).lab_data;
        const int half = location.idx;
        const bool l6 = width == 6;
        const auto block = ctx->labs.at(location.lab).is_mlab ? CV::MLAB : CV::LAB;
        CV::bmux_setting_t top, bottom;
        ASSERT_TRUE(decoded.bmux_get(block, cell->bel.pos, CV::TMODE, location.alm, top));
        ASSERT_TRUE(decoded.bmux_get(block, cell->bel.pos, CV::BMODE, location.alm, bottom));
        EXPECT_EQ(top.s, uint32_t(l6 ? CV::D_E : CV::E_0));
        EXPECT_EQ(bottom.s, uint32_t(l6 ? CV::C_E : CV::E_1));
        uint64_t actual;
        ASSERT_TRUE(emitted_mask(decoded, cell->bel, actual));
        // Independent physical-address oracle: C/D exchange address bits 2/3
        // between the two LUT6 banks, E selects bank 5, and F is address bit 4.
        // A LUT3 occupies this half's C/D, E, F. Do not call compute_lut_mask.
        const std::vector<IdString> pins = l6
                ? std::vector<IdString>{id_A, id_B, id_C, id_D, half ? id_E1 : id_E0, half ? id_F1 : id_F0}
                : std::vector<IdString>{half ? id_D : id_C, half ? id_E1 : id_E0, half ? id_F1 : id_F0};
        ASSERT_EQ(pins.size(), size_t(width));
        for (int i = 0; i < width; ++i) {
            ASSERT_EQ(cell->pin_data.at(logical[i]).bel_pins, std::vector<IdString>{pins[i]});
            EXPECT_EQ(cell->pin_data.at(logical[i]).state, ((inversions >> i) & 1) ? PIN_INV : PIN_SIG);
        }
        const std::array<unsigned, 64> mlab_permutation{
                0, 1, 4, 5, 8, 9, 12, 13, 29, 28, 25, 24, 21, 20, 17, 16,
                2, 3, 6, 7, 10, 11, 14, 15, 31, 30, 27, 26, 23, 22, 19, 18,
                32, 33, 36, 37, 40, 41, 44, 45, 61, 60, 57, 56, 53, 52, 49, 48,
                34, 35, 38, 39, 42, 43, 46, 47, 63, 62, 59, 58, 55, 54, 51, 50};
        for (unsigned address = 0; address < (l6 ? 64u : 32u); ++address) {
            std::array<unsigned, 6> bits = l6
                    ? std::array<unsigned, 6>{0, 1, address < 32 ? 2u : 3u, address < 32 ? 3u : 2u, 5, 4}
                    : std::array<unsigned, 6>{2, 3, 4, 0, 0, 0};
            unsigned index = 0;
            for (int i = 0; i < width; ++i) {
                // ALM data inputs are physically inverted; PIN_INV cancels it.
                bool value = ((address >> bits[i]) & 1) != (((inversions >> i) & 1) == 0);
                index |= unsigned(value) << i;
            }
            unsigned physical = address + (l6 ? 0 : 32 * half);
            if (block == CV::MLAB)
                physical = mlab_permutation.at(physical);
            EXPECT_EQ((actual >> physical) & 1, 1u ^ ((init >> index) & 1))
                    << "cell=" << ctx->nameOf(cell) << " address=" << address << " logical=" << index;
        }
    }
};

TEST_P(RoutedMovePhysical, AsymmetricLut6WithInversionSurvivesRbfRoundtrip)
{
    const int half = std::get<1>(GetParam());
    constexpr uint64_t init = 0x92ad6c1f58e3b407ULL;
    constexpr unsigned inversions = (1u << 2) | (1u << 4);
    auto *moved = lut(0, half, 6, init);
    feed(moved, 6, 1, inversions);
    output_sink(moved);
    ASSERT_TRUE(route_fixture());
    auto baseline = roundtrip();
    ASSERT_TRUE(baseline);
    expect_function(*baseline, moved, 6, init, inversions);
    ASSERT_TRUE(critical_routed_move_trial(ctx.get(), moved, bel(8, half), 500000, [] { return true; }));
    auto candidate = roundtrip();
    ASSERT_TRUE(candidate);
    expect_function(*candidate, moved, 6, init, inversions);
}

TEST_P(RoutedMovePhysical, DestinationMateKeepsItsEmittedFunctionAndMask)
{
    const int half = std::get<1>(GetParam());
    constexpr uint64_t moved_init = 0xd2, mate_init = 0x8e;
    auto *moved = lut(0, half, 3, moved_init);
    auto *mate = lut(8, 1 - half, 3, mate_init);
    feed(moved, 3, 1, 1u << 1);
    feed(mate, 3, 4, 1u << 2);
    output_sink(moved);
    ASSERT_TRUE(route_fixture());
    auto baseline = roundtrip();
    ASSERT_TRUE(baseline);
    expect_function(*baseline, moved, 3, moved_init, 1u << 1);
    expect_function(*baseline, mate, 3, mate_init, 1u << 2);
    uint64_t old_mask;
    ASSERT_TRUE(emitted_mask(*baseline, mate->bel, old_mask));
    ASSERT_TRUE(critical_routed_move_trial(ctx.get(), moved, bel(8, half), 500000, [] { return true; }));
    auto candidate = roundtrip();
    ASSERT_TRUE(candidate);
    expect_function(*candidate, moved, 3, moved_init, 1u << 1);
    expect_function(*candidate, mate, 3, mate_init, 1u << 2);
    uint64_t new_mask;
    ASSERT_TRUE(emitted_mask(*candidate, mate->bel, new_mask));
    const uint64_t mate_bits = uint64_t(0xffffffff) << (32 * (1 - half));
    EXPECT_EQ(old_mask & mate_bits, new_mask & mate_bits);
}

INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves, RoutedMovePhysical,
                        ::testing::Combine(::testing::Bool(), ::testing::Values(0, 1)));
} // namespace
