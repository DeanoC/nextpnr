#include "analogue_jobs.h"
#include "gtest/gtest.h"
#include <stdexcept>

TEST(MistralAnalogueJobs, SuccessfulChunksExecuteOnceAndJoin)
{
    std::vector<int> visited(193);
    nextpnr_mistral_workers::run(visited.size(), 4, [&](size_t index) { ++visited[index]; });
    for (auto visits : visited) EXPECT_EQ(visits, 1);
}

TEST(MistralAnalogueJobs, EmptyBatchCallsNoJobs)
{
    nextpnr_mistral_workers::run(0, 2, [](size_t) { ADD_FAILURE(); });
}

TEST(MistralAnalogueJobs, InvalidWorkerCountIsRefused)
{
    EXPECT_THROW(nextpnr_mistral_workers::run(1, 0, [](size_t) { ADD_FAILURE(); }), std::invalid_argument);
}

TEST(MistralAnalogueJobs, MultipleWorkerFailuresRethrowEarliestJobAfterAllWork)
{
    for (int workers : {1, 2, 4}) {
        std::vector<int> visited(193);
        try {
            nextpnr_mistral_workers::run(visited.size(), workers, [&](size_t index) {
                ++visited[index];
                if (index == 2) throw std::range_error("earliest job");
                if (index == 128) throw std::runtime_error("later job");
            });
            FAIL() << "Expected worker failure on calling thread";
        } catch (const std::range_error &error) {
            EXPECT_STREQ(error.what(), "earliest job");
        }
        for (auto visits : visited) EXPECT_EQ(visits, 1);
    }
}

TEST(MistralAnalogueJobs, ThreadStartupFailureJoinsAlreadyStartedWorkers)
{
    std::vector<int> visited(193);
    int starts = 0;
    auto start = [&](auto work) {
        if (++starts == 3) throw std::runtime_error("injected thread startup failure");
        return std::thread(work);
    };
    EXPECT_THROW(nextpnr_mistral_workers::run(visited.size(), 4,
                 [&](size_t index) { ++visited[index]; }, start), std::runtime_error);
    EXPECT_EQ(starts, 3);
    for (auto visits : visited) EXPECT_EQ(visits, 1);
}
