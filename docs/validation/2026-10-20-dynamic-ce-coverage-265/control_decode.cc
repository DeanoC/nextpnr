#include "cyclonev.h"
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    auto model = mistral::CycloneV::get_model("5CSEBA6U23I7");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    if (data.empty()) return 2;
    model->rbf_load(data.data(), data.size());
    for (const auto &setting : model->inv_get()) {
        auto p = model->rnode_to_pnode(setting.node);
        if (!p || p.bt() != mistral::CycloneV::GPIO || p.pt() != mistral::CycloneV::CEOUT) continue;
        auto pin = model->pin_find_pnode(p);
        if (pin) printf("%s\t%s\t%d\t%d\n",pin->name,p.to_string().c_str(),setting.value,setting.def);
    }
}
