/* SPDX-License-Identifier: GPL-2.0 */
#include "../completion.hpp"
#include <cassert>
#include <cstdio>

struct Job {
    GidsCompletion gate;
    bool kernel=false, copied=false;
    unsigned copies=0, finishes=0, queries=0;
    int fail_at=0;
    bool poll() {
        return gate.poll(
            [&] { ++queries; if(fail_at==1) throw 1; return kernel; },
            [&] { assert(kernel); ++copies; if(fail_at==2) throw 2; },
            [&] { assert(copies==1); if(fail_at==3) throw 3; return copied; },
            [&] { assert(copied); ++finishes; });
    }
};
int main() {
    Job slow,fast;
    assert(!fast.poll() && !fast.queries); // No unrecorded event is queried.
    slow.gate.begin(); fast.gate.begin();
    for(unsigned i=0;i<10;++i) assert(!slow.poll() && !fast.poll());
    assert(!slow.copies && !fast.copies);
    fast.kernel=true;
    assert(!fast.poll() && fast.copies==1 && !fast.finishes);
    for(unsigned i=0;i<10;++i) assert(!fast.poll());
    assert(fast.copies==1); // Copy pending: do not enqueue again.
    bool busy=false;
    try { fast.gate.begin(); } catch(const std::logic_error &) { busy=true; }
    assert(busy && fast.gate.active());
    fast.copied=true;
    assert(fast.poll() && fast.finishes==1 && !fast.gate.active());
    assert(fast.poll() && fast.finishes==1);
    assert(!slow.poll() && !slow.copies); // Fast job never waits for slow job.
    fast.gate.begin(); fast.copies=0;
    assert(fast.poll() && fast.copies==1 && fast.finishes==2); // Event reuse.
    slow.kernel=slow.copied=true;
    assert(slow.poll() && slow.finishes==1);
    for(int where=1;where<=3;++where) {
        Job bad;bad.kernel=bad.copied=true;bad.fail_at=where;bad.gate.begin();
        bool failed=false;
        try { bad.poll(); } catch(int e) { failed=e==where; }
        assert(failed && !bad.finishes);
        unsigned copies=bad.copies;
        try { bad.poll(); assert(false); } catch(const std::logic_error &) {}
        try { bad.gate.begin(); assert(false); } catch(const std::logic_error &) {}
        assert(bad.copies==copies); // Partial enqueue is poisoned, never retried.
    }
    Job launch_failed;launch_failed.gate.begin();launch_failed.gate.fail();
    try { launch_failed.poll(); assert(false); } catch(const std::logic_error &) {}
    assert(!launch_failed.copies && !launch_failed.finishes);
    std::puts("completion: ordering, isolation, reuse and fault cases passed");
}
