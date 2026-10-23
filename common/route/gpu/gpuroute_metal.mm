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
 *  Metal (Apple silicon) host side of the GPU routing backend. The kernel is
 *  gpuroute_metal.metal, a port of gpuroute_kernel.cuh; it is embedded in
 *  the binary and compiled when the backend opens a device, so a build needs
 *  only the Metal framework, not the offline Metal toolchain.
 *
 *  Apple GPUs share memory with the CPU. Every buffer is a shared buffer:
 *  uploads and downloads are plain copies, and wire state updates are written
 *  in place between launches.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpuroute_backend.h"
#include "gpuroute_metal_source.h"

namespace gpuroute {

namespace {

constexpr int NPILES = 4;
// Threads per net the kernel is built for, one pipeline each
constexpr int BLOCK_SIZES[] = {256, 512, 1024};

// Mirrors of the structs gpuroute_metal.metal declares. Metal lays them out
// as the host compiler does; a field added on one side only changes a size.
struct LaunchArgs
{
    int32_t n_wires;
    int32_t ntasks;
    int32_t cap;
};
struct alignas(16) WireRec
{
    int16_t x, y;
    uint8_t flags;
    uint8_t pad[3];
    int32_t reserved;
    int32_t occ;
    float hist;
};
static_assert(sizeof(WireRec) == 32 && offsetof(WireRec, hist) == 16, "WireRec layout");
static_assert(sizeof(RouteParams) == 68 && offsetof(RouteParams, exact) == 64, "RouteParams layout");
static_assert(sizeof(TaskDesc) == 68 && offsetof(TaskDesc, bb_x0) == 44 && offsetof(TaskDesc, shell_cnt) == 64,
              "TaskDesc layout");
static_assert(sizeof(ArcDesc) == 12, "ArcDesc layout");
static_assert(sizeof(ArcResult) == 28 && offsetof(ArcResult, cost) == 24, "ArcResult layout");
static_assert(sizeof(PathEntry) == 16, "PathEntry layout");

// Buffer indices of route_kernel
enum : NSUInteger
{
    B_OUT_OFF,
    B_OUT_DST,
    B_EDGE_SRC,
    B_EDGE_COST,
    B_EDGE_REGION,
    B_EDGE_OUTSIDE_HALO,
    B_WIRES,
    B_PARAMS,
    B_ARGS,
    B_TASKS,
    B_ARCS,
    B_SEEDS,
    B_SEED_DELAY,
    B_SEED_LOAD,
    B_SHELL_TILES,
    B_KEYS,
    B_VALS,
    B_SDELAY,
    B_SLOAD,
    B_PILES,
    B_CAND_PAR,
    B_TOUCHED,
    B_BEST,
    B_RESULTS,
    B_PATHS,
};

struct DeviceAllocationFailure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

std::string ns_error(NSError *error)
{
    if (error == nil)
        return "unknown error";
    return std::string([[error localizedDescription] UTF8String]);
}

std::string device_label(id<MTLDevice> dev)
{
    return std::string([[dev name] UTF8String]);
}

// Bumped on every buffer allocation or release, so the backend knows when to
// rebuild its residency set
uint64_t buffer_generation = 0;

// A shared (CPU and GPU visible) buffer of T. Metal has no empty buffers, so
// a zero-length buffer still holds one element.
template <typename T> struct SharedBuf
{
    id<MTLBuffer> buf = nil;
    size_t count = 0;
    T *ptr() const { return buf == nil ? nullptr : static_cast<T *>([buf contents]); }
    void release()
    {
        if (buf != nil)
            buffer_generation++;
        buf = nil;
        count = 0;
    }
    void alloc(id<MTLDevice> dev, size_t n)
    {
        release();
        const size_t bytes = std::max<size_t>(n, 1) * sizeof(T);
        if (bytes > [dev maxBufferLength])
            throw DeviceAllocationFailure("GPU allocation failed: " + std::to_string(bytes) +
                                          " bytes exceeds the device buffer limit");
        id<MTLBuffer> b = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (b == nil)
            throw DeviceAllocationFailure("GPU allocation failed: " + std::to_string(bytes) + " bytes");
        buf = b;
        count = n;
        buffer_generation++;
    }
    void ensure(id<MTLDevice> dev, size_t n)
    {
        if (buf == nil || n > count)
            alloc(dev, std::max(n, count + count / 2));
    }
    void upload(id<MTLDevice> dev, const T *src, size_t n)
    {
        ensure(dev, n);
        if (n)
            std::memcpy(ptr(), src, n * sizeof(T));
    }
};

struct Lane
{
    SharedBuf<uint32_t> keys;
    SharedBuf<uint64_t> vals;
    SharedBuf<float> sdelay, sload;
    SharedBuf<uint64_t> piles;
    SharedBuf<uint32_t> cand_par;
    SharedBuf<uint32_t> touched;
    SharedBuf<uint64_t> best;
    int slots = 0;
    int cap = 0;
    static size_t bytes_per_slot(int bits)
    {
        return (size_t(1) << bits) * (sizeof(uint32_t) * 5 + sizeof(uint64_t) * (1 + NPILES)) + sizeof(uint64_t);
    }
    void alloc(id<MTLDevice> dev, int nslots, int bits)
    {
        const int new_cap = 1 << bits;
        const size_t entries = (size_t)nslots * new_cap;
        keys.ensure(dev, entries);
        vals.ensure(dev, entries);
        sdelay.ensure(dev, entries);
        sload.ensure(dev, entries);
        piles.ensure(dev, (size_t)nslots * NPILES * (size_t)new_cap);
        cand_par.ensure(dev, entries);
        touched.ensure(dev, entries);
        best.ensure(dev, (size_t)nslots);
        slots = nslots;
        cap = new_cap;
        // Every table starts empty; each arc leaves its table empty again
        std::memset(keys.ptr(), 0xFF, entries * sizeof(uint32_t));
        std::memset(vals.ptr(), 0xFF, entries * sizeof(uint64_t));
    }
    template <typename F> void for_each_buffer(F f) const
    {
        for (id<MTLBuffer> b : {keys.buf, vals.buf, sdelay.buf, sload.buf, piles.buf, cand_par.buf, touched.buf, best.buf})
            f(b);
    }
    void release()
    {
        keys.release();
        vals.release();
        sdelay.release();
        sload.release();
        piles.release();
        cand_par.release();
        touched.release();
        best.release();
        slots = 0;
        cap = 0;
    }
};

class MetalBackend : public Backend
{
  public:
    MetalBackend(id<MTLDevice> dev, int device) : dev_(dev), device_(device) {}

    bool init(const GraphData &graph, int small_slots, int small_bits, int large_slots, int large_bits,
              std::string &error) override
    {
        @autoreleasepool {
            try {
                device_name_ = device_label(dev_);
                char registry_id[32];
                std::snprintf(registry_id, sizeof(registry_id), "%016" PRIx64, (uint64_t)[dev_ registryID]);
                name_ = std::string("metal:") + registry_id + ":" + device_name_;
                if (queue_ == nil)
                    build_pipeline();

                n_wires_ = graph.n_wires;
                out_off_.upload(dev_, graph.out_off, (size_t)graph.n_wires + 1);
                out_dst_.upload(dev_, graph.out_dst, (size_t)graph.n_edges);
                // The path walk needs the source wire of an edge; one load
                // beats a binary search over the row offsets for every hop.
                edge_src_.ensure(dev_, (size_t)graph.n_edges);
                for (int32_t w = 0; w < graph.n_wires; w++)
                    for (int32_t e = graph.out_off[w]; e < graph.out_off[w + 1]; e++)
                        edge_src_.ptr()[e] = w;
                edge_cost_.upload(dev_, graph.edge_cost, (size_t)graph.n_edges);
                // nullptr is an all-zero array (no reserved pips, no halo fence)
                upload_or_zero(edge_region_, graph.edge_region, (size_t)graph.n_edges);
                upload_or_zero(edge_outside_halo_, graph.edge_outside_halo, (size_t)graph.n_edges);
                wires_.ensure(dev_, (size_t)graph.n_wires);
                for (int32_t w = 0; w < graph.n_wires; w++) {
                    WireRec &r = wires_.ptr()[w];
                    r.x = graph.wire_x[w];
                    r.y = graph.wire_y[w];
                    r.flags = graph.wire_flags[w];
                    r.pad[0] = r.pad[1] = r.pad[2] = 0;
                    r.reserved = graph.wire_reserved[w];
                    r.occ = graph.wire_occ[w];
                    r.hist = graph.wire_hist[w];
                }
                small_.alloc(dev_, small_slots, small_bits);
                auto_large_slots_ = large_slots <= 0;
                if (large_slots <= 0) {
                    // As the HIP/CUDA backend: as many large slots as three
                    // quarters of the free memory holds, between 4 and 32,
                    // and no more than fit in one buffer.
                    const size_t per_slot = Lane::bytes_per_slot(large_bits);
                    const size_t pile_slot = (size_t(1) << large_bits) * NPILES * sizeof(uint64_t);
                    size_t slots = std::min<size_t>(32, std::max<size_t>(4, free_bytes() / 4 * 3 / per_slot));
                    slots = std::min<size_t>(slots, std::max<size_t>(1, [dev_ maxBufferLength] / pile_slot));
                    large_slots = int(slots);
                }
                large_slots_ = large_slots;
                large_bits_ = large_bits;
                large_.alloc(dev_, large_slots, large_bits);
            } catch (const std::exception &e) {
                error = e.what();
                return false;
            }
            return true;
        }
    }

    void update_wire_state(size_t count, const int32_t *wires, const int32_t *occ, const float *hist,
                           const int32_t *reserved) override
    {
        // No kernel is running between calls, and the buffers are shared:
        // write the new state in place
        auto t0 = std::chrono::steady_clock::now();
        WireRec *recs = wires_.ptr();
        for (size_t i = 0; i < count; i++) {
            WireRec &r = recs[wires[i]];
            r.occ = occ[i];
            r.hist = hist[i];
            r.reserved = reserved[i];
        }
        stats_.transfer_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    void route(const RouteParams &params, const std::vector<TaskDesc> &tasks, const std::vector<ArcDesc> &arcs,
               const std::vector<int32_t> &seeds, const std::vector<float> &seed_delay,
               const std::vector<float> &seed_load, const std::vector<int32_t> &shell_tiles, bool large_lane,
               std::vector<ArcResult> &results, std::vector<PathEntry> &paths) override
    {
        results.assign(arcs.size(), ArcResult());
        size_t total_paths = 0;
        for (auto &t : tasks)
            total_paths = std::max(total_paths, (size_t)t.path_off + (size_t)t.path_cap);
        paths.assign(total_paths, PathEntry{-1, -1, -1, 0.0f});
        if (tasks.empty())
            return;

        @autoreleasepool {
            auto t0 = std::chrono::steady_clock::now();
            auto prepare_route_buffers = [&]() {
                tasks_.upload(dev_, tasks.data(), tasks.size());
                arcs_.upload(dev_, arcs.data(), arcs.size());
                seeds_.upload(dev_, seeds.data(), seeds.size());
                seed_delay_.upload(dev_, seed_delay.data(), seed_delay.size());
                seed_load_.upload(dev_, seed_load.data(), seed_load.size());
                shell_tiles_.upload(dev_, shell_tiles.data(), shell_tiles.size());
                results_.ensure(dev_, arcs.size());
                std::memset(results_.ptr(), 0, arcs.size() * sizeof(ArcResult));
                // Unused path slots come back as the CPU backend leaves them
                paths_.upload(dev_, paths.data(), total_paths);
            };
            try {
                prepare_route_buffers();
            } catch (const DeviceAllocationFailure &) {
                if (!auto_large_slots_ || large_slots_ <= 1)
                    throw;
                // As the HIP/CUDA backend: free the automatically sized large
                // lane, allocate the batch buffers, then restore the largest
                // lane that still fits. No kernel has run yet.
                large_.release();
                prepare_route_buffers();
                for (int slots = large_slots_; slots > 0; --slots) {
                    try {
                        large_.alloc(dev_, slots, large_bits_);
                        large_slots_ = slots;
                        break;
                    } catch (const DeviceAllocationFailure &) {
                        large_.release();
                        if (slots == 1)
                            throw;
                    }
                }
            }
            auto t1 = std::chrono::steady_clock::now();
            stats_.transfer_seconds += std::chrono::duration<double>(t1 - t0).count();

            const Lane &lane = large_lane ? large_ : small_;
            const int grid = (int)std::min<size_t>(tasks.size(), (size_t)lane.slots);
            LaunchArgs args{n_wires_, (int32_t)tasks.size(), lane.cap};

            refresh_residency();
            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            const int block = pick_block(params.block_threads);
            [enc setComputePipelineState:route_pso_[block]];
            auto bind = [&](id<MTLBuffer> b, NSUInteger index) { [enc setBuffer:b offset:0 atIndex:index]; };
            bind(out_off_.buf, B_OUT_OFF);
            bind(out_dst_.buf, B_OUT_DST);
            bind(edge_src_.buf, B_EDGE_SRC);
            bind(edge_cost_.buf, B_EDGE_COST);
            bind(edge_region_.buf, B_EDGE_REGION);
            bind(edge_outside_halo_.buf, B_EDGE_OUTSIDE_HALO);
            bind(wires_.buf, B_WIRES);
            [enc setBytes:&params length:sizeof(params) atIndex:B_PARAMS];
            [enc setBytes:&args length:sizeof(args) atIndex:B_ARGS];
            bind(tasks_.buf, B_TASKS);
            bind(arcs_.buf, B_ARCS);
            bind(seeds_.buf, B_SEEDS);
            bind(seed_delay_.buf, B_SEED_DELAY);
            bind(seed_load_.buf, B_SEED_LOAD);
            bind(shell_tiles_.buf, B_SHELL_TILES);
            bind(lane.keys.buf, B_KEYS);
            bind(lane.vals.buf, B_VALS);
            bind(lane.sdelay.buf, B_SDELAY);
            bind(lane.sload.buf, B_SLOAD);
            bind(lane.piles.buf, B_PILES);
            bind(lane.cand_par.buf, B_CAND_PAR);
            bind(lane.touched.buf, B_TOUCHED);
            bind(lane.best.buf, B_BEST);
            bind(results_.buf, B_RESULTS);
            bind(paths_.buf, B_PATHS);
            [enc dispatchThreadgroups:MTLSizeMake(grid, 1, 1) threadsPerThreadgroup:MTLSizeMake(BLOCK_SIZES[block], 1, 1)];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if ([cmd status] != MTLCommandBufferStatusCompleted)
                throw std::runtime_error("GPU error: route kernel failed: " + ns_error([cmd error]));
            auto t2 = std::chrono::steady_clock::now();
            stats_.route_seconds += std::chrono::duration<double>(t2 - t1).count();
            stats_.launches++;

            std::memcpy(results.data(), results_.ptr(), arcs.size() * sizeof(ArcResult));
            std::memcpy(paths.data(), paths_.ptr(), total_paths * sizeof(PathEntry));
            stats_.transfer_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
        }
        for (auto &r : results) {
            stats_.arcs_routed++;
            stats_.wires_expanded += r.expanded;
        }
    }

    const char *name() const override { return name_.c_str(); }
    int device_index() const override { return device_; }
    const char *device_name() const override { return device_name_.c_str(); }
    const BackendStats &stats() const override { return stats_; }

  private:
    size_t free_bytes() const
    {
        const uint64_t budget = [dev_ recommendedMaxWorkingSetSize];
        const uint64_t used = [dev_ currentAllocatedSize];
        return budget > used ? size_t(budget - used) : 0;
    }

    // Keep every buffer resident between launches. Without a residency set
    // Metal makes the bound buffers resident for each command buffer, which
    // costs time in proportion to their size (gigabytes for the large lane).
    void refresh_residency()
    {
        if (@available(macOS 15.0, *)) {
            if (residency_ == nil || residency_generation_ == buffer_generation)
                return;
            // Only the buffers that changed: re-adding a large buffer costs
            // as much as making it resident in the first place
            std::vector<id<MTLBuffer>> now;
            auto add = [&](id<MTLBuffer> b) {
                if (b != nil)
                    now.push_back(b);
            };
            for (id<MTLBuffer> b : {out_off_.buf, out_dst_.buf, edge_src_.buf, edge_cost_.buf, edge_region_.buf,
                                    edge_outside_halo_.buf, wires_.buf, tasks_.buf, arcs_.buf, seeds_.buf,
                                    seed_delay_.buf, seed_load_.buf, shell_tiles_.buf, results_.buf, paths_.buf})
                add(b);
            small_.for_each_buffer(add);
            large_.for_each_buffer(add);
            id<MTLResidencySet> set = residency_;
            for (id<MTLBuffer> b : resident_)
                if (std::find(now.begin(), now.end(), b) == now.end())
                    [set removeAllocation:b];
            for (id<MTLBuffer> b : now)
                if (std::find(resident_.begin(), resident_.end(), b) == resident_.end())
                    [set addAllocation:b];
            [set commit];
            [set requestResidency];
            resident_ = std::move(now);
            residency_generation_ = buffer_generation;
        }
    }

    // Index into BLOCK_SIZES of the pipeline for the requested threads per
    // net: the largest built one not above it. The result does not depend on
    // the thread count, so a device that cannot run a size uses a smaller one.
    int pick_block(int threads) const
    {
        int pick = 0;
        for (int i = 0; i < (int)std::size(BLOCK_SIZES); i++)
            if (route_pso_[i] != nil && BLOCK_SIZES[i] <= threads)
                pick = i;
        return pick;
    }

    template <typename T> void upload_or_zero(SharedBuf<T> &buf, const T *src, size_t n)
    {
        if (src != nullptr) {
            buf.upload(dev_, src, n);
        } else {
            buf.alloc(dev_, n);
            std::memset(buf.ptr(), 0, std::max<size_t>(n, 1) * sizeof(T));
        }
    }

    void build_pipeline()
    {
        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        // Same IEEE single-precision results as the host backend: no fast
        // math (the kernel also turns off contraction into FMA)
        if (@available(macOS 15.0, *)) {
            opts.mathMode = MTLMathModeSafe;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSString *src = [NSString stringWithUTF8String:gpuroute_metal_source];
        id<MTLLibrary> lib = [dev_ newLibraryWithSource:src options:opts error:&err];
        if (lib == nil)
            throw std::runtime_error("Metal kernel compilation failed: " + ns_error(err));
        for (int i = 0; i < (int)std::size(BLOCK_SIZES); i++) {
            const int block = BLOCK_SIZES[i];
            MTLFunctionConstantValues *consts = [MTLFunctionConstantValues new];
            [consts setConstantValue:&block type:MTLDataTypeInt atIndex:0];
            id<MTLFunction> fn = [lib newFunctionWithName:@"route_kernel" constantValues:consts error:&err];
            if (fn == nil)
                throw std::runtime_error("Metal library has no route_kernel: " + ns_error(err));
            id<MTLComputePipelineState> pso = [dev_ newComputePipelineStateWithFunction:fn error:&err];
            if (pso == nil)
                throw std::runtime_error("Metal pipeline creation failed: " + ns_error(err));
            if ([pso maxTotalThreadsPerThreadgroup] >= (NSUInteger)block)
                route_pso_[i] = pso;
            else if (i == 0)
                throw std::runtime_error("Metal route kernel supports only " +
                                         std::to_string((unsigned long)[pso maxTotalThreadsPerThreadgroup]) +
                                         " threads per threadgroup, needs " + std::to_string(block));
        }
        queue_ = [dev_ newCommandQueue];
        if (queue_ == nil)
            throw std::runtime_error("Metal command queue creation failed");
        if (@available(macOS 15.0, *)) {
            MTLResidencySetDescriptor *desc = [MTLResidencySetDescriptor new];
            desc.label = @"gpuroute";
            id<MTLResidencySet> set = [dev_ newResidencySetWithDescriptor:desc error:&err];
            if (set == nil)
                throw std::runtime_error("Metal residency set creation failed: " + ns_error(err));
            [queue_ addResidencySet:set];
            residency_ = set;
        }
    }

    id<MTLDevice> dev_;
    id<MTLComputePipelineState> route_pso_[std::size(BLOCK_SIZES)] = {};
    id<MTLCommandQueue> queue_ = nil;
    id residency_ = nil; // id<MTLResidencySet> on macOS 15 and later
    uint64_t residency_generation_ = ~uint64_t(0);
    std::vector<id<MTLBuffer>> resident_; // the buffers in residency_
    int device_;
    std::string name_;
    std::string device_name_;
    int32_t n_wires_ = 0;
    SharedBuf<int32_t> out_off_, out_dst_, edge_src_;
    SharedBuf<float> edge_cost_;
    SharedBuf<uint8_t> edge_region_;
    SharedBuf<uint8_t> edge_outside_halo_;
    SharedBuf<WireRec> wires_;
    Lane small_, large_;
    SharedBuf<TaskDesc> tasks_;
    SharedBuf<ArcDesc> arcs_;
    SharedBuf<int32_t> seeds_;
    SharedBuf<float> seed_delay_, seed_load_;
    SharedBuf<int32_t> shell_tiles_;
    SharedBuf<ArcResult> results_;
    SharedBuf<PathEntry> paths_;
    BackendStats stats_;
    int large_slots_ = 0;
    int large_bits_ = 22;
    bool auto_large_slots_ = false;
};

std::vector<id<MTLDevice>> metal_devices()
{
    std::vector<id<MTLDevice>> result;
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        for (id<MTLDevice> dev in all)
            result.push_back(dev);
    }
    return result;
}

} // namespace

bool device_backend_available() { return !metal_devices().empty(); }

std::string describe_devices()
{
    std::string s;
    const auto devices = metal_devices();
    for (size_t i = 0; i < devices.size(); i++) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s%zu: %s (%zu MB working set)", i ? "; " : "", i,
                      device_label(devices[i]).c_str(),
                      (size_t)([devices[i] recommendedMaxWorkingSetSize] >> 20));
        s += buf;
    }
    return s;
}

std::unique_ptr<Backend> create_device_backend(int device, std::string &error)
{
    const auto devices = metal_devices();
    if (devices.empty()) {
        error = "no GPU device available (no Metal device)";
        return nullptr;
    }
    if (device < 0) {
        // Metal reports no compute unit count; take the device with the
        // largest working set, which on Apple silicon is the only one
        device = 0;
        for (size_t i = 1; i < devices.size(); i++)
            if ([devices[i] recommendedMaxWorkingSetSize] > [devices[device] recommendedMaxWorkingSetSize])
                device = int(i);
    }
    if (device >= int(devices.size())) {
        error = "requested GPU device index out of range";
        return nullptr;
    }
    return std::unique_ptr<Backend>(new MetalBackend(devices[device], device));
}

} // namespace gpuroute
