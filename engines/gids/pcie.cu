/* SPDX-License-Identifier: GPL-2.0 */
#include "internal.hpp"
#include "pcie_owner.hpp"
#include "csrc/data_paths/local_nvme/io/nvme_queue_group.h"
#include "csrc/data_paths/local_nvme/io/device_target.h"
#include "csrc/data_paths/local_nvme/io/prp_builder.h"
#include <nvm_ctrl.h>
#include <nvm_dma.h>
#include <nvm_error.h>
#include <ctrl.h>
#include <ioctl.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <limits.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <glob.h>
#include <vector>

namespace tnv=tutti::data_paths::local_nvme;
static_assert(GIDS_PCIE_MAX_QPS==NVM_MAX_QUEUES_PER_GROUP,"update PCIe QP option limit for this vendor ABI");
struct Owner {
    nvm_ctrl_t *ctrl=nullptr;
    disk info{};
    uint64_t capacity=0;
    unsigned gpu=0;
    std::string pci, chrdev;
    bool restore=false;
    ~Owner() {
        if (ctrl) { cudaSetDevice(gpu); nvm_ctrl_free(ctrl); }
        // Tutti UNBIND releases snvme but does not re-probe the original
        // driver. Restore only our own PCI function, including failed init.
        if (restore && !pci.empty()) {
            char path[PATH_MAX];
            std::string link="/sys/bus/pci/devices/"+pci+"/driver";
            if (!realpath(link.c_str(),path)) {
                std::ofstream bind("/sys/bus/pci/drivers/nvme/bind");
                bind << pci << '\n'; bind.flush();
                if (!bind) std::fprintf(stderr,"gids: failed to restore nvme binding for %s\n",pci.c_str());
            }
        }
    }
};
static PcieOwnerCache<Owner> owners;
static void check_nvm(int rc, const char *what) {
    if (rc) throw std::runtime_error(std::string(what)+": "+nvm_strerror(rc));
}
class Pcie final : public Backend {
    std::shared_ptr<Owner> owner;
    nvm_ctrl_t *client=nullptr;
    std::unique_ptr<tnv::NvmeQueueGroup> queues;
    nvm_dma_t *data_dma=nullptr, *prp_dma=nullptr;
    void *prp_mem=nullptr;
    tnv::DeviceTargetHandle *target=nullptr;
    void *overflow=nullptr;
    tnv::AddressDescriptor *h_desc=nullptr, *d_desc=nullptr;
    size_t stride=0, page=0;
    unsigned budget=0, window=0;
public:
    tnv::DeviceTargetHandle *device_target() const { return target; }
    const tnv::AddressDescriptor &descriptor(unsigned slot) const { return h_desc[slot]; }
    unsigned outstanding_window() const { return window; }
    unsigned queue_count() const { return queues->n_qps(); }
    void diagnose(unsigned slot) override {
        if (!std::getenv("GIDS_DEBUG")) return;
        alignas(QueuePair) unsigned char storage[sizeof(QueuePair)];
        if (cudaMemcpy(storage,queues->d_qps()+slot%queues->n_qps(),sizeof(storage),cudaMemcpyDeviceToHost)!=cudaSuccess) return;
        const auto *qp=reinterpret_cast<const QueuePair *>(storage);
        uint32_t sq[16]{}, cq[16]{};
        cudaMemcpy(sq,(const void *)qp->sq.vaddr,sizeof(sq),cudaMemcpyDeviceToHost);
        cudaMemcpy(cq,(const void *)qp->cq.vaddr,sizeof(cq),cudaMemcpyDeviceToHost);
        std::fprintf(stderr,"gids: qid=%u depth=%u SQ head=%u tail=%u CQ head=%u phase=%u SQiova=%llx CQiova=%llx\n",
            qp->sq.no,qp->sq.qs,unsigned(qp->sq.head),unsigned(qp->sq.tail),unsigned(qp->cq.head),qp->cq.phase,
            (unsigned long long)qp->sq.ioaddr,(unsigned long long)qp->cq.ioaddr);
        for(unsigned j=0;j<16;++j) std::fprintf(stderr,"%08x%c",sq[j],j==15?'\n':' ');
        for(unsigned j=0;j<16;++j) std::fprintf(stderr,"%08x%c",cq[j],j==15?'\n':' ');
    }
    ~Pcie() override {
        // Stop controller DMA before releasing registered GPU allocations.
        queues.reset();
        if (target) tnv::free_device_target(target,overflow,owner->gpu);
        if (prp_dma) nvm_dma_unmap(prp_dma);
        if (data_dma) nvm_dma_unmap(data_dma);
        if (prp_mem) free(prp_mem);
        if (d_desc) cudaFree(d_desc);
        if (h_desc) cudaFreeHost(h_desc);
        if (client) nvm_ctrl_free_client(client);
        owners.release(owner);
    }
    void open(const gids_options &o, void *data, size_t slot_stride, gids_geometry &geo) override {
        // Tutti's GET_DEV_INFO ABI reports the geometry of namespace 1.
        if (o.nsid!=1) throw std::runtime_error("the Tutti PCIe backend requires nsid=1");
        stride=slot_stride; budget=o.poll_budget;
        owner=owners.acquire(o.filename,[&](const std::string &path) {
            char resolved[PATH_MAX];
            std::string name=path.substr(path.find_last_of('/')+1);
            auto v=std::make_shared<Owner>(); v->gpu=o.gpu;
            int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_EXCL);
            if (fd<0) throw std::runtime_error("open local namespace failed");
            int rc=ioctl(fd,BLKGETSIZE64,&v->capacity);
            int actual_nsid=ioctl(fd,NVME_IOCTL_ID); ::close(fd);
            if (rc) throw std::runtime_error("filename must be a whole NVMe block namespace");
            if (actual_nsid<=0 || unsigned(actual_nsid)!=o.nsid) throw std::runtime_error("gids_nsid does not match local namespace");
            const std::string sysblock="/sys/class/block/"+name;
            if (::access((sysblock+"/partition").c_str(),F_OK)==0)
                throw std::runtime_error("partitions are unsupported");
            if (!realpath((sysblock+"/device/device").c_str(),resolved)) {
                // A native multipath namespace head may have exactly one PCIe
                // controller. Reject actual multipath instead of choosing one.
                glob_t controllers{};
                int status=glob((sysblock+"/device/nvme[0-9]*/address").c_str(),0,nullptr,&controllers);
                std::string controller;
                if (!status && controllers.gl_pathc==1) {
                    controller=controllers.gl_pathv[0];
                    controller=controller.substr(0,controller.find_last_of('/'))+"/device";
                }
                globfree(&controllers);
                if (controller.empty() || !realpath(controller.c_str(),resolved))
                    throw std::runtime_error("namespace must resolve to exactly one PCI controller");
            }
            std::string pci=std::string(resolved).substr(std::string(resolved).find_last_of('/')+1);
            if (pci.size()!=12 || pci[4]!=':' || pci[7]!=':') throw std::runtime_error("filename is not a PCIe NVMe namespace");
            // Rebinding affects all namespaces on a controller. Refuse
            // multi-namespace controllers rather than disrupt a sibling.
            glob_t siblings{};
            std::string pattern="/sys/bus/pci/devices/"+pci+"/nvme/nvme*/nvme*n*";
            int grc=glob(pattern.c_str(),0,nullptr,&siblings);
            size_t count=siblings.gl_pathc;
            globfree(&siblings);
            if (grc || count!=1) throw std::runtime_error("PCIe backend requires exactly one namespace on the controller");
            char driver[PATH_MAX];
            if (!realpath(("/sys/bus/pci/devices/"+pci+"/driver").c_str(),driver) ||
                std::string(driver).substr(std::string(driver).find_last_of('/')+1)!="nvme")
                throw std::runtime_error("local controller must initially use the nvme driver");
            v->pci=pci; v->restore=true;
            nvm_owner_bringup_result result{};
            check_nvm(nvm_controller_init_owner_with_result(&v->ctrl,"/dev/snvm_control",pci.c_str(),8,&v->info,&result),"Tutti NVMe bring-up (load matching snvme modules first)");
            v->chrdev=result.chrdev_path;
            v->info.ns_id=actual_nsid; // Tutti's bring-up leaves this caller-owned field unset.
            return v;
        });
        if (owner->gpu!=o.gpu) throw std::runtime_error("one PCIe controller cannot be rebound across GPUs by concurrent jobs");
        if (o.nsid!=owner->info.ns_id) throw std::runtime_error("gids_nsid does not match local namespace");
        // A queue group belongs to an open file description, not to a job.
        // Each job needs its own Tutti client handle under the shared owner.
        check_nvm(nvm_ctrl_attach_client(&client,owner->chrdev.c_str()),"attach per-job Tutti client");
        disk client_info{};
        check_nvm(ioctl_get_dev_info(client,&client_info),"query per-job controller geometry");
        page=client->page_size;
        if (!page || (page&(page-1)) || page>65536 || stride%page) throw std::runtime_error("unsupported controller page size");
        unsigned qdepth=client->q_depth;
        if (qdepth<2 || (qdepth&(qdepth-1))) throw std::runtime_error("Tutti GPU rings require a power-of-two queue depth >= 2");
        window=pcie_window(o.pcie_qd,qdepth,o.depth);
        geo.capacity=owner->capacity;
        geo.block_size=owner->info.block_size;
        geo.max_transfer=std::min<uint64_t>((page/8+1)*page,65536ull*geo.block_size);
        if (owner->info.max_data_size) geo.max_transfer=std::min<uint64_t>(geo.max_transfer,owner->info.max_data_size);
        if (o.max_bs>geo.max_transfer) throw std::runtime_error("bs exceeds local NVMe MDTS/PRP limit: "+std::to_string(geo.max_transfer));
        // The kernel creates queues at its authoritative q_depth. A smaller
        // software ring would wrap the doorbell before the controller does.
        unsigned qps=o.pcie_qps ? o.pcie_qps : 1;
        queues=std::make_unique<tnv::NvmeQueueGroup>(client,owner->info,o.nsid,o.gpu,qps,qdepth);
        if (queues->n_qps()!=qps)
            throw std::runtime_error("requested gids_qps exceeds controller/group queue capacity");
        std::fprintf(stderr,"gids: controller=%s GPU=%u qps=%u iodepth=%u ring_entries=%u outstanding_limit=%u\n",
                     owner->pci.c_str(),o.gpu,qps,o.depth,qdepth,window);
        check_nvm(nvm_dma_map_data_device(&data_dma,client,data,stride*o.depth),"map GPU payload for NVMe DMA");
        if (posix_memalign(&prp_mem,page,page*o.depth)) throw std::bad_alloc();
        std::memset(prp_mem,0,page*o.depth);
        check_nvm(nvm_dma_map_data_host(&prp_dma,client,prp_mem,page*o.depth),"map PRP lists");
        gpu_check(cudaMallocHost((void **)&h_desc,sizeof(*h_desc)*o.depth),"allocate PRP staging");
        gpu_check(cudaMalloc((void **)&d_desc,sizeof(*d_desc)*o.depth),"allocate GPU PRPs");
        tnv::DeviceTargetHandle h{};
        h.logical_size_bytes=geo.capacity; h.nvme_block_size=geo.block_size;
        while ((1u<<h.nvme_block_size_log)<geo.block_size) ++h.nvme_block_size_log;
        h.namespace_id=o.nsid; h.num_extents=1;
        h.extents[0]={0,geo.capacity/geo.block_size};
        h.d_qps=queues->d_qps(); h.num_d_qps=queues->n_qps();
        if (!tnv::build_device_target(h,nullptr,0,o.gpu,&target,&overflow)) throw std::runtime_error("build Tutti GPU namespace handle failed");
    }
    void prepare(unsigned slot, const Request &r) override {
        unsigned pages=(r.bytes+page-1)/page, start=slot*stride/page;
        if (pages>2) tnv::fill_prp_list_page((uint64_t *)((char *)prp_mem+slot*page),data_dma,start,pages,page);
        h_desc[slot]=tnv::fill_prp_descriptor(data_dma,start,pages,r.bytes,prp_dma->ioaddrs[slot]);
    }
    void launch(unsigned count, const Request *requests, Result *result, cudaStream_t stream) override {
        gpu_check(cudaMemcpyAsync(d_desc,h_desc,count*sizeof(*d_desc),cudaMemcpyHostToDevice,stream),"PRPs to GPU");
        gpu_check(launch_pcie_batch(target,d_desc,requests,result,count,window,budget,stream),"GPU NVMe batch launch");
    }
    void profile(const Request *r,Result *s,void *data,const void *reference,
                 ApSample *sample,cudaStream_t stream) override {
        gpu_check(cudaMemcpyAsync(d_desc,h_desc,sizeof(*d_desc),cudaMemcpyHostToDevice,stream),"profile PRP to GPU");
        gpu_check(launch_pcie_profile(target,d_desc,r,s,data,reference,sample,budget,stream),"profile Tutti SQE/read");
    }
};

/* One adapter/stream may own several SSDs. Payload registration is per
 * controller (IOVAs need not match); each prepared slot uses that device's
 * own PRPs. A single launch dispatches all devices and queues. */
class PcieGroup final : public Backend {
    std::vector<std::unique_ptr<Pcie>> devices;
    std::vector<gids_geometry> geometries;
    std::vector<unsigned> slot_devices, windows;
    PcieBatchPlan plan;
    PciePlanScratch plan_scratch;
    tnv::DeviceTargetHandle **d_targets=nullptr;
    tnv::AddressDescriptor *h_desc=nullptr,*d_desc=nullptr;
    unsigned *h_devices=nullptr,*d_devices=nullptr,*h_indices=nullptr,*d_indices=nullptr;
    PcieRange *h_ranges=nullptr,*d_ranges=nullptr;
    unsigned *cursor=nullptr;
    unsigned depth=0,qps=1,workers=0,budget=0,block_threads=32;
    bool parallel=false,cursor_initialized=false;
public:
    ~PcieGroup() override {
        devices.clear();
        if (d_targets) cudaFree(d_targets);
        if (d_desc) cudaFree(d_desc);
        if (h_desc) cudaFreeHost(h_desc);
        if (d_devices) cudaFree(d_devices);
        if (h_devices) cudaFreeHost(h_devices);
        if (d_indices) cudaFree(d_indices);
        if (h_indices) cudaFreeHost(h_indices);
        if (d_ranges) cudaFree(d_ranges);
        if (h_ranges) cudaFreeHost(h_ranges);
        if (cursor) cudaFree(cursor);
    }
    void open(const gids_options &o,void *data,size_t stride,gids_geometry &geo) override {
        unsigned n=o.nr_files ? o.nr_files : 1;
        depth=o.depth; qps=o.pcie_qps ? o.pcie_qps : 1; budget=o.poll_budget;
        block_threads=o.pcie_block_threads ? o.pcie_block_threads : 32;
        cudaDeviceProp props{};
        gpu_check(cudaGetDeviceProperties(&props,o.gpu),"query GPU block limit");
        if (block_threads>unsigned(props.maxThreadsPerBlock))
            throw std::runtime_error("gids_block_threads exceeds GPU maxThreadsPerBlock");
        parallel=n>1 || qps>1 || o.pcie_mode==GIDS_PCIE_WORKERS || o.pcie_block_threads;
        slot_devices.resize(depth);
        std::vector<tnv::DeviceTargetHandle *> targets;
        for (unsigned i=0;i<n;++i) {
            gids_options one=o;
            one.filename=o.nr_files ? o.filenames[i] : o.filename;
            one.filenames=nullptr; one.nr_files=0;
            auto d=std::make_unique<Pcie>();
            gids_geometry g{};
            d->open(one,data,stride,g);
            windows.push_back(d->outstanding_window());
            geometries.push_back(g); targets.push_back(d->device_target());
            devices.push_back(std::move(d));
        }
        geo=geometries[0];
        if (!parallel) return;
        if (o.pcie_mode==GIDS_PCIE_WORKERS) {
            unsigned least=*std::min_element(windows.begin(),windows.end());
            workers=pcie_worker_count(o.pcie_workers,depth,qps,least);
        }
        gpu_check(cudaMalloc((void **)&d_targets,n*sizeof(*d_targets)),"allocate PCIe device table");
        gpu_check(cudaMemcpy(d_targets,targets.data(),n*sizeof(*d_targets),cudaMemcpyHostToDevice),"copy PCIe device table");
        gpu_check(cudaMallocHost((void **)&h_desc,depth*sizeof(*h_desc)),"allocate multi-device PRP staging");
        gpu_check(cudaMalloc((void **)&d_desc,depth*sizeof(*d_desc)),"allocate multi-device PRPs");
        gpu_check(cudaMallocHost((void **)&h_devices,depth*sizeof(*h_devices)),"allocate file routing staging");
        gpu_check(cudaMalloc((void **)&d_devices,depth*sizeof(*d_devices)),"allocate file routing");
        if (workers) gpu_check(cudaMalloc((void **)&cursor,2*sizeof(*cursor)),"allocate worker cursor");
        else {
            gpu_check(cudaMallocHost((void **)&h_indices,depth*sizeof(*h_indices)),"allocate queue slot staging");
            gpu_check(cudaMalloc((void **)&d_indices,depth*sizeof(*d_indices)),"allocate queue slots");
            gpu_check(cudaMallocHost((void **)&h_ranges,depth*sizeof(*h_ranges)),"allocate queue partition staging");
            gpu_check(cudaMalloc((void **)&d_ranges,depth*sizeof(*d_ranges)),"allocate queue partitions");
        }
        uint64_t bound=0;
        for (unsigned w:windows) bound+=uint64_t(qps)*w;
        bound=std::min<uint64_t>(depth,workers ? workers : bound);
        std::fprintf(stderr,"gids: PCIe dispatch=%s files=%u qps_per_file=%u workers=%u block_threads=%u job_iodepth=%u max_inflight_bound=%llu streams=1\n",
            workers ? "workers" : "batched",n,qps,workers,block_threads,depth,(unsigned long long)bound);
    }
    gids_geometry file_geometry(unsigned file) const override {
        if (file>=geometries.size()) throw std::runtime_error("invalid file index");
        return geometries[file];
    }
    void prepare(unsigned slot,const Request &r) override { prepare_file(0,slot,r); }
    void prepare_file(unsigned file,unsigned slot,const Request &r) override {
        if (file>=devices.size() || slot>=depth) throw std::runtime_error("invalid PCIe file/slot");
        devices[file]->prepare(slot,r);
        slot_devices[slot]=file;
        if (parallel) { h_desc[slot]=devices[file]->descriptor(slot); h_devices[slot]=file; }
    }
    void launch(unsigned count,const Request *requests,Result *result,cudaStream_t stream) override {
        if (!parallel) { devices[0]->launch(count,requests,result,stream); return; }
        gpu_check(cudaMemcpyAsync(d_desc,h_desc,count*sizeof(*d_desc),cudaMemcpyHostToDevice,stream),"multi-device PRPs to GPU");
        unsigned nr=0;
        if (workers) {
            gpu_check(cudaMemcpyAsync(d_devices,h_devices,count*sizeof(*d_devices),cudaMemcpyHostToDevice,stream),"file routing to GPU");
            if (!cursor_initialized) {
                gpu_check(cudaMemsetAsync(cursor,0,2*sizeof(*cursor),stream),"initialize worker cursor");
                cursor_initialized=true;
            }
        } else {
            build_pcie_plan(plan,plan_scratch,slot_devices,count,unsigned(devices.size()),qps,windows);
            nr=unsigned(plan.ranges.size());
            std::copy(plan.indices.begin(),plan.indices.end(),h_indices);
            std::copy(plan.ranges.begin(),plan.ranges.end(),h_ranges);
            gpu_check(cudaMemcpyAsync(d_indices,h_indices,count*sizeof(*d_indices),cudaMemcpyHostToDevice,stream),"queue slots to GPU");
            gpu_check(cudaMemcpyAsync(d_ranges,h_ranges,nr*sizeof(*d_ranges),cudaMemcpyHostToDevice,stream),"queue partitions to GPU");
        }
        PcieLaunch p{d_targets,d_desc,requests,result,d_devices,d_indices,d_ranges,
                     count,nr,std::min(count,workers),budget,block_threads,cursor};
        gpu_check(launch_pcie_parallel(p,stream),"parallel GPU NVMe launch");
    }
    void profile(const Request *r,Result *s,void *data,const void *reference,
                 ApSample *sample,cudaStream_t stream) override {
        devices[0]->profile(r,s,data,reference,sample,stream);
    }
    void diagnose(unsigned slot) override {
        if (!std::getenv("GIDS_DEBUG") || slot>=depth) return;
        unsigned file=slot_devices[slot];
        std::fprintf(stderr,"gids: slot=%u file=%u; queue snapshots (worker assignment is dynamic)\n",slot,file);
        for (unsigned q=0;q<devices[file]->queue_count();++q) devices[file]->diagnose(q);
    }
};
std::unique_ptr<Backend> make_pcie() { return std::make_unique<PcieGroup>(); }
