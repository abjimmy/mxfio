/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#if defined(__CUDACC__)
#define GIDS_WORKER_DEVICE __device__ __forceinline__
#else
#define GIDS_WORKER_DEVICE inline
#endif

/* Every worker takes one terminal ticket after finishing its last request.
 * The final terminal ticket therefore proves all I/O has left execute().
 * Reset on the device, without an extra host memset/launch per batch. */
template<class Worker>
GIDS_WORKER_DEVICE void gids_run_worker(Worker &w,unsigned count,unsigned workers)
{
    for (;;) {
        unsigned slot=w.take();
        if (slot>=count) {
            if (slot==count+workers-1) w.reset();
            return;
        }
        if (w.aborted()) w.cancel(slot);
        else if (!w.execute(slot)) w.abort();
    }
}
#undef GIDS_WORKER_DEVICE
