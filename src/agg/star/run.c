/*
 * agg/star/run.c - the dimensions of a star join read from the plans of the node (agg/star/run.h).
 */
/* check-memory: allow-allocs every array here follows the dimensions of one statement, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "executor/nodeHash.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rangetypes.h"
#include "utils/typcache.h"

#include "agg/datum.h"
#include "agg/dim_cache.h"
#include "agg/join/lookup.h"
#include "agg/star/run.h"
#include "core/memtrack.h"

#define STAR_MAX_PARTS	(1 << 16)

struct SiftV6StarRun
{
	const SiftV6Ext *x;
	PlanState  *ps[SIFT_STAR_MAX_DIMS];
	SiftStarDim dim[SIFT_STAR_MAX_DIMS];
	bool		have[SIFT_STAR_MAX_DIMS];
	bool		read[SIFT_STAR_MAX_DIMS];	/* the plan has run: it is rescanned before it runs again */
	SiftV6FoundFact fact;		/* the fact scan whose found flags may answer a dimension (ops NULL: none) */
	SiftV6Found *found[SIFT_STAR_MAX_DIMS];
	SiftV6FoundStats fstats;
	SiftDimTry *dc[SIFT_STAR_MAX_DIMS];	/* what the pass may keep of the dimension (v6_dim_cache.h) */
	bool		kept[SIFT_STAR_MAX_DIMS];	/* the table is an entry of the cache: pinned until the tables are freed, not freed with them */
	uint32		hits;			/* the dimensions read from the cache, and those read from the plan to be kept */
	uint32		built;
	bool		shared[SIFT_STAR_MAX_DIMS];	/* the table is the statement's, shared with the other nodes that join the same relation (v6_join_lookup.h): not this node's to free */
	uint32		nshared;		/* the tables another node had read */
	uint32		nparts[SIFT_STAR_MAX_DIMS];
	uint32		part[SIFT_STAR_MAX_DIMS];
	uint32		nchunks[SIFT_STAR_MAX_DIMS];	/* the passes the part in hand is read in: its rows are more than a budget, whatever the parts, and are read a budget at a time */
	uint32		chunk[SIFT_STAR_MAX_DIMS];
	uint32	   *cstart[SIFT_STAR_MAX_DIMS];	/* the first row of each chunk among the rows of the part */
	bool		keyed[SIFT_STAR_MAX_DIMS];	/* the last chunk is the distinct keys of the part: a dimension of an outer, semi or anti join read in chunks */
	bool		dirty[SIFT_STAR_MAX_DIMS];	/* the part or chunk of the pass is not the one the table holds */
	uint32		passes;
	uint32		nulls;			/* the rows of a NULL key the read of a dimension in hand passed over (take_row) */
	uint32		key_filters;	/* the columns of the fact table the keys of a dimension filtered in its last scan (sift_v6_star_scan_keys) */
	uint32		scans_left;		/* the scans of the fact table the pass still has to read */
	uint64		budget;
	uint64		bytes;
	MemoryContext qcx;			/* the query: the plans of the dimensions allocate their scans in it */
	MemoryContext cx;			/* the tables */
	MemoryContext rowcx;		/* one row of a plan */
	SiftMemTracker mt;			/* agg_lookup: the tables of the pass, held to half of hash_mem */
};

uint64
sift_v6_star_budget(int ndims)
{
	return Min(Max((uint64) get_hash_memory_limit() / 2 / Max(ndims, 1), 4096), MaxAllocSize);
}

static uint64
star_budget(const SiftV6Ext *x)
{
	return sift_v6_star_budget(x->ndims);
}

SiftV6StarRun *
sift_v6_star_begin(const SiftV6Ext *x, PlanState *const *children, MemoryContext cx)
{
	SiftV6StarRun *r = MemoryContextAllocZero(cx, sizeof(SiftV6StarRun));

	r->x = x;
	r->qcx = cx;
	r->cx = sift_mem_allocset_create(cx, "sift agg v6 star dimensions");
	r->rowcx = sift_mem_allocset_create(cx, "sift agg v6 star row");
	sift_mem_init(&r->mt, "agg_lookup", r->cx, false, SIFT_BUDGET_HASH_MEM, 1, 2);
	r->budget = star_budget(x);
	r->passes = 1;
	for (int d = 0; d < x->ndims; d++)
	{
		r->ps[d] = children[d];
		r->nparts[d] = r->nchunks[d] = 1;
	}
	return r;
}

static SiftStarCls
cls_of_type(Oid typid)
{
	if (!sift_v6_type_native(typid))
		return get_typbyval(typid) ? SSC_INT : SSC_STR;
	switch (sift_v6_class(typid))
	{
		case SV6C_F64:
			return SSC_F64;
		case SV6C_TEXT:
			return SSC_STR;
		default:
			return SSC_INT;
	}
}

static int64
int_of(Oid typid, Datum v)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(v);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(v);
		case BOOLOID:
			return DatumGetBool(v);
		default:
			return DatumGetInt64(v);
	}
}

/* How a cell of a column of a dimension is read, resolved once a read: its class, its type and, for a type no native vector holds, the length of the type (-1: a varlena) */
typedef struct StarCell
{
	SiftStarCls cls;
	Oid			typid;
	bool		native;
	int16		typlen;
} StarCell;

typedef struct StarRead
{
	SiftStarShape shape;
	StarCell	key[SIFT_STAR_MAX_KEYS];
	StarCell	pay[SIFT_STAR_MAX_PAY];
	TypeCacheEntry *range_tc;	/* SV6J_RANGE_SMALL: the range type of the key */
} StarRead;

/* the bytes of a datum of a type no native vector holds and that is passed by reference: the whole value, in the memory of the row */
static void
datum_bytes(int16 typlen, Datum v, const char **p, uint32 *len)
{
	if (typlen == -1)
	{
		struct varlena *t = pg_detoast_datum((struct varlena *) DatumGetPointer(v));

		*p = (const char *) t;
		*len = VARSIZE(t);
	}
	else
	{
		*p = DatumGetPointer(v);
		*len = (uint32) typlen;
	}
}

static void
put_cell(const StarCell *c, Datum v, bool isnull, int64 *i, const char **p, uint32 *len, bool *null)
{
	*null = isnull;
	if (isnull)
		return;
	if (c->cls == SSC_STR && !c->native)
		datum_bytes(c->typlen, v, p, len);
	else if (c->cls == SSC_STR)
	{
		struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(v));

		*p = VARDATA_ANY(t);
		*len = VARSIZE_ANY_EXHDR(t);
	}
	else if (c->cls == SSC_F64)
	{
		const double f = c->typid == FLOAT4OID ? (double) DatumGetFloat4(v) : DatumGetFloat8(v);

		memcpy(i, &f, sizeof(double));
	}
	else
		*i = int_of(c->typid, v);
}

/* the bytes a value of an SSC_INT payload column of the type takes in a dense table: those int_of reads */
static uint8
int_width(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
			return 1;
		case INT2OID:
			return 2;
		case INT4OID:
		case DATEOID:
			return 4;
		default:
			return 8;
	}
}

static StarCell
cell_of(SiftStarCls cls, Oid typid)
{
	const bool	native = sift_v6_type_native(typid);

	return (StarCell) {.cls = cls,.typid = typid,.native = native,.typlen = native ? 0 : get_typlen(typid)};
}

static void
shape_of(const SiftV6Ext *x, int d, uint64 budget, SiftStarShape *sh)
{
	const SiftV6XDim *xd = &x->dim[d];

	*sh = (SiftStarShape) {.mode = xd->mode,.nkeys = xd->nkeys,.npay = xd->npay,.budget = budget,.lo_inc = true,.on = x->on[d] != NIL};
	for (int k = 0; k < xd->nkeys; k++)
		sh->kcls[k] = sift_star_period(xd->mode) ? SSC_INT : cls_of_type(xd->ktype[k]);
	for (int c = 0; c < xd->npay; c++)
	{
		sh->pcls[c] = cls_of_type(xd->ptype[c]);
		sh->pwidth[c] = int_width(xd->ptype[c]);
	}
}

static void
read_of(const SiftV6Ext *x, int d, uint64 budget, StarRead *rd)
{
	const SiftV6XDim *xd = &x->dim[d];

	shape_of(x, d, budget, &rd->shape);
	for (int k = 0; k < xd->nkeys; k++)
		rd->key[k] = cell_of(rd->shape.kcls[k], xd->ktype[k]);
	for (int c = 0; c < xd->npay; c++)
		rd->pay[c] = cell_of(rd->shape.pcls[c], xd->ptype[c]);
	rd->range_tc = sift_star_period(xd->mode) ? lookup_type_cache(xd->ktype[0], TYPECACHE_RANGE_INFO) : NULL;
}

/* The period a range column holds, as the int64 bounds [lo, hi) of its elements (a date or an int4): the row of an empty or NULL range is no period, and its first key is its lower bound (the one key a period dimension has) */
static void
take_period(const SiftV6XDim *xd, TypeCacheEntry *tc, Datum v, bool isnull, SiftStarRow *row)
{
	RangeBound	lower,
				upper;
	bool		empty = isnull;

	if (!isnull)
		range_deserialize(tc, DatumGetRangeTypeP(v), &lower, &upper, &empty);
	row->lo_null = row->hi_null = empty;
	row->key_null[0] = isnull;
	if (empty)
		return;
	row->lo = lower.infinite ? PG_INT64_MIN : int_of(xd->ftype[0], lower.val) + !lower.inclusive;
	row->hi = upper.infinite ? PG_INT64_MAX : int_of(xd->ftype[0], upper.val) + upper.inclusive;
	row->key_i[0] = row->lo;
}

/*
 * The cells of the row of the plan the keys and payload columns are, in the memory of the row; false when the row is not of the part. A row of a NULL key joins nothing: it is taken only when
 * nulls asks for those rows of a dimension a RIGHT or FULL join keeps (counted in r->nulls when passed over), which the end of a pass sends to the output without holding them in its table.
 */
static bool
take_row(SiftV6StarRun *r, int d, const StarRead *rd, TupleTableSlot *slot, uint32 nparts, uint32 part, bool nulls, SiftStarRow *row)
{
	const SiftV6XDim *xd = &r->x->dim[d];
	bool		anynull = false;

	for (int k = 0; k < xd->nkeys; k++)
	{
		bool		isnull;
		const Datum v = slot_getattr(slot, xd->kcol[k], &isnull);

		if (sift_star_period(xd->mode))
			take_period(xd, rd->range_tc, v, isnull, row);
		else
			put_cell(&rd->key[k], v, isnull, &row->key_i[k], &row->key_p[k], &row->key_len[k], &row->key_null[k]);
		anynull |= isnull;
	}
	r->nulls += anynull && !nulls && sift_star_preserves_dim(rd->shape.mode);
	if (anynull != nulls)
		return false;
	if (!nulls && nparts > 1 && sift_star_row_part(&rd->shape, row, nparts) != part)
		return false;
	for (int c = 0; c < xd->npay; c++)
	{
		bool		isnull;
		const Datum v = slot_getattr(slot, xd->pcol[c], &isnull);

		put_cell(&rd->pay[c], v, isnull, &row->pay_i[c], &row->pay_p[c], &row->pay_len[c], &row->pay_null[c]);
	}
	return true;
}

static TupleTableSlot *
next_row(SiftV6StarRun *r, int d)
{
	MemoryContext old = MemoryContextSwitchTo(r->qcx);
	TupleTableSlot *slot = ExecProcNode(r->ps[d]);

	MemoryContextSwitchTo(old);
	return slot;
}

static void
rewind_dim(SiftV6StarRun *r, int d)
{
	if (r->read[d])
	{
		MemoryContext old = MemoryContextSwitchTo(r->qcx);

		ExecReScan(r->ps[d]);
		MemoryContextSwitchTo(old);
	}
	r->read[d] = true;
	r->nulls = 0;
}

/* The next row of the plan of dimension d that is of the part, or with nulls of a NULL key (its cells in the memory of the row, until the next call): false at the end */
static bool
next_taken(SiftV6StarRun *r, int d, const StarRead *rd, uint32 nparts, uint32 part, bool nulls, SiftStarRow *row)
{
	TupleTableSlot *slot;

	while ((slot = next_row(r, d)) != NULL && !TupIsNull(slot))
	{
		MemoryContext old;
		bool		keep;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(r->rowcx);
		old = MemoryContextSwitchTo(r->rowcx);
		*row = (SiftStarRow) {0};
		keep = take_row(r, d, rd, slot, nparts, part, nulls, row);
		MemoryContextSwitchTo(old);
		if (keep)
			return true;
	}
	return false;
}

static bool
part_row(SiftV6StarRun *r, int d, const StarRead *rd, uint32 nparts, uint32 part, SiftStarRow *row)
{
	return next_taken(r, d, rd, nparts, part, false, row);
}

/* The pass of the distinct keys of the part: the last chunk of a keyed dimension */
static bool
keys_pass(const SiftV6StarRun *r, int d)
{
	return r->keyed[d] && r->chunk[d] + 1 == r->nchunks[d];
}

/*
 * How the table of the pass joins: as the statement says, but a keyed dimension joins the rows of each chunk as an inner join (a right one when it keeps its own rows), and its keys, in the last pass,
 * as a semi join (semi) or an anti join (left, full, anti: the fact rows of a key no row of the part has, which an outer join keeps with a NULL dimension side)
 */
static SiftV6JoinMode
pass_mode(const SiftV6StarRun *r, int d)
{
	const SiftV6JoinMode mode = r->x->dim[d].mode;

	if (!r->keyed[d])
		return mode;
	if (keys_pass(r, d))
		return mode == SV6J_SEMI_EQUI ? SV6J_SEMI_EQUI : SV6J_ANTI_EQUI;
	return sift_star_preserves_dim(mode) ? SV6J_RIGHT_EQUI : SV6J_INNER_EQUI;
}

/* Reads the plan of the dimension into its table: the rows [from, to) of the part of its keys, or its distinct keys in the keys pass; false when they do not fit the budget */
static bool
read_dim(SiftV6StarRun *r, int d, uint32 nparts, uint32 part, uint32 from, uint32 to, uint64 budget)
{
	StarRead	sh;
	SiftStarDim *dim = &r->dim[d];
	SiftStarRow row;
	bool		fits = true;
	const bool	keys = keys_pass(r, d);
	uint32		at = 0;

	read_of(r->x, d, budget, &sh);
	sh.shape.mode = pass_mode(r, d);
	rewind_dim(r, d);
	sift_star_dim_init(dim, &sh.shape);
	while (fits && at < to && part_row(r, d, &sh, nparts, part, &row))
		if (at++ >= from)
			fits = keys ? sift_star_dim_add_key(dim, &row) : sift_star_dim_add(dim, &row);
	if (fits)
		sift_star_dim_finish(dim);
	dim->nnull = r->nulls;
	return fits;
}

/* The rows of a part that no number of parts makes smaller (many rows of one key) in the chunks that each fit: the first row of each */
static void
plan_chunks(SiftV6StarRun *r, int d, uint32 nparts, uint32 part)
{
	StarRead	sh;
	SiftStarDim dim;
	SiftStarRow row;
	uint32		cap = 8,
				at = 0;

	read_of(r->x, d, r->budget, &sh);
	rewind_dim(r, d);
	if (r->cstart[d] != NULL)
		pfree(r->cstart[d]);
	r->cstart[d] = palloc(sizeof(uint32) * cap);
	r->cstart[d][0] = 0;
	r->nchunks[d] = 1;
	sift_star_dim_init(&dim, &sh.shape);
	while (part_row(r, d, &sh, nparts, part, &row))
	{
		if (!sift_star_dim_add(&dim, &row))
		{
			if (r->nchunks[d] == cap)
				r->cstart[d] = repalloc(r->cstart[d], sizeof(uint32) * (cap *= 2));
			r->cstart[d][r->nchunks[d]++] = at;
			sift_star_dim_free(&dim);
			sift_star_dim_init(&dim, &sh.shape);
			if (!sift_star_dim_add(&dim, &row))
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a row of a dimension of the star join does not fit one allocation"),
								errhint("Run the statement with sift.enable_agg = off.")));
		}
		at++;
	}
	sift_star_dim_free(&dim);
}

/* The rows of the chunk in hand among those of the part (the keys pass reads them all) */
static uint32
chunk_from(const SiftV6StarRun *r, int d)
{
	return r->nchunks[d] > 1 && !keys_pass(r, d) ? r->cstart[d][r->chunk[d]] : 0;
}

static uint32
chunk_to(const SiftV6StarRun *r, int d)
{
	return r->nchunks[d] - r->keyed[d] > r->chunk[d] + 1 ? r->cstart[d][r->chunk[d] + 1] : UINT32_MAX;
}

/* The join keeps the fact rows of the keys the dimension has (semi) or has not (anti, and the fact side of a left or full join), whatever its rows */
static bool
joins_keys(SiftV6JoinMode mode)
{
	return mode == SV6J_SEMI_EQUI || mode == SV6J_ANTI_EQUI || sift_star_keeps_fact(mode);
}

/*
 * The passes of a part that no number of parts makes smaller: the chunks of its rows that each fit, each matched by every fact row of the part (a right or full join sends the rows of the chunk
 * no fact row matched at the end of its pass), then for a join of the keys the distinct keys of the part (a semi or anti join needs those alone). The keys tell which fact row matches a row only
 * when the condition is the keys: a range join, or a join of the keys with a condition on the pairs, is not read in chunks.
 */
static void
chunk_dim(SiftV6StarRun *r, int d)
{
	const SiftV6JoinMode mode = r->x->dim[d].mode;

	if (mode == SV6J_RANGE_SMALL || mode == SV6J_RANGE_RIGHT || (joins_keys(mode) && r->x->on[d] != NIL))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a dimension of a star join does not fit work_mem and no chunks of it answer the join"),
						errdetail("A range join, or an outer, semi or anti join with a condition on its pairs, is not read in chunks."), errhint("Increase work_mem.")));
	r->keyed[d] = joins_keys(mode);
	r->nchunks[d] = 0;
	if (mode != SV6J_SEMI_EQUI && mode != SV6J_ANTI_EQUI)
		plan_chunks(r, d, r->nparts[d], r->part[d]);
	r->nchunks[d] += r->keyed[d];
	r->chunk[d] = 0;
}

static void
free_dim(SiftV6StarRun *r, int d)
{
	if (r->have[d] && !r->kept[d] && !r->shared[d])
		sift_star_dim_free(&r->dim[d]);
	r->have[d] = r->kept[d] = r->shared[d] = false;
}

/* The table of dimension d as a statement before it kept it, when the table it was read from is still the one */
static bool
find_kept(SiftV6StarRun *r, int d)
{
	SiftStarShape sh;

	shape_of(r->x, d, r->budget, &sh);
	r->dc[d] = sift_dimcache_try(r->ps[d], &r->x->dim[d], &sh, r->cx);
	if (r->dc[d] == NULL || !sift_dimcache_find(r->dc[d], r->budget, &r->dim[d]))
		return false;
	r->have[d] = r->kept[d] = true;
	r->hits++;
	return true;
}

/* Reads dimension d for the pass; in the first pass, with the classes of keys it needs to fit, or as the cache holds it when it need not be read. A table that fits whole and is not kept for later statements is the statement's: a node that joins the same relation takes it */
static void
build_dim(SiftV6StarRun *r, int d, uint32 pass)
{
	MemoryContext old = MemoryContextSwitchTo(r->cx);
	bool		settled = false;

	free_dim(r, d);
	r->dc[d] = NULL;
	if (pass == 0 && r->fact.ops != NULL)
		r->found[d] = sift_v6_found_open(&r->fact, r->x, d, r->ps[d], r->cx, &r->fstats);
	if (r->found[d] != NULL)
	{
		r->dirty[d] = false;
		MemoryContextSwitchTo(old);
		return;
	}
	if (pass == 0 && r->nparts[d] == 1 && find_kept(r, d))
	{
		r->bytes = Max(r->bytes, r->dim[d].bytes);
		r->dirty[d] = false;
		MemoryContextSwitchTo(old);
		return;
	}
	do
	{
		const bool	keep = pass == 0 && r->nparts[d] == 1 && r->dc[d] != NULL;
		SiftStarShape sh;
		const SiftStarDim *hit = NULL;
		SiftV6SharedDim *share = NULL;
		bool		fits,
					small,
					last,
					whole = true;

		if (!keep && pass == 0 && r->nparts[d] == 1)
		{
			shape_of(r->x, d, r->budget, &sh);
			share = sift_v6_shared_dim_open(r->ps[d], &r->x->dim[d], &sh, &hit);
		}
		if (hit != NULL)
		{
			r->dim[d] = *hit;
			r->shared[d] = true;
			r->nshared++;
			break;
		}
		if (keep)
			MemoryContextSwitchTo(sift_dimcache_read_cx(r->dc[d]));
		else if (share != NULL)
			MemoryContextSwitchTo(sift_v6_shared_dim_cx(share));
		fits = read_dim(r, d, r->nparts[d], r->part[d], chunk_from(r, d), chunk_to(r, d), r->budget);
		MemoryContextSwitchTo(r->cx);
		small = fits && (r->nparts[d] == 1 || r->dim[d].bytes <= r->budget / 2);
		last = small || pass > 0 || r->nparts[d] >= STAR_MAX_PARTS || !sift_star_parts_split(&r->dim[d].shape) ||
			!sift_star_dim_splits(&r->dim[d], r->nparts[d] * 2);
		if (!fits && last)
		{
			whole = false;
			sift_star_dim_free(&r->dim[d]);
			if (share != NULL)
				sift_v6_shared_dim_abort(share);
			share = NULL;
			if (!keys_pass(r, d))
			{
				chunk_dim(r, d);
				fits = read_dim(r, d, r->nparts[d], r->part[d], chunk_from(r, d), chunk_to(r, d), r->budget);
			}
			if (!fits)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the keys of a dimension of a star join do not fit work_mem"),
								errhint("Increase work_mem.")));
		}
		settled = fits && last;
		if (keep && fits && whole)
		{
			r->built++;
			r->kept[d] = sift_dimcache_keep(r->dc[d], &r->dim[d]);
		}
		else if (settled && share != NULL)
		{
			sift_v6_shared_dim_publish(share, &r->dim[d]);
			r->shared[d] = true;
		}
		if (settled)
			continue;
		sift_star_dim_free(&r->dim[d]);
		if (share != NULL)
			sift_v6_shared_dim_abort(share);
		r->nparts[d] *= 2;
		r->part[d] = 0;
	} while (!settled);
	r->have[d] = true;
	r->dirty[d] = false;
	r->bytes = Max(r->bytes, r->dim[d].bytes);
	MemoryContextSwitchTo(old);
}

/* the bytes of strings a row of width bytes puts in the arena of the table: each string is aligned */
static double
str_bytes(const SiftStarShape *sh, double width)
{
	int			nstr = 0;

	for (int k = 0; k < sh->nkeys; k++)
		nstr += sh->kcls[k] == SSC_STR;
	for (int c = 0; c < sh->npay; c++)
		nstr += sh->pcls[c] == SSC_STR;
	return nstr > 0 ? width + MAXIMUM_ALIGNOF * nstr : 0;
}

double
sift_v6_star_dim_bytes(SiftV6JoinMode mode, Oid ktype, const Oid *ptypes, int npay, double rows, double width, double span, uint64 budget)
{
	SiftStarShape sh = {.mode = mode,.nkeys = 1,.npay = npay,.budget = budget,.lo_inc = true};

	sh.kcls[0] = cls_of_type(ktype);
	for (int c = 0; c < npay; c++)
	{
		sh.pcls[c] = cls_of_type(ptypes[c]);
		sh.pwidth[c] = int_width(ptypes[c]);
	}
	return sift_star_dense_est(&sh, rows, span, str_bytes(&sh, width));
}

double
sift_v6_star_reads(const SiftV6Ext *x, int d, double rows, double width, double *passes)
{
	const uint64 budget = star_budget(x);
	SiftStarShape sh;
	double		parts = 2,
				strs,
				bytes;

	shape_of(x, d, budget, &sh);
	strs = str_bytes(&sh, width);
	bytes = x->dim[d].span > 0 ? sift_star_dense_est(&sh, rows, x->dim[d].span, strs) : -1;
	if (bytes < 0)
		bytes = sift_star_table_est(&sh, rows, strs);
	*passes = 1;
	if (bytes <= budget)
		return 1;
	if (!sift_star_parts_split(&sh))
	{
		*passes = x->dim[d].mode == SV6J_INNER_EQUI ? ceil(bytes / budget) : 1;
		return 2 + (x->dim[d].mode == SV6J_INNER_EQUI ? *passes : 0);
	}
	while (parts < STAR_MAX_PARTS && sift_star_table_est(&sh, rows / parts, strs) > budget / 2)
		parts *= 2;
	*passes = parts;
	return log2(parts) + parts;
}

/* The pass sends the rows of a NULL key of preserved dimension d: the one of its first chunk of part 0, when its read passed over such rows or stopped short of the end of its plan (a chunk) */
static bool
streams_nulls(const SiftV6StarRun *r, int d)
{
	return sift_star_preserves_dim(r->x->dim[d].mode) && r->found[d] == NULL && r->part[d] == 0 && r->chunk[d] == 0 && (r->dim[d].nnull > 0 || r->nchunks[d] > 1);
}

/* The odometer of the passes: the chunks of the part of a dimension, then its parts, then the next dimension's. The chunks of a part are known when it is read */
static bool
next_pass(SiftV6StarRun *r)
{
	for (int d = 0; d < r->x->ndims; d++)
	{
		if (r->chunk[d] + 1 < r->nchunks[d])
		{
			r->chunk[d]++;
			r->dirty[d] = true;
			return true;
		}
		if (r->part[d] + 1 < r->nparts[d])
		{
			r->part[d]++;
			r->chunk[d] = 0;
			r->nchunks[d] = 1;
			r->keyed[d] = false;
			r->dirty[d] = true;
			return true;
		}
		if (r->part[d] != 0 || r->chunk[d] != 0)
			r->dirty[d] = true;
		r->part[d] = r->chunk[d] = 0;
		r->nchunks[d] = 1;
		r->keyed[d] = false;
	}
	return false;
}

void
sift_v6_star_fact(SiftV6StarRun *r, const SiftV6Spec *spec, const SiftV6FoundFact *fact)
{
	if (sift_v6_found_serves(spec, fact))
		r->fact = *fact;
}

bool
sift_v6_star_found_col(const SiftV6StarRun *r, int d, SiftV6Col *col)
{
	if (r->found[d] == NULL)
		return false;
	*col = sift_v6_found_col(r->found[d]);
	return true;
}

const SiftV6FoundStats *
sift_v6_star_found_stats(const SiftV6StarRun *r)
{
	return &r->fstats;
}

bool
sift_v6_star_joins_none(const SiftV6StarRun *r)
{
	for (int d = 0; d < r->x->ndims && !sift_star_preserves_dim(r->x->dim[d].mode); d++)
		if (r->found[d] == NULL && r->dim[d].nrows == 0 &&
			(r->x->dim[d].mode == SV6J_INNER_EQUI || r->x->dim[d].mode == SV6J_SEMI_EQUI || r->x->dim[d].mode == SV6J_RANGE_SEMI))
			return true;
	return false;
}

SiftStarOpen
sift_v6_star_open(SiftV6StarRun *r, uint32 pass, uint32 scans)
{
	bool		preserves = false;

	if (pass == 0)
		for (int d = 0; d < r->x->ndims; d++)
		{
			r->part[d] = r->chunk[d] = 0;
			r->nchunks[d] = 1;
			r->keyed[d] = false;
			r->dirty[d] = true;
		}
	else if (!next_pass(r))
		return SSO_DONE;
	r->passes = pass + 1;
	r->scans_left = scans;
	for (int d = 0; d < r->x->ndims; d++)
		if (r->dirty[d])
			build_dim(r, d, pass);
	sift_mem_used(&r->mt);
	for (int d = 0; d < r->x->ndims; d++)
		preserves |= sift_star_preserves_dim(r->x->dim[d].mode);
	for (int d = 0; d < r->x->ndims; d++)
		if (r->found[d] == NULL && r->dim[d].nrows == 0 && ((r->x->dim[d].mode == SV6J_RIGHT_EQUI && !streams_nulls(r, d)) || r->x->dim[d].mode == SV6J_RANGE_RIGHT))
			return SSO_EMPTY;
	return !preserves && sift_v6_star_joins_none(r) ? SSO_EMPTY : SSO_SCAN;
}

/* The rows of a NULL key of preserved dimension d, read from its plan in tables of a budget each, come out as rows no fact row matched */
static void
stream_nulls(SiftV6StarRun *r, int d, SiftV6Pipe *p)
{
	MemoryContext cx = sift_mem_allocset_create(r->cx, "sift agg v6 star NULL keys");
	MemoryContext old = MemoryContextSwitchTo(cx);
	StarRead	rd;
	SiftStarDim batch;
	SiftStarRow row;
	bool		more;

	read_of(r->x, d, r->budget, &rd);
	rewind_dim(r, d);
	sift_star_dim_init(&batch, &rd.shape);
	do
	{
		more = next_taken(r, d, &rd, 1, 0, true, &row);
		if (more && sift_star_dim_add(&batch, &row))
			continue;
		sift_star_dim_finish(&batch);
		MemoryContextSwitchTo(old);
		sift_v6_pipe_unmatched(p, d, &batch);
		MemoryContextSwitchTo(cx);
		MemoryContextReset(cx);
		sift_star_dim_init(&batch, &rd.shape);
		if (more && !sift_star_dim_add(&batch, &row))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a row of a dimension of the star join does not fit one allocation"),
							errhint("Run the statement with sift.enable_agg = off.")));
	} while (more);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

void
sift_v6_star_scan_end(SiftV6StarRun *r, SiftV6Pipe *p)
{
	if (--r->scans_left > 0)
		return;
	for (int d = 0; d < r->x->ndims; d++)
		if (streams_nulls(r, d))
			stream_nulls(r, d, p);
	sift_v6_pipe_finish(p);
}

void
sift_v6_star_attach(const SiftV6StarRun *r, SiftV6Pipe *p)
{
	for (int d = 0; d < r->x->ndims; d++)
	{
		p->dim[d] = &r->dim[d];
		p->found[d] = r->found[d];
		p->nparts[d] = r->nparts[d];
		p->part[d] = r->part[d];
	}
}

bool
sift_v6_star_one_pass(const SiftV6StarRun *r)
{
	for (int d = 0; d < r->x->ndims; d++)
		if (r->nparts[d] > 1 || r->nchunks[d] > 1)
			return false;
	return true;
}

/* The tables as one block: the offset of each from the start, then each table flat (agg/star/flat.c) */
uint64
sift_v6_star_flat_bytes(const SiftV6StarRun *r)
{
	uint64		bytes = MAXALIGN(r->x->ndims * sizeof(uint64));

	for (int d = 0; d < r->x->ndims; d++)
		bytes += sift_star_dim_flat_bytes(&r->dim[d]);
	return bytes;
}

uint64
sift_v6_star_flat_write(const SiftV6StarRun *r, char *dst)
{
	uint64	   *off = (uint64 *) dst;
	uint64		at = MAXALIGN(r->x->ndims * sizeof(uint64)),
				rows = 0;

	for (int d = 0; d < r->x->ndims; d++)
	{
		off[d] = at;
		sift_star_dim_flat_write(&r->dim[d], dst + at);
		at += sift_star_dim_flat_bytes(&r->dim[d]);
		rows += r->dim[d].nrows;
	}
	return rows;
}

void
sift_v6_star_map(SiftV6StarRun *r, const char *src)
{
	const uint64 *off = (const uint64 *) src;

	for (int d = 0; d < r->x->ndims; d++)
	{
		free_dim(r, d);
		sift_star_dim_flat_map(&r->dim[d], src + off[d]);
		r->have[d] = r->shared[d] = true;
		r->found[d] = NULL;
		r->part[d] = r->chunk[d] = 0;
		r->nparts[d] = r->nchunks[d] = 1;
		r->dirty[d] = false;
	}
	r->passes = 1;
}

uint32
sift_v6_star_passes(const SiftV6StarRun *r)
{
	return r->passes;
}

uint32
sift_v6_star_shared(const SiftV6StarRun *r)
{
	return r->nshared;
}

uint32
sift_v6_star_key_filters(const SiftV6StarRun *r)
{
	return r->key_filters;
}

uint64
sift_v6_star_bytes(const SiftV6StarRun *r)
{
	return r->bytes;
}

bool
sift_v6_star_cache(const SiftV6StarRun *r, uint32 *hits, uint32 *built)
{
	*hits = r->hits;
	*built = r->built;
	return r->hits + r->built > 0;
}

/* A column the zones of the index order by the int64 of its value */
static bool
int_key_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID;
}

static Datum
key_datum(Oid typid, int64 v)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) v);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) v);
		default:
			return Int64GetDatum(v);
	}
}

static void
bound_key(Relation index, AttrNumber att, Oid typid, StrategyNumber strategy, Oid opno, int64 v, ScanKey key)
{
	ScanKeyEntryInitialize(key, 0, att, strategy, typid, index->rd_indcollation[att - 1], get_opcode(opno), key_datum(typid, v));
}

/* att = ANY(the keys): the point directory and the marks of a part read the rows of these keys only */
static void
in_key(Relation index, AttrNumber att, Oid typid, Oid opno, const SiftStarPush *push, ScanKey key)
{
	Datum	   *vals = palloc(push->nkeys * sizeof(Datum));
	int16		len;
	bool		byval;
	char		align;

	for (uint32 i = 0; i < push->nkeys; i++)
		vals[i] = key_datum(typid, push->keys[i]);
	get_typlenbyvalalign(typid, &len, &byval, &align);
	ScanKeyEntryInitialize(key, SK_SEARCHARRAY, att, BTEqualStrategyNumber, typid, index->rd_indcollation[att - 1], get_opcode(opno),
						   PointerGetDatum(construct_array(vals, (int) push->nkeys, typid, len, byval, align)));
}

/* A dimension that holds no key joins no fact row: a range no value is in, which the zones of every granule rule out (a preserved dimension still gets its rows) */
static SiftStarPush
empty_push(SiftStarPush push)
{
	return push.empty ? (SiftStarPush) {.kind = SSP_RANGE,.lo = 1,.hi = 0,.empty = true} : push;
}

ScanKey
sift_v6_star_scan_keys(SiftV6StarRun *r, Relation index, ScanKey keys, int nkeys, uint32 in_max, int *n)
{
	ScanKey		out = palloc(((Size) nkeys + 3 * r->x->ndims) * sizeof(ScanKeyData));
	Bitmapset  *filtered = NULL;

	if (nkeys > 0)
		memcpy(out, keys, nkeys * sizeof(ScanKeyData));
	*n = nkeys;
	r->key_filters = 0;
	for (int d = 0; d < r->x->ndims && !sift_star_preserves_dim(r->x->dim[d].mode); d++)
	{
		const SiftV6XDim *xd = &r->x->dim[d];
		const SiftStarPush push = r->found[d] != NULL ? (SiftStarPush) {.kind = SSP_NONE} : empty_push(sift_star_push(&r->dim[d]));
		const AttrNumber att = (AttrNumber) xd->fatt[0];
		const Oid	type = xd->ftype[0],
					family = att >= 1 && att <= index->rd_index->indnatts ? index->rd_opfamily[att - 1] : InvalidOid,
					ge = OidIsValid(family) ? get_opfamily_member(family, type, type, BTGreaterEqualStrategyNumber) : InvalidOid,
					le = OidIsValid(family) ? get_opfamily_member(family, type, type, BTLessEqualStrategyNumber) : InvalidOid,
					eq = OidIsValid(family) ? get_opfamily_member(family, type, type, BTEqualStrategyNumber) : InvalidOid;

		if (push.kind == SSP_NONE || (xd->mode != SV6J_INNER_EQUI && xd->mode != SV6J_SEMI_EQUI) || xd->fsrc[0] != SV6SRC_ATTR || xd->kx[0] != 0 ||
			type != xd->ktype[0] || !int_key_type(type) || !OidIsValid(ge) || !OidIsValid(le))
			continue;
		bound_key(index, att, type, BTGreaterEqualStrategyNumber, ge, push.lo, &out[(*n)++]);
		bound_key(index, att, type, BTLessEqualStrategyNumber, le, push.hi, &out[(*n)++]);
		filtered = bms_add_member(filtered, att);
		if (push.kind == SSP_EXACT && push.nkeys <= in_max && OidIsValid(eq))
			in_key(index, att, type, eq, &push, &out[(*n)++]);
	}
	r->key_filters = (uint32) bms_num_members(filtered);
	return out;
}

void
sift_v6_star_close(SiftV6StarRun *r)
{
	for (int d = 0; d < r->x->ndims; d++)
	{
		free_dim(r, d);
		r->dc[d] = NULL;
		r->found[d] = NULL;
		r->cstart[d] = NULL;
		r->nchunks[d] = 1;
		r->keyed[d] = false;
	}
	MemoryContextReset(r->cx);
	sift_mem_release_os(r->bytes);
}
