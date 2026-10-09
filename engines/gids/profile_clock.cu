/* SPDX-License-Identifier: GPL-2.0 */
#include "internal.hpp"
#include "profile_device.cuh"
#include <chrono>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cstdio>

struct ClockGate { volatile uint32_t ready, stop; uint64_t ticks, overhead; };
__global__ static void calibrate(ClockGate *gate) {
    uint64_t overhead=~uint64_t(0);
    for(unsigned i=0;i<128;++i) {
        uint64_t a=ap_clock(), b=ap_clock();
        if(b-a<overhead) overhead=b-a;
    }
    uint64_t start=ap_clock();
    gate->ready=1;
    __threadfence_system();
    // Bounded even if the host is interrupted. A single polling lane, QD0.
    unsigned i=0;
    for(;i<100000000 && !gate->stop;++i) __threadfence_system();
    gate->ticks=ap_clock()-start;
    gate->overhead=overhead;
    gate->ready=(i==100000000)?3:2;
    __threadfence_system();
}
ApCalibration calibrate_ap_clock(cudaStream_t stream) {
    ClockGate *host=nullptr,*dev=nullptr;
    gpu_check(cudaHostAlloc((void **)&host,sizeof(*host),cudaHostAllocMapped),"allocate clock calibration gate");
    try {
        gpu_check(cudaHostGetDevicePointer((void **)&dev,host,0),"map clock calibration gate");
        double rates[3]{};
        uint64_t overhead=0;
        for(unsigned trial=0;trial<3;++trial) {
            host->ready=0; host->stop=0;
            calibrate<<<1,1,0,stream>>>(dev);
            gpu_check(cudaGetLastError(),"launch clock calibration");
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!host->ready) {
                if(std::chrono::steady_clock::now()>deadline) {
                    host->stop=1;
                    throw std::runtime_error("GPU clock calibration did not start");
                }
                std::this_thread::yield();
            }
            auto start=std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(20+trial*20));
            auto end=std::chrono::steady_clock::now();
            host->stop=1;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            gpu_check(cudaStreamSynchronize(stream),"finish clock calibration");
            if(host->ready!=2) throw std::runtime_error("GPU clock calibration gate timed out");
            double us=std::chrono::duration<double,std::micro>(end-start).count();
            rates[trial]=host->ticks/us; overhead=host->overhead;
            std::fprintf(stderr,"gids: clock calibration %u: %.3f ticks/us (%llu ticks, %.3f us)\n",trial,rates[trial],(unsigned long long)host->ticks,us);
        }
        std::sort(rates,rates+3);
        if(rates[0]<=0 || rates[2]/rates[0]>1.02)
            throw std::runtime_error("GPU timer calibration varies by more than 2%; retry under stable load");
        ApCalibration result{rates[1],rates[0],rates[2],overhead};
        cudaFreeHost(host);
        return result;
    } catch(...) {
        host->stop=1;
        cudaStreamSynchronize(stream);
        cudaFreeHost(host);
        throw;
    }
}
