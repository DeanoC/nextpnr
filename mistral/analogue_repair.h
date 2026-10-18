/* Analogue repair reroute error policy. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_REPAIR_H
#define MISTRAL_ANALOGUE_REPAIR_H

#include <utility>

#include "gpurouter.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

template <typename Route> bool try_analogue_repair_reroute(int round, Route &&route)
{
    try {
        return std::forward<Route>(route)();
    } catch (const GpuRequiredFailure &) {
        throw;
    } catch (const log_execution_error_exception &) {
        log_warning("analogue repair round %d could not route; keeping the best earlier round\n", round);
        return false;
    }
}

NEXTPNR_NAMESPACE_END

#endif
