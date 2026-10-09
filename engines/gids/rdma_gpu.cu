/* SPDX-License-Identifier: GPL-2.0 */
#include "rdma_gpu.cuh"
#include "profile_device.cuh"
#include "batch_pump.hpp"

__device__ static void store32(uint8_t *p,unsigned off,uint32_t v) {
    *reinterpret_cast<volatile uint32_t *>(p+off)=be32(v);
}
__device__ static void store64(uint8_t *p,unsigned off,uint64_t v) {
    *reinterpret_cast<volatile uint64_t *>(p+off)=be64(v);
}
template<bool Profile>
__global__ static void rdma_submit(GpuRdmaQueue *queues,const Request *requests,
                                   Result *results,unsigned count,uint32_t nsid,
                                   uint32_t block_size,uint32_t budget,
                                   const void *reference=nullptr,ApSample *sample=nullptr) {
    unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=count) return;
    GpuRdmaQueue &q=queues[i];
    Request r=requests[i];
    const uint16_t cid=uint16_t(q.sq_pi);
    if constexpr(Profile) { ap_begin((void *)q.data,reference,r.bytes,r.write); ap_state().sq_begin=ap_clock(); }
    *q.command=rw_capsule(cid,nsid,r.offset/block_size,r.bytes/block_size,
                         r.write,q.data,r.bytes,q.data_rkey);

    // mlx5 receive WQE is one data segment (byte_count, lkey, address).
    uint8_t *rq=q.rq.at((q.rq_pi&(q.rq_count-1))*q.rq_stride);
    store32(rq,0,16); store32(rq,4,q.lkey); store64(rq,8,(uintptr_t)q.response);
    __threadfence_system();
    *reinterpret_cast<volatile uint32_t *>(q.db)=be32(++q.rq_pi);

    uint8_t *sq=q.sq.at((q.sq_pi&(q.sq_count-1))*64);
    for(unsigned k=0;k<16;++k) reinterpret_cast<volatile uint32_t *>(sq)[k]=0;
    store32(sq,0,((q.sq_pi&0xffff)<<8)|0x0a); /* MLX5_OPCODE_SEND */
    store32(sq,4,(q.qpn<<8)|2);              /* control + data, 2 segments */
    sq[11]=8;                               /* MLX5_WQE_CTRL_CQ_UPDATE */
    store32(sq,16,64); store32(sq,20,q.lkey); store64(sq,24,(uintptr_t)q.command);
    __threadfence_system();
    reinterpret_cast<volatile uint32_t *>(q.db)[1]=be32(++q.sq_pi);
    __threadfence_system();
    // A single 64-bit MMIO doorbell write; no multi-store BlueFlame copy.
    *reinterpret_cast<volatile uint64_t *>(q.uar)=*reinterpret_cast<volatile uint64_t *>(sq);
    if constexpr(Profile) ap_state().doorbell=ap_clock();
    __threadfence_system();

    bool sent=false,received=false;
    for(uint32_t poll=0;poll<budget && (!sent || !received);++poll) {
        uint8_t *c=q.cq.at((q.cq_ci&(q.cq_count-1))*q.cqe_size+q.cqe_size-64);
        uint8_t own=*reinterpret_cast<volatile uint8_t *>(c+63);
        if((own&1)!=((q.cq_ci/q.cq_count)&1) || (own>>4)==15) continue;
        __threadfence_system();
        uint8_t opcode=own>>4;
        ++q.cq_ci;
        *reinterpret_cast<volatile uint32_t *>(q.cq_db)=be32(q.cq_ci&0xffffff);
        if(opcode==0) sent=true;
        else if(opcode==2) {
            if constexpr(Profile) ap_state().cq_seen=ap_clock();
            received=true;
        }
        else {
            results[i]={4,uint32_t(opcode)};
            return;
        }
    }
    if(!sent || !received) { results[i]={2,0}; return; }
    __threadfence_system();
    volatile uint8_t *response=q.response;
    uint16_t completed=uint16_t(response[12])|(uint16_t(response[13])<<8);
    uint16_t status=uint16_t(response[14])|(uint16_t(response[15])<<8);
    if(completed!=cid) { results[i]={4,completed}; return; }
    results[i]={uint32_t((status>>1)?3:0),uint32_t(status)};
    if constexpr(Profile) {
        if(r.write) ap_finish_write(sample,results[i].result);
        else ap_consume((void *)q.data,reference,r.bytes,sample,&results[i].result);
    }
}
namespace {
struct RdmaBatch {
    GpuRdmaQueue &q;
    const Request *requests;
    Result *results;
    unsigned count, nsid, block_size;
    RdmaBatchTracker tracker;
    uint32_t last_error=0;

    __device__ bool can_submit() const { return true; } // pump bounds all WRs by q.window
    __device__ bool submit(unsigned slot) {
        const auto &r=requests[slot];
        q.command[slot]=rw_capsule(uint16_t(slot),nsid,r.offset/block_size,
            r.bytes/block_size,r.write,q.data+slot*q.stride,r.bytes,q.data_rkey);
        // Responses land in RQ posting order; their embedded CID identifies
        // the NVMe request, which may complete out of submission order.
        unsigned recv_slot=q.rq_pi&(q.rq_count-1);
        uint8_t *rq=q.rq.at(recv_slot*q.rq_stride);
        store32(rq,0,16); store32(rq,4,q.lkey);
        store64(rq,8,(uintptr_t)(q.response+recv_slot*16));
        __threadfence_system();
        *reinterpret_cast<volatile uint32_t *>(q.db)=be32(++q.rq_pi);

        uint8_t *sq=q.sq.at((q.sq_pi&(q.sq_count-1))*64);
        for (unsigned k=0;k<16;++k) reinterpret_cast<volatile uint32_t *>(sq)[k]=0;
        store32(sq,0,((q.sq_pi&0xffff)<<8)|0x0a);
        store32(sq,4,(q.qpn<<8)|2);
        *reinterpret_cast<volatile uint8_t *>(sq+11)=8; // signal every SEND
        store32(sq,16,64); store32(sq,20,q.lkey);
        store64(sq,24,(uintptr_t)(q.command+slot));
        __threadfence_system();
        reinterpret_cast<volatile uint32_t *>(q.db)[1]=be32(++q.sq_pi);
        __threadfence_system();
        *reinterpret_cast<volatile uint64_t *>(q.uar)=*reinterpret_cast<volatile uint64_t *>(sq);
        __threadfence_system();
        return true;
    }
    __device__ int poll(unsigned issued) {
        volatile uint8_t *c=q.cq.at((q.cq_ci&(q.cq_count-1))*q.cqe_size+q.cqe_size-64);
        const uint8_t own=c[63], opcode=own>>4;
        if ((own&1)!=((q.cq_ci/q.cq_count)&1) || opcode==15) return 0;
        __threadfence_system();
        uint16_t wqe=(uint16_t(c[60])<<8)|c[61];
        unsigned cid=0;
        int rc=-1;
        if (opcode==0) rc=tracker.sent(wqe,issued,cid);
        else if (opcode==2) {
            uint32_t bytes=be32(*reinterpret_cast<volatile uint32_t *>(c+44));
            if (bytes!=16) { last_error=bytes; return -1; }
            volatile uint8_t *p=q.response+(wqe&(q.rq_count-1))*16;
            cid=uint16_t(p[12])|(uint16_t(p[13])<<8);
            uint16_t status=uint16_t(p[14])|(uint16_t(p[15])<<8);
            rc=tracker.received(uint16_t(cid),status,issued);
        }
        if (rc<0) { last_error=(uint32_t(opcode)<<24)|wqe; return -1; }
        if (rc==1) {
            uint16_t status=q.slots[cid].status;
            results[cid]={uint32_t((status>>1)?3:0),status};
        }
        __threadfence_system();
        ++q.cq_ci;
        *reinterpret_cast<volatile uint32_t *>(q.cq_db)=be32(q.cq_ci&0xffffff);
        return rc;
    }
    __device__ void wait() { __threadfence_system(); }
    __device__ void finish() { __threadfence_system(); }
    __device__ void fail(unsigned error) {
        for (unsigned i=0;i<count;++i)
            if (results[i].result==UINT32_MAX) results[i]={error,last_error};
        __threadfence_system();
    }
};
__global__ void rdma_batch(GpuRdmaQueue *q,const Request *r,Result *s,
                           unsigned count,unsigned nsid,unsigned bs,unsigned budget) {
    for (unsigned i=0;i<count;++i) q->slots[i]={0,0};
    RdmaBatch batch{*q,r,s,count,nsid,bs,{q->slots,uint16_t(q->sq_pi)}};
    gids_pump_batch(batch,count,q->window,budget);
}
}
cudaError_t launch_rdma(GpuRdmaQueue *q,const Request *r,Result *s,unsigned count,
                       uint32_t nsid,uint32_t bs,uint32_t budget,cudaStream_t stream) {
    // One issuer per job/QP. Separate kernels also keep shared-UAR doorbell
    // writes out of the same warp's potentially coalesced store instruction.
    rdma_batch<<<1,1,0,stream>>>(q,r,s,count,nsid,bs,budget);
    return cudaGetLastError();
}
cudaError_t launch_rdma_profile(GpuRdmaQueue *q,const Request *r,Result *s,
        const void *reference,ApSample *sample,uint32_t nsid,uint32_t bs,
        uint32_t budget,cudaStream_t stream) {
    rdma_submit<true><<<1,1,sizeof(ApSample),stream>>>(q,r,s,1,nsid,bs,budget,reference,sample);
    return cudaGetLastError();
}
