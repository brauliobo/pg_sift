/*
 * agg/sel.c - the rows of a column unit that a match set keeps (agg/sel.h).
 */
#include "postgres.h"

#include "catalog/pg_type.h"

#include "agg/sel.h"
#include "core/popcount.h"
#include "agg/colsrc.h"

/* Few kept rows are decoded one by one; many are decoded with the whole chunk, and the mask keeps the others out */
#define SEL_SPARSE	8

void
sift_aggsel_init(SiftAggSel *a, MemoryContext mcxt)
{
	memset(a, 0, sizeof(*a));
	a->mcxt = mcxt;
}

/* idx: the stored ordinal of each kept row of a compacted unit */
static void
compact_idx(SiftAggSel *a)
{
	if (a->idxcap < a->sel.count + SIFT_COL_IDXPAD)
	{
		if (a->idx)
			pfree(a->idx);
		a->idxcap = Max(a->sel.count + SIFT_COL_IDXPAD, 1024);
		a->idx = MemoryContextAlloc(a->mcxt, a->idxcap * sizeof(uint32));
	}
	sift_col_sel_idx(&a->sel, a->idx);
}

bool
sift_aggsel_unit(SiftAggSel *a, const SiftColUnit *cu, uint32 *nrow)
{
	sift_col_sel_all(cu, &a->sel);
	a->nrow = a->sel.n;
	if (!sift_col_filter_tids(cu, a->keep, &a->sel))
	{
		a->units_skipped++;
		return false;
	}
	a->compact = (uint64) a->sel.count * SEL_SPARSE <= a->nrow;
	a->units_compact += a->compact;
	*nrow = a->compact ? a->sel.count : a->nrow;
	if (a->compact)
		compact_idx(a);
	return true;
}

void
sift_aggsel_narrow(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, const uint64 *bits, uint32 *nrow)
{
	SiftColSel	kept;

	Assert(!a->compact);
	sift_col_sel_alloc(src, a->keep ? a->nrow : cu->nrows, &kept);
	memcpy(kept.bits, bits, kept.nwords * sizeof(uint64));
	sift_col_sel_count(&kept);
	a->nrow = kept.n;
	a->sel = kept;
	a->compact = true;
	compact_idx(a);
	*nrow = kept.count;
}

void
sift_aggsel_read(SiftAggSel *a, const SiftColUnit *cu, int col, SiftColVec *vec)
{
	if (a->compact)
		sift_col_read_idx(cu, col, a->idx, a->sel.count, vec);
	else
		sift_col_read(cu, col, NULL, vec);
}

void
sift_aggsel_live(SiftAggSel *a, const SiftColUnit *cu, uint64 *bits)
{
	SiftColSel	all;

	if (a->compact)
	{
		memset(bits, 0, ((a->sel.count + 63) / 64) * sizeof(uint64));
		for (uint32 i = 0; i < a->sel.count; i++)
			bits[i >> 6] |= UINT64CONST(1) << (i & 63);
		return;
	}
	if (a->keep)
	{
		memcpy(bits, a->sel.bits, a->sel.nwords * sizeof(uint64));
		return;
	}
	sift_col_sel_all(cu, &all);
	memcpy(bits, all.bits, all.nwords * sizeof(uint64));
}

/* Runs op on the unit as stored: a compacted unit's rows are expanded to their stored ordinals, op restricts them, and the survivors are compressed back into bits */
void
sift_aggsel_stored(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits, SiftAggSelOp op, void *arg)
{
	SiftColSel	vs;
	const uint32 *idx = a->idx;

	if (a->compact)
	{
		sift_col_sel_alloc(src, a->nrow, &vs);
		for (uint32 i = 0; i < a->sel.count; i++)
			if (bits[i >> 6] >> (i & 63) & 1)
				vs.bits[idx[i] >> 6] |= UINT64CONST(1) << (idx[i] & 63);
		sift_col_sel_count(&vs);
		op(cu, &vs, arg);
		for (uint32 i = 0; i < a->sel.count; i++)
			if (!(vs.bits[idx[i] >> 6] >> (idx[i] & 63) & 1))
				bits[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
		return;
	}
	sift_col_sel_alloc(src, a->keep ? a->nrow : cu->nrows, &vs);
	memcpy(vs.bits, bits, vs.nwords * sizeof(uint64));
	sift_col_sel_count(&vs);
	op(cu, &vs, arg);
	memcpy(bits, vs.bits, vs.nwords * sizeof(uint64));
}

static void
vet_op(const SiftColUnit *cu, SiftColSel *vs, void *arg)
{
	sift_col_vet(cu, vs);
}

bool
sift_aggsel_vet_needed(const SiftColUnit *cu)
{
	return !sift_col_unit_allvisible(cu);
}

void
sift_aggsel_vet(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits)
{
	if (sift_aggsel_vet_needed(cu))
		sift_aggsel_stored(a, src, cu, bits, vet_op, NULL);
}

void
sift_aggsel_overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
			 errhint("Set sift.enable_agg to off for this query.")));
}

/* ---- the JSON type of the values of a leaf: the tags of the overflow values of the chunks ---- */

StaticAssertDecl(SIFT_COLTAG_STRING == 1 && SIFT_COLTAG_JNULL == 7, "SIFT_SRC_JT_* are the chunk tags shifted by one");

/* the tag of the values a leaf of this type stores */
static uint8
stored_tag(Oid typid)
{
	return typid == BOOLOID ? SIFT_COLTAG_BOOLEAN : typid == TEXTOID ? SIFT_COLTAG_STRING : SIFT_COLTAG_NUMBER;
}

void
sift_aggsel_types_unknown_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values whose JSON type its index did not store"),
			 errhint("REINDEX the index to store them, or set sift.enable_agg to off for this query.")));
}

typedef struct TypesTest
{
	SiftColSource *cs;
	int			col;
	Oid			typid;
	bool		astext;
	uint32		mask;			/* bit t: the type of tag t */
	bool		read;			/* the query reads the leaf elsewhere */
} TypesTest;

/* the rows of the unit whose value has one of the types; a row whose value of those types the leaf cannot hold is an error if a snapshot sees it and the query reads the leaf */
static void
types_op(const SiftColUnit *cu, SiftColSel *vs, void *arg)
{
	TypesTest  *t = arg;
	bool		known,
				left;
	uint64	   *ovf;
	SiftColSel	bad;

	left = sift_col_filter_types(cu, t->col, t->mask, stored_tag(t->typid), t->astext, vs, &known);
	if (!known)
		sift_aggsel_types_unknown_error();
	ovf = palloc(SIFT_COL_NWORDS(cu->nrows) * sizeof(uint64));
	if (!left || !t->read || !sift_col_ovf_bits(cu, t->col, ovf))
		return;
	sift_col_sel_alloc(t->cs, cu->nrows, &bad);
	for (uint32 w = 0; w < bad.nwords; w++)
		bad.bits[w] = vs->bits[w] & ovf[w];
	if (sift_col_sel_count(&bad) > 0 && sift_col_vet(cu, &bad))
		sift_aggsel_overflow_error();
}

void
sift_aggsel_filter_types(SiftAggSel *a, SiftColSource *cs, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 types, uint64 *bits)
{
	TypesTest	t = {cs, col, typid, astext, (types & SIFT_SRC_JT_ALL) << 1, (types & SIFT_SRC_JT_READ) != 0};

	sift_aggsel_stored(a, cs, cu, bits, types_op, &t);
}

/* a chunk tag as read_types gives it: the index of the name jsonb_typeof gives, or none */
static uint8
src_type(uint8 tag)
{
	return tag == SIFT_COLTAG_NONE ? SIFT_SRC_TYPE_NONE : tag == SIFT_COLTAG_JNULL ? 5 : tag - 1;
}

void
sift_aggsel_read_types(SiftAggSel *a, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 nrow, uint8 *tags)
{
	bool		compact = a->compact;
	uint8	   *all = compact ? palloc(a->nrow) : tags;

	if (!sift_col_row_types(cu, col, stored_tag(typid), astext, all))
		sift_aggsel_types_unknown_error();
	for (uint32 i = 0; i < nrow; i++)
		tags[i] = src_type(all[compact ? a->idx[i] : i]);
}

bool
sift_aggsel_rows_left(const uint64 *bits, uint32 nrow)
{
	uint64		left = 0;

	for (uint32 w = 0; w < (nrow + 63) / 64; w++)
		left += sift_pc64(bits[w]);
	return left > 0;
}
