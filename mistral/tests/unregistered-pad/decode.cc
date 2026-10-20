#include "cyclonev.h"
#include <fstream>
#include <iterator>
#include <cstdio>
int main(int argc,char**argv) {using C=mistral::CycloneV; auto m=C::get_model("5CSEBA6U23I7");
if(argc==2){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>d((std::istreambuf_iterator<char>(f)),{});if(d.empty())return 2;m->rbf_load(d.data(),d.size());}
for(const char *pin:{"V12","E8","D11","W12","AH13","D8","AH14","AF7","AE24","AD23","AE6","AE23","AG14","AD5","AF4","AH3"}) {
auto pp=m->pin_find_name(pin);auto pos=C::xycoords(pp->pad&0x3fff);int8_t bi=pp->pad>>14;auto lane=m->p2p_to(C::pnode_coords{C::GPIO,pos,C::PNONE,bi,-1});
for(auto mux:{C::RB_T1_SEL_IREG_CFF_DELAY,C::SET_T3_FOR_CDATA0IN,C::SET_T3_FOR_CDATA1IN,C::RB_T9_SEL_OREG_DFF_DELAY,C::RB_T9_SEL_EREG_CFF_DELAY,C::INPUT_REG4_SEL,C::OUTREG_OUTPUT_SEL,C::OEREG_OUTPUT_SEL}){
C::bmux_setting_t s;if(!m->bmux_get(C::DQS16,lane.p(),mux,lane.bi(),s))return 3;
printf("%s %s %u %d",pin,C::bmux_type_names[mux],s.s,s.def);for(auto b:s.r)printf(" %02x",b);printf("\n");}
}
}
