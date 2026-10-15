/* SPDX-License-Identifier: GPL-2.0 */
#include "../worker_pool.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <algorithm>

/* Exercise the production task scheduler concurrently, with delayed I/O,
 * partial batches, injected errors and reuse of the same device cursor. */
struct Pool {
    std::atomic<unsigned> cursor{0}, stop{0}, active{0}, resets{0}, tickets{0};
    std::vector<unsigned> done;
    std::vector<std::atomic<unsigned>> seen;
    unsigned fail, workers;
    Pool(unsigned n,unsigned w,unsigned f):done(n),seen(n),fail(f),workers(w) {
        for (auto &s:seen) s=0;
    }
    unsigned take() {
        unsigned slot=cursor.fetch_add(1);
        ++tickets;
        return slot;
    }
    bool aborted() { return stop.load()!=0; }
    void abort() { stop=1; }
    void cancel(unsigned slot) { assert(seen[slot].fetch_add(1)==0); done[slot]=2; }
    bool execute(unsigned slot) {
        assert(seen[slot].fetch_add(1)==0); ++active;
        if (slot%7==0) for (unsigned i=0;i<8;++i) std::this_thread::yield();
        done[slot]=slot==fail ? 3 : 1;
        --active;
        return slot!=fail;
    }
    void reset() {
        assert(!active.load());
        for (unsigned d:done) assert(d); // all result stores precede terminal tickets
        assert(resets.fetch_add(1)==0);
        stop=0; cursor=0;
    }
    void run() {
        std::vector<std::thread> threads;
        for (unsigned i=0;i<workers;++i)
            threads.emplace_back([&]{gids_run_worker(*this,unsigned(done.size()),workers);});
        for (auto &t:threads) t.join();
        assert(cursor==0 && stop==0 && resets==1 && tickets==done.size()+workers);
        for (unsigned i=0;i<done.size();++i) {
            assert(seen[i]==1);
            if (fail>=done.size()) assert(done[i]==1);
        }
        if (fail<done.size()) assert(done[fail]==3);
        // Mimic another batch without host-side reset of cursor or abort flag.
        std::fill(done.begin(),done.end(),0);
        for (auto &s:seen) s=0;
        resets=0; tickets=0;
    }
};
int main() {
    for (unsigned count:{1u,31u,33u,128u,2048u,65536u})
        for (unsigned workers:{1u,8u,32u,64u}) {
            Pool p(count,std::min(count,workers),count);
            p.run(); p.run();
            p.fail=0; p.run();
        }
    std::puts("worker pool exact completion, abort and repeated batches: PASS");
}
