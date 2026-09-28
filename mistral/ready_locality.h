#ifndef MISTRAL_READY_LOCALITY_H
#define MISTRAL_READY_LOCALITY_H
#include <functional>
#include <utility>
#include <vector>
#include "nextpnr.h"
NEXTPNR_NAMESPACE_BEGIN
// Fully bind, measure, and always restore. No intermediate legality repair.
bool ready_locality_trial(Context *ctx, const std::vector<std::pair<CellInfo *, BelId>> &moves,
                          const std::function<void(bool)> &measure);
void diagnostic_ready_locality(Context *ctx, const char *prefix, bool imported_context = false);
NEXTPNR_NAMESPACE_END
#endif
