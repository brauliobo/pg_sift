/*
 * sift_aggvk.h - the Vulkan compute device of the aggregate scan (docs/GPU.md): one device per backend, a cache of column chunks that stay on
 * it, and one kernel (sift_aggvk.comp) that counts and sums integer columns of those chunks, filtered and grouped by a low cardinality key.
 *
 * Plain C, no PostgreSQL headers. Compiled only with `make VULKAN=1` (SIFT_VULKAN); the default build does not have sift_aggvk.c at all (the backend,
 * sift_agggpu.c, uses this header only under SIFT_VULKAN). libvulkan.so.1 is opened with dlopen at the first sift_vk_open, never earlier,
 * and the Vulkan declarations are the vendored subset of sift_aggvk_min.h: a build needs neither the SDK nor its headers.
 *
 * Memory model. A chunk is the 32-bit words of one column chunk as the codec stores them. The cache is one device buffer cut in slabs; a chunk lives inside one slab and is addressed by its word offset. A slab is
 * evicted whole, least recently used first, never while the batch in preparation uses it. A batch is: chunks staged for upload, control
 * tables (sift_vk_run), one submission, results read back.
 *
 * Kernel contract. For every unit of a job and every slot (SIFT_VK_MAX_PRED predicates, the group key, SIFT_VK_MAX_SUM sums) the caller gives
 * four words {word offset, mode, flo, span}: mode is the field width in bits (0: the field is 0), SIFT_VK_BYTES for a plain array of
 * width/8 byte fields, SIFT_VK_NEG for the complement. A row passes a predicate when (field - flo) <= span (unsigned, NEG: when not), is
 * counted in group = key field (0 when ungrouped), and adds its sum fields in two halves of 16 bits. The result of a workgroup (a part of a
 * unit of at most SIFT_VK_WG_TILES tiles) is, per group, the count and the 16-bit-half sums of every sum slot, as 32-bit words that cannot
 * overflow: the host adds the parts of a unit in 64 bits, so the totals are exact integers whatever the order of the additions.
 */
#ifndef SIFT_AGGVK_H
#define SIFT_AGGVK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIFT_VK_MAX_PRED	4
#define SIFT_VK_MAX_SUM		4
#define SIFT_VK_MAX_SLOTS	(SIFT_VK_MAX_PRED + 1 + SIFT_VK_MAX_SUM)
#define SIFT_VK_MAX_GROUPS	64
#define SIFT_VK_MAX_RS		(1 + 2 * SIFT_VK_MAX_SUM)		/* result words of a group: the count, and two halves per sum */
#define SIFT_VK_TILE		1024							/* values of a tile: 32 rows of 32 lanes, the codec's vertical block */
#define SIFT_VK_WG_TILES	32								/* tiles of a workgroup: 32768 values, whose 16-bit halves sum below 2^31 */
#define SIFT_VK_BYTES		0x100
#define SIFT_VK_NEG			0x200
#define SIFT_VK_MAX_UNITS	4096							/* units of a job */
#define SIFT_VK_CHUNK_MAX	(1u << 19)						/* words of the largest cacheable chunk (a slab) */

typedef struct SiftVk SiftVk;

typedef struct SiftVkKey
{
	uint64_t	rel;			/* the index storage: database and relation file */
	uint64_t	gen;			/* the segment generation: a segment is immutable, a new generation is new storage */
	uint32_t	unit;
	uint32_t	leaf;
} SiftVkKey;

typedef enum SiftVkStatus
{
	SIFT_VK_OK = 0,
	SIFT_VK_LOST,				/* the device was lost: no further use */
	SIFT_VK_TIMEOUT,			/* the submission did not finish in time: the device is not used any more */
	SIFT_VK_FAIL				/* anything else */
} SiftVkStatus;

typedef struct SiftVkTimes
{
	double		upload_ms;		/* GPU time of the copies to the device (chunks and control tables) */
	double		kernel_ms;
	double		readback_ms;
	double		wall_ms;		/* submission to completion as the host saw it */
} SiftVkTimes;

typedef struct SiftVkJob
{
	uint32_t	npred;			/* predicate slots 0 .. npred - 1, the key slot npred, sum slots npred + 1 .. */
	uint32_t	nsum;
	uint32_t	groups;			/* 0: no key; else the group slots the key field can take (at most SIFT_VK_MAX_GROUPS) */
	uint32_t	nunits;
	uint32_t	nwg;			/* workgroups */
	const uint32_t *desc;		/* nunits * (npred + 1 + nsum) * 4 words */
	const uint32_t *wg;			/* nwg * 4 words: unit, first tile, tiles, rows of the unit */
} SiftVkJob;

/*
 * Opens the device named by `device` ("" the best one, a number its index, else a substring of its name) and a cache of cache_bytes (less if
 * the device has no room). A software device (llvmpipe) is taken only when allow_software. NULL and a message in err on any failure.
 */
extern SiftVk *sift_vk_open(const char *device, bool allow_software, uint64_t cache_bytes, char *err, size_t errlen);
extern void sift_vk_close(SiftVk *vk);
extern const char *sift_vk_device_name(const SiftVk *vk);
extern bool sift_vk_is_software(const SiftVk *vk);
extern uint64_t sift_vk_cache_bytes(const SiftVk *vk);
/* A new cache size; empties the cache. False (the old cache stays) when the device has no room */
extern bool sift_vk_set_cache(SiftVk *vk, uint64_t bytes);
/* The most workgroups, and result words per workgroup, one job can have (the callers cut their batches there) */
extern uint32_t sift_vk_max_wg(const SiftVk *vk, uint32_t groups, uint32_t nsum);

/*
 * Resident chunks. lookup: true and the word offset when the chunk is on the device (it is then kept for the batch in preparation).
 * stage: copies nwords words to the staging buffer for upload with the next sift_vk_run and gives the offset the chunk will have; false when
 * the staging buffer or the cache has no room left for this batch (run it, then stage again). nwords <= SIFT_VK_CHUNK_MAX.
 */
extern bool sift_vk_lookup(SiftVk *vk, const SiftVkKey *k, uint32_t *woff);
extern bool sift_vk_stage(SiftVk *vk, const SiftVkKey *k, const void *words, uint32_t nwords, uint32_t *woff);
/* Drops what the batch in preparation staged (the caller gives the batch up) */
extern void sift_vk_abandon(SiftVk *vk);

/*
 * Runs the job: uploads the staged chunks, dispatches, reads the results back. `interrupt` is called every few milliseconds while it waits (it
 * may leave by a longjmp: the next call on the device settles the submission first). The results are valid until the next call on the device:
 * nwg * groups' * SIFT_VK_MAX_RS words at most (1 + 2 * nsum), groups' = max(groups, 1) (ungrouped: one group), group-major per workgroup.
 */
extern SiftVkStatus sift_vk_run(SiftVk *vk, const SiftVkJob *job, void (*interrupt) (void), const uint32_t **results, SiftVkTimes *times);

extern const char *sift_vk_error(const SiftVk *vk);

#endif							/* SIFT_AGGVK_H */
