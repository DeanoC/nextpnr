/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
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
 *  Client side of the gpu-run phase lease.
 *
 *  `gpu-run --phase-lease` passes one end of an AF_UNIX stream socketpair to
 *  nextpnr as GPU_LEASE_FD=<n> and grants physical GPU ids over it, so a GPU
 *  is held only while a GPU router exists rather than for the whole run. The
 *  protocol is described in docs/gpurouter.md. Like gpuroute_backend.h this
 *  header is free of nextpnr types, so the fake-broker test builds without an
 *  architecture.
 */

#ifndef GPUROUTE_LEASE_H
#define GPUROUTE_LEASE_H

#include <functional>
#include <string>

namespace gpuroute {

// One holder of the process's GPU phase lease. The first guard sends
// "ACQUIRE 1" on GPU_LEASE_FD and waits for the broker's reply; guards
// created while that grant is held share it without a second ACQUIRE, and
// the last of them to be destroyed sends "RELEASE". Without GPU_LEASE_FD in
// the environment a guard does nothing at all.
class GpuLeaseGuard
{
  public:
    enum class Status
    {
        none,    // no GPU_LEASE_FD: no protocol, the configured device as before
        granted, // physical_id() is the GPU to use
        failed,  // ERR reply, closed socket, malformed reply or unusable GPU_LEASE_FD
    };

    // `warn` reports a refused or failed RELEASE, which a destructor cannot
    // return
    explicit GpuLeaseGuard(std::function<void(const std::string &)> warn = {});
    ~GpuLeaseGuard();
    GpuLeaseGuard(const GpuLeaseGuard &) = delete;
    GpuLeaseGuard &operator=(const GpuLeaseGuard &) = delete;

    Status status() const { return status_; }
    // The broker-returned physical GPU id, used as the device index as it
    // is: no HIP/ROCR/CUDA_VISIBLE_DEVICES mask is set or changed
    int physical_id() const { return physical_id_; }
    // True if the grant was already held by another guard (no ACQUIRE sent)
    bool shared() const { return shared_; }
    // Seconds this guard waited for the broker's reply to its ACQUIRE
    double waited_seconds() const { return waited_; }
    const std::string &error() const { return error_; }

  private:
    std::function<void(const std::string &)> warn_;
    Status status_ = Status::none;
    int physical_id_ = -1;
    bool shared_ = false;
    double waited_ = 0.0;
    std::string error_;
};

// Seconds this process has spent waiting for ACQUIRE replies in total, so
// that wall-clock route budgets can leave the wait out. Always 0 without
// GPU_LEASE_FD.
double lease_wait_seconds();

// What GpuRouter::setup_all() does with a guard before it opens a device. A
// lease that was not granted is treated like a device that cannot be opened:
// the CPU reference backend, or a hard error when GPU use is required.
enum class LeaseDevice
{
    configured, // no lease: the configured device, as without a broker
    leased,     // the granted physical id
    cpu_fallback,
    required_failure,
};

inline LeaseDevice lease_device(GpuLeaseGuard::Status status, bool gpu_required)
{
    switch (status) {
    case GpuLeaseGuard::Status::none:
        return LeaseDevice::configured;
    case GpuLeaseGuard::Status::granted:
        return LeaseDevice::leased;
    default:
        return gpu_required ? LeaseDevice::required_failure : LeaseDevice::cpu_fallback;
    }
}

} // namespace gpuroute

#endif // GPUROUTE_LEASE_H
