/*
 * agg/gpu_v6.h - the Vulkan path of SiftAggV6 (Spec 39): the four settings sift.gpu, sift.gpu_min_rows, sift.gpu_device and sift.gpu_cache_mb, the choice between the device and the CPU, and
 * the scan of the parts of a Format 2.0 relation (a sift_col index or a sift_table, through the gpu ops of its store) on the device.
 *
 * The aggregate asks for the device once, before it scans: sift_gpu_v6_agg gives back the groups that the device made of the parts it could read, or NULL when the CPU runs everything. The CPU then
 * scans what the device did not take (the slab, the parts with pending deletes or unsettled writers, any part sift_gpu_v6_covers refuses) into the same groups. Whatever goes wrong on the device
 * (lost, timeout, a chunk it cannot decode, a table too small) discards what it folded and the CPU runs the whole scan; an error of the program (division by zero, a value out of range) is raised as
 * PostgreSQL raises it.
 */
#ifndef SIFT_AGG_GPU_V6_H
#define SIFT_AGG_GPU_V6_H

#include "postgres.h"

#include "commands/explain.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "agg/gpu_cfg.h"
#include "agg/v6/v6.h"
#include "agg/vexpr.h"
#include "agg/vk.h"
#include "core/store_ops.h"

typedef enum SiftGpuMode
{
	SIFT_GPU_OFF,
	SIFT_GPU_AUTO,
	SIFT_GPU_ON
} SiftGpuMode;


/* The state of an aggregate a device result is folded into the groups as (the layout of V6Acc in agg/v6/group.c, without its pad) */
typedef struct SiftV6AccIn
{
	int64		n;				/* the values folded: a count, 0 for a state that has none */
	uint64		v0;				/* the low half of an integer sum, a double sum (its bits), a minimum or maximum (a double: its bits) */
	int64		v1;				/* the high half of an integer sum */
} SiftV6AccIn;

/* The value of a text key of a group of the device: the bytes, which no int64 holds */
typedef struct SiftV6StrIn
{
	const char *p;
	uint32		len;
} SiftV6StrIn;

/*
 * Folds one group of the device into the set of all the keys of g (a plain GROUP BY): key holds the spec's nkeys values as int64 (the value of the key column, as the group holds it) and
 * bit i of key_nulls says that key i is NULL; str[i] is the value of key i when that is a text (p NULL for the others; str NULL when no key is); acc has one state for each aggregate of the spec. With no key it is the one
 * group of an ungrouped aggregate. Implemented in agg/v6/group.c, which merges the states as it merges the groups of its spilled runs.
 */
extern void sift_v6_group_fold_rec(SiftV6Group *g, const int64 *key, const SiftV6StrIn *str, uint64 key_nulls, const SiftV6AccIn *acc);

/* column [NOT] IN (set of integers without NULL) */
typedef struct SiftGpuIn
{
	int			col;			/* the column slot of the programs below */
	bool		negate;
	uint32		n;
	const int64 *vals;
} SiftGpuIn;

/* column op constant on a whole number column, op one of SCMP_EQ..SCMP_GE: the zones of a granule can rule out every row of it, as they do for the scan of the CPU */
#define SIFT_GPU_ZONE_TESTS 8

typedef struct SiftGpuZoneCmp
{
	int			col;			/* the column slot */
	int			op;
	int64		val;
} SiftGpuZoneCmp;

/*
 * What the aggregate (or the top-N) asks of the device. Column slot i of the programs (aux of a SVX_COL) is the leaf leaf[i] of the store, and boolean input j of a program the column slot
 * mask_col[j]; a column the spec names that no program reads is made a slot of its own. A NULL program is the plain column of the spec (a key, the argument of an aggregate) or none (count(*)).
 * where, in and the filters are tests of three-valued logic: a row passes where they are true.
 */
typedef struct SiftGpuQuery
{
	Relation	rel;			/* the relation of the store: the index of a sift_col, the table of a sift_table */
	Relation	heap;			/* sift_col: its table, whose visibility map says which rows need no test; NULL for a sift_table */
	const SiftGpuOps *ops;		/* the format of the relation */
	SiftStoreSchema sch;		/* its leaf map and the kind of each leaf */
	double		live_rows;		/* the rows its parts hold that are not deleted */
	Snapshot	snap;
	const SiftV6Spec *spec;		/* NULL for a top-N */
	int			ncols;
	int			leaf[SIFT_VK_MAXCOLS];
	int8		mask_col[SIFT_VEXPR_MAX_MASKS];
	const SiftVexprProg *where;
	int			nin;
	const SiftGpuIn *in;
	int			nzone;
	SiftGpuZoneCmp zone[SIFT_GPU_ZONE_TESTS];
	const SiftVexprProg *key[SIFT_VK_MAXKEYS];
	const SiftVexprProg *arg[SIFT_VK_MAXAGGS];
	const SiftVexprProg *filter[SIFT_VK_MAXAGGS];
	const SiftVexprProg *sort;	/* top-N: the expression ordered by */
	SiftVkTopk	topk;			/* the groups: the order of the node that keeps the K best of them (k 0: all groups are wanted) */
	double		est_groups;		/* 0: not known */
	int			parallel_workers;	/* the workers the CPU plan would run besides the leader */
	const struct SiftGpuDictQual *dict;	/* the comparisons of a text column the scan makes: its programs hold a code for each (agg/gpu_dict_agg.h), NULL for none */
} SiftGpuQuery;

/* The slot of the column of attribute attno among those the device reads, -1 when it reads none of it */
static inline int
sift_gpu_query_slot(const SiftGpuQuery *q, int attno)
{
	const int	leaf = q->sch.att_first[attno];

	for (int c = 0; c < q->ncols; c++)
		if (q->leaf[c] == leaf)
			return c;
	return -1;
}

/* ---- the parts on the device (col/f2/f2_gpu_plan.c) ---- */

struct SiftGpuPlanPriv;

typedef struct SiftGpuPlan
{
	uint32		nparts;			/* the parts of the manifest */
	uint32		ndevice;		/* ... the device reads */
	uint32		ncols;
	uint32		ngran;			/* of the parts the device reads */
	SiftVkGranule *gran;		/* the granule i of the plan has id i */
	SiftVkChunk *chunk;			/* ngran * ncols: the key of each, the rest once loaded */
	uint32	   *est_chunk;		/* per granule: the bytes its chunks take decoded on the device */
	uint64		enc_bytes;		/* the bytes of the chunks as stored, estimated, that the device must be sent when it holds none */
	uint64		rows;			/* the rows the snapshot sees in them */
	uint64		scan_rows;		/* ... in the granules the zones leave to the predicates: what the scan of the CPU reads of them */
	uint64		dead_rows;		/* the rows of their granules the snapshot sees deleted, which the device leaves out (the deletion vectors and the pending deletes) */
	int64		zmin[SIFT_VK_MAXCOLS];	/* of each column, over the granules: the least and the greatest value, and whether every granule says so and whether some holds a NULL */
	int64		zmax[SIFT_VK_MAXCOLS];
	bool		zrange[SIFT_VK_MAXCOLS];
	bool		znulls[SIFT_VK_MAXCOLS];
	bool		cpu_rest;		/* rows are left to the CPU besides the parts the device reads: a part it refuses, the slab of a partition: a result of whole groups cannot be had from the device alone */
	char		why[96];		/* why the last part the device does not read is left to the CPU */
	const SiftGpuOps *ops;		/* the format that made the plan */
	struct SiftGpuPlanPriv *priv;
} SiftGpuPlan;

/* The parts the device reads, their chunks and where a granule is in its part: SiftGpuOps of the format (core/store_ops.h), reached through SiftGpuPlan.ops */

/* ---- one scan ---- */

typedef struct SiftGpuRun
{
	bool		used;
	char		why[160];		/* not used: why */
	double		cpu_ms;			/* what the router estimated */
	double		gpu_ms;
	SiftVkMode	kernel;
	bool		shared;			/* the worker of the server, else a device of the backend's own */
	char		device[128];
	double		pcie_gbps;
	bool		rebar;
	uint32		parts;			/* read by the device, of */
	uint32		parts_all;		/* the parts of the manifest */
	uint64		dead_rows;		/* the rows of the parts read that the snapshot sees deleted */
	uint32		granules;
	uint32		resident;
	uint32		uploaded;
	uint32		batches;
	uint64		bytes_up;
	uint64		bytes_lz4;
	SiftVkTimes times;
	SiftGpuPlan *plan;
} SiftGpuRun;

/*
 * The groups of the parts the device reads, in a context of gcx (desc is the tuple the groups leave as); NULL when the device is not used, with *run saying why. *run is always made, in gcx.
 * sift.gpu = off: NULL and nothing loaded, started or opened.
 */
extern SiftV6Group *sift_gpu_v6_agg(const SiftGpuQuery *q, MemoryContext gcx, TupleDesc desc, SiftGpuRun **run);

/* One row a top-N asks for: the key of the sort expression, and where the row is */
typedef struct SiftGpuCand
{
	int64		key;			/* the integer that orders the rows (a double: its order-preserving key) */
	bool		isnull;
	uint32		part_id;
	uint64		part_gen;
	uint32		granule;		/* in the part */
	uint32		row;			/* in the granule */
} SiftGpuCand;

/*
 * The k best rows of the parts the device reads, for ORDER BY q->sort LIMIT k that no zone can prune (prunable: the order of the index can, and the CPU tournament wins). False when the device is
 * not used. The caller reads the rest of the columns of the rows it outputs.
 */
extern bool sift_gpu_v6_topn(const SiftGpuQuery *q, uint32 k, bool desc, bool nulls_first, bool prunable, MemoryContext cx, SiftGpuRun **run, SiftGpuCand **cand, uint32 *ncand);

/* The CPU scan leaves this part out: the device took it */
extern bool sift_gpu_v6_covers(const SiftGpuRun *run, uint64 part_gen);
/* The lines of EXPLAIN; nothing with sift.gpu = off */
extern void sift_gpu_v6_explain(const SiftGpuRun *run, ExplainState *es);
/* Lets go of the parts pinned for the scan */
extern void sift_gpu_v6_end(SiftGpuRun *run);

#endif							/* SIFT_AGG_GPU_V6_H */
