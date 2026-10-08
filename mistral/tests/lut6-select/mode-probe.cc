#include "cyclonev.h"
#include <memory>
#include <cstdio>
int main(){using C=mistral::CycloneV;std::unique_ptr<C> c(C::get_model("5CSEBA6U23I7"));C::xycoords p(29,77);for(auto m:{C::TMODE,C::BMODE})for(auto v:{C::C_E,C::D_E,C::E_0,C::E_1})printf("mux=%d value=%d accepted=%d\n",int(m),int(v),c->bmux_m_set(C::LAB,p,m,5,v));}
