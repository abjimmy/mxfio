/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

/* One exclusive GPU owner per nonempty device/queue partition. CIDs are
 * local to this range; indices map them back to the original fio slots. */
struct PcieRange { unsigned device, queue, first, count, window; };
struct PcieBatchPlan {
    std::vector<PcieRange> ranges;
    std::vector<unsigned> indices;
};
struct PciePlanScratch {
    std::vector<unsigned> counts,base,next,written;
};
inline void build_pcie_plan(PcieBatchPlan &p,PciePlanScratch &s,
        const std::vector<unsigned> &devices,
        unsigned count, unsigned num_devices, unsigned qps,
        const std::vector<unsigned> &windows)
{
    if (!count || count>65536 || count>devices.size() || !num_devices ||
        !qps || qps>65536 || num_devices>65536 || windows.size()!=num_devices)
        throw std::invalid_argument("invalid PCIe batch dimensions");
    for (unsigned w:windows) if (!w || w>65536)
        throw std::invalid_argument("invalid PCIe outstanding window");
    /* Linear stable bucket partition. At most count nonempty queues:
     * no comparison sort and no devices*qps-sized allocation per batch. */
    auto &counts=s.counts; auto &base=s.base; auto &next=s.next;
    counts.assign(num_devices,0); base.resize(num_devices); next.assign(num_devices,0);
    for (unsigned i=0;i<count;++i) {
        if (devices[i]>=num_devices) throw std::invalid_argument("invalid PCIe file index");
        ++counts[devices[i]];
    }
    p.ranges.clear();
    p.indices.resize(count);
    unsigned first=0;
    for (unsigned d=0;d<num_devices;++d) {
        base[d]=unsigned(p.ranges.size());
        for (unsigned q=0;q<std::min(qps,counts[d]);++q) {
            unsigned n=counts[d]/qps+(q<counts[d]%qps);
            p.ranges.push_back({d,q,first,n,windows[d]}); first+=n;
        }
    }
    auto &written=s.written; written.assign(p.ranges.size(),0);
    for (unsigned slot=0;slot<count;++slot) {
        unsigned d=devices[slot], range=base[d]+next[d]++%qps;
        p.indices[p.ranges[range].first+written[range]++]=slot;
    }
}
inline PcieBatchPlan make_pcie_plan(const std::vector<unsigned> &devices,
        unsigned count,unsigned num_devices,unsigned qps,
        const std::vector<unsigned> &windows)
{
    PcieBatchPlan p; PciePlanScratch s;
    build_pcie_plan(p,s,devices,count,num_devices,qps,windows);
    return p;
}
inline unsigned pcie_window(unsigned requested, unsigned ring_entries, unsigned depth)
{
    if (ring_entries<2 || (ring_entries&(ring_entries-1)))
        throw std::invalid_argument("invalid NVMe ring capacity");
    if (requested>=ring_entries)
        throw std::invalid_argument("gids_qd exceeds hardware ring_entries-1");
    return std::min(depth, requested ? requested : ring_entries-1);
}
inline unsigned pcie_worker_count(unsigned requested, unsigned depth,
                                  unsigned qps, unsigned window)
{
    unsigned workers=std::min(depth,requested ? requested : 256u);
    if (!workers || !qps || !window || (uint64_t(workers)+qps-1)/qps>window)
        throw std::invalid_argument("workers per QP exceed gids_qd/hardware window; increase gids_qps or reduce gids_workers");
    return workers;
}
