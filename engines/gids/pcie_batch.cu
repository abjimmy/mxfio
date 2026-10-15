/* SPDX-License-Identifier: GPL-2.0 */
#include "internal.hpp"
#include "batch_pump.hpp"
#include "worker_pool.hpp"
#include "csrc/data_paths/local_nvme/io/nvme_submit_primitives.cuh"
#include "csrc/data_paths/local_nvme/io/prp_builder.h"

namespace tnv = tutti::data_paths::local_nvme;

namespace {
/* Exactly one GPU lane owns each SQ and CQ. Stage SQEs before ringing
 * the doorbell, and reclaim a run of CQEs with one CQ doorbell. Tutti's
 * multi-producer locks are unnecessary for this exclusive owner. Keep its
 * ticket counters consistent so a drained QP can also run the QD1 profiler. */
struct Batch {
    const tnv::DeviceTargetHandle *target;
    const tnv::AddressDescriptor *descriptors;
    const Request *requests;
    Result *results;
    QueuePair *qp;
    unsigned count;
    unsigned sq_head, sq_tail, cq_head, published_tail, published_head;
    const unsigned *indices;
    unsigned first;

    __device__ unsigned global_slot(unsigned local) const {
        return indices ? indices[first+local] : local;
    }

    __device__ void flush_completions() {
        if (published_head == cq_head) return;
        qp->sq.head.store(sq_head, cuda::memory_order_release);
        qp->cq.head.store(cq_head, cuda::memory_order_release);
        __threadfence_system();
        *qp->cq.db = cq_head & qp->cq.qs_minus_1;
        published_head = cq_head;
    }

    __device__ void flush_commands() {
        if (published_tail == sq_tail) return;
        qp->sq.in_ticket.store(sq_tail, cuda::memory_order_relaxed);
        qp->sq.tail.store(sq_tail, cuda::memory_order_release);
        /* All command words and PRPs must be visible before the SSD sees
         * the new tail. The doorbell covers every staged command. */
        __threadfence_system();
        *qp->sq.db = sq_tail & qp->sq.qs_minus_1;
        __threadfence_system();
        published_tail = sq_tail;
    }

    __device__ bool can_submit() const {
        return sq_tail - sq_head < qp->sq.qs_minus_1;
    }

    __device__ bool submit(unsigned slot) {
        const auto &r = requests[global_slot(slot)];
        const auto &d = descriptors[global_slot(slot)];
        uint64_t lba = 0, blocks = 0;
        if (!tnv::resolve_lba(target, r.offset, d.data_length,
                             &lba, &blocks, 0)) return false;
        nvm_cmd_t cmd;
        nvm_cmd_clear(&cmd);
        /* Slots are unique 16-bit command IDs for this drained batch.
         * No other producer, including the QD1 profiler, runs concurrently.
         * A failed batch is never reused. */
        nvm_cmd_header(&cmd, uint16_t(slot), r.write ? 1 : 2, qp->nvmNamespace);
        nvm_cmd_data_ptr(&cmd, d.prp1, d.prp2);
        nvm_cmd_rw_blks(&cmd, lba, blocks);
        flush_completions();
        const unsigned pos = sq_tail & qp->sq.qs_minus_1;
        auto *sq = static_cast<volatile nvm_cmd_t *>(qp->sq.vaddr);
        for (unsigned word = 0; word < 16; ++word)
            sq[pos].dword[word] = cmd.dword[word];
        qp->sq.tickets[pos].val.store(get_id(sq_tail, qp->sq.qs_log2) + 1,
                                      cuda::memory_order_relaxed);
        ++sq_tail;
        return true;
    }

    __device__ int poll(unsigned issued) {
        flush_commands();
        const unsigned pos = cq_head & qp->cq.qs_minus_1;
        const auto *cq = static_cast<volatile nvm_cpl_t *>(qp->cq.vaddr);
        const uint32_t raw = cq[pos].dword[3];
        const unsigned phase = (~(cq_head >> qp->cq.qs_log2)) & 1;
        if (((raw >> 16) & 1) != phase) { flush_completions(); return 0; }
        __threadfence_system();
        const unsigned slot = raw & 0xffff;
        if (slot >= issued || results[global_slot(slot)].result != UINT32_MAX) return -1;
        const unsigned device_head = cq[pos].dword[2] & 0xffff;
        if (device_head > qp->sq.qs_minus_1) return -1;
        const unsigned retired = (device_head - sq_head) & qp->sq.qs_minus_1;
        if (retired > published_tail - sq_head) return -1;
        for (unsigned i = 0; i < retired; ++i, ++sq_head) {
            const unsigned sq_pos = sq_head & qp->sq.qs_minus_1;
            qp->sq.tickets[sq_pos].val.store(get_id(sq_head, qp->sq.qs_log2) + 2,
                                            cuda::memory_order_relaxed);
        }
        ++cq_head;
        results[global_slot(slot)] = { unsigned((raw >> 17) ? 3 : 0), raw };
        return 1;
    }

    __device__ void wait() { __threadfence_system(); }
    __device__ void finish() { flush_completions(); __threadfence_system(); }
    __device__ void fail(unsigned error) {
        for (unsigned i = 0; i < count; ++i)
            if (results[global_slot(i)].result == UINT32_MAX)
                results[global_slot(i)] = { error, 0 };
        __threadfence_system();
    }
};

__global__ void pcie_batch(const tnv::DeviceTargetHandle *target,
                           const tnv::AddressDescriptor *descriptors,
                           const Request *requests, Result *results,
                           unsigned count, unsigned window, unsigned budget)
{
    auto *qp = target->d_qps;
    const unsigned sq_head = qp->sq.head.load(cuda::memory_order_acquire);
    const unsigned sq_tail = qp->sq.tail.load(cuda::memory_order_relaxed);
    const unsigned cq_head = qp->cq.head.load(cuda::memory_order_relaxed);
    Batch batch{ target, descriptors, requests, results, qp, count,
                 sq_head, sq_tail, cq_head, sq_tail, cq_head, nullptr, 0 };
    gids_pump_batch(batch, count, window, budget);
}

/* Queue owners from every SSD run in ONE kernel on ONE job stream. The
 * host partitions fio slots; each owner exclusively pumps its SQ/CQ. */
__global__ void pcie_parallel_batch(PcieLaunch p) {
    unsigned tid=blockIdx.x*blockDim.x+threadIdx.x;
    if (tid>=p.range_count) return;
    const auto r=p.ranges[tid];
    const auto *table=static_cast<const tnv::DeviceTargetHandle *const *>(p.targets);
    const auto *target=table[r.device];
    auto *qp=target->d_qps+r.queue;
    unsigned head=qp->sq.head.load(cuda::memory_order_acquire);
    unsigned tail=qp->sq.tail.load(cuda::memory_order_relaxed);
    unsigned cq=qp->cq.head.load(cuda::memory_order_relaxed);
    Batch b{target,static_cast<const tnv::AddressDescriptor *>(p.descriptors),
            p.requests,p.results,qp,r.count,head,tail,cq,tail,cq,p.indices,r.first};
    gids_pump_batch(b,r.count,r.window,p.budget);
}

/* Tutti's one synchronous command per worker, dynamic task cursor and
 * global-thread-index QP mapping. Shared QPs use the original Tutti
 * submission/completion primitives, not the exclusive-owner pump. */
struct PcieWorker {
    PcieLaunch p;
    const tnv::DeviceTargetHandle *const *table;
    const tnv::AddressDescriptor *desc;
    __device__ unsigned take() { return atomicAdd(p.cursor,1u); }
    __device__ bool aborted() { return atomicAdd(p.cursor+1,0u)!=0; }
    __device__ void abort() { atomicExch(p.cursor+1,1u); }
    __device__ void cancel(unsigned slot) { p.results[slot]={4,0}; }
    __device__ void reset() {
        atomicExch(p.cursor+1,0u);
        atomicExch(p.cursor,0u);
    }
    __device__ bool execute(unsigned slot) {
        const auto &r=p.requests[slot];
        const auto &d=desc[slot];
        const auto *target=table[p.devices[slot]];
        tnv::EntryCompletionStatus status{};
        if (r.write)
            tnv::submit_write_one(target,d.prp1,d.prp2,r.offset,d.data_length,&status,p.budget,0);
        else
            tnv::submit_read_one(target,d.prp1,d.prp2,r.offset,d.data_length,&status,p.budget,0);
        p.results[slot]={status.result,status.nvme_status_dword3};
        return status.result==0;
    }
};
__global__ void pcie_workers(PcieLaunch p) {
    unsigned tid=blockIdx.x*blockDim.x+threadIdx.x;
    if (tid>=p.workers) return;
    PcieWorker worker{p,static_cast<const tnv::DeviceTargetHandle *const *>(p.targets),
                        static_cast<const tnv::AddressDescriptor *>(p.descriptors)};
    gids_run_worker(worker,p.count,p.workers);
}
}

cudaError_t launch_pcie_batch(const void *target, const void *descriptors,
                             const Request *requests, Result *results, unsigned count,
                             unsigned window, unsigned budget, cudaStream_t stream)
{
    if (!count) return cudaErrorInvalidValue;
    pcie_batch<<<1, 1, 0, stream>>>(
        static_cast<const tnv::DeviceTargetHandle *>(target),
        static_cast<const tnv::AddressDescriptor *>(descriptors), requests, results,
        count, window, budget);
    return cudaGetLastError();
}

cudaError_t launch_pcie_parallel(const PcieLaunch &p,cudaStream_t stream) {
    if (!p.count || (!p.workers && !p.range_count) || !p.block_threads) return cudaErrorInvalidValue;
    const unsigned block=p.block_threads;
    if (p.workers) {
        unsigned threads=p.workers<block ? p.workers : block;
        pcie_workers<<<(p.workers+threads-1)/threads,threads,0,stream>>>(p);
    } else {
        unsigned threads=p.range_count<block ? p.range_count : block;
        pcie_parallel_batch<<<(p.range_count+threads-1)/threads,threads,0,stream>>>(p);
    }
    return cudaGetLastError();
}
