/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdexcept>

/* Per-job completion gate. Never put a host copy behind an unfinished I/O
 * kernel: a waiting copy can block other streams' otherwise ready copies.
 * Callbacks return readiness or throw; failed/partial copies are not retried. */
class GidsCompletion {
    enum class Phase { idle, kernel, copy, complete, failed } phase=Phase::idle;
public:
    bool active() const { return phase==Phase::kernel || phase==Phase::copy; }
    void begin() {
        if (active() || phase==Phase::failed)
            throw std::logic_error("batch completion is still active or failed");
        phase=Phase::kernel;
    }
    void fail() { phase=Phase::failed; }
    template<class KernelReady,class Copy,class CopyReady,class Finish>
    bool poll(KernelReady kernel_ready,Copy copy,CopyReady copy_ready,Finish finish) {
        try {
            if (phase==Phase::idle) return false;
            if (phase==Phase::failed) throw std::logic_error("batch completion failed");
            if (phase==Phase::kernel) {
                if (!kernel_ready()) return false;
                copy();
                phase=Phase::copy;
            }
            if (phase==Phase::copy) {
                if (!copy_ready()) return false;
                finish();
                phase=Phase::complete;
            }
            return true;
        } catch (...) { fail(); throw; }
    }
};
