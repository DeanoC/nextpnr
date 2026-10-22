// This file also builds as a small standalone differential corpus runner. The
// retained pre-change backend and current backend consume exactly the same cases.
#include "gpu/gpuroute_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
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
    std::vector<uint8_t> flags, region;
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
        region.assign(destinations.size(), 0); // this corpus has no reserved pips
        GraphData result;
        result.n_wires = int(rows.size()); result.n_edges = destinations.size();
        result.out_off = offsets.data(); result.out_dst = destinations.data(); result.edge_cost = costs.data();
        result.edge_region = region.data();
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
Result run_with_backend(Backend &backend, Graph &graph, RouteParams p, const std::vector<int32_t> &seeds,
                        const std::vector<float> &delays, const std::vector<float> &loads,
                        const std::vector<ArcDesc> &arcs, int path_cap = 128, int bits = 10,
                        bool bounded = false, bool large = true)
{
    std::string error;
    const auto data = graph.data();
    if (!backend.init(data, 1, bits, 1, bits + 1, error))
        throw std::runtime_error(error);
    TaskDesc t;
    t.net = 7; t.tree_cnt = int(seeds.size()); t.arc_cnt = int(arcs.size());
    t.path_cap = path_cap; t.fanout = int(arcs.size()); t.hpwl = 10;
    t.bb_x0 = 0; t.bb_y0 = 0; t.bb_x1 = 3; t.bb_y1 = 3;
    p.use_bb = bounded;
    Result result;
    backend.route(p, {t}, arcs, seeds, delays, loads, {}, large, result.arcs, result.paths);
    return result;
}
Result run_with_backend(std::unique_ptr<Backend> backend, Graph &graph, RouteParams p,
                        const std::vector<int32_t> &seeds, const std::vector<float> &delays,
                        const std::vector<float> &loads, const std::vector<ArcDesc> &arcs, int path_cap = 128,
                        int bits = 10, bool bounded = false, bool large = true)
{
    return run_with_backend(*backend, graph, p, seeds, delays, loads, arcs, path_cap, bits, bounded, large);
}
Result run(Graph &graph, RouteParams p, const std::vector<int32_t> &seeds, const std::vector<float> &delays,
           const std::vector<float> &loads, const std::vector<ArcDesc> &arcs, int path_cap = 128,
           int bits = 10, bool bounded = false, bool large = true)
{
    return run_with_backend(create_cpu_backend(), graph, p, seeds, delays, loads, arcs, path_cap, bits, bounded, large);
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
// Calls case_fn(graph, params, seeds, delays, loads, arcs, path_cap, bits,
// bounded, large) for each of the 2560 corpus cases, in order.
template <typename CaseFn> void for_each_corpus_case(CaseFn case_fn)
{
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
            case_fn(graph, p, seeds, delays, loads, arcs, setting % 5 == 0 ? 1 : 2 * n, setting % 4 == 0 ? 3 : 10,
                    setting % 2 != 0, setting % 3 != 1);
        }
    }
}
std::vector<std::string> corpus()
{
    std::vector<std::string> result;
    for_each_corpus_case([&](Graph &graph, const RouteParams &p, const std::vector<int32_t> &seeds,
                             const std::vector<float> &delays, const std::vector<float> &loads,
                             const std::vector<ArcDesc> &arcs, int path_cap, int bits, bool bounded, bool large) {
        result.push_back(serialize(run(graph, p, seeds, delays, loads, arcs, path_cap, bits, bounded, large)));
    });
    return result;
}

// Issue #228: a one-ULP frontier range gives a positive bin width, but
// minf + binw rounds back to minf. A strict threshold alone expands nothing.
// Keep this in the standalone runner too, so it can be run under a timeout.
void rounded_threshold_regression()
{
    for (float base : {16.0f, 65536.0f, 1.0e12f}) {
        for (bool reachable : {false, true}) {
            Graph graph(3);
            const float upper = std::nextafter(base, std::numeric_limits<float>::infinity());
            const float cost = upper - base;
            if (reachable) {
                graph.edge(0, 2, cost);
                graph.edge(1, 2, cost);
            }
            auto p = parameters();
            p.exact = 0;
            const auto result = run(graph, p, {0, 1}, {base, upper}, {0, 0}, {{2, 1, 1}});
            const auto &arc = result.arcs.at(0);
            if (arc.status != (reachable ? ARC_OK : ARC_NO_PATH) ||
                arc.steps != (reachable ? 1 : 2) || arc.expanded != (reachable ? 1 : 2))
                throw std::runtime_error("rounded histogram threshold failed to make progress");
            if (reachable && (arc.cost != upper || arc.path_len != 1 || result.paths[0].parent != 0))
                throw std::runtime_error("rounded histogram threshold returned the wrong route");
        }
    }
}

#ifndef GPUROUTE_EXACT_STANDALONE
// 0 -1-> 1 -5-> 2 -1-> 3 -1-> 4 -1-> 5, and 4 -1-> 2. Sinks 3, 2, 5 in that
// order. The first arc reaches 3 through 2, so the second arc is already
// routed; searched, it would return 3 -> 4 -> 2. The third arc must attach to
// a wire the first arc really added, not to 4.
Graph shared_sink_graph()
{
    Graph graph(6);
    graph.edge(0, 1, 1); graph.edge(1, 2, 5); graph.edge(2, 3, 1);
    graph.edge(3, 4, 1); graph.edge(4, 2, 1); graph.edge(4, 5, 1);
    return graph;
}
const std::vector<ArcDesc> shared_sink_arcs = {{3, 1, 0}, {2, 1, 0}, {5, 1, 0}};
void expect_shared_sink_routes(const Result &result)
{
    ASSERT_EQ(result.arcs[0].status, ARC_OK);
    EXPECT_EQ(result.arcs[0].path_len, 3);
    ASSERT_EQ(result.arcs[1].status, ARC_OK);
    EXPECT_EQ(result.arcs[1].path_len, 0);
    EXPECT_EQ(result.arcs[1].expanded, 0);
    ASSERT_EQ(result.arcs[2].status, ARC_OK);
    ASSERT_EQ(result.arcs[2].path_len, 2);
    const PathEntry *third = &result.paths[result.arcs[2].path_off];
    EXPECT_EQ(third[0].wire, 5);
    EXPECT_EQ(third[1].wire, 4);
    EXPECT_EQ(third[1].parent, 3);
}
// Every result field and the path entries each arc returned. Unlike
// serialize(), unused output slots are left out: the device leaves them unset.
std::string returned_routes(const Result &result)
{
    std::ostringstream out;
    for (const auto &arc : result.arcs) {
        out << arc.status << ',' << arc.reason << ',' << std::hex << bits(arc.cost) << std::dec << ','
            << arc.expanded << ',' << arc.steps << ',' << arc.path_off << ',' << arc.path_len << ':';
        for (int k = 0; k < arc.path_len; ++k) {
            const auto &path = result.paths.at(arc.path_off + k);
            out << path.wire << ',' << path.parent << ',' << path.edge << ',' << std::hex << bits(path.delay)
                << std::dec << ';';
        }
        out << '|';
    }
    return out.str();
}

TEST(GpuApproximateSearch, RoundedHistogramThresholdMakesProgress)
{
    rounded_threshold_regression();
}

TEST(GpuApproximateSearch, RoundedHistogramThresholdMakesProgressOnDevice)
{
    // Only under a gpu-run lease. It exports the leased ids as FES_GPU_IDS and
    // makes only those GPUs visible, so the default device below is a leased
    // one; a --phase-lease run sets neither and skips. Check first: even
    // device_backend_available() starts the GPU runtime.
    const char *leased = std::getenv("FES_GPU_IDS");
    if (leased == nullptr || *leased == '\0')
        GTEST_SKIP() << "needs a gpu-run GPU lease (no FES_GPU_IDS)";
    if (!device_backend_available())
        GTEST_SKIP() << "GPU device backend or GPU device unavailable";

    for (float base : {16.0f, 65536.0f, 1.0e12f}) {
        for (bool reachable : {false, true}) {
            Graph graph(3);
            const float upper = std::nextafter(base, std::numeric_limits<float>::infinity());
            const float cost = upper - base;
            if (reachable) {
                graph.edge(0, 2, cost);
                graph.edge(1, 2, cost);
            }
            auto p = parameters();
            p.exact = 0;
            std::string error;
            auto backend = create_device_backend(-1, error);
            ASSERT_NE(backend, nullptr) << error;
            const auto result = run_with_backend(std::move(backend), graph, p, {0, 1}, {base, upper}, {0, 0},
                                                 {{2, 1, 1}});
            const auto &arc = result.arcs.at(0);
            ASSERT_EQ(arc.status, reachable ? ARC_OK : ARC_NO_PATH);
            if (reachable) {
                EXPECT_EQ(arc.cost, upper);
                ASSERT_EQ(arc.path_len, 1);
                EXPECT_EQ(result.paths.at(arc.path_off).parent, 0);
            }
        }
    }
}

TEST(GpuApproximateSearch, SinkReachedByEarlierArcAddsNoPathOnDevice)
{
    // Only under a gpu-run lease, as above
    const char *leased = std::getenv("FES_GPU_IDS");
    if (leased == nullptr || *leased == '\0')
        GTEST_SKIP() << "needs a gpu-run GPU lease (no FES_GPU_IDS)";
    if (!device_backend_available())
        GTEST_SKIP() << "GPU device backend or GPU device unavailable";

    Graph graph = shared_sink_graph();
    auto p = parameters();
    p.exact = 0;
    std::string error;
    auto backend = create_device_backend(-1, error);
    ASSERT_NE(backend, nullptr) << error;
    const auto device = run_with_backend(std::move(backend), graph, p, {0}, {0}, {0}, shared_sink_arcs);
    expect_shared_sink_routes(device);
    EXPECT_EQ(returned_routes(device), returned_routes(run(graph, p, {0}, {0}, {0}, shared_sink_arcs)));
}

// True when the device tests may run: under a gpu-run lease (see above) with
// a device backend and a device. Otherwise skip_reason says why not.
bool device_tests_enabled(std::string &skip_reason)
{
    const char *leased = std::getenv("FES_GPU_IDS");
    if (leased == nullptr || *leased == '\0') {
        skip_reason = "needs a gpu-run GPU lease (no FES_GPU_IDS)";
        return false;
    }
    if (!device_backend_available()) {
        skip_reason = "GPU device backend or GPU device unavailable";
        return false;
    }
    return true;
}

bool any_overflow(const Result &result)
{
    return std::any_of(result.arcs.begin(), result.arcs.end(),
                       [](const ArcResult &arc) { return arc.status == ARC_OVERFLOW; });
}

TEST(GpuApproximateSearch, CorpusMatchesCpuOnDevice)
{
    // The K-best corpus cases (the device ignores exact) must route as the
    // CPU reference does, bit for bit. A case where either backend overflows
    // is skipped: the CPU emulates only the load factor limit, not the
    // device's pile and probe limits, and the router retries those arcs in
    // the large lane.
    std::string skip_reason;
    if (!device_tests_enabled(skip_reason))
        GTEST_SKIP() << skip_reason;
    std::string error;
    auto backend = create_device_backend(-1, error);
    ASSERT_NE(backend, nullptr) << error;
    int compared = 0, index = 0;
    for_each_corpus_case([&](Graph &graph, RouteParams p, const std::vector<int32_t> &seeds,
                             const std::vector<float> &delays, const std::vector<float> &loads,
                             const std::vector<ArcDesc> &arcs, int path_cap, int bits, bool bounded, bool large) {
        const int case_index = index++;
        if (p.exact)
            return;
        const auto device = run_with_backend(*backend, graph, p, seeds, delays, loads, arcs, path_cap, bits, bounded,
                                             large);
        const auto cpu = run(graph, p, seeds, delays, loads, arcs, path_cap, bits, bounded, large);
        if (any_overflow(device) || any_overflow(cpu))
            return;
        compared++;
        EXPECT_EQ(returned_routes(device), returned_routes(cpu)) << "corpus case " << case_index;
    });
    // 640 K-best cases; the small (8-entry) tables overflow in a quarter
    EXPECT_GE(compared, 400);
}

TEST(GpuApproximateSearch, ManyNetLaunchMatchesCpuOnDevice)
{
    // A 64x64 grid with long hops and congestion, 48 nets of 4 sinks each
    // routed in one launch: many threadgroups at once, many threads per
    // step, and real contention on the hash table and frontier. Costs are
    // not dyadic, so every rounding step of the cost expressions counts.
    std::string skip_reason;
    if (!device_tests_enabled(skip_reason))
        GTEST_SKIP() << skip_reason;
    constexpr int side = 64, n = side * side, nets = 48, sinks = 4, path_cap = 2048;
    uint32_t random = 0xc0ffeeu;
    auto next = [&]() {
        random = random * 1664525u + 1013904223u;
        return random >> 8;
    };
    Graph graph(n);
    for (int w = 0; w < n; ++w) {
        graph.x[w] = int16_t(w % side);
        graph.y[w] = int16_t(w / side);
        const int x = w % side, y = w / side;
        auto cost = [&]() { return 0.05f + float(next() % 37) * 0.013f; };
        if (x + 1 < side)
            graph.edge(w, w + 1, cost());
        if (x > 0)
            graph.edge(w, w - 1, cost());
        if (y + 1 < side)
            graph.edge(w, w + side, cost());
        if (y > 0)
            graph.edge(w, w - side, cost());
        const int hop = int(next() % n);
        graph.edge(w, hop, 0.3f + float(next() % 11) * 0.07f);
        graph.occupancy[w] = int(next() % 3);
        graph.history[w] = 1.0f + float(next() % 9) * 0.1f;
        if (w % 97 == 0)
            graph.flags[w] = WIRE_UNAVAILABLE;
        if (w % 89 == 0)
            graph.reservations[w] = int(next() % nets) | (w % 2 ? RESERVED_SOFT : 0);
    }
    const auto data = graph.data();
    std::vector<TaskDesc> tasks;
    std::vector<ArcDesc> arcs;
    std::vector<int32_t> seeds;
    std::vector<float> delays, loads;
    for (int t = 0; t < nets; ++t) {
        TaskDesc task;
        task.net = t;
        task.tree_off = int(seeds.size());
        task.tree_cnt = 1 + t % 3;
        for (int i = 0; i < task.tree_cnt; ++i) {
            seeds.push_back(int32_t(next() % n));
            delays.push_back(float(i) * 0.37f);
            loads.push_back(float(i % 2));
        }
        task.arc_off = int(arcs.size());
        task.arc_cnt = sinks;
        for (int i = 0; i < sinks; ++i)
            arcs.push_back(ArcDesc{int32_t(next() % n), 0.05f + float(next() % 19) * 0.05f, float(next() % 11) * 0.1f});
        task.path_off = t * path_cap;
        task.path_cap = path_cap;
        task.cx = side / 2;
        task.cy = side / 2;
        task.hpwl = 1 + t;
        task.fanout = sinks;
        task.bb_x0 = 0;
        task.bb_y0 = 0;
        task.bb_x1 = side - 1;
        task.bb_y1 = side - 1;
        tasks.push_back(task);
    }
    struct Config
    {
        int expand_k, expand_div;
        float load_penalty;
        bool large;
    };
    for (const Config &config : {Config{256, 0, 0.0f, false}, Config{16, 4, 0.3f, true}}) {
        SCOPED_TRACE(config.expand_k);
        RouteParams p;
        p.expand_k = config.expand_k;
        p.expand_div = config.expand_div;
        p.load_penalty = config.load_penalty;
        p.pip_adder = 0.01f;
        p.seed_delay_floor = 0.2f;
        std::vector<Result> results;
        std::string error;
        for (bool device : {true, false}) {
            std::unique_ptr<Backend> backend = device ? create_device_backend(-1, error) : create_cpu_backend();
            ASSERT_NE(backend, nullptr) << error;
            ASSERT_TRUE(backend->init(data, 12, 14, 4, 15, error)) << error;
            Result result;
            backend->route(p, tasks, arcs, seeds, delays, loads, {}, config.large, result.arcs, result.paths);
            results.push_back(std::move(result));
        }
        int routed = 0;
        for (const auto &arc : results[1].arcs)
            routed += arc.status == ARC_OK && arc.path_len > 0;
        EXPECT_FALSE(any_overflow(results[0]));
        EXPECT_FALSE(any_overflow(results[1]));
        EXPECT_GT(routed, nets * sinks / 2);
        // Arc by arc, so a mismatch names the first arc that differs
        std::istringstream device_routes(returned_routes(results[0])), cpu_routes(returned_routes(results[1]));
        std::string device_arc, cpu_arc;
        for (int arc = 0; std::getline(cpu_routes, cpu_arc, '|'); ++arc) {
            ASSERT_TRUE(std::getline(device_routes, device_arc, '|'));
            ASSERT_EQ(device_arc, cpu_arc) << "net " << arc / sinks << " arc " << arc % sinks;
        }
    }
}

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

TEST(GpuExactSearch, SinkReachedByEarlierArcAddsNoPath)
{
    Graph graph = shared_sink_graph();
    for (int exact : {1, 0}) {
        SCOPED_TRACE(exact ? "exact" : "K-best");
        auto p = parameters();
        p.exact = exact;
        expect_shared_sink_routes(run(graph, p, {0}, {0}, {0}, shared_sink_arcs));
    }
}

TEST(GpuExactSearch, DeterministicCorpusCoversExactAndApproximateSearches)
{
    // Differential validation against the pre-heap CPU implementation uses
    // this same corpus through the standalone entrypoint below.
    const auto first = corpus();
    EXPECT_EQ(first.size(), 2560u);
    EXPECT_EQ(first, corpus());
    // Pin to the pre-heap baseline (main babf6265), except for the 13 cases
    // whose second sink is on the first arc's path: that arc now comes back
    // with no path (#270). Hashes the exact byte stream printed by the
    // standalone runner: "<index>:<row>\n" per case.
    std::ostringstream stream;
    for (size_t i = 0; i < first.size(); ++i)
        stream << i << ':' << first[i] << '\n';
    EXPECT_EQ(sha256(stream.str()), "37bd2c314eac2ea8603baa00ebad464bca38cc7911a66f918df7c3cc8e69c6fb");
}
#endif
} // namespace

#ifdef GPUROUTE_EXACT_STANDALONE
int main()
{
    rounded_threshold_regression();
    const auto rows = corpus();
    for (size_t i = 0; i < rows.size(); ++i)
        std::cout << i << ':' << rows[i] << '\n';
}
#endif
