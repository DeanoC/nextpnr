/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021 gatecat <gatecat@ds0.me>
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
 *
 */

#include "log.h"
#include "nextpnr.h"
#include "util.h"

#include <algorithm>
#include <iterator>
#include <regex>

NEXTPNR_NAMESPACE_BEGIN

namespace {

struct QsfOption
{
    std::string name; // name, excluding the initial '-'
    int arg_count;    // number of arguments that follow the option
    bool required;    // error out if this option isn't passed
};

typedef dict<std::string, std::vector<std::string>> option_map_t;

struct QsfCommand
{
    std::string name;               // name of the command
    std::vector<QsfOption> options; // list of "-options"
    int pos_arg_count;              // number of positional arguments expected to follow the command, -1 for any
    std::function<void(Context *ctx, const option_map_t &options, const std::vector<std::string> &pos_args)> func;
};

bool is_wildcard_target(const std::string &target) { return target.find_first_of("*?") != std::string::npos; }

void set_location_assignment_cmd(Context *ctx, const option_map_t &options, const std::vector<std::string> &pos_args)
{
    const std::string &target = options.at("to").at(0);
    if (is_wildcard_target(target))
        log_error("set_location_assignment target '%s' must name one pin, not a wildcard.\n", target.c_str());
    ctx->io_attr[ctx->id(target)][id_LOC] = pos_args.at(0);
}

void set_instance_assignment_cmd(Context *ctx, const option_map_t &options, const std::vector<std::string> &pos_args)
{
    const std::string &to = options.at("to").at(0);
    if (options.at("name").at(0) == "PLACE_REGION") {
        ctx->note_place_region(to, pos_args.at(0));
        return;
    }
    IdString assignment = ctx->id(options.at("name").at(0));
    if (is_wildcard_target(to)) {
        // Quartus applies '*'/'?' targets such as SDRAM_* or HDMI_TX_D[*] to
        // every matching node. Keep them in order for the top-level ports.
        if (options.at("name").at(0) == "HPS_LOCATION")
            log_error("HPS_LOCATION target '%s' must name one cell, not a wildcard.\n", to.c_str());
        ctx->io_attr_patterns.push_back({to, assignment, Property(pos_args.at(0))});
        return;
    }
    IdString target = ctx->id(to);
    ctx->io_attr[target][assignment] = pos_args.at(0);

    // Quartus places HPS hard blocks with an instance assignment instead of
    // the BEL attribute emitted by the RTL primitive.  Keep the existing
    // io_attr path for package-pin assignments, but translate this internal
    // assignment to the normal nextpnr BEL constraint when its target cell is
    // present in the loaded JSON design.
    if (options.at("name").at(0) != "HPS_LOCATION")
        return;

    auto cell = ctx->cells.find(target);
    if (cell == ctx->cells.end()) {
        log_error("HPS_LOCATION target '%s' is not an internal cell.\n", options.at("to").at(0).c_str());
        return;
    }
    const IdString hps_i2c = ctx->id("cyclonev_hps_interface_peripheral_i2c");
    if (cell->second->type != hps_i2c) {
        log_error("HPS_LOCATION target '%s' is not a peripheral I2C cell.\n", options.at("to").at(0).c_str());
        return;
    }

    static const std::regex location_pattern(
            R"(^HPSINTERFACEPERIPHERALI2C_X([0-9]+)_Y([0-9]+)_N[0-9]+$)");
    std::smatch match;
    const std::string &location = pos_args.at(0);
    if (!std::regex_match(location, match, location_pattern)) {
        log_error("Unsupported HPS_LOCATION '%s' for '%s'.\n", location.c_str(), options.at("to").at(0).c_str());
        return;
    }

    const std::string bel = hps_i2c.str(ctx) + "." + match[1].str() + "." + match[2].str() + ".0";
    BelId bel_id = ctx->getBelByNameStr(bel);
    if (bel_id == BelId() || ctx->getBelType(bel_id) != hps_i2c) {
        log_error("HPS_LOCATION '%s' resolves to unsupported BEL '%s'.\n", location.c_str(), bel.c_str());
        return;
    }
    cell->second->attrs[ctx->id("BEL")] = bel;
}

void set_global_assignment_cmd(Context *ctx, const option_map_t &options, const std::vector<std::string> &pos_args)
{
    const std::string &name = options.at("name").at(0);
    const std::string &value = pos_args.at(0);
    // Device-wide options nextpnr implements; any other global assignment is
    // a Quartus project setting with no meaning here and is ignored.
    if (ctx->apply_control_option(name, value))
        return;
    if (name == "FES_RESERVED_BEL") {
        ctx->note_reserved_bel(value);
        return;
    }
    if (name == "FES_RESERVED_RECT") {
        ctx->note_reserved_rect(value);
        return;
    }
    if (name == "FES_RESERVED_RECT_GROUP") {
        ctx->note_reserved_rect_group(value);
        return;
    }
}

static const std::vector<QsfCommand> commands = {
        {"set_location_assignment", {{"to", 1, true}}, 1, set_location_assignment_cmd},
        {"set_instance_assignment",
         {{"to", 1, true}, {"name", 1, true}, {"section_id", 1, false}, {"entity", 1, false}},
         1,
         set_instance_assignment_cmd},
        {"set_global_assignment",
         {{"name", 1, true}, {"section_id", 1, false}, {"rise", 0, false}, {"fall", 0, false}},
         1,
         set_global_assignment_cmd},
};

struct QsfParser
{
    std::string buf;
    int pos = 0;
    int lineno = 0;
    Context *ctx;

    QsfParser(const std::string &buf, Context *ctx) : buf(buf), ctx(ctx) {};

    inline bool eof() const { return pos == int(buf.size()); }

    inline char peek() const { return buf.at(pos); }

    inline char get()
    {
        char c = buf.at(pos++);
        if (c == '\n')
            ++lineno;
        return c;
    }

    std::string get(int n)
    {
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }

    // If next char matches c, take it from the stream and return true
    bool check_get(char c)
    {
        if (peek() == c) {
            get();
            return true;
        } else {
            return false;
        }
    }

    // If next char matches any in chars, take it from the stream and return true
    bool check_get_any(const std::string &chrs)
    {
        char c = peek();
        if (chrs.find(c) != std::string::npos) {
            get();
            return true;
        } else {
            return false;
        }
    }

    inline void skip_blank(bool nl = false)
    {
        while (!eof() && check_get_any(nl ? " \t\n\r" : " \t"))
            ;
    }

    // Return true if end of line (or file)
    inline bool skip_check_eol()
    {
        skip_blank(false);
        if (eof())
            return true;
        char c = peek();
        // Comments count as end of line
        if (c == '#') {
            get();
            while (!eof() && peek() != '\n' && peek() != '\r')
                get();
            return true;
        }
        if (c == ';') {
            // Forced end of line
            get();
            return true;
        }
        return (c == '\n' || c == '\r');
    }

    // We need to distinguish between quoted and unquoted strings, the former don't count as options
    struct StringVal
    {
        std::string str;
        bool is_quoted = false;
    };

    inline StringVal get_str()
    {
        StringVal s;
        skip_blank(false);
        if (eof())
            return {"", false};

        bool in_quotes = false, in_braces = false, escaped = false;

        char c = get();

        if (c == '"') {
            in_quotes = true;
            s.is_quoted = true;
        } else if (c == '{') {
            in_braces = true;
            s.is_quoted = true;
        } else {
            s.str += c;
        }

        while (!eof()) {
            char c = peek();
            if (!in_quotes && !in_braces && !escaped && (std::isblank(c) || c == '\n' || c == '\r')) {
                break;
            }
            get();
            if (escaped) {
                s.str += c;
                escaped = false;
            } else if ((in_quotes && c == '"') || (in_braces && c == '}')) {
                break;
            } else if (c == '\\') {
                escaped = true;
            } else {
                s.str += c;
            }
        }

        return s;
    }

    std::vector<StringVal> get_arguments()
    {
        std::vector<StringVal> args;
        while (!skip_check_eol()) {
            args.push_back(get_str());
        }
        skip_blank(true);
        return args;
    }

    void evaluate(const std::vector<StringVal> &args)
    {
        if (args.empty())
            return;
        auto cmd_name = args.at(0).str;
        auto fnd_cmd =
                std::find_if(commands.begin(), commands.end(), [&](const QsfCommand &c) { return c.name == cmd_name; });
        if (fnd_cmd == commands.end()) {
            log_warning("Ignoring unknown command '%s' (line %d)\n", cmd_name.c_str(), lineno);
            return;
        }
        option_map_t opt;
        std::vector<std::string> pos_args;
        for (size_t i = 1; i < args.size(); i++) {
            auto arg = args.at(i);
            if (arg.str.at(0) == '-' && !arg.is_quoted) {
                for (auto &opt_data : fnd_cmd->options) {
                    if (arg.str.compare(1, std::string::npos, opt_data.name) != 0)
                        continue;
                    opt[opt_data.name]; // create empty entry, even if 0 arguments
                    for (int j = 0; j < opt_data.arg_count; j++) {
                        ++i;
                        if (i >= args.size())
                            log_error("Unexpected end of argument list to option '%s' (line %d)\n", arg.str.c_str(),
                                      lineno);
                        opt[opt_data.name].push_back(args.at(i).str);
                    }
                    goto done;
                }
                log_error("Unknown option '%s' to command '%s' (line %d)\n", arg.str.c_str(), cmd_name.c_str(), lineno);
            done:;
            } else {
                // positional argument
                pos_args.push_back(arg.str);
            }
        }
        // Check positional argument count
        if (int(pos_args.size()) != fnd_cmd->pos_arg_count && fnd_cmd->pos_arg_count != -1) {
            log_error("Expected %d positional arguments to command '%s', got %d (line %d)\n", fnd_cmd->pos_arg_count,
                      cmd_name.c_str(), int(pos_args.size()), lineno);
        }
        // Check required options
        for (auto &opt_data : fnd_cmd->options) {
            if (opt_data.required && !opt.count(opt_data.name))
                log_error("Missing required option '%s' to command '%s' (line %d)\n", opt_data.name.c_str(),
                          cmd_name.c_str(), lineno);
        }
        // Execute
        fnd_cmd->func(ctx, opt, pos_args);
    }

    void operator()()
    {
        while (!eof()) {
            skip_blank(true);
            auto args = get_arguments();
            evaluate(args);
        }
    }
};

}; // namespace

void Arch::read_qsf(std::istream &in)
{
    std::string buf(std::istreambuf_iterator<char>(in), {});
    QsfParser(buf, getCtx())();
}

namespace {
// Quartus node-name matching for assignment targets: '*' matches any run of
// characters and '?' one character; everything else, including the brackets
// of bus indices, is literal.
bool quartus_name_match(const char *pattern, const char *name)
{
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*')
                ++pattern;
            if (!*pattern)
                return true;
            for (; *name; ++name)
                if (quartus_name_match(pattern, name))
                    return true;
            return false;
        }
        if (!*name || (*pattern != '?' && *pattern != *name))
            return false;
        ++pattern;
        ++name;
    }
    return !*name;
}
} // namespace

void Arch::apply_io_attrs(IdString port, CellInfo *cell)
{
    // Least to most specific: wildcard targets in file order, then an
    // assignment to the whole bus (SDRAM_DQ for SDRAM_DQ[3]), then the exact
    // port name. Locations only come from exact targets.
    const std::string name = port.str(this);
    for (const auto &pattern : io_attr_patterns)
        if (quartus_name_match(pattern.target.c_str(), name.c_str()))
            cell->attrs[pattern.name] = pattern.value;
    auto bracket = name.find('[');
    if (bracket != std::string::npos && bracket > 0 && name.back() == ']') {
        auto bus = io_attr.find(id(name.substr(0, bracket)));
        if (bus != io_attr.end())
            for (const auto &kv : bus->second)
                if (kv.first != id_LOC)
                    cell->attrs[kv.first] = kv.second;
    }
    auto exact = io_attr.find(port);
    if (exact != io_attr.end())
        for (const auto &kv : exact->second)
            cell->attrs[kv.first] = kv.second;
}

void Arch::note_place_region(const std::string &target, const std::string &value)
{
    static const std::regex rect_pattern(R"(^\s*X([0-9]+)\s+Y([0-9]+)\s+X([0-9]+)\s+Y([0-9]+)\s*$)");
    std::smatch match;
    if (!std::regex_match(value, match, rect_pattern))
        log_error("PLACE_REGION '%s' for '%s' must be one rectangle 'X<x0> Y<y0> X<x1> Y<y1>'.\n", value.c_str(),
                  target.c_str());
    PlaceRegionAssignment assignment{target, std::stoi(match[1]), std::stoi(match[2]), std::stoi(match[3]),
                                     std::stoi(match[4])};
    if (assignment.x0 > assignment.x1 || assignment.y0 > assignment.y1 || assignment.x1 >= getGridDimX() ||
        assignment.y1 >= getGridDimY())
        log_error("PLACE_REGION '%s' for '%s' is empty or outside the %dx%d device grid.\n", value.c_str(),
                  target.c_str(), getGridDimX(), getGridDimY());
    place_region_assignments.push_back(assignment);
}

void Arch::apply_place_regions()
{
    // Quartus-style precedence: an assignment naming the cell itself beats
    // any hierarchy or wildcard target, a longer target beats a shorter one,
    // and a later assignment beats an earlier one of the same length. A
    // plain target names the cell itself or every cell below it in the
    // hierarchy; '*' and '?' match against the full cell name. Only core
    // logic is constrained: I/O, clock buffers, PLLs and hard blocks named
    // under a fenced hierarchy keep their own placement.
    if (place_region_assignments.empty())
        return;
    auto core_logic = [&](const CellInfo *ci) {
        return is_comb_cell(ci->type) ||
               ci->type.in(id_MISTRAL_FF, id_MISTRAL_MLAB, id_MISTRAL_M10K, id_MISTRAL_M10K_TDP, id_MISTRAL_MUL9X9,
                           id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27, id_MISTRAL_MUL18X19,
                           id_MISTRAL_MUL18X19_COMBINED);
    };
    std::vector<int> matched(place_region_assignments.size(), 0);
    dict<IdString, pool<IdString>> region_types;
    for (auto &cell : cells) {
        CellInfo *ci = cell.second.get();
        if (!core_logic(ci))
            continue;
        const std::string name = ci->name.str(this);
        int best = -1;
        size_t best_score = 0;
        for (int i = 0; i < int(place_region_assignments.size()); i++) {
            const std::string &target = place_region_assignments.at(i).target;
            size_t score;
            if (name == target)
                score = std::string::npos;
            else if (is_wildcard_target(target) ? quartus_name_match(target.c_str(), name.c_str())
                                                : name.size() > target.size() &&
                                                          name.compare(0, target.size(), target) == 0 &&
                                                          name[target.size()] == '.')
                score = target.size();
            else
                continue;
            if (best < 0 || score >= best_score) {
                best = i;
                best_score = score;
            }
        }
        if (best < 0)
            continue;
        const auto &assignment = place_region_assignments.at(best);
        if (ci->region != nullptr)
            log_error("PLACE_REGION target '%s' matches cell '%s', which is already constrained to region '%s'.\n",
                      assignment.target.c_str(), name.c_str(), ci->region->name.c_str(this));
        IdString region_name = id(stringf("PLACE_REGION X%d Y%d X%d Y%d", assignment.x0, assignment.y0,
                                          assignment.x1, assignment.y1));
        if (!region.count(region_name)) {
            createRectangularRegion(region_name, assignment.x0, assignment.y0, assignment.x1, assignment.y1);
            place_region_names.insert(region_name);
        }
        ci->region = region.at(region_name).get();
        matched.at(best)++;
        if (region_types[region_name].insert(ci->type).second) {
            const auto &bels = ci->region->bels;
            if (!std::any_of(bels.begin(), bels.end(),
                             [&](BelId bel) { return isValidBelForCellType(ci->type, bel); }))
                log_error("PLACE_REGION X%d Y%d X%d Y%d -to %s has no %s location for cell '%s'.\n", assignment.x0,
                          assignment.y0, assignment.x1, assignment.y1, assignment.target.c_str(),
                          ci->type.c_str(this), name.c_str());
        }
    }
    for (int i = 0; i < int(place_region_assignments.size()); i++) {
        const auto &assignment = place_region_assignments.at(i);
        if (matched.at(i) == 0)
            log_error("PLACE_REGION target '%s' matches no core logic cell.\n", assignment.target.c_str());
        log_info("PLACE_REGION X%d Y%d X%d Y%d -to %s: %d cells\n", assignment.x0, assignment.y0, assignment.x1,
                 assignment.y1, assignment.target.c_str(), matched.at(i));
    }
}

void Arch::check_place_regions() const
{
    // Later placement passes are trusted to respect regions only as far as
    // this check: every constrained cell must still sit inside its rectangle.
    for (auto &cell : cells) {
        const CellInfo *ci = cell.second.get();
        if (ci->region == nullptr || !place_region_names.count(ci->region->name))
            continue;
        if (ci->bel == BelId() || !ci->region->bels.count(ci->bel))
            log_error("Cell '%s' was placed at %s, outside its %s.\n", ci->name.c_str(this),
                      ci->bel == BelId() ? "no BEL" : getCtx()->nameOfBel(ci->bel), ci->region->name.c_str(this));
    }
}

NEXTPNR_NAMESPACE_END
