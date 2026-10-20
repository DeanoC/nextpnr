#include "cyclonev.h"
#include <cstdio>
int main() {
using C = mistral::CycloneV;
auto m=C::get_model("5CSEBA6U23I7");
C::pnode_coords p{C::GPIO,38,0,C::DATAOUT,2,0};
auto lane=m->p2p_to(C::pnode_coords{C::GPIO,38,0,C::PNONE,2,-1}); printf("lane %s valid %d\n",lane.to_string().c_str(),bool(lane));
auto ri=m->pnode_to_rnode(p);
printf("gpio %s ri %u\n",p.to_string().c_str(),ri);
auto to=m->p2p_to(p);printf("p2p_to %s\n",to.to_string().c_str());
for(auto f:m->p2p_from(p)) printf("p2p_from %s\n",f.to_string().c_str());
auto b=m->hmc_get_bypass(to); printf("bypass %s ri %u rc %s\n",b.to_string().c_str(),m->pnode_to_rnode(b),m->ri2rc(m->pnode_to_rnode(b)).to_string().c_str());
}
