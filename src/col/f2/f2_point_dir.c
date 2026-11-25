/*
 * col/f2/f2_point_dir.c - SEC_POINT, the directory from a key (a value of one column, or of two to four together) to the rows that hold it (col/f2/f2_point_dir.h).
 *
 * check-memory: allow-allocs the tables follow the rows of the part being written, which the writer holds in memory already, and the candidates of a scan follow its few values
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "col/f2/f2_pred.h"
#include "col/f2/f2_point_dir.h"
#include "core/damage.h"

#define POINT_POOL		6		/* columns that are not keys by themselves and are tried together */

/* The columns of a key of the part being written, and how many of the sampled rows hold distinct values of it */
typedef struct PointKey
{
	int			nleaves;
	int			leaf[SIFT_POINT_MAX_COMP];
	int			distinct;
} PointKey;

static int
uint64_cmp(const void *a, const void *b)
{
	const uint64 x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static int
uint32_cmp(const void *a, const void *b)
{
	const uint32 x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/* A column of integers, uuids or text that is not the time of the part, nor the lead of its key (SEC_KEY_MARKS answers it), none of whose rows holds a value it cannot (a tag: such a row is null here and is left to the heap, so the directory could not name it) */
static bool
point_eligible(const SiftRows *rows, int col)
{
	const SiftColIn *c = &rows->cols[col];
	const int	n = rows->nrows;

	if (col + 1 == rows->time_col || col + 1 == rows->time_cols[0] || col + 1 == rows->time_cols[1] || (rows->nsort != 0 && col == rows->sort_cols[0]))
		return false;
	if (c->key_leaf || c->group1 != 0 || c->eoff != NULL || c->promo != NULL || c->astext)
		return false;
	if (c->kind == SIFT_CELL_INT && c->hint != SIFT_CHINT_AUTO)
		return false;
	if (c->kind == SIFT_CELL_STR && ((c->hint != SIFT_CHINT_AUTO && c->hint != SIFT_CHINT_UUID) || c->s == NULL || c->slen == NULL))
		return false;
	if (c->kind != SIFT_CELL_INT && c->kind != SIFT_CELL_STR)
		return false;
	for (int r = 0; c->tag != NULL && r < n; r++)
		if (c->tag[r] != 0)
			return false;
	return true;
}

static SiftPointKeyKind
key_kind(const SiftRows *rows, const PointKey *k)
{
	const SiftColIn *c = &rows->cols[k->leaf[0]];

	return k->nleaves > 1 ? SIFT_PK_COMPOSITE : c->kind == SIFT_CELL_INT ? SIFT_PK_INT64 : c->hint == SIFT_CHINT_UUID ? SIFT_PK_UUID16 : SIFT_PK_TEXT;
}

/* The hash of the key of a row, false when a column of it is null there */
static bool
row_hash(const SiftRows *rows, const PointKey *k, int row, uint64 *hash)
{
	uint64		h = 0;

	for (int i = 0; i < k->nleaves; i++)
	{
		const SiftColIn *c = &rows->cols[k->leaf[i]];
		uint64		x;

		if (c->isnull != NULL && c->isnull[row])
			return false;
		x = c->kind == SIFT_CELL_INT ? sift_point_hash_i64(c->i[row]) : sift_point_hash_bytes(c->s[row], c->slen[row]);
		h = i == 0 ? x : sift_point_hash_combine(h, x);
	}
	*hash = h;
	return true;
}

/* The distinct keys among SIFT_POINT_SAMPLE rows spread over the part: a key is worth a directory when nearly a quarter of them are different */
static int
sample_distinct(const SiftRows *rows, const PointKey *k, int *seen)
{
	uint64		hashes[SIFT_POINT_SAMPLE];
	int			m = 0,
				distinct = 0;

	for (int i = 0; i < SIFT_POINT_SAMPLE; i++)
		if (row_hash(rows, k, (int) ((int64) i * rows->nrows / SIFT_POINT_SAMPLE), &hashes[m]))
			m++;
	qsort(hashes, m, sizeof(uint64), uint64_cmp);
	for (int i = 0; i < m; i++)
		distinct += i == 0 || hashes[i] != hashes[i - 1];
	*seen = m;
	return distinct;
}

static int
key_nnz(const SiftRows *rows, const PointKey *k)
{
	int			nnz = 0;
	uint64		h;

	for (int r = 0; r < rows->nrows; r++)
		nnz += row_hash(rows, k, r, &h);
	return nnz;
}

/* The buckets of a table of nnz values */
static uint32
column_buckets(int nnz)
{
	uint32		log2_buckets = 4;

	while ((UINT64CONST(1) << log2_buckets) * SIFT_POINT_LOAD < (uint64) nnz)
		log2_buckets++;
	return log2_buckets;
}

/* STOR[STOR-COL-07]: SEC_POINT raw uint32 hash directory (~8 B/row capped by SIFT_POINT_MAX_PCT) */
static uint64
column_bytes(int nnz)
{
	return ((UINT64CONST(1) << column_buckets(nnz)) + 1) * sizeof(uint32) + (uint64) nnz * sizeof(uint32) + 2 * sizeof(uint64);
}

/* Whether the rows of a bucket (ascending refs) hold no two equal keys: equal hashes are taken for equal keys, so a bucket of more than 64 is not told apart */
static bool
bucket_unique(const SiftRows *rows, const PointKey *k, const int *gstart, const uint32 *ref, uint32 cnt)
{
	uint64		hashes[64];

	if (cnt > lengthof(hashes))
		return false;
	for (uint32 i = 0; i < cnt; i++)
		(void) row_hash(rows, k, gstart[ref[i] >> SIFT_POINT_ROW_BITS] + (int) (ref[i] & ((1 << SIFT_POINT_ROW_BITS) - 1)), &hashes[i]);
	qsort(hashes, cnt, sizeof(uint64), uint64_cmp);
	for (uint32 i = 1; i < cnt; i++)
		if (hashes[i] == hashes[i - 1])
			return false;
	return true;
}

static void
put_column(const SiftRows *rows, const PointKey *k, const int *gstart, int ng, int nnz, StringInfo out, SiftPointCol *col)
{
	const uint32 log2_buckets = column_buckets(nnz),
				nb = UINT32_C(1) << log2_buckets;
	uint32	   *start = palloc0((nb + 1) * sizeof(uint32)),
			   *ref = palloc(Max(nnz, 1) * sizeof(uint32)),
			   *at = palloc(nb * sizeof(uint32));
	bool		unique = true;
	uint64		h;

	for (int g = 0; g < ng; g++)
		for (int r = gstart[g]; r < gstart[g + 1]; r++)
			if (row_hash(rows, k, r, &h))
				start[sift_point_bucket(h, log2_buckets) + 1]++;
	for (uint32 b = 0; b < nb; b++)
		start[b + 1] += start[b];
	memcpy(at, start, nb * sizeof(uint32));
	for (int g = 0; g < ng; g++)
		for (int r = gstart[g]; r < gstart[g + 1]; r++)
			if (row_hash(rows, k, r, &h))
				ref[at[sift_point_bucket(h, log2_buckets)]++] = ((uint32) g << SIFT_POINT_ROW_BITS) | (uint32) (r - gstart[g]);
	for (uint32 b = 0; b < nb && unique; b++)
		unique = start[b + 1] - start[b] < 2 || bucket_unique(rows, k, gstart, ref + start[b], start[b + 1] - start[b]);
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
	col->flags = unique ? SIFT_PCOL_UNIQUE : 0;
	pfree(start);
	pfree(ref);
	pfree(at);
}

/* The keys, most distinct values first, the keys of fewer columns before those of more */
static int
key_cmp(const void *a, const void *b)
{
	const PointKey *x = a,
			   *y = b;

	return x->distinct != y->distinct ? (x->distinct > y->distinct ? -1 : 1) : (x->nleaves > y->nleaves) - (x->nleaves < y->nleaves);
}

/* Whether the sample says a key is worth a directory */
static bool
key_qualifies(const SiftRows *rows, PointKey *k, int *distinct_out)
{
	int			seen;

	k->distinct = sample_distinct(rows, k, &seen);
	*distinct_out = seen;
	return seen >= SIFT_POINT_SAMPLE / 2 && k->distinct >= seen / 4;
}

/* A column that is no key alone, and how many of the sampled rows hold different values of it */
typedef struct PoolCol
{
	int			col;
	int			distinct;
} PoolCol;

static int
pool_cmp(const void *a, const void *b)
{
	const PoolCol *x = a,
			   *y = b;

	return (x->distinct < y->distinct) - (x->distinct > y->distinct);
}

/* The columns that are keys alone, and the keys of two to four of the others (the POINT_POOL with the most distinct values) that are no more than one: the columns of a mask over the pool are one key when the sample says they are, and no mask that has a smaller one in it is tried */
static int
point_keys(const SiftRows *rows, PointKey *keys, int cap)
{
	PoolCol	   *weak = palloc(Max(rows->ncols, 1) * sizeof(PoolCol));
	uint32		taken[1 << POINT_POOL];
	int			nkeys = 0,
				nweak = 0,
				ntaken = 0;

	for (int c = 0; c < rows->ncols; c++)
	{
		PointKey	k = {.nleaves = 1,.leaf = {c}};
		int			seen;

		if (!point_eligible(rows, c))
			continue;
		if (key_qualifies(rows, &k, &seen))
		{
			if (nkeys < cap)
				keys[nkeys++] = k;
		}
		else if (seen >= SIFT_POINT_SAMPLE / 2 && k.distinct >= 2)
			weak[nweak++] = (PoolCol) {c, k.distinct};
	}
	qsort(weak, nweak, sizeof(PoolCol), pool_cmp);
	nweak = Min(nweak, POINT_POOL);
	for (int size = 2; size <= SIFT_POINT_MAX_COMP; size++)
		for (uint32 mask = 1; mask < (UINT32_C(1) << nweak); mask++)
		{
			PointKey	k = {0};
			bool		covered = false;
			int			seen;

			if (pg_popcount32(mask) != size)
				continue;
			for (int t = 0; t < ntaken; t++)
				covered |= (taken[t] & mask) == taken[t];
			for (int i = 0; i < nweak; i++)
				if (mask & (UINT32_C(1) << i))
					k.leaf[k.nleaves++] = weak[i].col;
			if (!covered && nkeys < cap && key_qualifies(rows, &k, &seen))
			{
				keys[nkeys++] = k;
				taken[ntaken++] = mask;
			}
		}
	pfree(weak);
	qsort(keys, nkeys, sizeof(PointKey), key_cmp);
	return nkeys;
}

bool
sift_point_dir_build(const SiftRows *rows, const int *gstart, int ng, uint64 part_bytes, StringInfo out)
{
	PointKey	keys[4 * SIFT_POINT_MAX_COLS];
	int			nkeys,
				nkeep = 0,
				nnz[SIFT_POINT_MAX_COLS];
	PointKey	kept[SIFT_POINT_MAX_COLS];
	uint64		spent = sizeof(SiftPointHdr);
	SiftPointHdr hdr = {.magic = SIFT_POINT_MAGIC,.version = SIFT_POINT_VERSION};
	SiftPointCol cols[SIFT_POINT_MAX_COLS] = {0};

	if (rows->ncol_ids > 0 || rows->nrows < SIFT_POINT_MIN_ROWS)
		return false;
	nkeys = point_keys(rows, keys, lengthof(keys));
	for (int i = 0; i < nkeys && nkeep < SIFT_POINT_MAX_COLS; i++)
	{
		const int	n = key_nnz(rows, &keys[i]);
		const uint64 cost = sizeof(SiftPointCol) + column_bytes(n);

		if ((spent + cost) * 100 <= part_bytes * SIFT_POINT_MAX_PCT)
		{
			nnz[nkeep] = n;
			kept[nkeep++] = keys[i];
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
		cols[k].key_kind = key_kind(rows, &kept[k]);
		cols[k].nleaves = kept[k].nleaves;
		for (int i = 0; i < SIFT_POINT_MAX_COMP; i++)
			cols[k].leaves[i] = i < kept[k].nleaves ? (uint16) kept[k].leaf[i] : SIFT_POINT_NO_LEAF;
		put_column(rows, &kept[k], gstart, ng, nnz[k], out, &cols[k]);
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
	if (se->len < sizeof(SiftPointHdr))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory is shorter than its header"));
	pd = MemoryContextAllocZero(cx, sizeof(SiftPointDir));
	pd->sec = *se;
	sift_part_read_section_bytes(rel, part, se, 0, Min(se->len, sizeof(head)), head);
	memcpy(&pd->hdr, head, sizeof(pd->hdr));
	if (pd->hdr.magic != SIFT_POINT_MAGIC)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory has a header that is not one"));
	if (pd->hdr.version != SIFT_POINT_VERSION)
	{
		pfree(pd);
		return NULL;
	}
	if (pd->hdr.ncols == 0 || pd->hdr.ncols > SIFT_POINT_MAX_COLS || se->len < sizeof(SiftPointHdr) + pd->hdr.ncols * sizeof(SiftPointCol))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory has a header that is not one"));
	memcpy(pd->cols, head + sizeof(SiftPointHdr), pd->hdr.ncols * sizeof(SiftPointCol));
	for (int k = 0; k < pd->hdr.ncols; k++)
	{
		const SiftPointCol *c = &pd->cols[k];

		if (c->nleaves == 0 || c->nleaves > SIFT_POINT_MAX_COMP || c->log2_buckets < 1 || c->log2_buckets > 31 ||
			(uint64) c->off_buckets + ((UINT64CONST(1) << c->log2_buckets) + 1) * 4 > se->len || (uint64) c->off_entries + (uint64) c->nentries * 4 > se->len)
			SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory has a column past its end"));
	}
	return pd;
}

/* A predicate the directory can answer on a column: an equality or a short list on a plain column of one lane (an integer, a uuid, a text of a deterministic collation) whose constants the stored values can be compared with */
static const SiftColPred *
leaf_pred(const SiftColSchema *schema, int leaf, const SiftColPred *preds, int npreds)
{
	const SiftColDef *d = &schema->cols[leaf];

	for (int k = 0; k < npreds; k++)
	{
		const SiftColPred *p = &preds[k];

		if (p->col == leaf && (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_IN) && p->nvals <= SIFT_POINT_MAX_VALUES && d->lane == 0 && d->parent < 0 && !(d->flags & SIFT_COLD_ASTEXT) &&
			!sift_col_def_is_wide(d) && !sift_f2_undecided(d, p) &&
			(d->store == SIFT_COLS_INT || (d->store == SIFT_COLS_STR && (d->type == SIFT_COLT_TEXT || d->type == SIFT_COLT_UUID))))
			return p;
	}
	return NULL;
}

/* The predicates a key of the directory is asked by (one for each of its columns), and the lookups they make; 0 when a column has none */
static int
key_preds(const SiftColSchema *schema, const SiftPointCol *col, const SiftColPred *preds, int npreds, const SiftColPred **pred)
{
	int			lookups = 1;

	for (int i = 0; i < col->nleaves; i++)
	{
		pred[i] = col->leaves[i] < (uint32) schema->ncols ? leaf_pred(schema, col->leaves[i], preds, npreds) : NULL;
		if (pred[i] == NULL)
			return 0;
		lookups *= Max(pred[i]->nvals, 1);
		if (lookups > SIFT_POINT_MAX_VALUES)
			return 0;
	}
	return lookups;
}

static uint64
const_hash(const SiftColDef *d, const SiftColValue *v)
{
	return d->store == SIFT_COLS_INT ? sift_point_hash_i64(v->v.i) : sift_point_hash_bytes(v->v.s, v->len);
}

/* The hash of the combination of constants the digits (an index into the constants of each column) name, false when one of them is NULL */
static bool
combo_hash(const SiftColSchema *schema, const SiftPointCol *col, const SiftColPred *const *pred, const int *digit, uint64 *hash)
{
	uint64		h = 0;

	for (int i = 0; i < col->nleaves; i++)
	{
		const SiftColValue *v = &pred[i]->vals[digit[i]];
		const uint64 x = const_hash(&schema->cols[col->leaves[i]], v);

		if (v->isnull)
			return false;
		h = i == 0 ? x : sift_point_hash_combine(h, x);
	}
	*hash = h;
	return true;
}

/* The rows of the bucket of a hash, appended to the candidates */
static void
add_bucket(Relation rel, const SiftPart *part, const SiftPointDir *pd, const SiftPointCol *col, uint64 hash, SiftPointCands *pc, uint32 *cap)
{
	uint32		range[2],
				cnt;

	sift_part_read_section_bytes(rel, part, &pd->sec, col->off_buckets + (uint64) sift_point_bucket(hash, col->log2_buckets) * 4, sizeof(range), range);
	if (range[0] > range[1] || range[1] > col->nentries)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a point directory bucket is past its entries"));
	cnt = range[1] - range[0];
	if ((uint64) pc->n + cnt > MaxAllocSize / sizeof(uint32))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: the buckets of a point directory name more rows than a lookup holds"));
	if (pc->n + cnt > *cap)
	{
		*cap = (uint32) Min(Max((uint64) *cap * 2, (uint64) pc->n + cnt), MaxAllocSize / sizeof(uint32));
		pc->ref = pc->ref != NULL ? repalloc(pc->ref, *cap * sizeof(uint32)) : palloc(*cap * sizeof(uint32));
	}
	if (cnt > 0)
		sift_part_read_section_bytes(rel, part, &pd->sec, col->off_entries + (uint64) range[0] * 4, (uint64) cnt * 4, pc->ref + pc->n);
	pc->n += cnt;
}

/* The key that needs the fewest lookups among those whose columns the predicates all name */
static const SiftPointCol *
cheapest_key(const SiftPointDir *pd, const SiftColSchema *schema, const SiftColPred *preds, int npreds, const SiftColPred **pred, int *lookups)
{
	const SiftPointCol *best = NULL;

	for (int k = 0; k < pd->hdr.ncols; k++)
	{
		const SiftColPred *p[SIFT_POINT_MAX_COMP];
		const int	n = key_preds(schema, &pd->cols[k], preds, npreds, p);

		if (n > 0 && (best == NULL || n < *lookups))
		{
			best = &pd->cols[k];
			*lookups = n;
			memcpy(pred, p, sizeof(p));
		}
	}
	return best;
}

bool
sift_point_dir_cands(Relation rel, const SiftPart *part, const SiftPointDir *pd, const SiftColSchema *schema, const SiftColPred *preds, int npreds, MemoryContext cx, SiftPointCands *pc)
{
	const SiftColPred *pred[SIFT_POINT_MAX_COMP];
	const SiftPointCol *col;
	MemoryContext old;
	int			lookups,
				digit[SIFT_POINT_MAX_COMP] = {0};
	uint32		cap = 0;

	col = cheapest_key(pd, schema, preds, npreds, pred, &lookups);
	if (col == NULL)
		return false;
	old = MemoryContextSwitchTo(cx);
	*pc = (SiftPointCands) {.pred = pred[0],.pred_idx = (int) (pred[0] - preds),.str = schema->cols[col->leaves[0]].store == SIFT_COLS_STR};
	for (int j = 0; j < lookups; j++)
	{
		uint64		hash;

		if (combo_hash(schema, col, pred, digit, &hash))
			add_bucket(rel, part, pd, col, hash, pc, &cap);
		for (int i = col->nleaves - 1; i >= 0; i--)
		{
			if (++digit[i] < Max(pred[i]->nvals, 1))
				break;
			digit[i] = 0;
		}
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

static bool
str_hit(const SiftColPred *p, const SiftStrCell *cell)
{
	for (int m = 0; m < p->nvals && !cell->isnull; m++)
		if (!p->vals[m].isnull && p->vals[m].len == cell->len && memcmp(p->vals[m].v.s, cell->p, cell->len) == 0)
			return true;
	return false;
}

static bool
int_hit(const SiftColPred *p, int64 v, bool isnull)
{
	for (int m = 0; m < p->nvals && !isnull; m++)
		if (!p->vals[m].isnull && p->vals[m].v.i == v)
			return true;
	return false;
}

int
sift_point_cands_verify(const SiftPointCands *pc, const SiftGranuleView *v, uint16 *rows, int n)
{
	int64		vals[SIFT_POINT_ROWS_MAX];
	bool		isnull[SIFT_POINT_ROWS_MAX];
	SiftStrCell cells[SIFT_POINT_ROWS_MAX];
	int			kept = 0;

	if (pc->str)
		sift_granule_point_strs(v, pc->pred->col, rows, n, cells);
	else
		sift_granule_point_ints(v, pc->pred->col, rows, n, vals, isnull);
	for (int j = 0; j < n; j++)
		if (pc->str ? str_hit(pc->pred, &cells[j]) : int_hit(pc->pred, vals[j], isnull[j]))
			rows[kept++] = rows[j];
	return kept;
}
