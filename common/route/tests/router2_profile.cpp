#include "router2_profile.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Block the destination after the initial snapshot, then show that finish(true)
// can be retried and that a later finish(false) / the destructor leave the
// completed snapshot untouched.
int retry_final_snapshot(const std::string &path)
{
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0) {
        std::cerr << "retry mode needs a path inside a directory" << std::endl;
        return 1;
    }
    const std::string parent = path.substr(0, slash);
    const std::string saved = parent + ".saved";
    std::string completed;
    {
        std::vector<std::pair<std::string, int>> nets{{"net0", 1}};
        router2_diagnostics::Profile profile(path, nets);
        if (std::rename(parent.c_str(), saved.c_str()) != 0) {
            std::cerr << "cannot move profile directory aside" << std::endl;
            return 1;
        }
        {
            std::ofstream blocker(parent, std::ios::trunc);
            blocker << "not-a-directory\n";
            if (!blocker) {
                std::rename(saved.c_str(), parent.c_str());
                std::cerr << "cannot block profile directory" << std::endl;
                return 1;
            }
        }
        bool threw = false;
        try {
            profile.finish(true);
        } catch (const std::exception &error) {
            threw = std::string(error.what()).find("Cannot write router2 profile") != std::string::npos;
            if (!threw)
                std::cerr << error.what() << std::endl;
        }
        ::remove(parent.c_str());
        if (std::rename(saved.c_str(), parent.c_str()) != 0) {
            std::cerr << "cannot restore profile directory" << std::endl;
            return 1;
        }
        if (!threw) {
            std::cerr << "finish(true) did not fail while the destination was blocked" << std::endl;
            return 1;
        }
        if (read_file(path).find("\"completed\": false") == std::string::npos) {
            std::cerr << "failed finish replaced the incomplete snapshot" << std::endl;
            return 1;
        }
        profile.finish(true);
        completed = read_file(path);
        if (completed.find("\"completed\": true") == std::string::npos) {
            std::cerr << "retried finish did not write a completed snapshot" << std::endl;
            return 1;
        }
        profile.finish(false);
        if (read_file(path) != completed) {
            std::cerr << "finish(false) replaced a successful final snapshot" << std::endl;
            return 1;
        }
    }
    if (read_file(path) != completed) {
        std::cerr << "destructor replaced a successful final snapshot" << std::endl;
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 2 && std::string(argv[2]) == "retry")
        return retry_final_snapshot(argv[1]);
    std::vector<std::pair<std::string,int>> nets;
    for (int i=0;i<30;++i) nets.emplace_back("net\""+std::to_string(i), i);
    nets.emplace_back(std::string(1023,'x')+"\xe2\x82\xac-tail", 1);
    try {
        router2_diagnostics::Profile profile(argv[1],nets);
        if (argc>2) {
            router2_diagnostics::Profile::Visit active(&profile,30);
            std::cout << "active" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(60));
        } else {
            std::vector<std::thread> workers;
            for(size_t i=0;i<nets.size();++i) workers.emplace_back([&,i] {
                router2_diagnostics::Profile::Visit visit(&profile,i);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            });
            for(auto &worker:workers) worker.join();
        }
        profile.finish(true);
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
