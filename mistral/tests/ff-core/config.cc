// Decode only the actual FF input/clock selection bits; never edits an RBF.
#include "cyclonev.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>
using CV=mistral::CycloneV;
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  std::unique_ptr<CV> cv(CV::get_model("5CSEBA6U23I7"));
  std::ifstream f(argv[1],std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
  if(!f||b.empty())return 2;
  cv->rbf_load(b.data(),b.size());
  const CV::bmux_type_t pkr[]={CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
  const CV::bmux_type_t clocks[]={CV::CLK0_SEL,CV::CLK1_SEL,CV::CLK2_SEL};
  const CV::bmux_type_t invs[]={CV::CLK0_INV,CV::CLK1_INV,CV::CLK2_INV};
  const CV::bmux_type_t ena_en[]={CV::EN0_EN,CV::EN1_EN,CV::EN2_EN};
  const CV::bmux_type_t ena_inv[]={CV::EN0_NINV,CV::EN1_NINV,CV::EN2_NINV};
  const CV::bmux_type_t sclr_dis[]={CV::TSCLR_DIS,CV::BSCLR_DIS};
  const CV::bmux_type_t ef_sel[]={CV::TEF_SEL,CV::BEF_SEL};
  int x,y,a,s;
  while(std::cin>>x) {
    if(!(std::cin>>y>>a>>s)||x<0||x>127||y<0||y>127||a<0||a>=10||s<0||s>=4)return 3;
    CV::xycoords pos(x,y);auto blocks=cv->pos_get_bels(pos);
    CV::block_type_t bt=std::find(blocks.begin(),blocks.end(),CV::LAB)!=blocks.end()?CV::LAB:CV::MLAB;
    if(std::find(blocks.begin(),blocks.end(),bt)==blocks.end())return 3;
    CV::bmux_setting_t input,clock,source;
    if(!cv->bmux_get(bt,pos,pkr[s],a,input)||input.type!=CV::MT_BOOL)return 3;
    if(!cv->bmux_get(bt,pos,(s==0||s==3)?CV::TCLK_SEL:CV::BCLK_SEL,a,clock)||clock.type!=CV::MT_MUX)return 3;
    int index=clock.s==CV::CLK0?0:clock.s==CV::CLK1?1:clock.s==CV::CLK2?2:-1;
    if(index<0||!cv->bmux_get(bt,pos,clocks[index],0,source))return 4;
    std::cout<<x<<'\t'<<y<<'\t'<<a<<'\t'<<s<<'\t'<<(bt==CV::LAB?"LAB":"MLAB")
             <<'\t'<<input.s<<'\t'<<index<<'\t'<<CV::bmux_type_names[source.s];
    for(auto field:{invs[index],ena_en[index],ena_inv[index],sclr_dis[s/2],CV::SCLR_DIS,CV::SCLR_INV,CV::SCLR_SEL,ef_sel[s/2]}) {
      CV::bmux_setting_t v;
      if(!cv->bmux_get(bt,pos,field,(field==sclr_dis[s/2]||field==ef_sel[s/2])?a:0,v))return 5;
      std::cout<<'\t'<<CV::bmux_type_names[field]<<'=';
      if(v.type==CV::MT_MUX)std::cout<<CV::bmux_type_names[v.s];
      else if(v.type==CV::MT_BOOL)std::cout<<v.s;
      else return 5;
      // Resolve selected inputs with the device API rather than equating
      // a decoded selector name with an independently observed route.
      if(field==CV::SCLR_SEL||field==ef_sel[s/2]) {
        CV::port_type_t port;int block_index=-1,port_index=-1;
        if(field==CV::SCLR_SEL) {
          port=CV::DATAIN;
          port_index=v.s==CV::DIN0?0:v.s==CV::DIN1?1:v.s==CV::DIN2?2:v.s==CV::DIN3?3:-1;
          if(port_index<0)return 6;
        } else {
          if(v.s!=CV::E&&v.s!=CV::F)return 6;
          port=s/2==0?(v.s==CV::E?CV::E0:CV::F1):(v.s==CV::E?CV::E1:CV::F0);
          block_index=a;
        }
        auto ri=cv->pnode_to_rnode(CV::pnode_coords(bt,pos,port,block_index,port_index));
        if(ri==0xffffffff)return 6;
        auto node=cv->ri2rc(ri);
        if(node.t()!=CV::GOUT||node.p()!=pos)return 6;
        std::cout<<'\t'<<(field==CV::SCLR_SEL?"SCLR_INPUT_MUX":"EF_INPUT_MUX")<<'='<<node.z();
      }
    }
    std::cout<<'\n';
  }
  if(!std::cin.eof())return 3;
}
