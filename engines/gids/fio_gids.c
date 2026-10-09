/* SPDX-License-Identifier: GPL-2.0 */
#include "../../fio.h"
#include "../../optgroup.h"
#include "../../verify.h"
#include "adapter.h"
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

struct options {
    void *pad;
    char *transport, *ap_latency, *ap_plan;
    unsigned gpu, nsid, poll_budget, host_buffers;
};
#define OPTSTR(key, member, desc, value) { .name=key, .lname=key, .type=FIO_OPT_STR_STORE, .off1=offsetof(struct options,member), .help=desc, .def=value, .category=FIO_OPT_C_ENGINE, .group=FIO_OPT_G_INVALID }
#define OPTINT(key, member, desc, value, lo, hi) { .name=key, .lname=key, .type=FIO_OPT_INT, .off1=offsetof(struct options,member), .help=desc, .def=value, .minval=lo, .maxval=hi, .category=FIO_OPT_C_ENGINE, .group=FIO_OPT_G_INVALID }
static struct fio_option options[] = {
    OPTSTR("gids_transport", transport, "pcie (no host data bounce)", "pcie"),
    OPTSTR("gids_ap_latency", ap_latency, "AP timestamp CSV (new file; QD1; fixed read or plan replay)", NULL),
    OPTSTR("gids_ap_plan", ap_plan, "Recorded random read/write plan; replay matching read_iolog", NULL),
    OPTINT("gids_gpu", gpu, "MetaX GPU ordinal", "0", 0, 1023),
    OPTINT("gids_nsid", nsid, "Namespace ID", "1", 1, 0x7fffffff),
    OPTINT("gids_poll_budget", poll_budget, "Bound on GPU CQ polling iterations", "1000000", 1, 0x7fffffff),
    OPTINT("gids_host_buffers", host_buffers, "Copy fio buffers for patterns/verification; adds H2D/D2H overhead", "0", 0, 1),
    { .name=NULL }
};
/* One adapter per fio job. A PCIe adapter owns one QP. Slots remain live
 * until the whole GPU batch completes and fio consumes every event. */
struct batch {
    struct gids *ctx;
    struct io_u **ios;
    unsigned depth;
    unsigned pending, active, consumed;
    int ready;
};
struct state {
    struct gids_geometry geo;
    struct batch batch;
    struct io_u **ios, **events;
    int fatal;
};
static int setup(struct thread_data *td)
{
    struct options *o = td->eo;
    if (o->ap_plan && !o->ap_latency) {
        log_err("gids: gids_ap_plan requires gids_ap_latency\n");
        return 1;
    }
    if (o->ap_latency && (td->o.iodepth != 1 ||
                         (td->o.td_ddir != TD_DDIR_READ && !(o->ap_plan && td->o.td_ddir == TD_DDIR_WRITE)) ||
                         o->host_buffers || td->o.verify != VERIFY_NONE)) {
        log_err("gids: AP latency requires QD1, rw=read (or write with a plan), verify=0, gids_host_buffers=0\n");
        return 1;
    }
    if (!td->o.use_thread || td->o.nr_files != 1 || !td->o.iodepth ||
        td_trim(td) || td->o.verify_async || td->o.zone_mode == ZONE_MODE_ZBD) {
        log_err("gids: requires thread=1, one namespace per job, iodepth>=1; trim, verify_async and ZBD are unsupported\n");
        return 1;
    }
    unsigned limit = GIDS_PCIE_MAX_DEPTH;
    if (td->o.iodepth > limit) {
        log_err("gids: %s requires iodepth<=%u\n", o->transport, limit);
        return 1;
    }
    if (strcmp(o->transport, "pcie")) {
        log_err("gids: gids_transport must be pcie\n");
        return 1;
    }
    return 0;
}
static int file_size(struct thread_data *td, struct fio_file *f)
{
    struct state *s = td->io_ops_data;
    uint64_t capacity;
    if (s) capacity = s->geo.capacity;
    else {
        int fd = open(f->file_name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return errno;
        int rc = ioctl(fd, BLKGETSIZE64, &capacity);
        int err = errno;
        close(fd);
        if (rc) return err;
    }
    f->real_file_size = capacity;
    fio_file_set_size_known(f);
    return 0;
}
static int init(struct thread_data *td)
{
    struct options *o = td->eo;
    struct state *s = calloc(1, sizeof(*s));
    char error[512] = {0};
    int rc;
    unsigned max_bs = td->o.max_bs[DDIR_READ] > td->o.max_bs[DDIR_WRITE] ?
        td->o.max_bs[DDIR_READ] : td->o.max_bs[DDIR_WRITE];
    if (!s) return ENOMEM;
    td->io_ops_data = s;
    s->ios = calloc(td->o.iodepth, sizeof(*s->ios));
    s->events = calloc(td->o.iodepth, sizeof(*s->events));
    if (!s->ios || !s->events) return ENOMEM;
    struct gids_options opts = {
        .filename=td->files[0]->file_name, .transport=o->transport,
        .ap_latency=o->ap_latency, .ap_plan=o->ap_plan,
        .gpu=o->gpu, .depth=td->o.iodepth, .max_bs=max_bs, .nsid=o->nsid,
        .poll_budget=o->poll_budget,
        .host_buffers=o->host_buffers || td->o.verify != VERIFY_NONE
    };
    s->batch.depth = td->o.iodepth;
    s->batch.ios = s->ios;
    rc = gids_create(&opts, &s->batch.ctx, &s->geo, error, sizeof(error));
    if (rc) {
        log_err("gids: %s\n", error);
        return rc;
    }
    fprintf(stderr, "gids: %s GPU=%u nsid=%u capacity=%llu LBA=%u max_io=%u host_buffers=%u\n",
        o->transport, o->gpu, o->nsid, (unsigned long long)s->geo.capacity,
        s->geo.block_size, s->geo.max_transfer, opts.host_buffers);
    return 0;
}
static enum fio_q_status queue(struct thread_data *td, struct io_u *io)
{
    struct state *s = td->io_ops_data;
    struct batch *b = &s->batch;
    int rc;
    fio_ro_check(td, io);
    if (s->fatal) { io->error=s->fatal; return FIO_Q_COMPLETED; }
    if (b->active || b->pending == b->depth) return FIO_Q_BUSY;
    if (io->ddir != DDIR_READ && io->ddir != DDIR_WRITE) {
        io->error=EOPNOTSUPP;
        return FIO_Q_COMPLETED;
    }
    rc = gids_prepare(b->ctx, b->pending, io->xfer_buf, io->offset, io->xfer_buflen, io->ddir == DDIR_WRITE);
    if (rc) { io->error=rc; return FIO_Q_COMPLETED; }
    b->ios[b->pending++] = io;
    return FIO_Q_QUEUED;
}
static int commit(struct thread_data *td)
{
    struct state *s = td->io_ops_data;
    struct batch *b = &s->batch;
    struct timespec now;
    int rc;
    if (!b->pending) return 0;
    rc = s->fatal ? s->fatal : gids_submit(b->ctx, b->pending);
    /* Match fio's asynchronous commit engines: slat ends when host
     * submission returns; clat includes GPU work and host completion
     * observation. Neither metric is a device-only SQ-to-CQ timer. */
    if (fio_fill_issue_time(td)) {
        fio_gettime(&now, NULL);
        for (unsigned i=0; i<b->pending; ++i) {
            b->ios[i]->issue_time = now;
            io_u_queued(td, b->ios[i]);
        }
        if (td->o.read_iolog_file) td->last_issue = now;
    }
    io_u_mark_submit(td, b->pending);
    b->active=b->pending;
    b->pending=0;
    b->consumed=0;
    b->ready=rc != 0;
    if (rc) s->fatal=rc;
    return 0; /* Fail the accepted io_us through getevents, exactly once. */
}
static uint64_t nsec_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int getevents(struct thread_data *td, unsigned min, unsigned max, const struct timespec *timeout)
{
    struct state *s=td->io_ops_data;
    unsigned n=0;
    uint64_t start=nsec_now(), limit=UINT64_MAX;
    if (timeout) limit=(uint64_t)timeout->tv_sec*1000000000+timeout->tv_nsec;
    if (!max) return 0;
    commit(td);
    for (;;) {
        struct batch *b = &s->batch;
        if (!b->active) return n;
        if (!b->ready) {
            int rc=gids_poll(b->ctx);
            if (rc) {
                b->ready=1;
                if (rc < 0) s->fatal=-rc;
            }
        }
        if (b->ready) {
            while (b->consumed < b->active && n < max) {
                unsigned slot=b->consumed++;
                struct io_u *io=b->ios[slot];
                io->error=s->fatal ? s->fatal : gids_result(b->ctx, slot);
                if (io->error == ETIMEDOUT) s->fatal=ETIMEDOUT;
                s->events[n++]=io;
            }
            if (b->consumed == b->active) { b->active=0; b->ready=0; }
        }
        if (n >= min || n == max || !b->active || nsec_now()-start >= limit) return n;
        struct timespec nap={0, 10000};
        nanosleep(&nap, NULL);
    }
}
static struct io_u *event(struct thread_data *td, int i)
{ return ((struct state *)td->io_ops_data)->events[i]; }
static int open_file(struct thread_data *td, struct fio_file *f)
{ return file_size(td, f); }
static int close_file(struct thread_data *td, struct fio_file *f)
{ (void)td; (void)f; return 0; }
static void cleanup(struct thread_data *td)
{
    struct state *s=td->io_ops_data;
    if (!s) return;
    gids_destroy(s->batch.ctx);
    free(s->ios); free(s->events); free(s);
    td->io_ops_data=NULL;
}
struct ioengine_ops ioengine = {
    .name="gids", .version=FIO_IOOPS_VERSION,
    .flags=FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_NOFILEHASH | FIO_NO_OFFLOAD | FIO_ASYNCIO_SETS_ISSUE_TIME,
    .setup=setup, .init=init, .queue=queue, .commit=commit,
    .getevents=getevents, .event=event, .cleanup=cleanup,
    .get_file_size=file_size, .open_file=open_file, .close_file=close_file,
    .options=options, .option_struct_size=sizeof(struct options)
};
/* Runtime SDK helpers may spawn a shell inheriting LD_PRELOAD. */
#pragma weak register_ioengine
#pragma weak unregister_ioengine
static void fio_init register_gids(void) { if (register_ioengine) register_ioengine(&ioengine); }
static void fio_exit unregister_gids(void) { if (unregister_ioengine) unregister_ioengine(&ioengine); }
