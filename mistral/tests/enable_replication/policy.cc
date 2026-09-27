#include "enable_replication_policy.h"
#include <cassert>
#include <iostream>
using namespace enable_replication_policy;
int main()
{
    assert(inputs_nonregressing({900, 300}, {800, 300}));
    assert(!inputs_nonregressing({900, 300}, {800, 400})); // max alone hides regression
    assert(!inputs_nonregressing({900}, {}));
    assert(group_gain({1600, 1600}, {1300, 1300}) == 300);
    assert(group_gain({1600, 1600}, {1300, 1400}) == 0); // whole group must improve
    assert(group_gain({1600}, {1350}) == 250);
    assert(group_gain({1600}, {1351}) == 0);
    assert(group_gain({}, {}) == 0);
    assert(critical_failing(0.95f, -10));
    assert(!critical_failing(0.95f, 0));
    assert(!critical_failing(0.5f, -10));
    assert(!critical_failing(0.95f, 100));
    assert(hold_nonregressing({}, {}));
    assert(!hold_nonregressing({}, {{"ep", -1}}));
    assert(hold_nonregressing({{"ep", -10}}, {{"ep", -5}}));
    assert(!hold_nonregressing({{"ep", -10}}, {{"ep", -11}}));
    assert(hold_nonregressing({{"ep", -10}}, {}));
    assert(!hold_nonregressing({{"ep", -10}}, {{"other", -1}}));
    assert(valid_budget(0) && valid_budget(8));
    assert(!valid_budget(-1) && !valid_budget(9));
    std::cout << "PASS: input guards, whole-LAB gains, criticality, hold rollback and budget\n";
}
