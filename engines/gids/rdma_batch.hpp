/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <cstdint>
#if defined(__CUDACC__)
#define GIDS_RDMA_HD __host__ __device__
#else
#define GIDS_RDMA_HD
#endif

struct RdmaSlot {
    uint16_t status;
    uint8_t flags; // bit 0: SEND completed; bit 1: NVMe response received
};

/* Each batch uses unique CIDs and one 64-byte SEND WQE per request. A
 * request can be released only after both independent completions arrive.
 * Return 1 for a completed request, 2 for progress, -1 for a malformed CQE. */
struct RdmaBatchTracker {
    RdmaSlot *slots;
    uint16_t first_send;
    GIDS_RDMA_HD int mark(unsigned cid, unsigned issued, uint8_t flag, uint16_t status) {
        if (cid >= issued || (slots[cid].flags & flag)) return -1;
        if (flag == 2) slots[cid].status=status;
        slots[cid].flags |= flag;
        return slots[cid].flags == 3 ? 1 : 2;
    }
    GIDS_RDMA_HD int sent(uint16_t wqe, unsigned issued, unsigned &cid) {
        cid=uint16_t(wqe-first_send);
        return mark(cid,issued,1,0);
    }
    GIDS_RDMA_HD int received(uint16_t cid, uint16_t status, unsigned issued) {
        return mark(cid,issued,2,status);
    }
};
#undef GIDS_RDMA_HD
