/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
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

#include <cerrno>
#include <fstream>
#include "command.h"
#include "design_utils.h"
#include "jsonwrite.h"
#include "log.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

class MistralCommandHandler : public CommandHandler
{
  public:
    MistralCommandHandler(int argc, char **argv);
    virtual ~MistralCommandHandler() {};
    std::unique_ptr<Context> createContext(dict<std::string, Property> &values) override;
    void setupArchContext(Context *ctx) override {};
    void customBitstream(Context *ctx) override;
    void customAfterLoad(Context *ctx) override;

  protected:
    po::options_description getArchOptions() override;
};

MistralCommandHandler::MistralCommandHandler(int argc, char **argv) : CommandHandler(argc, argv) {}

po::options_description MistralCommandHandler::getArchOptions()
{
    po::options_description specific("Architecture specific options");
    specific.add_options()("device", po::value<std::string>(), "device name (e.g. 5CSEBA6U23I7)");
    specific.add_options()("qsf", po::value<std::string>(), "path to QSF constraints file");
    specific.add_options()("rbf", po::value<std::string>(), "RBF bitstream to write");
    specific.add_options()("compress-rbf", "generate compressed bitstream");
    specific.add_options()("fes-scaffold", "lock loaded shell BEL+routing to STRENGTH_USER");
    specific.add_options()("fes-cart", po::value<std::string>(), "merge unbound cart JSON into the reserved socket");
    specific.add_options()("fes-cart-region", po::value<std::string>(),
                            "name of the FES_RESERVED_RECT region this --fes-cart targets (default: cart)");
    specific.add_options()("fes-slot-clock", po::value<std::string>(), "exact shell clock net for FES cart cells");
    specific.add_options()("fes-cram-region", po::value<std::string>(), "half-open CRAM x0,y0,x1,y1 region for new scaffold routing");

    return specific;
}

void MistralCommandHandler::customBitstream(Context *ctx)
{
    if (vm.count("rbf")) {
        std::string filename = vm["rbf"].as<std::string>();
        for (const auto &item : ctx->nets)
            for (const auto &wire : item.second->wires)
                if (wire.second.pip != PipId() && !ctx->fes_pip_preserves_cram(wire.second.pip))
                    log_error("Routed pip %s violates frozen CRAM region.\n",
                              ctx->getPipName(wire.second.pip).str(ctx).c_str());
        ctx->build_bitstream();
        std::vector<uint8_t> data;
        ctx->cyclonev->rbf_save(data);

        std::ofstream out(filename, std::ios::binary);
        if (!out.is_open()) {
            log_error("Failed to open RBF file '%s' for writing: %s.\n", filename.c_str(),
                      std::error_code(errno, std::generic_category()).message().c_str());
        }
        out.write(reinterpret_cast<const char *>(data.data()), data.size());
    }
}

std::unique_ptr<Context> MistralCommandHandler::createContext(dict<std::string, Property> &values)
{
    ArchArgs chipArgs;
    if (!vm.count("device")) {
        log_error("device must be specified on the command line (e.g. --device 5CSEBA6U23I7)\n");
    }
    chipArgs.device = vm["device"].as<std::string>();
    auto ctx = std::unique_ptr<Context>(new Context(chipArgs));
    if (vm.count("compress-rbf"))
        ctx->settings[id_compress_rbf] = Property::State::S1;
    return ctx;
}

void MistralCommandHandler::customAfterLoad(Context *ctx)
{
    const bool routed = ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "route";
    if (vm.count("fes-cram-region")) {
        if (!routed)
            log_error("FES CRAM region requires an already routed scaffold.\n");
        ctx->note_fes_cram_region(vm["fes-cram-region"].as<std::string>());
    }
    if (vm.count("fes-slot-clock"))
        ctx->settings[ctx->id("fes/slot_clock")] = vm["fes-slot-clock"].as<std::string>();
    if (vm.count("router"))
        ctx->settings[ctx->id("router")] = vm["router"].as<std::string>();
    if (vm.count("qsf")) {
        std::string filename = vm["qsf"].as<std::string>();
        auto in = open_ifstream_and_log_error(filename, "input QSF file");
        ctx->read_qsf(in);
    }
    if (vm.count("fes-cart")) {
        std::string region = vm.count("fes-cart-region") ? vm["fes-cart-region"].as<std::string>() : "cart";
        log_info("FES merging cart JSON...\n");
        ctx->merge_fes_cart(vm["fes-cart"].as<std::string>(), region);
        log_info("FES packing unbound cart cells...\n");
        ctx->pack_unbound_cells();
        log_info("FES unbound pack complete.\n");
    } else if (vm.count("fes-cart-region")) {
        log_error("--fes-cart-region requires --fes-cart.\n");
    }
    if (vm.count("fes-scaffold") || routed)
        ctx->lock_fes_scaffold();
    // After the scaffold is locked, frozen LABs are known; constrain cart
    // cells to the reserved rectangle and report its capacity.
    ctx->fes_constrain_slot_region();
}

int main(int argc, char *argv[])
{
    MistralCommandHandler handler(argc, argv);
    return handler.exec();
}
