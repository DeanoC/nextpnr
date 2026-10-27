/* Guarded Boolean LUT references matched to the existing native GOUT/COMBOUT cuts. */
#include "nextpnr.h"
#include "util.h"
#include "lut_core_reference_data.h"

#include <algorithm>
#include <limits>

NEXTPNR_NAMESPACE_BEGIN

namespace {
using CV = mistral::CycloneV;

int width(const CellInfo *cell)
{
    return cell->type == id_MISTRAL_ALUT6 ? 6 : cell->type == id_MISTRAL_ALUT5 ? 5 :
           cell->type == id_MISTRAL_ALUT4 ? 4 : cell->type == id_MISTRAL_ALUT3 ? 3 :
           cell->type == id_MISTRAL_ALUT2 ? 2 : cell->type.in(id_MISTRAL_BUF,id_MISTRAL_NOT) ? 1 : 0;
}

int physical_index(IdString pin)
{
    if(pin==id_A)return 0;
    if(pin==id_B)return 1;
    if(pin==id_C)return 2;
    if(pin==id_D)return 3;
    if(pin.in(id_E0,id_E1))return 4;
    if(pin.in(id_F0,id_F1))return 5;
    return -1;
}
}

const char *Arch::lut_core_reference_identity() const
{
    return lut_core_reference::reference_sha256;
}

bool Arch::lut_core_delay(const CellInfo *cell, IdString from, IdString to, DelayQuad &delay) const
{
    using namespace lut_core_reference;
    if (!use_lut_core_reference)
        return false;
    const int n = width(cell);
    if (!n || to != id_Q)
        return false;
    const std::array<IdString,6> inputs{id_A,id_B,id_C,id_D,id_E,id_F};
    if(std::find(inputs.begin(),inputs.begin()+n,from)==inputs.begin()+n)
        return false;
    auto reject=[&](const char *reason) {
        log_error("LUT core reference does not cover '%s.%s': %s.\n",
                  cell->name.c_str(getCtx()),from.c_str(getCtx()),reason);
    };
    if(!cell->getPort(from) && (cell->get_pin_state(from)==PIN_0 || cell->get_pin_state(from)==PIN_1))
        return false; // An explicit inactive constant is not a routed data path.
    if(getChipName()!="5CSEBA6U23I7")
        reject("require the characterized 5CSEBA6U23I7 device");
    if(n>1) {
        auto init=cell->params.find(id_LUT);
        if(init==cell->params.end() || !init->second.is_fully_def() || init->second.size()>64)
            reject("missing, unknown or oversized LUT truth table");
    }
    const bool owner=cell->bel!=BelId() && getBoundBelCell(cell->bel)==cell;
    if(bitstream_configured && !owner)
        reject("a configured model requires its actual placed owner");
    IdString physical=lut_placement_pin(cell,from);
    auto pin=cell->pin_data.find(from);
    if(owner && pin!=cell->pin_data.end() && pin->second.bel_pins.size()==1)
        physical=pin->second.bel_pins.front();
    const int p=physical_index(physical);
    if(p<0)reject("uncharacterized physical input");
    Bounds b{};
    if(!owner) {
        b=planning[n==6][p];
    } else {
        if(!getBelType(cell->bel).in(id_MISTRAL_COMB,id_MISTRAL_MCOMB))
            reject("the placed owner is not a combinational ALM half");
        const auto &site=bel_data(cell->bel).lab_data;
        if(site.lab>=labs.size() || site.alm>=10 || site.idx>=2)
            reject("invalid ALM geometry");
        const auto &lab=labs.at(site.lab);
        const int block=lab.is_mlab;
        int mode=-1;
        if(bitstream_configured) {
            const auto bt=lab.is_mlab?CV::MLAB:CV::LAB;
            auto mux=[&](CV::bmux_type_t field) {
                CV::bmux_setting_t v{};
                if(!cyclonev->bmux_get(bt,cell->bel.pos,field,site.alm,v) || v.type!=CV::MT_MUX)
                    reject("missing native mode selector");
                return v.s;
            };
            const auto top=mux(CV::TMODE),bottom=mux(CV::BMODE);
            if(top==CV::E_0 && bottom==CV::E_1)mode=0;
            if(top==CV::D_E && bottom==CV::C_E)mode=1;
            if(mode<0 || (n==6 && mode!=1) || mux(CV::ARITH_SEL)!=CV::LUT)
                reject("feedback, RAM, arithmetic or incompatible LUT modes are uncharacterized");
            if(pin==cell->pin_data.end() || pin->second.bel_pins.size()!=1 ||
               !cell->getPort(from) || !cell->getPort(from)->driver.cell ||
               (cell->get_pin_state(from)!=PIN_SIG && cell->get_pin_state(from)!=PIN_INV))
                reject("a live input lacks its driven physical pin identity");
            if((physical.in(id_E0,id_F0) && site.idx!=0) ||
               (physical.in(id_E1,id_F1) && site.idx!=1))
                reject("the private input belongs to another ALM half");
            const CV::port_type_t native[]{CV::A,CV::B,CV::C,CV::D,
                site.idx?CV::E1:CV::E0,site.idx?CV::F1:CV::F0};
            const auto expected=get_port(bt,cell->bel.pos.x(),cell->bel.pos.y(),site.alm,native[p]);
            auto port=cell->ports.find(from);
            auto *net=port==cell->ports.end()?nullptr:port->second.net;
            if(!net || !net->users.count(port->second.user_idx) ||
               net->users.at(port->second.user_idx).cell!=cell ||
               net->users.at(port->second.user_idx).port!=from)
                reject("the connected input lacks its actual routed user identity");
            PortRef sink; sink.cell=const_cast<CellInfo *>(cell);sink.port=from;
            if(expected==WireId() || expected.is_nextpnr_created() || expected.node.t()!=CV::GOUT ||
               getBelPinWire(cell->bel,physical)!=expected || getCtx()->getNetinfoSinkWire(net,sink,0)!=expected ||
               !net->wires.count(expected) || getBoundWireNet(expected)!=net)
                reject("the input lacks its native net-owned GOUT cut");
            auto *out=cell->getPort(id_Q);
            if(out) {
                auto q=cell->pin_data.find(id_Q);
                auto cut=getBelPinWire(cell->bel,id_COMBOUT);
                if(q==cell->pin_data.end() || q->second.bel_pins!=std::vector<IdString>{id_COMBOUT} ||
                   out->driver.cell!=cell || out->driver.port!=id_Q || getCtx()->getNetinfoSourceWire(out)!=cut ||
                   !out->wires.count(cut) || getBoundWireNet(cut)!=out)
                    reject("the output lacks its native net-owned COMBOUT cut");
            }
        }
        bool found=false;
        b={std::numeric_limits<int>::max(),std::numeric_limits<int>::min(),
           std::numeric_limits<int>::max(),std::numeric_limits<int>::min()};
        for(int m=0;m<2;++m) {
            if((mode>=0 && m!=mode) || (n==6 && m!=1))continue;
            const auto &entry=entries[block][m][site.alm][site.idx][p];
            if(!entry.valid)continue;
            const auto &v=entry.corner[0]; // Match the public native slow100 routing/FF/GPIO engine.
            found=true;
            b.rmin=std::min(b.rmin,v.rmin);b.rmax=std::max(b.rmax,v.rmax);
            b.fmin=std::min(b.fmin,v.fmin);b.fmax=std::max(b.fmax,v.fmax);
        }
        if(!found)reject("no complete observation for this physical mode and input");
    }
    delay=DelayQuad{b.rmin,b.rmax,b.fmin,b.fmax};
    return true;
}

NEXTPNR_NAMESPACE_END
