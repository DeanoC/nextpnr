// Fake-broker tests for the GPU_LEASE_FD client (gpu/gpuroute_lease.h). The
// broker is the other end of an AF_UNIX stream socketpair, as created by
// `gpu-run --phase-lease`; replies are queued on it before the client asks,
// or sent from a thread where timing matters.

#include "gtest/gtest.h"

#include "gpuroute_lease.h"

#ifndef _WIN32
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using gpuroute::GpuLeaseGuard;
using gpuroute::LeaseDevice;
using Status = gpuroute::GpuLeaseGuard::Status;

TEST(GpuLease, RequiredGpuDecidesWhatAFailedLeaseMeans)
{
    EXPECT_EQ(gpuroute::lease_device(Status::none, false), LeaseDevice::configured);
    EXPECT_EQ(gpuroute::lease_device(Status::none, true), LeaseDevice::configured);
    EXPECT_EQ(gpuroute::lease_device(Status::granted, false), LeaseDevice::leased);
    EXPECT_EQ(gpuroute::lease_device(Status::granted, true), LeaseDevice::leased);
    EXPECT_EQ(gpuroute::lease_device(Status::failed, false), LeaseDevice::cpu_fallback);
    EXPECT_EQ(gpuroute::lease_device(Status::failed, true), LeaseDevice::required_failure);
}

#ifndef _WIN32

// The broker end of a socketpair whose client end is advertised in
// GPU_LEASE_FD
class FakeBroker
{
  public:
    explicit FakeBroker(bool advertise = true)
    {
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0);
        if (advertise)
            setenv("GPU_LEASE_FD", std::to_string(fds_[0]).c_str(), 1);
        else
            unsetenv("GPU_LEASE_FD");
    }
    ~FakeBroker()
    {
        unsetenv("GPU_LEASE_FD");
        close_broker();
        close(fds_[0]);
    }

    void send_raw(const std::string &bytes)
    {
        EXPECT_EQ(send(fds_[1], bytes.data(), bytes.size(), MSG_NOSIGNAL), ssize_t(bytes.size()));
    }
    // Queue one reply line
    void reply(const std::string &line) { send_raw(line + "\n"); }
    // Everything the client has sent and the broker not yet read
    std::string received()
    {
        std::string out;
        char buf[512];
        ssize_t n;
        while ((n = recv(fds_[1], buf, sizeof(buf), MSG_DONTWAIT)) > 0)
            out.append(buf, size_t(n));
        return out;
    }
    // Blocks for one request line, returned without its '\n'
    std::string read_request()
    {
        std::string line;
        char c;
        while (recv(fds_[1], &c, 1, 0) == 1 && c != '\n')
            line.push_back(c);
        return line;
    }
    // EOF on the broker end: the client shut the stream down (implicit RELEASE)
    bool client_hung_up()
    {
        char c;
        return recv(fds_[1], &c, 1, MSG_DONTWAIT) == 0;
    }
    // Whether a queued reply is still waiting to be read by the client
    bool reply_unread()
    {
        char c;
        return recv(fds_[0], &c, 1, MSG_DONTWAIT | MSG_PEEK) > 0;
    }
    // EOF for the client after anything already queued; it can still send
    void stop_replying() { shutdown(fds_[1], SHUT_WR); }
    // EOF for the broker: unblocks a read_request() that will get no request
    void stop_requests() { shutdown(fds_[0], SHUT_WR); }
    void make_client_nonblocking() { fcntl(fds_[0], F_SETFL, fcntl(fds_[0], F_GETFL) | O_NONBLOCK); }
    void close_broker()
    {
        if (fds_[1] >= 0)
            close(fds_[1]);
        fds_[1] = -1;
    }

  private:
    int fds_[2] = {-1, -1};
};

struct Warnings
{
    std::vector<std::string> seen;
    std::function<void(const std::string &)> sink()
    {
        return [this](const std::string &w) { seen.push_back(w); };
    }
};

TEST(GpuLease, NoLeaseFdIsANoOp)
{
    FakeBroker broker(false); // a socket exists, but is not advertised
    Warnings warnings;
    const double total = gpuroute::lease_wait_seconds();
    {
        GpuLeaseGuard outer(warnings.sink()), inner(warnings.sink());
        for (const GpuLeaseGuard *guard : {&outer, &inner}) {
            EXPECT_EQ(guard->status(), Status::none);
            EXPECT_EQ(guard->physical_id(), -1);
            EXPECT_FALSE(guard->shared());
            EXPECT_EQ(guard->waited_seconds(), 0.0);
            EXPECT_TRUE(guard->error().empty());
        }
    }
    EXPECT_EQ(broker.received(), "");
    EXPECT_FALSE(broker.client_hung_up());
    EXPECT_EQ(gpuroute::lease_wait_seconds(), total);
    EXPECT_TRUE(warnings.seen.empty());
}

TEST(GpuLease, GrantNamesThePhysicalIdAndDestructionReleasesIt)
{
    FakeBroker broker;
    Warnings warnings;
    broker.reply("GRANT 1");
    {
        GpuLeaseGuard guard(warnings.sink());
        ASSERT_EQ(guard.status(), Status::granted);
        broker.reply("OK");
        EXPECT_EQ(guard.physical_id(), 1);
        EXPECT_FALSE(guard.shared());
        EXPECT_GE(guard.waited_seconds(), 0.0);
        EXPECT_TRUE(guard.error().empty());
        EXPECT_EQ(broker.received(), "ACQUIRE 1\n");
    }
    EXPECT_EQ(broker.received(), "RELEASE\n");
    EXPECT_FALSE(broker.reply_unread()); // the client waited for the OK
    EXPECT_TRUE(warnings.seen.empty());
}

TEST(GpuLease, OverlappingGuardsShareOneGrant)
{
    FakeBroker broker;
    Warnings warnings;
    broker.reply("GRANT 0");
    auto first = std::make_unique<GpuLeaseGuard>(warnings.sink());
    ASSERT_EQ(first->status(), Status::granted);
    broker.reply("OK");
    auto second = std::make_unique<GpuLeaseGuard>(warnings.sink());
    auto third = std::make_unique<GpuLeaseGuard>(warnings.sink());
    for (const auto *guard : {second.get(), third.get()}) {
        EXPECT_EQ(guard->status(), Status::granted);
        EXPECT_EQ(guard->physical_id(), 0);
        EXPECT_TRUE(guard->shared());
        EXPECT_LT(guard->waited_seconds(), 0.1); // nothing to wait for
    }
    EXPECT_FALSE(first->shared());
    EXPECT_EQ(broker.received(), "ACQUIRE 1\n"); // one ACQUIRE for three holders
    // Holders go in any order; only the last one releases
    first.reset();
    third.reset();
    EXPECT_EQ(broker.received(), "");
    second.reset();
    EXPECT_EQ(broker.received(), "RELEASE\n");
    EXPECT_FALSE(broker.reply_unread());

    // The next phase acquires again, possibly another GPU
    broker.reply("GRANT 1");
    {
        GpuLeaseGuard next(warnings.sink());
        ASSERT_EQ(next.status(), Status::granted);
        broker.reply("OK");
        EXPECT_FALSE(next.shared());
        EXPECT_EQ(next.physical_id(), 1);
    }
    EXPECT_EQ(broker.received(), "ACQUIRE 1\nRELEASE\n");
    EXPECT_TRUE(warnings.seen.empty());
}

TEST(GpuLease, ConcurrentGuardsSendOneAcquire)
{
    FakeBroker broker;
    std::thread server([&] {
        EXPECT_EQ(broker.read_request(), "ACQUIRE 1");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        broker.reply("GRANT 1");
        broker.reply("OK");
    });
    std::unique_ptr<GpuLeaseGuard> a, b;
    std::thread other([&] { b = std::make_unique<GpuLeaseGuard>(); });
    a = std::make_unique<GpuLeaseGuard>();
    other.join();
    server.join();
    EXPECT_EQ(a->status(), Status::granted);
    EXPECT_EQ(b->status(), Status::granted);
    EXPECT_EQ(a->physical_id(), 1);
    EXPECT_EQ(b->physical_id(), 1);
    EXPECT_NE(a->shared(), b->shared()); // exactly one of them asked
    EXPECT_EQ(broker.received(), "");
    a.reset();
    b.reset();
    EXPECT_EQ(broker.received(), "RELEASE\n");
}

TEST(GpuLease, ErrReplyIsNotAGrant)
{
    FakeBroker broker;
    Warnings warnings;
    broker.reply("ERR no-gpu-for-you");
    {
        GpuLeaseGuard guard(warnings.sink());
        EXPECT_EQ(guard.status(), Status::failed);
        EXPECT_EQ(guard.physical_id(), -1);
        EXPECT_NE(guard.error().find("no-gpu-for-you"), std::string::npos);
        EXPECT_EQ(gpuroute::lease_device(guard.status(), true), LeaseDevice::required_failure);
        EXPECT_EQ(gpuroute::lease_device(guard.status(), false), LeaseDevice::cpu_fallback);
    }
    // Nothing was granted, so nothing is released, and the stream stays up
    EXPECT_EQ(broker.received(), "ACQUIRE 1\n");
    EXPECT_FALSE(broker.client_hung_up());
    // The next router may ask again
    broker.reply("GRANT 1");
    {
        GpuLeaseGuard guard(warnings.sink());
        ASSERT_EQ(guard.status(), Status::granted);
        broker.reply("OK");
    }
    EXPECT_EQ(broker.received(), "ACQUIRE 1\nRELEASE\n");
    EXPECT_TRUE(warnings.seen.empty());
}

TEST(GpuLease, BrokerClosingWhileTheClientWaitsFails)
{
    FakeBroker broker;
    std::thread server([&] {
        EXPECT_EQ(broker.read_request(), "ACQUIRE 1");
        broker.close_broker();
    });
    GpuLeaseGuard guard;
    server.join();
    EXPECT_EQ(guard.status(), Status::failed);
    EXPECT_NE(guard.error().find("closed the connection"), std::string::npos);
    EXPECT_EQ(gpuroute::lease_device(guard.status(), true), LeaseDevice::required_failure);
    EXPECT_EQ(gpuroute::lease_device(guard.status(), false), LeaseDevice::cpu_fallback);
    // A later router fails at once instead of hanging or being granted
    GpuLeaseGuard later;
    EXPECT_EQ(later.status(), Status::failed);
    EXPECT_FALSE(later.error().empty());
}

TEST(GpuLease, BrokerGoneBeforeAcquireFails)
{
    FakeBroker broker;
    broker.close_broker();
    GpuLeaseGuard guard; // must not die of SIGPIPE
    EXPECT_EQ(guard.status(), Status::failed);
    EXPECT_NE(guard.error().find("cannot send ACQUIRE"), std::string::npos);
}

TEST(GpuLease, ReplyCutShortByEofFails)
{
    FakeBroker broker;
    broker.send_raw("GRANT 1");
    broker.stop_replying();
    GpuLeaseGuard guard;
    EXPECT_EQ(guard.status(), Status::failed);
    EXPECT_NE(guard.error().find("in the middle of a reply"), std::string::npos);
    EXPECT_EQ(broker.received(), "ACQUIRE 1\n");
}

TEST(GpuLease, MalformedReplyAbandonsTheStream)
{
    const std::vector<std::string> replies = {"GRANT",     "GRANT ",           "GRANT x", "GRANT -1",
                                              "GRANT 1 2", "GRANT 1\r",        "GRANT 0x1", "GRANT 1234567890",
                                              "grant 1",   "OK",               "",        std::string(300, 'G')};
    for (const auto &reply : replies) {
        SCOPED_TRACE(reply);
        FakeBroker broker;
        {
            broker.reply(reply);
            GpuLeaseGuard guard;
            EXPECT_EQ(guard.status(), Status::failed);
            EXPECT_EQ(guard.physical_id(), -1);
            EXPECT_FALSE(guard.error().empty());
            EXPECT_EQ(gpuroute::lease_device(guard.status(), true), LeaseDevice::required_failure);
        }
        EXPECT_EQ(broker.received(), "ACQUIRE 1\n");
        // Out of step: the client hangs up, which the broker treats as an
        // implicit RELEASE, so nothing stays held on its behalf
        EXPECT_TRUE(broker.client_hung_up());
        GpuLeaseGuard later;
        EXPECT_EQ(later.status(), Status::failed);
    }
}

TEST(GpuLease, RefusedReleaseIsReported)
{
    FakeBroker broker;
    Warnings warnings;
    broker.reply("GRANT 2");
    {
        GpuLeaseGuard guard(warnings.sink());
        ASSERT_EQ(guard.status(), Status::granted);
        broker.reply("ERR not-held");
    }
    EXPECT_EQ(broker.received(), "ACQUIRE 1\nRELEASE\n");
    ASSERT_EQ(warnings.seen.size(), 1u);
    EXPECT_NE(warnings.seen[0].find("not-held"), std::string::npos);
    EXPECT_FALSE(broker.client_hung_up()); // still in step
}

TEST(GpuLease, ReleaseToAClosedBrokerIsReported)
{
    FakeBroker broker;
    Warnings warnings;
    broker.reply("GRANT 2");
    {
        GpuLeaseGuard guard(warnings.sink());
        ASSERT_EQ(guard.status(), Status::granted);
        broker.close_broker();
    }
    ASSERT_EQ(warnings.seen.size(), 1u);
    EXPECT_NE(warnings.seen[0].find("RELEASE"), std::string::npos);
}

TEST(GpuLease, WaitForTheGrantIsTimedAndAccumulated)
{
    FakeBroker broker;
    const double total = gpuroute::lease_wait_seconds();
    std::thread server([&] {
        EXPECT_EQ(broker.read_request(), "ACQUIRE 1");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        broker.reply("GRANT 1");
        broker.reply("OK");
    });
    double waited;
    {
        GpuLeaseGuard guard;
        server.join();
        EXPECT_EQ(guard.status(), Status::granted);
        waited = guard.waited_seconds();
        EXPECT_GE(waited, 0.29);
        EXPECT_NEAR(gpuroute::lease_wait_seconds() - total, waited, 1e-6);
    }
    // Releasing a grant is not waiting
    EXPECT_NEAR(gpuroute::lease_wait_seconds() - total, waited, 1e-6);
}

TEST(GpuLease, WaitsCountForTheThreadThatWaited)
{
    FakeBroker broker;
    std::promise<void> asked;
    std::thread server([&] {
        EXPECT_EQ(broker.read_request(), "ACQUIRE 1");
        asked.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        broker.reply("GRANT 1");
        broker.reply("OK");
    });
    // A router on another thread asks the broker and waits for the grant
    double first_waited = 0, first_thread_total = 0;
    std::unique_ptr<GpuLeaseGuard> first;
    std::thread other([&] {
        const double before = gpuroute::lease_wait_seconds();
        first = std::make_unique<GpuLeaseGuard>();
        first_waited = first->waited_seconds();
        first_thread_total = gpuroute::lease_wait_seconds() - before;
    });
    // This thread's guard arrives while that ACQUIRE is outstanding and is
    // held up until it can share the grant
    asked.get_future().wait();
    const double before = gpuroute::lease_wait_seconds();
    auto second = std::make_unique<GpuLeaseGuard>();
    const double this_thread_total = gpuroute::lease_wait_seconds() - before;
    other.join();
    server.join();
    ASSERT_EQ(first->status(), Status::granted);
    ASSERT_EQ(second->status(), Status::granted);
    EXPECT_FALSE(first->shared());
    EXPECT_TRUE(second->shared());
    EXPECT_GE(first_waited, 0.29);
    EXPECT_GE(second->waited_seconds(), 0.2); // blocked behind the first ACQUIRE
    // Each thread's total holds its own wait and nothing of the other's
    EXPECT_NEAR(first_thread_total, first_waited, 1e-6);
    EXPECT_NEAR(this_thread_total, second->waited_seconds(), 1e-6);
    first.reset();
    second.reset();
    EXPECT_EQ(broker.received(), "RELEASE\n");
}

TEST(GpuLease, NonBlockingLeaseFdStillWaitsForReplies)
{
    FakeBroker broker;
    broker.make_client_nonblocking();
    std::thread server([&] {
        EXPECT_EQ(broker.read_request(), "ACQUIRE 1");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        broker.reply("GRANT 3");
        EXPECT_EQ(broker.read_request(), "RELEASE");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        broker.reply("OK");
    });
    {
        GpuLeaseGuard guard;
        EXPECT_EQ(guard.status(), Status::granted);
        EXPECT_EQ(guard.physical_id(), 3);
        EXPECT_GE(guard.waited_seconds(), 0.19);
    }
    broker.stop_requests();
    server.join();
    EXPECT_FALSE(broker.reply_unread()); // RELEASE waited for its OK as well
}

TEST(GpuLease, UnusableLeaseFdIsNotAGrant)
{
    char path[] = "/tmp/nextpnr-gpu-lease-XXXXXX";
    const int file = mkstemp(path);
    ASSERT_GE(file, 0);
    unlink(path);
    int datagram[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, datagram), 0);
    const int unused = dup(file);
    close(unused);
    std::vector<std::string> values = {"", "x", "-1", "3x", " 3", "1234567890"};
    values.push_back(std::to_string(file));        // not a socket
    values.push_back(std::to_string(datagram[0])); // not a stream socket
    values.push_back(std::to_string(unused));      // not open
    for (const auto &value : values) {
        SCOPED_TRACE(value);
        setenv("GPU_LEASE_FD", value.c_str(), 1);
        GpuLeaseGuard guard;
        EXPECT_EQ(guard.status(), Status::failed);
        EXPECT_NE(guard.error().find("GPU_LEASE_FD"), std::string::npos);
        EXPECT_LT(guard.waited_seconds(), 0.1); // fails without asking
        EXPECT_EQ(gpuroute::lease_device(guard.status(), true), LeaseDevice::required_failure);
        EXPECT_EQ(gpuroute::lease_device(guard.status(), false), LeaseDevice::cpu_fallback);
    }
    unsetenv("GPU_LEASE_FD");
    // No request was written anywhere
    EXPECT_EQ(lseek(file, 0, SEEK_END), 0);
    char c;
    EXPECT_LT(recv(datagram[1], &c, 1, MSG_DONTWAIT), 0);
    close(file);
    close(datagram[0]);
    close(datagram[1]);
}

#endif

} // namespace
