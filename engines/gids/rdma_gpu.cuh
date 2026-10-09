/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "internal.hpp"
#include "wire.h"
#include "rdma_batch.hpp"
/* One fio job exclusively owns one RC QP. After Fabrics Connect completes,
 * GPU owns its SQ, RQ and CQ; no host ibv_post_send/poll_cq in the data path. */
struct GpuQueuePages {
    uint8_t *pages[16];
    uint32_t offset, shift;
#ifdef __CUDACC__
    __device__ uint8_t *at(uint32_t byte) const {
        byte += offset;
        return pages[byte >> shift] + (byte & ((1u << shift) - 1));
    }
#endif
};
struct GpuRdmaQueue {
    GpuQueuePages sq, rq, cq;
    uint32_t *db, *cq_db;
    uint64_t *uar;
    Capsule *command;
    uint8_t *response;
    uint64_t data, stride;
    RdmaSlot *slots;
    uint32_t window;
    uint32_t qpn, sq_count, rq_count, cq_count, rq_stride, cqe_size;
    uint32_t sq_pi, rq_pi, cq_ci, lkey, rkey, data_rkey;
};
cudaError_t launch_rdma(GpuRdmaQueue *, const Request *, Result *, unsigned,
                       uint32_t, uint32_t, uint32_t, cudaStream_t);
cudaError_t launch_rdma_profile(GpuRdmaQueue *,const Request *,Result *,
    const void *,ApSample *,uint32_t,uint32_t,uint32_t,cudaStream_t);
