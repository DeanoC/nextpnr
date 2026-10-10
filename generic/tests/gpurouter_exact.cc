// This file also builds as a small standalone differential corpus runner. The
// retained pre-change backend and current backend consume exactly the same cases.
#include "gpu/gpuroute_backend.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef GPUROUTE_EXACT_STANDALONE
#include <iostream>
#else
#include "gtest/gtest.h"
#endif

namespace {
using namespace gpuroute;
struct Graph
{
    std::vector<std::vector<std::pair<int, float>>> rows;
    std::vector<int32_t> offsets, destinations, occupancy, reservations;
    std::vector<float> costs, history;
    std::vector<int16_t> x, y;
    std::vector<uint8_t> flags;
    explicit Graph(int n) : rows(n), occupancy(n, 0), reservations(n, -1), history(n, 1), x(n), y(n), flags(n, 0)
    {
        for (int i = 0; i < n; ++i) {
            x[i] = i % 8;
            y[i] = i / 8;
        }
    }
    void edge(int src, int dst, float cost) { rows[src].emplace_back(dst, cost); }
    GraphData data()
    {
        offsets.clear(); destinations.clear(); costs.clear();
        for (const auto &row : rows) {
            offsets.push_back(int(destinations.size()));
            for (const auto &e : row) {
                destinations.push_back(e.first);
                costs.push_back(e.second);
            }
        }
        offsets.push_back(int(destinations.size()));
        GraphData result;
        result.n_wires = int(rows.size()); result.n_edges = destinations.size();
        result.out_off = offsets.data(); result.out_dst = destinations.data(); result.edge_cost = costs.data();
        result.wire_x = x.data(); result.wire_y = y.data(); result.wire_flags = flags.data();
        result.wire_occ = occupancy.data(); result.wire_hist = history.data(); result.wire_reserved = reservations.data();
        return result;
    }
};
struct Result
{
    std::vector<ArcResult> arcs;
    std::vector<PathEntry> paths;
};
RouteParams parameters()
{
    RouteParams p;
    p.exact = 1; p.est_weight = 0; p.bias_factor = 0;
    p.expand_k = 1; p.expand_div = 0;
    return p;
}
Result run(Graph &graph, RouteParams p, const std::vector<int32_t> &seeds, const std::vector<float> &delays,
           const std::vector<float> &loads, const std::vector<ArcDesc> &arcs, int path_cap = 128,
           int bits = 10, bool bounded = false, bool large = true)
{
    auto backend = create_cpu_backend();
    std::string error;
    const auto data = graph.data();
    if (!backend->init(data, 1, bits, 1, bits + 1, error))
        throw std::runtime_error(error);
    TaskDesc t;
    t.net = 7; t.tree_cnt = int(seeds.size()); t.arc_cnt = int(arcs.size());
    t.path_cap = path_cap; t.fanout = int(arcs.size()); t.hpwl = 10;
    t.bb_x0 = 0; t.bb_y0 = 0; t.bb_x1 = 3; t.bb_y1 = 3;
    p.use_bb = bounded;
    Result result;
    backend->route(p, {t}, arcs, seeds, delays, loads, large, result.arcs, result.paths);
    return result;
}
uint32_t bits(float value)
{
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
std::string serialize(const Result &result)
{
    // Includes unused/failed output slots as well as every ArcResult field.
    // Work counts, parent edges and bitwise float values must all agree.
    std::ostringstream out;
    for (const auto &arc : result.arcs)
        out << arc.status << ',' << arc.reason << ',' << std::hex << bits(arc.cost) << std::dec << ','
            << arc.expanded << ',' << arc.steps << ',' << arc.path_off << ',' << arc.path_len << ';';
    out << '|';
    for (const auto &path : result.paths)
        out << path.wire << ',' << path.parent << ',' << path.edge << ',' << std::hex << bits(path.delay) << std::dec << ';';
    return out.str();
}
// Minimal SHA-256 so the test can pin the corpus to the pre-heap baseline.
std::string sha256(const std::string &message)
{
    static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> data(message.begin(), message.end());
    const uint64_t length = uint64_t(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56)
        data.push_back(0);
    for (int i = 7; i >= 0; --i)
        data.push_back(uint8_t(length >> (i * 8)));
    auto rotr = [](uint32_t v, int s) { return (v >> s) | (v << (32 - s)); };
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(data[chunk + 4 * i]) << 24 | uint32_t(data[chunk + 4 * i + 1]) << 16 |
                   uint32_t(data[chunk + 4 * i + 2]) << 8 | uint32_t(data[chunk + 4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::ostringstream out;
    for (uint32_t v : h)
        out << std::hex << std::setw(8) << std::setfill('0') << v;
    return out.str();
}
std::vector<std::string> corpus()
{
    std::vector<std::string> result;
    uint32_t random = 0x51eedu;
    auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
    for (int example = 0; example < 160; ++example) {
        Graph graph(8 + next() % 73);
        const int n = int(graph.rows.size());
        for (int wire = 0; wire < n; ++wire) {
            const int edges = 2 + next() % 9;
            for (int i = 0; i < edges; ++i) {
                const int dst = next() % n;
                const float cost = float(next() % 13) * 0.25f;
                graph.edge(wire, dst, cost); // cycles, ties, zero edges
            }
            if (wire % 7 == 0) graph.edge(wire, (wire + 1) % n, -1); // unusable edge
            if (wire % 11 == 0) graph.flags[wire] = WIRE_UNAVAILABLE;
            if (wire % 13 == 0) graph.reservations[wire] = 8 | RESERVED_SOFT;
            if (wire % 17 == 0) graph.reservations[wire] = 8;
            graph.occupancy[wire] = next() % 3;
            graph.history[wire] = 1 + float(next() % 8) * 0.125f;
        }
        for (int setting = 0; setting < 16; ++setting) {
            auto p = parameters();
            p.exact = setting < 12;
            p.est_weight = setting % 3 == 0 ? 0 : 1.25f;
            p.est_x = 0.125f; p.est_y = 0.25f;
            p.seed_delay_weight = setting % 2 ? 0.75f : 1;
            p.seed_delay_floor = 0.25f;
            p.load_penalty = setting % 3 ? 0 : 0.5f;
            p.pip_adder = setting % 4 ? 0 : 0.125f;
            p.bias_factor = setting % 5 ? 0 : 0.125f;
            p.ignore_soft = setting % 2;
            p.ignore_hist = setting % 3 == 1;
            p.expand_k = setting % 3 + 1; p.expand_div = setting % 2 ? 0 : 3;
            const std::vector<int32_t> seeds = {0, 1, 1, 2, 3};
            const std::vector<float> delays = {0, setting % 2 ? -1.0f : 0.5f, 9, -1, 0.25f};
            const std::vector<float> loads = {2, 0, 0, 0, 1};
            const std::vector<ArcDesc> arcs = {{n - 1, setting % 2 ? 0.0f : 0.5f, 1}, {n - 2, 0.25f, 0.5f}};
            result.push_back(serialize(run(graph, p, seeds, delays, loads, arcs,
                                          setting % 5 == 0 ? 1 : 2 * n, setting % 4 == 0 ? 3 : 10,
                                          setting % 2, setting % 3 != 1)));
        }
    }
    return result;
}

#ifndef GPUROUTE_EXACT_STANDALONE
TEST(GpuExactSearch, EqualCostFrontierChoosesLowerWireThenSinkEdge)
{
    Graph graph(4);
    graph.edge(0, 2, 1); graph.edge(0, 1, 1); // reverse insertion order
    graph.edge(1, 3, 1); graph.edge(2, 3, 1);
    auto result = run(graph, parameters(), {0}, {0}, {0}, {{3, 0, 1}});
    ASSERT_EQ(result.arcs[0].status, ARC_OK);
    EXPECT_EQ(result.arcs[0].cost, 2);
    EXPECT_EQ(result.arcs[0].expanded, 3);
    EXPECT_EQ(result.arcs[0].steps, 3);
    EXPECT_EQ(result.paths[0].parent, 1);
    EXPECT_EQ(result.paths[0].edge, 2);
}

TEST(GpuExactSearch, DecreasedFrontierEntryDiscardsObsoleteG)
{
    Graph graph(5);
    graph.edge(0, 1, 8); graph.edge(0, 2, 1);
    graph.edge(1, 4, 10); graph.edge(2, 1, 1);
    auto result = run(graph, parameters(), {0}, {0}, {0}, {{4, 0, 1}});
    ASSERT_EQ(result.arcs[0].status, ARC_OK);
    EXPECT_EQ(result.arcs[0].cost, 12);
    EXPECT_EQ(result.arcs[0].expanded, 3); // stale g=8 entry must not expand
    ASSERT_EQ(result.arcs[0].path_len, 3);
    EXPECT_EQ(result.paths[0].parent, 1);
    EXPECT_EQ(result.paths[1].parent, 2);
}

TEST(GpuExactSearch, EqualGParentUpdateDoesNotQueueAgain)
{
    Graph graph(5);
    graph.edge(0, 2, 0); graph.edge(0, 1, 1);
    graph.edge(1, 3, 0); graph.edge(2, 3, 1); graph.edge(3, 4, 2);
    auto result = run(graph, parameters(), {0}, {0}, {0}, {{4, 0, 1}});
    ASSERT_EQ(result.arcs[0].status, ARC_OK);
    EXPECT_EQ(result.arcs[0].expanded, 4);
    EXPECT_EQ(result.arcs[0].cost, 3);
    EXPECT_EQ(result.paths[1].parent, 1); // later lower CSR edge wins at equal g
}

TEST(GpuExactSearch, DuplicateAndBlockedSeedsPreserveFirstInsertionAndLoadCost)
{
    Graph graph(4);
    graph.edge(0, 1, 0); graph.edge(0, 2, 1); graph.edge(1, 3, 0); graph.edge(2, 3, 1);
    auto p = parameters(); p.load_penalty = 0.5f;
    auto result = run(graph, p, {0, 1, 1}, {0, -1, 0}, {2, 0, 0}, {{3, 0, 1}});
    ASSERT_EQ(result.arcs[0].status, ARC_OK);
    EXPECT_EQ(result.arcs[0].cost, 3);
    EXPECT_EQ(result.paths[0].parent, 2); // blocked first seed survives later duplicate
    EXPECT_EQ(result.arcs[0].expanded, 2);
}

TEST(GpuExactSearch, TablePathAndReachabilityFailuresRetainCountersAndRetry)
{
    Graph graph(6);
    for (int i = 0; i < 5; ++i) graph.edge(i, i + 1, 1);
    const auto small = run(graph, parameters(), {0}, {0}, {0}, {{5, 0, 1}}, 8, 2, false, false);
    EXPECT_EQ(small.arcs[0].status, ARC_OVERFLOW);
    EXPECT_EQ(small.arcs[0].reason, FAIL_LOAD);
    EXPECT_EQ(small.arcs[0].expanded, 1);
    const auto large = run(graph, parameters(), {0}, {0}, {0}, {{5, 0, 1}}, 8, 3);
    EXPECT_EQ(large.arcs[0].status, ARC_OK);
    EXPECT_EQ(large.arcs[0].expanded, 5);
    const auto full = run(graph, parameters(), {0}, {0}, {0}, {{5, 0, 1}}, 1);
    EXPECT_EQ(full.arcs[0].status, ARC_PATH_FULL);
    EXPECT_EQ(full.arcs[0].expanded, 5);
    graph.reservations[2] = 8;
    const auto blocked = run(graph, parameters(), {0}, {0}, {0}, {{5, 0, 1}});
    EXPECT_EQ(blocked.arcs[0].status, ARC_NO_PATH);
    EXPECT_EQ(blocked.arcs[0].expanded, 2);
}

TEST(GpuExactSearch, DeterministicCorpusCoversExactAndApproximateSearches)
{
    // Differential validation against the pre-heap CPU implementation uses
    // this same corpus through the standalone entrypoint below.
    const auto first = corpus();
    EXPECT_EQ(first.size(), 2560u);
    EXPECT_EQ(first, corpus());
    // Pin to the pre-heap baseline (main babf6265). Hashes the exact byte
    // stream printed by the standalone runner: "<index>:<row>\n" per case.
    std::ostringstream stream;
    for (size_t i = 0; i < first.size(); ++i)
        stream << i << ':' << first[i] << '\n';
    EXPECT_EQ(sha256(stream.str()), "a26d6a23dc4f80b478fdb53f6a77991f87870c1eb18540b2c189dd5cab32f430");
}
#endif
} // namespace

#ifdef GPUROUTE_EXACT_STANDALONE
int main()
{
    const auto rows = corpus();
    for (size_t i = 0; i < rows.size(); ++i)
        std::cout << i << ':' << rows[i] << '\n';
}
#endif
