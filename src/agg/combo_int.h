/*
 * agg/combo_int.h - combos: expressions evaluated over combinations of values (agg/combo.c)
 */
#ifndef SIFT_AGG_COMBO_INT_H
#define SIFT_AGG_COMBO_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/lookup_int.h"
#include "agg/tup_int.h"

/* the distinct values of a dictionary leaf over all the dictionaries of the scan: ids 1.. */
typedef struct Intern
{
	uint32		n;
	uint32		cap;
	uint32		nslot;
	const char **ptr;
	uint32	   *len;
	uint32	   *hv;
	uint32	   *slot;
} Intern;

typedef struct ComboEnt
{
	uint32		nout;			/* output rows of the tuple: its matches, one row of NULLs (LEFT JOIN) or none (INNER) */
	uint32		base;			/* first output row */
} ComboEnt;

/* the ids (1..) of the distinct values of a 64-bit leaf of a combo: one table of (value, id), so a probe is one cache line */
typedef struct Id64Ent
{
	int64		key;
	uint32		id;				/* 0: free */
} Id64Ent;

typedef struct Id64
{
	MemoryContext cxt;
	Id64Ent    *tab;
	uint32		nslot;
	uint32		n;
	uint32		cap;
	int64	   *val;			/* the value of an id */
	uint32		nan_id;			/* the id of the NaN of a numeric leaf (not in tab), 0 until one is seen */
} Id64;

/* a numeric value as an integer times 10^-scale (ok: it is a finite number of at most 38 digits) */
typedef struct DNum
{
	sift_i128	m;
	int16		scale;
	bool		ok;
} DNum;

/* a leaf that is a column of an earlier lookup: its values are known by id, not by the rows of that table that hold them */
typedef struct PriorVal
{
	uint32	   *memo;			/* per output row of the earlier lookup: the id of its value (0 NULL), COMBO_UNEVAL until read */
	uint32		nmemo;
	uint32	   *rep;			/* per id: an output row that holds the value, which binds it */
	uint32		nrep;
} PriorVal;

typedef struct ComboSt
{
	Intern		in[SIFT_AGG_MAX_CLEAVES];
	PriorVal	pv[SIFT_AGG_MAX_CLEAVES];
	Id64		id64[SIFT_AGG_MAX_CLEAVES];	/* 64-bit leaves (numeric, int8, timestamps, floats): the ids of their distinct values */
	Datum		unit[SIFT_AGG_MAX_CLEAVES];	/* numeric leaves: 10^-scale with that many digits, a stored value is the integer times it */
	MemoryContext cxt;			/* the cache: entries, outputs, values and ids of the leaves; reset when it passes sift.agg_combo_cache */
	bool		resettable;		/* no join, no dictionary leaf: nothing outside the cache holds its ids */
	bool		by_id;			/* one 64-bit leaf, no join: the entry of a tuple is the id of its value, there is no table of entries */
	MapTab	   *map;
	uint32	   *kmemo[SIFT_AGG_MAX_JKEYS];	/* per text key of the join: the id of the table's value of each interned value of the leaf (COMBO_UNEVAL: not looked up yet, 0: no row has it) */
	uint32		nmemo[SIFT_AGG_MAX_JKEYS];
	uint8	   *slut;			/* a semi or anti join that is a plain filter on one integer leaf: a table over [slo, slo + snlut) of its keys */
	int32		slo;
	uint32		snlut;
	bool		kfilt;			/* the scan's join key filter is the keys of the table, which hold its own conditions (join_filters) */
	bool		spass;			/* ... and every tuple that reaches the join passes it: a semi join, or an inner one with unique keys, that returns no column */
	ExprState **slot;
	List       *args;			/* correlated child parameter expressions */
	ExprState  *kx[SIFT_AGG_MAX_JKEYS];	/* the join keys that are expressions */
	int16	   *tlen;			/* per slot: the length and pass-by-value of its result, for the direct slots */
	bool	   *tbyval;
	Datum	   *dat;			/* the values of the direct slots, a slot's output is the number + 1 of one */
	DNum	   *dnum;			/* per value of dat, for the slots of numeric sums: the exact integer and scale it stands for */
	uint32		ndat;
	uint32		datcap;
	char	   *arena;			/* the by-reference values of dat are copied here, in blocks of the cache */
	Size		arena_left;
	bool	   *decode;			/* per slot: the argument of a numeric sum or average, whose values dnum holds */
	uint32		nevals;
	uint32		nent;
	uint32		entcap;
	int			ninputs;		/* stored cache tuple width, at least one */
	uint64	   *vid;			/* [ent * ninputs]: 0 NULL, else the value's id in `in` / the integer + 1 */
	bool	   *null_input;		/* the nullable input was absent for this entry */
	ComboEnt   *ent;
	uint32		nht;
	uint32	   *ht;				/* ent + 1 */
	uint32		dircap;			/* one input: entry + 1 of the id (below COMBO_DIR_MAX) by index, 0 not seen (combo_ent) */
	uint32	   *dir;
	uint32		nout;
	uint32		outcap;
	uint32	   *val;			/* [out * nslots + slot]: key id (0 NULL) or boolean */
	uint32	   *maprow;		/* output row -> lookup row, UINT32_MAX for a NULL extension */
} ComboSt;

#define COMBO_UNEVAL	UINT32_MAX
#define COMBO_NONE		UINT32_MAX

#define COMBO_DIR_MAX	((uint64) 1 << 16)	/* the ids a combo of one input finds its entry by index (256 kB at most) */

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* combo.c */
void combo_bind_leaves(AggExec *ex, int ci, const uint64 *v);
uint32 combo_eval_slot(AggExec *ex, int ci, uint32 row, int sl);
void combo_emit(AggExec *ex, int ci, ComboEnt *ent, const uint32 *rows, int nm, uint32 nout);
ComboEnt combo_unmatched(AggExec *ex, int ci, const uint32 *prior);
bool combo_prior_keyed(AggExec *ex, int ci, int prior);
bool combo_prior_matches(AggExec *ex, int ci, int prior, uint32 row);
void combo_reset(AggExec *ex, int ci);
void combo_forget_priors(AggExec *ex, int ci);
void combo_trim(AggExec *ex);
uint64 combo_prior_vid(AggExec *ex, int ci, int p, uint32 out);
void combo_resolve(AggExec *ex, int ci, int qlevel, int n, const uint32 *tidx, const uint32 *ridx, uint32 *eid, const uint64 *alive, const Tup *tuples);
void combo_rows(AggExec *ex, int ci, const uint64 *alive, uint32 *o);
const uint32 *combo_outs(AggExec *ex, int ci);

#pragma GCC visibility pop

#endif
