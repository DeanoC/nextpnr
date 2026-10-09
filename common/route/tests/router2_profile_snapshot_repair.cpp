#include "json11.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef ROUTER2_PROFILE_WIN32_SHIM
#include "router2_profile_win32_shim.h"
#endif

#include "router2_profile.h"

namespace {

void write_text(const std::string &path, const std::string &text)
{
    std::ofstream out(path, std::ios::trunc);
    out << text;
    assert(out);
}

std::string read_text(const std::string &path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void snapshot_cycle(const std::string &path, bool wait_heartbeat)
{
    std::vector<std::pair<std::string, int>> nets{{"net0", 1}};
    router2_diagnostics::Profile profile(path, nets);
    {
        router2_diagnostics::Profile::Visit visit(&profile, 0);
        if (wait_heartbeat)
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
    profile.finish(true);
    std::string err;
    auto value = json11::Json::parse(read_text(path), err);
    assert(err.empty());
    assert(value["completed"].bool_value());
    assert(value["calls_started"].number_value() == 1);
}

} // namespace

int main(int argc, char **argv)
{
    assert(argc >= 3);
    const std::string dest = argv[1];
    const std::string mode = argv[2];
    if (mode == "replace") {
        write_text(dest, "stale-destination\n");
        snapshot_cycle(dest, argc > 3 && std::string(argv[3]) == "heartbeat");
        assert(read_text(dest).find("router2-profile-v1") != std::string::npos);
#ifdef ROUTER2_PROFILE_WIN32_SHIM
        assert(router2_win32_shim().replace_existing >= 1);
        assert(router2_win32_shim().create_new >= 1);
#endif
    } else if (mode == "utf8") {
        write_text(dest, "stale-destination\n");
        snapshot_cycle(dest, false);
        assert(read_text(dest).find("router2-profile-v1") != std::string::npos);
#ifdef ROUTER2_PROFILE_WIN32_SHIM
        auto &shim = router2_win32_shim();
        assert(shim.create_w >= 1 && shim.replace_w >= 1);
        assert(shim.create_a == 0 && shim.replace_a == 0 && shim.delete_a == 0);
        for (const auto &created : shim.created)
            assert(created.find('\xC3') != std::string::npos);
        for (const auto &pair : shim.replaced)
            assert(pair.second == dest);
#endif
    } else if (mode == "neighbor") {
        const std::string neighbor = dest + ".tmp";
        const std::string secret = "neighbor-secret-keep\n";
        write_text(neighbor, secret);
        snapshot_cycle(dest, false);
        assert(read_text(neighbor) == secret);
    } else if (mode == "symlink") {
        const std::string victim = std::string(argv[3]);
        const std::string neighbor = dest + ".tmp";
        const std::string secret = "symlink-victim-keep\n";
        write_text(victim, secret);
        assert(::symlink(victim.c_str(), neighbor.c_str()) == 0);
        snapshot_cycle(dest, false);
        assert(read_text(victim) == secret);
        char buf[4096];
        ssize_t n = ::readlink(neighbor.c_str(), buf, sizeof(buf) - 1);
        assert(n > 0);
        buf[n] = 0;
        assert(std::string(buf) == victim);
    } else if (mode == "concurrent") {
        const std::string dest_b = argv[3];
        const std::string neighbor_a = dest + ".tmp";
        const std::string neighbor_b = dest_b + ".tmp";
        write_text(neighbor_a, "keep-a\n");
        write_text(neighbor_b, "keep-b\n");
        std::thread first([&] { snapshot_cycle(dest, false); });
        std::thread second([&] { snapshot_cycle(dest_b, false); });
        first.join();
        second.join();
        assert(read_text(neighbor_a) == "keep-a\n");
        assert(read_text(neighbor_b) == "keep-b\n");
    } else {
        return 2;
    }
#ifdef ROUTER2_PROFILE_WIN32_SHIM
    auto &shim = router2_win32_shim();
    assert(shim.create_new >= 1);
    assert(shim.replace_existing >= 1);
    for (const auto &deleted : shim.deleted)
        assert(deleted != dest && deleted.find(".tmp") != std::string::npos);
    std::cout << "win32-shim create_new=" << shim.create_new << " replace=" << shim.replace_existing
              << " (not Windows-native)" << std::endl;
#endif
    return 0;
}
