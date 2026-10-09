/* SPDX-License-Identifier: GPL-2.0 */
#include "../batch_pump.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
/* Delayed completions in reverse order, with an independent SQ space limit. */
struct Device {
    std::vector<unsigned> inflight, done;
    unsigned capacity, submitted=0, completed=0, highwater=0, polls=0;
    unsigned failure=0, waits=0, fail_submit=UINT32_MAX;
    bool stall=false, bad_cid=false, finished=false;
    explicit Device(unsigned count, unsigned cap): done(count), capacity(cap) {}
    bool can_submit() const { return inflight.size() < capacity; }
    bool submit(unsigned slot) {
        if (slot == fail_submit) return false;
        assert(slot == submitted++ && slot < done.size());
        assert(can_submit()); inflight.push_back(slot);
        highwater=std::max<unsigned>(highwater,inflight.size()); return true;
    }
    int poll(unsigned issued) {
        assert(issued == submitted);
        if (stall || ++polls%3 != 0) return 0;
        if (bad_cid) return -1;
        assert(!inflight.empty()); unsigned cid=inflight.back(); inflight.pop_back();
        assert(cid < issued && !done[cid]); done[cid]=1; ++completed; return 1;
    }
    void wait() { ++waits; }
    void finish() { finished=true; assert(inflight.empty()); }
    void fail(unsigned code) { failure=code; }
};
int main()
{
    for (unsigned n: {1u,31u,32u,33u,64u,128u,255u,256u,1024u,65536u})
        for (unsigned ring: {2u,32u,128u,256u}) {
            unsigned window=std::min(n,ring-1);
            Device d(n,ring-1); gids_pump_batch(d,n,window,10);
            assert(!d.failure && d.finished && d.completed == n);
            assert(d.highwater == window);
            for (auto done: d.done) assert(done == 1);
        }
    Device capped(128,3); gids_pump_batch(capped,128,64,10);
    assert(capped.finished && capped.highwater == 3);
    Device timeout(128,31); timeout.stall=true;
    gids_pump_batch(timeout,128,31,10);
    assert(timeout.failure == 2 && timeout.submitted == 31 && !timeout.finished);
    Device bad(128,31); bad.bad_cid=true;
    gids_pump_batch(bad,128,31,10); assert(bad.failure == 4 && !bad.finished);
    Device rejected(128,31); rejected.fail_submit=40;
    gids_pump_batch(rejected,128,31,10);
    assert(rejected.failure == 4 && rejected.completed && rejected.submitted == 40);
    Device invalid(1,1); gids_pump_batch(invalid,1,0,10);
    assert(invalid.failure == 4 && !invalid.submitted);
    puts("single-QP batch scheduling: PASS");
}
