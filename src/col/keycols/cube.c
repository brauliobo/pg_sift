/*
 * col/keycols/cube.c - the cube, the arrays and the rollup of rows by default: which dimensions and measures the build keeps
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "utils/lsyscache.h"
#include "col/am.h"
#include "col/keycols/keycols_int.h"
#include "col/shred_pg.h"

/*
 * Which plain columns take the dimensions of the cube. It holds SIFT_PAGG_MAX_DIMS of them (the calendar cells of the dates among them) and the default gives
 * the slots to the columns in their order; a column with more than SIFT_PAGG_DEF_GROUPS values in a unit has no histogram in it, so a slot given to such a column
 * holds nothing. An index with more columns than slots and preagg = 'auto' has them ranked here, once: CUBE_STRETCHES stretches of CUBE_STRETCH_BLOCKS blocks
 * (about a unit), evenly spaced over the table, are read, the columns that fit the groups in most of them come first, then the others, each in the order of the
 * schema, and the list is written to the option as 'auto: a, b' so that a rebuild keeps it (the cube takes the first of it, the calendar cells their room). A
 * NOTICE says so when the ranking changed which columns hold a slot. The columns of jsonb paths are not looked at: the index keeps the order of the schema.
 * It is not the default: the histograms it gives the columns that fit grow the index by more than the 3% a default may.
 */
#define CUBE_STRETCHES		8
#define CUBE_STRETCH_BLOCKS	256
#define CUBE_SET_SLOTS		1024	/* a power of two over twice the groups of a histogram */

typedef struct CubeCand
{
	int			leaf;
	AttrNumber	hatt;
	int			rank;			/* its place in the schema */
	uint32		fit;			/* the sampled stretches in which it has at most SIFT_PAGG_DEF_GROUPS values, NULL as one */
	uint32		seen;			/* values met in the stretch being read; over SIFT_PAGG_DEF_GROUPS it is out */
	uint64		groups;			/* the values of the stretches, together (at most SIFT_PAGG_DEF_GROUPS + 1 of each) */
} CubeCand;

static int
cmp_cube_cand(const void *a, const void *b)
{
	const CubeCand *x = a,
			   *y = b;

	return x->fit != y->fit ? (x->fit > y->fit ? -1 : 1) : x->rank - y->rank;
}

/* the key of a value in the set of its stretch (0 marks a free slot) */
static uint64
cube_key(Datum d, bool isnull, const Form_pg_attribute att)
{
	uint64		k;

	if (isnull)
		return 1;
	if (att->attbyval)
		k = (uint64) DatumGetInt64(d) * UINT64CONST(0x9E3779B97F4A7C15);
	else if (att->attlen == -1)
	{
		struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

		k = DatumGetUInt64(hash_any_extended((unsigned char *) VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), 0));
	}
	else
		k = DatumGetUInt64(hash_any_extended((unsigned char *) DatumGetPointer(d), att->attlen, 0));
	return k | 2;
}

/* the stretch st of nst over a table of nblocks: [*start, *start + *len) */
static void
cube_stretch_at(BlockNumber nblocks, int nst, int st, BlockNumber *start, BlockNumber *len)
{
	*start = (BlockNumber) ((uint64) st * nblocks / nst);
	*len = Min(CUBE_STRETCH_BLOCKS, nblocks / nst);
}

/* the stretch [start, start + len) of heap: cand[].seen counts the values of each column up to SIFT_PAGG_DEF_GROUPS + 1; fit and groups of those within it grow */
static void
cube_stretch(Relation heap, BlockNumber start, BlockNumber len, CubeCand *cand, int n, uint64 *sets, MemoryContext row_cx)
{
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);

	memset(sets, 0, (size_t) n * CUBE_SET_SLOTS * sizeof(uint64));
	for (int i = 0; i < n; i++)
		cand[i].seen = 0;
	heap_setscanlimits(scan, start, len);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cx);

		CHECK_FOR_INTERRUPTS();
		for (int i = 0; i < n; i++)
		{
			uint64	   *set = sets + (size_t) i * CUBE_SET_SLOTS;
			bool		isnull;
			Datum		d;
			uint64		k;

			if (cand[i].seen > SIFT_PAGG_DEF_GROUPS)
				continue;
			d = slot_getattr(slot, cand[i].hatt, &isnull);
			k = cube_key(d, isnull, TupleDescAttr(RelationGetDescr(heap), cand[i].hatt - 1));
			for (uint32 at = (uint32) (k >> 40) & (CUBE_SET_SLOTS - 1);; at = (at + 1) & (CUBE_SET_SLOTS - 1))
			{
				if (set[at] == 0)
				{
					set[at] = k;
					cand[i].seen++;
					break;
				}
				if (set[at] == k)
					break;
			}
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(row_cx);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	for (int i = 0; i < n; i++)
	{
		cand[i].fit += cand[i].seen <= SIFT_PAGG_DEF_GROUPS;
		cand[i].groups += cand[i].seen;
	}
}

/*
 * The plain columns of the cube's kind (a dimension of the top level, not a jsonb path) in the order of the schema; -1 when the choice is not made here: an
 * index of jsonb paths, a column that is an expression, or a name the option cannot hold (it is a list split at commas, and its names are not quoted).
 */
static int
cube_cands(Relation index, const SiftColSchema *schema, CubeCand *cand)
{
	int			n = 0;

	if (schema->kinds)
		return -1;
	for (int c = 0; c < schema->ncols; c++)
	{
		const SiftColDef *d = &schema->cols[c];
		const char *name = keycols_leaf_name(index, d);

		if (d->parent >= 0 || !sift_cpagg_dim_ok(schema, d))
			continue;
		if (keycols_heap_attno(index, d) == 0 || name[0] == '\0' || strpbrk(name, ",:;)") != NULL)
			return -1;
		cand[n] = (CubeCand) {.leaf = c, .hatt = keycols_heap_attno(index, d), .rank = n};
		n++;
	}
	return n;
}

/*
 * The elements of the array columns (paths = 'auto'): an array column of text or integers whose table has ARR_AUTO_ROWS rows or more and nearly every
 * array different (over ARR_DISTINCT_PCT% of a sample) gets the list of its elements (the paths 'col[] type', docs/COL-SHRED.md) in place of its image, which
 * cannot share anything there, when a sample says that this does not make the index more than ARR_AUTO_GROWTH_PCT% larger. Arrays that repeat keep the image,
 * which a GROUP BY or equality over whole arrays reads.
 */
#define ARR_AUTO_ROWS		10000
#define ARR_AUTO_GROWTH_PCT	25
#define ARR_DISTINCT_PCT	50
#define ARR_SAMPLE_ROWS		4096
#define ARR_SAMPLE_ELEMS	200000
#define ARR_SET_BITS		19
#define ARR_DIR_BYTES		1.5

typedef struct ArrSet
{
	uint64	   *slot;
	uint64		n;
} ArrSet;

typedef struct ArrCand
{
	int			idx;			/* its place in the index */
	AttrNumber	hatt;
	Oid			elem;
	uint64		rows;			/* arrays read (not NULL) */
	uint64		elems;			/* elements read, NULL ones too */
	uint64		bytes;			/* bytes of the arrays read */
	ArrSet		arrs;			/* hashes of the distinct arrays and elements */
	ArrSet		els;
} ArrCand;

/* adds the hash to the set; false when it was there or the set is full */
static bool
arr_set_add(ArrSet *s, uint64 h)
{
	uint64		mask = ((uint64) 1 << ARR_SET_BITS) - 1;

	h |= 1;
	if (s->n >= (mask >> 1))
		return false;
	for (uint64 p = (h >> 20) & mask;; p = (p + 1) & mask)
	{
		if (s->slot[p] == h)
			return false;
		if (s->slot[p] == 0)
		{
			s->slot[p] = h;
			s->n++;
			return true;
		}
	}
}

static void
arr_stretch(Relation heap, BlockNumber start, BlockNumber len, ArrCand *cand, int n, uint32 quota, MemoryContext row_cx)
{
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	uint32		got = 0;

	heap_setscanlimits(scan, start, len);
	while (got < quota && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cx);

		CHECK_FOR_INTERRUPTS();
		got++;
		for (int k = 0; k < n; k++)
		{
			ArrCand    *c = &cand[k];
			bool		isnull;
			Datum		d = slot_getattr(slot, c->hatt, &isnull);
			ArrayType  *a;
			int16		elen;
			bool		byval;
			char		align;
			Datum	   *vals;
			bool	   *nulls;
			int			ne;

			if (isnull)
				continue;
			a = DatumGetArrayTypeP(d);
			c->rows++;
			c->bytes += VARSIZE(a);
			arr_set_add(&c->arrs, DatumGetUInt64(hash_any_extended((unsigned char *) a, VARSIZE(a), 0)));
			get_typlenbyvalalign(c->elem, &elen, &byval, &align);
			deconstruct_array(a, c->elem, elen, byval, align, &vals, &nulls, &ne);
			for (int i = 0; i < ne && c->els.n < ARR_SAMPLE_ELEMS; i++)
			{
				c->elems++;
				if (nulls[i])
					continue;
				if (byval)
					arr_set_add(&c->els, (uint64) DatumGetInt64(vals[i]) * UINT64CONST(0x9E3779B97F4A7C15));
				else
				{
					text	   *t = DatumGetTextPP(vals[i]);

					arr_set_add(&c->els, DatumGetUInt64(hash_any_extended((unsigned char *) VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), 0)));
				}
			}
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(row_cx);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

/* the type name a path spec gives the elements */
static const char *
arr_elem_type(Oid elem)
{
	return elem == INT2OID ? "int2" : elem == INT4OID ? "int4" : elem == INT8OID ? "int8" : "text";
}

/*
 * An array column whose arrays repeat keeps its image (GROUP BY, DISTINCT and = read one code a row). An element test over the image decodes, hashes and
 * interns each distinct array of the dictionary, ARR_IMAGE_INSTR instructions an array (311.6 M instructions for 196,999 distinct arrays of 1,000,000 rows, 8.0 M
 * with the directory of the elements), where the elements cost about ARR_ELEM_ROW_INSTR a row. The elements are kept beside the image when the default formula
 * says so: speedup S, benefit R log2(S) / log2(1.05) with R = 1 (a common class) against G / 3 + W / 5, W
 * the build's write cost, taken as G (the same bytes are written), G at most ARR_AUTO_GROWTH_PCT% and the saving at least 100,000 instructions. The distinct arrays
 * are the statistics' when the table has them (a sample of a few thousand rows sees few repeats), else the sample's.
 */
#define ARR_IMAGE_INSTR		1500
#define ARR_ELEM_ROW_INSTR	5

static bool
arr_both_pays(Relation heap, const ArrCand *c, double growth)
{
	double		total = heap->rd_rel->reltuples,
				ndistinct,
				corr,
				distinct = (double) c->arrs.n / c->rows * total,
				saved,
				base,
				speedup;
	int32		width;

	if (keycols_col_stats(heap, c->hatt, &ndistinct, &corr, &width))
		distinct = Max(distinct, ndistinct < 0 ? -ndistinct * total : ndistinct);
	saved = distinct * ARR_IMAGE_INSTR;
	base = total * ARR_ELEM_ROW_INSTR;
	speedup = (base + saved) / base;
	return growth <= ARR_AUTO_GROWTH_PCT && saved >= 100000 && speedup >= 1.05 &&
		log2(speedup) / log2(1.05) >= growth / 3 + growth / 5;
}

/*
 * The arrays of the index that get their elements, from a sample of the table: appends 'col[] type' to spec and the names to added.
 */
void
keycols_auto_array_specs(Relation heap, Relation index, StringInfo spec, StringInfo only, StringInfo both)
{
	ArrCand		cand[SIFT_COL_MAX_COLS];
	int			n = 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	int			nst = (int) Min(CUBE_STRETCHES, (nblocks + CUBE_STRETCH_BLOCKS - 1) / CUBE_STRETCH_BLOCKS);
	MemoryContext row_cx;
	double		base;

	if (sift_col_options(index)->array_lengths != SIFT_COL_ARRLEN_ON || heap->rd_rel->reltuples < ARR_AUTO_ROWS || nst == 0)
		return;
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index) && n < SIFT_COL_MAX_COLS; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		Oid			elem = sift_col_is_array_type(att->atttypid) ? get_element_type(att->atttypid) : InvalidOid;

		if (!OidIsValid(elem) || !sift_shred_array_elem_ok(elem) || index->rd_index->indkey.values[i] == 0 ||
			strcmp(quote_identifier(NameStr(att->attname)), NameStr(att->attname)) != 0)
			continue;
		cand[n] = (ArrCand) {.idx = i,.hatt = index->rd_index->indkey.values[i],.elem = elem};
		cand[n].arrs.slot = palloc0(sizeof(uint64) << ARR_SET_BITS);
		cand[n].els.slot = palloc0(sizeof(uint64) << ARR_SET_BITS);
		n++;
	}
	if (n == 0)
		return;
	row_cx = AllocSetContextCreate(CurrentMemoryContext, "sift array sample row", ALLOCSET_DEFAULT_SIZES);
	for (int st = 0; st < nst; st++)
	{
		BlockNumber start,
					len;

		cube_stretch_at(nblocks, nst, st, &start, &len);
		arr_stretch(heap, start, len, cand, n, ARR_SAMPLE_ROWS / nst, row_cx);
	}
	MemoryContextDelete(row_cx);
	base = keycols_est_index_bytes(heap, index);
	for (int k = 0; k < n; k++)
	{
		const ArrCand *c = &cand[k];
		const char *name = NameStr(TupleDescAttr(RelationGetDescr(index), c->idx)->attname);
		double		total = heap->rd_rel->reltuples,
					elem_row,
					image_row;
		bool		alone = c->arrs.n * 100 > c->rows * ARR_DISTINCT_PCT;

		if (c->rows < 100 || c->elems == 0)
			continue;
		elem_row = (double) c->elems / c->rows * (log2(Max(c->els.n, 2)) / 8 + ARR_DIR_BYTES);
		image_row = (double) c->bytes / c->rows * Min(1.0, (double) c->arrs.n / c->rows);
		if (alone)
		{
			/* nearly every array different: the image shares nothing and the elements, far smaller, stand in its place */
			if ((elem_row - image_row) * 100 / Max(base, 1) > ARR_AUTO_GROWTH_PCT)
				continue;
		}
		else if (!arr_both_pays(heap, c, elem_row * 100 / Max(base, 1)))
			continue;
		appendStringInfo(spec, "%s%s%s[] %s", spec->len ? ", " : "", alone ? "" : "+", name, arr_elem_type(c->elem));
		appendStringInfo(alone ? only : both, "%s%s", (alone ? only : both)->len ? ", " : "", name);
	}
	for (int k = 0; k < n; k++)
	{
		pfree(cand[k].arrs.slot);
		pfree(cand[k].els.slot);
	}
}

/* the stretches read over heap, and what the candidates hold in them (fit, groups); the number of stretches, 0 for an empty table */
static int
cube_sample(Relation heap, CubeCand *cand, int n)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	int			nst = (int) Min(CUBE_STRETCHES, (nblocks + CUBE_STRETCH_BLOCKS - 1) / CUBE_STRETCH_BLOCKS);
	uint64	   *sets;
	MemoryContext row_cx;

	if (nst == 0)
		return 0;
	sets = palloc((size_t) n * CUBE_SET_SLOTS * sizeof(uint64));
	row_cx = AllocSetContextCreate(CurrentMemoryContext, "sift cube ranking row", ALLOCSET_SMALL_SIZES);
	for (int st = 0; st < nst; st++)
	{
		BlockNumber start,
					len;

		cube_stretch_at(nblocks, nst, st, &start, &len);
		cube_stretch(heap, start, len, cand, n, sets, row_cx);
	}
	MemoryContextDelete(row_cx);
	pfree(sets);
	return nst;
}

void
sift_col_auto_cube(Relation heap, Relation index)
{
	const char *opt = sift_col_options_preagg(index);
	const SiftColRel *rel;
	CubeCand	cand[SIFT_COL_MAX_COLS];
	int			n,
				slots,
				nst;
	StringInfoData list,
				first,
				v;
	bool		moved = false;

	/* the default order, off, cube(...) and the list an earlier build chose stay: only preagg = 'auto' asks for the ranking */
	if (pg_strcasecmp(opt, "auto") != 0)
		return;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	n = cube_cands(index, rel->schema, cand);
	slots = SIFT_PAGG_MAX_DIMS - sift_cpagg_cal_count(rel->schema);
	if (n <= slots || slots <= 0 || (nst = cube_sample(heap, cand, n)) == 0)
		return;
	qsort(cand, n, sizeof(CubeCand), cmp_cube_cand);

	initStringInfo(&list);
	initStringInfo(&first);
	for (int i = 0; i < n && i < SIFT_PAGG_MAX_DIMS; i++)
	{
		keycols_append_name(&list, keycols_leaf_name(index, &rel->schema->cols[cand[i].leaf]));
		if (i < slots && cand[i].fit * 2 > (uint32) nst)
			keycols_append_name(&first, keycols_leaf_name(index, &rel->schema->cols[cand[i].leaf]));
		moved |= i < slots && cand[i].fit * 2 > (uint32) nst && cand[i].rank >= slots;
	}
	initStringInfo(&v);
	appendStringInfo(&v, "auto: %s", list.data);
	sift_col_set_option(index, "preagg", v.data);
	if (moved)
		ereport(NOTICE, (errmsg("sift_col index %s gives the dimensions of its cube to the columns whose histograms fit: %s", quote_identifier(RelationGetRelationName(index)), first.data),
						 errdetail("The cube holds at most %d dimensions, the calendar cells of the dates among them. A column with more than %d distinct values in a unit has no histogram there, so a dimension given to it holds nothing. Stretches of about a unit (%d of them, spread over the table) were read, and the columns that fit most of them come first.",
								   SIFT_PAGG_MAX_DIMS, SIFT_PAGG_DEF_GROUPS, nst),
						 errhint("ALTER INDEX %s SET (preagg = 'default'), then REINDEX, gives them in the order of the columns; preagg = 'cube(...)' names them.", quote_identifier(RelationGetRelationName(index)))));
}

/*
 * The rows rollup of rollup_dims = 'auto' (docs/COL-PREAGG.md): the pair advice of sift.index_advice, chosen before the build. Of the columns that fit the cube's
 * histograms in ROWS_AGREE_PCT of the sampled stretches (the SIFT_PAGG_MAX_RDIMS with the fewest values), the pair whose combinations fit the cells a unit may
 * keep (a tenth of its rows, at most SIFT_PAGG_ROWS_CELLS) in as many stretches, and has the fewest of them (the cheapest rollup, far from the limit of a unit),
 * is written to the option as 'auto: a, b'; none fits, nothing. It is the default (an empty option): the cells pay by the default formula (docs/OPTIONS.md); 'off' writes none.
 */
#define ROWS_AGREE_PCT	90
#define ROWS_CAP		(1 << 16)		/* rows of a stretch looked at */
#define ROWS_SLOTS		(4 * SIFT_PAGG_ROWS_CELLS)	/* a power of two over the cells a unit may keep */

typedef struct RowsPair
{
	uint32		ok;				/* stretches in which the combinations fit the cells of their rows */
	uint64		cells;			/* the combinations of those stretches, together */
} RowsPair;

/* the distinct pairs of a[] and b[], at most limit + 1 (it stops counting there); tab is ROWS_SLOTS zeroed words */
static uint32
rows_cells(const uint64 *a, const uint64 *b, uint32 rows, uint32 limit, uint64 *tab)
{
	uint32		cells = 0;

	memset(tab, 0, ROWS_SLOTS * sizeof(uint64));
	for (uint32 r = 0; r < rows && cells <= limit; r++)
	{
		uint64		h = ((a[r] ^ (b[r] * UINT64CONST(0xC2B2AE3D27D4EB4F))) * UINT64CONST(0x9E3779B97F4A7C15)) | 1;

		for (uint32 at = (uint32) (h >> 40) & (ROWS_SLOTS - 1);; at = (at + 1) & (ROWS_SLOTS - 1))
		{
			if (tab[at] == 0)
			{
				tab[at] = h;
				cells++;
				break;
			}
			if (tab[at] == h)
				break;
		}
	}
	return cells;
}

static int
cmp_cube_groups(const void *a, const void *b)
{
	const CubeCand *x = a,
			   *y = b;

	return x->groups != y->groups ? (x->groups < y->groups ? -1 : 1) : x->rank - y->rank;
}

/* the pairs of the m columns of cand in the stretch: pr[x * m + y] (x < y) counts the stretch when the combinations of both fit the cells of the rows read */
static void
rows_stretch(Relation heap, BlockNumber start, BlockNumber len, const CubeCand *cand, int m, RowsPair *pr)
{
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	MemoryContext row_cx = AllocSetContextCreate(CurrentMemoryContext, "sift rows rollup row", ALLOCSET_SMALL_SIZES);
	uint64	   *keys[SIFT_PAGG_MAX_RDIMS],
			   *tab = palloc(ROWS_SLOTS * sizeof(uint64));
	uint32		rows = 0;

	for (int k = 0; k < m; k++)
		keys[k] = palloc(ROWS_CAP * sizeof(uint64));
	heap_setscanlimits(scan, start, len);
	while (rows < ROWS_CAP && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cx);

		CHECK_FOR_INTERRUPTS();
		for (int k = 0; k < m; k++)
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, cand[k].hatt, &isnull);

			keys[k][rows] = cube_key(d, isnull, TupleDescAttr(RelationGetDescr(heap), cand[k].hatt - 1));
		}
		rows++;
		MemoryContextSwitchTo(old);
		MemoryContextReset(row_cx);
	}
	for (int x = 0; x < m; x++)
		for (int y = x + 1; y < m; y++)
		{
			uint32		limit = Min(rows / 10, SIFT_PAGG_ROWS_CELLS),
						cells = rows_cells(keys[x], keys[y], rows, limit, tab);

			if (cells <= limit)
			{
				pr[x * m + y].ok++;
				pr[x * m + y].cells += cells;
			}
		}
	for (int k = 0; k < m; k++)
		pfree(keys[k]);
	pfree(tab);
	MemoryContextDelete(row_cx);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

/*
 * array_lengths = auto: an index with an array column keeps the cardinality and the array_length(x, 1) of every array as two integer lanes, so that a test of either, and x IS [NOT] NULL, never decode the arrays: the images cost a hash, an
 * interning and a decompression of their own length for every row. Written as 'on' before the build, so that the build, its workers and a REINDEX share one schema.
 */
void
sift_col_auto_arrlen(Relation index)
{
	StringInfoData names;

	if (sift_col_options(index)->array_lengths != SIFT_COL_ARRLEN_AUTO)
		return;
	initStringInfo(&names);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);

		if (sift_col_is_array_type(att->atttypid))
			keycols_append_name(&names, NameStr(att->attname));
	}
	if (names.len == 0)
		return;
	sift_col_set_option(index, "array_lengths", "on");
	ereport(NOTICE, (errmsg("sift_col index %s keeps the lengths of the arrays %s", quote_identifier(RelationGetRelationName(index)), names.data),
					 errdetail("cardinality(x), array_length(x, 1) and x IS [NOT] NULL are read from two integer columns of each array, without the array: about a byte a row for each."),
					 errhint("ALTER INDEX %s SET (array_lengths = 'off'), then REINDEX, drops them.", quote_identifier(RelationGetRelationName(index)))));
}

void
sift_col_auto_rows(Relation heap, Relation index)
{
	const char *opt = sift_col_options_rollup_dims(index);
	const SiftColRel *rel;
	CubeCand	cand[SIFT_COL_MAX_COLS];
	RowsPair	pr[SIFT_PAGG_MAX_RDIMS * SIFT_PAGG_MAX_RDIMS] = {{0}};
	int			n,
				m = 0,
				nst,
				bx = -1,
				by = -1;
	BlockNumber nblocks;
	StringInfoData v;

	if (*opt != '\0' && pg_strcasecmp(opt, "auto") != 0)
		return;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	if ((n = cube_cands(index, rel->schema, cand)) < 2 || (nst = cube_sample(heap, cand, n)) == 0)
		return;
	/* the columns that fit, the fewest values first */
	qsort(cand, n, sizeof(CubeCand), cmp_cube_groups);
	for (int i = 0; i < n && m < SIFT_PAGG_MAX_RDIMS; i++)
		if (cand[i].fit * 100 >= (uint32) nst * ROWS_AGREE_PCT)
			cand[m++] = cand[i];
	if (m < 2)
		return;
	nblocks = RelationGetNumberOfBlocks(heap);
	for (int st = 0; st < nst; st++)
	{
		BlockNumber start,
					len;

		cube_stretch_at(nblocks, nst, st, &start, &len);
		rows_stretch(heap, start, len, cand, m, pr);
	}
	for (int x = 0; x < m; x++)
		for (int y = x + 1; y < m; y++)
			if (pr[x * m + y].ok * 100 >= (uint32) nst * ROWS_AGREE_PCT &&
				(bx < 0 || pr[x * m + y].cells < pr[bx * m + by].cells ||
				 (pr[x * m + y].cells == pr[bx * m + by].cells && cand[x].rank + cand[y].rank < cand[bx].rank + cand[by].rank)))
			{
				bx = x;
				by = y;
			}
	if (bx < 0)
		return;
	initStringInfo(&v);
	if (cand[bx].rank < cand[by].rank)
		appendStringInfo(&v, "auto: %s, %s", keycols_leaf_name(index, &rel->schema->cols[cand[bx].leaf]), keycols_leaf_name(index, &rel->schema->cols[cand[by].leaf]));
	else
		appendStringInfo(&v, "auto: %s, %s", keycols_leaf_name(index, &rel->schema->cols[cand[by].leaf]), keycols_leaf_name(index, &rel->schema->cols[cand[bx].leaf]));
	sift_col_set_option(index, "rollup_dims", v.data);
	ereport(NOTICE, (errmsg("sift_col index %s writes a rollup of the rows by %s", quote_identifier(RelationGetRelationName(index)), v.data + 6),
					 errdetail("A GROUP BY or a filter of both columns is read from the cells of each unit. Their combinations fit the cells a unit may keep (a tenth of its rows, at most %d) in at least %d%% of %d sampled stretches of about a unit, about %d of them a unit; the cells add to the index and to its build.",
							   SIFT_PAGG_ROWS_CELLS, ROWS_AGREE_PCT, nst, (int) (pr[bx * m + by].cells / pr[bx * m + by].ok)),
					 errhint("ALTER INDEX %s SET (rollup_dims = 'off'), then REINDEX, drops it; rollup_dims = 'a, b: measure' names the columns.", quote_identifier(RelationGetRelationName(index)))));
}
