/*
 * table/wide.c - the wide leaves of the column source of a sift_table: the dictionary of exact datum images and the conjuncts decided by the type's own comparison
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "common/hashfn.h"
#include "nodes/bitmapset.h"
#include "pgstat.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteHandler.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "sift_aggkern.h"
#include "sift_aggpre.h"
#include "sift_aggsel.h"
#include "sift_aggwide.h"
#include "col/col.h"
#include "col/am.h"
#include "col/pagg.h"
#include "col/seg.h"
#include "sift_colsrc.h"
#include "table/table.h"
#include "core/visibility.h"
#include "table/src_int.h"

/* ---- a wide leaf: the dictionary of the exact datum images of the rows read, an entry the datum itself (sift_aggwide.h) ---- */

typedef struct WideBuild
{
	uint32		nd;
	uint32		dcap;
	uint32	   *off;			/* nd + 1: the entries start on 8 bytes */
	uint32	   *len;			/* nd: the size of the image */
	char	   *bytes;
	size_t		bcap;
	uint32		nslot;
	uint32	   *slot;			/* the entry + 1 of a hash of an image */
} WideBuild;

/* the entry of an image, added when new */
static uint32
wide_entry(WideBuild *w, const char *img, uint32 len)
{
	uint32		mask = w->nslot - 1,
				s = hash_bytes((const unsigned char *) img, len) & mask,
				e;
	size_t		end;

	for (; (e = w->slot[s]) != 0; s = (s + 1) & mask)
		if (w->len[e - 1] == len && memcmp(w->bytes + w->off[e - 1], img, len) == 0)
			return e - 1;
	end = (size_t) w->off[w->nd] + MAXALIGN(Max(len, 1));
	if (w->nd == w->dcap)
	{
		w->dcap *= 2;
		w->off = repalloc(w->off, (w->dcap + 1) * sizeof(uint32));
		w->len = repalloc(w->len, w->dcap * sizeof(uint32));
	}
	if (end > w->bcap)
	{
		w->bcap = Max(end, w->bcap * 2);
		w->bytes = repalloc(w->bytes, w->bcap);
	}
	memcpy(w->bytes + w->off[w->nd], img, len);
	w->len[w->nd] = len;
	w->off[w->nd + 1] = end;
	w->slot[s] = ++w->nd;
	return w->nd - 1;
}

/* the image of a datum of a wide leaf: a varlena in its plain form, or the bytes of a fixed size */
static const char *
datum_image(Datum d, int16 typlen, uint32 *len)
{
	if (typlen > 0)
	{
		*len = typlen;
		return DatumGetPointer(d);
	}
	d = PointerGetDatum(PG_DETOAST_DATUM(d));
	*len = VARSIZE(DatumGetPointer(d));
	return DatumGetPointer(d);
}

/*
 * The leaf of the n rows `v` holds (NULL: the rows of a column the segment lacks, all the default `dflt`) as codes into a dictionary of their images,
 * made in a context of its own that the next read of the leaf resets: the dictionary and codes are valid until then.
 */
void
wide_read(TblSource *t, int leaf, const TblPart *p, const SiftColVec *v, size_t n, Datum dflt, bool dnull, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];
	int16		typlen = TupleDescAttr(RelationGetDescr(t->rel), l->attno - 1)->attlen;
	TblLeafBuf *b = leaf_buf(t, leaf, SCV_CODE, n);
	MemoryContext old;
	WideBuild	w = {0};
	bool		has_null = false;

	if (t->wcxt[leaf] == NULL)
		t->wcxt[leaf] = AllocSetContextCreate(t->cxt, "sift table wide leaf", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(t->wcxt[leaf]);
	old = MemoryContextSwitchTo(t->wcxt[leaf]);
	w.dcap = 64;
	w.off = palloc0((w.dcap + 1) * sizeof(uint32));
	w.len = palloc(w.dcap * sizeof(uint32));
	w.bcap = 4096;
	w.bytes = palloc(w.bcap);
	for (w.nslot = 64; w.nslot < 2 * Max(n, 1); w.nslot *= 2)
		;
	w.slot = palloc0(w.nslot * sizeof(uint32));
	for (size_t i = 0; i < n; i++)
	{
		bool		isnull = v ? v->nulls && sift_colsrc_bit(v->nulls, i) : dnull;
		uint32		len;
		const char *img;

		((uint32 *) b->data)[i] = 0;
		b->isnull[i] = isnull;
		if (isnull)
		{
			has_null = true;
			continue;
		}
		img = datum_image(v ? sift_tbl_leaf_datum(t->rel, &p->schema->cols[p->col[leaf]], v, i) : dflt, typlen, &len);
		((uint32 *) b->data)[i] = wide_entry(&w, img, len);
	}
	MemoryContextSwitchTo(old);
	memset((uint32 *) b->data + n, 0, SIFT_AGG_PAD * sizeof(uint32));
	t->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++t->wseq), w.nd, w.off, w.bytes};
	*vec = (SiftSrcVec) {.data = b->data,.isnull = has_null ? b->isnull : NULL,.dict = &t->dict[leaf],.n = n};
}

/* ---- the conjuncts on wide leaves: the type's own comparison over the values of the column (no format of lanes for a table, no zone) ---- */

int
tbl_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	TblSource  *tb = impl;

	tb->wide = MemoryContextAlloc(tb->cxt, Max(nt, 1) * sizeof(TblWide));
	for (int i = 0; i < nt; i++)
		tb->wide[i] = (TblWide) {t[i].leaf, t[i].op, t[i].nvals, t[i].vals, t[i].isnull,
			&lookup_type_cache(tb->lay->leaf[t[i].leaf].wtypid, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo};
	tb->nwide = nt;
	return nt;
}

/* the verdict of a comparison operator (SiftCmpOp) on the result of the type's cmp function */
static bool
cmp_holds(int op, int c)
{
	static const bool table[6][3] = {{false, true, false}, {true, false, true}, {true, false, false}, {true, true, false}, {false, false, true}, {false, true, true}};

	return table[op][c < 0 ? 0 : c == 0 ? 1 : 2];
}

/* the SQL meaning of the test for a value (a NULL operand never matches, NOT IN with one passes nothing) */
static bool
wide_pass(const TblWide *w, bool isnull, Datum d)
{
	bool		found = false;

	if (w->op == SWT_ISNULL || w->op == SWT_NOTNULL)
		return isnull == (w->op == SWT_ISNULL);
	if (w->op == SWT_NOTIN && w->nvals == 0)
		return true;
	if (isnull)
		return false;
	if (w->op < SWT_IN)
		return !w->isnull[0] && cmp_holds(w->op, DatumGetInt32(FunctionCall2Coll(w->cmp, InvalidOid, d, w->vals[0])));
	for (int k = 0; k < w->nvals; k++)
	{
		if (w->isnull[k])
		{
			if (w->op == SWT_NOTIN)
				return false;
			continue;
		}
		found |= DatumGetInt32(FunctionCall2Coll(w->cmp, InvalidOid, d, w->vals[k])) == 0;
	}
	return w->op == SWT_IN ? found : !found;
}

bool
tbl_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const TblWide *w = &t->wide[i];
	size_t		nrw = (unit->nrow + 63) / 64;
	SiftColVec	v;
	bool		any = false,
				absent = p->col[w->leaf] < 0,
				isnull = false,
				pass = false;
	Datum		d;

	if (absent)
	{
		d = getmissingattr(RelationGetDescr(t->rel), t->lay->leaf[w->leaf].attno, &isnull);
		pass = wide_pass(w, isnull, d);
	}
	else
		sift_aggsel_read(&t->sel, &p->unit, p->col[w->leaf], &v);
	for (size_t k = 0; k < nrw; k++)
		for (uint64 m = rows[k]; m; m &= m - 1)
		{
			uint32		r = k * 64 + __builtin_ctzll(m);
			bool		ok = pass;

			if (!absent)
			{
				isnull = v.nulls && sift_colsrc_bit(v.nulls, r);
				ok = wide_pass(w, isnull, isnull ? (Datum) 0 : sift_tbl_leaf_datum(t->rel, &p->schema->cols[p->col[w->leaf]], &v, r));
			}
			if (ok)
				any = true;
			else
				rows[k] &= ~(UINT64CONST(1) << (r & 63));
		}
	return any;
}
