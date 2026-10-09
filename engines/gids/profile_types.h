/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdint.h>
struct ApSample {
    uint64_t sq_begin, doorbell, cq_seen, first_read, all_read, ap_done;
    // Upstream per-entry timing view, converted from nanoseconds to the raw
    // realtime ticks of the queue-header hooks: CQE observed, and the slot
    // reclaimed after cq_dequeue + put_cid inside poll_bounded.
    uint64_t cqe_observed, slot_reclaimed;
    uint64_t expected_first, observed_first;
    uint32_t mismatches, reserved;
};
struct ApCalibration {
    double ticks_per_us, min_ticks_per_us, max_ticks_per_us;
    uint64_t timer_pair_ticks;
};
