/* Joined analogue workers; propagate failures before publishing results. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_JOBS_H
#define MISTRAL_ANALOGUE_JOBS_H

#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace nextpnr_mistral_workers {
struct StartThread
{
    template <typename Work> std::thread operator()(Work &&work) const
    {
        return std::thread(std::forward<Work>(work));
    }
};

// Keep the production 64-job scheduling and finish every assigned job. Each
// failure slot has one writer; join before inspecting them. If several jobs
// fail, choose the earliest job, independently of thread completion order.
template <typename Job, typename Start = StartThread>
void run(size_t count, int workers, Job &&job, Start start = Start())
{
    if (workers < 1)
        throw std::invalid_argument("Analogue jobs require at least one worker");
    std::vector<std::exception_ptr> failures(count);
    std::atomic<size_t> next(0);
    std::vector<std::thread> threads;
    threads.reserve(workers);
    struct Join
    {
        std::vector<std::thread> &threads;
        ~Join()
        {
            for (auto &thread : threads)
                if (thread.joinable())
                    thread.join();
        }
    } join{threads};
    for (int t = 0; t < workers; ++t)
        threads.emplace_back(start([&]() {
            for (size_t i = next.fetch_add(64); i < count; i = next.fetch_add(64))
                for (size_t j = i; j < std::min(i + 64, count); ++j) {
                    try {
                        job(j);
                    } catch (...) {
                        failures[j] = std::current_exception();
                    }
                }
        }));
    for (auto &thread : threads)
        thread.join();
    for (const auto &failure : failures)
        if (failure)
            std::rethrow_exception(failure);
}
} // namespace nextpnr_mistral_workers
#endif
