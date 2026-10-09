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
            if (!realpath(("/sys/class/block/"+name+"/device/device").c_str(),resolved))
                throw std::runtime_error("cannot resolve PCI controller; partitions are unsupported");
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
        window=std::min(o.depth,qdepth-1);
        geo.capacity=owner->capacity;
        geo.block_size=owner->info.block_size;
        geo.max_transfer=std::min<uint64_t>((page/8+1)*page,65536ull*geo.block_size);
        if (owner->info.max_data_size) geo.max_transfer=std::min<uint64_t>(geo.max_transfer,owner->info.max_data_size);
        if (o.max_bs>geo.max_transfer) throw std::runtime_error("bs exceeds local NVMe MDTS/PRP limit: "+std::to_string(geo.max_transfer));
        // The kernel creates queues at its authoritative q_depth. A smaller
        // software ring would wrap the doorbell before the controller does.
        queues=std::make_unique<tnv::NvmeQueueGroup>(client,owner->info,o.nsid,o.gpu,1,qdepth);
        std::fprintf(stderr,"gids: controller=%s GPU=%u qps=1 iodepth=%u ring_entries=%u outstanding_limit=%u\n",
                     owner->pci.c_str(),o.gpu,o.depth,qdepth,window);
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
std::unique_ptr<Backend> make_pcie() { return std::make_unique<Pcie>(); }
