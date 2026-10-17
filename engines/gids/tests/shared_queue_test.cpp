/* SPDX-License-Identifier: GPL-2.0 */
/* CPU/mock regression of the actual workers submission and shared queue
 * code. This does not validate a GPU compiler, MMIO ordering or live DMA. */
#define __CUDACC__ 1
#define TUTTI_USE_MACA 1
#include <cstdio>
#include "csrc/data_paths/local_nvme/io/nvme_submit_primitives.cuh"
#include <algorithm>
#include <cassert>
#include <vector>

namespace tnv = tutti::data_paths::local_nvme;
using Submit = tnv::QueueAcquireHelper;
constexpr unsigned budget = 4;

struct Queue {
    QueuePair qp;
    unsigned size, log;
    uint32_t sq_db = 0, cq_db = 0;
    std::vector<nvm_cmd_t> commands;
    std::vector<nvm_cpl_t> completions;
    std::vector<padded_struct> tickets, tail_marks, head_marks, positions, cids;
    uint64_t cq_tail;

    explicit Queue(unsigned log2 = 3, uint64_t start = 0)
        : size(1u << log2), log(log2), commands(size), completions(size),
          tickets(size), tail_marks(size), head_marks(size), positions(size),
          cids(65536), cq_tail(start) {
        queue_mock::reset();
        static_assert(sizeof(nvm_atomic_u32_dev_t) == sizeof(uint32_t),
                      "the mock must retain the production counter width");
        for (auto* q : {&qp.sq, &qp.cq}) {
            q->head_lock.store(0); q->tail_lock.store(0);
            q->head.store(uint32_t(start)); q->tail.store(uint32_t(start));
            q->head_copy.store(uint32_t(start)); q->tail_copy.store(uint32_t(start));
            q->in_ticket.store(uint32_t(start)); q->cid_ticket.store(0);
            q->qs = size; q->qs_minus_1 = size - 1; q->qs_log2 = log;
        }
        qp.sq.vaddr = commands.data(); qp.cq.vaddr = completions.data();
        qp.sq.tickets = tickets.data(); qp.sq.tail_mark = tail_marks.data();
        qp.sq.cid = cids.data(); qp.cq.head_mark = head_marks.data();
        qp.cq.pos_locks = positions.data();
        qp.sq.db = &sq_db; qp.cq.db = &cq_db;
        sq_db = cq_db = uint32_t(start) & (size - 1);
        for (unsigned pos = 0; pos < size; ++pos) {
            /* Initialize the state from the full history, before truncation.
             * Using get_id(uint32_t(start)) here would hide the old bug. */
            const uint64_t next = start + ((pos - uint32_t(start)) & (size - 1));
            tickets[pos].val.store(uint32_t((next >> log) * 2));
            tail_marks[pos].val.store(0); head_marks[pos].val.store(0);
            positions[pos].val.store(0);
            /* No completion in this generation yet. */
            completions[pos].dword[3] = ((uint32_t(next) >> log) & 1) << 16;
        }
        for (auto &cid : cids) cid.val.store(0);
    }

    bool issue(uint16_t &cid, unsigned polls = budget) {
        return Submit::issue_nvme_cmd(&qp, 0x1000, 0, 8, 16,
                                      NVM_IO_READ, &cid, polls);
    }
    void complete(uint16_t cid, uint32_t sq_head, unsigned status = 0) {
        const unsigned pos = uint32_t(cq_tail) & (size - 1);
        auto &cpl = completions[pos];
        cpl.dword[2] = sq_head & (size - 1);
        cpl.dword[3] = cid | (((~(uint32_t(cq_tail) >> log)) & 1) << 16)
                          | (status << 17);
        ++cq_tail;
    }
    uint32_t poll(uint16_t cid, unsigned polls = budget) {
        uint32_t raw = 0;
        return Submit::poll_bounded(&qp, cid, polls, &raw);
    }
    tnv::DeviceTargetHandle target() {
        tnv::DeviceTargetHandle target{};
        target.logical_size_bytes = 1u << 20;
        target.nvme_block_size = 512; target.nvme_block_size_log = 9;
        target.num_extents = 1; target.extents[0] = {0, 2048};
        target.d_qps = &qp; target.num_d_qps = 1;
        return target;
    }
};

static void wrap_and_reuse() {
    for (unsigned log : {1u, 3u, 10u, 16u}) {
        /* Both the first in_ticket rollover and eventual uint32_t slot
         * generation rollover, followed by more than two full rings. */
        for (uint64_t boundary : {uint64_t(1) << 32, uint64_t(1) << (31 + log)}) {
            const uint64_t start = boundary - 3;
            Queue q(log, start);
            for (unsigned i = 0; i < q.size * 2 + 7; ++i) {
                const uint32_t sequence = uint32_t(start + i);
                uint16_t cid = 0;
                assert(q.issue(cid));
                assert(q.sq_db == ((sequence + 1) & (q.size - 1)));
                assert((q.commands[sequence & (q.size - 1)].dword[0] >> 16) == cid);
                q.complete(cid, sequence + 1);
                assert(q.poll(cid) == (sequence & (q.size - 1)));
                assert(q.qp.sq.head.load() == sequence + 1);
                assert(q.qp.sq.tail.load() == sequence + 1);
                assert(q.qp.sq.in_ticket.load() == sequence + 1);
                assert(q.qp.cq.head.load() == sequence + 1);
                assert(q.cq_db == ((sequence + 1) & (q.size - 1)));
                assert(q.cids[cid].val.load() == UNLOCKED);
                assert(q.positions[sequence & (q.size - 1)].val.load() == UNLOCKED);
            }
        }
    }
}

static void late_completion_hole() {
    Queue q;
    uint16_t a = 0, b = 0;
    assert(q.issue(a) && q.issue(b));
    assert(q.poll(a) == NVM_CQ_TIMEOUT);
    assert(q.cids[a].val.load() == LOCKED);
    /* A has stopped polling. Its late CQE blocks B's otherwise valid CQE. */
    q.complete(a, 1); q.complete(b, 2);
    queue_mock::sleeps = 0;
    assert(q.poll(b) == NVM_CQ_TIMEOUT);
    assert(queue_mock::sleeps <= budget);
    assert(q.qp.cq.head.load() == 0 && q.cq_db == 0);
    assert(q.qp.sq.head.load() == 0);
    assert(q.cids[a].val.load() == LOCKED && q.cids[b].val.load() == LOCKED);
    assert(q.head_marks[1].val.load() == LOCKED);
    assert(q.positions[1].val.load() == LOCKED);
    assert(q.qp.cq.head_lock.load() == UNLOCKED);
}

static void out_of_order_reclaim() {
    Queue q;
    uint16_t a = 0, b = 0;
    assert(q.issue(a) && q.issue(b));
    q.complete(a, 1); q.complete(b, 2);
    /* B marks its entry first. A then acknowledges the contiguous run. */
    queue_mock::on_wait = [&] {
        queue_mock::on_wait = {};
        assert(q.poll(a) == 0);
    };
    assert(q.poll(b) == 1);
    assert(q.qp.cq.head.load() == 2 && q.qp.sq.head.load() == 2);
    assert(q.cids[a].val.load() == UNLOCKED && q.cids[b].val.load() == UNLOCKED);
    assert(q.positions[0].val.load() == UNLOCKED);
    assert(q.positions[1].val.load() == UNLOCKED);
}

static void allocation_and_publication_timeouts() {
    {
        Queue q;
        for (auto &cid : q.cids) cid.val.store(LOCKED);
        uint16_t cid = 42;
        assert(!q.issue(cid));
        assert(cid == 42); // No owned CID and no SQ ticket.
        assert(q.qp.sq.in_ticket.load() == 0 && q.qp.sq.tail.load() == 0);
        assert(q.qp.sq.cid_ticket.load() <= budget + 1);
    }
    {
        Queue q;
        q.tickets[0].val.store(1); // Slot not yet retired.
        uint16_t cid = 0;
        assert(!q.issue(cid));
        assert(q.qp.sq.in_ticket.load() == 1 && q.qp.sq.tail.load() == 0);
        assert(q.cids[cid].val.load() == LOCKED);
        assert(q.tail_marks[0].val.load() == UNLOCKED);
    }
    {
        Queue q;
        q.qp.sq.tail_lock.store(LOCKED); // Another publisher is stalled.
        uint16_t cid = 0;
        assert(!q.issue(cid));
        assert(q.cids[cid].val.load() == LOCKED);
        assert(q.tail_marks[0].val.load() == LOCKED); // SQE may still publish.
        assert(q.qp.sq.tail_lock.load() == LOCKED); // Do not unlock another owner.
        assert(q.qp.sq.tail.load() == 0);
    }
}

static void completion_lock_timeouts() {
    {
        Queue q;
        uint16_t cid = 0;
        assert(q.issue(cid)); q.complete(cid, 1);
        q.positions[0].val.store(LOCKED);
        assert(q.poll(cid) == NVM_CQ_TIMEOUT);
        assert(q.cids[cid].val.load() == LOCKED);
        assert(q.positions[0].val.load() == LOCKED);
        assert(q.head_marks[0].val.load() == UNLOCKED);
    }
    {
        Queue q;
        uint16_t cid = 0;
        assert(q.issue(cid)); q.complete(cid, 1);
        q.qp.cq.head_lock.store(LOCKED);
        assert(q.poll(cid) == NVM_CQ_TIMEOUT);
        assert(q.cids[cid].val.load() == LOCKED);
        assert(q.positions[0].val.load() == LOCKED);
        assert(q.qp.cq.head_lock.load() == LOCKED);
    }
    {
        Queue q;
        uint16_t cid = 0;
        assert(q.issue(cid)); q.complete(cid, 1);
        q.qp.cq.head_lock.store(LOCKED);
        queue_mock::on_wait = [&] {
            queue_mock::on_wait = {};
            /* Real shared reclamation clears the mark, but its lock owner
             * is delayed before ringing the CQ doorbell/publishing head. */
            assert(move_head_cq(&q.qp.cq, 0, &q.qp.sq) == 1);
        };
        assert(q.poll(cid) == NVM_CQ_TIMEOUT);
        assert(q.head_marks[0].val.load() == UNLOCKED);
        assert(q.qp.cq.head.load() == 0 && q.cq_db == 0);
        assert(q.cids[cid].val.load() == LOCKED);
        assert(q.positions[0].val.load() == LOCKED);
    }
}

static void entry_timeout_status() {
    for (bool write : {false, true}) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            Queue q;
            auto target = q.target();
            tnv::EntryCompletionStatus status{};
            /* The workers/AP entry points must poison the job for each
             * stage: absent CQE, unavailable SQ slot, or failed CQ reclaim. */
            if (fault == 1) q.tickets[0].val.store(1);
            if (fault == 2) {
                q.qp.cq.head_lock.store(LOCKED);
                queue_mock::on_wait = [&] {
                    queue_mock::on_wait = {};
                    q.complete(0, 1);
                };
            }
            if (write)
                tnv::submit_write_one(&target, 0x1000, 0, 0, 4096, &status, budget, 0);
            else
                tnv::submit_read_one(&target, 0x1000, 0, 0, 4096, &status, budget, 0);
            assert(status.result == 2);
            assert(q.cids[0].val.load() == LOCKED);
            if (fault == 2) assert(q.positions[0].val.load() == LOCKED);
        }
    }
}

int main() {
    wrap_and_reuse();
    late_completion_hole();
    out_of_order_reclaim();
    allocation_and_publication_timeouts();
    completion_lock_timeouts();
    entry_timeout_status();
    std::puts("production shared SQ/CQ rollover, late CQE and bounded waits: PASS");
}
