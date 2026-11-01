/*
 * col/scan/gmem.c - the rows of the write buffers sorted for the global order of a leaf, made from the batches of a scan or found where the backend kept them from the last scan
 */
#include "postgres.h"
#include "col/point.h"
#include "col/wbuf.h"
#include "core/cmp.h"
#include "col/scan/gmem.h"

/*
 * The rows [cov[i], nrows) of the batches of the write buffers (all of them for NULL) as the pairs of a numeric key (pv, nv) and the addresses of the rows with no value (pn, nn), the
 * rows of a group leaf's value gval only when gleaf >= 0; false when one holds an overflow value of the leaf or an address a block of the global order cannot name
 */
static bool
gord_collect(SiftColSource *src, int leaf, int gleaf, int64 gval, const uint32 *cov, GOrdPair *pv, uint32 *nv, GOrdPair *pn, uint32 *nn)
{
	*nv = *nn = 0;
	for (int i = 0; i < src->nmem; i++)
	{
		const SiftColBatch *b = src->mem[i].b;
		const SiftCVals *v = &b->col[leaf];

		for (uint32 r = cov ? cov[i] : 0; r < b->nrows; r++)
		{
			if ((v->ovf && (v->ovf[r / 64] >> (r % 64) & 1)) || (b->keys[r] & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF)
				return false;
			if (gleaf >= 0)
			{
				const SiftCVals *gv = &b->col[gleaf];

				if ((gv->ovf && (gv->ovf[r / 64] >> (r % 64) & 1)) || (gv->nulls && (gv->nulls[r / 64] >> (r % 64) & 1)))
				{
					if (gv->ovf && (gv->ovf[r / 64] >> (r % 64) & 1))
						return false;
					continue;
				}
				if (gv->key[r] != gval)
					continue;
			}
			if (v->nulls && (v->nulls[r / 64] >> (r % 64) & 1))
				pn[(*nn)++] = (GOrdPair) {0, (uint64) b->keys[r]};
			else
				pv[(*nv)++] = (GOrdPair) {v->key[r], (uint64) b->keys[r]};
		}
	}
	return true;
}

uint64
gord_mem_rows(const SiftColSource *src)
{
	uint64		n = 0;

	for (int i = 0; i < src->nmem; i++)
		n += src->mem[i].b->nrows;
	return n;
}

/* The rows of the write buffers sorted like a segment's entries, in mc; false when collect refuses a row */
static bool
gord_mem_num(SiftColSource *src, int leaf, int gleaf, int64 gval, int v, MemoryContext mc)
{
	ColGOrdMem *g;
	uint64		n = gord_mem_rows(src);
	GOrdPair   *pv,
			   *pn;
	uint32		nv,
				nn;

	pv = palloc((n + 1) * sizeof(GOrdPair));
	pn = palloc((n + 1) * sizeof(GOrdPair));
	if (!gord_collect(src, leaf, gleaf, gval, NULL, pv, &nv, pn, &nn))
		return false;
	gord_pairs_sort(pn, nn);
	g = MemoryContextAllocZero(mc, sizeof(ColGOrdMem));
	g->leaf = leaf;
	g->gleaf = gleaf;
	g->gval = gval;
	g->n = nv + nn;
	g->nvals = nv;
	g->key = MemoryContextAlloc(mc, (Size) (g->n + 1) * sizeof(int64));
	g->tid = MemoryContextAlloc(mc, (Size) (g->n + 1) * sizeof(uint64));
	gmem_ranges(g, mc, pv, nv);
	for (uint32 i = 0; i < nn; i++)
	{
		g->key[nv + i] = 0;
		g->tid[nv + i] = pn[i].tid;
	}
	src->gmem[v] = g;
	pfree(pn);
	return true;
}

/*
 * ---- the order of the buffers' rows kept between scans ----
 * A scan of a table whose write buffer holds rows made this order from the batches, 60 instructions a row on every query. The batches are kept by the backend (col/wbuf/batches.c)
 * and so is the order, named by the state of the sets it was made from (SiftCWbufToken): the same sets with the same rows give the same order, and a set that gained rows (appended
 * since) gives the order extended with them, when their keys fall in its ranges. A scan that holds an order (refs) keeps it alive; an order that left the cache goes with the last scan.
 */
#define GKEPT_N			4

#ifdef SIFT_TESTING
uint64		sift_col_gkept_stat[4];
bool		sift_test_col_gkept_fail = false;
#define GKEPT_STAT(i)	(sift_col_gkept_stat[i]++)
#else
#define GKEPT_STAT(i)	((void) 0)
#endif

/* An order bigger than this is made for its scan and not kept (the bound of a kept set of batches) */
#define GKEPT_BYTES		((Size) 16 << 20)

typedef struct GOrdKept
{
	MemoryContext cx;			/* everything of the entry; a child of gkept_cxt while kept, of the scan's context else */
	Oid			db;
	RelFileNumber rel;
	int			ntok;
	SiftCWbufToken *tok;
	int			leaf,
				gleaf;
	int64		gval;
	uint32		collid;
	ColGOrdMem *g;
	int			ncov;			/* the batches the rows of g came from and how many of their rows: only the rows after them are new */
	const SiftColBatch **cb;
	uint32	   *cn;
	uint32		nbuilt;			/* the rows when g was made from the batches, not extended: the ranges were cut for that many */
	uint64		used;
	int			refs;
	bool		kept;
} GOrdKept;

static MemoryContext gkept_cxt;

static GOrdKept *gkept[GKEPT_N];

static uint64 gkept_clock;

static void
gkept_drop(GOrdKept *k)
{
	for (int i = 0; i < GKEPT_N; i++)
		if (gkept[i] == k)
			gkept[i] = NULL;
	k->kept = false;
	GKEPT_STAT(3);
	if (k->refs == 0)
		MemoryContextDelete(k->cx);
}

static void
gkept_release(void *arg)
{
	GOrdKept   *k = arg;

	if (--k->refs == 0 && !k->kept)
		MemoryContextDelete(k->cx);
}

/* The scan holds k's order as its value v from now on */
static void
gkept_use(SiftColSource *src, GOrdKept *k, int v)
{
	MemoryContextCallback *cb = MemoryContextAlloc(src->mcxt, sizeof(MemoryContextCallback));

	*cb = (MemoryContextCallback) {gkept_release, k};
	MemoryContextRegisterResetCallback(src->mcxt, cb);
	k->refs++;
	k->used = ++gkept_clock;
	src->gmem[v] = k->g;
}

/* Whether the batches of the scan come from kept sets, so that their state names them */
static bool
gkept_named(const SiftColSource *src)
{
	return src->wtok != NULL && src->nwtok > 0 && src->nmem > 0;
}

static GOrdKept *
gkept_find(const SiftColSource *src, int leaf, int gleaf, int64 gval, uint32 collid)
{
	if (!gkept_named(src))
		return NULL;
	for (int i = 0; i < GKEPT_N; i++)
	{
		GOrdKept   *k = gkept[i];
		int			t;

		if (k == NULL || k->db != src->index->rd_locator.dbOid || k->rel != src->index->rd_locator.relNumber || k->leaf != leaf || k->gleaf != gleaf ||
			(gleaf >= 0 && k->gval != gval) || k->collid != collid || k->ntok != src->nwtok)
			continue;
		for (t = 0; t < k->ntok && k->tok[t].serial == src->wtok[t].serial; t++)
			;
		if (t == k->ntok)
			return k;
	}
	return NULL;
}

/*
 * k's order extended with the rows the sets gained since it was made: the new rows go into the ranges, which hold when every key falls in the range of keys the order was cut for, and
 * the rows are no more than twice those it was cut for. The old arrays stay as they were until the new are complete (an error leaves the order as it was). False: made again.
 */
static bool
gkept_extend(SiftColSource *src, GOrdKept *k)
{
	ColGOrdMem *g = k->g;
	uint32	   *cov = palloc0(src->nmem * sizeof(uint32));
	uint64		fresh = 0;
	GOrdPair   *pv,
			   *pn;
	uint32		nv,
				nn,
				nnull = g->n - g->nvals,
				newn;
	uint32	   *add,
			   *pos,
			   *nbstart;
	bool	   *nbdone;
	int64	   *nkey;
	uint64	   *ntid,
			   *nulls;
	GOrdPair   *nraw;
	const SiftColBatch **ncb;
	uint32	   *ncn;
	SiftCWbufToken *ntok;

	if (g->ent != NULL || g->str != NULL)
		return false;
	for (int i = 0; i < src->nmem; i++)
	{
		const SiftColBatch *b = src->mem[i].b;

		for (int j = 0; j < k->ncov; j++)
			if (k->cb[j] == b)
			{
				cov[i] = k->cn[j];
				break;
			}
		if (cov[i] > b->nrows)
			return false;
		fresh += b->nrows - cov[i];
	}
	if (fresh == 0 || g->n + fresh > 2 * (uint64) k->nbuilt + 1024)
		return fresh == 0;
	pv = palloc((fresh + 1) * sizeof(GOrdPair));
	pn = palloc((fresh + 1) * sizeof(GOrdPair));
	if (!gord_collect(src, k->leaf, k->gleaf, k->gval, cov, pv, &nv, pn, &nn))
		return false;
	add = palloc0(((Size) g->nb + 1) * sizeof(uint32));
	for (uint32 i = 0; i < nv; i++)
	{
		uint64		d;

		if (pv[i].key < g->kmin)
			return false;
		d = ((uint64) pv[i].key - (uint64) g->kmin) >> g->shift;
		if (g->nb == 0 || d >= g->nb)
			return false;
		add[d]++;
	}
	newn = g->n + nv + nn;
	nbstart = MemoryContextAllocZero(k->cx, ((Size) g->nb + 2) * sizeof(uint32));
	nbdone = MemoryContextAllocZero(k->cx, ((Size) g->nb + 1) * sizeof(bool));
	nraw = MemoryContextAlloc(k->cx, ((Size) g->nvals + nv + 1) * sizeof(GOrdPair));
	nkey = MemoryContextAllocZero(k->cx, ((Size) newn + 1) * sizeof(int64));
	ntid = MemoryContextAllocZero(k->cx, ((Size) newn + 1) * sizeof(uint64));
	pos = palloc(((Size) g->nb + 1) * sizeof(uint32));
	for (uint32 b = 0; b < g->nb; b++)
	{
		uint32		old = g->bstart[b + 1] - g->bstart[b];

		nbstart[b + 1] = nbstart[b] + old + add[b];
		pos[b] = nbstart[b];
		memcpy(nraw + pos[b], g->raw + g->bstart[b], old * sizeof(GOrdPair));
		pos[b] += old;
		/* a range that took no row keeps its sorted copy; one that did is sorted again when it is read */
		if (add[b] == 0 && g->bdone[b])
		{
			memcpy(nkey + nbstart[b], g->key + g->bstart[b], old * sizeof(int64));
			memcpy(ntid + nbstart[b], g->tid + g->bstart[b], old * sizeof(uint64));
			nbdone[b] = true;
		}
	}
	for (uint32 i = 0; i < nv; i++)
		nraw[pos[((uint64) pv[i].key - (uint64) g->kmin) >> g->shift]++] = pv[i];
	/* the addresses of the rows with no value, by address */
	nulls = palloc((nnull + nn + 1) * sizeof(uint64));
	memcpy(nulls, g->tid + g->nvals, nnull * sizeof(uint64));
	for (uint32 i = 0; i < nn; i++)
		nulls[nnull + i] = pn[i].tid;
	sift_sort_u64(nulls, nnull + nn);
	memcpy(ntid + g->nvals + nv, nulls, (nnull + nn) * sizeof(uint64));
#ifdef SIFT_TESTING
	if (sift_test_col_gkept_fail)
		elog(ERROR, "kept order of the write buffer: injected failure before the order takes its new arrays");	/* check-errors: allow */
#endif
	/* what can fail is done: the entry changes */
	ncb = MemoryContextAlloc(k->cx, src->nmem * sizeof(SiftColBatch *));
	ncn = MemoryContextAlloc(k->cx, src->nmem * sizeof(uint32));
	ntok = MemoryContextAlloc(k->cx, src->nwtok * sizeof(SiftCWbufToken));
	for (int i = 0; i < src->nmem; i++)
	{
		ncb[i] = src->mem[i].b;
		ncn[i] = src->mem[i].b->nrows;
	}
	memcpy(ntok, src->wtok, src->nwtok * sizeof(SiftCWbufToken));
	pfree(g->key);
	pfree(g->tid);
	pfree(g->raw);
	pfree(g->bstart);
	pfree(g->bdone);
	pfree(k->cb);
	pfree(k->cn);
	pfree(k->tok);
	g->key = nkey;
	g->tid = ntid;
	g->raw = nraw;
	g->bstart = nbstart;
	g->bdone = nbdone;
	g->nvals += nv;
	g->n = newn;
	k->cb = ncb;
	k->cn = ncn;
	k->ncov = src->nmem;
	k->tok = ntok;
	GKEPT_STAT(1);
	pfree(pv);
	pfree(pn);
	pfree(add);
	pfree(pos);
	pfree(nulls);
	pfree(cov);
	return true;
}

/* The kept order for the sets of the scan, extended when they gained rows; NULL: none (a kept order the sets left is dropped) */
static GOrdKept *
gkept_current(SiftColSource *src, int leaf, int gleaf, int64 gval, uint32 collid)
{
	GOrdKept   *k = gkept_find(src, leaf, gleaf, gval, collid);
	bool		same = k != NULL;

	for (int t = 0; same && t < k->ntok; t++)
		same = k->tok[t].fills == src->wtok[t].fills;
	if (k != NULL && same)
		GKEPT_STAT(0);
	if (k == NULL || same)
		return k;
	if (k->refs == 0 && gkept_extend(src, k))
	{
		if (MemoryContextMemAllocated(k->cx, true) > GKEPT_BYTES)
			gkept_drop(k);
		return k;
	}
	gkept_drop(k);
	return NULL;
}

/* The order just made in cx, as value v of the scan, kept for the next scan when it is not too big */
static void
gkept_store(SiftColSource *src, MemoryContext cx, int leaf, int gleaf, int64 gval, uint32 collid, int v)
{
	GOrdKept   *k;
	int			slot = 0;

	if (!gkept_named(src) || MemoryContextMemAllocated(cx, true) > GKEPT_BYTES)
		return;
	k = MemoryContextAllocZero(cx, sizeof(GOrdKept));
	k->cx = cx;
	k->db = src->index->rd_locator.dbOid;
	k->rel = src->index->rd_locator.relNumber;
	k->ntok = src->nwtok;
	k->tok = MemoryContextAlloc(cx, src->nwtok * sizeof(SiftCWbufToken));
	memcpy(k->tok, src->wtok, src->nwtok * sizeof(SiftCWbufToken));
	k->leaf = leaf;
	k->gleaf = gleaf;
	k->gval = gval;
	k->collid = collid;
	k->g = src->gmem[v];
	k->ncov = src->nmem;
	k->cb = MemoryContextAlloc(cx, src->nmem * sizeof(SiftColBatch *));
	k->cn = MemoryContextAlloc(cx, src->nmem * sizeof(uint32));
	k->nbuilt = k->g->n;
	for (int i = 0; i < src->nmem; i++)
	{
		k->cb[i] = src->mem[i].b;
		k->cn[i] = src->mem[i].b->nrows;
	}
	if (gkept_cxt == NULL)
		gkept_cxt = AllocSetContextCreate(TopMemoryContext, "sift cache write buffer orders", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSetParent(cx, gkept_cxt);
	/* the order this one replaces (the same key of the same index, made from sets that are gone) */
	for (int i = 0; i < GKEPT_N; i++)
		if (gkept[i] && gkept[i]->db == k->db && gkept[i]->rel == k->rel && gkept[i]->leaf == leaf && gkept[i]->gleaf == gleaf &&
			(gleaf < 0 || gkept[i]->gval == gval) && gkept[i]->collid == collid)
			gkept_drop(gkept[i]);
	for (int i = 0; i < GKEPT_N; i++)
		if (gkept[i] == NULL)
		{
			slot = i;
			break;
		}
		else if (gkept[i]->used < gkept[slot]->used)
			slot = i;
	if (gkept[slot])
		gkept_drop(gkept[slot]);
	gkept[slot] = k;
	k->kept = true;
	gkept_use(src, k, v);
}

/* The rows of the write buffers sorted like a segment's entries; false when a text key under a locale has more than maxrows rows, another key more than SIFT_CGORD_MEM_MAX, or one holds an overflow value of the leaf or an address a block of the global order cannot name */
bool
gord_mem(SiftColSource *src, int leaf, uint64 maxrows, int gleaf, int64 gval, int v, uint32 collid)
{
	ColGOrdMem *g = src->gmem[v];
	GOrdKept   *k;
	MemoryContext cx;
	bool		ok;

	/* the limit holds for an order that is kept as for one that is made: the buffer may have grown, or the read may allow fewer rows than the one that made it */
	if (gord_mem_rows(src) > (collid != 0 && !sift_collate_is_c(collid) ? maxrows : SIFT_CGORD_MEM_MAX))
	{
		src->gmem[v] = NULL;
		return false;
	}
	if (g != NULL && g->leaf == leaf && g->gleaf == gleaf && (gleaf < 0 || g->gval == gval) && (g->str != NULL) == (collid != 0))
		return true;
	if ((k = gkept_current(src, leaf, gleaf, gval, collid)) != NULL)
	{
		gkept_use(src, k, v);
		return true;
	}
	cx = AllocSetContextCreate(src->mcxt, "sift cache write buffer order", ALLOCSET_DEFAULT_SIZES);
	ok = collid != 0 ? gord_mem_text(src, leaf, collid, v, cx) : gord_mem_num(src, leaf, gleaf, gval, v, cx);
	if (!ok)
	{
		src->gmem[v] = NULL;
		MemoryContextDelete(cx);
		return false;
	}
	GKEPT_STAT(2);
	gkept_store(src, cx, leaf, gleaf, gval, collid, v);
	return true;
}

