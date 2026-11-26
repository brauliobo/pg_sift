/*
 * agg/gpu/worker.c - the persistent GPU worker and the connection of a backend to it (agg/gpu/shm.h), and the device of a backend of its own where the worker cannot be had.
 *
 * Nothing of this runs with sift.gpu = off. A build without SIFT_VULKAN has no device: every connection is refused.
 */
#include "postgres.h"

#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "agg/gpu/shm.h"
#include "agg/gpu/scan.h"
#include "agg/vk/layout.h"
#include "col/dsmreg.h"
#include "core/memtrack.h"
#include "core/memtrack.h"

#ifndef SIFT_VULKAN

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
SiftGpuConn *
sift_gpu_conn_open(bool allow_local, const char **why)
{
	*why = "this build has no Vulkan support (build with VULKAN=1)";
	return NULL;
}

void
sift_gpu_conn_close(SiftGpuConn *c)
{
}

const SiftVkCaps *
sift_gpu_conn_caps(const SiftGpuConn *c)
{
	return NULL;
}

bool
sift_gpu_conn_shared(const SiftGpuConn *c)
{
	return false;
}

uint64
sift_gpu_conn_batch_bytes(const SiftGpuConn *c)
{
	return 0;
}

uint64
sift_gpu_conn_result_bytes(const SiftGpuConn *c)
{
	return 0;
}

void
sift_gpu_conn_resident(SiftGpuConn *c, uint64 tables, const SiftVkKey *keys, uint32 n, uint8 *resident)
{
	memset(resident, 0, n);
}

SiftVkStatus
sift_gpu_conn_run(SiftGpuConn *c, const SiftVkJob *job, SiftVkResult *res)
{
	return SIFT_VK_FAIL;
}

const char *
sift_gpu_conn_error(const SiftGpuConn *c)
{
	return "this build has no Vulkan support";
}

void
sift_gpu_worker_main(Datum main_arg)
{
	proc_exit(0);
}

#pragma GCC diagnostic pop

#else							/* SIFT_VULKAN */

#define LOCAL_BATCH_BYTES		((uint64) 128 << 20)
#define WAIT_SLICE_MS			5
#define START_WAIT_MS			2000
#define SLOT_WAIT_MS			30000
#define FAILED_RETRY_US			(30 * USECS_PER_SEC)
#define ALIGN8(x)				TYPEALIGN(8, (x))

/* What the backend that registers the worker passes to it: the device and the cache it will be asked for */
typedef struct WorkerExtra
{
	uint32		cache_mb;
	bool		software;
	char		device[64];
} WorkerExtra;

StaticAssertDecl(sizeof(WorkerExtra) <= BGW_EXTRALEN, "bgw_extra holds the device the worker opens");

/* ---- running a job on a device: the worker's, or the backend's own ---- */

static bool
query_cancelled(void)
{
	return QueryCancelPending || ProcDiePending;
}

static void
account_to(void *arg, int64 delta)
{
	pg_atomic_fetch_add_u64((pg_atomic_uint64 *) arg, (uint64) delta);
}

/* ---- the job in the staging area ---- */

static bool
wire_pack(char *stage, SiftGpuWireJob *wj, const SiftVkJob *job, const char **why)
{
	Size		nchunks = 0,
				ndead = 0,
				data = 0,
				off;
	SiftGpuWireChunk *wc;
	SiftGpuWireGran *wg;
	char	   *dead,
			   *bytes;
	uint32		k = 0;

	for (uint32 g = 0; g < job->ngran; g++)
	{
		ndead += job->gran[g].dead != NULL;
		for (uint32 c = 0; c < job->ncols; c++)
		{
			const SiftVkChunk *ch = job->gran[g].col[c];

			nchunks += ch != NULL;
			data += ch != NULL && ch->bytes != NULL ? ALIGN8(ch->len) : 0;
		}
	}
	wj->chunk_off = 0;
	wj->gran_off = nchunks * sizeof(SiftGpuWireChunk);
	wj->ops_off = wj->gran_off + (Size) job->ngran * sizeof(SiftGpuWireGran);
	wj->pool_off = wj->ops_off + ALIGN8((Size) job->nops * SIFT_VK_OP_WORDS * 4);
	off = wj->pool_off + ALIGN8((Size) job->npool * 4);
	dead = stage + off;
	bytes = dead + ndead * 1024;
	if (off + ndead * 1024 + data > SIFT_GPU_STAGE_BYTES)
	{
		*why = "the job does not fit the staging area of the worker";
		return false;
	}
	wc = (SiftGpuWireChunk *) (stage + wj->chunk_off);
	wg = (SiftGpuWireGran *) (stage + wj->gran_off);
	for (uint32 g = 0; g < job->ngran; g++, wg++)
	{
		const SiftVkGranule *sg = &job->gran[g];

		wg->id = sg->id;
		wg->nrows = sg->nrows;
		wg->dead_off = SIFT_GPU_NOT_THERE;
		if (sg->dead != NULL)
		{
			memcpy(dead, sg->dead, 1024);
			wg->dead_off = dead - stage;
			dead += 1024;
		}
		for (uint32 c = 0; c < SIFT_VK_MAXCOLS; c++)
		{
			const SiftVkChunk *ch = c < job->ncols ? sg->col[c] : NULL;

			wg->col[c] = ch == NULL ? UINT32_MAX : k;
			if (ch == NULL)
				continue;
			wc[k] = (SiftGpuWireChunk) {ch->key, ch->len, ch->enc, ch->flags, ch->nrows, ch->nnonnull, ch->vlen, ch->rawlen, ch->w, ch->base, SIFT_GPU_NOT_THERE};
			if (ch->bytes != NULL)
			{
				memcpy(bytes, ch->bytes, ch->len);
				wc[k].data_off = bytes - stage;
				bytes += ALIGN8(ch->len);
			}
			k++;
		}
	}
	memcpy(stage + wj->ops_off, job->ops, (Size) job->nops * SIFT_VK_OP_WORDS * 4);
	if (job->npool > 0)
		memcpy(stage + wj->pool_off, job->pool, (Size) job->npool * 4);
	wj->mode = job->mode;
	wj->ncols = job->ncols;
	wj->ngran = job->ngran;
	wj->nchunks = (uint32) nchunks;
	wj->nops = job->nops;
	wj->npool = job->npool;
	wj->where_slot = job->where_slot;
	wj->nkeys = job->nkeys;
	memcpy(wj->key_reg, job->key_reg, sizeof(wj->key_reg));
	wj->naggs = job->naggs;
	memcpy(wj->agg, job->agg, sizeof(wj->agg));
	memcpy(wj->dense_kmin, job->dense_kmin, sizeof(wj->dense_kmin));
	memcpy(wj->dense_span, job->dense_span, sizeof(wj->dense_span));
	wj->est_groups = job->est_groups;
	wj->tables = job->tables;
	wj->finish = job->finish;
	wj->topk = job->topk;
	wj->topn_k = job->topn_k;
	wj->topn_desc = job->topn_desc;
	wj->topn_nulls = job->topn_nulls;
	wj->sort_float = job->sort_float;
	wj->sort_reg = job->sort_reg;
	wj->max_cand = job->max_cand;
	wj->tab = job->tab;
	return true;
}

/* The job as the device runs it: its arrays in the current memory context, its bytes in the staging area */
static SiftVkJob
wire_unpack(char *stage, const SiftGpuWireJob *wj)
{
	const SiftGpuWireChunk *wc = (const SiftGpuWireChunk *) (stage + wj->chunk_off);
	const SiftGpuWireGran *wg = (const SiftGpuWireGran *) (stage + wj->gran_off);
	SiftVkChunk *chunk = palloc(Max(wj->nchunks, 1) * sizeof(SiftVkChunk));
	SiftVkGranule *gran = palloc0(Max(wj->ngran, 1) * sizeof(SiftVkGranule));
	SiftVkJob	job = {.mode = wj->mode,.ncols = wj->ncols,.ngran = wj->ngran,.nops = wj->nops,.npool = wj->npool,.where_slot = wj->where_slot,
	.nkeys = wj->nkeys,.naggs = wj->naggs,.est_groups = wj->est_groups,.tables = wj->tables,.finish = wj->finish,.topk = wj->topk,.topn_k = wj->topn_k,.topn_desc = wj->topn_desc,
	.topn_nulls = wj->topn_nulls,.sort_float = wj->sort_float,.sort_reg = wj->sort_reg,.max_cand = wj->max_cand,.tab = wj->tab};

	for (uint32 k = 0; k < wj->nchunks; k++)
		chunk[k] = (SiftVkChunk) {.key = wc[k].key,.bytes = wc[k].data_off == SIFT_GPU_NOT_THERE ? NULL : stage + wc[k].data_off,.len = wc[k].len,.enc = wc[k].enc,
		.flags = wc[k].flags,.nrows = wc[k].nrows,.nnonnull = wc[k].nnonnull,.vlen = wc[k].vlen,.rawlen = wc[k].rawlen,.w = wc[k].w,.base = wc[k].base};
	for (uint32 g = 0; g < wj->ngran; g++)
	{
		gran[g].id = wg[g].id;
		gran[g].nrows = wg[g].nrows;
		gran[g].dead = wg[g].dead_off == SIFT_GPU_NOT_THERE ? NULL : (const uint32 *) (stage + wg[g].dead_off);
		for (uint32 c = 0; c < SIFT_VK_MAXCOLS; c++)
			gran[g].col[c] = wg[g].col[c] == UINT32_MAX ? NULL : &chunk[wg[g].col[c]];
	}
	memcpy(job.key_reg, wj->key_reg, sizeof(job.key_reg));
	memcpy(job.agg, wj->agg, sizeof(job.agg));
	memcpy(job.dense_kmin, wj->dense_kmin, sizeof(job.dense_kmin));
	memcpy(job.dense_span, wj->dense_span, sizeof(job.dense_span));
	job.gran = gran;
	job.ops = (const uint32 *) (stage + wj->ops_off);
	job.pool = (const uint32 *) (stage + wj->pool_off);
	return job;
}

/* ---- the worker ---- */

typedef struct WorkerState
{
	SiftGpuShm *shm;
	SiftVkSeat	seat;
	MemoryContext jobcx;		/* NULL between jobs: nothing of the worker is a context of its own while it waits */
	MemoryContext basecx;
	const SiftGpuSlot *cur;
} WorkerState;

static WorkerState *worker;

static void
heartbeat(SiftGpuShm *shm)
{
	pg_atomic_write_u64(&shm->heartbeat_us, (uint64) GetCurrentTimestamp());
}

static void
wake(const SiftGpuSlot *slot)
{
	SetLatch(&GetPGProcByNumber(slot->owner_procno)->procLatch);
}

static bool
worker_cancelled(void)
{
	heartbeat(worker->shm);
	return pg_atomic_read_u32((pg_atomic_uint32 *) &worker->cur->cancelled) != 0 || ProcDiePending;
}

static void
shm_init(SIFT_DSM_INIT_ARGS)
{
	SiftGpuShm *shm = ptr;

	memset(shm, 0, sizeof(*shm));
	shm->magic = 0x47505536;
	shm->version = SIFT_GPU_SHM_VERSION;
	pg_atomic_init_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	pg_atomic_init_u32(&shm->worker_pid, 0);
	pg_atomic_init_u64(&shm->heartbeat_us, 0);
	pg_atomic_init_u64(&shm->failed_at_us, 0);
	pg_atomic_init_u64(&shm->host_bytes, 0);
	for (int i = 0; i < SIFT_GPU_SLOTS; i++)
	{
		pg_atomic_init_u32(&shm->slot[i].state, SIFT_GPU_S_FREE);
		pg_atomic_init_u32(&shm->slot[i].cancelled, 0);
	}
}

static SiftGpuShm *
attach_shm(void)
{
	static SiftGpuShm *shm;

	if (shm == NULL)
		shm = sift_dsm_try(SIFT_GPU_SHM_NAME, SIFT_GPU_SHM_BYTES, shm_init);
	return shm;
}

/* The worker ends, whatever the way: the state says so and every job it holds is lost, so that no backend waits for it */
static void
worker_exit(pg_attribute_unused() int code, Datum arg)
{
	SiftGpuShm *shm = (SiftGpuShm *) DatumGetPointer(arg);

	pg_atomic_write_u32(&shm->worker_pid, 0);
	if (pg_atomic_read_u32(&shm->worker_state) != SIFT_GPU_W_FAILED)
		pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	for (int i = 0; i < SIFT_GPU_SLOTS; i++)
	{
		SiftGpuSlot *s = &shm->slot[i];
		uint32		st = pg_atomic_read_u32(&s->state);

		if (st != SIFT_GPU_S_SUBMITTED && st != SIFT_GPU_S_RUNNING)
			continue;
		if (pg_atomic_read_u32(&s->cancelled) != 0)
			pg_atomic_write_u32(&s->state, SIFT_GPU_S_FREE);
		else
		{
			s->status = SIFT_VK_LOST;
			snprintf(s->error, sizeof(s->error), "the GPU worker ended");
			pg_atomic_write_u32(&s->state, SIFT_GPU_S_DONE);
			wake(s);
		}
	}
	if (worker->jobcx != NULL)
	{
		MemoryContextSwitchTo(worker->basecx);
		MemoryContextDelete(worker->jobcx);
		worker->jobcx = NULL;
	}
	sift_vk_seat_close(&worker->seat);
}

/* The device the job asks for (agg/vk/seat.c): one that was lost or timed out, or that was opened for other settings, is opened again, a change of the cache resizes it (the worker runs one job at a time: no job is in flight) */
static bool
worker_open(WorkerState *w, const char *device, bool software, uint64 cache_bytes, char *err, size_t errlen)
{
	const uint32 opens = w->seat.opens;
	SiftVk	   *vk = sift_vk_seat_follow(&w->seat, device, software, cache_bytes);

	if (vk == NULL)
	{
		snprintf(err, errlen, "%s", w->seat.err);
		return false;
	}
	if (w->seat.opens != opens)
	{
		sift_vk_set_account(vk, account_to, &w->shm->host_bytes);
		sift_vk_set_envelope(vk, SIFT_VK_VRAM_ENVELOPE, SIFT_VK_VRAM_FIXED);
		w->shm->caps = *sift_vk_caps(vk);
	}
	return true;
}

static bool
worker_follow(WorkerState *w, const SiftGpuSlot *s, char *err, size_t errlen)
{
	if (!worker_open(w, s->device, s->software, (uint64) s->cache_mb << 20, err, errlen))
		return false;
	if (!s->software && sift_vk_caps(w->seat.vk)->software)
	{
		snprintf(err, errlen, "the only device is a software one, which sift.gpu = on alone may use");
		return false;
	}
	return true;
}

static void
serve_resident(WorkerState *w, SiftGpuSlot *s, char *stage)
{
	const SiftVkKey *keys = (const SiftVkKey *) stage;
	uint8	   *out = (uint8 *) stage + ALIGN8((Size) s->nkeys * sizeof(SiftVkKey));

	s->status = sift_vk_fit_cache(w->seat.vk, s->tables) ? SIFT_VK_OK : SIFT_VK_FAIL;
	if (s->status == SIFT_VK_OK)
		sift_vk_resident_batch(w->seat.vk, keys, s->nkeys, out);
}

static void
serve_run(WorkerState *w, SiftGpuSlot *s, char *stage)
{
	SiftVkJob	job = wire_unpack(stage, &s->job);
	SiftVkResult res;
	SiftVkStatus st = sift_vk_run(w->seat.vk, &job, worker_cancelled, &res);
	uint64		acc = (uint64) res.acc_words * 8,
				cand = (uint64) res.ncand * 16;

	if (st == SIFT_VK_OK && ALIGN8(acc) + cand > SIFT_GPU_STAGE_BYTES)
	{
		st = SIFT_VK_OVERFLOW;
		snprintf(s->error, sizeof(s->error), "the result does not fit the staging area of the worker");
	}
	else if (st == SIFT_VK_OK)
	{
		if (acc > 0)
			memcpy(stage, res.acc, acc);
		if (cand > 0)
			memcpy(stage + ALIGN8(acc), res.cand, cand);
	}
	else if (st != SIFT_VK_PROG_ERROR && st != SIFT_GPU_NEED_BYTES)
		strlcpy(s->error, sift_vk_error(w->seat.vk), sizeof(s->error));
	s->acc_bytes = acc;
	s->cand_bytes = cand;
	s->res = res;
	s->status = st;
}

/* Of a job whose backend left: whichever of the worker (done after it) and the backend (cancelled before it) comes last frees the slot, for the other may not have seen it */
static void
slot_free_done(SiftGpuSlot *s)
{
	uint32		st = SIFT_GPU_S_DONE;

	pg_atomic_compare_exchange_u32(&s->state, &st, SIFT_GPU_S_FREE);
}

/* MEM[MEM-VK-01]: gpu worker jobcx ("sift gpu job" made for each job and deleted when it ends, so none is live while the worker waits or exits idle: the leak gate of leak_test.c finds nothing) */
static void
serve(WorkerState *w, SiftGpuSlot *s, char *stage)
{
	MemoryContext old = MemoryContextSwitchTo(w->basecx);

	w->jobcx = sift_mem_allocset_create(w->basecx, "sift gpu job");
	MemoryContextSwitchTo(w->jobcx);
	w->cur = s;
	s->error[0] = '\0';
	PG_TRY();
	{
		char		err[160];

		if (!worker_follow(w, s, err, sizeof(err)))
		{
			s->status = SIFT_VK_FAIL;
			strlcpy(s->error, err, sizeof(s->error));
		}
		else if (s->kind == SIFT_GPU_REQ_RESIDENT)
			serve_resident(w, s, stage);
		else
			serve_run(w, s, stage);
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(w->jobcx);
		ed = CopyErrorData();
		FlushErrorState();
		s->status = SIFT_VK_FAIL;
		strlcpy(s->error, ed->message, sizeof(s->error));
		if (pg_atomic_read_u32(&s->cancelled) == 0)
			ereport(LOG, (errmsg("sift: GPU worker: %s", ed->message)));
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);
	sift_mem_release_os(MemoryContextMemAllocated(w->jobcx, true));
	MemoryContextDelete(w->jobcx);
	w->jobcx = NULL;
	w->cur = NULL;
	pg_atomic_write_u32(&s->state, SIFT_GPU_S_DONE);
	wake(s);
	if (pg_atomic_read_u32(&s->cancelled) != 0)
		slot_free_done(s);
}

/* The worker idle: the device is closed with its buffers, cache and compiled kernels (the next job opens it again: what the driver keeps of a device grows with the kernels it ran), the pages of the free staging areas leave this process, and the heap goes back to the system */
static void
worker_idle_release(WorkerState *w)
{
	const Size	page = sysconf(_SC_PAGESIZE);

	sift_vk_seat_close(&w->seat);
	for (int i = 0; i < SIFT_GPU_SLOTS; i++)
		if (pg_atomic_read_u32(&w->shm->slot[i].state) == SIFT_GPU_S_FREE)
			(void) madvise((void *) TYPEALIGN(page, sift_gpu_stage(w->shm, i)), SIFT_GPU_STAGE_BYTES - page, MADV_DONTNEED);
	sift_mem_trim_os();
}

static void
worker_loop(WorkerState *w, const WorkerExtra *extra)
{
	SiftGpuShm *shm = w->shm;
	TimestampTz last_job = GetCurrentTimestamp();
	bool		released = false;
	char		err[160];

	if (!worker_open(w, extra->device, extra->software, (uint64) extra->cache_mb << 20, err, sizeof(err)))
	{
		strlcpy(shm->last_error, err, sizeof(shm->last_error));
		pg_atomic_write_u64(&shm->failed_at_us, (uint64) GetCurrentTimestamp());
		pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_FAILED);
		return;
	}
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_READY);
	for (;;)
	{
		bool		did = false;

		ResetLatch(MyLatch);
		heartbeat(shm);
		for (int i = 0; i < SIFT_GPU_SLOTS; i++)
		{
			uint32		st = SIFT_GPU_S_SUBMITTED;

			if (!pg_atomic_compare_exchange_u32(&shm->slot[i].state, &st, SIFT_GPU_S_RUNNING))
				continue;
			did = true;
			if (pg_atomic_read_u32(&shm->slot[i].cancelled) != 0)
				pg_atomic_write_u32(&shm->slot[i].state, SIFT_GPU_S_FREE);
			else
				serve(w, &shm->slot[i], sift_gpu_stage(shm, i));
		}
		CHECK_FOR_INTERRUPTS();
		if (did)
		{
			last_job = GetCurrentTimestamp();
			released = false;
		}
		else if (GetCurrentTimestamp() - last_job > SIFT_GPU_IDLE_US)
			break;
		else
		{
			if (!released && GetCurrentTimestamp() - last_job > SIFT_GPU_RELEASE_US)
			{
				worker_idle_release(w);
				released = true;
			}
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1000, PG_WAIT_EXTENSION);
		}
	}
	sift_vk_seat_close(&w->seat);
}

void
sift_gpu_worker_main(pg_attribute_unused() Datum main_arg)
{
	WorkerState w = {0};
	WorkerExtra extra;

	memcpy(&extra, MyBgworkerEntry->bgw_extra, sizeof(extra));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	w.shm = attach_shm();
	if (w.shm == NULL)
		proc_exit(1);
	w.basecx = CurrentMemoryContext;
	worker = &w;
	before_shmem_exit(worker_exit, PointerGetDatum(w.shm));
	w.shm->worker_procno = MyProcNumber;
	pg_atomic_write_u32(&w.shm->worker_pid, (uint32) MyProcPid);
	heartbeat(w.shm);
	worker_loop(&w, &extra);
	proc_exit(0);
}

/* ---- a backend ---- */

struct SiftGpuConn
{
	SiftGpuShm *shm;			/* NULL: the device is the backend's own */
	SiftVk	   *vk;
	SiftVkCaps	caps;
	char		error[200];
	/* MEM[MEM-VK-10]: GPU client result context rcx (the copy of a result, at most the staging area of 4MiB; the heap goes back to the system when the connection closes) */
	MemoryContext rcx;
};

static bool
worker_alive(const SiftGpuShm *shm)
{
	uint32		pid = pg_atomic_read_u32((pg_atomic_uint32 *) &shm->worker_pid);

	return pid != 0 && (kill((pid_t) pid, 0) == 0 || errno != ESRCH);
}

static bool
worker_fresh(SiftGpuShm *shm)
{
	return (uint64) GetCurrentTimestamp() - pg_atomic_read_u64(&shm->heartbeat_us) < (uint64) SIFT_GPU_STALE_US || worker_alive(shm);
}

static bool
register_worker(SiftGpuShm *shm)
{
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	WorkerExtra extra = {.cache_mb = (uint32) sift_gpu_cache_mb,.software = sift_gpu_mode == SIFT_GPU_ON};
	pid_t		pid;

	strlcpy(extra.device, sift_gpu_device, sizeof(extra.device));
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bw.bgw_start_time = BgWorkerStart_ConsistentState;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	bw.bgw_notify_pid = MyProcPid;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_gpu_worker_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift gpu worker");
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift gpu");
	memcpy(bw.bgw_extra, &extra, sizeof(extra));
	if (RegisterDynamicBackgroundWorker(&bw, &handle) && WaitForBackgroundWorkerStartup(handle, &pid) == BGWH_STARTED)
		return true;
	strlcpy(shm->last_error, "no background worker could be started (max_worker_processes)", sizeof(shm->last_error));
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	return false;
}

/* True when the worker is up and ready; else why not. The first backend that finds none registers it and waits for its device to open */
static bool
ensure_worker(SiftGpuShm *shm, const char **why)
{
	for (int waited = 0; waited <= START_WAIT_MS;)
	{
		uint32		st = pg_atomic_read_u32(&shm->worker_state);

		if (st == SIFT_GPU_W_READY && worker_fresh(shm))
			return true;
		if (st == SIFT_GPU_W_FAILED && (uint64) GetCurrentTimestamp() - pg_atomic_read_u64(&shm->failed_at_us) < (uint64) FAILED_RETRY_US)
		{
			*why = shm->last_error;
			return false;
		}
		if (st != SIFT_GPU_W_STARTING && pg_atomic_compare_exchange_u32(&shm->worker_state, &st, SIFT_GPU_W_STARTING))
		{
			if (!register_worker(shm))
			{
				*why = shm->last_error;
				return false;
			}
			continue;
		}
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, WAIT_SLICE_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		waited += WAIT_SLICE_MS;
	}
	if (pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_STARTING && !worker_alive(shm))
		pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	*why = shm->last_error[0] ? shm->last_error : "the GPU worker did not start in time";
	return false;
}

/*
 * Whether the host has a device that sift.gpu = auto may use, asked once by the backend for each value of sift.gpu_device. A host with none (no loader, no driver, a software device only) has none for
 * every query: the first backend that finds a table of the worker does not ask (a worker of the server saw a device), and where there is no table, a headless host makes none (16 MiB of shared memory
 * and a worker that fails to open its device again every FAILED_RETRY_US).
 */
static bool
host_has_device(const char **why)
{
	static struct
	{
		bool		probed,
					found;
		char		device[NAMEDATALEN];
		char		why[160];
	}			host;
	SiftVkDevInfo devs[16];
	int			chosen = -1;

	if (!host.probed || strcmp(host.device, sift_gpu_device) != 0)
	{
		const int	n = sift_vk_list_devices(devs, lengthof(devs), &chosen, sift_gpu_device, false, host.why, sizeof(host.why));

		if (n >= 0 && chosen < 0)
			strlcpy(host.why, "no Vulkan device that sift.gpu = auto may use", sizeof(host.why));
		host.found = chosen >= 0;
		host.probed = true;
		strlcpy(host.device, sift_gpu_device, sizeof(host.device));
	}
	*why = host.why;
	return host.found;
}

SiftGpuConn *
sift_gpu_conn_open(bool allow_local, const char **why)
{
	SiftGpuShm *shm;
	SiftGpuConn *c;

	if (sift_gpu_mode == SIFT_GPU_AUTO && sift_dsm_find(SIFT_GPU_SHM_NAME, NULL) == NULL && !host_has_device(why))
		return NULL;
	shm = attach_shm();
	c = palloc0(sizeof(SiftGpuConn));
	if (shm != NULL && ensure_worker(shm, why))
	{
		c->shm = shm;
		c->caps = shm->caps;
	}
	else if (allow_local && (c->vk = sift_gpu_local_open(why)) != NULL)
		c->caps = *sift_vk_caps(c->vk);
	else
	{
		pfree(c);
		return NULL;
	}
	c->rcx = sift_mem_allocset_create(CurrentMemoryContext, "sift gpu result");
	return c;
}

void
sift_gpu_conn_close(SiftGpuConn *c)
{
	if (c == NULL)
		return;
	sift_mem_release_os(MemoryContextMemAllocated(c->rcx, true));
	MemoryContextDelete(c->rcx);
	pfree(c);
}

const SiftVkCaps *
sift_gpu_conn_caps(const SiftGpuConn *c)
{
	return &c->caps;
}

bool
sift_gpu_conn_shared(const SiftGpuConn *c)
{
	return c->shm != NULL;
}

uint64
sift_gpu_conn_batch_bytes(const SiftGpuConn *c)
{
	return c->shm != NULL ? SIFT_GPU_STAGE_BYTES / 4 * 3 : LOCAL_BATCH_BYTES;
}

uint64
sift_gpu_conn_result_bytes(const SiftGpuConn *c)
{
	return c->shm != NULL ? SIFT_GPU_STAGE_BYTES : UINT64_MAX;
}

const char *
sift_gpu_conn_error(const SiftGpuConn *c)
{
	return c->shm != NULL ? c->error : sift_vk_error(c->vk);
}

static bool
owner_gone(const SiftGpuSlot *s)
{
	return s->owner_pid != 0 && kill((pid_t) s->owner_pid, 0) != 0 && errno == ESRCH;
}

static int
slot_claim(SiftGpuShm *shm)
{
	for (int waited = 0; waited < SLOT_WAIT_MS; waited += WAIT_SLICE_MS)
	{
		for (int i = 0; i < SIFT_GPU_SLOTS; i++)
		{
			SiftGpuSlot *s = &shm->slot[i];
			uint32		st = pg_atomic_read_u32(&s->state);

			if ((st == SIFT_GPU_S_CLAIMED || st == SIFT_GPU_S_DONE) && owner_gone(s))
				pg_atomic_compare_exchange_u32(&s->state, &st, SIFT_GPU_S_FREE);
			st = SIFT_GPU_S_FREE;
			if (pg_atomic_compare_exchange_u32(&s->state, &st, SIFT_GPU_S_CLAIMED))
			{
				s->owner_pid = MyProcPid;
				s->owner_procno = MyProcNumber;
				pg_atomic_write_u32(&s->cancelled, 0);
				return i;
			}
		}
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, WAIT_SLICE_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	return -1;
}

/* A backend that leaves its slot (an error, a cancel): the worker drops the job and frees it; one that never submitted, or whose worker is gone, frees it itself */
static void
slot_abandon(SiftGpuShm *shm, SiftGpuSlot *s)
{
	uint32		st;

	pg_atomic_write_u32(&s->cancelled, 1);
	st = pg_atomic_read_u32(&s->state);
	if (st == SIFT_GPU_S_CLAIMED || !worker_alive(shm))
		pg_atomic_write_u32(&s->state, SIFT_GPU_S_FREE);
	else
	{
		slot_free_done(s);
		SetLatch(&GetPGProcByNumber(shm->worker_procno)->procLatch);
	}
}

/* True when the worker answered; false when it is gone */
static bool
slot_wait(SiftGpuShm *shm, SiftGpuSlot *s)
{
	for (;;)
	{
		if (pg_atomic_read_u32(&s->state) == SIFT_GPU_S_DONE)
			return true;
		if (pg_atomic_read_u32(&shm->worker_state) != SIFT_GPU_W_READY || !worker_fresh(shm))
			return false;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, WAIT_SLICE_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* Submits what the caller put in slot i and waits for the answer; the slot is the caller's again when this returns true */
static bool
submit(SiftGpuConn *c, int i)
{
	SiftGpuShm *shm = c->shm;
	SiftGpuSlot *s = &shm->slot[i];

	pg_atomic_write_u32(&s->state, SIFT_GPU_S_SUBMITTED);
	SetLatch(&GetPGProcByNumber(shm->worker_procno)->procLatch);
	if (slot_wait(shm, s))
		return true;
	snprintf(c->error, sizeof(c->error), "the GPU worker is gone");
	slot_abandon(shm, s);
	return false;
}

static void
fill_request(SiftGpuSlot *s, uint32 kind)
{
	s->kind = kind;
	s->cache_mb = (uint32) sift_gpu_cache_mb;
	s->software = sift_gpu_mode == SIFT_GPU_ON;
	strlcpy(s->device, sift_gpu_device, sizeof(s->device));
}

static void
take_result(SiftGpuConn *c, const SiftGpuSlot *s, const char *stage, SiftVkResult *res)
{
	MemoryContext old;

	MemoryContextReset(c->rcx);
	old = MemoryContextSwitchTo(c->rcx);
	*res = s->res;
	res->acc = NULL;
	res->cand = NULL;
	if (s->status == SIFT_VK_OK && s->acc_bytes > 0)
		res->acc = memcpy(palloc(s->acc_bytes), stage, s->acc_bytes);
	if (s->status == SIFT_VK_OK && s->cand_bytes > 0)
		res->cand = memcpy(palloc(s->cand_bytes), stage + ALIGN8(s->acc_bytes), s->cand_bytes);
	MemoryContextSwitchTo(old);
}

static SiftVkStatus
remote_run(SiftGpuConn *c, const SiftVkJob *job, SiftVkResult *res)
{
	SiftGpuShm *shm = c->shm;
	int			i = slot_claim(shm);
	SiftGpuSlot *s;
	char	   *stage;
	const char *why;
	volatile bool answered = false;
	SiftVkStatus st;

	memset(res, 0, sizeof(*res));
	if (i < 0)
	{
		snprintf(c->error, sizeof(c->error), "every slot of the GPU worker is busy");
		return SIFT_VK_FAIL;
	}
	s = &shm->slot[i];
	stage = sift_gpu_stage(shm, i);
	fill_request(s, SIFT_GPU_REQ_RUN);
	if (!wire_pack(stage, &s->job, job, &why))
	{
		snprintf(c->error, sizeof(c->error), "%s", why);
		pg_atomic_write_u32(&s->state, SIFT_GPU_S_FREE);
		return SIFT_VK_FAIL;
	}
	PG_TRY();
	{
		answered = submit(c, i);
	}
	PG_CATCH();
	{
		slot_abandon(shm, s);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (!answered)
		return SIFT_VK_LOST;
	st = (SiftVkStatus) s->status;
	if (st != SIFT_VK_OK)
		snprintf(c->error, sizeof(c->error), "%s", s->error);
	take_result(c, s, stage, res);
	pg_atomic_write_u32(&s->state, SIFT_GPU_S_FREE);
	return st;
}

SiftVkStatus
sift_gpu_conn_run(SiftGpuConn *c, const SiftVkJob *job, SiftVkResult *res)
{
	SiftVkStatus st;

	if (c->shm != NULL)
		return remote_run(c, job, res);
	st = sift_vk_run(c->vk, job, query_cancelled, res);
	if (st == SIFT_VK_CANCELLED)
		CHECK_FOR_INTERRUPTS();
	return st;
}

/* Which of the m keys the worker holds, asked through a slot; none when no slot is free */
static void
resident_remote(SiftGpuConn *c, uint64 tables, const SiftVkKey *keys, uint32 m, uint8 *out)
{
	SiftGpuShm *shm = c->shm;
	int			i = slot_claim(shm);
	SiftGpuSlot *s;
	char	   *stage;

	memset(out, 0, m);
	if (i < 0)
		return;
	s = &shm->slot[i];
	stage = sift_gpu_stage(shm, i);
	fill_request(s, SIFT_GPU_REQ_RESIDENT);
	s->nkeys = m;
	s->tables = tables;
	memcpy(stage, keys, (Size) m * sizeof(SiftVkKey));
	PG_TRY();
	{
		if (submit(c, i))
		{
			if (s->status == SIFT_VK_OK)
				memcpy(out, stage + ALIGN8((Size) m * sizeof(SiftVkKey)), m);
			pg_atomic_write_u32(&s->state, SIFT_GPU_S_FREE);
		}
	}
	PG_CATCH();
	{
		slot_abandon(shm, s);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

void
sift_gpu_conn_resident(SiftGpuConn *c, uint64 tables, const SiftVkKey *keys, uint32 n, uint8 *resident)
{
	uint32		per = (uint32) (SIFT_GPU_STAGE_BYTES / (sizeof(SiftVkKey) + 8));

	if (c->shm == NULL && !sift_vk_fit_cache(c->vk, tables))
		return;
	for (uint32 from = 0; from < n; from += per)
		if (c->shm != NULL)
			resident_remote(c, tables, keys + from, Min(per, n - from), resident + from);
		else
			sift_vk_resident_batch(c->vk, keys + from, Min(per, n - from), resident + from);
}

#endif							/* SIFT_VULKAN */
