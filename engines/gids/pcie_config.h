/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GIDS_PCIE_CONFIG_H
#define GIDS_PCIE_CONFIG_H
#include "adapter.h"
#include <string.h>

/* Validate before allocating GPU resources or rebinding any controller. */
static inline const char *gids_parallel_config_error(const struct gids_options *o)
{
    unsigned files=o->nr_files ? o->nr_files : 1;
    unsigned qps=o->pcie_qps ? o->pcie_qps : 1;
    if (!o->transport) return "missing transport";
    if (files>GIDS_PCIE_MAX_DEPTH || qps>GIDS_PCIE_MAX_QPS ||
        o->pcie_workers>GIDS_PCIE_MAX_DEPTH || o->pcie_qd>=GIDS_PCIE_MAX_DEPTH ||
        o->pcie_block_threads>1024)
        return "PCIe files/workers <=65536, QPs <=32, queue depth <=65535 and block threads <=1024 required";
    if (o->pcie_mode>GIDS_PCIE_WORKERS) return "invalid PCIe submit mode";
    if (strcmp(o->transport,"pcie")) {
        if (files!=1 || qps!=1 || o->pcie_mode || o->pcie_workers || o->pcie_qd || o->pcie_block_threads)
            return "multi-file/queue/worker options require gids_transport=pcie";
    }
    if (!o->pcie_mode && o->pcie_workers)
        return "gids_workers requires gids_submit_mode=workers";
    if (o->ap_latency && (files!=1 || qps!=1 || o->pcie_mode || o->pcie_workers || o->pcie_qd || o->pcie_block_threads))
        return "AP profiling requires one file/queue and the default batched mode";
    return 0;
}
#endif
