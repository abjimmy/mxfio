/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <nvm_types.h>
/* Production nvm_queue_t and its actual atomic fields; only the owning
 * QueuePair's unused host allocation/destruction methods are omitted. */
struct QueuePair {
    unsigned nvmNamespace = 1;
    nvm_queue_t sq{}, cq{};
};
