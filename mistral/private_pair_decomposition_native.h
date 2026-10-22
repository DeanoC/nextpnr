/* Native private-pair transaction and bounded timing eligibility. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PRIVATE_PAIR_DECOMPOSITION_NATIVE_H
#define MISTRAL_PRIVATE_PAIR_DECOMPOSITION_NATIVE_H
#include "nextpnr.h"
#include "private_pair_decomposition.h"
#include "timing.h"
#include <array>
#include <functional>
NEXTPNR_NAMESPACE_BEGIN
bool private_pair_trial(Context *ctx, CellInfo *producer,
                        const private_pair_decomposition::Candidate &candidate,
                        const std::function<bool()> &accept, std::string *reason = nullptr,
                        const std::function<void(unsigned)> &stage = {});
bool private_pair_common_arrivals(Context *ctx, TimingAnalyser &timing,
                                  const std::vector<NetInfo *> &signals,
                                  std::array<delay_t, 7> &arrivals, std::string *reason = nullptr);
bool private_pair_output_coverage(Context *ctx, TimingAnalyser &timing, NetInfo *output,
                                  std::string *reason = nullptr);
bool private_pair_clock_safe(const TimingResult &before, const TimingResult &after, std::string *reason = nullptr);
NEXTPNR_NAMESPACE_END
#endif
