#ifndef MISTRAL_READY_FALLBACK_POLICY_H
#define MISTRAL_READY_FALLBACK_POLICY_H
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
namespace ready_fallback_policy {
struct Slack
{
    int setup, hold;
};
inline bool finite(Slack s)
{
    return s.setup != std::numeric_limits<int>::max() && s.setup != std::numeric_limits<int>::lowest() &&
           s.hold != std::numeric_limits<int>::max() && s.hold != std::numeric_limits<int>::lowest();
}
inline bool endpoint_ok(Slack old, Slack now)
{
    return finite(old) && finite(now) && now.setup >= old.setup && now.hold >= std::min(old.hold, 0);
}
inline bool clock_ok(float old, float now) { return std::isfinite(old) && std::isfinite(now) && old > 0 && now >= old; }
inline bool gain_ok(int old, int now) { return int64_t(now) - int64_t(old) >= 20; }
} // namespace ready_fallback_policy
#endif
