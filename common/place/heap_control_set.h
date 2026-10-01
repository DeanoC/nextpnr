#ifndef HEAP_CONTROL_SET_H
#define HEAP_CONTROL_SET_H

#include "hashlib.h"

NEXTPNR_NAMESPACE_BEGIN

// Track each resident signature separately so evicting one set does not
// erase affinity for another set, or for another FF using the same set.
struct HeapControlSetState
{
    dict<int32_t, int32_t> members;
    int32_t count = 0;
    void bind(int32_t ctrl_set)
    {
        ++members[ctrl_set];
        ++count;
    }
    void unbind(int32_t ctrl_set)
    {
        auto found = members.find(ctrl_set);
        NPNR_ASSERT(found != members.end() && found->second > 0);
        if (--found->second == 0)
            members.erase(found);
        --count;
        NPNR_ASSERT(count >= 0);
    }
    bool check(int32_t ctrl_set) const { return count == 0 || (members.size() == 1 && members.count(ctrl_set)); }
};

NEXTPNR_NAMESPACE_END

#endif
