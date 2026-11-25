/*
 * agg/gpu/dict.h - a text column on the device as the codes of the dictionaries of the parts (agg/gpu/dict.c).
 *
 * The chunks of a text column of a part that has a dictionary for it (SEC_DICTS) hold the codes of its strings: the device reads them as whole numbers, so `col = 'x'`, `col <> 'x'` and a
 * text group key are comparisons and keys of numbers. The numbers mean strings only inside one part, because each part has a dictionary of its own: the code of a constant is looked up once
 * in each, and a text key is a key of one part. A scan is cut in units that the device runs one after another, each over granules that share what the codes mean: the parts that have one
 * text key, else the runs of parts in which the constants have the same codes. The CPU maps the codes of the groups the device made back to strings, which meet the groups of the other
 * units (and of the CPU's own scan) in the one table of groups.
 */
#ifndef SIFT_AGG_GPU_DICT_AGG_H
#define SIFT_AGG_GPU_DICT_AGG_H

#include "agg/gpu/scan.h"
#include "col/gpu.h"

#define SIFT_GPU_DICT_CMPS	4

/* column = 'constant' (or <>) on a text column the device reads as codes: its node is a comparison with the number 0 that each unit sets to the code of the constant */
typedef struct SiftGpuDictCmp
{
	int			slot;			/* the column slot */
	bool		ne;				/* <> (else =) */
	const char *str;
	uint32		len;
	SiftVexprNode *code;		/* the constant of the comparison */
} SiftGpuDictCmp;

typedef struct SiftGpuDictQual
{
	int			n;
	SiftGpuDictCmp cmp[SIFT_GPU_DICT_CMPS];
	const SiftVexprNode *where;	/* the tree of the WHERE of the scan, whose constants cmp names */
} SiftGpuDictQual;

typedef struct SiftGpuUnit
{
	uint32		g0;				/* the granules [g0, g1) of the plan */
	uint32		g1;
	uint32		part;			/* the first part of it: the one of a unit of a text key */
	bool		skip;			/* no row of it passes (a constant is in no dictionary of its parts): nothing to run */
	SiftVexprProg where;		/* the WHERE with the codes of the unit */
} SiftGpuUnit;

typedef struct SiftGpuUnits
{
	uint32		n;
	SiftGpuUnit *unit;
	bool		text_key;		/* a key of the groups is a text: the groups of two units are of different strings that the numbers of the keys do not tell apart */
} SiftGpuUnits;

/* The units of the scan of the parts of plan p that the dictionaries cut it in: always at least one when the query reads a text column, NULL when it reads none; why (when not NULL) is the reason the compile of a unit failed */
extern SiftGpuUnits *sift_gpu_units(const SiftGpuQuery *q, const SiftGpuPlan *p, MemoryContext cx, const char **why);
/* The text a code of the column slot is in unit u: false when it is NULL's (the code past the strings of the dictionary) */
extern bool sift_gpu_unit_str(const SiftGpuPlan *p, const SiftGpuUnit *u, int slot, int64 code, SiftV6StrIn *out);
/* Whether column slot `slot` is one the device reads as codes */
static inline bool
sift_gpu_slot_is_text(const SiftGpuQuery *q, int slot)
{
	return slot >= 0 && sift_gpu_dict_leaf_ok(q->sch.leaf_kinds[q->leaf[slot]], q->sch.leaf_hints[q->leaf[slot]]);
}
/* The jobs the scan runs as: a piece of it with no row that can pass is none (a scan with no pieces is one job) */
extern int	sift_gpu_units_to_run(const SiftGpuUnits *us);
/* The text of the keys of a group of the unit u that are texts, the strings their codes are in the dictionary of its part (NULL when no key is a text); key_slot is the column slot of each key */
extern const SiftV6StrIn *sift_gpu_key_strings(const SiftGpuQuery *q, const SiftGpuPlan *p, const SiftGpuUnits *us, const SiftGpuUnit *u, uint32 nkeys, const int *key_slot, const int64 *key,
											   uint64 nulls, SiftV6StrIn *str);

#endif							/* SIFT_AGG_GPU_DICT_AGG_H */
