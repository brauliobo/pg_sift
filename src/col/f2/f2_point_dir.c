/*
 * col/f2/f2_point_dir.c - SEC_POINT, the directory from a value of an integer column to the rows that hold it (col/f2/f2_point_dir.h).
 *
 * check-memory: allow-allocs the tables follow the rows of the part being written, which the writer holds in memory already, and the candidates of a scan follow its few values
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/f2_pred.h"
#include "col/f2/f2_point_dir.h"
#include "core/damage.h"

static int
int64_cmp(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

static int
uint32_cmp(const void *a, const void *b)
{
	const uint32 x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/* A plain integer column that is not a time (a range, not a point, is asked of it), none of whose rows holds a value it cannot (a tag: such a row is null here and is left to the heap, so the directory could not name it) */
static bool
point_eligible(const SiftRows *rows, int col)
{
	const SiftColIn *c = &rows->cols[col];
	const int	n = rows->nrows;

	if (col + 1 == rows->time_col || col + 1 == rows->time_cols[0] || col + 1 == rows->time_cols[1])
		return false;
	if (c->kind != SIFT_CELL_INT || c->hint != SIFT_CHINT_AUTO || c->key_leaf || c->group1 != 0 || c->eoff != NULL || c->promo != NULL)
		return false;
	for (int r = 0; c->tag != NULL && r < n; r++)
		if (c->tag[r] != 0)
			return false;
	return true;
}

/* The distinct values among SIFT_POINT_SAMPLE rows spread over the part: the column is worth a directory when nearly a quarter of them are different */
static int
sample_distinct(const SiftColIn *c, int n, int *seen)
{
	int64		vals[SIFT_POINT_SAMPLE];
	int			m = 0,
				distinct = 0;

	for (int i = 0; i < SIFT_POINT_SAMPLE; i++)
	{
		const int	r = (int) ((int64) i * n / SIFT_POINT_SAMPLE);

		if (c->isnull == NULL || !c->isnull[r])
			vals[m++] = c->i[r];
	}
	qsort(vals, m, sizeof(int64), int64_cmp);
	for (int i = 0; i < m; i++)
		distinct += i == 0 || vals[i] != vals[i - 1];
	*seen = m;
	return distinct;
}

static void
put_column(const SiftColIn *c, const int *gstart, int ng, int nnz, uint32 log2_buckets, StringInfo out, SiftPointCol *col)
{
	const uint32 nb = UINT32_C(1) << log2_buckets;
	uint32	   *start = palloc0((nb + 1) * sizeof(uint32)),
			   *ref = palloc(Max(nnz, 1) * sizeof(uint32));

	for (int g = 0; g < ng; g++)
		for (int r = 0; r < gstart[g + 1] - gstart[g]; r++)
			if (c->isnull == NULL || !c->isnull[gstart[g] + r])
				start[sift_point_bucket(c->i[gstart[g] + r], log2_buckets) + 1]++;
	for (uint32 b = 0; b < nb; b++)
		start[b + 1] += start[b];
	{
		uint32	   *at = palloc(nb * sizeof(uint32));

		memcpy(at, start, nb * sizeof(uint32));
		for (int g = 0; g < ng; g++)
			for (int r = 0; r < gstart[g + 1] - gstart[g]; r++)
				if (c->isnull == NULL || !c->isnull[gstart[g] + r])
					ref[at[sift_point_bucket(c->i[gstart[g] + r], log2_buckets)]++] = ((uint32) g << SIFT_POINT_ROW_BITS) | r;
		pfree(at);
	}
	while (out->len % 8 != 0)
		appendStringInfoChar(out, '\0');
	col->off_buckets = out->len;
	appendBinaryStringInfo(out, (char *) start, (nb + 1) * sizeof(uint32));
	while (out->len % 8 != 0)
		appendStringInfoChar(out, '\0');
	col->off_entries = out->len;
	appendBinaryStringInfo(out, (char *) ref, nnz * sizeof(uint32));
	col->log2_buckets = log2_buckets;
	col->nentries = nnz;
	pfree(start);
	pfree(ref);
}

/* The columns by their distinct values, most first, at most SIFT_POINT_MAX_COLS of them */
static void
rank_insert(int *pick, int *score, int *npick, int c, int distinct)
{
	int			k = Min(*npick, SIFT_POINT_MAX_COLS - 1);

	if (*npick == SIFT_POINT_MAX_COLS && distinct <= score[k])
		return;
	for (; k > 0 && score[k - 1] < distinct; k--)
	{
		pick[k] = pick[k - 1];
		score[k] = score[k - 1];
	}
	pick[k] = c;
	score[k] = distinct;
	*npick = Min(*npick + 1, SIFT_POINT_MAX_COLS);
}

/* The buckets of a column of nnz values, and the bytes of its table and entries */
static uint32
column_buckets(int nnz)
{
	uint32		log2_buckets = 4;

	while ((UINT64CONST(1) << log2_buckets) * SIFT_POINT_LOAD < (uint64) nnz)
		log2_buckets++;
	return log2_buckets;
}

static int
column_nnz(const SiftColIn *c, int nrows)
{
	int			nnz = nrows;

	for (int r = 0; c->isnull != NULL && r < nrows; r++)
		nnz -= c->isnull[r];
	return nnz;
}

/* STOR[STOR-COL-07]: SEC_POINT raw uint32 hash directory (~8 B/row capped by SIFT_POINT_MAX_PCT) */
static uint64
column_bytes(int nnz)
{
	return ((UINT64CONST(1) << column_buckets(nnz)) + 1) * sizeof(uint32) + (uint64) nnz * sizeof(uint32) + 2 * sizeof(uint64);
}

bool
sift_point_dir_build(const SiftRows *rows, const int *gstart, int ng, uint64 part_bytes, StringInfo out)
{
	int			pick[SIFT_POINT_MAX_COLS],
				score[SIFT_POINT_MAX_COLS],
				npick = 0,
				nkeep = 0;
	uint64		spent = sizeof(SiftPointHdr);
	SiftPointHdr hdr = {.magic = SIFT_POINT_MAGIC,.version = SIFT_POINT_VERSION};
	SiftPointCol cols[SIFT_POINT_MAX_COLS] = {0};

	if (rows->nsort != 0 || rows->nrows < SIFT_POINT_MIN_ROWS)
		return false;
	for (int c = 0; c < rows->ncols; c++)
	{
		int			seen,
					distinct;

		if (!point_eligible(rows, c))
			continue;
		distinct = sample_distinct(&rows->cols[c], rows->nrows, &seen);
		if (seen < SIFT_POINT_SAMPLE / 2 || distinct < seen / 4)
			continue;
		rank_insert(pick, score, &npick, c, distinct);
	}
	for (int k = 0; k < npick; k++)
	{
		const uint64 cost = sizeof(SiftPointCol) + column_bytes(column_nnz(&rows->cols[pick[k]], rows->nrows));

		if ((spent + cost) * 100 <= part_bytes * SIFT_POINT_MAX_PCT)
		{
			pick[nkeep++] = pick[k];
			spent += cost;
		}
	}
	if (nkeep == 0)
		return false;
	hdr.ncols = nkeep;
	appendBinaryStringInfo(out, (char *) &hdr, sizeof(hdr));
	appendStringInfoSpaces(out, nkeep * sizeof(SiftPointCol));
	for (int k = 0; k < nkeep; k++)
	{
		const SiftColIn *c = &rows->cols[pick[k]];
		const int	nnz = column_nnz(c, rows->nrows);

		cols[k].leaf = pick[k];
		put_column(c, gstart, ng, nnz, column_buckets(nnz), out, &cols[k]);
	}
	memcpy(out->data + sizeof(hdr), cols, nkeep * sizeof(SiftPointCol));
	return true;
}

SiftPointDir *
sift_point_dir_open(Relation rel, const SiftPart *part, MemoryContext cx)
{
	const SiftSecDirEntry *se = sift_part_section(part, SEC_POINT);
	SiftPointDir *pd;
	uint8		head[sizeof(SiftPointHdr) + SIFT_POINT_MAX_COLS * sizeof(SiftPointCol)];

	if (se == NULL)
		return NULL;
	pd = MemoryContextAllocZero(cx, sizeof(SiftPointDir));
	pd->sec = *se;
	if (se->len < sizeof(SiftPointHdr))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory is shorter than its header"));
	sift_part_read_section_bytes(rel, part, se, 0, Min(se->len, sizeof(head)), head);
	memcpy(&pd->hdr, head, sizeof(pd->hdr));
	if (pd->hdr.magic != SIFT_POINT_MAGIC || pd->hdr.version != SIFT_POINT_VERSION || pd->hdr.ncols == 0 || pd->hdr.ncols > SIFT_POINT_MAX_COLS ||
		se->len < sizeof(SiftPointHdr) + pd->hdr.ncols * sizeof(SiftPointCol))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory has a header that is not one"));
	memcpy(pd->cols, head + sizeof(SiftPointHdr), pd->hdr.ncols * sizeof(SiftPointCol));
	for (int k = 0; k < pd->hdr.ncols; k++)
	{
		const SiftPointCol *c = &pd->cols[k];

		if (c->log2_buckets > 31 || (uint64) c->off_buckets + ((UINT64CONST(1) << c->log2_buckets) + 1) * 4 > se->len || (uint64) c->off_entries + (uint64) c->nentries * 4 > se->len)
			SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory has a column past its end"));
	}
	return pd;
}

static const SiftPointCol *
point_col(const SiftPointDir *pd, int leaf)
{
	for (int k = 0; k < pd->hdr.ncols; k++)
		if (pd->cols[k].leaf == (uint32) leaf)
			return &pd->cols[k];
	return NULL;
}

/* A predicate the directory can answer: an equality or a short list on an integer column of one lane that it covers, whose constants the stored values can be compared with */
static const SiftColPred *
point_pred(const SiftPointDir *pd, const SiftColSchema *schema, const SiftColPred *preds, int npreds)
{
	for (int k = 0; k < npreds; k++)
	{
		const SiftColPred *p = &preds[k];
		const SiftColDef *d = &schema->cols[p->col];

		if ((p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_IN) && p->nvals <= SIFT_POINT_MAX_VALUES && d->store == SIFT_COLS_INT && d->lane == 0 && d->parent < 0 &&
			!sift_col_def_is_wide(d) && !sift_f2_undecided(d, p) && point_col(pd, p->col) != NULL)
			return p;
	}
	return NULL;
}

bool
sift_point_dir_cands(Relation rel, const SiftPart *part, const SiftPointDir *pd, const SiftColSchema *schema, const SiftColPred *preds, int npreds, MemoryContext cx, SiftPointCands *pc)
{
	const SiftColPred *p = point_pred(pd, schema, preds, npreds);
	const SiftPointCol *col;
	MemoryContext old;
	uint32		cap = 0;

	if (p == NULL)
		return false;
	col = point_col(pd, p->col);
	old = MemoryContextSwitchTo(cx);
	pc->n = 0;
	pc->ref = NULL;
	pc->pred = p;
	pc->pred_idx = (int) (p - preds);
	for (int j = 0; j < p->nvals; j++)
	{
		uint32		range[2],
					cnt;
		const uint32 b = sift_point_bucket(p->vals[j].v.i, col->log2_buckets);

		if (p->vals[j].isnull)
			continue;
		sift_part_read_section_bytes(rel, part, &pd->sec, col->off_buckets + (uint64) b * 4, sizeof(range), range);
		if (range[0] > range[1] || range[1] > col->nentries)
			SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory bucket is past its entries"));
		cnt = range[1] - range[0];
		if (pc->n + cnt > cap)
		{
			cap = Max(cap * 2, pc->n + cnt);
			pc->ref = pc->ref != NULL ? repalloc(pc->ref, cap * sizeof(uint32)) : palloc(cap * sizeof(uint32));
		}
		if (cnt > 0)
			sift_part_read_section_bytes(rel, part, &pd->sec, col->off_entries + (uint64) range[0] * 4, (uint64) cnt * 4, pc->ref + pc->n);
		pc->n += cnt;
	}
	MemoryContextSwitchTo(old);
	qsort(pc->ref, pc->n, sizeof(uint32), uint32_cmp);
	{
		uint32		w = 0;

		for (uint32 i = 0; i < pc->n; i++)
			if (i == 0 || pc->ref[i] != pc->ref[i - 1])
				pc->ref[w++] = pc->ref[i];
		pc->n = w;
	}
	return true;
}

/* The index of the first candidate in a granule at or after g */
static uint32
first_at(const SiftPointCands *pc, uint32 g)
{
	uint32		lo = 0,
				hi = pc->n;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if ((pc->ref[mid] >> SIFT_POINT_ROW_BITS) < g)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

int
sift_point_cands_rows(const SiftPointCands *pc, uint32 g, uint16 *rows, int cap)
{
	uint32		i = first_at(pc, g);
	int			n = 0;

	for (; i < pc->n && (pc->ref[i] >> SIFT_POINT_ROW_BITS) == g; i++)
	{
		if (n == cap)
			return -1;
		rows[n++] = pc->ref[i] & ((1 << SIFT_POINT_ROW_BITS) - 1);
	}
	return n;
}

uint32
sift_point_cands_next(const SiftPointCands *pc, uint32 g)
{
	const uint32 i = first_at(pc, g);

	return i < pc->n ? pc->ref[i] >> SIFT_POINT_ROW_BITS : UINT32_MAX;
}

int
sift_point_cands_verify(const SiftPointCands *pc, const SiftGranuleView *v, uint16 *rows, int n)
{
	int64		vals[SIFT_POINT_ROWS_MAX];
	bool		isnull[SIFT_POINT_ROWS_MAX];
	int			kept = 0;

	sift_granule_point_ints(v, pc->pred->col, rows, n, vals, isnull);
	for (int j = 0; j < n; j++)
	{
		bool		hit = false;

		for (int m = 0; m < pc->pred->nvals && !hit && !isnull[j]; m++)
			hit = !pc->pred->vals[m].isnull && pc->pred->vals[m].v.i == vals[j];
		if (hit)
			rows[kept++] = rows[j];
	}
	return kept;
}
