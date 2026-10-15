/* SPDX-License-Identifier: GPL-2.0 */
/* Actual fio callbacks with delayed/failing adapter completions. */
#include <assert.h>
#include <stdarg.h>
#include <time.h>
static unsigned sleeps;
static int test_nanosleep(const struct timespec *t, struct timespec *left)
{ (void)t; (void)left; assert(++sleeps < 100); return 0; }
#define nanosleep test_nanosleep
#include "../fio_gids.c"
#undef nanosleep
struct gids {
    unsigned depth, prepared, submitted, polls, ready_at;
    unsigned files, last_file;
    int busy, submit_error, poll_error, result_error, prepare_error;
};
static unsigned created, destroyed, marked, stamped;
static int fail_create;
bool read_only;
ssize_t log_err(const char *format, ...) { (void)format; return 0; }
void io_u_mark_submit(struct thread_data *td, unsigned nr) { (void)td; marked+=nr; }
void fio_gettime(struct timespec *t, void *caller)
{ (void)caller; *t=(struct timespec){123,456}; }
void io_u_queued(struct thread_data *td, struct io_u *io)
{ (void)td; assert(io->issue_time.tv_sec == 123); ++stamped; }
int gids_create(const struct gids_options *o, struct gids **out,
                struct gids_geometry *geo, char *error, size_t len)
{
    (void)error; (void)len; ++created;
    if (fail_create) return ENOMEM;
    *out=calloc(1,sizeof(**out)); assert(*out); (*out)->depth=o->depth;
    (*out)->files=o->nr_files;
    *geo=(struct gids_geometry){1ULL<<30,512,131072}; return 0;
}
int gids_prepare(struct gids *g, unsigned slot, void *host, uint64_t offset,
                 unsigned bytes, int write)
{
    (void)host; (void)offset; (void)bytes; (void)write;
    assert(!g->busy && slot < g->depth);
    if (g->prepare_error) return g->prepare_error;
    assert(slot == g->prepared++); return 0;
}
int gids_prepare_file(struct gids *g,unsigned file,unsigned slot,void *host,
                      uint64_t offset,unsigned bytes,int write) {
    if (file>=g->files) return EINVAL;
    g->last_file=file;
    return gids_prepare(g,slot,host,offset,bytes,write);
}
int gids_file_geometry(struct gids *g,unsigned file,struct gids_geometry *geo) {
    if (file>=g->files) return EINVAL;
    *geo=(struct gids_geometry){(uint64_t)(file+1)<<30,512,131072}; return 0;
}
int gids_submit(struct gids *g, unsigned count)
{
    assert(!g->busy && count == g->prepared && count <= g->depth);
    g->submitted=count; g->prepared=0; g->polls=0; g->busy=!g->submit_error;
    return g->submit_error;
}
int gids_poll(struct gids *g)
{
    assert(g->busy); ++g->polls;
    if (g->poll_error) { g->busy=0; return -g->poll_error; }
    if (g->ready_at && g->polls >= g->ready_at) { g->busy=0; return 1; }
    return 0;
}
int gids_result(struct gids *g, unsigned slot)
{ assert(!g->busy && slot < g->submitted); return g->result_error; }
void gids_destroy(struct gids *g) { if (g) ++destroyed; free(g); }
const char *gids_error(struct gids *g) { (void)g; return "mock"; }
static struct thread_data td;
static struct options opts;
static struct fio_file file;
static struct fio_file *files[]={&file};
static struct io_u ios[512];
static struct state *start(unsigned depth)
{
    memset(&td,0,sizeof(td)); memset(&opts,0,sizeof(opts)); memset(ios,0,sizeof(ios));
    created=destroyed=marked=stamped=sleeps=0; fail_create=0;
    opts.transport="pcie"; opts.nsid=1;
    td.eo=&opts; td.files=files; td.o.iodepth=depth;
    td.o.use_thread=1; td.o.nr_files=1;
    td.o.max_bs[DDIR_READ]=4096; file.file_name="/dev/mock";
    assert(!setup(&td) && !init(&td)); assert(created == 1);
    return td.io_ops_data;
}
static void put(unsigned first, unsigned n)
{
    for (unsigned i=first; i<first+n; ++i) {
        ios[i].ddir=DDIR_READ; ios[i].xfer_buflen=4096; ios[i].offset=(uint64_t)i*4096;
        assert(queue(&td,&ios[i]) == FIO_Q_QUEUED);
    }
}
static void check_events(unsigned first, unsigned n, int error)
{
    for (unsigned i=0; i<n; ++i) {
        assert(event(&td,i) == &ios[first+i]);
        assert(ios[first+i].error == (unsigned)error);
    }
}
static void finish(void)
{ cleanup(&td); assert(!td.io_ops_data && destroyed == 1); cleanup(&td); }
static void depths_and_partial_events(void)
{
    const unsigned depths[]={1,31,32,33,64,128,256};
    for (unsigned k=0; k<sizeof(depths)/sizeof(*depths); ++k) {
        const unsigned n=depths[k]; struct state *s=start(n);
        assert(s->batch.ctx->depth == n);
        put(0,n); assert(queue(&td,&ios[n]) == FIO_Q_BUSY);
        commit(&td); commit(&td); assert(marked == n && stamped == n);
        assert(getevents(&td,0,n,NULL) == 0 && !sleeps);
        const struct timespec zero={0,0};
        assert(getevents(&td,n,n,&zero) == 0 && !sleeps);
        s->batch.ctx->ready_at=s->batch.ctx->polls+3;
        assert(getevents(&td,1,1,NULL) == 1 && sleeps == 2); check_events(0,1,0);
        if (n>1) {
            assert(queue(&td,&ios[n]) == FIO_Q_BUSY);
            assert(getevents(&td,n-1,n,NULL) == (int)n-1); check_events(1,n-1,0);
        }
        put(n,1); s->batch.ctx->ready_at=1;
        assert(getevents(&td,1,n,NULL) == 1); check_events(n,1,0);
        assert(marked == n+1 && stamped == n+1);
        assert(getevents(&td,0,n,NULL) == 0); finish();
    }
}
static void failures(void)
{
    for (unsigned kind=0; kind<3; ++kind) {
        struct state *s=start(128); struct gids *g=s->batch.ctx;
        g->prepare_error=EINVAL;
        assert(queue(&td,&ios[0]) == FIO_Q_COMPLETED && ios[0].error == EINVAL);
        assert(s->batch.pending == 0); g->prepare_error=0;
        put(0,128);
        if (!kind) g->submit_error=EIO;
        if (kind == 1) g->poll_error=EIO;
        if (kind == 2) { g->result_error=ETIMEDOUT; g->ready_at=1; }
        int err=kind == 2 ? ETIMEDOUT : EIO;
        commit(&td); assert(marked == 128 && stamped == 128);
        assert(getevents(&td,128,128,NULL) == 128); check_events(0,128,err);
        assert(queue(&td,&ios[128]) == FIO_Q_COMPLETED && ios[128].error == (unsigned)err);
        assert(getevents(&td,0,128,NULL) == 0); finish();
    }
    start(1); finish(); fail_create=1; destroyed=0;
    assert(init(&td) == ENOMEM); cleanup(&td); assert(!destroyed);
}
static void limits_and_timestamps(void)
{
    struct state *s=start(1);
    td.o.iodepth=GIDS_PCIE_MAX_DEPTH; assert(!setup(&td));
    ++td.o.iodepth; assert(setup(&td)); td.o.iodepth=1;
    opts.transport="rdma"; assert(setup(&td)); opts.transport="pcie";
    td.o.disable_clat=td.o.disable_slat=td.o.disable_bw=1;
    put(0,1); s->batch.ctx->ready_at=1;
    assert(getevents(&td,1,1,NULL) == 1 && !stamped);
    td.o.read_iolog_file="plan.iolog"; put(1,1);
    assert(getevents(&td,1,1,NULL) == 1 && stamped == 1);
    assert(td.last_issue.tv_sec == 123); finish();
}
static void multiple_files(void)
{
    start(8); finish();
    struct fio_file second={.file_name="/dev/mock1",.fileno=1};
    struct fio_file *many[]={&file,&second};
    td.files=many; td.o.nr_files=2;
    opts.submit_mode="workers"; opts.qps=32; opts.workers=256;
    assert(!setup(&td) && !init(&td));
    struct state *s=td.io_ops_data;
    assert(s->batch.ctx->files==2);
    assert(!file_size(&td,&second) && second.real_file_size==(2ULL<<30));
    ios[0].file=&file; ios[1].file=&second;
    put(0,1); assert(s->batch.ctx->last_file==0);
    put(1,1); assert(s->batch.ctx->last_file==1);
    s->batch.ctx->ready_at=1;
    assert(getevents(&td,2,8,NULL)==2); check_events(0,2,0);
    opts.ap_latency="ap.csv"; assert(setup(&td)); opts.ap_latency=NULL;
    opts.transport="rdma"; assert(setup(&td)); opts.transport="pcie";
    opts.submit_mode="batched"; assert(setup(&td)); opts.workers=0;
    opts.submit_mode="invalid"; assert(setup(&td));
    cleanup(&td); assert(!td.io_ops_data); td.files=files;
}
int main(void)
{
    depths_and_partial_events(); failures(); limits_and_timestamps(); multiple_files();
    puts("scheduler callbacks: PASS"); return 0;
}
