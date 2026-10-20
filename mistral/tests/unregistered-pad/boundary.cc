#include "cyclonev.h"
#include <cstdio>
int main() { using C=mistral::CycloneV; auto m=C::get_model("5CSEBA6U23I7");
for(const char *pin:{"V12","E8","D11","W12","AH13","D8","AH14","AF7","AE24","AD23","AE6","AE23","AG14","AD5","AF4","AH3"}) {
auto pp=m->pin_find_name(pin); auto pos=C::xycoords(pp->pad & 0x3fff); int8_t bi=pp->pad>>14;
for(auto port:{C::DATAOUT,C::OEIN,C::DATAIN}) {C::pnode_coords p{C::GPIO,pos,port,bi,0};auto r=m->pnode_to_rnode(p);auto native=p;
if(r==0xffffffff){auto p2=m->p2p_to(p);if(!p2){auto v=m->p2p_from(p);if(v.empty())return 2;p2=v[0];}native=m->hmc_get_bypass(p2);r=m->pnode_to_rnode(native);}
printf("%s %s %s %s\n",pin,p.to_string().c_str(),native.to_string().c_str(),m->ri2rc(r).to_string().c_str());}
}
}
