/*
 * agg/v6/v6_star_run.c - the dimensions of a star join read from the plans of the node (agg/v6/v6_star_run.h).
 */
/* check-memory: allow-allocs every array here follows the dimensions of one statement, not the rows of the table */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "agg/v6/v6_dim_cache.h"
#include "agg/v6/v6_star_run.h"
#include "core/memtrack.h"

#define STAR_MAX_PARTS	(1 << 16)

struct SiftV6StarRun
{
	const SiftV6Ext *x;
	PlanState  *ps[SIFT_STAR_MAX_DIMS];
	SiftStarDim dim[SIFT_STAR_MAX_DIMS];
	bool		have[SIFT_STAR_MAX_DIMS];
	bool		read[SIFT_STAR_MAX_DIMS];	/* the plan has run: it is rescanned before it runs again */
	SiftDimTry *dc[SIFT_STAR_MAX_DIMS];	/* what the pass may keep of the dimension (v6_dim_cache.h) */
	bool		kept[SIFT_STAR_MAX_DIMS];	/* the table is an entry of the cache: pinned until the tables are freed, not freed with them */
	uint32		hits;			/* the dimensions read from the cache, and those read from the plan to be kept */
	uint32		built;
	uint32		nparts[SIFT_STAR_MAX_DIMS];
	uint32		part[SIFT_STAR_MAX_DIMS];
	uint32		passes;
	uint64		budget;
	uint64		bytes;
	MemoryContext qcx;			/* the query: the plans of the dimensions allocate their scans in it */
	MemoryContext cx;			/* the tables */
	MemoryContext rowcx;		/* one row of a plan */
};

SiftV6StarRun *
sift_v6_star_begin(const SiftV6Ext *x, PlanState *const *children, MemoryContext cx)
{
	SiftV6StarRun *r = MemoryContextAllocZero(cx, sizeof(SiftV6StarRun));

	r->x = x;
	r->qcx = cx;
	r->cx = sift_mem_allocset_create(cx, "sift agg v6 star dimensions");
	r->rowcx = sift_mem_allocset_create(cx, "sift agg v6 star row");
	r->budget = Max((uint64) work_mem * 1024 / 2 / Max(x->ndims, 1), 4096);
	r->passes = 1;
	for (int d = 0; d < x->ndims; d++)
	{
		r->ps[d] = children[d];
		r->nparts[d] = 1;
	}
	return r;
}

static SiftStarCls
cls_of_type(Oid typid)
{
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

static void
put_cell(SiftStarCls cls, Oid typid, Datum v, bool isnull, int64 *i, const char **p, uint32 *len, bool *null)
{
	*null = isnull;
	if (isnull)
		return;
	if (cls == SSC_STR)
	{
		struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(v));

		*p = VARDATA_ANY(t);
		*len = VARSIZE_ANY_EXHDR(t);
	}
	else if (cls == SSC_F64)
	{
		const double f = typid == FLOAT4OID ? (double) DatumGetFloat4(v) : DatumGetFloat8(v);

		memcpy(i, &f, sizeof(double));
	}
	else
		*i = int_of(typid, v);
}

static void
shape_of(const SiftV6XDim *xd, uint64 budget, SiftStarShape *sh)
{
	*sh = (SiftStarShape) {.mode = xd->mode,.nkeys = xd->nkeys,.npay = xd->npay,.budget = budget};
	for (int k = 0; k < xd->nkeys; k++)
		sh->kcls[k] = cls_of_type(xd->ktype[k]);
	for (int c = 0; c < xd->npay; c++)
		sh->pcls[c] = cls_of_type(xd->ptype[c]);
}

/* The cells of the row of the plan the keys and payload columns are, in the memory of the row; false when the row is not of the part (or has a NULL key: it is joined to nothing) */
static bool
take_row(SiftV6StarRun *r, int d, const SiftStarShape *sh, TupleTableSlot *slot, uint32 nparts, uint32 part, SiftStarRow *row)
{
	const SiftV6XDim *xd = &r->x->dim[d];
	bool		anynull = false;

	for (int k = 0; k < xd->nkeys; k++)
	{
		bool		isnull;
		const Datum v = slot_getattr(slot, xd->kcol[k], &isnull);

		put_cell(sh->kcls[k], xd->ktype[k], v, isnull, &row->key_i[k], &row->key_p[k], &row->key_len[k], &row->key_null[k]);
		anynull |= isnull;
	}
	if (anynull || (nparts > 1 && sift_star_row_part(sh, row, nparts) != part))
		return false;
	for (int c = 0; c < xd->npay; c++)
	{
		bool		isnull;
		const Datum v = slot_getattr(slot, xd->pcol[c], &isnull);

		put_cell(sh->pcls[c], xd->ptype[c], v, isnull, &row->pay_i[c], &row->pay_p[c], &row->pay_len[c], &row->pay_null[c]);
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

/* Reads the plan of the dimension into its table: the rows of the part of its keys; false when they do not fit */
static bool
read_dim(SiftV6StarRun *r, int d, uint32 nparts, uint32 part)
{
	SiftStarShape sh;
	SiftStarDim *dim = &r->dim[d];
	TupleTableSlot *slot;
	bool		fits = true;

	shape_of(&r->x->dim[d], r->budget, &sh);
	if (r->read[d])
	{
		MemoryContext old = MemoryContextSwitchTo(r->qcx);

		ExecReScan(r->ps[d]);
		MemoryContextSwitchTo(old);
	}
	r->read[d] = true;
	sift_star_dim_init(dim, &sh);
	while (fits && (slot = next_row(r, d)) != NULL && !TupIsNull(slot))
	{
		SiftStarRow row = {0};
		MemoryContext old;
		bool		keep;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(r->rowcx);
		old = MemoryContextSwitchTo(r->rowcx);
		keep = take_row(r, d, &sh, slot, nparts, part, &row);
		MemoryContextSwitchTo(old);
		if (keep)
			fits = sift_star_dim_add(dim, &row);
	}
	if (fits)
		sift_star_dim_finish(dim);
	return fits;
}

static void
free_dim(SiftV6StarRun *r, int d)
{
	if (r->have[d] && !r->kept[d])
		sift_star_dim_free(&r->dim[d]);
	r->have[d] = r->kept[d] = false;
}

/* The table of dimension d as a statement before it kept it, when the table it was read from is still the one */
static bool
find_kept(SiftV6StarRun *r, int d)
{
	SiftStarShape sh;

	shape_of(&r->x->dim[d], r->budget, &sh);
	r->dc[d] = sift_dimcache_try(r->ps[d], &r->x->dim[d], &sh, r->cx);
	if (r->dc[d] == NULL || !sift_dimcache_find(r->dc[d], r->budget, &r->dim[d]))
		return false;
	r->have[d] = r->kept[d] = true;
	r->hits++;
	return true;
}

/* Reads dimension d for the pass; in the first pass, with the classes of keys it needs to fit, or as the cache holds it when it need not be read */
static void
build_dim(SiftV6StarRun *r, int d, uint32 pass)
{
	MemoryContext old = MemoryContextSwitchTo(r->cx);
	bool		settled;

	free_dim(r, d);
	r->dc[d] = NULL;
	if (pass == 0 && r->nparts[d] == 1 && find_kept(r, d))
	{
		r->bytes = Max(r->bytes, r->dim[d].bytes);
		MemoryContextSwitchTo(old);
		return;
	}
	do
	{
		const bool	keep = pass == 0 && r->nparts[d] == 1 && r->dc[d] != NULL;
		bool		fits;
		bool		last;

		if (keep)
			MemoryContextSwitchTo(sift_dimcache_read_cx(r->dc[d]));
		fits = read_dim(r, d, r->nparts[d], r->part[d]);
		MemoryContextSwitchTo(r->cx);
		last = pass > 0 || r->nparts[d] >= STAR_MAX_PARTS;
		settled = fits && (last || r->nparts[d] == 1 || r->dim[d].bytes <= r->budget / 2);
		if (keep && fits)
		{
			r->built++;
			r->kept[d] = sift_dimcache_keep(r->dc[d], &r->dim[d]);
		}
		if (settled)
			continue;
		sift_star_dim_free(&r->dim[d]);
		if (!fits && last)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a dimension of the star join does not fit work_mem in any part of its keys"),
							errhint("Increase work_mem.")));
		r->nparts[d] *= 2;
		r->part[d] = 0;
	} while (!settled);
	r->have[d] = true;
	r->bytes = Max(r->bytes, r->dim[d].bytes);
	MemoryContextSwitchTo(old);
}

SiftStarOpen
sift_v6_star_open(SiftV6StarRun *r, uint32 pass)
{
	uint32		stride = 1;

	if (pass >= r->passes)
		return SSO_DONE;
	for (int d = 0; d < r->x->ndims; d++)
	{
		r->part[d] = (pass / stride) % r->nparts[d];
		if (pass == 0 || r->nparts[d] > 1)
			build_dim(r, d, pass);
		stride *= r->nparts[d];
	}
	if (pass == 0)
		r->passes = stride;
	for (int d = 0; d < r->x->ndims; d++)
		if (r->x->dim[d].mode == SV6J_INNER_EQUI && r->dim[d].nrows == 0)
			return SSO_EMPTY;
	return SSO_SCAN;
}

void
sift_v6_star_attach(const SiftV6StarRun *r, SiftV6Pipe *p)
{
	for (int d = 0; d < r->x->ndims; d++)
	{
		p->dim[d] = &r->dim[d];
		p->nparts[d] = r->nparts[d];
		p->part[d] = r->part[d];
	}
}

uint32
sift_v6_star_passes(const SiftV6StarRun *r)
{
	return r->passes;
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

void
sift_v6_star_close(SiftV6StarRun *r)
{
	for (int d = 0; d < r->x->ndims; d++)
	{
		free_dim(r, d);
		r->dc[d] = NULL;
	}
	MemoryContextReset(r->cx);
}
