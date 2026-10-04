// SPDX-License-Identifier: ISC
//
// Cyclone V control block (CTRL) atoms and device-wide QSF options.
//
// Every mapping here was taken from Quartus Prime Lite 17.0.2 compiles of
// 5CSEBA6U23I7 decoded with mistral-cv; mistral/tests/control-block holds the
// oracle projects and the regression. The supported CTRL atoms need routing
// only: Quartus sets no block, inverter or option bit for them, apart from
// the CRC divider of cyclonev_crcblock. The remote-update and active-serial
// atoms are refused because Quartus accepts them only with an Active Serial
// configuration scheme, whose option bits Mistral does not decode.

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

struct ControlPin
{
    const char *name;
    PortType dir;
    CycloneV::port_type_t port;
};

struct ControlAtom
{
    const char *type;
    std::vector<ControlPin> pins;
    std::vector<const char *> params; // accepted parameters; values are checked below
};

// Atom port -> CTRL block port, as routed by Quartus.
const std::vector<ControlAtom> &control_atoms()
{
    static const std::vector<ControlAtom> atoms = {
            {"cyclonev_chipidblock",
             {{"clk", PORT_IN, CycloneV::CLOCK_CHIPID},
              {"shiftnld", PORT_IN, CycloneV::SHIFTNLD_CHIPID},
              {"regout", PORT_OUT, CycloneV::REG_OUT_CHIPID}},
             // id_value only seeds the simulation model.
             {"lpm_type", "lpm_hint", "id_value"}},
            {"cyclonev_crcblock",
             {{"clk", PORT_IN, CycloneV::CLOCK_CRC},
              {"shiftnld", PORT_IN, CycloneV::SHIFTNLD_CRC},
              {"crcerror", PORT_OUT, CycloneV::CRCERROR},
              {"regout", PORT_OUT, CycloneV::REG_OUT_CRC},
              {"endofedfullchip", PORT_OUT, CycloneV::END_OF_ED_FULLCHIP}},
             {"lpm_type", "lpm_hint", "oscillator_divider", "crc_deld_disable", "error_delay",
              "error_dra_dl_bypass", "quad_adj_err_correction", "triple_adj_err_correction"}},
            {"cyclonev_opregblock",
             {{"clk", PORT_IN, CycloneV::CLOCK_OPREG},
              {"shiftnld", PORT_IN, CycloneV::SHIFTNLD_OPREG},
              {"regout", PORT_OUT, CycloneV::REG_OUT_OPREG}},
             {"lpm_type"}},
            // The user data register of the device TAP (USER0/USER1) and the
            // core-driven TAP inputs. Quartus routes the atom's tdouser input
            // to the block port Mistral calls TDOUTAP and rejects a connection
            // to the atom's own tdoutap port (error 176304). tck/tms/tdi/tdo
            // are the dedicated JTAG pins and have no fabric route.
            {"cyclonev_jtag",
             {{"tdouser", PORT_IN, CycloneV::TDOUTAP},
              {"tckutap", PORT_OUT, CycloneV::TCKUTAP},
              {"tdiutap", PORT_OUT, CycloneV::TDIUTAP},
              {"tmsutap", PORT_OUT, CycloneV::TMSUTAP},
              {"shiftuser", PORT_OUT, CycloneV::SHIFTUSER},
              {"updateuser", PORT_OUT, CycloneV::UPDATEUSER},
              {"clkdruser", PORT_OUT, CycloneV::CLKDRUSER},
              {"runidleuser", PORT_OUT, CycloneV::RUNIDLEUSER},
              {"usr1user", PORT_OUT, CycloneV::USR1USER},
              {"tckcore", PORT_IN, CycloneV::TCKCORE},
              {"tmscore", PORT_IN, CycloneV::TMSCORE},
              {"tdicore", PORT_IN, CycloneV::TDICORE},
              {"tdocore", PORT_OUT, CycloneV::TDOCORE},
              {"corectl", PORT_IN, CycloneV::CORECTL_JTAG},
              {"ntdopinena", PORT_IN, CycloneV::NTDOPINENA}},
             {"lpm_type", "lpm_hint"}},
    };
    return atoms;
}

std::string to_upper(std::string s)
{
    for (auto &c : s)
        c = std::toupper(static_cast<unsigned char>(c));
    return s;
}

// Integer parameters arrive from Yosys as bit vectors; a hand-written
// netlist may carry a decimal string instead.
int64_t int_param(const Context *ctx, const CellInfo *ci, const char *name, int64_t def)
{
    auto found = ci->params.find(ctx->id(name));
    if (found == ci->params.end())
        return def;
    const Property &value = found->second;
    if (!value.is_string)
        return value.as_int64();
    const std::string &text = value.as_string();
    char *end = nullptr;
    long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0')
        log_error("%s '%s': parameter %s must be an integer, got '%s'.\n", ci->type.c_str(ctx), ctx->nameOf(ci),
                  name, text.c_str());
    return parsed;
}

std::string str_param(const Context *ctx, const CellInfo *ci, const char *name, const char *def)
{
    auto found = ci->params.find(ctx->id(name));
    if (found == ci->params.end())
        return to_upper(def);
    if (!found->second.is_string)
        log_error("%s '%s': parameter %s must be a string.\n", ci->type.c_str(ctx), ctx->nameOf(ci), name);
    return to_upper(found->second.as_string());
}

// CRC check frequency divisor: 1, 2, 4, ... 256. Returns its log2, which is
// the CRC_DIVIDE_ORDER option value, or -1.
int divisor_order(long long divisor)
{
    for (int order = 0; order <= 8; order++)
        if (divisor == (1LL << order))
            return order;
    return -1;
}

int crc_divide_order(const Context *ctx, const CellInfo *crc)
{
    int64_t divisor = int_param(ctx, crc, "oscillator_divider", 256);
    int order = divisor_order(divisor);
    if (order < 0)
        log_error("cyclonev_crcblock '%s': oscillator_divider must be 1, 2, 4, 8, 16, 32, 64, 128 or 256, got %lld.\n",
                  ctx->nameOf(crc), (long long)divisor);
    return order;
}

void check_params(const Context *ctx, const CellInfo *ci, const ControlAtom &atom)
{
    for (const auto &param : ci->params) {
        bool known = false;
        for (const char *name : atom.params)
            known |= param.first == ctx->id(name);
        if (!known)
            log_error("%s '%s': unsupported parameter %s.\n", atom.type, ctx->nameOf(ci), param.first.c_str(ctx));
    }
    for (const char *name : atom.params) {
        if (std::string(name) == "lpm_type" && str_param(ctx, ci, name, atom.type) != to_upper(atom.type))
            log_error("%s '%s': lpm_type must be %s.\n", atom.type, ctx->nameOf(ci), atom.type);
        if (std::string(name) == "lpm_hint" && str_param(ctx, ci, name, "UNUSED") != "UNUSED")
            log_error("%s '%s': lpm_hint must be UNUSED.\n", atom.type, ctx->nameOf(ci));
    }
    if (ci->type == ctx->id("cyclonev_crcblock")) {
        crc_divide_order(ctx, ci);
        // Only the default error-detection behaviour has a Quartus oracle.
        for (const char *flag : {"crc_deld_disable", "error_dra_dl_bypass", "quad_adj_err_correction",
                                 "triple_adj_err_correction"})
            if (str_param(ctx, ci, flag, "false") != "FALSE")
                log_error("cyclonev_crcblock '%s': %s must be \"false\".\n", ctx->nameOf(ci), flag);
        if (int_param(ctx, ci, "error_delay", 0) != 0)
            log_error("cyclonev_crcblock '%s': error_delay must be 0.\n", ctx->nameOf(ci));
    }
}

bool is_top_buffer(const Context *ctx, const CellInfo *cell, bool output)
{
    if (cell == nullptr || !ctx->ports.count(cell->name))
        return false;
    return cell->type == ctx->id("$nextpnr_iobuf") ||
           cell->type == ctx->id(output ? "$nextpnr_obuf" : "$nextpnr_ibuf");
}

// The atom's tck/tms/tdi/tdo ports stand for the dedicated JTAG pins. Quartus
// insists that tdo reaches a top-level port (error 176551), but the pins carry
// no configuration. Accept direct top-level port connections (no IO buffer:
// the Yosys blackbox marks the ports iopad_external_pin) or no connection,
// and drop them so the top-level ports are trimmed as unused.
void detach_dedicated_jtag_pins(Context *ctx, CellInfo *jtag)
{
    for (const char *name : {"tck", "tms", "tdi", "tdo"}) {
        IdString port = ctx->id(name);
        if (!jtag->ports.count(port))
            continue;
        if (NetInfo *net = jtag->getPort(port)) {
            bool tdo = port == ctx->id("tdo");
            bool direct = net->users.entries() == 1 &&
                          (tdo ? is_top_buffer(ctx, (*net->users.begin()).cell, true)
                               : is_top_buffer(ctx, net->driver.cell, false));
            if (!direct)
                log_error("cyclonev_jtag '%s': port %s is the dedicated JTAG pin; connect it directly to a top-level "
                          "port or leave it unconnected.\n",
                          ctx->nameOf(jtag), name);
            jtag->disconnectPort(port);
        }
        jtag->ports.erase(port);
    }
}

// Pins whose function a QSF option assigns. The pad settings are the Quartus
// 17.0.2 decode of each pin on 5CSEBA6U23 with the device default I/O
// standard 3.3-V LVTTL: DEV_CLRn and DEV_OE become inputs with the 1.8 V
// (NVR_LOW) input standard, the others outputs driven by the control block,
// with no fabric route.
struct DedicatedPin
{
    const char *option;   // global assignment that enables the function
    const char *function; // Quartus reserved-pin name
    const char *pin;      // 5CSEBA6U23 package pin
    bool output;
    bool open_drain;
    bool invert_data;
};

const std::vector<DedicatedPin> &dedicated_pins()
{
    static const std::vector<DedicatedPin> pins = {
            {"ENABLE_DEVICE_WIDE_RESET", "DEV_CLRn", "AB23", false, false, false},
            {"ENABLE_DEVICE_WIDE_OE", "DEV_OE", "AC24", false, false, false},
            {"ENABLE_INIT_DONE_OUTPUT", "INIT_DONE", "AA20", true, true, false},
            {"ENABLE_CRC_ERROR_PIN", "CRC_ERROR", "Y19", true, true, true},
            {"ENABLE_NCEO_OUTPUT", "nCEO", "AE25", true, false, true},
    };
    return pins;
}

// Global assignments implemented here, with the values that have a Quartus
// oracle; the first value is the Quartus default. An empty list means a
// free-form value that apply_control_option() checks itself.
struct OptionSpec
{
    const char *name;
    std::vector<const char *> values;
};

const std::vector<OptionSpec> &option_specs()
{
    static const std::vector<OptionSpec> specs = {
            {"STRATIX_JTAG_USER_CODE", {}},
            {"USE_CHECKSUM_AS_USERCODE", {"ON", "OFF"}},
            {"CRC_ERROR_CHECKING", {"OFF", "ON"}},
            {"ERROR_CHECK_FREQUENCY_DIVISOR", {}},
            {"ENABLE_CRC_ERROR_PIN", {"OFF", "ON"}},
            {"CRC_ERROR_OPEN_DRAIN", {"ON"}},
            {"ENABLE_DEVICE_WIDE_RESET", {"OFF", "ON"}},
            {"ENABLE_DEVICE_WIDE_OE", {"OFF", "ON"}},
            {"ENABLE_INIT_DONE_OUTPUT", {"OFF", "ON"}},
            {"INIT_DONE_OPEN_DRAIN", {"ON"}},
            {"ENABLE_NCEO_OUTPUT", {"OFF", "ON"}},
            {"NCEO_OPEN_DRAIN", {"ON"}},
            {"RELEASE_CLEARS_BEFORE_TRI_STATES", {"OFF", "ON"}},
            {"AUTO_RESTART_CONFIGURATION", {"ON", "OFF"}},
            {"ENABLE_OCT_DONE", {"OFF", "ON"}},
            {"RESERVE_ALL_UNUSED_PINS_WEAK_PULLUP",
             {"AS INPUT TRI-STATED WITH WEAK PULL-UP", "AS INPUT TRI-STATED", "AS INPUT TRI-STATED WITH BUS-HOLD"}},
    };
    return specs;
}

const OptionSpec *find_option_spec(const std::string &name)
{
    for (const auto &spec : option_specs())
        if (name == spec.name)
            return &spec;
    return nullptr;
}

const char *const verified_device = "5CSEBA6U23";

const CellInfo *find_crc_block(const Context *ctx)
{
    const IdString type = ctx->id("cyclonev_crcblock");
    for (const auto &entry : ctx->cells)
        if (entry.second->type == type)
            return entry.second.get();
    return nullptr;
}

} // namespace

void Arch::create_control_atoms(int x, int y)
{
    for (const auto &atom : control_atoms()) {
        // Keep a BEL out of a die whose CTRL block lacks one of its ports
        // rather than half-create it.
        bool complete = true;
        for (const auto &pin : atom.pins)
            complete &= has_port(CycloneV::CTRL, x, y, -1, pin.port, -1);
        if (!complete)
            continue;
        BelId bel = add_bel(x, y, id(atom.type), id(atom.type));
        for (const auto &pin : atom.pins)
            add_bel_pin(bel, id(pin.name), pin.dir, get_port(CycloneV::CTRL, x, y, -1, pin.port, -1));
    }
}

bool Arch::apply_control_option(const std::string &name, const std::string &raw_value)
{
    const std::string key = to_upper(name);
    const OptionSpec *spec = find_option_spec(key);
    if (spec == nullptr)
        return false;
    std::string value = to_upper(raw_value);
    if (key == "STRATIX_JTAG_USER_CODE") {
        if (value.empty() || value.size() > 8 || value.find_first_not_of("0123456789ABCDEF") != std::string::npos)
            log_error("STRATIX_JTAG_USER_CODE must be 1 to 8 hexadecimal digits, got '%s'.\n", raw_value.c_str());
    } else if (key == "ERROR_CHECK_FREQUENCY_DIVISOR") {
        char *end = nullptr;
        long long divisor = std::strtoll(value.c_str(), &end, 10);
        if (value.empty() || *end != '\0' || divisor_order(divisor) < 0)
            log_error("ERROR_CHECK_FREQUENCY_DIVISOR must be 1, 2, 4, 8, 16, 32, 64, 128 or 256, got '%s'.\n",
                      raw_value.c_str());
        value = std::to_string(divisor);
    } else {
        std::string accepted;
        bool supported = false;
        for (const char *candidate : spec->values) {
            supported |= value == candidate;
            accepted += std::string(accepted.empty() ? "" : ", ") + "\"" + candidate + "\"";
        }
        if (!supported)
            log_error("Unsupported value '%s' for %s; nextpnr implements %s.\n", raw_value.c_str(), key.c_str(),
                      accepted.c_str());
    }
    // As in Quartus, a later assignment replaces an earlier one.
    control_options[key] = value;
    return true;
}

std::string Arch::control_option(const std::string &name) const
{
    auto found = control_options.find(name);
    if (found != control_options.end())
        return found->second;
    const OptionSpec *spec = find_option_spec(name);
    NPNR_ASSERT(spec != nullptr);
    return spec->values.empty() ? std::string() : spec->values.front();
}

void Arch::pack_control_atoms()
{
    Context *ctx = getCtx();
    const IdString rublock = id("cyclonev_rublock"), asmiblock = id("cyclonev_asmiblock");
    dict<IdString, const ControlAtom *> atoms;
    for (const auto &atom : control_atoms())
        atoms[id(atom.type)] = &atom;
    dict<IdString, IdString> instance;
    std::vector<std::string> used;
    for (auto &entry : cells) {
        CellInfo *ci = entry.second.get();
        if (ci->type.in(rublock, asmiblock))
            log_error("%s '%s' is not supported: Quartus accepts it only with an Active Serial configuration scheme, "
                      "whose option bits Mistral does not decode.\n",
                      ci->type.c_str(ctx), ctx->nameOf(ci));
        auto found = atoms.find(ci->type);
        if (found == atoms.end())
            continue;
        const ControlAtom *atom = found->second;
        if (instance.count(ci->type))
            log_error("The device has one %s; '%s' and '%s' both instantiate it.\n", atom->type,
                      instance.at(ci->type).c_str(ctx), ctx->nameOf(ci));
        instance[ci->type] = ci->name;
        used.push_back(atom->type);
        bool has_bel = false;
        auto ctrl = cyclonev->ctrl_get_pos().at(0);
        for (BelId bel : getBelsByTile(ctrl.x(), ctrl.y()))
            has_bel |= getBelType(bel) == ci->type;
        if (!has_bel)
            log_error("%s '%s': the control block of %s has no such atom.\n", atom->type, ctx->nameOf(ci),
                      cyclonev->current_model()->name);
        check_params(ctx, ci, *atom);
        if (ci->type == id("cyclonev_jtag"))
            detach_dedicated_jtag_pins(ctx, ci);
        std::vector<IdString> unconnected;
        for (auto &port : ci->ports) {
            bool known = false;
            for (const auto &pin : atom->pins)
                known |= port.first == id(pin.name);
            if (known)
                continue;
            if (port.second.net != nullptr && port.first == id("tdoutap"))
                log_error("cyclonev_jtag '%s': connect the user data register output to tdouser; Quartus rejects "
                          "the tdoutap port (error 176304).\n",
                          ctx->nameOf(ci));
            if (port.second.net != nullptr)
                log_error("%s '%s': port %s is not supported.\n", atom->type, ctx->nameOf(ci),
                          port.first.c_str(ctx));
            unconnected.push_back(port.first);
        }
        for (IdString port : unconnected)
            ci->ports.erase(port);
        // Quartus places a crcerror pad on the dedicated CRC_ERROR pin and
        // enables that pin function; nextpnr implements the pin only through
        // ENABLE_CRC_ERROR_PIN.
        if (ci->type == id("cyclonev_crcblock"))
            if (NetInfo *error = ci->getPort(id("crcerror")))
                for (auto &user : error->users)
                    if (is_io_cell(user.cell->type) || is_top_buffer(ctx, user.cell, true))
                        log_error("cyclonev_crcblock '%s': crcerror drives a pad directly, which Quartus maps to the "
                                  "dedicated CRC_ERROR pin; register it in the fabric first or use "
                                  "ENABLE_CRC_ERROR_PIN.\n",
                                  ctx->nameOf(ci));
    }
    // Quartus takes the divider of a CRC block atom over the QSF value
    // (warning 176287).
    const CellInfo *crc = find_crc_block(ctx);
    if (crc != nullptr && control_options.count("ERROR_CHECK_FREQUENCY_DIVISOR") &&
        (1LL << crc_divide_order(ctx, crc)) != std::stoll(control_option("ERROR_CHECK_FREQUENCY_DIVISOR")))
        log_warning("ERROR_CHECK_FREQUENCY_DIVISOR %s differs from oscillator_divider of '%s'; as in Quartus the "
                    "atom's divider is used.\n",
                    control_option("ERROR_CHECK_FREQUENCY_DIVISOR").c_str(), ctx->nameOf(crc));
    if (!used.empty()) {
        std::string list;
        for (const auto &type : used)
            list += (list.empty() ? "" : ", ") + type;
        log_warning("Control block atoms (%s) have no characterized interface timing; paths to and from them are not "
                    "analysed.\n",
                    list.c_str());
    }
}

void Arch::check_control_options() const
{
    const Context *ctx = getCtx();
    // Quartus replaces a user code with its design checksum unless
    // USE_CHECKSUM_AS_USERCODE is OFF; nextpnr does not compute that checksum.
    if (control_options.count("STRATIX_JTAG_USER_CODE") && control_option("USE_CHECKSUM_AS_USERCODE") != "OFF")
        log_error("STRATIX_JTAG_USER_CODE needs USE_CHECKSUM_AS_USERCODE OFF: otherwise Quartus programs its design "
                  "checksum as the usercode, which nextpnr does not compute.\n");
    if (control_options.count("USE_CHECKSUM_AS_USERCODE") && control_option("USE_CHECKSUM_AS_USERCODE") == "ON")
        log_error("USE_CHECKSUM_AS_USERCODE ON is not supported: nextpnr does not compute the Quartus checksum.\n");

    bool configures_device = control_options.count("STRATIX_JTAG_USER_CODE") > 0;
    for (const auto &option : control_options) {
        const OptionSpec *spec = find_option_spec(option.first);
        if (!spec->values.empty() && option.second != spec->values.front())
            configures_device = true;
    }
    const std::string model = cyclonev->current_model()->name;
    if (configures_device && model.compare(0, std::string(verified_device).size(), verified_device) != 0)
        log_error("QSF device options are only verified on %s devices, not %s.\n", verified_device, model.c_str());

    for (const auto &pin : dedicated_pins()) {
        if (control_option(pin.option) != "ON")
            continue;
        const CycloneV::pin_info_t *info = cyclonev->pin_find_name(pin.pin);
        BelId bel = info ? get_io_pin_bel(info) : BelId();
        if (bel == BelId())
            log_error("%s needs package pin %s, which this device lacks.\n", pin.option, pin.pin);
        if (const CellInfo *user = getBoundBelCell(bel))
            log_error("%s reserves pin %s for %s, but the design places '%s' there.\n", pin.option, pin.pin,
                      pin.function, ctx->nameOf(user));
    }
}

void Arch::write_control_bitstream()
{
    const Context *ctx = getCtx();
    check_control_options();

    // CRC error detection: the atom's divider, else the QSF value when the
    // CRC_ERROR pin is enabled. Quartus leaves both unset otherwise.
    const CellInfo *crc = find_crc_block(ctx);
    int crc_order = crc ? crc_divide_order(ctx, crc) : -1;
    if (control_option("ENABLE_CRC_ERROR_PIN") == "ON") {
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::CRC_ERROR_DETECTION_EN, true));
        if (crc_order < 0)
            crc_order = control_options.count("ERROR_CHECK_FREQUENCY_DIVISOR")
                                ? divisor_order(std::stoll(control_option("ERROR_CHECK_FREQUENCY_DIVISOR")))
                                : 8;
    }
    if (crc_order >= 0)
        NPNR_ASSERT(cyclonev->opt_n_set(CycloneV::CRC_DIVIDE_ORDER, crc_order));

    if (control_option("USE_CHECKSUM_AS_USERCODE") == "OFF") {
        uint64_t code = 0xffffffff;
        if (control_options.count("STRATIX_JTAG_USER_CODE"))
            code = std::stoull(control_option("STRATIX_JTAG_USER_CODE"), nullptr, 16);
        NPNR_ASSERT(cyclonev->opt_r_set(CycloneV::JTAG_ID, code));
    }

    // Mistral's names for option bits 6.2 and 6.3 do not match their Quartus
    // meaning: 6.3 follows ENABLE_INIT_DONE_OUTPUT and 6.2 follows
    // RELEASE_CLEARS_BEFORE_TRI_STATES. All of these bits are active low.
    if (control_option("ENABLE_DEVICE_WIDE_RESET") == "ON")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::DEVICE_WIDE_RESET_EN, false));
    if (control_option("ENABLE_DEVICE_WIDE_OE") == "ON")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::ALLOW_DEVICE_WIDE_OUTPUT_ENABLE_DIS, false));
    if (control_option("ENABLE_INIT_DONE_OUTPUT") == "ON")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::RELEASE_CLEARS_BEFORE_TRISTATES_DIS, false));
    if (control_option("ENABLE_NCEO_OUTPUT") == "ON")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::NCEO_DIS, false));
    if (control_option("RELEASE_CLEARS_BEFORE_TRI_STATES") == "ON")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::CVP_CONF_DONE_EN, false));
    if (control_option("AUTO_RESTART_CONFIGURATION") == "OFF")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::RETRY_CONFIG_ON_ERROR_EN, false));
    if (control_option("ENABLE_OCT_DONE") == "ON")
        NPNR_ASSERT(cyclonev->opt_b_set(CycloneV::OCT_DONE_DIS, false));

    pool<BelId> reserved;
    for (const auto &pin : dedicated_pins()) {
        if (control_option(pin.option) != "ON")
            continue;
        BelId bel = get_io_pin_bel(cyclonev->pin_find_name(pin.pin));
        reserved.insert(bel);
        Loc loc = getBelLocation(bel);
        int bi = bel_data(bel).block_index;
        auto pos = CycloneV::xycoords{uint32_t(loc.x), uint32_t(loc.y)};
        NPNR_ASSERT(cyclonev->bmux_b_set(CycloneV::GPIO, pos, CycloneV::USE_WEAK_PULLUP, bi, false));
        if (!pin.output) {
            NPNR_ASSERT(cyclonev->bmux_m_set(CycloneV::GPIO, pos, CycloneV::IOCSR_STD, bi, CycloneV::NVR_LOW));
            continue;
        }
        NPNR_ASSERT(cyclonev->bmux_m_set(CycloneV::GPIO, pos, CycloneV::DRIVE_STRENGTH, bi,
                                         CycloneV::V3P3_LVTTL_16MA_LVCMOS_2MA));
        NPNR_ASSERT(cyclonev->bmux_m_set(CycloneV::GPIO, pos, CycloneV::IOCSR_STD, bi, CycloneV::DIS));
        if (pin.open_drain)
            NPNR_ASSERT(cyclonev->bmux_b_set(CycloneV::GPIO, pos, CycloneV::USE_OPEN_DRAIN, bi, true));
        auto dqs = cyclonev->p2p_to(CycloneV::pnode_coords{CycloneV::GPIO, pos, CycloneV::PNONE, int8_t(bi), -1});
        NPNR_ASSERT(dqs != CycloneV::pnode_coords{});
        NPNR_ASSERT(cyclonev->bmux_m_set(CycloneV::DQS16, dqs.p(), CycloneV::INPUT_REG4_SEL, dqs.bi(),
                                         CycloneV::SEL_LOCKED_DPA));
        NPNR_ASSERT(cyclonev->inv_set(cyclonev->rc2ri(find_rnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::OEIN, bi, 0)),
                                      true));
        NPNR_ASSERT(cyclonev->inv_set(cyclonev->rc2ri(find_rnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::OEIN, bi, 1)),
                                      false));
        if (pin.invert_data)
            NPNR_ASSERT(cyclonev->inv_set(
                    cyclonev->rc2ri(find_rnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::DATAOUT, bi, 0)), true));
    }

    // Unused pins: every bonded GPIO pad without a cell. Quartus leaves the
    // dedicated configuration and JTAG pads alone; they have no GPIO BEL here.
    const std::string unused = control_option("RESERVE_ALL_UNUSED_PINS_WEAK_PULLUP");
    if (unused != "AS INPUT TRI-STATED WITH WEAK PULL-UP") {
        bool bus_hold = unused == "AS INPUT TRI-STATED WITH BUS-HOLD";
        for (auto pos : cyclonev->gpio_get_pos())
            for (int bi = 0; bi < 4; bi++) {
                if (cyclonev->pin_find_pos(pos, bi) == nullptr)
                    continue;
                BelId bel = bel_by_block_idx(pos.x(), pos.y(), id_MISTRAL_IO, bi);
                if (bel == BelId() || getBoundBelCell(bel) != nullptr || reserved.count(bel))
                    continue;
                NPNR_ASSERT(cyclonev->bmux_b_set(CycloneV::GPIO, pos, CycloneV::USE_WEAK_PULLUP, bi, false));
                if (bus_hold)
                    NPNR_ASSERT(cyclonev->bmux_b_set(CycloneV::GPIO, pos, CycloneV::USE_BUS_HOLD, bi, true));
            }
    }
}

NEXTPNR_NAMESPACE_END
