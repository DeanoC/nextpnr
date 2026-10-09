#include "router2_profile.h"
#include <iostream>
int main(int argc, char **argv) {
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
