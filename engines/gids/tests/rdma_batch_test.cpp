/* SPDX-License-Identifier: GPL-2.0 */
#include "../rdma_batch.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    for (unsigned count: {1u,32u,64u,128u,256u,65536u}) {
        std::vector<RdmaSlot> slots(count);
        RdmaBatchTracker t{slots.data(),65530};
        unsigned cid=0;
        // Reverse-order NVMe responses arrive before corresponding SEND
        // completions. SEND WQE counters wrap across 65535 within the batch.
        for (unsigned i=count;i>0;--i)
            assert(t.received(uint16_t(i-1),uint16_t(i&1 ? 0 : 2),count)==2);
        for (unsigned i=0;i<count;++i) {
            assert(t.sent(uint16_t(t.first_send+i),count,cid)==1 && cid==i);
            assert(slots[i].status==((i+1)&1 ? 0 : 2));
            assert(t.sent(uint16_t(t.first_send+i),count,cid)==-1);
            assert(t.received(uint16_t(i),0,count)==-1);
        }
        for (auto &slot:slots) slot={0,0};
        for (unsigned i=0;i<count;++i) {
            assert(t.sent(uint16_t(t.first_send+i),count,cid)==2 && cid==i);
            assert(t.received(uint16_t(i),0,count)==1);
        }
        if (count<65536) {
            assert(t.received(uint16_t(count),0,count)==-1);
            assert(t.sent(uint16_t(t.first_send+count),count,cid)==-1);
        }
    }
    puts("RDMA SEND/response tracking: PASS");
}
