/*
 * col/advice.c - sift.index_advice(index): the profile of the columns of a sift_col index and what its options could still use (docs/USAGE.md). Advice
 * only: nothing is changed, and nothing is decided here that CREATE INDEX does not decide by the same rule. The units looked at are SAMPLE evenly spaced
 * ones over the immutable segments, so the answer is the same for the same index; a rule holds where AGREE_PCT of them satisfy it.
 *
 *   dimension  at most DIM_GROUPS distinct values (NULL counts as one) in AGREE_PCT of the sampled units: a column the cube holds a histogram of
 *   pair       two dimensions whose combinations fit a rollup of the rows of the unit (min(rows / 10, SIFT_PAGG_ROWS_CELLS) cells) in AGREE_PCT of the
 *              units and whose cells take at most GROWTH_PCT of the index: rollup_dims would answer a GROUP BY of both from cells
 *   key        an int2, int4 or date foreign key or column named *_id that is not unique: key_columns keeps a key list or bitmap of it (col/keycols.c)
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/enc.h"
#include "col/pagg.h"
#include "col/pg.h"
#include "col/seg.h"

#define SAMPLE			32		/* units looked at */
#define AGREE_PCT		90		/* the share of them that must satisfy a rule */
#define DIM_GROUPS		256		/* the groups of a histogram a unit may hold for the cube to keep it (col/preagg.h: max_groups) */
#define GROWTH_PCT		3		/* the growth of the index a structure may cost (the rule of every default of 3.3) */
#define PAIR_DIMS		6		/* dimensions whose pairs are looked at, in column order */
#define CELL_BYTES		24		/* a cell of a rollup of two dimensions: two keys and a count, as the sidecar stores them at most */

typedef struct Leaf
{
	bool		cand;			/* an INT or dictionary STR top level scalar: its distinct values per unit are counted */
	bool		dim;			/* a dimension */
	uint32		seen;
	uint32		small;
} Leaf;

typedef struct Pair
{
	int			a,
				b;				/* positions in dims[] */
	uint32		seen,
				fit;
	double		cells;			/* of the sampled units that fit, together */
} Pair;

static int
cmp_i64(const void *x, const void *y)
{
	int64		a = *(const int64 *) x,
				b = *(const int64 *) y;

	return a < b ? -1 : a > b;
}

typedef struct Pt
{
	int64		a,
				b;
} Pt;

static int
cmp_pt(const void *x, const void *y)
{
	const Pt   *p = x,
			   *q = y;

	return p->a != q->a ? (p->a < q->a ? -1 : 1) : cmp_i64(&p->b, &q->b);
}

/* the values of leaf l of a unit (a dictionary STR as its codes) in vals[0..n), isnull[i] for a NULL slot; false when the leaf has none to count (inline text) */
static bool
unit_values(SiftColSegment *seg, const SiftCUnitView *v, int l, int64 *vals, bool *isnull)
{
	SiftCChunk	ch;
	const SiftColDef *d = &seg->schema->cols[l];

	if (!sift_cunit_chunk(v, l, &ch))
	{
		memset(vals, 0, v->nrows * sizeof(int64));
		memset(isnull, 1, v->nrows);
		return true;
	}
	if (d->store == SIFT_COLS_STR)
	{
		SiftCDict  *dict = ch.enc == SIFT_COLENC_ISTR ? NULL : sift_cseg_dict(seg, l);
		uint32	   *codes;

		if (dict == NULL)
			return false;
		codes = palloc((v->nrows + SIFT_COL_PAD) * sizeof(uint32));
		sift_cchunk_decode_codes(&ch, dict->n, codes);
		for (uint32 i = 0; i < v->nrows; i++)
			vals[i] = codes[i];
		pfree(codes);
	}
	else
	{
		int64	   *tmp = palloc((v->nrows + SIFT_COL_PAD) * sizeof(int64));

		sift_cchunk_decode(&ch, tmp);
		memcpy(vals, tmp, v->nrows * sizeof(int64));
		pfree(tmp);
	}
	for (uint32 i = 0; i < v->nrows; i++)
		isnull[i] = ch.nulls != NULL && ((ch.nulls[i / 64] >> (i % 64)) & 1);
	return true;
}

/* distinct values of the slots of vals, NULL as one more */
static uint32
distinct(int64 *vals, const bool *isnull, uint32 n, int64 *scratch)
{
	uint32		m = 0,
				k = 0;
	bool		nulls = false;

	for (uint32 i = 0; i < n; i++)
	{
		if (isnull[i])
			nulls = true;
		else
			scratch[m++] = vals[i];
	}
	qsort(scratch, m, sizeof(int64), cmp_i64);
	for (uint32 i = 0; i < m; i++)
		k += i == 0 || scratch[i] != scratch[i - 1];
	return k + nulls;
}

static uint32
distinct_pairs(const int64 *va, const bool *na, const int64 *vb, const bool *nb, uint32 n, Pt *scratch)
{
	uint32		k = 0;

	for (uint32 i = 0; i < n; i++)
		scratch[i] = (Pt) {na[i] ? PG_INT64_MIN : va[i], nb[i] ? PG_INT64_MIN : vb[i]};
	qsort(scratch, n, sizeof(Pt), cmp_pt);
	for (uint32 i = 0; i < n; i++)
		k += i == 0 || cmp_pt(&scratch[i], &scratch[i - 1]) != 0;
	return k;
}

static int
share(uint32 n, uint32 of)
{
	return of ? (int) ((n * UINT64CONST(100)) / of) : 0;
}

/* what the options of the index hold of leaf c */
static const char *
held_by(const SiftColRel *rel, int c, int histo_pct)
{
	const SiftCPaggSpec *spec = &rel->pagg;
	StringInfoData s;

	initStringInfo(&s);
	for (int d = 0; d < spec->ndims; d++)
		if (spec->dim[d].leaf == c && (spec->dim[d].cal || histo_pct > 0))
			appendStringInfo(&s, "%s%s", s.len ? ", " : "", spec->dim[d].cal ? "calendar cells" : psprintf("cube histogram in %d%% of the units", histo_pct));
	if (spec->pres[c])
		appendStringInfo(&s, "%skey list or bitmap", s.len ? ", " : "");
	else if (spec->set[c])
		appendStringInfo(&s, "%skey set", s.len ? ", " : "");
	for (int r = 0; r < spec->nrollups; r++)
		for (int i = 0; i < spec->rollup[r].ndims; i++)
			if (spec->rollup[r].list < 0 && spec->rollup[r].dim[i] == c)
				appendStringInfo(&s, "%srollup", s.len ? ", " : "");
	return s.len ? s.data : NULL;
}

static bool
is_date(const SiftColDef *d)
{
	return d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ;
}

static bool
has_rollup(const SiftCPaggSpec *spec, int a, int b)
{
	for (int r = 0; r < spec->nrollups; r++)
	{
		bool		ha = false,
					hb = false;

		for (int i = 0; spec->rollup[r].list < 0 && i < spec->rollup[r].ndims; i++)
		{
			ha |= spec->rollup[r].dim[i] == a;
			hb |= spec->rollup[r].dim[i] == b;
		}
		if (ha && hb)
			return true;
	}
	return false;
}

static const char *
leaf_label(Relation index, const SiftColDef *d)
{
	return d->path[0] ? d->path : NameStr(TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attname);
}

Datum
sift_col_index_advice(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Relation	heap;
	const SiftColRel *rel;
	const SiftColSchema *sc;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col index advice", ALLOCSET_DEFAULT_SIZES);
	Leaf	   *leaf;
	Pair		pairs[PAIR_DIMS * (PAIR_DIMS - 1) / 2];
	int			dims[PAIR_DIMS],
				ndim = 0,
				npair = 0,
				nplain = 0;		/* the columns a cube could take: more than its cap, and preagg = 'auto' picks the ones that fit */
	uint64		units = 0,
				bytes = 0;
	int64	   *vals,
			   *scratch;
	bool	   *nulls;
	uint32		histo[SIFT_COL_MAX_COLS] = {0};

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	InitMaterializedSRF(fcinfo, 0);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	sc = rel->schema;
	leaf = palloc0(sc->ncols * sizeof(Leaf));
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *d = &sc->cols[c];

		/* a date has its calendar cells, which the profile leaves to calendar_columns */
		leaf[c].cand = d->parent < 0 && d->kind == SIFT_COLK_SCALAR && d->lanes <= 1 && !sift_col_def_is_image(d) && !is_date(d) &&
			(d->store == SIFT_COLS_INT || (d->store == SIFT_COLS_STR && d->type == SIFT_COLT_TEXT));
	}
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], cx);

			SiftCPaggCensus census;

			sift_cseg_pre_census(seg, &rel->pagg, sift_col_format_minor, 0, &census);
			for (int d = 0; d < rel->pagg.ndims; d++)
				if (!rel->pagg.dim[d].cal)
					histo[rel->pagg.dim[d].leaf] += census.dim[d];
			units += seg->nsb;
			bytes += snap->segs[i].bytes;
			sift_cseg_close(seg);
		}
	/* pass 1 counts the distinct values of every leaf per sampled unit, pass 2 the combinations of the dimensions it found */
	for (int pass = 0; pass < 2 && units > 0; pass++)
	{
		uint64		base = 0;
		uint32		k = 0,
					nk = Min(SAMPLE, units);
		int64	   *v2[PAIR_DIMS];
		bool	   *n2[PAIR_DIMS],
					ok[PAIR_DIMS];

		for (int i = 0; i < snap->nsegs && k < nk; i++)
		{
			SiftColSegment *seg;

			if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
				continue;
			seg = sift_cseg_open(index, &snap->segs[i], cx);
			for (; k < nk; k++)
			{
				uint64		g = (k * units + units / 2) / nk;
				uint32		len;
				const uint8 *rec;
				SiftCUnitView uv;

				if (g >= base + seg->nsb)
					break;
				rec = sift_cseg_unit(seg, (uint32) (g - base), &len);
				if (!sift_cunit_parse(rec, len, sc, &uv))
					ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("index \"%s\" has a unit that does not parse", RelationGetRelationName(index))));
				vals = palloc((uv.nrows + SIFT_COL_PAD) * sizeof(int64));
				scratch = palloc((uv.nrows + SIFT_COL_PAD) * sizeof(int64));
				nulls = palloc(uv.nrows + SIFT_COL_PAD);
				if (pass == 0)
				{
					for (int c = 0; c < sc->ncols; c++)
						if (leaf[c].cand && unit_values(seg, &uv, c, vals, nulls))
						{
							leaf[c].seen++;
							leaf[c].small += distinct(vals, nulls, uv.nrows, scratch) <= DIM_GROUPS;
						}
				}
				else
				{
					uint32		cap = Min(uv.nrows / 10, SIFT_PAGG_ROWS_CELLS);
					Pt		   *pts = palloc((uv.nrows + SIFT_COL_PAD) * sizeof(Pt));

					for (int a = 0; a < ndim; a++)
					{
						v2[a] = palloc((uv.nrows + SIFT_COL_PAD) * sizeof(int64));
						n2[a] = palloc(uv.nrows + SIFT_COL_PAD);
						ok[a] = unit_values(seg, &uv, dims[a], v2[a], n2[a]);
					}
					for (int p = 0; p < npair; p++)
					{
						uint32		cells;

						if (!ok[pairs[p].a] || !ok[pairs[p].b])
							continue;
						cells = distinct_pairs(v2[pairs[p].a], n2[pairs[p].a], v2[pairs[p].b], n2[pairs[p].b], uv.nrows, pts);
						pairs[p].seen++;
						if (cells <= cap)
						{
							pairs[p].fit++;
							pairs[p].cells += cells;
						}
					}
				}
				pfree(vals);
				pfree(scratch);
				pfree(nulls);
			}
			base += seg->nsb;
			sift_cseg_close(seg);
			MemoryContextReset(cx);
		}
		if (pass == 0)
		{
			for (int c = 0; c < sc->ncols; c++)
				leaf[c].dim = leaf[c].seen > 0 && leaf[c].small * 100 >= (uint64) leaf[c].seen * AGREE_PCT;
			for (int c = 0; c < sc->ncols && ndim < PAIR_DIMS; c++)
				if (leaf[c].dim && sc->cols[c].path[0] == 0)
					dims[ndim++] = c;
			for (int a = 0; a < ndim; a++)
				for (int b = a + 1; b < ndim; b++)
					if (!has_rollup(&rel->pagg, dims[a], dims[b]))
						pairs[npair++] = (Pair) {.a = a, .b = b};
		}
	}
	for (int c = 0; c < sc->ncols; c++)
		nplain += sc->cols[c].parent < 0 && sift_cpagg_dim_ok(sc, &sc->cols[c]);
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *d = &sc->cols[c];
		int			cube_pct = share(histo[c], (uint32) units);
		const char *held = held_by(rel, c, cube_pct),
				   *kind;
		StringInfoData advice;
		Datum		v[6];
		bool		isnull[6] = {false};

		if (d->parent >= 0)
			continue;
		initStringInfo(&advice);
		if (d->kind != SIFT_COLK_SCALAR)
			kind = "list";
		else if (is_date(d))
			kind = "date";
		else if (sift_col_leaf_unique(heap, index, d))
			kind = "unique";
		else if (sift_col_key_shaped(heap, index, sc, d))
			kind = "key";
		else if (leaf[c].cand && leaf[c].seen == 0)
			kind = "unknown";
		else if (leaf[c].dim)
			kind = "dimension";
		else
			kind = d->store == SIFT_COLS_STR ? "text" : "measure";
		if (leaf[c].dim && cube_pct < AGREE_PCT)
			appendStringInfo(&advice, "add it to preagg = 'cube(...)'%s: %u of %u sampled units hold at most %d values of it",
							 nplain > SIFT_PAGG_MAX_DIMS ? " (or ask preagg = 'auto' for the columns that fit)" : "", leaf[c].small, leaf[c].seen, DIM_GROUPS);
		else if (strcmp(kind, "key") == 0 && !rel->pagg.pres[c] && cube_pct < SIFT_COL_AUTO_KEEP_PCT)
			appendStringInfo(&advice, "key_columns = 'auto' keeps a key list or bitmap of it for count(DISTINCT)");
		v[0] = CStringGetTextDatum(leaf_label(index, d));
		v[1] = CStringGetTextDatum(kind);
		v[2] = held ? CStringGetTextDatum(held) : (Datum) 0;
		isnull[2] = held == NULL;
		v[3] = Int32GetDatum(leaf[c].seen);
		v[4] = leaf[c].seen ? Int32GetDatum(share(leaf[c].small, leaf[c].seen)) : (Datum) 0;
		isnull[4] = leaf[c].seen == 0;
		v[5] = advice.len ? CStringGetTextDatum(advice.data) : (Datum) 0;
		isnull[5] = advice.len == 0;
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, isnull);
	}
	for (int p = 0; p < npair; p++)
	{
		Datum		v[6];
		bool		isnull[6] = {false};
		double		growth = pairs[p].seen && units ? pairs[p].cells / Max(pairs[p].fit, 1) * (double) units * CELL_BYTES * 100.0 / Max((double) bytes, 1.0) : 0.0;
		bool		pass = pairs[p].seen > 0 && pairs[p].fit * 100 >= (uint64) pairs[p].seen * AGREE_PCT && growth <= GROWTH_PCT;
		const char *na = leaf_label(index, &sc->cols[dims[pairs[p].a]]),
				   *nb = leaf_label(index, &sc->cols[dims[pairs[p].b]]);

		v[0] = CStringGetTextDatum(psprintf("%s, %s", na, nb));
		v[1] = CStringGetTextDatum("pair");
		isnull[2] = true;
		v[3] = Int32GetDatum(pairs[p].seen);
		v[4] = Int32GetDatum(share(pairs[p].fit, pairs[p].seen));
		if (pass)
			v[5] = CStringGetTextDatum(psprintf("rollup_dims = '%s, %s: <measure>' would answer %d%% of the units from cells, within %d%% more index (rollup_dims = 'auto' writes the cheapest pair)", na, nb,
												share(pairs[p].fit, pairs[p].seen), GROWTH_PCT));
		else
			isnull[5] = true;
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, isnull);
	}
	MemoryContextDelete(cx);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
