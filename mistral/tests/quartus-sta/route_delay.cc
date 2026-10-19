// route_delay: time every active route of a Cyclone V RBF with the analogue
// model that nextpnr's Mistral backend uses for signoff (#174).
//
//   route_delay <model> <file.rbf> <out.tsv>
//
// Each row is one routed input port: the driving port, the rnode chain and
// the delay of the chain. The per-pip loop is Arch::analogue_arc_delay
// (mistral/delay.cc): DELAY_MAX, EST_SLOW, one input wave built at the first
// circuit node and carried hop to hop. 100 C is the nextpnr signoff slot;
// -40 C is the second slow Quartus corner of an I7 part.
#include "cyclonev.h"
#include "mistral-analogsim.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using mistral::AnalogSim;
using mistral::CycloneV;

namespace {

std::vector<uint8_t> load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    fclose(f);
    return data;
}

// Returns false when a hop has no analogue model (nextpnr then keeps the
// delay-table estimate for the whole arc).
// With to_port, also time the sink's own circuit to its block port (for a
// GOUT, the input mux into the ALM pin). nextpnr's route ends at the sink
// rnode, so its arc delay stops before that stage.
bool chain_delay(const CycloneV *cv, std::vector<CycloneV::rnode_index> chain,
                 CycloneV::timing_slot_t temp, double &rise, double &fall, double &early, bool to_port = false)
{
    if (to_port)
        chain.push_back(0xffffffff);
    bool inverted = false;
    AnalogSim::wave input_wave[2], output_wave[2];
    AnalogSim::time_interval output_delays[2];
    double sum[2] = {0, 0}, sum_min[2] = {0, 0};
    const auto est = CycloneV::EST_SLOW;
    for (size_t i = 0; i + 1 < chain.size(); i++) {
        const CycloneV::rnode_index src_ri = chain[i], dst_ri = chain[i + 1];
        auto mode = cv->rnode_timing_get_mode(src_ri);
        if (mode == CycloneV::RTM_UNSUPPORTED)
            return false;
        auto inverting = cv->rnode_is_inverting(src_ri);
        if (mode == CycloneV::RTM_P2P) {
            if (inverting == CycloneV::INV_YES || inverting == CycloneV::INV_PROGRAMMABLE)
                inverted = !inverted;
            continue;
        }
        if (mode == CycloneV::RTM_NO_DELAY) {
            if (inverting)
                inverted = !inverted;
            continue;
        }
        if (input_wave[0].empty()) {
            cv->rnode_timing_build_input_wave(src_ri, temp, CycloneV::DELAY_MAX,
                                              inverted ? CycloneV::RF_FALL : CycloneV::RF_RISE, est, input_wave[0]);
            cv->rnode_timing_build_input_wave(src_ri, temp, CycloneV::DELAY_MAX,
                                              inverted ? CycloneV::RF_RISE : CycloneV::RF_FALL, est, input_wave[1]);
            if (input_wave[CycloneV::RF_RISE].empty() || input_wave[CycloneV::RF_FALL].empty())
                return false;
        }
        for (int edge = 0; edge != 2; edge++) {
            auto actual_edge = edge       ? inverted ? CycloneV::RF_RISE : CycloneV::RF_FALL
                               : inverted ? CycloneV::RF_FALL
                                          : CycloneV::RF_RISE;
            AnalogSim sim;
            int input = -1;
            std::vector<std::pair<CycloneV::rnode_index, int>> outputs;
            cv->rnode_timing_build_circuit(src_ri, temp, CycloneV::DELAY_MAX, actual_edge, sim, input, outputs);
            sim.set_input_wave(input, input_wave[edge]);
            auto o = std::find_if(
                    outputs.begin(), outputs.end(),
                    [&](const std::pair<CycloneV::rnode_index, int> &out) { return out.first == dst_ri; });
            if (o == outputs.end())
                return false;
            output_wave[edge].clear();
            sim.set_output_wave(o->second, output_wave[edge], output_delays[edge]);
            sim.run();
            cv->rnode_timing_trim_wave(temp, CycloneV::DELAY_MAX, output_wave[edge], input_wave[edge]);
            sum[edge] += output_delays[edge].mx;
            sum_min[edge] += output_delays[edge].mi;
        }
        if (inverting == CycloneV::INV_YES || inverting == CycloneV::INV_PROGRAMMABLE)
            inverted = !inverted;
    }
    rise = sum[0] * 1e12;
    fall = sum[1] * 1e12;
    early = std::min(sum_min[0], sum_min[1]) * 1e12;
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <model> <file.rbf> <out.tsv>\n", argv[0]);
        return 1;
    }
    std::unique_ptr<CycloneV> cv(CycloneV::get_model(argv[1]));
    if (!cv) {
        fprintf(stderr, "unknown model %s\n", argv[1]);
        return 1;
    }
    auto rbf = load(argv[2]);
    cv->rbf_load(rbf.data(), rbf.size());

    std::unordered_map<CycloneV::rnode_index, CycloneV::rnode_index> source;
    std::unordered_map<CycloneV::rnode_index, int> fanout;
    for (auto &link : cv->route_all_active_links()) {
        source[link.second] = link.first;
        fanout[link.first]++;
    }

    FILE *out = fopen(argv[3], "w");
    if (!out) {
        perror(argv[3]);
        return 1;
    }
    fprintf(out, "dst\tsrc\thops\tchain\trise100\tfall100\tmin100\trisen40\tfalln40\tminn40"
                 "\tport_rise100\tport_fall100\tport_risen40\tport_falln40\n");
    size_t rows = 0;
    // A route sink is an active destination that drives nothing further:
    // a GOUT block input, a clock tap or a hard-block port.
    for (auto &dst : source) {
        if (fanout.count(dst.first))
            continue;
        // Walk back to the driver. A loop in the decoded muxes has no driver;
        // report it untimed instead of as a measured route.
        std::vector<CycloneV::rnode_index> chain{dst.first};
        std::unordered_set<CycloneV::rnode_index> seen{dst.first};
        bool loop = false;
        auto it = source.find(dst.first);
        while (it != source.end()) {
            if (!seen.insert(it->second).second) {
                loop = true;
                break;
            }
            chain.push_back(it->second);
            it = source.find(it->second);
        }
        std::reverse(chain.begin(), chain.end());
        auto name = [&](CycloneV::rnode_index ri) {
            auto pn = cv->rnode_to_pnode(ri);
            return pn ? pn.to_string() : cv->ri2rc(ri).to_string();
        };
        std::string names;
        for (auto ri : chain)
            names += (names.empty() ? "" : ",") + cv->ri2rc(ri).to_string();
        double r100 = -1, f100 = -1, m100 = -1, rn40 = -1, fn40 = -1, mn40 = -1;
        bool ok = !loop && chain_delay(cv.get(), chain, CycloneV::T_100, r100, f100, m100) &&
                  chain_delay(cv.get(), chain, CycloneV::T_N40, rn40, fn40, mn40);
        if (!ok)
            r100 = f100 = m100 = rn40 = fn40 = mn40 = -1;
        double pr100 = -1, pf100 = -1, prn40 = -1, pfn40 = -1, unused;
        if (loop || !(chain_delay(cv.get(), chain, CycloneV::T_100, pr100, pf100, unused, true) &&
                      chain_delay(cv.get(), chain, CycloneV::T_N40, prn40, pfn40, unused, true)))
            pr100 = pf100 = prn40 = pfn40 = -1;
        const std::string src = loop ? "loop" : name(chain.front());
        fprintf(out, "%s\t%s\t%zu\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
                name(chain.back()).c_str(), src.c_str(), chain.size() - 1, names.c_str(), r100, f100, m100, rn40, fn40,
                mn40, pr100, pf100, prn40, pfn40);
        rows++;
    }
    fclose(out);
    fprintf(stderr, "%zu routed input ports\n", rows);
    return 0;
}
