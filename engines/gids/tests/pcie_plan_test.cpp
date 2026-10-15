/* SPDX-License-Identifier: GPL-2.0 */
#include "../pcie_plan.hpp"
#include "../pcie_config.h"
#include <cassert>
#include <cstdio>
#include <numeric>
#include <random>

template<class F> static void rejects(F f) {
    bool failed=false;
    try { f(); } catch (const std::invalid_argument &) { failed=true; }
    assert(failed);
}
static void check(const std::vector<unsigned> &files,unsigned n,unsigned qps) {
    std::vector<unsigned> windows(n,17), seen(files.size()), counts(n), last(n*qps,~0u);
    auto p=make_pcie_plan(files,unsigned(files.size()),n,qps,windows);
    unsigned total=0;
    for (const auto &r:p.ranges) {
        assert(r.count && r.window==17 && r.first==total && r.device<n && r.queue<qps);
        for (unsigned cid=0;cid<r.count;++cid) {
            unsigned slot=p.indices[r.first+cid], key=r.device*qps+r.queue;
            assert(slot<files.size() && !seen[slot]++ && files[slot]==r.device);
            assert(last[key]==~0u || last[key]<slot); last[key]=slot;
            ++counts[r.device];
        }
        total+=r.count;
    }
    assert(total==files.size());
    for (unsigned d=0;d<n;++d) {
        unsigned lo=~0u,hi=0;
        for (const auto &r:p.ranges) if (r.device==d) {
            lo=std::min(lo,r.count); hi=std::max(hi,r.count);
        }
        if (counts[d]) assert(hi-lo<=1);
    }
}
int main() {
    std::mt19937 rng(20261013);
    for (unsigned n:{1u,2u,3u,4u,17u}) for (unsigned qps:{1u,2u,8u,32u})
        for (unsigned count:{1u,2u,31u,32u,33u,128u,256u,1023u,2048u,65536u}) {
            std::vector<unsigned> files(count);
            for (auto &d:files) d=rng()%n;
            check(files,n,qps);
            std::fill(files.begin(),files.end(),n-1); check(files,n,qps);
        }
    auto p=make_pcie_plan({1,0,1,0,1,0},6,2,2,{3,7});
    assert((p.indices==std::vector<unsigned>{1,5,3,0,4,2}));
    PciePlanScratch scratch;
    build_pcie_plan(p,scratch,{0,0,0},3,2,2,{3,7});
    assert((p.indices==std::vector<unsigned>{0,2,1}) && p.ranges.size()==2);
    build_pcie_plan(p,scratch,{1},1,2,2,{3,7});
    assert(p.indices.size()==1 && p.ranges.size()==1 && p.ranges[0].device==1);
    rejects([]{make_pcie_plan({0,2},2,2,1,{1,1});});
    rejects([]{make_pcie_plan({0},2,1,1,{1});});
    rejects([]{make_pcie_plan({0},1,1,1,{0});});
    assert(pcie_window(0,1024,2048)==1023);
    assert(pcie_window(32,1024,2048)==32);
    rejects([]{pcie_window(1024,1024,2048);});
    rejects([]{pcie_window(0,1000,2048);});
    assert(pcie_worker_count(0,2048,32,8)==256);
    assert(pcie_worker_count(256,33,8,5)==33);
    rejects([]{pcie_worker_count(257,512,32,8);});
    struct gids_options o{}; o.transport="pcie";
    assert(!gids_parallel_config_error(&o));
    o.nr_files=4; o.pcie_qps=32; o.pcie_workers=256; o.pcie_mode=GIDS_PCIE_WORKERS;
    assert(!gids_parallel_config_error(&o));
    o.transport="rdma"; assert(gids_parallel_config_error(&o)); o.transport="pcie";
    o.ap_latency="trace.csv"; assert(gids_parallel_config_error(&o)); o.ap_latency=nullptr;
    o.pcie_mode=GIDS_PCIE_BATCHED; assert(gids_parallel_config_error(&o));
    o.pcie_workers=0; assert(!gids_parallel_config_error(&o));
    o.pcie_qps=33; assert(gids_parallel_config_error(&o)); o.pcie_qps=32;
    o.pcie_block_threads=1025; assert(gids_parallel_config_error(&o)); o.pcie_block_threads=1;
    assert(!gids_parallel_config_error(&o));
    o.pcie_qd=65536; assert(gids_parallel_config_error(&o));
    std::puts("PCIe routing, queue balance and resource bounds: PASS");
}
