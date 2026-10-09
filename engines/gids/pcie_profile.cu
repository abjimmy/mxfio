/* SPDX-License-Identifier: GPL-2.0 */
#include "internal.hpp"
#include "profile_device.cuh"
#include "csrc/data_paths/local_nvme/io/nvme_submit_primitives.cuh"
#include "csrc/data_paths/local_nvme/io/prp_builder.h"

namespace tnv=tutti::data_paths::local_nvme;
__global__ static void pcie_profile(tnv::DeviceTargetHandle *target,
        const tnv::AddressDescriptor *desc,const Request *request,Result *result,
        void *data,const void *reference,ApSample *sample,unsigned budget) {
    // Upstream per-entry timing slots: [0] entry, [1] command published,
    // [2] CQE observed, [3] completion slot reclaimed (after cq_dequeue +
    // put_cid). Tutti returns nanoseconds; the AP hooks and CSV use raw
    // realtime ticks and calibrate ticks/us. Undo the port's scale before
    // combining timestamps from the two sources.
    __shared__ unsigned long long timing[4];
    timing[0]=timing[1]=timing[2]=timing[3]=0;
    ap_begin(data,reference,request->bytes,request->write);
    if(request->write) {
        tnv::submit_write_one(target,desc->prp1,desc->prp2,request->offset,desc->data_length,
            reinterpret_cast<tnv::EntryCompletionStatus *>(result),budget,0,timing);
        ap_state().cqe_observed=timing[2]/TUTTI_MACA_REALTIME_NS_PER_TICK;
        ap_state().slot_reclaimed=timing[3]/TUTTI_MACA_REALTIME_NS_PER_TICK;
        ap_finish_write(sample,result->result);
    } else {
        tnv::submit_read_one(target,desc->prp1,desc->prp2,request->offset,desc->data_length,
            reinterpret_cast<tnv::EntryCompletionStatus *>(result),budget,0,timing);
        ap_state().cqe_observed=timing[2]/TUTTI_MACA_REALTIME_NS_PER_TICK;
        ap_state().slot_reclaimed=timing[3]/TUTTI_MACA_REALTIME_NS_PER_TICK;
        ap_consume(data,reference,request->bytes,sample,&result->result);
    }
}
cudaError_t launch_pcie_profile(void *target,const void *desc,const Request *r,
        Result *s,void *data,const void *reference,ApSample *sample,unsigned budget,
        cudaStream_t stream) {
    pcie_profile<<<1,1,sizeof(ApSample),stream>>>(
        (tnv::DeviceTargetHandle *)target,(const tnv::AddressDescriptor *)desc,
        r,s,data,reference,sample,budget);
    return cudaGetLastError();
}
