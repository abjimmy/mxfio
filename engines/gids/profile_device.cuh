/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "profile_types.h"
#include <cuda_runtime.h>

// One lane per block. Shared state belongs to this kernel instance, so
// profiling jobs never communicate through a device-global trace pointer.
__device__ __forceinline__ ApSample &ap_state() {
    extern __shared__ ApSample ap_shared[];
    return ap_shared[0];
}
__device__ __forceinline__ uint64_t ap_clock() {
    asm volatile("" ::: "memory");
    // MACA clock64() tracks the variable XCORE clock. This SDK intrinsic
    // reads the realtime counter instead; its rate is still measured.
    uint64_t t=__builtin_mxc_get_realtime();
    asm volatile("" ::: "memory");
    return t;
}
__device__ inline void ap_begin(void *data,const void *reference,unsigned bytes,bool write=false) {
    ap_state()={};
    const volatile uint64_t *ref=(const volatile uint64_t *)reference;
    volatile uint64_t *dst=(volatile uint64_t *)data;
    ap_state().expected_first=ref[0];
    // Every byte must change on the next read. No stale successful payload
    // can satisfy the comparison, even when repeatedly reading one LBA.
    for(unsigned j=0;j<bytes/8;++j) dst[j]=write ? ref[j] : ~ref[j];
    __threadfence_system();
}
__device__ inline void ap_finish_write(ApSample *out,uint32_t status) {
    if(!status) ap_state().ap_done=ap_clock();
    *out=ap_state();
}
__device__ inline void ap_consume(void *data,const void *reference,unsigned bytes,
                                  ApSample *out,uint32_t *status) {
    auto &s=ap_state();
    if(!*status) {
        const volatile uint64_t *src=(const volatile uint64_t *)data;
        const volatile uint64_t *ref=(const volatile uint64_t *)reference;
        uint64_t first=src[0];
        // The comparison consumes the load before the end timestamp. A
        // mismatch is a failed sample, never an apparently fast completion.
        if(first==s.expected_first) s.first_read=ap_clock();
        else ++s.mismatches;
        s.observed_first=first;
        for(unsigned j=1;j<bytes/8;++j) {
            uint64_t value=src[j];
            if(value!=ref[j]) ++s.mismatches;
        }
        if(!s.mismatches) s.all_read=ap_clock();
        else *status=5;
    }
    *out=s;
}
