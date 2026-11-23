/*
 * agg/vk.h - the Vulkan compute device of the aggregate scan (docs/GPU.md, Spec 39): one device, a cache of decoded column chunks that
 * stays on it, the Format 2.0 chunk decoder (LZ4, constant, frame of reference, delta, run lengths, dictionary, decimal and split-bits
 * floats, scale, the nulls of a chunk), and three aggregation kernels (no key, dense keys, hashed keys) and a top-N kernel that read them.
 *
 * Plain C, no PostgreSQL headers. Compiled only with `make VULKAN=1` (SIFT_VULKAN). libvulkan.so.1 is opened with dlopen at the first
 * sift_vk_open or sift_vk_list_devices, never earlier, and the Vulkan declarations are the vendored subset of agg/vk_min.h.
 *
 * What the host gives. A job is a list of granules; each has the chunks (as stored, possibly LZ4 wrapped) of the columns the program
 * reads, and optionally the bitmap of its deleted rows. The program (agg/vk/layout.h: operations of SiftVecOpCode) computes from the
 * columns the WHERE slot, the key registers and the value registers of the aggregates. The device decodes the chunks it does not hold into
 * its cache (value = base + field of w bytes, a bit per row of validity), runs the kernel over all granules and returns ONE merged table.
 *
 * What it gives back, as int64 words (acc): per group, the rows (every row that passed WHERE), then three words per aggregate:
 *   COUNT(*), COUNT(col): w0 = count
 *   SUM of integers:      w0 = sum of the low 32 bits of the values (unsigned), w1 = sum of the high 32 bits (signed), w2 = values
 *                         summed; the exact sum is w1 * 2^32 + w0 in 128 bits, whatever the order of the additions
 *   SUM of doubles:       w0 = the bits of the double sum, w2 = values summed
 *   MIN, MAX:             w0 = the least or the greatest (a double: its order-preserving key), w2 = values compared (0: NULL)
 */
#ifndef SIFT_AGGVK_H
#define SIFT_AGGVK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIFT_VK_MAXCOLS			16
#define SIFT_VK_MAXAGGS			16
#define SIFT_VK_MAXKEYS			8
#define SIFT_VK_GRANULE_ROWS	8192
#define SIFT_VK_SLAB_BYTES		((uint64_t) 4 << 20)

typedef struct SiftVk SiftVk;

typedef enum SiftVkStatus
{
	SIFT_VK_OK = 0,
	SIFT_VK_LOST,				/* the device was lost: no further use */
	SIFT_VK_TIMEOUT,			/* the submission did not finish in time: the device is not used any more */
	SIFT_VK_FAIL,				/* anything else (no memory, the device is unusable) */
	SIFT_VK_DECODE,				/* a chunk the device cannot decode: run the scan on the CPU */
	SIFT_VK_OVERFLOW,			/* the hash table or the candidate list was too small: run the scan on the CPU */
	SIFT_VK_PROG_ERROR			/* a live row raised an error of the program: res.err_* name the row, the CPU raises the error */
} SiftVkStatus;

typedef enum SiftVkMode
{
	SIFT_VK_UNGROUPED,
	SIFT_VK_DENSE,
	SIFT_VK_HASH,
	SIFT_VK_TOPN
} SiftVkMode;

/* The identity of a decoded chunk: a part is immutable, a rewrite is a new part_gen, TRUNCATE a new relation file */
typedef struct SiftVkKey
{
	uint32_t	spc;
	uint32_t	db;
	uint64_t	rel;
	uint32_t	part_id;
	uint32_t	col_id;
	uint64_t	part_gen;
	uint32_t	granule_id;
	uint32_t	flags;
} SiftVkKey;

/*
 * What names the descriptor table of a job: where the decoded chunks of its granules are on the device. The device keeps it between jobs
 * with the same key, so a warm job does not look up every chunk again. The caller guarantees that equal keys mean the same chunks in the same
 * granule and column slots; any change of a part, of the admitted parts, of the column slots or of the granule range is a different key.
 * rel == 0: no table is kept.
 */
typedef struct SiftVkTabKey
{
	uint32_t	spc;
	uint32_t	db;
	uint64_t	rel;			/* the relation file */
	uint64_t	manifest_gen;	/* the manifest generation the parts were read at */
	uint64_t	col_mask[4];	/* the leaves of the columns of the slots, one bit each (leaf modulo 256) */
	uint64_t	granules;		/* a signature of the granule range, its parts and the physical columns in slot order */
} SiftVkTabKey;

typedef struct SiftVkChunk
{
	SiftVkKey	key;
	const void *bytes;			/* the record as stored: [wrap header] [validity] [body], 8-aligned, len bytes */
	uint32_t	len;
	uint32_t	enc;			/* SIFT_VK_ENC_* */
	uint32_t	flags;			/* SIFT_VK_CF_* */
	uint32_t	nrows;
	uint32_t	nnonnull;
	uint32_t	vlen;			/* bytes of the validity in front of the body (after an unwrap) */
	uint32_t	rawlen;			/* LZ4: the bytes after the unwrap (validity and body) */
	uint32_t	w;				/* the field of a decoded value: 0 (the column is the constant base), 1, 2, 4 or 8 bytes */
	int64_t		base;
} SiftVkChunk;

typedef struct SiftVkGranule
{
	uint32_t	id;
	uint32_t	nrows;
	const uint32_t *dead;		/* NULL, or 256 words: a bit for each row that is deleted for the snapshot */
	const SiftVkChunk *col[SIFT_VK_MAXCOLS];	/* by column slot of the program; NULL when the program reads none of it */
} SiftVkGranule;

typedef struct SiftVkAgg
{
	uint8_t		kind;			/* SIFT_VK_AK_* */
	uint8_t		vreg;			/* the value register (255: none, COUNT(*)) */
	uint8_t		freg;			/* the mask slot of the FILTER (255: none) */
} SiftVkAgg;

typedef struct SiftVkJob
{
	SiftVkMode	mode;
	uint32_t	ncols;			/* column slots of the granules */
	uint32_t	ngran;
	const SiftVkGranule *gran;
	uint32_t	nops;
	const uint32_t *ops;		/* nops * SIFT_VK_OP_WORDS words */
	uint32_t	npool;
	const uint32_t *pool;		/* the sets of the IN operations */
	uint32_t	where_slot;		/* mask slot that must be true (255: every live row) */
	uint32_t	nkeys;
	uint8_t		key_reg[SIFT_VK_MAXKEYS];
	uint32_t	naggs;
	SiftVkAgg	agg[SIFT_VK_MAXAGGS];
	int64_t		dense_kmin[2];	/* DENSE: the least value of each key */
	uint32_t	dense_span[2];	/* ... and the values it can take plus one (the slot of NULL) */
	uint64_t	est_groups;		/* HASH: the groups to expect (0: the rows) */
	uint32_t	topn_k;			/* TOPN: the rows wanted */
	bool		topn_desc;
	bool		topn_nulls;		/* NULL sorts first: the rows without a key are candidates */
	bool		sort_float;		/* the sort register holds doubles */
	uint32_t	sort_reg;
	uint32_t	max_cand;		/* TOPN: the capacity of the list of candidates (0: a default) */
	SiftVkTabKey tab;			/* the descriptor table of the granules kept on the device (rel 0: none) */
} SiftVkJob;

typedef struct SiftVkTimes
{
	double		stage_ms;		/* host: the batches built (the chunks looked up or copied into the staging buffers, the control words) */
	double		decode_ms;		/* device: LZ4 and decode of the chunks missing from the cache */
	double		kernel_ms;		/* device: the aggregation */
	double		readback_ms;
	double		wall_ms;		/* the run as the host saw it, the compile included */
	double		compile_ms;		/* host: the pipeline of a fast kernel made for a shape not seen before (the first job of a shape pays it once) */
} SiftVkTimes;

typedef struct SiftVkResult
{
	const int64_t *acc;			/* UNGROUPED: rows, 3 words per aggregate. DENSE: per group the same. HASH: ngroups records */
	uint32_t	acc_words;
	uint32_t	ngroups;
	uint32_t	rec_words;		/* HASH: a record is the nkeys key words, the NULL mask of the key, the rows, 3 words per aggregate */
	const int64_t *cand;		/* TOPN: ncand pairs (sort key, granule << 32 | row | 1 << 62 when the key is NULL) */
	uint32_t	ncand;
	bool		fast;			/* a fast kernel ran the job (agg/vk/fast.c) */
	bool		tab_hit;		/* the descriptor table of the job came from the device: no chunk was looked up */
	bool		has_err;
	uint32_t	err_gran, err_row, err_op, err_code;
	uint32_t	chunks_resident, chunks_uploaded, batches;
	uint64_t	bytes_uploaded, bytes_lz4;
	SiftVkTimes times;
} SiftVkResult;

typedef struct SiftVkDevInfo
{
	char		name[128];
	uint32_t	vendor, device_id;
	int			type;			/* 1 integrated, 2 discrete, 3 virtual, 4 software */
	bool		pci;
	uint32_t	pci_domain, pci_bus, pci_dev, pci_fn;
	double		link_cur_gts, link_max_gts;	/* sysfs, GT/s; 0 unknown */
	uint32_t	link_cur_width, link_max_width;
	double		pcie_gbps;		/* effective host to device GB/s of the link, 0 unknown */
	uint64_t	vram_bytes;
	bool		usable;			/* has what the kernels need */
	char		why[96];
} SiftVkDevInfo;

typedef struct SiftVkCaps
{
	char		name[128];
	bool		software, rebar, uma, dedicated_transfer, ext_budget, ext_pci;
	uint32_t	subgroup_size;
	double		pcie_gbps;
	double		vram_gbps;		/* measured at open */
	uint64_t	vram_bytes, vram_budget_free, cache_bytes;
	uint32_t	pci_bus, pci_dev;
} SiftVkCaps;

/*
 * The devices there are, ranked as sift_vk_open ranks them: *chosen is the index of the one `device` ("" the best, a number its index,
 * "BB:DD" a PCI bus and device in hexadecimal, else a substring of the name) selects, or -1. Returns the number of devices, -1 and a
 * message in err when there is no Vulkan driver. A software device (llvmpipe) is chosen only with allow_software.
 */
extern int	sift_vk_list_devices(SiftVkDevInfo *out, int max, int *chosen, const char *device, bool allow_software, char *err, size_t errlen);

/*
 * Opens the device and a cache of at most cache_bytes (less when the device has no room: three quarters of what VK_EXT_memory_budget
 * says is free). NULL and a message in err on any failure, including a missing driver.
 */
extern SiftVk *sift_vk_open(const char *device, bool allow_software, uint64_t cache_bytes, char *err, size_t errlen);
extern void sift_vk_close(SiftVk *vk);
/* Gives back the buffers of a device that is idle, and with cache its cache too (the next job makes them again); false when a submission cannot be waited for */
extern bool sift_vk_release(SiftVk *vk, bool cache);
extern const SiftVkCaps *sift_vk_caps(const SiftVk *vk);
extern const char *sift_vk_error(const SiftVk *vk);
extern bool sift_vk_usable(const SiftVk *vk);
/* The cache with a new size; empties it. False (the old one stays) when the device has no room */
extern bool sift_vk_set_cache(SiftVk *vk, uint64_t bytes);
/* How much of the chunks the cache holds, for the routing of the scan: the share of the n chunks that are resident, 0..1 */
extern double sift_vk_resident_share(SiftVk *vk, const SiftVkKey *keys, uint32_t n);
/* resident[i] = whether the chunk keys[i] is on the device, in one pass that looks ahead in the table; returns the number that are */
extern uint32_t sift_vk_resident_batch(SiftVk *vk, const SiftVkKey *keys, uint32_t n, uint8_t *resident);
/* delta is called with the bytes of host memory the device layer holds when that changes (SiftMemTracker); may be NULL */
extern void sift_vk_set_account(SiftVk *vk, void (*account) (void *arg, int64_t delta), void *arg);
/* The next run ends as the device had failed: SIFT_VK_LOST or SIFT_VK_TIMEOUT (tests of the fallback) */
extern void sift_vk_test_inject(SiftVk *vk, SiftVkStatus status);
/* True when the job runs on a fast kernel (agg/vk/fast.c): a program of columns and comparisons with constants, aggregates without a FILTER; job->gran may be NULL. The router's cost differs. */
extern bool sift_vk_job_is_fast(const SiftVkJob *job);
/* With true every job runs the kernels that interpret the program, never the fast kernels (agg/vk/fast.c); tests compare the two */
extern void sift_vk_test_nofast(SiftVk *vk, bool on);
/* With true every job that hits a descriptor table also builds it again from its chunks and compares (tests of the keys); a difference fails the job */
extern void sift_vk_test_verify_tabs(SiftVk *vk, bool on);
/* The decoded column of a cached chunk, read back: vals[r] and valid[r] for every row (the arrays hold SIFT_VK_GRANULE_ROWS); false when it is not cached (tests of the decoder) */
extern bool sift_vk_test_dcol(SiftVk *vk, const SiftVkKey *k, int64_t *vals, uint8_t *valid);

/*
 * Runs the job. `interrupt` is called every few milliseconds while a submission is awaited (it may leave by a longjmp: the next call on the
 * device settles the submission first). The result is valid until the next call on the device. On any status but SIFT_VK_OK the result
 * holds nothing to use (except the error row of SIFT_VK_PROG_ERROR) and the caller runs the scan again on the CPU.
 */
extern SiftVkStatus sift_vk_run(SiftVk *vk, const SiftVkJob *job, void (*interrupt) (void), SiftVkResult *res);

#endif							/* SIFT_AGGVK_H */
