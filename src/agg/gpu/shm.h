/*
 * agg/gpu/shm.h - the persistent GPU worker (agg/gpu/worker.c) and the segment of dynamic shared memory every backend talks to it through (Spec 39).
 *
 * One background worker owns the one device and its cache of decoded chunks for the whole server; it is registered by the first backend that needs it (never when sift.gpu = off: no
 * segment, no worker, no library of the driver is loaded) and ends after SIFT_GPU_IDLE_US without a job. A backend claims one of SIFT_GPU_SLOTS slots, writes its job (the chunks the device
 * lacks, the granule table, the program) into the staging area of the slot, wakes the worker by its latch and waits, in slices that check for interrupts, until the slot says done; the
 * answer comes back in the same staging area. A backend whose worker is gone, or cannot be had, runs the scan on the CPU, or (sift.gpu = on) on a device of its own.
 */
#ifndef SIFT_AGG_GPU_SHM_H
#define SIFT_AGG_GPU_SHM_H

#include "postgres.h"

#include "port/atomics.h"
#include "storage/procnumber.h"

#include "agg/vk/vk.h"

#define SIFT_GPU_SHM_NAME			"sift_gpu_shm_v6"
#define SIFT_GPU_SHM_VERSION		3
#define SIFT_GPU_SLOTS				4
/* MEM[MEM-VK-02]: SiftGpuShm staging DSM (SIFT_GPU_SLOTS * 4MiB = 16MiB of pinned shared memory, pages of idle slots dropped from the worker; was 4 * 16MiB) */
#define SIFT_GPU_STAGE_BYTES		((Size) 4 << 20)
/* MEM[MEM-VK-01]: worker idle tiers (the device gives back its buffers and cache after SIFT_GPU_RELEASE_US without a job, the worker ends after SIFT_GPU_IDLE_US; was 300s with all of it held) */
#define SIFT_GPU_RELEASE_US			(15 * USECS_PER_SEC)
#define SIFT_GPU_IDLE_US			(60 * USECS_PER_SEC)
#define SIFT_GPU_STALE_US			(10 * USECS_PER_SEC)
#define SIFT_GPU_NOT_THERE			UINT64_MAX

#define SIFT_GPU_NEED_BYTES			SIFT_VK_NEED_BYTES

typedef enum SiftGpuWorkerState
{
	SIFT_GPU_W_STOPPED = 0,
	SIFT_GPU_W_STARTING,
	SIFT_GPU_W_READY,
	SIFT_GPU_W_FAILED,			/* the device could not be opened: last_error says why, a backend tries again after a while */
	SIFT_GPU_W_STOPPING			/* the worker ends and finishes its jobs: no other is started until it has */
} SiftGpuWorkerState;

typedef enum SiftGpuSlotState
{
	SIFT_GPU_S_FREE = 0,
	SIFT_GPU_S_CLAIMED,			/* a backend fills it */
	SIFT_GPU_S_SUBMITTED,
	SIFT_GPU_S_RUNNING,
	SIFT_GPU_S_DONE
} SiftGpuSlotState;

typedef enum SiftGpuReqKind
{
	SIFT_GPU_REQ_RUN = 0,
	SIFT_GPU_REQ_RESIDENT		/* which of the keys in the staging area are on the device: a byte each, after the keys */
} SiftGpuReqKind;

/* The stored chunk of a (granule, column) in the staging area: data_off is SIFT_GPU_NOT_THERE for a chunk the device holds */
typedef struct SiftGpuWireChunk
{
	SiftVkKey	key;
	uint32		len;
	uint32		enc;
	uint32		flags;
	uint32		nrows;
	uint32		nnonnull;
	uint32		vlen;
	uint32		rawlen;
	uint32		w;
	int64		base;
	uint64		data_off;
} SiftGpuWireChunk;

typedef struct SiftGpuWireGran
{
	uint32		id;
	uint32		nrows;
	uint64		dead_off;		/* 1024 bytes, or SIFT_GPU_NOT_THERE */
	uint32		col[SIFT_VK_MAXCOLS];	/* the index of its chunk in the table of chunks, or UINT32_MAX */
} SiftGpuWireGran;

/* The SiftVkJob without its pointers; the arrays are at these offsets of the staging area */
typedef struct SiftGpuWireJob
{
	SiftVkMode	mode;
	uint32		ncols;
	uint32		ngran;
	uint32		nchunks;
	uint32		nops;
	uint32		npool;
	uint32		where_slot;
	uint32		nkeys;
	uint8		key_reg[SIFT_VK_MAXKEYS];
	uint32		naggs;
	SiftVkAgg	agg[SIFT_VK_MAXAGGS];
	int64		dense_kmin[2];
	uint32		dense_span[2];
	uint64		est_groups;
	uint64		tables;
	SiftVkFinish finish;
	SiftVkTopk	topk;
	uint32		topn_k;
	bool		topn_desc;
	bool		topn_nulls;
	bool		sort_float;
	uint32		sort_reg;
	uint32		max_cand;
	SiftVkTabKey tab;
	uint64		chunk_off;
	uint64		gran_off;
	uint64		ops_off;
	uint64		pool_off;
} SiftGpuWireJob;

typedef struct SiftGpuSlot
{
	pg_atomic_uint64 word;		/* generation << 32 | SiftGpuSlotState (agg/gpu/slot.h) */
	pg_atomic_uint32 cancelled;	/* the generation whose backend left: the worker drops the job and frees the slot */
	uint32		owner_gen;		/* the generation owner_pid is of, written after it */
	int32		owner_pid;
	ProcNumber	owner_procno;
	uint32		kind;			/* SiftGpuReqKind */
	uint32		cache_mb;		/* what sift.gpu_cache_mb and sift.gpu_device were in the backend: the worker follows them when no job runs */
	bool		software;		/* sift.gpu = on: a software device may serve the job */
	char		device[64];
	uint32		nkeys;			/* REQ_RESIDENT */
	uint64		tables;			/* REQ_RESIDENT: SiftVkJob.tables */
	SiftGpuWireJob job;
	uint32		status;			/* SiftVkStatus or SIFT_GPU_NEED_BYTES */
	SiftVkResult res;			/* its pointers mean nothing here: acc and cand are at the start of the staging area */
	uint64		acc_bytes;
	uint64		cand_bytes;
	char		error[160];
} SiftGpuSlot;

typedef struct SiftGpuShm
{
	uint32		magic;
	uint32		version;
	pg_atomic_uint32 worker_state;	/* SiftGpuWorkerState */
	ProcNumber	worker_procno;
	pg_atomic_uint32 worker_pid;
	pg_atomic_uint64 heartbeat_us;
	pg_atomic_uint64 failed_at_us;
	pg_atomic_uint64 host_bytes;	/* the host memory the device layer holds (sift_vk_set_account) */
	SiftVkCaps	caps;
	char		last_error[160];
	SiftGpuSlot slot[SIFT_GPU_SLOTS];
} SiftGpuShm;

#define SIFT_GPU_SHM_BYTES	(TYPEALIGN(64, sizeof(SiftGpuShm)) + (Size) SIFT_GPU_SLOTS * SIFT_GPU_STAGE_BYTES)

static inline char *
sift_gpu_stage(SiftGpuShm *shm, int slot)
{
	return (char *) shm + TYPEALIGN(64, sizeof(SiftGpuShm)) + (Size) slot * SIFT_GPU_STAGE_BYTES;
}

/* The entry of the worker, as the postmaster finds it by name in the library */
extern PGDLLEXPORT void sift_gpu_worker_main(Datum main_arg);

/* ---- the side of a backend ---- */

typedef struct SiftGpuConn SiftGpuConn;

/*
 * The device for this backend: the shared worker (started when none is up), or with allow_local a device of the backend's own when the worker cannot be had. NULL, and why, when there is none
 * (a build without Vulkan, no usable device, no room for a worker and no leave to open one).
 */
extern SiftGpuConn *sift_gpu_conn_open(bool allow_local, const char **why);
/* The device of the backend's own (gpu_local.c): opened for the session, NULL and why when there is none */
extern SiftVk *sift_gpu_local_open(const char **why);
extern void sift_gpu_conn_close(SiftGpuConn *c);
extern const SiftVkCaps *sift_gpu_conn_caps(const SiftGpuConn *c);
extern bool sift_gpu_conn_shared(const SiftGpuConn *c);
/* The most bytes of chunks one job carries, and of result it can bring back (a job whose groups would not fit goes to the CPU) */
extern uint64 sift_gpu_conn_batch_bytes(const SiftGpuConn *c);
extern uint64 sift_gpu_conn_result_bytes(const SiftGpuConn *c);
/* resident[i] = 1 when the chunk of keys[i] is on the device now, once its cache is sized for jobs whose tables take `tables` bytes (SiftVkJob.tables) */
extern void sift_gpu_conn_resident(SiftGpuConn *c, uint64 tables, const SiftVkKey *keys, uint32 n, uint8 *resident);
/*
 * Runs the job: the status of sift_vk_run, SIFT_VK_LOST for a worker that is gone, SIFT_GPU_NEED_BYTES when a chunk the job left out (bytes NULL) is not on the device. The result is valid until
 * the next call.
 */
extern SiftVkStatus sift_gpu_conn_run(SiftGpuConn *c, const SiftVkJob *job, SiftVkResult *res);
extern const char *sift_gpu_conn_error(const SiftGpuConn *c);

#endif							/* SIFT_AGG_GPU_SHM_H */
