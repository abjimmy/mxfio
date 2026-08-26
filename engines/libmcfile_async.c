/*
 *
 * License: GPLv2, see COPYING.
 *
 * libmcfile_async engine
 *
 * fio asynchronous I/O engine using the MACA mcFile API, supporting two
 * submission paths selectable at runtime through the "api_type" option:
 *
 *   api_type=batch  - use the mcFile Batch API
 *                       (mcFileBatchIOSetUp / mcFileBatchIOSubmit /
 *                        mcFileBatchIOGetStatus). Multiple I/O requests are
 *                       gathered by ->queue() and submitted together by
 *                       ->commit().
 *
 *   api_type=async  - use the mcFile Stream (async) API
 *                       (mcFileReadAsync / mcFileWriteAsync). Each request
 *                       is submitted to a dedicated CUDA stream; completions
 *                       are polled with mcStreamQuery.
 *
 * Both paths provide asynchronous GPU-direct storage I/O for performance
 * testing. The implementation follows the async I/O engine patterns used by
 * the libaio and io_uring engines, and reuses the GPU memory management
 * approach from the libmcfile engine.
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <mcfile.h>
#include <mc_runtime.h>
#include <pthread.h>

#include "../fio.h"
#include "../lib/pow2.h"
#include "../optgroup.h"
#include "../lib/memalign.h"

#define ALIGNED_4KB(v) (((v) & 0x0fff) == 0)

#define LOGGED_BUFLEN_NOT_ALIGNED     0x01
#define LOGGED_GPU_OFFSET_NOT_ALIGNED 0x02
#define GPU_ID_SEP ":"

enum api_type {
	MCFILE_API_BATCH = 0,
	MCFILE_API_ASYNC = 1,
};

struct libmcfile_async_options {
	struct thread_data *td;
	char               *gpu_ids;       /* colon-separated list of GPU ids,
					      one per job */
	void               *dev_mem_ptr;    /* GPU memory */
	int                 my_gpu_id;     /* GPU id to use for this job */
	size_t              total_mem;     /* size for dev_mem_ptr */
	int                 logged;        /* bitmask of log messages that have
					      been output, prevent flood */
	int                 api;           /* selected mcFile API path */
};

struct fio_libmcfile_async_file_data {
	MCfileDescr_t  cf_descr;
	MCfileHandle_t cf_handle;
};

/*
 * Per-slot state for the async stream path. Each slot owns a CUDA stream and
 * the mcFileReadAsync/mcFileWriteAsync argument set, which the API requires to
 * remain valid until the operation completes.
 */
enum async_slot_state {
	ASYNC_SLOT_FREE = 0,   /* available for reuse */
	ASYNC_SLOT_QUEUED,    /* prepared by queue(), not yet submitted */
	ASYNC_SLOT_IN_FLIGHT, /* submitted, awaiting completion */
};

struct libmcfile_async_slot {
	size_t   size;
	off_t    file_offset;
	off_t    buf_offset;      /* always 0 (mcFile bug workaround) */
	size_t   gpu_offset;      /* actual GPU memory offset, baked into devPtr_base */
	ssize_t  bytes_done;
	int      opcode;          /* MCFILE_READ or MCFILE_WRITE */
	int      state;
	struct io_u *io_u;        /* owning io_u, NULL when ASYNC_SLOT_FREE */
};

/*
 * Per-thread engine state.
 *
 * Shared:
 *   api       - selected submission path (batch or async stream)
 *   entries   - batch / stream capacity (== iodepth)
 *
 * Batch path:
 *   io_params  - filled one entry at a time in ->queue(); consumed in
 *                ->commit() by mcFileBatchIOSubmit().
 *   io_events  - filled by mcFileBatchIOGetStatus() in ->getevents();
 *                drained one entry at a time by ->event().
 *   queued     - number of io_us prepared but not yet submitted
 *   in_flight  - number of io_us submitted but not yet completed
 *
 * Async stream path:
 *   slots            - one per entry, each with its own CUDA stream
 *   pending_slots    - slot indices prepared by queue() but not submitted
 *   pending_nr       - count of pending slot indices
 *   completed_slots  - slot indices filled by getevents() (poll completions)
 *   completed_nr     - number of valid entries in completed_slots
 *   completed_head   - next index consumed by event()
 *   in_flight        - number of slots in ASYNC_SLOT_IN_FLIGHT state
 */
struct libmcfile_async_data {
	int                 api;
	unsigned int        entries;

	/* batch path */
	MCfileBatchHandle_t batch_handle;
	MCfileIOParams_t   *io_params;
	MCfileIOEvents_t   *io_events;
	unsigned int        queued;
	unsigned int        in_flight;

	/* async stream path */
	mcStream_t stream;           /* single shared MACA stream per thread */
	struct libmcfile_async_slot *slots;
	unsigned int       *pending_slots;
	unsigned int        pending_nr;
	unsigned int       *completed_slots;
	unsigned int        completed_nr;
	unsigned int        completed_head;
};

static struct fio_option options[] = {
	{
		.name	  = "gpu_dev_ids",
		.lname	  = "libmcfile_async engine gpu dev ids",
		.type	  = FIO_OPT_STR_STORE,
		.off1	  = offsetof(struct libmcfile_async_options, gpu_ids),
		.help	  = "GPU IDs, one per subjob, separated by " GPU_ID_SEP,
		.category = FIO_OPT_C_ENGINE,
		.group	  = FIO_OPT_G_LIBCUFILE,
	},
	{
		.name	  = "api_type",
		.lname	  = "mcfile API type",
		.type	  = FIO_OPT_STR,
		.off1	  = offsetof(struct libmcfile_async_options, api),
		.help	  = "Select mcFile API: batch or async",
		.def	  = "batch",
		.category = FIO_OPT_C_ENGINE,
		.group	  = FIO_OPT_G_LIBCUFILE,
		.posval	  = {
			  { .ival = "batch", .oval = MCFILE_API_BATCH },
			  { .ival = "async", .oval = MCFILE_API_ASYNC },
		},
	},
	{
		.name	 = NULL,
	},
};

static int running = 0;
static int mcfile_initialized = 0;
static pthread_mutex_t running_lock = PTHREAD_MUTEX_INITIALIZER;

#define check_mcruntimecall(fn, rc)                                               \
	do {                                                                        \
		mcError_t res = fn;                                               \
		if (res != mcSuccess) {                                           \
			const char *str = mcGetErrorName(res);                    \
			log_err("mc runtime api call failed %s:%d : err=%d:%s\n", \
				#fn, __LINE__, res, str);                           \
			rc = -1;                                                    \
		} else                                                              \
			rc = 0;                                                     \
	} while(0)

static const char *fio_libmcfile_async_get_mc_error(MCfileError_t st)
{
	if (IS_MCFILE_ERR(st.err))
		return mcfileop_status_error(st.err);
	return "unknown";
}

/*
 * Assign GPU to subjob roundrobin, similar to how multiple
 * entries in 'directory' are handled by fio.
 */
static int fio_libmcfile_async_find_gpu_id(struct thread_data *td)
{
	struct libmcfile_async_options *o = td->eo;
	int gpu_id = 0;

	if (o->gpu_ids != NULL) {
		char *gpu_ids, *pos, *cur;
		int i, id_count, gpu_idx;

		for (id_count = 0, cur = o->gpu_ids; cur != NULL; id_count++) {
			cur = strchr(cur, GPU_ID_SEP[0]);
			if (cur != NULL)
				cur++;
		}

		gpu_idx = td->subjob_number % id_count;

		pos = gpu_ids = strdup(o->gpu_ids);
		if (gpu_ids == NULL) {
			log_err("strdup(gpu_ids): err=%d\n", errno);
			return -1;
		}

		i = 0;
		while (pos != NULL && i <= gpu_idx) {
			i++;
			cur = strsep(&pos, GPU_ID_SEP);
		}

		if (cur)
			gpu_id = atoi(cur);

		free(gpu_ids);
	}

	return gpu_id;
}

static int fio_libmcfile_async_init(struct thread_data *td)
{
	struct libmcfile_async_options *o = td->eo;
	struct libmcfile_async_data *ad;
	MCfileError_t status;
	int initialized;
	int rc;

	ad = calloc(1, sizeof(*ad));
	if (ad == NULL) {
		log_err("calloc failed for libmcfile_async_data: err=%d\n", errno);
		return ENOMEM;
	}
	ad->api = o->api;
	ad->entries = td->o.iodepth;
	td->io_ops_data = ad;

	pthread_mutex_lock(&running_lock);
	if (running == 0) {
		assert(mcfile_initialized == 0);
		/* only open the driver if this is the first worker thread */
		status = mcFileDriverOpen();
		if (status.err != MC_FILE_SUCCESS) {
			log_err("mcFileDriverOpen: err=%d:%s\n", status.err,
				fio_libmcfile_async_get_mc_error(status));
		} else {
			mcfile_initialized = 1;
		}
	}
	running++;
	initialized = mcfile_initialized;
	pthread_mutex_unlock(&running_lock);

	if (!initialized)
		return 1;

	o->my_gpu_id = fio_libmcfile_async_find_gpu_id(td);
	if (o->my_gpu_id < 0)
		return 1;

	dprint(FD_MEM, "Subjob %d uses GPU %d\n", td->subjob_number, o->my_gpu_id);
	check_mcruntimecall(mcSetDevice(o->my_gpu_id), rc);
	if (rc != 0)
		return 1;

	return 0;
}

static int fio_libmcfile_async_post_init(struct thread_data *td)
{
	struct libmcfile_async_data *ad = td->io_ops_data;
	MCfileError_t status;
	unsigned int i;
	int rc;

	if (ad->api == MCFILE_API_BATCH) {
		ad->io_params = calloc(ad->entries, sizeof(MCfileIOParams_t));
		ad->io_events = calloc(ad->entries, sizeof(MCfileIOEvents_t));
		if (ad->io_params == NULL || ad->io_events == NULL) {
			log_err("calloc failed for batch arrays: err=%d\n", errno);
			return 1;
		}

		status = mcFileBatchIOSetUp(&ad->batch_handle, ad->entries);
		if (status.err != MC_FILE_SUCCESS) {
			log_err("mcFileBatchIOSetUp: err=%d:%s\n", status.err,
				fio_libmcfile_async_get_mc_error(status));
			return 1;
		}
	} else {
		/* async stream path: one shared CUDA stream per thread */
		ad->slots = calloc(ad->entries, sizeof(*ad->slots));
		ad->pending_slots = calloc(ad->entries, sizeof(*ad->pending_slots));
		ad->completed_slots = calloc(ad->entries, sizeof(*ad->completed_slots));
		if (ad->slots == NULL || ad->pending_slots == NULL ||
		    ad->completed_slots == NULL) {
			log_err("calloc failed for async arrays: err=%d\n", errno);
			return 1;
		}

		check_mcruntimecall(
			mcStreamCreateWithFlags(&ad->stream,
						  mcStreamNonBlocking), rc);
		if (rc != 0) {
			log_err("mcStreamCreateWithFlags failed\n");
			return 1;
		}

		for (i = 0; i < ad->entries; i++) {
			ad->slots[i].state = ASYNC_SLOT_FREE;
			ad->slots[i].io_u = NULL;
		}
	}

	return 0;
}

static inline int fio_libmcfile_async_pre_write(struct thread_data *td,
						struct libmcfile_async_options *o,
						struct io_u *io_u,
						size_t gpu_offset)
{
	int rc = 0;

	if (td->o.verify) {
		/*
		  Data is being verified, copy the io_u buffer to GPU memory.
		  This isn't done in the non-verify case because the data would
		  already be in GPU memory in a normal mcFile application.
		*/
		check_mcruntimecall(mcMemcpy(((char*) o->dev_mem_ptr) + gpu_offset,
						 io_u->xfer_buf,
						 io_u->xfer_buflen,
						 mcMemcpyHostToDevice), rc);
		if (rc != 0) {
			log_err("DDIR_WRITE mcMemcpy H2D failed\n");
			io_u->error = EIO;
		}
	}

	return rc;
}

static inline int fio_libmcfile_async_post_read(struct thread_data *td,
						struct libmcfile_async_options *o,
						struct io_u *io_u,
						size_t gpu_offset)
{
	int rc = 0;

	if (td->o.verify) {
		/* Copy GPU memory to CPU buffer for verify */
		check_mcruntimecall(mcMemcpy(io_u->xfer_buf,
						 ((char*) o->dev_mem_ptr) + gpu_offset,
						 io_u->xfer_buflen,
						 mcMemcpyDeviceToHost), rc);
		if (rc != 0) {
			log_err("DDIR_READ mcMemcpy D2H failed\n");
			io_u->error = EIO;
		}
	}

	return rc;
}

static size_t fio_libmcfile_async_gpu_offset(struct io_u *io_u)
{
	/*
	 * There may be a better way to calculate gpu_offset. The intent is
	 * that gpu_offset equals the the difference between io_u->xfer_buf and
	 * the page-aligned base address for io_u buffers.
	 */
	return io_u->index * io_u->xfer_buflen;
}

static int fio_libmcfile_async_queue_sync(struct thread_data *td,
					  struct io_u *io_u)
{
	struct libmcfile_async_data *ad = td->io_ops_data;

	/* wait for outstanding async IO to drain before handling sync/trim */
	if (ad->api == MCFILE_API_BATCH) {
		if (ad->queued || ad->in_flight)
			return FIO_Q_BUSY;
	} else {
		if (ad->pending_nr || ad->in_flight)
			return FIO_Q_BUSY;
	}

	if (io_u->ddir == DDIR_SYNC)
		do_io_u_sync(td, io_u);
	else if (io_u->ddir == DDIR_DATASYNC)
		do_io_u_sync(td, io_u);
	else
		do_io_u_trim(td, io_u);

	io_u_mark_submit(td, 1);
	io_u_mark_complete(td, 1);
	return FIO_Q_COMPLETED;
}

/* ---- batch path ---------------------------------------------------------- */

static enum fio_q_status fio_libmcfile_async_queue_batch(struct thread_data *td,
							 struct io_u *io_u)
{
	struct libmcfile_async_options *o = td->eo;
	struct libmcfile_async_data *ad = td->io_ops_data;
	struct fio_libmcfile_async_file_data *fcd = FILE_ENG_DATA(io_u->file);
	size_t gpu_offset;
	MCfileIOParams_t *params;

	if (ad->queued + ad->in_flight >= ad->entries)
		return FIO_Q_BUSY;

	gpu_offset = fio_libmcfile_async_gpu_offset(io_u);
	assert(gpu_offset + io_u->xfer_buflen <= o->total_mem);

	if (!(ALIGNED_4KB(io_u->xfer_buflen) ||
	      (o->logged & LOGGED_BUFLEN_NOT_ALIGNED))) {
		log_err("buflen not 4KB-aligned: %llu\n", io_u->xfer_buflen);
		o->logged |= LOGGED_BUFLEN_NOT_ALIGNED;
	}

	if (!(ALIGNED_4KB(gpu_offset) ||
	      (o->logged & LOGGED_GPU_OFFSET_NOT_ALIGNED))) {
		log_err("gpu_offset not 4KB-aligned: %lu\n", gpu_offset);
		o->logged |= LOGGED_GPU_OFFSET_NOT_ALIGNED;
	}

	if (io_u->ddir == DDIR_WRITE)
		fio_libmcfile_async_pre_write(td, o, io_u, gpu_offset);

	if (io_u->error != 0) {
		td_verror(td, io_u->error, "xfer");
		return FIO_Q_COMPLETED;
	}

	params = &ad->io_params[ad->queued];
	params->mode = MCFILE_BATCH;
	params->fh = fcd->cf_handle;
	params->u.batch.devPtr_base = o->dev_mem_ptr;
	params->u.batch.file_offset = io_u->offset;
	params->u.batch.devPtr_offset = gpu_offset;
	params->u.batch.size = io_u->xfer_buflen;
	params->opcode = (io_u->ddir == DDIR_READ) ? MCFILE_READ : MCFILE_WRITE;
	params->cookie = io_u;

	ad->queued++;
	return FIO_Q_QUEUED;
}

static void fio_libmcfile_async_queued(struct thread_data *td,
				       struct io_u **io_us, unsigned int nr)
{
	struct timespec now;
	unsigned int i;

	if (!fio_fill_issue_time(td))
		return;

	fio_gettime(&now, NULL);

	for (i = 0; i < nr; i++) {
		struct io_u *io_u = io_us[i];

		memcpy(&io_u->issue_time, &now, sizeof(now));
		io_u_queued(td, io_u);
	}

	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &now, sizeof(now));
}

static int fio_libmcfile_async_commit_batch(struct thread_data *td)
{
	struct libmcfile_async_data *ad = td->io_ops_data;
	struct io_u *io_us[ad->entries];
	unsigned int i;
	MCfileError_t status;

	if (!ad->queued)
		return 0;

	status = mcFileBatchIOSubmit(ad->batch_handle, ad->queued,
				     ad->io_params, 0);
	if (status.err != MC_FILE_SUCCESS) {
		log_err("mcFileBatchIOSubmit: err=%d:%s\n", status.err,
			fio_libmcfile_async_get_mc_error(status));
		td_verror(td, EIO, "mcFileBatchIOSubmit");
		return -EIO;
	}

	for (i = 0; i < ad->queued; i++)
		io_us[i] = (struct io_u *) ad->io_params[i].cookie;

	fio_libmcfile_async_queued(td, io_us, ad->queued);
	io_u_mark_submit(td, ad->queued);

	ad->in_flight += ad->queued;
	ad->queued = 0;

	return 0;
}

static int fio_libmcfile_async_getevents_batch(struct thread_data *td,
					       unsigned int min, unsigned int max,
					       const struct timespec *t)
{
	struct libmcfile_async_data *ad = td->io_ops_data;
	unsigned int actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	MCfileError_t status;
	unsigned int nr;
	int events = 0;

	if (max == 0)
		return 0;

	do {
		nr = max - events;
		status = mcFileBatchIOGetStatus(ad->batch_handle, actual_min,
						&nr, ad->io_events + events,
						(struct timespec *) t);
		if (status.err != MC_FILE_SUCCESS) {
			log_err("mcFileBatchIOGetStatus: err=%d:%s\n", status.err,
				fio_libmcfile_async_get_mc_error(status));
			td_verror(td, EIO, "mcFileBatchIOGetStatus");
			return -EIO;
		}

		if (nr > 0) {
			events += nr;
			if (events >= min)
				break;
			if (actual_min > nr)
				actual_min -= nr;
			else
				actual_min = 0;
		}

		/* non-blocking probe with nothing available: stop looping */
		if (actual_min == 0 && events == 0)
			break;

		/* still short of the minimum: ensure IOs are submitted */
		if (events < min) {
			fio_libmcfile_async_commit_batch(td);
			if (actual_min)
				usleep(10);
		}
	} while (events < min);

	ad->in_flight -= events;
	return events;
}

static struct io_u *fio_libmcfile_async_event_batch(struct thread_data *td,
						    int event)
{
	struct libmcfile_async_options *o = td->eo;
	struct libmcfile_async_data *ad = td->io_ops_data;
	MCfileIOEvents_t *ev;
	struct io_u *io_u;
	size_t gpu_offset;

	ev = &ad->io_events[event];
	io_u = (struct io_u *) ev->cookie;

	if (ev->status != MCFILE_COMPLETE) {
		io_u->error = EIO;
		log_err("mcFile batch IO not complete: status=0x%x\n", ev->status);
	} else if ((ssize_t) ev->ret < 0) {
		io_u->error = EIO;
		log_err("mcFile batch IO failed: ret=%zd\n", (ssize_t) ev->ret);
	} else if (ev->ret != io_u->xfer_buflen) {
		io_u->resid = io_u->xfer_buflen - ev->ret;
	} else {
		io_u->error = 0;
	}

	if (io_u->ddir == DDIR_READ && io_u->error == 0) {
		gpu_offset = fio_libmcfile_async_gpu_offset(io_u);
		fio_libmcfile_async_post_read(td, o, io_u, gpu_offset);
	}

	return io_u;
}

/* ---- async stream path --------------------------------------------------- */

static enum fio_q_status fio_libmcfile_async_queue_async(struct thread_data *td,
							 struct io_u *io_u)
{
	struct libmcfile_async_options *o = td->eo;
	struct libmcfile_async_data *ad = td->io_ops_data;
	size_t gpu_offset;
	unsigned int i, slot;

	if (ad->pending_nr + ad->in_flight >= ad->entries)
		return FIO_Q_BUSY;

	/* find a free slot */
	slot = ad->entries;
	for (i = 0; i < ad->entries; i++) {
		if (ad->slots[i].state == ASYNC_SLOT_FREE) {
			slot = i;
			break;
		}
	}
	if (slot == ad->entries)
		return FIO_Q_BUSY;

	gpu_offset = slot * io_u->xfer_buflen;

	assert(gpu_offset + io_u->xfer_buflen <= o->total_mem);

	if (!(ALIGNED_4KB(io_u->xfer_buflen) ||
	      (o->logged & LOGGED_BUFLEN_NOT_ALIGNED))) {
		log_err("buflen not 4KB-aligned: %llu\n", io_u->xfer_buflen);
		o->logged |= LOGGED_BUFLEN_NOT_ALIGNED;
	}

	if (!(ALIGNED_4KB(gpu_offset) ||
	      (o->logged & LOGGED_GPU_OFFSET_NOT_ALIGNED))) {
		log_err("gpu_offset not 4KB-aligned: %lu\n", gpu_offset);
		o->logged |= LOGGED_GPU_OFFSET_NOT_ALIGNED;
	}

	if (io_u->ddir == DDIR_WRITE)
		fio_libmcfile_async_pre_write(td, o, io_u, gpu_offset);

	if (io_u->error != 0) {
		td_verror(td, io_u->error, "xfer");
		return FIO_Q_COMPLETED;
	}

	ad->slots[slot].size = io_u->xfer_buflen;
	ad->slots[slot].file_offset = io_u->offset;
	ad->slots[slot].buf_offset = 0;  /* mcFile bug: must be 0 */
	ad->slots[slot].gpu_offset = gpu_offset;
	ad->slots[slot].bytes_done = 0;
	ad->slots[slot].opcode = (io_u->ddir == DDIR_READ) ? MCFILE_READ : MCFILE_WRITE;
	ad->slots[slot].io_u = io_u;
	ad->slots[slot].state = ASYNC_SLOT_QUEUED;

	ad->pending_slots[ad->pending_nr++] = slot;
	return FIO_Q_QUEUED;
}

static int fio_libmcfile_async_commit_async(struct thread_data *td)
{
#ifdef MCFILE_ASYNC_IMPLEMENTED
	struct libmcfile_async_options *o = td->eo;
	struct libmcfile_async_data *ad = td->io_ops_data;
	struct libmcfile_async_slot *slot;
	struct fio_libmcfile_async_file_data *fcd;
	struct io_u *io_u, *io_us[ad->entries];
	unsigned int i, idx, nr = 0;
	MCfileError_t status;

	if (!ad->pending_nr)
		return 0;

	for (i = 0; i < ad->pending_nr; i++) {
		idx = ad->pending_slots[i];
		slot = &ad->slots[idx];
		io_u = slot->io_u;
		fcd = FILE_ENG_DATA(io_u->file);

		/*
		 * mcFile bug workaround: bufPtr_offset_p must be 0.
		 * Bake the actual GPU offset into devPtr_base instead.
		 */
		if (slot->opcode == MCFILE_READ) {
			status = mcFileReadAsync(fcd->cf_handle,
						 o->dev_mem_ptr + slot->gpu_offset,
						 &slot->size,
						 &slot->file_offset,
						 &slot->buf_offset,
						 &slot->bytes_done,
						 ad->stream);
		} else {
			status = mcFileWriteAsync(fcd->cf_handle,
						  o->dev_mem_ptr + slot->gpu_offset,
						  &slot->size,
						  &slot->file_offset,
						  &slot->buf_offset,
						  &slot->bytes_done,
						  ad->stream);
		}

		if (status.err != MC_FILE_SUCCESS) {
			log_err("mcFile%sAsync: err=%d:%s\n",
				slot->opcode == MCFILE_READ ? "Read" : "Write",
				status.err,
				fio_libmcfile_async_get_mc_error(status));
			io_u->error = EIO;
			td_verror(td, EIO, "mcFileAsync");
			slot->state = ASYNC_SLOT_FREE;
			slot->io_u = NULL;
			continue;
		}

		slot->state = ASYNC_SLOT_IN_FLIGHT;
		ad->in_flight++;
		io_us[nr++] = io_u;
	}

	if (nr) {
		fio_libmcfile_async_queued(td, io_us, nr);
		io_u_mark_submit(td, nr);
	}

	ad->pending_nr = 0;
#endif
	return 0;
}

static int fio_libmcfile_async_getevents_async(struct thread_data *td,
					       unsigned int min, unsigned int max,
					       const struct timespec *t)
{
	struct libmcfile_async_data *ad = td->io_ops_data;
	unsigned int i;
	int events = 0;

	ad->completed_nr = 0;
	ad->completed_head = 0;

	if (max == 0 || ad->in_flight == 0)
		return 0;

	/*
	 * With a single shared CUDA stream, all in-flight IOs complete
	 * together. Use mcStreamQuery for non-blocking (min==0) or
	 * mcStreamSynchronize for blocking (min>0).
	 */
	if (min == 0) {
		mcError_t cres = mcStreamQuery(ad->stream);
		if (cres != mcSuccess)
			return 0;
	} else {
		mcError_t cres = mcStreamSynchronize(ad->stream);
		if (cres != mcSuccess) {
			log_err("mcStreamSynchronize failed: %s\n",
				mcGetErrorName(cres));
		}
	}

	/* All in-flight IOs on the shared stream are now complete */
	for (i = 0; i < ad->entries && events < (int)max; i++) {
		struct libmcfile_async_slot *slot = &ad->slots[i];

		if (slot->state == ASYNC_SLOT_IN_FLIGHT) {
			ad->completed_slots[events++] = i;
			slot->state = ASYNC_SLOT_FREE;
			ad->in_flight--;
		}
	}

	return events;
}

static struct io_u *fio_libmcfile_async_event_async(struct thread_data *td,
						    int event)
{
	struct libmcfile_async_options *o = td->eo;
	struct libmcfile_async_data *ad = td->io_ops_data;
	unsigned int idx = ad->completed_slots[ad->completed_head + event];
	struct libmcfile_async_slot *slot = &ad->slots[idx];
	struct io_u *io_u;

	io_u = slot->io_u;

	if (slot->bytes_done < 0) {
		io_u->error = EIO;
		log_err("mcFile async IO failed: bytes_done=%zd\n",
			slot->bytes_done);
	} else if ((size_t) slot->bytes_done != io_u->xfer_buflen) {
		io_u->resid = io_u->xfer_buflen - slot->bytes_done;
	} else {
		io_u->error = 0;
	}

	if (io_u->ddir == DDIR_READ && io_u->error == 0)
		fio_libmcfile_async_post_read(td, o, io_u, slot->gpu_offset);

	slot->io_u = NULL;
	return io_u;
}

/* ---- shared dispatch ----------------------------------------------------- */

static enum fio_q_status fio_libmcfile_async_queue(struct thread_data *td,
						    struct io_u *io_u)
{
	struct libmcfile_async_data *ad = td->io_ops_data;
	struct fio_libmcfile_async_file_data *fcd = FILE_ENG_DATA(io_u->file);

	if (fcd == NULL) {
		io_u->error = EINVAL;
		td_verror(td, EINVAL, "xfer");
		return FIO_Q_COMPLETED;
	}

	fio_ro_check(td, io_u);

	if (ddir_sync(io_u->ddir) || io_u->ddir == DDIR_TRIM ||
	    io_u->ddir == DDIR_SYNCFS)
		return fio_libmcfile_async_queue_sync(td, io_u);

	if (io_u->ddir != DDIR_READ && io_u->ddir != DDIR_WRITE) {
		io_u->error = EINVAL;
		td_verror(td, EINVAL, "xfer");
		return FIO_Q_COMPLETED;
	}

	if (ad->api == MCFILE_API_BATCH)
		return fio_libmcfile_async_queue_batch(td, io_u);

	return fio_libmcfile_async_queue_async(td, io_u);
}

static int fio_libmcfile_async_commit(struct thread_data *td)
{
	struct libmcfile_async_data *ad = td->io_ops_data;

	if (ad->api == MCFILE_API_BATCH)
		return fio_libmcfile_async_commit_batch(td);

	return fio_libmcfile_async_commit_async(td);
}

static int fio_libmcfile_async_getevents(struct thread_data *td, unsigned int min,
					 unsigned int max, const struct timespec *t)
{
	struct libmcfile_async_data *ad = td->io_ops_data;

	if (ad->api == MCFILE_API_BATCH)
		return fio_libmcfile_async_getevents_batch(td, min, max, t);

	return fio_libmcfile_async_getevents_async(td, min, max, t);
}

static struct io_u *fio_libmcfile_async_event(struct thread_data *td, int event)
{
	struct libmcfile_async_data *ad = td->io_ops_data;

	if (ad->api == MCFILE_API_BATCH)
		return fio_libmcfile_async_event_batch(td, event);

	return fio_libmcfile_async_event_async(td, event);
}

/* ---- file / memory / cleanup -------------------------------------------- */

static int fio_libmcfile_async_open_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_libmcfile_async_file_data *fcd = NULL;
	int rc;
	MCfileError_t status;

	rc = generic_open_file(td, f);
	if (rc)
		return rc;

	fcd = calloc(1, sizeof(*fcd));
	if (fcd == NULL) {
		rc = ENOMEM;
		goto exit_err;
	}

	fcd->cf_descr.handle.fd = f->fd;
	fcd->cf_descr.type = MC_FILE_HANDLE_TYPE_OPAQUE_FD;
	status = mcFileHandleRegister(&fcd->cf_handle, &fcd->cf_descr);
	if (status.err != MC_FILE_SUCCESS) {
		log_err("mcfile register: err=%d:%s\n", status.err,
			fio_libmcfile_async_get_mc_error(status));
		rc = EINVAL;
		goto exit_err;
	}

	FILE_SET_ENG_DATA(f, fcd);
	return 0;

exit_err:
	if (fcd)
		free(fcd);
	{
		int rc2 = generic_close_file(td, f);
		if (rc2)
			log_err("generic_close_file: err=%d\n", rc2);
	}
	return rc;
}

static int fio_libmcfile_async_close_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_libmcfile_async_file_data *fcd = FILE_ENG_DATA(f);

	if (fcd != NULL) {
		mcFileHandleDeregister(fcd->cf_handle);
		FILE_SET_ENG_DATA(f, NULL);
		free(fcd);
	}

	return generic_close_file(td, f);
}

static int fio_libmcfile_async_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	struct libmcfile_async_options *o = td->eo;
	int rc;
	MCfileError_t status;

	o->total_mem = total_mem;
	o->logged = 0;
	o->dev_mem_ptr = NULL;
	td->orig_buffer = calloc(1, total_mem);
	if (!td->orig_buffer) {
		log_err("orig_buffer calloc failed: err=%d\n", errno);
		goto exit_error;
	}

	dprint(FD_MEM, "Alloc %zu for GPU %d\n", total_mem, o->my_gpu_id);

	check_mcruntimecall(mcMalloc(&o->dev_mem_ptr, total_mem), rc);
	if (rc != 0)
		goto exit_error;
	check_mcruntimecall(mcMemset(o->dev_mem_ptr, 0xab, total_mem), rc);
	if (rc != 0)
		goto exit_error;

	status = mcFileBufRegister(o->dev_mem_ptr, total_mem, 0);
	if (status.err != MC_FILE_SUCCESS) {
		log_err("mcFileBufRegister: err=%d:%s\n", status.err,
			fio_libmcfile_async_get_mc_error(status));
		goto exit_error;
	}

	return 0;

exit_error:
	if (td->orig_buffer) {
		free(td->orig_buffer);
		td->orig_buffer = NULL;
	}
	if (o->dev_mem_ptr) {
		mcFree(o->dev_mem_ptr);
		o->dev_mem_ptr = NULL;
	}
	return 1;
}

static void fio_libmcfile_async_iomem_free(struct thread_data *td)
{
	struct libmcfile_async_options *o = td->eo;

	if (o->dev_mem_ptr) {
		mcFileBufDeregister(o->dev_mem_ptr);
		mcFree(o->dev_mem_ptr);
		o->dev_mem_ptr = NULL;
	}
	if (td->orig_buffer) {
		free(td->orig_buffer);
		td->orig_buffer = NULL;
	}
}

static void fio_libmcfile_async_cleanup(struct thread_data *td)
{
	struct libmcfile_async_data *ad = td->io_ops_data;

	if (ad) {
		if (ad->api == MCFILE_API_BATCH) {
			if (ad->batch_handle)
				mcFileBatchIODestroy(ad->batch_handle);
			free(ad->io_params);
			free(ad->io_events);
		} else {
			if (ad->stream)
				mcStreamDestroy(ad->stream);
			free(ad->slots);
			free(ad->pending_slots);
			free(ad->completed_slots);
		}
		free(ad);
		td->io_ops_data = NULL;
	}

	pthread_mutex_lock(&running_lock);
	running--;
	assert(running >= 0);
	if (running == 0) {
		/* only close the driver if initialized and
		   this is the last worker thread */
		if (mcfile_initialized)
			mcFileDriverClose();
		mcfile_initialized = 0;
	}
	pthread_mutex_unlock(&running_lock);
}

FIO_STATIC struct ioengine_ops ioengine = {
	.name                = "libmcfile_async",
	.version             = FIO_IOOPS_VERSION,
	.init                = fio_libmcfile_async_init,
	.post_init           = fio_libmcfile_async_post_init,
	.queue               = fio_libmcfile_async_queue,
	.commit              = fio_libmcfile_async_commit,
	.getevents           = fio_libmcfile_async_getevents,
	.event               = fio_libmcfile_async_event,
	.get_file_size       = generic_get_file_size,
	.open_file           = fio_libmcfile_async_open_file,
	.close_file          = fio_libmcfile_async_close_file,
	.iomem_alloc         = fio_libmcfile_async_iomem_alloc,
	.iomem_free          = fio_libmcfile_async_iomem_free,
	.cleanup             = fio_libmcfile_async_cleanup,
	.flags               = FIO_ASYNCIO_SETS_ISSUE_TIME,
	.options             = options,
	.option_struct_size  = sizeof(struct libmcfile_async_options)
};

void fio_init fio_libmcfile_async_register(void)
{
	register_ioengine(&ioengine);
}

void fio_exit fio_libmcfile_async_unregister(void)
{
	unregister_ioengine(&ioengine);
}
