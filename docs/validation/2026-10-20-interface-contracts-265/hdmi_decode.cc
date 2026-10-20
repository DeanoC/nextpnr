#include "cyclonev.h"
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
int main(int argc, char **argv) {
    if(argc != 2) return 1;
    using C=mistral::CycloneV;
    auto m=C::get_model("5CSEBA6U23I7");
    std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>d((std::istreambuf_iterator<char>(f)),{});
    if(d.empty())return 2; m->rbf_load(d.data(),d.size());
    auto lane=m->p2p_to(C::pnode_coords{C::GPIO,38,0,C::PNONE,2,-1});
    for (auto mux: {C::RB_T9_SEL_OREG_DFF_DELAY,C::RB_T9_SEL_EREG_CFF_DELAY,C::OUTREG_OUTPUT_SEL,C::OUTREG_MODE_SEL,C::OEREG_OUTPUT_SEL,C::INPUT_REG4_SEL}) {
        C::bmux_setting_t s;
        if(!m->bmux_get(C::DQS16,lane.p(),mux,lane.bi(),s))return 3;
        printf("%s\t%u\t%d\ttype=%d",C::bmux_type_names[mux],s.s,s.def,s.type);
        for(auto byte:s.r)printf("\t%02x",byte); printf("\n");
    }
    auto p=m->hmc_get_bypass(m->p2p_to(C::pnode_coords{C::GPIO,38,0,C::DATAOUT,2,0}));
    auto ri=m->pnode_to_rnode(p);
    bool found=false;
    for(const auto&i:m->inv_get()) if(i.node==ri) {printf("data_inversion\t%d\t%d\n",i.value,i.def);found=true;}
    if(!found)return 4;
    printf("boundary\t%s\t%s\n",p.to_string().c_str(),m->ri2rc(ri).to_string().c_str());
}
