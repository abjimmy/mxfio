/* SPDX-License-Identifier: GPL-2.0 */
#include "rdma_gpu.cuh"
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <array>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <cerrno>

static void syscheck(bool ok,const char *msg) {
    if(!ok) throw std::runtime_error(std::string(msg)+": "+strerror(errno));
}
using Clock=std::chrono::steady_clock;
class Connection {
    rdma_event_channel *channel=nullptr;
    ibv_mr *admin_mr=nullptr;
    uint8_t *arena=nullptr;
    unsigned timeout=0;
    uint16_t cid=0;
public:
    rdma_cm_id *id=nullptr;
    ibv_pd *pd=nullptr;
    ibv_cq *cq=nullptr;
    unsigned window=0;
    ~Connection() {
        if(id && id->qp) { rdma_disconnect(id); rdma_destroy_qp(id); }
        if(admin_mr) ibv_dereg_mr(admin_mr);
        free(arena);
        if(cq) ibv_destroy_cq(cq);
        if(pd) ibv_dealloc_pd(pd);
        if(id) rdma_destroy_id(id);
        if(channel) rdma_destroy_event_channel(channel);
    }
    void wait(rdma_cm_event_type expected) {
        pollfd p{channel->fd,POLLIN,0};
        int n;
        do { n=::poll(&p,1,timeout); } while(n<0 && errno==EINTR);
        if(n<=0) throw std::runtime_error("RDMA connection timed out / poll failed");
        rdma_cm_event *event=nullptr;
        syscheck(!rdma_get_cm_event(channel,&event),"rdma_get_cm_event");
        auto type=event->event; int status=event->status;
        std::string name=rdma_event_str(type);
        if (type==RDMA_CM_EVENT_ESTABLISHED && !status) {
            const auto &p=event->param.conn;
            if (p.private_data && p.private_data_len>=4) {
                unsigned peer_depth=get16((const uint8_t *)p.private_data+2);
                if (peer_depth) window=std::min(window,peer_depth);
            }
        }
        rdma_ack_cm_event(event);
        if(type!=expected || status) throw std::runtime_error("RDMA CM: "+name+" status="+std::to_string(status));
    }
    void open(const gids_options &o,uint16_t qid,unsigned requested=31) {
        timeout=o.timeout_ms;
        channel=rdma_create_event_channel(); syscheck(channel,"create RDMA event channel");
        syscheck(!rdma_create_id(channel,&id,nullptr,RDMA_PS_TCP),"create RDMA CM ID");
        sockaddr_in addr{}; addr.sin_family=AF_INET;
        char *end=nullptr; long port=strtol(o.trsvcid,&end,10);
        if(!end || *end || port<1 || port>65535 || inet_pton(AF_INET,o.traddr,&addr.sin_addr)!=1)
            throw std::runtime_error("RDMA requires an IPv4 traddr and a numeric service port");
        addr.sin_port=htons(port);
        syscheck(!rdma_resolve_addr(id,nullptr,reinterpret_cast<sockaddr *>(&addr),timeout),"resolve RDMA address");
        wait(RDMA_CM_EVENT_ADDR_RESOLVED);
        syscheck(!rdma_resolve_route(id,timeout),"resolve RDMA route"); wait(RDMA_CM_EVENT_ROUTE_RESOLVED);
        if(std::string(ibv_get_device_name(id->verbs->device)).find("mlx5")!=0)
            throw std::runtime_error("GPU RDMA queues require an mlx5 device");
        ibv_device_attr limits{};
        syscheck(!ibv_query_device(id->verbs,&limits),"query RDMA queue limits");
        window=std::min({requested,unsigned(limits.max_qp_wr),unsigned(limits.max_cqe/2)});
        if (!window) throw std::runtime_error("RDMA device has no usable queue capacity");
        pd=ibv_alloc_pd(id->verbs); syscheck(pd,"allocate RDMA PD");
        cq=ibv_create_cq(id->verbs,2*window,nullptr,nullptr,0); syscheck(cq,"create RDMA CQ");
        ibv_qp_init_attr_ex init{};
        init.send_cq=cq; init.recv_cq=cq; init.qp_type=IBV_QPT_RC;
        init.cap.max_send_wr=window; init.cap.max_recv_wr=window;
        init.cap.max_send_sge=1; init.cap.max_recv_sge=1;
        init.pd=pd; init.comp_mask=IBV_QP_INIT_ATTR_PD;
        mlx5dv_qp_init_attr dv{};
        dv.comp_mask=MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
        // CQE scatter would place the NVMe response in a host CQ buffer instead
        // of the GPU receive buffer. Always disable it explicitly.
        dv.create_flags=MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE;
        id->qp=mlx5dv_create_qp(id->verbs,&init,&dv); syscheck(id->qp,"create mlx5 RC QP");
        window=std::min({window,init.cap.max_send_wr,init.cap.max_recv_wr});
        uint8_t priv[32]{}; put16(priv+2,qid); put16(priv+4,window+1); put16(priv+6,window);
        rdma_conn_param param{};
        param.qp_num=id->qp->qp_num; param.private_data=priv; param.private_data_len=sizeof(priv);
        param.responder_resources=16; param.initiator_depth=16;
        param.flow_control=1; param.retry_count=7; param.rnr_retry_count=7;
        syscheck(!rdma_connect(id,&param),"connect NVMe/RDMA"); wait(RDMA_CM_EVENT_ESTABLISHED);
        if(posix_memalign((void **)&arena,4096,8192)) throw std::bad_alloc();
        std::memset(arena,0,8192);
        admin_mr=ibv_reg_mr(pd,arena,8192,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE);
        syscheck(admin_mr,"register admin buffers");
    }
    uint8_t *data() { return arena+4096; }
    uint64_t command(Capsule c,unsigned bytes=0) {
        put16(c.b+2,++cid);
        if(bytes) keyed_sgl(c,(uintptr_t)data(),bytes,admin_mr->rkey);
        else { c.b[1]=0x40; c.b[39]=0x40; }
        std::memcpy(arena,&c,64); std::memset(arena+128,0,16);
        ibv_sge recv_sge{(uintptr_t)(arena+128),16,admin_mr->lkey};
        ibv_recv_wr recv{},*bad_recv=nullptr;
        recv.wr_id=1; recv.sg_list=&recv_sge; recv.num_sge=1;
        syscheck(!ibv_post_recv(id->qp,&recv,&bad_recv),"post admin receive");
        ibv_sge send_sge{(uintptr_t)arena,64,admin_mr->lkey};
        ibv_send_wr send{},*bad_send=nullptr;
        send.wr_id=2; send.sg_list=&send_sge; send.num_sge=1;
        send.opcode=IBV_WR_SEND; send.send_flags=IBV_SEND_SIGNALED;
        syscheck(!ibv_post_send(id->qp,&send,&bad_send),"post admin capsule");
        bool sent=false,received=false;
        auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
        while(!sent || !received) {
            ibv_wc wc{}; int n=ibv_poll_cq(cq,1,&wc);
            syscheck(n>=0,"poll admin CQ");
            if(n) {
                if(wc.status!=IBV_WC_SUCCESS) throw std::runtime_error(std::string("admin RDMA completion: ")+ibv_wc_status_str(wc.status));
                if(wc.wr_id==1) { if(wc.byte_len!=16) throw std::runtime_error("short NVMe response"); received=true; }
                else if(wc.wr_id==2) sent=true;
                else throw std::runtime_error("unknown admin completion");
            }
            if(Clock::now()>deadline) throw std::runtime_error("NVMe admin command timeout");
        }
        uint16_t status=get16(arena+142)>>1;
        if(get16(arena+140)!=cid || status) throw std::runtime_error("NVMe admin completion status="+std::to_string(status));
        return get64(arena+128);
    }
    uint16_t connect(const gids_options &o,uint16_t qid,uint16_t cntlid,const std::string &host,const std::array<uint8_t,16> &hostid) {
        if(strlen(o.subnqn)>223 || host.size()>223) throw std::runtime_error("NQN too long");
        std::memset(data(),0,1024);
        std::memcpy(data(),hostid.data(),16); put16(data()+16,cntlid);
        std::memcpy(data()+256,o.subnqn,strlen(o.subnqn)); std::memcpy(data()+512,host.data(),host.size());
        Capsule c{}; c.b[0]=0x7f; c.b[4]=1;
        put16(c.b+42,qid); put16(c.b+44,window);
        // KATO=0: no keepalive requirement. No hidden background traffic is
        // introduced into measured GPU I/O. Unsupported targets fail Connect.
        return uint16_t(command(c,1024));
    }
    uint64_t property(uint32_t off,bool wide,bool write=false,uint64_t value=0) {
        Capsule c{}; c.b[0]=0x7f; c.b[4]=write?0:4; c.b[40]=wide?1:0;
        put32(c.b+44,off); put64(c.b+48,value); return command(c);
    }
};

// libmlx5 packs doorbell records from many QPs into shared host pages. Keep
// one accelerator registration per page with references across all jobs.
struct HostPage { void *gpu; unsigned refs; };
static std::mutex map_lock;
static std::map<std::pair<unsigned,uintptr_t>,HostPage> mappings;
class GpuMapping {
    std::vector<std::pair<unsigned,uintptr_t>> pages;
public:
    GpuQueuePages map_queue(void *ptr,size_t len,unsigned gpu) {
        // MACA may assign unrelated GPU virtual addresses to adjacent host
        // pages. WQEs/CQEs never straddle pages, so translate each entry.
        GpuQueuePages q{};
        size_t ps=sysconf(_SC_PAGESIZE);
        uintptr_t base=(uintptr_t)ptr&~(ps-1);
        q.offset=(uintptr_t)ptr-base;
        while((size_t(1)<<q.shift)<ps) ++q.shift;
        size_t count=(q.offset+len+ps-1)/ps;
        if(count>16) throw std::runtime_error("mlx5 queue exceeds GPU page table");
        for(size_t i=0;i<count;++i) q.pages[i]=(uint8_t *)map((void *)(base+i*ps),ps,gpu);
        return q;
    }
    ~GpuMapping() {
        std::lock_guard<std::mutex> lock(map_lock);
        for(auto k:pages) {
            auto it=mappings.find(k);
            if(it!=mappings.end() && !--it->second.refs) { cudaHostUnregister((void *)k.second); mappings.erase(it); }
        }
    }
    void *map(void *ptr,size_t len,unsigned gpu,bool io=false) {
        size_t ps=sysconf(_SC_PAGESIZE);
        uintptr_t first=(uintptr_t)ptr&~(ps-1),last=((uintptr_t)ptr+len+ps-1)&~(ps-1);
        void *base=nullptr;
        std::lock_guard<std::mutex> lock(map_lock);
        for(uintptr_t p=first;p<last;p+=ps) {
            auto key=std::make_pair(gpu,p); auto it=mappings.find(key);
            if(it==mappings.end()) {
                gpu_check(cudaHostRegister((void *)p,ps,cudaHostRegisterMapped|(io?cudaHostRegisterIoMemory:0)),io?"map NIC UAR into GPU":"map NIC queue into GPU");
                void *dev=nullptr;
                cudaError_t e=cudaHostGetDevicePointer(&dev,(void *)p,0);
                if(e!=cudaSuccess) { cudaHostUnregister((void *)p); gpu_check(e,"get mapped GPU address"); }
                it=mappings.emplace(key,HostPage{dev,0}).first;
            }
            ++it->second.refs; pages.push_back(key);
            if(!base) base=it->second.gpu;
            if((uintptr_t)it->second.gpu!=(uintptr_t)base+p-first) throw std::runtime_error("non-contiguous GPU queue mapping");
        }
        return (char *)base+((uintptr_t)ptr-first);
    }
};
struct IoQueue {
    std::unique_ptr<Connection> conn;
    std::unique_ptr<GpuMapping> mapping;
    ibv_mr *meta_mr=nullptr,*data_mr=nullptr;
    void *meta=nullptr;
    ~IoQueue() {
        mapping.reset();
        if(conn && conn->id && conn->id->qp) { rdma_disconnect(conn->id); rdma_destroy_qp(conn->id); }
        if(meta_mr) ibv_dereg_mr(meta_mr);
        if(data_mr) ibv_dereg_mr(data_mr);
        if(meta) cudaFree(meta);
    }
};
class Rdma final : public Backend {
    std::unique_ptr<Connection> admin;
    std::vector<std::unique_ptr<IoQueue>> queues;
    GpuRdmaQueue *d_queues=nullptr;
    unsigned nsid=0,bs=0,budget=0;
public:
    ~Rdma() override { queues.clear(); admin.reset(); if(d_queues) cudaFree(d_queues); }
    void open(const gids_options &o,void *data,size_t stride,gids_geometry &geo) override {
        nsid=o.nsid; budget=o.poll_budget;
        std::string host;
        if(o.hostnqn) host=o.hostnqn;
        else { std::ifstream f("/etc/nvme/hostnqn"); std::getline(f,host); }
        if(host.empty()) throw std::runtime_error("set gids_hostnqn or create /etc/nvme/hostnqn");
        std::array<uint8_t,16> hostid{};
        // Deterministic host identity within this controller's set of queues.
        std::ifstream random("/dev/urandom",std::ios::binary); random.read((char *)hostid.data(),16);
        if(!random) throw std::runtime_error("generate host ID failed");
        admin=std::make_unique<Connection>(); admin->open(o,0);
        uint16_t cntlid=admin->connect(o,0,0xffff,host,hostid);
        uint64_t cap=admin->property(0,true);
        if(!(cap&0xffff)) throw std::runtime_error("target has no usable I/O queue capacity");
        admin->property(0x14,false,true,0x460001); // EN, IOSQES=6, IOCQES=4
        auto deadline=Clock::now()+std::chrono::milliseconds(o.timeout_ms);
        while(!(admin->property(0x1c,false)&1)) {
            if(Clock::now()>deadline) throw std::runtime_error("NVMe controller ready timeout");
            usleep(1000);
        }
        Capsule c{}; c.b[0]=6; put32(c.b+40,1); admin->command(c,4096);
        uint8_t mdts=admin->data()[77];
        unsigned mpsmin=12+((cap>>48)&15);
        if(mdts && mdts+mpsmin>=32) throw std::runtime_error("invalid target MDTS");
        uint32_t max_io=mdts?(1u<<(mdts+mpsmin)):0xffffff;
        if(!(get32(admin->data()+536)&(1u<<2))) throw std::runtime_error("target does not support keyed SGLs");
        c={}; c.b[0]=6; put32(c.b+4,o.nsid); admin->command(c,4096);
        uint8_t *n=admin->data(); unsigned fmt=n[26]&15;
        unsigned lbads=n[128+fmt*4+2];
        if(fmt>n[25] || get16(n+128+fmt*4) || n[29] || lbads<9 || lbads>16)
            throw std::runtime_error("unsupported namespace LBA format / metadata / protection information");
        bs=1u<<lbads;
        uint64_t blocks=get64(n);
        if(!blocks || blocks>UINT64_MAX/bs) throw std::runtime_error("invalid namespace capacity");
        geo={blocks*bs,bs,uint32_t(std::min<uint64_t>(max_io,std::min<uint64_t>(0xffffffu,65536ull*bs)))};
        if(o.max_bs>geo.max_transfer) throw std::runtime_error("bs exceeds NVMe/RDMA MDTS: "+std::to_string(geo.max_transfer));
        c={}; c.b[0]=9; put32(c.b+40,7); put32(c.b+44,0); // one data QP
        admin->command(c);
        GpuRdmaQueue g{};
        {
            auto q=std::make_unique<IoQueue>();
            // Bound physical rings to the GPU queue-page table. Larger fio
            // batches refill this window without creating additional QPs.
            unsigned requested=std::min({o.depth,256u,unsigned(cap&0xffff)});
            q->conn=std::make_unique<Connection>(); q->conn->open(o,1,requested);
            q->conn->connect(o,1,cntlid,host,hostid);
            mlx5dv_qp qp{}; mlx5dv_cq cq{}; mlx5dv_obj obj{};
            obj.qp.in=q->conn->id->qp; obj.qp.out=&qp;
            obj.cq.in=q->conn->cq; obj.cq.out=&cq;
            syscheck(!mlx5dv_init_obj(&obj,MLX5DV_OBJ_QP|MLX5DV_OBJ_CQ),"export mlx5 queues");
            auto power2=[](unsigned n) { return n && !(n&(n-1)); };
            if(!power2(qp.sq.wqe_cnt)||!power2(qp.rq.wqe_cnt)||!power2(cq.cqe_cnt)||qp.sq.stride!=64||qp.rq.stride!=16||cq.cqe_size!=64)
                throw std::runtime_error("unsupported mlx5 queue geometry");
            g.window=std::min({q->conn->window,qp.sq.wqe_cnt,qp.rq.wqe_cnt,cq.cqe_cnt/2});
            if (!g.window) throw std::runtime_error("mlx5 queue has no usable capacity");
            size_t response_offset=size_t(o.depth)*sizeof(Capsule);
            size_t state_offset=response_offset+size_t(qp.rq.wqe_cnt)*16;
            size_t meta_bytes=(state_offset+size_t(o.depth)*sizeof(RdmaSlot)+65535)&~size_t(65535);
            gpu_check(cudaMalloc(&q->meta,meta_bytes),"allocate GPU RDMA capsule pool");
            gpu_check(cudaMemset(q->meta,0,meta_bytes),"initialize GPU capsules");
            q->meta_mr=ibv_reg_mr(q->conn->pd,q->meta,meta_bytes,IBV_ACCESS_LOCAL_WRITE);
            syscheck(q->meta_mr,"register GPU capsules with NIC (peer-memory driver required)");
            q->data_mr=ibv_reg_mr(q->conn->pd,data,stride*o.depth,
                                IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE);
            syscheck(q->data_mr,"register GPU payload with NIC; host bounce is forbidden");
            q->mapping=std::make_unique<GpuMapping>(); auto &m=*q->mapping;
            g.sq=m.map_queue(qp.sq.buf,qp.sq.wqe_cnt*64,o.gpu);
            g.rq=m.map_queue(qp.rq.buf,qp.rq.wqe_cnt*qp.rq.stride,o.gpu);
            g.cq=m.map_queue(cq.buf,cq.cqe_cnt*cq.cqe_size,o.gpu);
            g.db=(uint32_t *)m.map((void *)qp.dbrec,8,o.gpu);
            g.cq_db=(uint32_t *)m.map((void *)cq.dbrec,8,o.gpu);
            g.uar=(uint64_t *)m.map(qp.bf.reg,8,o.gpu,true);
            g.command=(Capsule *)q->meta; g.response=(uint8_t *)q->meta+response_offset;
            g.slots=(RdmaSlot *)((uint8_t *)q->meta+state_offset);
            g.data=(uintptr_t)data; g.stride=stride; g.qpn=q->conn->id->qp->qp_num;
            g.sq_count=qp.sq.wqe_cnt; g.rq_count=qp.rq.wqe_cnt; g.cq_count=cq.cqe_cnt;
            g.rq_stride=qp.rq.stride; g.cqe_size=cq.cqe_size;
            g.sq_pi=ntohl(qp.dbrec[1]); g.rq_pi=ntohl(qp.dbrec[0]); g.cq_ci=ntohl(cq.dbrec[0])&0xffffff;
            g.lkey=q->meta_mr->lkey; g.rkey=q->meta_mr->rkey; g.data_rkey=q->data_mr->rkey;
            queues.push_back(std::move(q));
        }
        gpu_check(cudaMalloc((void **)&d_queues,sizeof(g)),"allocate GPU RDMA queue handle");
        gpu_check(cudaMemcpy(d_queues,&g,sizeof(g),cudaMemcpyHostToDevice),"upload GPU RDMA queue handle");
        std::fprintf(stderr,"gids: rdma GPU=%u data_qps=1 qpn=%u iodepth=%u SQ=%u RQ=%u CQ=%u outstanding_limit=%u\n",
                     o.gpu,g.qpn,o.depth,g.sq_count,g.rq_count,g.cq_count,g.window);
    }
    void prepare(unsigned,const Request &) override {}
    void launch(unsigned count,const Request *r,Result *s,cudaStream_t stream) override {
        gpu_check(launch_rdma(d_queues,r,s,count,nsid,bs,budget,stream),"GPU NVMe/RDMA launch");
    }
    void profile(const Request *r,Result *s,void *,const void *reference,
                 ApSample *sample,cudaStream_t stream) override {
        gpu_check(launch_rdma_profile(d_queues,r,s,reference,sample,nsid,bs,budget,stream),"profile GPU NVMe/RDMA");
    }
};
std::unique_ptr<Backend> make_rdma() { return std::make_unique<Rdma>(); }
