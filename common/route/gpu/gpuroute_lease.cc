/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 */

#include "gpuroute_lease.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if !defined(_WIN32) && !defined(__wasi__)
#define GPUROUTE_LEASE_SOCKETS 1
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gpuroute {

namespace {

// Lease wait of the calling thread. Kept per thread so that a budget only
// leaves out the waits that held up its own work, not those of a router
// running elsewhere.
thread_local int64_t thread_waited_ns = 0;

#ifdef GPUROUTE_LEASE_SOCKETS

// Process-wide lease state. The mutex is held across the blocking ACQUIRE,
// so a guard created meanwhile on another thread waits for the reply and
// then shares the grant.
struct LeaseState
{
    std::mutex mutex;
    int holders = 0; // guards sharing the current grant
    int fd = -1;     // socket the grant was made on
    int physical_id = -1;
};

LeaseState &lease_state()
{
    static LeaseState state;
    return state;
}

// Replies are short ASCII lines; anything longer is malformed
constexpr size_t max_reply = 256;

// Broker text for a log message: printable ASCII only
std::string printable(const std::string &text)
{
    std::string out;
    for (char c : text)
        out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    return out;
}

bool lease_socket(const char *text, int &fd, std::string &error)
{
    // A plain decimal descriptor number, as gpu-run writes it
    long value = 0;
    size_t digits = 0;
    for (const char *p = text; *p != '\0'; ++p, ++digits) {
        if (*p < '0' || *p > '9' || digits >= 9) {
            value = -1;
            break;
        }
        value = value * 10 + (*p - '0');
    }
    if (digits == 0 || value < 0) {
        error = "GPU_LEASE_FD='" + printable(text) + "' is not a file descriptor number";
        return false;
    }
    struct stat st;
    int type = 0;
    socklen_t len = sizeof(type);
    if (fstat(int(value), &st) != 0 || !S_ISSOCK(st.st_mode) ||
        getsockopt(int(value), SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_STREAM) {
        error = "GPU_LEASE_FD=" + std::to_string(value) + " is not an open stream socket";
        return false;
    }
    fd = int(value);
    return true;
}

// The broker may hand over a non-blocking descriptor (Python sockets with a
// timeout are); wait for it rather than treating EAGAIN as a failure
bool retry_after(int fd, short events)
{
    if (errno == EINTR)
        return true;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return false;
    pollfd ready{fd, events, 0};
    int n;
    while ((n = poll(&ready, 1, -1)) < 0 && errno == EINTR) {
    }
    return n > 0;
}

bool send_request(int fd, const std::string &request, std::string &error)
{
    // A broker that has gone away is an error here, not a SIGPIPE
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
    const std::string line = request + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        ssize_t n = send(fd, line.data() + sent, line.size() - sent, flags);
        if (n < 0 && retry_after(fd, POLLOUT))
            continue;
        if (n <= 0) {
            error = "cannot send " + request + " to the lease broker: " + std::strerror(errno);
            return false;
        }
        sent += size_t(n);
    }
    return true;
}

// One reply line, without its '\n'
bool read_reply(int fd, std::string &reply, std::string &error)
{
    reply.clear();
    while (true) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && retry_after(fd, POLLIN))
            continue;
        if (n < 0) {
            error = std::string("cannot read the lease broker's reply: ") + std::strerror(errno);
            return false;
        }
        if (n == 0) {
            error = reply.empty() ? "the lease broker closed the connection"
                                  : "the lease broker closed the connection in the middle of a reply";
            return false;
        }
        if (c == '\n')
            return true;
        if (reply.size() >= max_reply) {
            error = "the lease broker's reply is longer than " + std::to_string(max_reply) + " bytes";
            return false;
        }
        reply.push_back(c);
    }
}

// "GRANT <decimal physical id>"
bool parse_grant(const std::string &reply, int &id)
{
    const size_t prefix = 6;
    if (reply.compare(0, prefix, "GRANT ") != 0 || reply.size() == prefix || reply.size() > prefix + 9)
        return false;
    int value = 0;
    for (size_t i = prefix; i < reply.size(); i++) {
        if (reply[i] < '0' || reply[i] > '9')
            return false;
        value = value * 10 + (reply[i] - '0');
    }
    id = value;
    return true;
}

// "ERR <msg>"
bool parse_err(const std::string &reply, std::string &message)
{
    if (reply == "ERR") {
        message = "no reason given";
        return true;
    }
    if (reply.compare(0, 4, "ERR ") != 0)
        return false;
    message = printable(reply.substr(4));
    return true;
}

// The stream can no longer be trusted (closed, failed or out of step).
// Shutting it down makes the broker see EOF, which the protocol treats as an
// implicit RELEASE of anything it still thinks this process holds, and makes
// every later request fail at once instead of reading a stale reply. The
// descriptor stays open, so its number cannot be reused for another file.
void abandon(int fd) { shutdown(fd, SHUT_RDWR); }

#endif

} // namespace

GpuLeaseGuard::GpuLeaseGuard(std::function<void(const std::string &)> warn) : warn_(std::move(warn))
{
    const char *fd_text = std::getenv("GPU_LEASE_FD");
    if (fd_text == nullptr)
        return;
    status_ = Status::failed;
#ifndef GPUROUTE_LEASE_SOCKETS
    error_ = "GPU_LEASE_FD is not supported on this platform";
#else
    // All the time this thread spends here is lease wait: blocked behind
    // another guard's ACQUIRE as much as on the broker's reply to its own
    struct WaitTimer
    {
        double &waited;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~WaitTimer()
        {
            const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
            waited = double(ns.count()) * 1e-9;
            thread_waited_ns += int64_t(ns.count());
        }
    } timer{waited_};
    auto &state = lease_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.holders > 0) {
        state.holders++;
        status_ = Status::granted;
        physical_id_ = state.physical_id;
        shared_ = true;
        return;
    }
    int fd = -1;
    if (!lease_socket(fd_text, fd, error_))
        return;
    std::string reply;
    const bool replied = send_request(fd, "ACQUIRE 1", error_) && read_reply(fd, reply, error_);
    std::string message;
    if (!replied) {
        abandon(fd);
    } else if (parse_grant(reply, physical_id_)) {
        state.holders = 1;
        state.fd = fd;
        state.physical_id = physical_id_;
        status_ = Status::granted;
    } else if (parse_err(reply, message)) {
        error_ = "the lease broker refused: " + message;
    } else {
        error_ = "malformed reply from the lease broker: '" + printable(reply) + "'";
        abandon(fd);
    }
#endif
}

GpuLeaseGuard::~GpuLeaseGuard()
{
#ifdef GPUROUTE_LEASE_SOCKETS
    if (status_ != Status::granted)
        return;
    auto &state = lease_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (--state.holders > 0)
        return;
    const int fd = state.fd;
    state.fd = -1;
    state.physical_id = -1;
    std::string error, reply, message;
    if (!send_request(fd, "RELEASE", error) || !read_reply(fd, reply, error)) {
        abandon(fd);
    } else if (reply == "OK") {
        return;
    } else if (parse_err(reply, message)) {
        error = "the lease broker refused RELEASE: " + message;
    } else {
        error = "malformed reply to RELEASE from the lease broker: '" + printable(reply) + "'";
        abandon(fd);
    }
    if (warn_)
        warn_(error);
#endif
}

double lease_wait_seconds() { return double(thread_waited_ns) * 1e-9; }

} // namespace gpuroute
