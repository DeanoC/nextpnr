/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2024  rowanG077 <goemansrowan@gmail.com>
 *
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

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>
#include <sstream>
#include "io_delay.h"

NEXTPNR_NAMESPACE_BEGIN

struct SdcEntity
{
    enum EntityType
    {
        ENTITY_CELL,
        ENTITY_PORT,
        ENTITY_NET,
        ENTITY_PIN,
        ENTITY_CLOCK, // name is a clock name pattern
    } type;
    IdString name;
    IdString pin; // for cell pins only

    SdcEntity(EntityType type, IdString name) : type(type), name(name) {}
    SdcEntity(EntityType type, IdString name, IdString pin) : type(type), name(name), pin(pin) {}

    const std::string &to_string(Context *ctx) { return name.str(ctx); }

    CellInfo *get_cell(Context *ctx) const
    {
        if (type != ENTITY_CELL)
            return nullptr;
        return ctx->cells.at(name).get();
    }

    PortInfo *get_port(Context *ctx) const
    {
        if (type != ENTITY_PORT)
            return nullptr;
        return &ctx->ports.at(name);
    }

    NetInfo *get_net(Context *ctx) const
    {
        if (type == ENTITY_PIN) {
            CellInfo *cell = nullptr;
            if (ctx->cells.count(name)) {
                cell = ctx->cells.at(name).get();
            } else {
                return nullptr;
            }
            if (!cell->ports.count(pin))
                return nullptr;
            return cell->ports.at(pin).net;
        } else if (type == ENTITY_NET) {
            return ctx->nets.at(name).get();
        } else {
            return nullptr;
        }
    }
};

struct SdcValue
{
    SdcValue(const std::string &s) : is_string(true), str(s) {};
    SdcValue(const std::vector<SdcEntity> &l) : is_string(false), list(l) {};

    bool is_string;
    std::string str;             // simple string value
    std::vector<SdcEntity> list; // list of entities
};

struct SDCParser
{
    std::string buf;
    int pos = 0;
    int lineno = 1;
    Context *ctx;

    SDCParser(const std::string &buf, Context *ctx) : buf(buf), ctx(ctx) {};

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

    // Tcl-based SDC files use a backslash followed by a newline to continue a
    // command on the next line.  Treat the pair as whitespace when looking
    // for the next argument.  Backslashes in ordinary strings remain
    // handled by get_str below.
    inline void skip_line_continuation()
    {
        while (true) {
            skip_blank(false);
            if (eof() || peek() != '\\' || pos + 1 >= int(buf.size()) ||
                (buf.at(pos + 1) != '\n' && buf.at(pos + 1) != '\r'))
                return;
            get();
            char newline = get();
            if (newline == '\r' && !eof() && peek() == '\n')
                get();
        }
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

    inline std::string get_str()
    {
        std::string s;
        skip_line_continuation();
        if (eof())
            return "";

        bool in_quotes = false, in_braces = false, escaped = false;

        char c = get();

        if (c == '"')
            in_quotes = true;
        else if (c == '{')
            in_braces = true;
        else
            s += c;

        while (true) {
            if (eof()) {
                if (in_quotes || in_braces || escaped)
                    log_error("EOF while parsing string '%s'\n", s.c_str());
                else
                    break;
            }

            char c = peek();
            if (!in_quotes && !in_braces && !escaped &&
                (std::isblank(c) || c == '\n' || c == '\r' || c == ']')) {
                break;
            }
            get();
            if (escaped) {
                s += c;
                escaped = false;
            } else if ((in_quotes && c == '"') || (in_braces && c == '}')) {
                break;
            } else if (c == '\\') {
                escaped = true;
            } else {
                s += c;
            }
        }

        return s;
    }

    SdcValue evaluate(const std::vector<SdcValue> &arguments)
    {
        NPNR_ASSERT(!arguments.empty());
        auto &arg0 = arguments.at(0);
        NPNR_ASSERT(arg0.is_string);
        const std::string &cmd = arg0.str;
        if (cmd == "get_ports")
            return cmd_get_ports(arguments);
        else if (cmd == "get_cells")
            return cmd_get_cells(arguments);
        else if (cmd == "get_nets")
            return cmd_get_nets(arguments);
        else if (cmd == "get_pins")
            return cmd_get_pins(arguments);
        else if (cmd == "get_clocks")
            return cmd_get_clocks(arguments);
        else if (cmd == "create_clock")
            return cmd_create_clock(arguments);
        else if (cmd == "set_false_path")
            return cmd_set_false_path(arguments);
        else if (cmd == "set_multicycle_path")
            return cmd_set_multicycle_path(arguments);
        else if (cmd == "set_input_delay" || cmd == "set_output_delay")
            return cmd_set_io_delay(arguments, cmd == "set_input_delay");
        else if (cmd == "derive_pll_clocks" || cmd == "derive_clock_uncertainty")
            return cmd_ignored(arguments);
        else if (cmd == "set_clock_groups")
            return cmd_set_clock_groups(arguments);
        else
            log_error("Unsupported SDC command '%s'\n", cmd.c_str());
    }

    std::vector<SdcValue> get_arguments()
    {
        std::vector<SdcValue> args;
        while (!skip_check_eol()) {
            if (check_get('[')) {
                // Start of a sub-expression
                auto result = evaluate(get_arguments());
                NPNR_ASSERT(check_get(']'));
                args.push_back(result);
            } else if (peek() == ']') {
                break;
            } else {
                args.push_back(get_str());
            }
        }
        skip_blank(true);
        return args;
    }

    SdcValue cmd_get_nets(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> nets;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_nets expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (!s.empty() && s.at(0) == '-')
                log_error("unsupported argument '%s' to get_nets (line %d)\n", s.c_str(), lineno);
            IdString id = ctx->id(s);
            if (ctx->nets.count(id) || ctx->net_aliases.count(id))
                nets.emplace_back(SdcEntity::ENTITY_NET, ctx->net_aliases.count(id) ? ctx->net_aliases.at(id) : id);
            else
                log_warning("get_nets argument '%s' matched no objects.\n", s.c_str());
        }
        return nets;
    }

    SdcValue cmd_get_ports(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> ports;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_ports expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (!s.empty() && s.at(0) == '-')
                log_error("unsupported argument '%s' to get_ports (line %d)\n", s.c_str(), lineno);
            std::istringstream patterns(s);
            std::string pattern;
            while (patterns >> pattern) {
                IdString id = ctx->id(pattern);
                if (ctx->ports.count(id)) {
                    ports.emplace_back(SdcEntity::ENTITY_PORT, id);
                    continue;
                }
                for (const auto &port : ctx->ports)
                    if (glob_match(pattern, port.first.str(ctx)))
                        ports.emplace_back(SdcEntity::ENTITY_PORT, port.first);
            }
        }
        std::sort(ports.begin(), ports.end(), [&](const SdcEntity &a, const SdcEntity &b) {
            return a.name.str(ctx) < b.name.str(ctx);
        });
        ports.erase(std::unique(ports.begin(), ports.end(), [](const SdcEntity &a, const SdcEntity &b) {
            return a.name == b.name;
        }), ports.end());
        return ports;
    }

    // Brackets are literal bus delimiters, rather than glob character classes.
    static bool glob_match(const std::string &pattern, const std::string &name)
    {
        size_t p = 0, n = 0, star = std::string::npos, retry = 0;
        while (n < name.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) { ++p; ++n; }
            else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = n; }
            else if (star != std::string::npos) { p = star + 1; n = ++retry; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    SdcValue cmd_get_cells(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> cells;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_cells expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_cells (line %d)\n", s.c_str(), lineno);
            IdString id = ctx->id(s);
            if (ctx->cells.count(id))
                cells.emplace_back(SdcEntity::ENTITY_CELL, id);
        }
        return cells;
    }

    SdcValue cmd_get_pins(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> pins;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_pins expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_pins (line %d)\n", s.c_str(), lineno);
            auto pos = s.rfind('/');
            if (pos == std::string::npos)
                log_error("expected / in cell pin name '%s' (line %d)\n", s.c_str(), lineno);
            pins.emplace_back(SdcEntity::ENTITY_PIN, ctx->id(s.substr(0, pos)), ctx->id(s.substr(pos + 1)));
            if (pins.back().get_net(ctx) == nullptr) {
                log_warning("cell pin '%s' not found\n", s.c_str());
                pins.pop_back();
            }
        }
        return pins;
    }

    // Split a Tcl list such as {clk_a clk_b} (already stripped of braces).
    static std::vector<std::string> split_list(const std::string &s)
    {
        std::vector<std::string> items;
        std::string item;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!item.empty())
                    items.push_back(item);
                item.clear();
            } else {
                item += c;
            }
        }
        if (!item.empty())
            items.push_back(item);
        return items;
    }

    SdcValue cmd_get_clocks(const std::vector<SdcValue> &arguments)
    {
        // Clock names are patterns resolved against create_clock names and
        // clock net names when timing analysis meets the clock, because PLL
        // output clocks only exist after packing.
        std::vector<SdcEntity> clocks;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_clocks expected string arguments (line %d)\n", lineno);
            if (!arg.str.empty() && arg.str.at(0) == '-')
                log_error("unsupported argument '%s' to get_clocks (line %d)\n", arg.str.c_str(), lineno);
            for (const auto &name : split_list(arg.str))
                clocks.emplace_back(SdcEntity::ENTITY_CLOCK, ctx->id(name));
        }
        return clocks;
    }

    SdcValue cmd_set_io_delay(const std::vector<SdcValue> &arguments, bool input)
    {
        bool minimum = false, maximum = false, falling = false, have_value = false;
        double delay = 0;
        NetInfo *clock = nullptr;
        std::vector<SdcEntity> targets;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const auto &arg = arguments[i];
            if (!arg.is_string) {
                targets.insert(targets.end(), arg.list.begin(), arg.list.end());
            } else if (arg.str == "-min") minimum = true;
            else if (arg.str == "-max") maximum = true;
            else if (arg.str == "-clock_fall") falling = true;
            else if (arg.str == "-clock") {
                if (clock || ++i == arguments.size())
                    log_error("IO delay requires exactly one -clock (line %d).\n", lineno);
                const auto &value = arguments[i];
                if (value.is_string) {
                    auto named = ctx->settings.find(ctx->id("sdc/clock/" + value.str));
                    clock = io_delay_clock(ctx, named == ctx->settings.end() ? value.str : named->second.as_string());
                } else if (value.list.size() == 1 && value.list[0].type == SdcEntity::ENTITY_CLOCK) {
                    auto name = value.list[0].name.str(ctx);
                    auto named = ctx->settings.find(ctx->id("sdc/clock/" + name));
                    clock = io_delay_clock(ctx, named == ctx->settings.end() ? name : named->second.as_string());
                }
                if (!clock) log_error("IO delay clock matched no single physical clock (line %d).\n", lineno);
            } else {
                size_t consumed = 0;
                try { delay = std::stod(arg.str, &consumed); }
                catch (const std::exception &) {
                    log_error("Unsupported IO delay argument '%s' (line %d).\n", arg.str.c_str(), lineno);
                }
                if (have_value || consumed != arg.str.size())
                    log_error("IO delay requires one numeric delay and [get_ports ...] (line %d).\n", lineno);
                io_delay_value(ctx, json11::Json(delay));
                have_value = true;
            }
        }
        if (!clock || !have_value || targets.empty())
            log_error("IO delay requires -clock, a numeric delay, and matching ports (line %d).\n", lineno);
        if (!minimum && !maximum) minimum = maximum = true;
        auto rows = read_io_delays(ctx);
        for (const auto &target : targets) {
            if (target.type != SdcEntity::ENTITY_PORT)
                log_error("IO delay applies only to top-level ports (line %d).\n", lineno);
            auto *port = target.get_port(ctx);
            if ((input && port->type == PORT_OUT) || (!input && port->type == PORT_IN))
                log_error("IO delay has wrong direction for port '%s' (line %d).\n", target.name.c_str(ctx), lineno);
            json11::Json::object row{{"port", target.name.str(ctx)}, {"input", input},
                                     {"clock", clock->name.str(ctx)}, {"fall", falling}};
            size_t index = rows.size();
            for (size_t r = 0; r < rows.size(); ++r) {
                if (rows[r]["port"].string_value() != target.name.str(ctx) || rows[r]["input"].bool_value() != input)
                    continue;
                if (rows[r]["clock"].string_value() != clock->name.str(ctx) || rows[r]["fall"].bool_value() != falling)
                    log_error("Multiple clock/edge IO delays on one port require unsupported -add_delay semantics.\n");
                row = rows[r].object_items(); index = r; break;
            }
            if (minimum) row["min"] = delay;
            if (maximum) row["max"] = delay;
            if (index == rows.size()) rows.emplace_back(row);
            else rows[index] = row;
        }
        ctx->settings[ctx->id("timing/io_delays")] = json11::Json(rows).dump();
        return std::string{};
    }

    SdcValue cmd_ignored(const std::vector<SdcValue> &arguments)
    {
        // Quartus derives PLL clocks from the primitive during packing and
        // nextpnr has no clock uncertainty model, so accepting these is a
        // deliberate no-op that allows the same SDC to be shared.
        (void)arguments;
        return std::string{};
    }

    // Clock patterns of a -group/-from/-to value: a get_clocks result or a
    // plain list of clock names.
    std::vector<std::string> clock_patterns(const SdcValue &value, const char *cmd)
    {
        std::vector<std::string> patterns;
        if (value.is_string)
            return split_list(value.str);
        for (const auto &ety : value.list) {
            if (ety.type != SdcEntity::ENTITY_CLOCK)
                log_error("%s expects clocks here (line %d)\n", cmd, lineno);
            patterns.push_back(ety.name.str(ctx));
        }
        return patterns;
    }

    static bool is_clock_list(const SdcValue &value)
    {
        return !value.is_string && !value.list.empty() &&
               std::all_of(value.list.begin(), value.list.end(),
                           [](const SdcEntity &e) { return e.type == SdcEntity::ENTITY_CLOCK; });
    }

    SdcValue cmd_set_clock_groups(const std::vector<SdcValue> &arguments)
    {
        std::vector<std::vector<std::string>> groups;
        bool kind = false;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("set_clock_groups expected an option (line %d)\n", lineno);
            const std::string &s = arg.str;
            if (s == "-asynchronous" || s == "-exclusive" || s == "-logically_exclusive" ||
                s == "-physically_exclusive") {
                kind = true;
            } else if (s == "-group" || s == "-name") {
                if (++i >= int(arguments.size()))
                    log_error("missing value for %s (line %d)\n", s.c_str(), lineno);
                if (s == "-group")
                    groups.push_back(clock_patterns(arguments.at(i), "set_clock_groups -group"));
            } else {
                log_error("unsupported argument '%s' to set_clock_groups (line %d)\n", s.c_str(), lineno);
            }
        }
        if (!kind)
            log_error("set_clock_groups needs -asynchronous, -exclusive, -logically_exclusive or "
                      "-physically_exclusive (line %d)\n",
                      lineno);
        if (groups.empty())
            log_error("set_clock_groups needs at least one -group (line %d)\n", lineno);
        ctx->sdc_clock_groups.push_back(groups);
        return std::string{};
    }

    SdcValue cmd_set_multicycle_path(const std::vector<SdcValue> &arguments)
    {
        BaseCtx::SdcClockException exception;
        exception.false_path = false;
        bool setup = false, hold = false, have_value = false, have_hold_value = false;
        int value = 1, hold_value = 0;
        // A number written after -setup or -hold belongs to that option, so
        // `-setup 2 -hold 1` is one command rather than a stray argument.
        auto take_multiplier = [&](int &index, int &out) {
            if (index + 1 >= int(arguments.size()))
                return false;
            const auto &next = arguments.at(index + 1);
            if (!next.is_string || next.str.empty() || next.str.at(0) == '-')
                return false;
            try {
                out = std::stoi(next.str);
            } catch (std::exception &) {
                return false;
            }
            ++index;
            return true;
        };
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("set_multicycle_path expected an option or path multiplier (line %d)\n", lineno);
            const std::string &s = arg.str;
            if (s == "-setup") {
                setup = true;
                int parsed = 0;
                if (take_multiplier(i, parsed)) {
                    value = parsed;
                    have_value = true;
                }
            } else if (s == "-hold") {
                hold = true;
                int parsed = 0;
                if (take_multiplier(i, parsed)) {
                    hold_value = parsed;
                    have_hold_value = true;
                }
            } else if (s == "-start") {
                exception.start = true;
            } else if (s == "-end") {
                exception.start = false;
            } else if (s == "-from" || s == "-to") {
                if (++i >= int(arguments.size()))
                    log_error("missing value for %s (line %d)\n", s.c_str(), lineno);
                auto &val = arguments.at(i);
                if (!is_clock_list(val))
                    log_error("set_multicycle_path supports only clock -from/-to targets (line %d)\n", lineno);
                (s == "-from" ? exception.from : exception.to) = clock_patterns(val, "set_multicycle_path");
            } else if (!s.empty() && s.at(0) != '-' && !have_value) {
                try {
                    value = std::stoi(s);
                } catch (std::exception &) {
                    log_error("invalid path multiplier '%s' to set_multicycle_path (line %d)\n", s.c_str(), lineno);
                }
                have_value = true;
            } else {
                log_error("unsupported argument '%s' to set_multicycle_path (line %d)\n", s.c_str(), lineno);
            }
        }
        if (!have_value && have_hold_value) {
            value = hold_value;
            have_value = true;
        }
        if (!have_value || value < 0)
            log_error("set_multicycle_path needs a non-negative path multiplier (line %d)\n", lineno);
        // Hold stays on the single-cycle edge. A hold-only command records
        // nothing; -setup and -hold on one command still record the setup.
        int reported_hold = have_hold_value ? hold_value : value;
        if (hold && !setup) {
            log_warning("set_multicycle_path -hold %d: hold checks keep the single-cycle relationship "
                        "(line %d)\n",
                        reported_hold, lineno);
            return std::string{};
        }
        if (hold)
            log_warning("set_multicycle_path -hold %d is ignored; hold checks keep the single-cycle "
                        "relationship (line %d)\n",
                        reported_hold, lineno);
        if (value < 1)
            log_error("set_multicycle_path -setup needs a multiplier of at least 1 (line %d)\n", lineno);
        exception.setup_multiplier = value;
        ctx->sdc_clock_exceptions.push_back(exception);
        return std::string{};
    }

    SdcValue cmd_create_clock(const std::vector<SdcValue> &arguments)
    {
        double period = 10;
        std::string clock_name;
        std::vector<SdcEntity> targets;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const auto &arg = arguments[i];
            if (!arg.is_string) {
                targets.insert(targets.end(), arg.list.begin(), arg.list.end());
                continue;
            }
            if (arg.str != "-period" && arg.str != "-name")
                log_error("Unsupported argument '%s' to create_clock.\n", arg.str.c_str());
            if (++i == arguments.size() || !arguments[i].is_string)
                log_error("create_clock option '%s' requires a value.\n", arg.str.c_str());
            if (arg.str == "-name") {
                clock_name = arguments[i].str;
            } else {
                size_t consumed = 0;
                try { period = std::stod(arguments[i].str, &consumed); }
                catch (const std::exception &) { log_error("Invalid create_clock period.\n"); }
                io_delay_value(ctx, json11::Json(period));
                if (consumed != arguments[i].str.size() || period <= 0 || ctx->getDelayFromNS(period) <= 0)
                    log_error("create_clock period must be positive and representable.\n");
            }
        }
        if (targets.empty())
            log_error("create_clock requires a physical target; virtual clocks are not supported.\n");
        if (!clock_name.empty() && targets.size() != 1)
            log_error("A named create_clock requires exactly one target.\n");
        for (const auto &target : targets) {
            NetInfo *net = nullptr;
            if (target.type == SdcEntity::ENTITY_PORT) net = target.get_port(ctx)->net;
            else if (target.type == SdcEntity::ENTITY_NET || target.type == SdcEntity::ENTITY_PIN)
                net = target.get_net(ctx);
            if (!net) log_error("create_clock target must be a connected port, net, or pin.\n");
            if (!clock_name.empty()) {
                IdString key = ctx->id("sdc/clock/" + clock_name);
                auto old = ctx->settings.find(key);
                if (old != ctx->settings.end() && old->second.as_string() != net->name.str(ctx))
                    log_error("Clock name '%s' already names another net.\n", clock_name.c_str());
                ctx->settings[key] = net->name.str(ctx);
                ctx->sdc_clock_names[clock_name] = net->name;
            }
            ctx->addClock(net->name, 1000.0 / period);
            ctx->settings[ctx->id("sdc/period/" + net->name.str(ctx))] = json11::Json(period).dump();
        }
        return std::string{};
    }

    SdcValue cmd_set_false_path(const std::vector<SdcValue> &arguments)
    {
        // Clock-to-clock false paths ([get_clocks ...] on -from and/or -to)
        // are applied to the clock-domain pairs they name.
        bool clocks = arguments.size() > 1;
        for (int i = 1; i < int(arguments.size()); i += 2) {
            const auto &opt = arguments.at(i);
            if (!opt.is_string || (opt.str != "-from" && opt.str != "-to") || i + 1 >= int(arguments.size()) ||
                !is_clock_list(arguments.at(i + 1)))
                clocks = false;
        }
        if (clocks) {
            BaseCtx::SdcClockException exception;
            for (int i = 1; i < int(arguments.size()); i += 2)
                (arguments.at(i).str == "-from" ? exception.from : exception.to) =
                        clock_patterns(arguments.at(i + 1), "set_false_path");
            ctx->sdc_clock_exceptions.push_back(exception);
            return std::string{};
        }

        NetInfo *from = nullptr;
        NetInfo *to = nullptr;

        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (arg.is_string) {
                std::string s = arg.str;

                bool is_from = true;
                if (s == "-to") {
                    is_from = false;
                } else if (s != "-from") {
                    log_error("expecting either -to or -from to set_false_path(line %d)\n", lineno);
                }

                i++;
                auto &val = arguments.at(i);
                if (val.is_string) {
                    log_error("expecting SdcValue argument to -from (line %d)\n", lineno);
                }

                if (val.list.size() != 1) {
                    log_error("Expected a single SdcEntity as argument to -to/-from (line %d)\n", lineno);
                }

                auto &ety = val.list.at(0);

                NetInfo *net = nullptr;
                if (ety.type == SdcEntity::ENTITY_PIN)
                    net = ety.get_net(ctx);
                else if (ety.type == SdcEntity::ENTITY_NET)
                    net = ctx->nets.at(ety.name).get();
                else if (ety.type == SdcEntity::ENTITY_PORT)
                    net = ctx->ports.at(ety.name).net;
                else
                    log_error("set_false_path applies only to nets, cell pins, or IO ports (line %d)\n", lineno);

                if (is_from) {
                    from = net;
                } else {
                    to = net;
                }
            }
        }

        if (from == nullptr) {
            log_error("-from is required for set_false_path (line %d)\n", lineno);
        } else if (to == nullptr) {
            log_error("-to is required for set_false_path (line %d)\n", lineno);
        }

        log_warning("set_false_path from: %s, to: %s does not do anything(yet).\n", from->name.c_str(ctx),
                    to->name.c_str(ctx));

        return std::string{};
    }

    void operator()()
    {
        while (!eof()) {
            skip_blank(true);
            auto args = get_arguments();
            if (args.empty())
                continue;
            evaluate(args);
        }
    }
};

namespace {
// '*' matches any run of characters and '?' one character.
bool sdc_glob(const char *pattern, const char *name)
{
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*')
                ++pattern;
            if (!*pattern)
                return true;
            for (; *name; ++name)
                if (sdc_glob(pattern, name))
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

bool BaseCtx::sdc_clock_match(const std::string &pattern, IdString clock_net) const
{
    if (sdc_glob(pattern.c_str(), clock_net.c_str(this)))
        return true;
    for (const auto &named : sdc_clock_names)
        if (named.second == clock_net && sdc_glob(pattern.c_str(), named.first.c_str()))
            return true;
    return false;
}

bool BaseCtx::sdc_clock_false(IdString launch, IdString capture) const
{
    if (launch == IdString() || capture == IdString())
        return false;
    auto any = [&](const std::vector<std::string> &patterns, IdString clock) {
        if (patterns.empty())
            return true;
        for (const auto &pattern : patterns)
            if (sdc_clock_match(pattern, clock))
                return true;
        return false;
    };
    for (const auto &exception : sdc_clock_exceptions)
        if (exception.false_path && any(exception.from, launch) && any(exception.to, capture))
            return true;
    for (const auto &groups : sdc_clock_groups) {
        auto group_of = [&](IdString clock) {
            for (int i = 0; i < int(groups.size()); i++)
                for (const auto &pattern : groups.at(i))
                    if (sdc_clock_match(pattern, clock))
                        return i;
            return -1;
        };
        int a = group_of(launch), b = group_of(capture);
        // A single group is exclusive with every other clock.
        if (groups.size() == 1 ? (a >= 0) != (b >= 0) : (a >= 0 && b >= 0 && a != b))
            return true;
    }
    return false;
}

const BaseCtx::SdcClockException *BaseCtx::sdc_clock_multicycle(IdString launch, IdString capture) const
{
    if (launch == IdString() || capture == IdString())
        return nullptr;
    const SdcClockException *found = nullptr;
    for (const auto &exception : sdc_clock_exceptions) {
        if (exception.false_path)
            continue;
        auto any = [&](const std::vector<std::string> &patterns, IdString clock) {
            if (patterns.empty())
                return true;
            for (const auto &pattern : patterns)
                if (sdc_clock_match(pattern, clock))
                    return true;
            return false;
        };
        if (any(exception.from, launch) && any(exception.to, capture))
            found = &exception; // the last matching command wins
    }
    return found;
}

void Context::read_sdc(std::istream &in)
{
    std::string buf(std::istreambuf_iterator<char>(in), {});
    SDCParser(buf, getCtx())();
}

NEXTPNR_NAMESPACE_END
