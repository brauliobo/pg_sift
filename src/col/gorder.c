/*
 * col/gorder.c - the global order of a leaf in a segment (col/gorder.h). Portable (core/host.h only).
 */

#include "core/host.h"

#include <math.h>

#include "col/codec.h"
#include "col/gquant.h"
#include "col/gorder.h"
#include "col/point.h"
#include "col/runs.h"
#include "core/cmp.h"

#define GORD_ALIGN8(x)	(((uint64) (x) + 7) & ~(uint64) 7)

typedef struct OrdEnt
{
	int64		group;
	int64		key;
	uint64		code;			/* the address as sift_cpoint_tid_code: it sorts as the address does and takes 9 bits for the offset */
} OrdEnt;

static inline int
ord_cmp(const OrdEnt *x, const OrdEnt *y)
{
	if (x->group != y->group)
		return SIFT_CMP3(x->group, y->group);
	return x->key != y->key ? SIFT_CMP3(x->key, y->key) : SIFT_CMP3(x->code, y->code);
}

#define ORD_SORT_LESS(x, y) (ord_cmp(x, y) < 0)
SIFT_DEFINE_SORT(ord_sort_cmp, OrdEnt, ORD_SORT_LESS)

/* a copy of the entries for the radix sort: none when it would pass the largest allocation (the comparison sort then) */
static void *
ord_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

#define ORD_GROUP(x) ((x)->group)
#define ORD_KEY(x) ((x)->key)
#define ORD_CODE(x) ((x)->code)
SIFT_DEFINE_RADIX_SORT(ord_sort_by_code_order, OrdEnt, ORD_GROUP, ORD_KEY, ORD_CODE, ord_copy, pfree, ord_sort_cmp)

#define ORD_CODE_LESS(x, y) ((x)->code < (y)->code)
#define ORD_ZERO(x) ((int64) 0)
#define ORD_CODE_SIGNED(x) ((int64) ((x)->code ^ UINT64CONST(0x8000000000000000)))	/* the order of the unsigned code as a signed key */
SIFT_DEFINE_SORT(ord_code_cmp, OrdEnt, ORD_CODE_LESS)
SIFT_DEFINE_RADIX_SORT(ord_by_code, OrdEnt, ORD_CODE_SIGNED, ORD_ZERO, ORD_ZERO, ord_copy, pfree, ord_code_cmp)

/*
 * The entries by (group, key, address). The radix sort keeps the order of the entries it finds, so they must come in the order of the address: the rows of a segment cut by a key
 * do not, and the comparison sort that took their place was a tenth of such a build (1,000,000 rows). They are put in the order of the address by a radix sort of their own first.
 */
static void
ord_sort(OrdEnt *a, size_t n)
{
	for (size_t i = 1; i < n; i++)
		if (a[i].code < a[i - 1].code)
		{
			ord_by_code(a, n);
			break;
		}
	ord_sort_by_code_order(a, n);
}

bool
sift_cgord_leaf_text(const SiftColDef *d)
{
	return d->type == SIFT_COLT_TEXT && d->store == SIFT_COLS_STR;
}

bool
sift_cgord_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	if (!sift_crun_leaf_ok(s, d))
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return true;
		default:
			return false;
	}
}

bool
sift_cgord_leaf_array_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return sift_cgord_leaf_ok(s, d) ||
		(d->kind == SIFT_COLK_SCALAR && d->parent < 0 && (d->path == NULL || d->path[0] == '\0') && !(d->flags & SIFT_COLD_LOSSY) && sift_cgord_leaf_text(d));
}

bool
sift_cgord_leaf_time(const SiftColDef *d)
{
	return d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ;
}

static void
pad8(StringInfo out)
{
	while (out->len % 8)
		appendStringInfoChar(out, '\0');
}

/* the runs of equal group in ent[from, from + n): the groups, with the first entry and the number of the run, appended to out in the order of the entries */
static uint32
group_runs(const OrdEnt *ent, uint32 from, uint32 n, SiftCGOrdGrp *out, bool nulls)
{
	uint32		m = 0;

	for (uint32 i = 0; i < n;)
	{
		uint32		j = i + 1;

		while (j < n && ent[from + j].group == ent[from + i].group)
			j++;
		out[m].group = ent[from + i].group;
		if (nulls)
		{
			out[m].nstart = from + i;
			out[m].ncount = j - i;
		}
		else
		{
			out[m].vstart = from + i;
			out[m].vcount = j - i;
		}
		m++;
		i = j;
	}
	return m;
}

/*
 * The addresses of a chunk (sorted by key, then address). A dense chunk (dir.pad != 0) codes an address as block << ob | offset, ob the bits of the largest offset
 * of the chunk, not the 9 of the point directory: 5 bits for 18 rows a page. Where a key repeats it also writes the first address of each run of equal keys
 * and, as a second blob, the gaps to the address before it: {u32 bytes of the first blob, u32 runs, the first blobs padded to 8, the gaps blob}.
 */
static void
addr_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, bool runs, const SiftCcPolicy *pol)
{
	int64	   *dv = palloc(m * sizeof(int64)),
			   *fv = palloc(m * sizeof(int64)),
			   *gv = palloc(m * sizeof(int64));
	uint64		maxoff = 0;
	uint32		nf = 0,
				ob,
				k0 = body->len;
	bool		ascend = runs;
	StringInfoData one,
				first,
				gaps;

	for (uint32 i = 0; i < m; i++)
		maxoff = Max(maxoff, (uint64) av[i] & (SIFT_CPOINT_TID_MAXOFF - 1));
	ob = Max(1, 64 - __builtin_clzll(maxoff | 1));
	for (uint32 i = 0; i < m; i++)
		dv[i] = (int64) (((uint64) av[i] >> 9) << ob | ((uint64) av[i] & (SIFT_CPOINT_TID_MAXOFF - 1)));
	for (uint32 i = 0; runs && i < m; i++)
	{
		if (i == 0 || kv[i] != kv[i - 1])
			fv[nf++] = dv[i];
		else if (dv[i] > dv[i - 1])
			gv[i - nf] = dv[i] - dv[i - 1] - 1;
		else
			ascend = false;
	}
	initStringInfo(&one);
	sift_cc_encode(&one, SIFT_CC_INT, dv, m, NULL, pol, NULL);
	d->pad = ob << 8;
	d->alen = one.len;
	if (ascend && nf < m)
	{
		uint32		hdr[2];

		initStringInfo(&first);
		initStringInfo(&gaps);
		sift_cc_encode(&first, SIFT_CC_INT, fv, nf, NULL, pol, NULL);
		sift_cc_encode(&gaps, SIFT_CC_INT, gv, m - nf, NULL, pol, NULL);
		if (sizeof(hdr) + GORD_ALIGN8(first.len) + gaps.len < (Size) one.len)
		{
			hdr[0] = first.len;
			hdr[1] = nf;
			appendBinaryStringInfo(body, (const char *) hdr, sizeof(hdr));
			appendBinaryStringInfo(body, first.data, first.len);
			pad8(body);
			appendBinaryStringInfo(body, gaps.data, gaps.len);
			d->pad |= SIFT_CGORD_GAPS;
			d->alen = body->len - k0;
		}
		pfree(first.data);
		pfree(gaps.data);
	}
	if (!(d->pad & SIFT_CGORD_GAPS))
		appendBinaryStringInfo(body, one.data, one.len);
	pfree(one.data);
	pfree(dv);
	pfree(fv);
	pfree(gv);
}

/* The chunk of m entries as a compact one: the quantized buckets of the keys and the plain addresses */
static void
compact_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, const SiftCcPolicy *pol)
{
	uint32		k0 = body->len;

	sift_gq_encode(kv, m, SIFT_GQ_Q, body);
	d->klen = body->len - k0;
	pad8(body);
	addr_chunk(body, d, kv, av, m, false, pol);
	d->pad |= SIFT_CGORD_COMPACT;
	pad8(body);
}

/* ... and as a whole one: the keys, and the addresses (as runs of equal keys where dense) */
static void
whole_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, bool dense, const SiftCcPolicy *pol)
{
	uint32		k0 = body->len;

	sift_cc_encode(body, SIFT_CC_INT, (int64 *) kv, m, NULL, pol, NULL);
	d->klen = body->len - k0;
	pad8(body);
	k0 = body->len;
	if (dense)
		addr_chunk(body, d, kv, av, m, true, pol);
	else
	{
		sift_cc_encode(body, SIFT_CC_INT, (int64 *) av, m, NULL, pol, NULL);
		d->alen = body->len - k0;
	}
	pad8(body);
}

/* The longest run of equal keys of the chunk (sorted) */
static uint32
longest_run(const int64 *kv, uint32 m)
{
	uint32		best = 0,
				run = 0;

	for (uint32 i = 0; i < m; i++)
	{
		run = i > 0 && kv[i] == kv[i - 1] ? run + 1 : 1;
		best = Max(best, run);
	}
	return best;
}

/*
 * Writes the chunk compact when that is at least a fifth smaller than the whole one and has no run of SIFT_CGORD_TIE_RUN equal keys, the whole one otherwise; true when it is the compact
 * one. A run of equal keys is cheap to address as a run, which a compact chunk has no form for, and a bound that falls on it makes the compact chunk ask the heap for the exact key of
 * every row of the run (a table of half unique timestamps and half 50 days read up to 4 times the blocks of the whole order). d has the first key and the offset of the chunk.
 */
static bool
compact_smaller(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, bool dense, const SiftCcPolicy *pol)
{
	StringInfoData cb,
				wb;
	SiftCGOrdDir cd = *d,
				wd = *d;
	bool		smaller;

	initStringInfo(&cb);
	initStringInfo(&wb);
	compact_chunk(&cb, &cd, kv, av, m, pol);
	whole_chunk(&wb, &wd, kv, av, m, dense, pol);
	smaller = (uint64) cb.len * 5 <= (uint64) wb.len * 4 && longest_run(kv, m) < SIFT_CGORD_TIE_RUN;
	*d = smaller ? cd : wd;
	appendBinaryStringInfo(body, smaller ? cb.data : wb.data, smaller ? cb.len : wb.len);
	pfree(cb.data);
	pfree(wb.data);
	return smaller;
}

static void
build_block(OrdEnt *ent, uint32 nvals, uint32 nnull, int gcol, bool grouped, bool dense, StringInfo out, uint32 collid, const StringInfoData *strs, uint32 head, uint32 vtotal, int layout)
{
	bool		compact = layout != SIFT_CGORD_L_WHOLE,
				anycompact = false;
	uint64		n = (uint64) nvals + nnull;
	uint32		crows = sift_cgord_rows();
	uint32		nchunks = (uint32) ((n + crows - 1) / crows);
	int64	   *kv = palloc(SIFT_CGORD_CAP * sizeof(int64)),
			   *av = palloc(SIFT_CGORD_CAP * sizeof(int64));
	StringInfoData body;
	SiftCGOrdDir *dir = palloc0((nchunks + 1) * sizeof(SiftCGOrdDir));
	SiftCGOrdGrp *gv = NULL,
			   *gn = NULL,
			   *gs = NULL;
	uint32		ngv = 0,
				ngn = 0,
				ng = 0;
	uint32		hdr[8] = {SIFT_CGORD_MAGIC, strs != NULL ? SIFT_CGORD_VERSION_TEXT : SIFT_CGORD_VERSION, (uint32) n, nvals, nchunks, crows, head, vtotal};
	SiftCcPolicy pol;

	if (compact)
	{
		hdr[1] = SIFT_CGORD_VERSION_COMPACT;
		hdr[6] = SIFT_GQ_Q;
		hdr[7] = 0;
	}

	if (strs != NULL)
	{
		hdr[6] = collid;
		hdr[7] = (uint32) strs->len;
	}
	ord_sort(ent, nvals);
	ord_sort(ent + nvals, nnull);
	if (grouped)
	{
		uint32		i = 0,
					j = 0;

		gv = palloc((nvals + 1) * sizeof(SiftCGOrdGrp));
		gn = palloc((nnull + 1) * sizeof(SiftCGOrdGrp));
		gs = palloc0((nvals + nnull + 1) * sizeof(SiftCGOrdGrp));
		memset(gv, 0, (nvals + 1) * sizeof(SiftCGOrdGrp));
		memset(gn, 0, (nnull + 1) * sizeof(SiftCGOrdGrp));
		ngv = group_runs(ent, 0, nvals, gv, false);
		ngn = group_runs(ent, nvals, nnull, gn, true);
		/* the runs of the two regions merged by group */
		while (i < ngv || j < ngn)
		{
			if (j >= ngn || (i < ngv && gv[i].group < gn[j].group))
				gs[ng++] = gv[i++];
			else if (i >= ngv || gn[j].group < gv[i].group)
				gs[ng++] = gn[j++];
			else
			{
				gs[ng] = gv[i++];
				gs[ng].nstart = gn[j].nstart;
				gs[ng++].ncount = gn[j++].ncount;
			}
		}
		/* a group with no run in a region starts where that region's runs of the groups before it end */
		{
			uint32		vnext = 0,
						nnext = nvals;

			for (uint32 k = 0; k < ng; k++)
			{
				if (gs[k].vcount == 0)
					gs[k].vstart = vnext;
				if (gs[k].ncount == 0)
					gs[k].nstart = nnext;
				vnext = gs[k].vstart + gs[k].vcount;
				nnext = gs[k].nstart + gs[k].ncount;
			}
		}
		hdr[1] = SIFT_CGORD_VERSION_GROUP;
		hdr[6] = ng;
		hdr[7] = (uint32) gcol + 1;
	}
	sift_cc_policy_default(&pol);
	initStringInfo(&body);
	for (uint32 c = 0; c < nchunks; c++)
	{
		uint32		m = (uint32) Min((uint64) crows, n - (uint64) c * crows);
		uint64		at = (uint64) c * crows;

		for (uint32 i = 0; i < m; i++)
		{
			kv[i] = ent[at + i].key;
			av[i] = (int64) ent[at + i].code;
		}
		dir[c].first = kv[0];
		dir[c].off = (uint32) body.len;		/* made absolute below */
		if (compact && at >= 2 * sift_cgord_ends() && at + m + 2 * sift_cgord_ends() <= nvals)
		{
			if (layout == SIFT_CGORD_L_COMPACT)
			{
				compact_chunk(&body, &dir[c], kv, av, m, &pol);
				if (longest_run(kv, m) >= SIFT_CGORD_TIE_RUN)
					dir[c].pad |= SIFT_CGORD_TIES;
				anycompact = true;
			}
			else
				anycompact = compact_smaller(&body, &dir[c], kv, av, m, dense, &pol) || anycompact;
			continue;
		}
		whole_chunk(&body, &dir[c], kv, av, m, dense, &pol);
	}
	if (compact && !anycompact)
	{
		/* no chunk is smaller compact: the block is the whole order, of the version a library before the compact blocks reads */
		hdr[1] = SIFT_CGORD_VERSION;
		hdr[6] = head;
		hdr[7] = vtotal;
	}
	appendBinaryStringInfo(out, (const char *) hdr, sizeof(hdr));
	for (uint32 c = 0; c < nchunks; c++)
	{
		dir[c].off += SIFT_CGORD_HDR + nchunks * SIFT_CGORD_DIRENT;
		appendBinaryStringInfo(out, (const char *) &dir[c], SIFT_CGORD_DIRENT);
	}
	appendBinaryStringInfo(out, body.data, body.len);
	pad8(out);
	if (strs != NULL)
	{
		/* version 3: the strings end the block */
		appendBinaryStringInfo(out, strs->data, strs->len);
	}
	if (grouped)
	{
		appendBinaryStringInfo(out, (const char *) gs, ng * sizeof(SiftCGOrdGrp));
		pfree(gv);
		pfree(gn);
		pfree(gs);
	}
	pfree(kv);
	pfree(av);
	pfree(dir);
	pfree(body.data);
}

#ifdef SIFT_TESTING
int			sift_test_col_gorder_rows = 0;

int			sift_test_col_gorder_ends = 0;

int			sift_test_col_gorder_choice = 0;

bool		sift_test_col_gorder_noskip = false;
#endif

/* the bits of x >= 1, as a real: log2 within a few percent, which is what a size estimate needs */
static double
lg2(double x)
{
	double		b = 0;

	if (x < 1)
		return 0;
	for (; x >= 2; x /= 2)
		b++;
	return b + (x - 1);
}

void
sift_cgord_pay(const SiftCGOrdZones *z, SiftCGOrdPay *out)
{
	double		units = 0,
				blocks = 0,
				nv = 0;
	int			probes = 0;
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	memset(out, 0, sizeof(*out));
	for (uint32 u = 0; u < z->nsb; u++)
		if (z->nvals[u] > 0)
		{
			lo = Min(lo, z->min[u]);
			hi = Max(hi, z->max[u]);
			nv += z->nvals[u];
		}
	if (nv == 0)
		return;
	for (int t = 0; t < SIFT_CGORD_PAY_PROBES; t++)
	{
		uint32		u0 = (uint32) (((2 * (uint64) t + 1) * z->nsb) / (2 * SIFT_CGORD_PAY_PROBES));
		int64		b;
		double		n = 0,
					bl = 0;

		while (u0 < z->nsb && z->nvals[u0] == 0)
			u0++;
		if (u0 >= z->nsb)
			continue;
		b = z->min[u0] / 2 + z->max[u0] / 2;
		for (uint32 u = 0; u < z->nsb; u++)
			if (z->nvals[u] > 0 && z->min[u] <= b && b <= z->max[u])
			{
				double		width = (double) z->max[u] - (double) z->min[u] + 1;

				n++;
				bl += SIFT_CGORD_PAY_UNIT_BLOCKS + z->nvals[u] * Max(lg2(width), 1.0) / 8.0 / 8192.0 * SIFT_CGORD_PAY_UNIT_BITS;
			}
		units += n;
		blocks += bl;
		probes++;
	}
	if (probes == 0)
		return;
	out->units = units / probes;
	out->blocks = blocks / probes;
	out->saved = out->blocks * SIFT_CGORD_PAY_BLOCK;
	{
		double		range = (double) hi - (double) lo,
					dup = range + 1 < nv ? nv / (range + 1) : 1,
					bits = lg2(1 + range / nv) + 2 + Max(lg2((double) Max(z->rows, 2)) + 2.5 - lg2(dup), 1.0);

		out->bytes = nv * bits / 8.0;
	}
	out->complete = out->saved > out->bytes;
}

/* the entries kept at each end of a block of the ends only */
uint32
sift_cgord_ends(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_gorder_ends > 0)
		return (uint32) sift_test_col_gorder_ends;
#endif
	return SIFT_CGORD_ENDS;
}

/* the entries of the chunks a block is written with */
uint32
sift_cgord_rows(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_gorder_rows == SIFT_CGORD_ROWS)
		return SIFT_CGORD_ROWS;
#endif
	return SIFT_CGORD_ROWS_FINE;
}

/*
 * The ends of a sorted block: the first head entries and the last ones, neither end splitting a run of equal keys (a run that crosses the cut is left out whole), and the
 * first SIFT_CGORD_ENDS NULL rows; they follow one another in ent. vtotal is the values the segment has.
 */
static void
ends_reduce(OrdEnt *ent, uint32 *nvals, uint32 *nnull, uint32 *head, uint32 *vtotal)
{
	uint32		k = sift_cgord_ends(),
				nv = *nvals,
				h = k,
				s = nv - k,
				nn = Min(*nnull, k);

	while (h > 0 && ent[h - 1].key == ent[h].key)
		h--;
	while (s < nv && ent[s - 1].key == ent[s].key)
		s++;
	memmove(ent + h, ent + s, (nv - s) * sizeof(OrdEnt));
	memmove(ent + h + (nv - s), ent + nv, nn * sizeof(OrdEnt));
	*vtotal = nv;
	*head = h;
	*nvals = h + (nv - s);
	*nnull = nn;
}

void
sift_cgord_build(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, bool dense, bool ends, int layout, StringInfo out)
{
	OrdEnt	   *ent = palloc(((uint64) nvals + nnull + 1) * sizeof(OrdEnt));
	uint32		head = 0,
				vtotal = 0;

	for (uint32 i = 0; i < nvals; i++)
		ent[i] = (OrdEnt) {0, key[i], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nnull; i++)
		ent[nvals + i] = (OrdEnt) {0, 0, sift_cpoint_tid_code(ntid[i])};
	if (ends && nvals > 2 * sift_cgord_ends())
	{
		ord_sort(ent, nvals);
		ord_sort(ent + nvals, nnull);
		ends_reduce(ent, &nvals, &nnull, &head, &vtotal);
	}
	build_block(ent, nvals, nnull, 0, false, dense, out, 0, NULL, head, vtotal, dense && vtotal == 0 && nvals > 8 * sift_cgord_ends() && sift_cgord_rows() == SIFT_CGORD_ROWS_FINE ? layout : SIFT_CGORD_L_WHOLE);
	pfree(ent);
}

void
sift_cgord_build_grouped(int64 *key, uint64 *tid, int64 *group, uint32 nvals, uint64 *ntid, int64 *ngroup, uint32 nnull, int gcol, bool dense, StringInfo out)
{
	OrdEnt	   *ent = palloc(((uint64) nvals + nnull + 1) * sizeof(OrdEnt));

	for (uint32 i = 0; i < nvals; i++)
		ent[i] = (OrdEnt) {group[i], key[i], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nnull; i++)
		ent[nvals + i] = (OrdEnt) {ngroup[i], 0, sift_cpoint_tid_code(ntid[i])};
	build_block(ent, nvals, nnull, gcol, true, dense, out, 0, NULL, 0, 0, SIFT_CGORD_L_WHOLE);
	pfree(ent);
}

SiftCStrCmp sift_cgord_strcmp = NULL;

static int
str_cmp_bytes(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	int			c = memcmp(a, b, Min(la, lb));

	return c != 0 ? c : SIFT_CMP3(la, lb);
}

int
sift_cgord_str_order(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	int			c = (sift_cgord_strcmp != NULL ? sift_cgord_strcmp : str_cmp_bytes) (a, la, b, lb, collid);

	return c != 0 ? c : str_cmp_bytes(a, la, b, lb, 0);
}

static const SiftCDict *rank_dict;
static uint32 rank_coll;

/* the codes in the order of their strings */
static int
rank_cmp(const void *pa, const void *pb)
{
	uint32		la,
				lb;
	const char *a = sift_cdict_str(rank_dict, *(const uint32 *) pa, &la),
			   *b = sift_cdict_str(rank_dict, *(const uint32 *) pb, &lb);

	return sift_cgord_str_order(a, la, b, lb, rank_coll);
}

static void
put_varint(StringInfo out, uint32 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(out, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(out, (char) v);
}

void
sift_cgord_build_text(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, const SiftCDict *d, uint32 collid, bool dense, StringInfo out)
{
	uint32		nstr = d->n,
				nblk = (nstr + SIFT_CGORD_STR_BLK - 1) / SIFT_CGORD_STR_BLK;
	uint32	   *order = palloc((nstr + 1) * sizeof(uint32)),
			   *rank = palloc((nstr + 1) * sizeof(uint32)),
			   *boff = palloc((nblk + 1) * sizeof(uint32));
	OrdEnt	   *ent = palloc(((uint64) nvals + nnull + 1) * sizeof(OrdEnt));
	StringInfoData body,
				strs;
	uint32		hdr[4] = {nstr, SIFT_CGORD_STR_BLK, nblk, 0};
	const char *prev = NULL;
	uint32		plen = 0;

	for (uint32 i = 0; i < nstr; i++)
		order[i] = i;
	rank_dict = d;
	rank_coll = collid;
	qsort(order, nstr, sizeof(uint32), rank_cmp);
	for (uint32 r = 0; r < nstr; r++)
		rank[order[r]] = r;
	initStringInfo(&body);
	for (uint32 r = 0; r < nstr; r++)
	{
		uint32		len;
		const char *s = sift_cdict_str(d, order[r], &len);
		uint32		shared = 0;

		if (r % SIFT_CGORD_STR_BLK == 0)
			boff[r / SIFT_CGORD_STR_BLK] = (uint32) body.len;
		else
			while (shared < Min(len, plen) && s[shared] == prev[shared])
				shared++;
		put_varint(&body, shared);
		put_varint(&body, len - shared);
		appendBinaryStringInfo(&body, s + shared, len - shared);
		prev = s;
		plen = len;
	}
	boff[nblk] = (uint32) body.len;
	initStringInfo(&strs);
	appendBinaryStringInfo(&strs, (const char *) hdr, sizeof(hdr));
	{
		/* the offsets are from the start of the section */
		uint32		base = sizeof(hdr) + (nblk + 1) * sizeof(uint32);

		for (uint32 b = 0; b <= nblk; b++)
		{
			uint32		o = boff[b] + base;

			appendBinaryStringInfo(&strs, (const char *) &o, sizeof(o));
		}
	}
	appendBinaryStringInfo(&strs, body.data, body.len);
	pad8(&strs);
	for (uint32 i = 0; i < nvals; i++)
		ent[i] = (OrdEnt) {0, rank[key[i]], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nnull; i++)
		ent[nvals + i] = (OrdEnt) {0, 0, sift_cpoint_tid_code(ntid[i])};
	build_block(ent, nvals, nnull, 0, false, dense, out, collid, &strs, 0, 0, SIFT_CGORD_L_WHOLE);
	pfree(order);
	pfree(rank);
	pfree(boff);
	pfree(ent);
	pfree(body.data);
	pfree(strs.data);
}

bool
sift_cgord_str_head(const uint8 *sec, const SiftCGOrdHdr *h, uint32 *nstr, uint32 *nblk)
{
	uint32		w[4];

	if (h->strbytes < sizeof(w))
		return false;
	memcpy(w, sec, sizeof(w));
	if (w[1] != SIFT_CGORD_STR_BLK || w[2] != (w[0] + w[1] - 1) / w[1] || w[3] != 0 || (uint64) sizeof(w) + ((uint64) w[2] + 1) * sizeof(uint32) > h->strbytes)
		return false;
	*nstr = w[0];
	*nblk = w[2];
	return true;
}

bool
sift_cgord_str_get(const uint8 *b, uint32 len, SiftCStrPos *st, uint32 idx, char *out, uint32 *slen)
{
	uint32		pos,
				olen;

	/* the string before idx is out's: a walk in rank order decodes each string once (a block of 16 would otherwise be decoded from its start for every one) */
	if (idx + 1 < st->next)
		*st = (SiftCStrPos) {0, 0, 0};
	pos = st->pos;
	olen = st->olen;
	for (uint32 i = st->next; i <= idx; i++)
	{
		uint32		shared = 0,
					own = 0;
		int			sh = 0;

		for (;;)
		{
			if (pos >= len)
				return false;
			shared |= (uint32) (b[pos] & 0x7F) << sh;
			sh += 7;
			if (!(b[pos++] & 0x80))
				break;
			if (sh > 28)
				return false;
		}
		sh = 0;
		for (;;)
		{
			if (pos >= len)
				return false;
			own |= (uint32) (b[pos] & 0x7F) << sh;
			sh += 7;
			if (!(b[pos++] & 0x80))
				break;
			if (sh > 28)
				return false;
		}
		if (shared > olen || (uint64) shared + own >= SIFT_CGORD_STR_MAX || (uint64) pos + own > len)
			return false;
		memcpy(out + shared, b + pos, own);
		pos += own;
		olen = shared + own;
		*st = (SiftCStrPos) {i + 1, pos, olen};
	}
	*slen = olen;
	return true;
}

bool
sift_cgord_head(const uint8 *blk, uint64 len, SiftCGOrdHdr *h)
{
	uint32		w[8];
	bool		grouped,
				text;

	if (len < SIFT_CGORD_HDR)
		return false;
	memcpy(w, blk, sizeof(w));
	grouped = w[1] == SIFT_CGORD_VERSION_GROUP;
	text = w[1] == SIFT_CGORD_VERSION_TEXT;
	if (w[1] == SIFT_CGORD_VERSION_COMPACT)
	{
		if (w[0] != SIFT_CGORD_MAGIC || w[3] > w[2] || w[5] != SIFT_CGORD_ROWS_FINE || w[6] < 1 || w[6] > 16 || w[7] != 0 ||
			w[4] != (uint32) (((uint64) w[2] + w[5] - 1) / w[5]) || SIFT_CGORD_HDR + (uint64) w[4] * SIFT_CGORD_DIRENT > len)
			return false;
		*h = (SiftCGOrdHdr) {w[2], w[3], w[4], w[5], 0, -1, 0, 0, 0, 0, w[6]};
		return true;
	}
	if (w[0] != SIFT_CGORD_MAGIC || (w[1] != SIFT_CGORD_VERSION && !grouped && !text) || w[3] > w[2] || (w[5] != SIFT_CGORD_ROWS && w[5] != SIFT_CGORD_ROWS_FINE) ||
		(grouped ? (w[6] == 0 || w[7] == 0 || w[7] > SIFT_COL_MAX_COLS) : text ? (w[7] == 0 || w[7] % 8 != 0) : (w[7] == 0 ? w[6] != 0 : (w[6] > w[3] || w[7] <= w[3] || w[3] - w[6] > SIFT_CGORD_ENDS || w[6] > SIFT_CGORD_ENDS || w[2] - w[3] > SIFT_CGORD_ENDS))) ||
		w[4] != (uint32) (((uint64) w[2] + w[5] - 1) / w[5]) ||
		SIFT_CGORD_HDR + (uint64) w[4] * SIFT_CGORD_DIRENT + (uint64) (text ? w[7] : grouped ? (uint64) w[6] * SIFT_CGORD_GRPENT : 0) > len)
		return false;
	*h = (SiftCGOrdHdr) {w[2], w[3], w[4], w[5], grouped ? w[6] : 0, grouped ? (int) w[7] - 1 : -1, text ? w[6] : 0, text ? w[7] : 0, !grouped && !text ? w[6] : 0, !grouped && !text ? w[7] : 0, 0};
	return true;
}

bool
sift_cgord_groups(const uint8 *blk, const SiftCGOrdHdr *h, uint64 len, SiftCGOrdGrp *out)
{
	uint32		vnext = 0,
				nnext = h->values;

	memcpy(out, blk + len - (uint64) h->ngroups * SIFT_CGORD_GRPENT, (Size) h->ngroups * SIFT_CGORD_GRPENT);
	for (uint32 i = 0; i < h->ngroups; i++)
	{
		const SiftCGOrdGrp *g = &out[i];

		if ((i > 0 && g->group <= out[i - 1].group) || g->vstart != vnext || g->nstart != nnext || (g->vcount == 0 && g->ncount == 0) ||
			(uint64) g->vstart + g->vcount > h->values || (uint64) g->nstart + g->ncount > h->entries)
			return false;
		vnext += g->vcount;
		nnext += g->ncount;
	}
	return vnext == h->values && nnext == h->entries;
}

bool
sift_cgord_dirent(const uint8 *ent, const SiftCGOrdHdr *h, uint64 len, uint32 c, SiftCGOrdDir *d)
{
	if (c >= h->chunks)
		return false;
	memcpy(d, ent, sizeof(*d));
	return (d->pad == 0 || ((d->pad & ~(SIFT_CGORD_GAPS | SIFT_CGORD_COMPACT | SIFT_CGORD_TIES | 0xF00u)) == 0 && !((d->pad & SIFT_CGORD_COMPACT) && ((d->pad & SIFT_CGORD_GAPS) || h->quant == 0)) && (!(d->pad & SIFT_CGORD_TIES) || (d->pad & SIFT_CGORD_COMPACT)) && (d->pad >> 8) >= 1 && (d->pad >> 8) <= 9)) && d->off % 8 == 0 && (uint64) d->off + (((uint64) d->klen + 7) & ~(uint64) 7) + (((uint64) d->alen + 7) & ~(uint64) 7) <= len &&
		d->off >= SIFT_CGORD_HDR + (uint64) h->chunks * SIFT_CGORD_DIRENT;
}

bool
sift_cgord_decode(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, uint32 w, int64 *key, uint64 *addr)
{
	SiftCc	   *k = sift_cc_open_rows(mcxt, kb, klen, false, m);
	uint32		ob = pad ? pad >> 8 & 0xF : 9;

	w = Min(w, m);
	sift_cc_decode_head(k, w, key);
	sift_cc_close(k);
	if (pad & SIFT_CGORD_GAPS)
	{
		uint32		hdr[2],
					flen,
					nf,
					f = 0,
					fi = 0,
					gi = 0;
		int64	   *fv,
				   *gv;
		SiftCc	   *fc,
				   *gc;
		uint64		prev = 0;

		if (alen < sizeof(hdr))
			return false;
		memcpy(hdr, ab, sizeof(hdr));
		flen = hdr[0];
		nf = hdr[1];
		if (nf == 0 || nf >= m || (uint64) sizeof(hdr) + GORD_ALIGN8(flen) > alen)
			return false;
		fc = sift_cc_open_rows(mcxt, ab + sizeof(hdr), flen, false, nf);
		gc = sift_cc_open_rows(mcxt, ab + sizeof(hdr) + GORD_ALIGN8(flen), alen - sizeof(hdr) - GORD_ALIGN8(flen), false, m - nf);
		for (uint32 i = 0; i < w; i++)
			f += i == 0 || key[i] != key[i - 1];
		fv = palloc((Size) (f + 1) * sizeof(int64));
		gv = palloc((Size) (w - f + 1) * sizeof(int64));
		sift_cc_decode_head(fc, f, fv);
		sift_cc_decode_head(gc, w - f, gv);
		for (uint32 i = 0; i < w; i++)
		{
			prev = i == 0 || key[i] != key[i - 1] ? (uint64) fv[fi++] : prev + (uint64) gv[gi++] + 1;
			addr[i] = (prev >> ob) << 16 | (prev & (((uint64) 1 << ob) - 1));
		}
		pfree(fv);
		pfree(gv);
		sift_cc_close(fc);
		sift_cc_close(gc);
		return true;
	}
	{
		SiftCc	   *a = sift_cc_open_rows(mcxt, ab, alen, false, m);

		sift_cc_decode_head(a, w, (int64 *) addr);
		for (uint32 i = 0; i < w; i++)
			addr[i] = (addr[i] >> ob) << 16 | (addr[i] & (((uint64) 1 << ob) - 1));
		sift_cc_close(a);
	}
	return true;
}

bool
sift_cgord_decode_compact(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first, uint32 w, int64 *kmin, uint32 *sh, uint64 *addr)
{
	uint32		ob = pad >> 8 & 0xF;
	SiftCc	   *a;

	if (kmin != NULL && !sift_gq_decode(kb, klen, m, first, kmin, sh))
		return false;
	w = Min(w, m);
	a = sift_cc_open_rows(mcxt, ab, alen, false, m);
	sift_cc_decode_head(a, w, (int64 *) addr);
	for (uint32 i = 0; i < w; i++)
		addr[i] = (addr[i] >> ob) << 16 | (addr[i] & (((uint64) 1 << ob) - 1));
	sift_cc_close(a);
	return true;
}
