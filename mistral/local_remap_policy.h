#ifndef MISTRAL_LOCAL_REMAP_POLICY_H
#define MISTRAL_LOCAL_REMAP_POLICY_H
#include <algorithm>
#include <cstdint>
#include <vector>

namespace local_remap_policy {
constexpr int ZERO = -1, ONE = -2, INTERMEDIATE = -3;
struct Pin { int signal; bool inverted; };
struct Composition {
    bool valid = false;
    uint64_t mask = 0;
    std::vector<int> signals;
};

inline Composition compose(uint64_t inner, const std::vector<Pin> &inputs,
                           uint64_t outer, const std::vector<Pin> &outputs)
{
    Composition result;
    if (inputs.size() < 2 || inputs.size() > 6 || outputs.size() < 2 || outputs.size() > 6)
        return result;
    bool connected = false;
    for (const auto &pin : inputs) {
        if (pin.signal < ONE) return result;
        if (pin.signal >= 0) result.signals.push_back(pin.signal);
    }
    for (const auto &pin : outputs) {
        if (pin.signal < INTERMEDIATE) return result;
        connected |= pin.signal == INTERMEDIATE;
        if (pin.signal >= 0) result.signals.push_back(pin.signal);
    }
    std::sort(result.signals.begin(), result.signals.end());
    result.signals.erase(std::unique(result.signals.begin(), result.signals.end()), result.signals.end());
    if (!connected || result.signals.size() > 6) return result;
    for (unsigned row = 0; row < (1u << result.signals.size()); ++row) {
        auto value = [&](Pin pin, bool intermediate) {
            bool bit = pin.signal == ONE || (pin.signal == INTERMEDIATE && intermediate);
            if (pin.signal >= 0) {
                auto index = std::lower_bound(result.signals.begin(), result.signals.end(), pin.signal) - result.signals.begin();
                bit = (row >> index) & 1;
            }
            return bit ^ pin.inverted;
        };
        unsigned in = 0, out = 0;
        for (size_t i = 0; i < inputs.size(); ++i) in |= unsigned(value(inputs[i], false)) << i;
        bool mid = (inner >> in) & 1;
        for (size_t i = 0; i < outputs.size(); ++i) out |= unsigned(value(outputs[i], mid)) << i;
        if ((outer >> out) & 1) result.mask |= uint64_t(1) << row;
    }
    result.valid = true;
    return result;
}
}
#endif
