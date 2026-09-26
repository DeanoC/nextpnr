/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  Lofty <dan.ravensloft@gmail.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <sstream>

#include "log.h"
#include "nextpnr.h"

#include "placer1.h"
#include "placer_heap.h"
#include "router1.h"
#include "router2.h"
#include "gpurouter.h"
#include "timing.h"
#include "util.h"

#include "cyclonev.h"
#include "dsp.h"

NEXTPNR_NAMESPACE_BEGIN

using namespace mistral;

namespace {

constexpr float router2_retry_margin = 1.10f;

bool dsp_bool_param(const dict<IdString, Property> &params, IdString key, bool def = false)
{
    auto it = params.find(key);
    if (it == params.end())
        return def;
    if (!it->second.is_string)
        return it->second.as_bool();
    const std::string &value = it->second.as_string();
    if (value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "reg" ||
        value == "registered")
        return true;
    if (value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "bypass")
        return false;
    return def;
}

bool dsp_shared_config_equal(const CellInfo *a, const CellInfo *b)
{
    if (dsp_bool_param(a->params, id_A_SIGNED, true) != dsp_bool_param(b->params, id_A_SIGNED, true) ||
        dsp_bool_param(a->params, id_B_SIGNED, true) != dsp_bool_param(b->params, id_B_SIGNED, true) ||
        dsp_bool_param(a->params, id_C_SIGNED, true) != dsp_bool_param(b->params, id_C_SIGNED, true) ||
        dsp_bool_param(a->params, id_D_SIGNED, true) != dsp_bool_param(b->params, id_D_SIGNED, true))
        return false;
    for (IdString key : {id_INREG_CTRL_AX, id_INREG_CTRL_AY, id_INREG_CTRL_AZ, id_INREG_CTRL_BX,
                         id_INREG_CTRL_BY, id_INREG_CTRL_BZ, id_OREG_CTRL, id_PREADDER_EN, id_PREADDER_SUB,
                         id_CASCADE_EN, id_CASCADE_1ST_EN, id_CHAIN_OUTPUT_EN}) {
        if (dsp_bool_param(a->params, key) != dsp_bool_param(b->params, key))
            return false;
    }
    for (IdString port : {id_CLK, id_ACLR, id_ENA, id_ACCUMULATE, id_SUB, id_NEGATE, id_LOADCONST}) {
        const NetInfo *an = a->getPort(port);
        const NetInfo *bn = b->getPort(port);
        // Hard constants have no net after packing. Keep their retained pin
        // states in the shared-control comparison so opposite constants do
        // not become legal occupants of one three-lane DSP tile.
        if (an != bn || a->get_pin_state(port) != b->get_pin_state(port))
            return false;
    }
    return true;
}

} // namespace

void IdString::initialize_arch(const BaseCtx *ctx)
{
#define X(t) initialize_add(ctx, #t, ID_##t);

#include "constids.inc"

#undef X
}

CycloneV::rnode_coords Arch::find_rnode(CycloneV::block_type_t bt, int x, int y, CycloneV::port_type_t port, int bi,
                                   int pi) const
{
    auto pn1 = CycloneV::pnode_coords{bt, x, y, port, bi, pi};
    auto rn1 = cyclonev->pnode_to_rnode(pn1);
    if (rn1 != 0xffffffff)
        return cyclonev->ri2rc(rn1);

    if (bt == CycloneV::GPIO) {
        auto pn2 = cyclonev->p2p_to(pn1);
        if (pn2 == CycloneV::pnode_coords{}) {
            auto pnv = cyclonev->p2p_from(pn1);
            if (!pnv.empty())
                pn2 = pnv[0];
        }
        auto pn3 = cyclonev->hmc_get_bypass(pn2);
        auto rn2 = cyclonev->pnode_to_rnode(pn3);
        return rn2 == 0xffffffff ? CycloneV::rnode_coords{} : cyclonev->ri2rc(rn2);
    }

    return CycloneV::rnode_coords{};
}

WireId Arch::get_port(CycloneV::block_type_t bt, int x, int y, int bi, CycloneV::port_type_t port, int pi) const
{
    auto rn = find_rnode(bt, x, y, port, bi, pi);
    if (rn)
        return WireId(rn);

    log_error("Trying to connect unknown node %s\n", CycloneV::pnode_coords{bt, x, y, port, bi, pi}.to_string().c_str());
}

bool Arch::has_port(CycloneV::block_type_t bt, int x, int y, int bi, CycloneV::port_type_t port, int pi) const
{
    return find_rnode(bt, x, y, port, bi, pi) != 0;
}

Arch::Arch(ArchArgs args)
{
    this->args = args;
    this->cyclonev = mistral::CycloneV::get_model(args.device);
    NPNR_ASSERT(this->cyclonev != nullptr);

    // Setup fast identifier maps
    for (int i = 0; i < 1024; i++) {
        IdString int_id = idf("%d", i);
        int2id.push_back(int_id);
        id2int[int_id] = i;
    }

    for (int t = int(CycloneV::NONE); t <= int(CycloneV::DCMUX); t++) {
        IdString rnode_id = id(CycloneV::rnode_type_names[t]);
        rn_t2id.push_back(rnode_id);
        id2rn_t[rnode_id] = CycloneV::rnode_type_t(t);
    }

    log_info("Initialising bels...\n");
    bels_by_tile.resize(cyclonev->get_tile_sx() * cyclonev->get_tile_sy());

    for (auto lab_pos : cyclonev->lab_get_pos())
        create_lab(lab_pos.x(), lab_pos.y(), /*is_mlab=*/false);

    for (auto mlab_pos : cyclonev->mlab_get_pos())
        create_lab(mlab_pos.x(), mlab_pos.y(), /*is_mlab=*/true);

    for (auto gpio_pos : cyclonev->gpio_get_pos())
        create_gpio(gpio_pos.x(), gpio_pos.y());

    for (auto cmuxh_pos : cyclonev->cmuxh_get_pos())
        create_clkbuf(cmuxh_pos.x(), cmuxh_pos.y());

    create_control(cyclonev->ctrl_get_pos()[0].x(), cyclonev->ctrl_get_pos()[0].y());

    auto hps_pos = cyclonev->hps_get_pos();
    if (!hps_pos.empty()) {
        create_hps_mpu_general_purpose(hps_pos[CycloneV::I_HPS_MPU_GENERAL_PURPOSE].x(),
                                       hps_pos[CycloneV::I_HPS_MPU_GENERAL_PURPOSE].y());
        for (int index = 0; index < 4; index++) {
            auto pos = hps_pos[CycloneV::I_HPS_PERIPHERAL_I2C + index];
            create_hps_peripheral_i2c(pos.x(), pos.y());
        }
    }

    for (auto m10k_pos : cyclonev->m10k_get_pos())
        create_m10k(m10k_pos.x(), m10k_pos.y());

    for (auto dsp_pos : cyclonev->dsp_get_pos())
        create_dsp(dsp_pos.x(), dsp_pos.y());

    create_plls();

    // This import takes about 5s, perhaps long term we can speed it up, e.g. defer to Mistral more...
    log_info("Initialising routing graph...\n");
    int pip_count = 0;
    // Sources are stored as node indices; resolve them through one flat
    // table rather than an object lookup per pip (27 M of them), and walk
    // the node objects in memory order rather than index order, which
    // the library does not keep aligned
    const uint32_t rnode_count = cyclonev->rnode_index_count();
    std::vector<CycloneV::rnode_coords> ri2rc(rnode_count);
    std::vector<const CycloneV::rnode_object *> objects;
    objects.reserve(rnode_count);
    for (uint32_t ri = 0; ri < rnode_count; ri++) {
        const auto *rnode = cyclonev->ri2ro(ri);
        if (rnode == nullptr)
            continue;
        ri2rc[ri] = rnode->rc();
        objects.push_back(rnode);
    }
    std::sort(objects.begin(), objects.end());
    for (const auto *rnode : objects) {
        WireId dst_wire(rnode->rc());
        for (const auto *src = rnode->sources_begin(); src != rnode->sources_end(); ++src) {
            WireId src_wire(ri2rc.at(*src));
            wires[dst_wire].wires_uphill.push_back(src_wire);
            wires[src_wire].wires_downhill.push_back(dst_wire);
            ++pip_count;
        }
    }

    log_info("    imported %d wires and %d pips\n", int(wires.size()), pip_count);

    BaseArch::init_cell_types();
    BaseArch::init_bel_buckets();
}

int Arch::getTileBelDimZ(int x, int y) const
{
    // This seems like a reasonable upper bound
    return 256;
}

BelId Arch::getBelByName(IdStringList name) const
{
    if (name.size() != 4)
        return BelId();
    auto x_it = id2int.find(name[1]);
    auto y_it = id2int.find(name[2]);
    auto z_it = id2int.find(name[3]);
    if (x_it == id2int.end() || y_it == id2int.end() || z_it == id2int.end())
        return BelId();
    int x = x_it->second;
    int y = y_it->second;
    int z = z_it->second;
    if (x < 0 || x >= getGridDimX() || y < 0 || y >= getGridDimY())
        return BelId();
    const auto &bels = bels_by_tile.at(pos2idx(x, y));
    if (z < 0 || z >= int(bels.size()) || bels.at(z).type != name[0])
        return BelId();
    return BelId(CycloneV::xycoords{x, y}, z);
}

IdStringList Arch::getBelName(BelId bel) const
{
    int x = bel.pos.x();
    int y = bel.pos.y();
    int z = bel.z & 0xFF;

    std::array<IdString, 4> ids{
            getBelType(bel),
            int2id.at(x),
            int2id.at(y),
            int2id.at(z),
    };

    return IdStringList(ids);
}

void Arch::note_reserved_bel(const std::string &name)
{
    BelId bel = getCtx()->getBelByNameStr(name);
    if (bel == BelId())
        log_error("FES_RESERVED_BEL '%s' is not a device BEL.\n", name.c_str());
    // FES_RESERVED_BEL has no region syntax of its own; it joins the default
    // "cart" region, matching the legacy boolean FES_SLOT=1 cart tag.
    IdString region_id = id("cart");
    auto absorbed = fes_region_absorbed_by.find(region_id);
    if (absorbed != fes_region_absorbed_by.end())
        log_error("FES_RESERVED_BEL '%s' targets region 'cart', already absorbed into group '%s'.\n", name.c_str(),
                  absorbed->second.c_str(getCtx()));
    auto existing = fes_bel_region.find(bel);
    if (existing != fes_bel_region.end() && existing->second != region_id)
        log_error("FES_RESERVED_BEL '%s' overlaps region '%s'.\n", name.c_str(), existing->second.c_str(getCtx()));
    fes_bel_region[bel] = region_id;
    fes_region_bels[region_id].insert(bel);
    // Deliberately not added to fes_declared_region_names: a BEL reservation
    // augments whichever region owns "cart" (by default or via an explicit
    // FES_RESERVED_RECT/_GROUP declared before or after it) rather than
    // declaring a rectangle of its own, so it must not trip the "region name
    // already declared" duplicate check in note_reserved_rect/_group.
    log_info("FES reserved BEL %s (region 'cart')\n", name.c_str());
}

void Arch::note_reserved_rect(const std::string &spec)
{
    std::istringstream in(spec);
    std::vector<std::string> tokens;
    for (std::string tok; in >> tok;)
        tokens.push_back(tok);
    std::string name = "cart";
    int x0, y0, x1, y1;
    bool numeric_first = false;
    if (!tokens.empty()) {
        std::istringstream probe(tokens.front());
        int ignored;
        numeric_first = bool(probe >> ignored) && probe.eof();
    }
    std::istringstream fields;
    if (tokens.size() == 5 && !numeric_first) {
        name = tokens.front();
        fields.str(tokens.at(1) + " " + tokens.at(2) + " " + tokens.at(3) + " " + tokens.at(4));
    } else if (tokens.size() == 4 && numeric_first) {
        fields.str(tokens.at(0) + " " + tokens.at(1) + " " + tokens.at(2) + " " + tokens.at(3));
    } else {
        log_error("FES_RESERVED_RECT '%s' must be 'x0 y0 x1 y1' or 'name x0 y0 x1 y1'.\n", spec.c_str());
    }
    if (!(fields >> x0 >> y0 >> x1 >> y1))
        log_error("FES_RESERVED_RECT '%s' must be 'x0 y0 x1 y1' or 'name x0 y0 x1 y1'.\n", spec.c_str());
    if (x1 < x0 || y1 < y0)
        log_error("FES_RESERVED_RECT '%s' is empty.\n", spec.c_str());
    IdString region_id = id(name);
    auto rect_absorbed = fes_region_absorbed_by.find(region_id);
    if (rect_absorbed != fes_region_absorbed_by.end())
        log_error("FES_RESERVED_RECT '%s' region name '%s' was absorbed into group '%s' by FES_RESERVED_RECT_GROUP.\n",
                  spec.c_str(), name.c_str(), rect_absorbed->second.c_str(getCtx()));
    if (fes_declared_region_names.count(region_id))
        log_error("FES_RESERVED_RECT '%s' region name '%s' is already declared.\n", spec.c_str(), name.c_str());
    int count = 0;
    for (BelId bel : getBels()) {
        Loc loc = getBelLocation(bel);
        if (loc.x < x0 || loc.x > x1 || loc.y < y0 || loc.y > y1)
            continue;
        auto existing = fes_bel_region.find(bel);
        if (existing != fes_bel_region.end() && existing->second != region_id)
            log_error("FES_RESERVED_RECT '%s' region '%s' overlaps region '%s' at BEL %s.\n", spec.c_str(),
                      name.c_str(), existing->second.c_str(getCtx()), getBelName(bel).str(getCtx()).c_str());
        fes_bel_region[bel] = region_id;
        fes_region_bels[region_id].insert(bel);
        ++count;
    }
    fes_reserved_rects.push_back(FesReservedRect{name, x0, y0, x1, y1});
    fes_has_reserved_rect = true;
    fes_declared_region_names.insert(region_id);
    log_info("FES reserved rect '%s' %d %d %d %d (%d bels)\n", name.c_str(), x0, y0, x1, y1, count);
}

void Arch::note_reserved_rect_group(const std::string &spec)
{
    std::istringstream in(spec);
    std::vector<std::string> tokens;
    for (std::string tok; in >> tok;)
        tokens.push_back(tok);
    if (tokens.size() < 3)
        log_error("FES_RESERVED_RECT_GROUP '%s' must be 'name region1 region2 [region3 ...]' (at least two source "
                  "regions).\n",
                  spec.c_str());
    const std::string &group_name = tokens.front();
    IdString group_id = id(group_name);
    if (fes_declared_region_names.count(group_id))
        log_error("FES_RESERVED_RECT_GROUP '%s' region name '%s' is already declared.\n", spec.c_str(),
                  group_name.c_str());
    // A big card can claim several already-declared regions at once; every
    // member is fully absorbed (its BELs move to the group, and its name is
    // blocked from independent use), mirroring a large expansion card
    // physically covering its smaller neighbours' backplane slots.
    std::set<IdString> members;
    // A loose FES_RESERVED_BEL may already have tagged BELs under this exact
    // name (most commonly the default "cart"), with no rectangle of its own
    // and so no fes_declared_region_names entry to reject above; fold those
    // BELs into the merged set instead of treating the name as taken, since
    // a BEL reservation always augments whichever rectangle/group ends up
    // owning that name.
    std::set<BelId> merged;
    auto preexisting = fes_region_bels.find(group_id);
    if (preexisting != fes_region_bels.end())
        merged = preexisting->second;
    for (size_t i = 1; i < tokens.size(); ++i) {
        IdString member_id = id(tokens[i]);
        if (member_id == group_id)
            log_error("FES_RESERVED_RECT_GROUP '%s' cannot list its own group name '%s' as a member.\n",
                      spec.c_str(), tokens[i].c_str());
        if (!members.insert(member_id).second)
            log_error("FES_RESERVED_RECT_GROUP '%s' lists region '%s' twice.\n", spec.c_str(), tokens[i].c_str());
        // A member must be a region actually declared with FES_RESERVED_RECT
        // or FES_RESERVED_RECT_GROUP, not just a name that happens to have
        // loose FES_RESERVED_BEL content (fes_region_bels alone isn't proof
        // of declaration); the group's own name has separate, narrower
        // fold-in handling above.
        if (!fes_declared_region_names.count(member_id))
            log_error("FES_RESERVED_RECT_GROUP '%s' region '%s' was never declared with FES_RESERVED_RECT.\n",
                      spec.c_str(), tokens[i].c_str());
        auto absorbed = fes_region_absorbed_by.find(member_id);
        if (absorbed != fes_region_absorbed_by.end())
            log_error("FES_RESERVED_RECT_GROUP '%s' region '%s' was already absorbed into group '%s'.\n",
                      spec.c_str(), tokens[i].c_str(), absorbed->second.c_str(getCtx()));
        auto bels = fes_region_bels.find(member_id);
        if (bels == fes_region_bels.end())
            log_error("FES_RESERVED_RECT_GROUP '%s' region '%s' was never declared with FES_RESERVED_RECT.\n",
                      spec.c_str(), tokens[i].c_str());
        for (BelId bel : bels->second)
            merged.insert(bel);
    }
    for (IdString member_id : members) {
        fes_region_absorbed_by[member_id] = group_id;
        fes_region_bels.erase(member_id);
    }
    // Nested groups: a member absorbed here may already have its own
    // descendants pointing at it (e.g. "inner" absorbed "s1" earlier, and
    // this group now absorbs "inner"). Retarget those so a stale lookup
    // resolves straight to the outermost, still-usable group instead of a
    // name that is itself no longer independently available.
    for (auto &entry : fes_region_absorbed_by) {
        if (members.count(entry.second))
            entry.second = group_id;
    }
    for (BelId bel : merged)
        fes_bel_region[bel] = group_id;
    fes_region_bels[group_id] = std::move(merged);
    fes_declared_region_names.insert(group_id);
    log_info("FES reserved rect group '%s' absorbs %zu regions (%zu bels); they are no longer independently "
             "available.\n",
             group_name.c_str(), members.size(), fes_region_bels.at(group_id).size());
}

IdString Arch::fes_cell_slot_region(const CellInfo *cell) const
{
    if (cell == nullptr || !cell->attrs.count(id("FES_SLOT")))
        return IdString();
    const Property &prop = cell->attrs.at(id("FES_SLOT"));
    // Legacy carts tag cells with the bare boolean FES_SLOT=1; treat that as
    // the default "cart" region so existing single-socket QSF/cart recipes
    // keep working unchanged.
    if (prop.is_string)
        return prop.as_string().empty() ? IdString() : id(prop.as_string());
    return prop.as_bool() ? id("cart") : IdString();
}

bool Arch::fes_placement_allowed(BelId bel, const CellInfo *cell, bool explain_invalid) const
{
    if (fes_cell_is_slot(cell) && bel_data(bel).type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) {
        // A LAB shares modes and control selectors. Frozen LABs must not gain
        // new cart cells, because their existing configuration is immutable.
        const auto &lab = labs.at(bel_data(bel).lab_data.lab);
        for (const auto &alm : lab.alms) {
            for (BelId other : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1],
                                alm.ff_bels[2], alm.ff_bels[3]}) {
                const CellInfo *occupant = getBoundBelCell(other);
                if (occupant && !fes_cell_is_slot(occupant) && occupant->belStrength >= STRENGTH_LOCKED) {
                    if (explain_invalid)
                        log_info("FES slot cell %s cannot share a frozen LAB.\n", nameOf(cell));
                    return false;
                }
            }
        }
    }
    if (fes_bel_region.empty() || cell == nullptr)
        return true;
    auto region_it = fes_bel_region.find(bel);
    const bool reserved = region_it != fes_bel_region.end();
    const IdString cell_region = fes_cell_slot_region(cell);
    const bool slot_cell = cell_region != IdString();
    const bool in_own_region = reserved && slot_cell && region_it->second == cell_region;
    bool bel_locked = false;
    if (cell->attrs.count(id("BEL"))) {
        const Property &locked = cell->attrs.at(id("BEL"));
        const std::string name = locked.is_string ? locked.as_string() : locked.to_string();
        bel_locked = (name == getBelName(bel).str(getCtx()));
    }
    // Routed scaffold cells have NEXTPNR_BEL rather than BEL. The lock step
    // records their original sites; placement strength alone is insufficient
    // because placement can strengthen a newly bound, unconstrained cell.
    auto frozen = fes_frozen_cells.find(cell);
    const bool frozen_here = frozen != fes_frozen_cells.end() && frozen->second == bel && cell->bel == bel &&
                             cell->belStrength >= STRENGTH_LOCKED;
    if (reserved && !(in_own_region || bel_locked || frozen_here)) {
        if (explain_invalid)
            log_info("FES reserved BEL %s (region '%s') rejects cell %s%s.\n", getBelName(bel).str(getCtx()).c_str(),
                     region_it->second.c_str(getCtx()), nameOf(cell),
                     slot_cell ? stringf(" (region '%s')", cell_region.c_str(getCtx())).c_str() : " (unconstrained)");
        return false;
    }
    if (slot_cell && !in_own_region) {
        if (explain_invalid)
            log_info("FES slot cell %s must stay in its own reserved region '%s' (tried %s).\n", nameOf(cell),
                     cell_region.c_str(getCtx()), getBelName(bel).str(getCtx()).c_str());
        return false;
    }
    return true;
}

bool Arch::isBelLocationValid(BelId bel, bool explain_invalid) const
{
    auto &data = bel_data(bel);
    if (data.bound && !fes_placement_allowed(bel, data.bound, explain_invalid))
        return false;
    if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) {
        bool any_locked = false;
        auto occupant_ok = [&](BelId other) {
            CellInfo *cell = getBoundBelCell(other);
            if (cell == nullptr)
                return true;
            if (cell->belStrength < STRENGTH_LOCKED)
                return false;
            any_locked = true;
            return true;
        };
        const auto &alm_data = labs.at(data.lab_data.lab).alms.at(data.lab_data.alm);
        if (occupant_ok(alm_data.lut_bels[0]) && occupant_ok(alm_data.lut_bels[1]) && occupant_ok(alm_data.ff_bels[0]) &&
            occupant_ok(alm_data.ff_bels[1]) && occupant_ok(alm_data.ff_bels[2]) && occupant_ok(alm_data.ff_bels[3]) &&
            any_locked)
            return true;
        if (data.bound == nullptr) {
            const auto &lab_data = labs.at(data.lab_data.lab);
            for (const auto &alm : lab_data.alms) {
                for (BelId other : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2],
                                    alm.ff_bels[3]}) {
                    CellInfo *cell = getBoundBelCell(other);
                    if (cell != nullptr && cell->belStrength >= STRENGTH_LOCKED)
                        return true;
                }
            }
        }
    }
    if (data.type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                     id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED) &&
        data.bound) {
        // The mode-specific BELs are alternate views of one physical DSP
        // tile. Three M9 lanes may share a tile, while a wide multiplier owns
        // the tile and cannot coexist with another mode.
        for (BelId other : getBelsByTile(bel.pos.x(), bel.pos.y())) {
            if (other == bel)
                continue;
            const auto &other_data = bel_data(other);
            if (!other_data.bound ||
                !other_data.type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                                    id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED))
                continue;
            if (other_data.type != data.type) {
                if (explain_invalid)
                    log_info("DSP tile already contains multiplier mode %s; cannot place %s here.\n",
                             nameOf(other_data.type), nameOf(data.type));
                return false;
            }
            if (data.type == id_MISTRAL_MUL9X9 && !dsp_shared_config_equal(data.bound, other_data.bound)) {
                if (explain_invalid)
                    log_info("DSP tile already contains an M9 lane with different shared controls.\n");
                return false;
            }
        }
    }
    if (data.type == id_MISTRAL_CLKENA && data.block_index != 2 && data.bound) {
        auto input = data.bound->getPort(id_A);
        if (!input || !input->driver.cell || input->driver.cell->type != id_altera_pll ||
            input->driver.cell->bel == BelId())
            return false;
        WireId source = getBelPinWire(input->driver.cell->bel, input->driver.port);
        WireId dest = getBelPinWire(bel, id_A);
        return pll_clock_select.count(PipId(source.node, dest.node));
    }
    if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB)) {
        return is_alm_legal(data.lab_data.lab, data.lab_data.alm) && check_lab_input_count(data.lab_data.lab) &&
               check_mlab_groups(data.lab_data.lab);
    } else if (data.type == id_MISTRAL_FF) {
        return is_alm_legal(data.lab_data.lab, data.lab_data.alm) && check_lab_input_count(data.lab_data.lab) &&
               is_lab_ctrlset_legal(data.lab_data.lab) && check_mlab_groups(data.lab_data.lab);
    }
    return true;
}

void Arch::update_bel(BelId bel)
{
    auto &data = bel_data(bel);
    if (data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) {
        update_alm_input_count(data.lab_data.lab, data.lab_data.alm);
    }
}

WireId Arch::getWireByName(IdStringList name) const
{
    // non-mistral wires
    auto found_npnr = npnr_wirebyname.find(name);
    if (found_npnr != npnr_wirebyname.end())
        return found_npnr->second;
    // mistral wires
    NPNR_ASSERT(name.size() == 4);
    CycloneV::rnode_type_t ty = id2rn_t.at(name[0]);
    int x = id2int.at(name[1]);
    int y = id2int.at(name[2]);
    int z = id2int.at(name[3]);
    return WireId(CycloneV::rnode_coords{ty, x, y, z});
}

IdStringList Arch::getWireName(WireId wire) const
{
    if (wire.is_nextpnr_created()) {
        // non-mistral wires
        std::array<IdString, 4> ids{
                id_WIRE,
                int2id.at(wire.node.x()),
                int2id.at(wire.node.y()),
                wires.at(wire).name_override,
        };
        return IdStringList(ids);
    } else {
        std::array<IdString, 4> ids{
                rn_t2id.at(wire.node.t()),
                int2id.at(wire.node.x()),
                int2id.at(wire.node.y()),
                int2id.at(wire.node.z()),
        };
        return IdStringList(ids);
    }
}

PipId Arch::getPipByName(IdStringList name) const
{
    WireId src = getWireByName(name.slice(0, 4));
    WireId dst = getWireByName(name.slice(4, 8));
    NPNR_ASSERT(src != WireId());
    NPNR_ASSERT(dst != WireId());
    return PipId(src.node, dst.node);
}

IdStringList Arch::getPipName(PipId pip) const
{
    return IdStringList::concat(getWireName(getPipSrcWire(pip)), getWireName(getPipDstWire(pip)));
}

std::vector<BelId> Arch::getBelsByTile(int x, int y) const
{
    // This should probably be redesigned, but it's a hack.
    std::vector<BelId> bels;
    if (x >= 0 && x < cyclonev->get_tile_sx() && y >= 0 && y < cyclonev->get_tile_sy()) {
        for (size_t i = 0; i < bels_by_tile.at(pos2idx(x, y)).size(); i++)
            bels.push_back(BelId(CycloneV::xycoords{x, y}, i));
    }

    return bels;
}

IdString Arch::getBelType(BelId bel) const { return bel_data(bel).type; }

std::vector<IdString> Arch::getBelPins(BelId bel) const
{
    std::vector<IdString> pins;
    for (auto &p : bel_data(bel).pins)
        pins.push_back(p.first);
    return pins;
}

bool Arch::isValidBelForCellType(IdString cell_type, BelId bel) const
{
    // Any combinational cell type can - theoretically - be placed at a combinational ALM bel
    // The precise legality mechanics will be dealt with in isBelLocationValid.
    IdString bel_type = getBelType(bel);
    if (bel_type == id_MISTRAL_COMB)
        return is_comb_cell(cell_type);
    else if (bel_type == id_MISTRAL_MCOMB)
        return is_comb_cell(cell_type) || (cell_type == id_MISTRAL_MLAB);
    else if (bel_type == id_MISTRAL_IO)
        return is_io_cell(cell_type);
    else if (bel_type == id_MISTRAL_CLKENA)
        return is_clkbuf_cell(cell_type);
    else
        return bel_type == cell_type;
}

BelBucketId Arch::getBelBucketForCellType(IdString cell_type) const
{
    if (is_comb_cell(cell_type) || cell_type == id_MISTRAL_MLAB)
        return id_MISTRAL_COMB;
    else if (is_io_cell(cell_type))
        return id_MISTRAL_IO;
    else if (is_clkbuf_cell(cell_type))
        return id_MISTRAL_CLKENA;
    else
        return cell_type;
}

BelBucketId Arch::getBelBucketForBel(BelId bel) const
{
    IdString bel_type = getBelType(bel);
    if (bel_type == id_MISTRAL_MCOMB)
        return id_MISTRAL_COMB;
    else
        return bel_type;
}

BelId Arch::bel_by_block_idx(int x, int y, IdString type, int block_index) const
{
    auto &bels = bels_by_tile.at(pos2idx(x, y));
    for (size_t i = 0; i < bels.size(); i++) {
        auto &bel_data = bels.at(i);
        if (bel_data.type == type && bel_data.block_index == block_index)
            return BelId(CycloneV::xycoords{x, y}, i);
    }
    return BelId();
}

BelId Arch::add_bel(int x, int y, IdString name, IdString type)
{
    auto &bels = bels_by_tile.at(pos2idx(x, y));
    BelId id = BelId(CycloneV::xycoords{x, y}, bels.size());
    all_bels.push_back(id);
    bels.emplace_back();
    auto &bel = bels.back();
    bel.name = name;
    bel.type = type;
    // TODO: buckets (for example LABs and MLABs in the same bucket)
    bel.bucket = type;
    return id;
}

WireId Arch::add_wire(int x, int y, IdString name, uint64_t flags)
{
    std::array<IdString, 4> ids{
            id_WIRE,
            int2id.at(x),
            int2id.at(y),
            name,
    };
    IdStringList full_name(ids);
    auto existing = npnr_wirebyname.find(full_name);
    if (existing != npnr_wirebyname.end()) {
        // Already exists, don't create anything
        return existing->second;
    } else {
        // Determine a unique ID for the wire
        int z = 0;
        WireId id;
        while (wires.count(id = WireId(CycloneV::rnode_coords{CycloneV::rnode_type_t((z >> 10) + 128), x, y, (z & 0x3FF)})))
            z++;
        wires[id].name_override = name;
        wires[id].flags = flags;
        npnr_wirebyname[full_name] = id;
        return id;
    }
}

void Arch::reserve_route(WireId src, WireId dst)
{
    auto &dst_data = wires.at(dst);
    int idx = -1;

    for (int i = 0; i < int(dst_data.wires_uphill.size()); i++) {
        if (dst_data.wires_uphill.at(i) == src) {
            idx = i;
            break;
        }
    }

    NPNR_ASSERT(idx != -1);

    dst_data.flags = WireInfo::RESERVED_ROUTE | unsigned(idx);
}

bool Arch::wires_connected(WireId src, WireId dst) const
{
    PipId pip(src.node, dst.node);
    return getBoundPipNet(pip) != nullptr;
}

PipId Arch::add_pip(WireId src, WireId dst)
{
    wires[src].wires_downhill.push_back(dst);
    wires[dst].wires_uphill.push_back(src);
    return PipId(src.node, dst.node);
}

void Arch::add_bel_pin(BelId bel, IdString pin, PortType dir, WireId wire)
{
    auto &b = bel_data(bel);
    NPNR_ASSERT(!b.pins.count(pin));
    b.pins[pin].dir = dir;
    b.pins[pin].wire = wire;

    BelPin bel_pin;
    bel_pin.bel = bel;
    bel_pin.pin = pin;
    wires[wire].bel_pins.push_back(bel_pin);
}

void Arch::assign_default_pinmap(CellInfo *cell)
{
    if (cell->type.in(id_MISTRAL_M10K, id_MISTRAL_M10K_TDP))
        return; // M10Ks always have a custom pinmap
    for (auto &port : cell->ports) {
        auto &pinmap = cell->pin_data[port.first].bel_pins;
        if ((is_comb_cell(cell->type) || cell->type.in(id_MISTRAL_BUF, id_MISTRAL_MLAB)) &&
            comb_pinmap.count(port.first)) {
            pinmap = {comb_pinmap.at(port.first)};
            continue;
        }
        if (!pinmap.empty())
            continue; // already mapped
        pinmap.push_back(port.first); // default: assume bel pin named the same as cell pin
    }
}

void Arch::assignArchInfo()
{
    std::vector<BelId> placed;
    for (auto &cell : cells) {
        CellInfo *ci = cell.second.get();
        if (is_comb_cell(ci->type) || ci->type.in(id_MISTRAL_MLAB, id_MISTRAL_BUF))
            assign_comb_info(ci);
        else if (ci->type == id_MISTRAL_FF)
            assign_ff_info(ci);
        assign_default_pinmap(ci);
        if (ci->bel != BelId())
            placed.push_back(ci->bel);
    }
    // bindBel during JSON reload ran before combInfo existed; recount now.
    for (BelId bel : placed)
        update_bel(bel);
}

BoundingBox Arch::getRouteBoundingBox(WireId src, WireId dst) const
{
    BoundingBox bounds;
    int src_x = src.node.x();
    int src_y = src.node.y();
    int dst_x = dst.node.x();
    int dst_y = dst.node.y();
    bounds.x0 = std::min(src_x, dst_x);
    bounds.y0 = std::min(src_y, dst_y);
    bounds.x1 = std::max(src_x, dst_x);
    bounds.y1 = std::max(src_y, dst_y);
    return bounds;
}

bool Arch::place()
{
    std::string placer = str_or_default(settings, id_placer, defaultPlacer);

    if (placer == "heap") {
        PlacerHeapCfg cfg(getCtx());
        cfg.ioBufTypes.insert(id_MISTRAL_IO);
        cfg.ioBufTypes.insert(id_MISTRAL_IB);
        cfg.ioBufTypes.insert(id_MISTRAL_OB);
        cfg.cellGroups.emplace_back();
        cfg.cellGroups.back().insert({id_MISTRAL_COMB});
        cfg.cellGroups.back().insert({id_MISTRAL_FF});

        // The Cyclone V is asymmetrical enough that it's somewhat beneficial to prefer connecting things horizontally.
        cfg.hpwl_scale_x = 1;
        cfg.hpwl_scale_y = 2;

        cfg.beta = 0.5; // TODO: find a good value of beta for sensible ALM spreading
        cfg.criticalityExponent = 7;
        if (fes_any_slot_region_active) {
            // A cart confined to a small rectangle can cycle evictions for
            // a long time; report the cycling cell instead of running on.
            cfg.cellRipupLimit = std::max(cfg.cellRipupLimit, 500);
            // Legalise flip-flops LAB by LAB. HeAP's model admits one control
            // set per LAB, so key it on the signals a Cyclone V LAB really
            // has one of: clock (LabCtrlSetWorker allows one), synchronous
            // clear and synchronous load. Enables and asynchronous clears
            // have several LAB lines and are left to the full validity
            // check, which still decides legality.
            cfg.ff_bel_bucket = id_MISTRAL_FF;
            cfg.ff_control_set_groups.assign(1, {});
            for (int alm = 0; alm < 10; alm++)
                for (int ff = 0; ff < 4; ff++)
                    cfg.ff_control_set_groups.at(0).push_back(alm * 6 + 2 + ff);
            cfg.ctrl_set_max_radius = std::vector<int>{12, 12, 12, 8, 6, 4};
            // Deterministic ids keyed by net names, not pointers.
            auto ids = std::make_shared<std::map<std::array<int, 6>, int32_t>>();
            cfg.get_cell_control_set = [ids, this](Context *, const CellInfo *ci) -> int32_t {
                // Frozen shell LABs legitimately mix enables under the full
                // LAB rules; HeAP's one-set-per-LAB model must not see them.
                if (ci->type != id_MISTRAL_FF || !fes_cell_is_slot(ci))
                    return -1;
                const auto &cs = ci->ffInfo.ctrlset;
                auto sig = [](const ControlSig &s) { return s.net ? s.net->name.index : -1; };
                std::array<int, 6> key{sig(cs.clk),  int(cs.clk.inverted),  sig(cs.sclr),
                                       int(cs.sclr.inverted), sig(cs.sload), int(cs.sload.inverted)};
                auto found = ids->find(key);
                if (found == ids->end())
                    found = ids->emplace(key, int32_t(ids->size())).first;
                return found->second;
            };
        }
        if (!placer_heap(getCtx(), cfg))
            return false;
    } else if (placer == "sa") {
        if (fes_any_slot_region_active)
            log_error("The SA placer moves FES cart LUT/FF pairs and carry chains cell by cell and can end with an "
                      "unrepaired cluster; use --placer heap for cart placement.\n");
        if (!placer1(getCtx(), Placer1Cfg(getCtx())))
            return false;
    } else {
        log_error("Mistral architecture does not support placer '%s'\n", placer.c_str());
    }

    getCtx()->attrs[id_step] = std::string("place");
    archInfoToAttributes();
    return true;
}

bool Arch::route()
{
    lab_pre_route();

    route_globals();

    std::string router = str_or_default(settings, id_router, defaultRouter);
    bool result;
    if (router == "router1") {
        result = router1(getCtx(), Router1Cfg(getCtx()));
    } else if (router == "router2") {
        int pll_count = 0;
        int m10k_count = 0;
        for (const auto &cell : getCtx()->cells) {
            if (cell.second->type == id_altera_pll)
                ++pll_count;
            else if (cell.second->type == id_MISTRAL_M10K)
                ++m10k_count;
        }
        const bool multi_pll_m10k = pll_count >= 2 && m10k_count > 0;

        // Router2's final legality check invokes router1, whose normal
        // timing report treats a pre-bitstream estimate miss as an error.
        // A dense multi-clock design may intentionally take the retry below,
        // so keep that diagnostic non-fatal while the first pass is running.
        const IdString timing_allow_fail = id("timing/allowFail");
        const auto old_timing_allow_fail = settings.find(timing_allow_fail);
        const bool had_timing_allow_fail = old_timing_allow_fail != settings.end();
        Property saved_timing_allow_fail;
        if (had_timing_allow_fail)
            saved_timing_allow_fail = old_timing_allow_fail->second;
        auto restore_timing_allow_fail = [&]() {
            if (!multi_pll_m10k)
                return;
            if (had_timing_allow_fail)
                settings[timing_allow_fail] = saved_timing_allow_fail;
            else
                settings.erase(timing_allow_fail);
        };
        if (multi_pll_m10k)
            settings[timing_allow_fail] = true;
        try {
            router2(getCtx(), Router2Cfg(getCtx()));
        } catch (...) {
            restore_timing_allow_fail();
            throw;
        }
        restore_timing_allow_fail();
        result = true;

        // Router2 is fast and normally provides the best result.  Dense
        // designs combining multiple PLLs with M10Ks are more sensitive to
        // its placement-dependent timing estimate, though.  Give those
        // designs a slower router1 retry when the first route has little
        // timing margin.  This keeps router2 as the default for ordinary
        // designs while avoiding a marginal route that can fail analogue
        // signoff after bitstream generation.
        if (multi_pll_m10k) {
            TimingAnalyser timing(getCtx());
            timing.setup(false, false, true);
            bool marginal = false;
            for (const auto &clock : timing.get_timing_result().clock_fmax) {
                if (clock.second.achieved < clock.second.constraint * router2_retry_margin) {
                    marginal = true;
                    break;
                }
            }
            if (marginal) {
                log_info("Router2 timing margin is small for a multi-PLL M10K design; retrying with router1.\n");
                for (const auto &net : getCtx()->nets) {
                    if (!net.second->is_global)
                        getCtx()->ripupNet(net.first);
                }
                result = router1(getCtx(), Router1Cfg(getCtx()));
            }
        }
    } else if (router == "gpu") {
        result = gpurouter(getCtx(), GpuRouterCfg(getCtx()));
        if (result)
            result = analogue_repair();
    } else {
        log_error("Mistral architecture does not support router '%s'\n", router.c_str());
    }
    {
        // The routers optimise nextpnr's per-pip delay table; the final report
        // after bitstream generation uses Mistral's analogue model. Log the
        // table-model view so the two can be compared.
        TimingAnalyser timing(getCtx());
        timing.setup(false, false, true);
        for (const auto &clock : timing.get_timing_result().clock_fmax)
            log_info("Routed Fmax (pip delay table) for clock '%s': %.2f MHz\n", clock.first.c_str(getCtx()),
                     clock.second.achieved);
    }
    getCtx()->attrs[id_step] = std::string("route");
    save_fes_pin_maps();
    archInfoToAttributes();
    return result;
}

const std::string Arch::defaultPlacer = "heap";

const std::vector<std::string> Arch::availablePlacers = {"sa", "heap"};

const std::string Arch::defaultRouter = "router2";
const std::vector<std::string> Arch::availableRouters = {"router1", "router2", "gpu"};

NEXTPNR_NAMESPACE_END
