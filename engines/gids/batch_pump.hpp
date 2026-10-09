/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <cstdint>

#if defined(__CUDACC__)
#define GIDS_BATCH_DEVICE __device__ __forceinline__
#else
#define GIDS_BATCH_DEVICE inline
#endif

/* One owner pumps a QP. A batch may exceed the hardware ring capacity:
 * only window requests are issued before a completion returns a credit.
 * Transport::poll consumes the CQ in arrival order, not submission order:
 * 0 = idle, 1 = one request completed, 2 = progress without completion
 * (e.g. a SEND completion awaiting its NVMe response), negative = error.
 * Transport::fail marks only unfinished/unsubmitted entries; a timeout
 * poisons the adapter, which destroys the QP before releasing DMA memory. */
template<class Transport>
GIDS_BATCH_DEVICE void gids_pump_batch(Transport &transport, unsigned count,
                                      unsigned window, unsigned poll_budget)
{
    if (!count) return;
    if (!window || !poll_budget) { transport.fail(4); return; }
    unsigned issued = 0, completed = 0, idle = 0;
    while (completed < count) {
        bool progress = false;
        while (issued < count && issued - completed < window &&
               transport.can_submit()) {
            if (!transport.submit(issued)) { transport.fail(4); return; }
            ++issued;
            progress = true;
        }
        /* Drain ready completions, including out-of-order command IDs,
         * before refilling the outstanding window. */
        while (completed < issued) {
            const int rc = transport.poll(issued);
            if (rc < 0) { transport.fail(4); return; }
            if (!rc) break;
            if (rc == 1) ++completed;
            progress = true;
        }
        if (progress) idle = 0;
        else if (++idle >= poll_budget) { transport.fail(2); return; }
        if (!progress) transport.wait();
    }
    transport.finish();
}

#undef GIDS_BATCH_DEVICE
