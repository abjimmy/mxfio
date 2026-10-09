/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FIO_GIDS_ADAPTER_H
#define FIO_GIDS_ADAPTER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct gids;
/* A drained batch uses one unique 16-bit NVMe CID per fio slot. */
#define GIDS_PCIE_MAX_DEPTH 65536u
struct gids_options {
    const char *filename, *transport;
    const char *ap_latency, *ap_plan;
    unsigned gpu, depth, max_bs, nsid, poll_budget, host_buffers;
};
struct gids_geometry {
    uint64_t capacity;
    uint32_t block_size, max_transfer;
};
int gids_create(const struct gids_options *, struct gids **, struct gids_geometry *, char *, size_t);
/* Each batch has dense slot numbers [0, count). No overlap between batches. */
int gids_prepare(struct gids *, unsigned slot, void *host, uint64_t offset, unsigned bytes, int write);
int gids_submit(struct gids *, unsigned count);
int gids_poll(struct gids *); /* 0 pending, 1 completed, negative errno */
int gids_result(struct gids *, unsigned slot);
const char *gids_error(struct gids *);
void gids_destroy(struct gids *);
#ifdef __cplusplus
}
#endif
#endif
