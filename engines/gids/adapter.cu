/* SPDX-License-Identifier: GPL-2.0 */
#include "internal.hpp"
#include "ap_plan.hpp"
#include "pcie_config.h"
#include "completion.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

struct gids {
    gids_options opt{};
    gids_geometry geo{};
    std::unique_ptr<Backend> backend;
    void *allocation=nullptr, *data=nullptr;
    size_t stride=0;
    Request *requests=nullptr, *d_requests=nullptr;
    Result *results=nullptr, *d_results=nullptr;
    unsigned char *staging=nullptr;
    std::vector<void *> hosts;
    cudaStream_t stream=nullptr;
    cudaEvent_t kernel_done=nullptr, done=nullptr;
    GidsCompletion completion;
    std::string error;
    unsigned count=0;
    bool poisoned=false;
    void *reference=nullptr;
    ApSample *d_ap=nullptr,*h_ap=nullptr;
    FILE *ap_file=nullptr;
    bool reference_ready=false,ap_written=false;
    uint64_t ap_offset=0,ap_sequence=0;
    unsigned ap_bytes=0;
    ApCalibration calibration{};
    ApPlan plan;
    ~gids() {
        cudaSetDevice(opt.gpu);
        // A stream event never substitutes for transport teardown: destroy
        // NVMe queues / RDMA QPs before unregistering or freeing data memory.
        if (stream) cudaStreamSynchronize(stream);
        backend.reset();
        if (ap_file) {
            try {
                if (poisoned) throw std::runtime_error("failed I/O; end calibration skipped");
                auto end=calibrate_ap_clock(stream);
                std::fprintf(ap_file,"# ticks_per_us_after=%.9f\n",end.ticks_per_us);
            } catch(const std::exception &e) {
                std::fprintf(ap_file,"# end_calibration_error=%s\n",e.what());
            }
            std::fclose(ap_file);
        }
        if (d_ap) cudaFree(d_ap);
        if (h_ap) cudaFreeHost(h_ap);
        if (reference) cudaFree(reference);
        if (done) cudaEventDestroy(done);
        if (kernel_done) cudaEventDestroy(kernel_done);
        if (stream) cudaStreamDestroy(stream);
        if (d_results) cudaFree(d_results);
        if (d_requests) cudaFree(d_requests);
        if (allocation) cudaFree(allocation);
        if (requests) cudaFreeHost(requests);
        if (results) cudaFreeHost(results);
        if (staging) cudaFreeHost(staging);
    }
};
__global__ static void fill_data(uint64_t *p, size_t words, uint64_t seed) {
    for (size_t i=blockIdx.x*blockDim.x+threadIdx.x; i<words; i+=blockDim.x*gridDim.x) {
        uint64_t x=seed ^ (uint64_t(i)*0x9e3779b97f4a7c15ull);
        x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
        x=(x^(x>>27))*0x94d049bb133111ebull;
        p[i]=x^(x>>31);
    }
    __threadfence_system();
}
__global__ static void write_reference(uint64_t *reference,unsigned words,
        uint64_t seed,uint64_t offset,uint64_t sequence) {
    for(unsigned i=blockIdx.x*blockDim.x+threadIdx.x;i<words;i+=blockDim.x*gridDim.x) {
        // Different words, addresses and requests get distinct mixed payloads.
        uint64_t x=seed ^ offset ^ (sequence*0xd1342543de82ef95ull) ^ (uint64_t(i)*0x9e3779b97f4a7c15ull);
        x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
        x=(x^(x>>27))*0x94d049bb133111ebull;
        reference[i]=x^(x>>31);
    }
    __threadfence_system();
}
__global__ static void poison_readback(uint64_t *data,const uint64_t *reference,
        unsigned words,Request *request) {
    if(!blockIdx.x && !threadIdx.x) request->write=0;
    for(unsigned i=blockIdx.x*blockDim.x+threadIdx.x;i<words;i+=blockDim.x*gridDim.x)
        data[i]=~reference[i];
    __threadfence_system();
}
__global__ static void verify_readback(const volatile uint64_t *data,
        const volatile uint64_t *reference,unsigned words,Result *result,ApSample *sample) {
    // Run in one lane: status must not race with another lane's failure update.
    if(result->result) return;
    for(unsigned i=0;i<words;++i) if(data[i]!=reference[i]) ++sample->mismatches;
    if(sample->mismatches) result->result=5;
}
static void snapshot_plan(gids *s) {
    if(s->plan.write) { s->reference_ready=true; return; }
    std::fprintf(stderr,"gids: snapshot %zu planned blocks directly to GPU before measured replay\n",s->plan.offsets.size());
    for(size_t i=0;i<s->plan.offsets.size();++i) {
        s->requests[0]={s->plan.offsets[i],unsigned(s->plan.bytes),0};
        s->backend->prepare(0,s->requests[0]);
        gpu_check(cudaMemcpyAsync(s->d_requests,s->requests,sizeof(Request),cudaMemcpyHostToDevice,s->stream),"snapshot request");
        gpu_check(cudaMemsetAsync(s->d_results,0xff,sizeof(Result),s->stream),"snapshot reset status");
        s->backend->launch(1,s->d_requests,s->d_results,s->stream);
        gpu_check(cudaMemcpyAsync(s->results,s->d_results,sizeof(Result),cudaMemcpyDeviceToHost,s->stream),"snapshot status");
        gpu_check(cudaStreamSynchronize(s->stream),"snapshot read completion");
        if(s->results[0].result) {
            s->poisoned=true;
            throw std::runtime_error("planned reference read failed at sample "+std::to_string(i));
        }
        gpu_check(cudaMemcpyAsync((char *)s->reference+i*s->plan.bytes,s->data,s->plan.bytes,cudaMemcpyDeviceToDevice,s->stream),"save planned reference");
    }
    gpu_check(cudaStreamSynchronize(s->stream),"finish reference snapshot");
    s->reference_ready=true;
}
extern "C" int gids_create(const gids_options *o, gids **out, gids_geometry *geo, char *err, size_t n) {
    *out=nullptr;
    try {
        if (const char *why=gids_parallel_config_error(o)) throw std::runtime_error(why);
        if (o->nr_files) {
            if (!o->filenames) throw std::runtime_error("missing namespace list");
            for (unsigned i=0;i<o->nr_files;++i)
                if (!o->filenames[i] || !*o->filenames[i]) throw std::runtime_error("empty namespace filename");
        }
#if !GIDS_HAVE_MLX5
        if (std::strcmp(o->transport,"rdma")==0)
            throw std::runtime_error("RDMA is disabled; rebuild with GIDS_RDMA=mlx5");
#endif
        const bool pcie=std::strcmp(o->transport,"pcie")==0;
        if (!pcie && std::strcmp(o->transport,"rdma"))
            throw std::runtime_error("transport must be pcie or rdma");
        const unsigned limit=pcie ? GIDS_PCIE_MAX_DEPTH : GIDS_RDMA_MAX_DEPTH;
        if (!o->depth || o->depth>limit || !o->max_bs || o->max_bs>16*1024*1024)
            throw std::runtime_error("iodepth must be 1.."+std::to_string(limit)+" and bs must be 1..16MiB");
        auto s=std::make_unique<gids>();
        s->opt=*o;
        if(o->ap_plan) {
            if(!o->ap_latency) throw std::runtime_error("AP plan requires AP profiling");
            s->plan=ApPlan::read(o->ap_plan);
        }
        gpu_check(cudaSetDevice(o->gpu), "select MetaX GPU");
        s->stride=(size_t(o->max_bs)+65535)&~size_t(65535);
        gpu_check(cudaMalloc(&s->allocation, s->stride*o->depth+65535), "allocate GPU data");
        s->data=reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(s->allocation)+65535)&~uintptr_t(65535));
        gpu_check(cudaStreamCreateWithFlags(&s->stream, cudaStreamNonBlocking), "create stream");
        gpu_check(cudaEventCreateWithFlags(&s->kernel_done, cudaEventDisableTiming), "create kernel event");
        gpu_check(cudaEventCreateWithFlags(&s->done, cudaEventDisableTiming), "create event");
        gpu_check(cudaMallocHost((void **)&s->requests, o->depth*sizeof(Request)), "allocate request staging");
        gpu_check(cudaMallocHost((void **)&s->results, o->depth*sizeof(Result)), "allocate completion staging");
        gpu_check(cudaMalloc((void **)&s->d_requests, o->depth*sizeof(Request)), "allocate GPU requests");
        gpu_check(cudaMalloc((void **)&s->d_results, o->depth*sizeof(Result)), "allocate GPU completions");
        if (o->host_buffers) gpu_check(cudaMallocHost((void **)&s->staging, s->stride*o->depth), "allocate fio verification staging");
        s->hosts.resize(o->depth);
        if (o->ap_latency) {
            if (o->depth!=1 || o->host_buffers) throw std::runtime_error("AP profiling requires QD1 without host payload copies");
            size_t ref_bytes=o->ap_plan && !s->plan.write ? s->plan.offsets.size()*s->plan.bytes : s->stride;
            gpu_check(cudaMalloc(&s->reference,ref_bytes),"allocate GPU reference payload");
            gpu_check(cudaMalloc((void **)&s->d_ap,sizeof(ApSample)),"allocate GPU trace");
            gpu_check(cudaMallocHost((void **)&s->h_ap,sizeof(ApSample)),"allocate trace staging");
            s->calibration=calibrate_ap_clock(s->stream);
            s->ap_file=std::fopen(o->ap_latency,"wx");
            if(!s->ap_file) throw std::runtime_error("create new AP CSV: "+std::string(strerror(errno)));
            char pci[64]{};
            gpu_check(cudaDeviceGetPCIBusId(pci,sizeof(pci),o->gpu),"get profiling GPU PCI address");
            std::fprintf(s->ap_file,"# time_source=__builtin_mxc_get_realtime\n# transport=%s\n# gpu_pci=%s\n# ticks_per_us_before=%.9f\n# calibration_min=%.9f\n# calibration_max=%.9f\n# timer_pair_ticks=%llu\n",
                o->transport,pci,s->calibration.ticks_per_us,s->calibration.min_ticks_per_us,
                s->calibration.max_ticks_per_us,(unsigned long long)s->calibration.timer_pair_ticks);
            if(o->ap_plan) std::fprintf(s->ap_file,
                "# access=planned_random\n# operation=%s\n# namespace_bytes=%llu\n# expected_samples=%zu\n# seed=%llu\n# reference_bytes=%zu\n# write_fua=0\n# write_readback=%u\n",
                s->plan.write ? "write" : "read",(unsigned long long)s->plan.capacity,
                s->plan.offsets.size(),(unsigned long long)s->plan.seed,ref_bytes,unsigned(s->plan.write));
            std::fprintf(s->ap_file,"sample,offset,bytes,status,sq_begin,doorbell,cq_seen,cqe_observed,slot_reclaimed,first_read,all_read,mismatches,ap_done\n");
        }
        fill_data<<<64,256,0,s->stream>>>((uint64_t *)s->data, s->stride*o->depth/8, o->buffer_seed);
        gpu_check(cudaGetLastError(), "GPU fill launch");
        gpu_check(cudaStreamSynchronize(s->stream), "GPU fill completion");
#if GIDS_HAVE_MLX5
        s->backend=std::string(o->transport)=="pcie" ? make_pcie() : make_rdma();
#else
        s->backend=make_pcie();
#endif
        s->backend->open(*o, s->data, s->stride, s->geo);
        s->opt.filenames=nullptr; // borrowed only during open; backend owns routing.
        if (o->max_bs>s->geo.max_transfer)
            throw std::runtime_error("bs exceeds controller/PRP maximum transfer ("+std::to_string(s->geo.max_transfer)+" bytes)");
        if(o->ap_plan) {
            s->plan.validate(s->geo.capacity,s->geo.block_size,o->max_bs);
            snapshot_plan(s.get());
        }
        *geo=s->geo;
        *out=s.release();
        return 0;
    } catch (const std::exception &e) { std::snprintf(err,n,"%s",e.what()); return EIO; }
}
extern "C" int gids_prepare(gids *s, unsigned slot, void *host, uint64_t offset, unsigned bytes, int write) {
    return gids_prepare_file(s,0,slot,host,offset,bytes,write);
}
extern "C" int gids_file_geometry(gids *s,unsigned file,gids_geometry *out) {
    if (file>=(s->opt.nr_files ? s->opt.nr_files : 1)) return EINVAL;
    try {
        *out=file ? s->backend->file_geometry(file) : s->geo;
        return 0;
    } catch (const std::exception &e) { s->error=e.what(); return EIO; }
}
extern "C" int gids_prepare_file(gids *s,unsigned file,unsigned slot,void *host,
                                 uint64_t offset,unsigned bytes,int write) {
    if (s->poisoned) return EIO;
    if (s->completion.active()) return EBUSY;
    gids_geometry geo{};
    int rc=gids_file_geometry(s,file,&geo);
    if (rc) return rc;
    if (slot>=s->opt.depth || !bytes || bytes>s->opt.max_bs || bytes>geo.max_transfer || bytes%geo.block_size ||
        offset%geo.block_size || offset>geo.capacity || bytes>geo.capacity-offset) return EINVAL;
    try {
        if (s->ap_file) {
            if(bytes%8) return EINVAL;
            if(s->opt.ap_plan) {
                if(s->ap_sequence>=s->plan.offsets.size() ||
                   offset!=s->plan.offsets[s->ap_sequence] || bytes!=s->plan.bytes || bool(write)!=s->plan.write) {
                    std::fprintf(stderr,"gids: fio request differs from AP plan at sample %llu\n",(unsigned long long)s->ap_sequence);
                    return EINVAL;
                }
                s->ap_offset=offset; s->ap_bytes=bytes;
            } else if(write) return EINVAL;
            else if(!s->ap_bytes) { s->ap_offset=offset; s->ap_bytes=bytes; }
            else if(offset!=s->ap_offset || bytes!=s->ap_bytes) {
                std::fprintf(stderr,"gids: AP latency requires a fixed block; use size=bs and loops=\n");
                return EINVAL;
            }
        }
        s->requests[slot]={offset,bytes,unsigned(write)};
        s->hosts[slot]=host;
        if (s->staging && write) std::memcpy(s->staging+slot*s->stride,host,bytes);
        s->backend->prepare_file(file,slot,s->requests[slot]);
        return 0;
    } catch (const std::exception &e) { s->error=e.what(); return EIO; }
}
extern "C" int gids_submit(gids *s, unsigned count) {
    if (s->completion.active()) return EBUSY;
    try {
        if (!count || count>s->opt.depth || s->poisoned) return EINVAL;
        s->completion.begin();
        s->count=count;
        gpu_check(cudaSetDevice(s->opt.gpu), "select GPU");
        for (unsigned i=0;i<count;++i) if (s->staging && s->requests[i].write)
            gpu_check(cudaMemcpyAsync((char *)s->data+i*s->stride,s->staging+i*s->stride,s->requests[i].bytes,cudaMemcpyHostToDevice,s->stream),"fio write buffer to GPU");
        gpu_check(cudaMemcpyAsync(s->d_requests,s->requests,count*sizeof(Request),cudaMemcpyHostToDevice,s->stream),"requests to GPU");
        gpu_check(cudaMemsetAsync(s->d_results,0xff,count*sizeof(Result),s->stream),"reset status");
        if (s->ap_file) {
            if(!s->reference_ready) {
                // One untimed read captures the fixed LBA entirely on GPU.
                s->backend->launch(1,s->d_requests,s->d_results,s->stream);
                gpu_check(cudaMemcpyAsync(s->results,s->d_results,sizeof(Result),cudaMemcpyDeviceToHost,s->stream),"reference read status");
                gpu_check(cudaStreamSynchronize(s->stream),"finish reference read");
                if(s->results[0].result) throw std::runtime_error("reference read failed");
                gpu_check(cudaMemcpyAsync(s->reference,s->data,s->ap_bytes,cudaMemcpyDeviceToDevice,s->stream),"save GPU reference payload");
                s->reference_ready=true;
            }
            s->ap_written=false;
            gpu_check(cudaMemsetAsync(s->d_ap,0,sizeof(ApSample),s->stream),"reset AP trace");
            void *reference=s->opt.ap_plan && !s->plan.write ?
                (char *)s->reference+s->ap_sequence*s->plan.bytes : s->reference;
            if(s->plan.write) {
                write_reference<<<16,256,0,s->stream>>>((uint64_t *)reference,s->ap_bytes/8,
                    s->plan.seed,s->ap_offset,s->ap_sequence);
                gpu_check(cudaGetLastError(),"write reference launch");
            }
            s->backend->profile(s->d_requests,s->d_results,s->data,reference,s->d_ap,s->stream);
            if(s->plan.write) {
                // Do not read back a failed/unfinished write. This host wait and
                // the following read/compare are outside the recorded interval.
                gpu_check(cudaMemcpyAsync(s->results,s->d_results,sizeof(Result),cudaMemcpyDeviceToHost,s->stream),"write status");
                gpu_check(cudaStreamSynchronize(s->stream),"finish profiled write");
                if(!s->results[0].result) {
                    Request read=s->requests[0]; read.write=0;
                    s->backend->prepare(0,read);
                    poison_readback<<<16,256,0,s->stream>>>((uint64_t *)s->data,(const uint64_t *)reference,s->ap_bytes/8,s->d_requests);
                    gpu_check(cudaGetLastError(),"poison write readback buffer");
                    gpu_check(cudaMemsetAsync(s->d_results,0xff,sizeof(Result),s->stream),"reset readback status");
                    s->backend->launch(1,s->d_requests,s->d_results,s->stream);
                    verify_readback<<<1,1,0,s->stream>>>((const uint64_t *)s->data,(const uint64_t *)reference,s->ap_bytes/8,s->d_results,s->d_ap);
                    gpu_check(cudaGetLastError(),"verify write readback launch");
                }
            }
        } else s->backend->launch(count,s->d_requests,s->d_results,s->stream);
        // Poll this event before enqueuing any final D2H work. Each fio job
        // retains its own stream and can return independently of slower jobs.
        gpu_check(cudaEventRecord(s->kernel_done,s->stream),"record kernel event");
        return 0;
    } catch (const std::exception &e) { s->completion.fail(); s->error=e.what(); s->poisoned=true; return EIO; }
}
static bool event_ready(cudaEvent_t event) {
    cudaError_t e=cudaEventQuery(event);
    if (e==cudaErrorNotReady) return false;
    gpu_check(e,"query batch event");
    return true;
}
static void finish_batch(gids *s) {
    if(s->ap_file && !s->ap_written) {
        const auto &t=*s->h_ap;
        std::fprintf(s->ap_file,"%llu,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu\n",
            (unsigned long long)s->ap_sequence++,(unsigned long long)s->ap_offset,s->ap_bytes,
            s->results[0].result,(unsigned long long)t.sq_begin,(unsigned long long)t.doorbell,
            (unsigned long long)t.cq_seen,(unsigned long long)t.cqe_observed,
            (unsigned long long)t.slot_reclaimed,(unsigned long long)t.first_read,
            (unsigned long long)t.all_read,t.mismatches,(unsigned long long)t.ap_done);
        s->ap_written=true;
    }
    for (unsigned i=0;i<s->count;++i) {
        if (s->results[i].result==2 || s->results[i].result==4) s->poisoned=true;
        if (s->staging && !s->requests[i].write && !s->results[i].result)
            std::memcpy(s->hosts[i],s->staging+i*s->stride,s->requests[i].bytes);
    }
}
extern "C" int gids_poll(gids *s) {
    if (s->poisoned) return -EIO;
    try {
        gpu_check(cudaSetDevice(s->opt.gpu),"select completion GPU");
        return s->completion.poll(
            [&] { return event_ready(s->kernel_done); },
            [&] {
                if (s->ap_file)
                    gpu_check(cudaMemcpyAsync(s->h_ap,s->d_ap,sizeof(ApSample),cudaMemcpyDeviceToHost,s->stream),"AP timestamps to host");
                gpu_check(cudaMemcpyAsync(s->results,s->d_results,s->count*sizeof(Result),cudaMemcpyDeviceToHost,s->stream),"completion status to host");
                for (unsigned i=0;i<s->count;++i) if (s->staging && !s->requests[i].write)
                    gpu_check(cudaMemcpyAsync(s->staging+i*s->stride,(char *)s->data+i*s->stride,s->requests[i].bytes,cudaMemcpyDeviceToHost,s->stream),"GPU read buffer for fio verification");
                gpu_check(cudaEventRecord(s->done,s->stream),"record copy event");
            },
            [&] { return event_ready(s->done); },
            [&] { finish_batch(s); }) ? 1 : 0;
    } catch (const std::exception &e) { s->completion.fail(); s->error=e.what(); s->poisoned=true; return -EIO; }
}
extern "C" int gids_result(gids *s, unsigned i) {
    if (!s->results[i].result) return 0;
    s->backend->diagnose(i);
    std::fprintf(stderr,"gids: slot=%u result=%u raw=0x%x\n",i,s->results[i].result,s->results[i].raw);
    return s->results[i].result==2 ? ETIMEDOUT : EIO;
}
extern "C" const char *gids_error(gids *s) { return s->error.c_str(); }
extern "C" void gids_destroy(gids *s) { delete s; }
