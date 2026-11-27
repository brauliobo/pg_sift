/*
 * agg/vk/job.h - what the preparation of a batch (agg/vk/job.c) and its submission (agg/vk/run.c) share
 */
#ifndef SIFT_AGG_VK_JOB_H
#define SIFT_AGG_VK_JOB_H

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

/* what a job needs of the device, worked out once: the accumulators and the table */
typedef struct Plan
{
	uint32_t	acc_words;		/* UNGROUPED, DENSE: the words of the result */
	uint32_t	groups, stride;	/* DENSE: the groups and the words of one; HASH: the u64 words of a slot */
	uint32_t	ht_mask, ht_maxload;
	uint64_t	ht_bytes, out_bytes;
	uint32_t	cand_cap;
	uint32_t	flags;			/* SIFT_VK_FL_* */
	bool		have_theta;
	int64_t		theta;
	bool		fast;			/* the kernel is a fast one: the header carries the shape */
	FastShape	shape;
	VkPipeline	kern;			/* the aggregation kernel */
	double		compile_ms;
	Tab		   *tab;			/* the descriptor table of the job's granules, when the device holds it */
	uint32_t   *tab_host;		/* else: the table as the batches build it, kept when the job ends well (NULL: not kept) */
	uint64_t	gen0;			/* vk->evict_gen when the job began */
} Plan;

/* A batch: the granules [g0, g0 + ng) of the job, and the work they make */
typedef struct Batch
{
	Slot	   *s;
	uint32_t	g0, ng;
	uint32_t	njobs;
	uint64_t	id;
	uint32_t	chunks_resident, chunks_uploaded;
	uint64_t	bytes_uploaded, bytes_lz4;
	double		stage_ms;
	VkBuffer	tab_buf;		/* a table hit: the rows of its granules are copied into the control words */
	uint64_t	tab_src, tab_dst, tab_bytes;
} Batch;

typedef enum BuildStatus
{
	BUILD_OK,
	BUILD_NO_ROOM,				/* not even one granule fits: the cache is held by the batch in flight */
	BUILD_FAIL					/* vk->err says why */
} BuildStatus;

bool plan_job(SiftVk *vk, const SiftVkJob *job, Plan *p);
void plan_free(Plan *p);
/* Fills the slot for granules from g0 on, as many as fit */
BuildStatus batch_build(SiftVk *vk, Slot *s, const SiftVkJob *job, const Plan *p, uint32_t g0, uint32_t g1, Batch *b);
bool batch_check(SiftVk *vk, Batch *b);
bool ensure_buffers(SiftVk *vk, const Plan *p);
void batch_abandon(SiftVk *vk, Batch *b);
/* False when a sum of floats of the result is not finite: an overflow of finite values (an error of PostgreSQL) and the sum of an infinity of the table (a value) look the same to the device, so the CPU decides */
bool sift_vk_sums_finite(const SiftVkJob *job, const SiftVkResult *res);

#endif

#endif
