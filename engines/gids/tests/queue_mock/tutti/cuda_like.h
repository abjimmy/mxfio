/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <cuda/atomic>
#include <cassert>
#include <cstdint>
#include <functional>

#define __device__
#define __host__
#define __forceinline__ inline
#define __align__(n) __attribute__((aligned(n)))

struct alignas(32) ulonglong4 { unsigned long long x, y, z, w; };
struct mock_dim3 { unsigned x, y, z; };
inline mock_dim3 threadIdx{0, 0, 0}, blockIdx{0, 0, 0}, blockDim{1, 1, 1};
inline constexpr unsigned warpSize = 32;
inline unsigned __lane_id() { return threadIdx.x % warpSize; }

namespace queue_mock {
inline unsigned sleeps = 0;
inline std::function<void()> on_wait;
inline void reset() { sleeps = 0; on_wait = {}; }
}
inline void __nanosleep(unsigned) {
    /* Deterministic interleavings let one logical worker complete while
     * another is waiting, without C++ data races on volatile CQ memory. */
    assert(++queue_mock::sleeps < 1000000);
    auto run = queue_mock::on_wait;
    if (run) run();
}
inline void __threadfence_system() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
