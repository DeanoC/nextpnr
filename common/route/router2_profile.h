/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  gatecat <gatecat@ds0.me>
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

/* Optional router2 diagnostics. No access to router state, routing costs or RNG. */
#ifndef ROUTER2_PROFILE_H
#define ROUTER2_PROFILE_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef ROUTER2_PROFILE_WIN32_SHIM
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "json11.hpp"

namespace router2_diagnostics {

class Profile
{
    using Clock = std::chrono::steady_clock;
    struct State
    {
        std::string name;
        int fanout;
        bool truncated = false;
        unsigned active = 0;
        uint64_t started = 0, finished = 0;
        Clock::time_point active_since;
        double total_seconds = 0, max_seconds = 0;
    };
    std::vector<State> states;
    std::string path;
    Clock::time_point start = Clock::now();
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    uint64_t sequence = 0;
    std::exception_ptr failure;
    std::thread writer;

    void write(bool completed)
    {
        struct Row {
            size_t index;
            unsigned active;
            uint64_t started, finished;
            double total, maximum, active_seconds;
        };
        std::vector<Row> active, slowest;
        active.reserve(states.size());
        slowest.reserve(states.size());
        double elapsed;
        uint64_t calls_started = 0, calls_finished = 0;
        {
            std::lock_guard<std::mutex> guard(mutex);
            auto now = Clock::now();
            elapsed = std::chrono::duration<double>(now - start).count();
            for (size_t index = 0; index < states.size(); ++index) {
                const auto &s = states[index];
                calls_started += s.started;
                calls_finished += s.finished;
                Row row{index, s.active, s.started, s.finished, s.total_seconds, s.max_seconds, 0};
                if (s.active) {
                    row.active_seconds = std::chrono::duration<double>(now - s.active_since).count();
                    active.push_back(row);
                }
                if (s.finished)
                    slowest.push_back(row);
            }
        }
        auto top = [](std::vector<Row> &rows, bool active) {
            size_t count = std::min(rows.size(), size_t(20));
            std::partial_sort(rows.begin(), rows.begin() + count, rows.end(), [active](const Row &a, const Row &b) {
                double av = active ? a.active_seconds : a.total;
                double bv = active ? b.active_seconds : b.total;
                if (av != bv)
                    return av > bv;
                return a.index < b.index;
            });
            rows.resize(count);
        };
        size_t active_count = active.size();
        top(active, true);
        top(slowest, false);
        auto encode = [this](const std::vector<Row> &rows, bool active) {
            json11::Json::array output;
            for (const auto &r : rows) {
                const auto &s = states[r.index]; // Names/fanout are immutable.
                json11::Json::object row{{"net_index", double(r.index)}, {"net", s.name},
                    {"name_truncated", s.truncated}, {"fanout", s.fanout},
                    {"calls_started", double(r.started)}, {"calls_finished", double(r.finished)},
                    {"total_seconds", r.total}, {"max_seconds", r.maximum}};
                if (active) {
                    row["active_calls"] = int(r.active);
                    row["active_seconds"] = r.active_seconds;
                }
                output.emplace_back(row);
            }
            return output;
        };
        json11::Json snapshot = json11::Json::object{{"schema", "router2-profile-v1"},
            {"sequence", double(++sequence)}, {"elapsed_seconds", elapsed},
            {"completed", completed}, {"net_count", double(states.size())},
            {"calls_started", double(calls_started)}, {"calls_finished", double(calls_finished)},
            {"active_net_count", double(active_count)}, {"active", encode(active, true)},
            {"slowest", encode(slowest, false)}};
        const auto payload = snapshot.dump() + '\n';
        std::string temporary;
        bool created = false;
        for (unsigned n = 0; n < 10000 && !created; ++n) {
#if defined(_WIN32)
            temporary = path + ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(n);
            HANDLE handle = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                DWORD error = GetLastError();
                if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
                    continue;
                throw std::runtime_error("Cannot write router2 profile: " + path);
            }
            DWORD written = 0;
            BOOL ok = WriteFile(handle, payload.data(), DWORD(payload.size()), &written, nullptr);
            ok = ok && written == DWORD(payload.size()) && FlushFileBuffers(handle);
            CloseHandle(handle);
            if (!ok) {
                DeleteFileA(temporary.c_str());
                throw std::runtime_error("Cannot write router2 profile: " + path);
            }
#else
            temporary = path + ".tmp." + std::to_string(getpid()) + "." + std::to_string(n);
            int fd = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) {
                if (errno == EEXIST)
                    continue;
                throw std::runtime_error("Cannot write router2 profile: " + path);
            }
            ssize_t written = ::write(fd, payload.data(), payload.size());
            bool ok = written == ssize_t(payload.size()) && ::fsync(fd) == 0;
            if (::close(fd) != 0)
                ok = false;
            if (!ok) {
                ::unlink(temporary.c_str());
                throw std::runtime_error("Cannot write router2 profile: " + path);
            }
#endif
            created = true;
        }
        if (!created)
            throw std::runtime_error("Cannot write router2 profile: " + path);
#if defined(_WIN32)
        if (!MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(temporary.c_str());
            throw std::runtime_error("Cannot write router2 profile: " + path);
        }
#else
        if (std::rename(temporary.c_str(), path.c_str()) != 0) {
            ::unlink(temporary.c_str());
            throw std::runtime_error("Cannot write router2 profile: " + path);
        }
#endif
    }

  public:
    Profile(const std::string &path, const std::vector<std::pair<std::string, int>> &nets) : path(path)
    {
        for (const auto &net : nets) {
            State s;
            s.name = net.first;
            s.fanout = net.second;
            if (s.name.size() > 1024) {
                size_t end = 1024;
                while ((static_cast<unsigned char>(s.name[end]) & 0xc0) == 0x80)
                    --end;
                s.name.resize(end);
                s.truncated = true;
            }
            states.push_back(std::move(s));
        }
        write(false); // Reject an unwritable destination before routing starts.
        writer = std::thread([this]() {
            try {
                std::unique_lock<std::mutex> lock(mutex);
                while (!wake.wait_for(lock, std::chrono::seconds(1), [this]() { return stopping; })) {
                    lock.unlock();
                    write(false);
                    lock.lock();
                }
            } catch (...) {
                failure = std::current_exception();
            }
        });
    }
    Profile(const Profile &) = delete;
    Profile &operator=(const Profile &) = delete;
    void finish(bool completed)
    {
        if (!writer.joinable())
            return;
        {
            std::lock_guard<std::mutex> guard(mutex);
            stopping = true;
        }
        wake.notify_one();
        writer.join();
        if (failure)
            std::rethrow_exception(failure);
        write(completed);
    }
    ~Profile()
    {
        try { finish(false); } catch (...) { }
    }
    class Visit
    {
        Profile *profile;
        size_t index;
        Clock::time_point began;
      public:
        Visit(Profile *profile, size_t index) : profile(profile), index(index)
        {
            if (!profile)
                return;
            began = Clock::now();
            std::lock_guard<std::mutex> guard(profile->mutex);
            auto &s = profile->states.at(index);
            if (s.active++ == 0)
                s.active_since = began;
            ++s.started;
        }
        Visit(const Visit &) = delete;
        Visit &operator=(const Visit &) = delete;
        ~Visit()
        {
            if (!profile)
                return;
            double seconds = std::chrono::duration<double>(Clock::now() - began).count();
            std::lock_guard<std::mutex> guard(profile->mutex);
            auto &s = profile->states.at(index);
            --s.active;
            ++s.finished;
            s.total_seconds += seconds;
            s.max_seconds = std::max(s.max_seconds, seconds);
        }
    };
};
} // namespace router2_diagnostics
#endif
