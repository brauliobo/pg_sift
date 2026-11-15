/*
 * col/gorder.c - the global order of a leaf in a segment (col/gorder.h). Portable (core/host.h only).
 */

#include "core/rowmem.h"
#include "col/codec.h"
#include "col/enc_int.h"
#include "col/gquant.h"
#include "col/gorder.h"
#include "col/point.h"
#include "col/runs.h"
#include "core/cmp.h"
#include "core/limits.h"

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

/* a numeric(p, s) of up to 18 digits: its key is the number scaled by 10^s, an integer in the order of the numbers (a NaN or an infinity is an overflow row, which leaves the segment without an array) */
bool
sift_cgord_leaf_numeric(const SiftColSchema *s, const SiftColDef *d)
{
	return d->type == SIFT_COLT_NUMERIC && sift_crun_leaf_ok(s, d);
}

bool
sift_cgord_leaf_array_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return sift_cgord_leaf_ok(s, d) || sift_cgord_leaf_numeric(s, d) ||
		(d->kind == SIFT_COLK_SCALAR && d->parent < 0 && (d->path == NULL || d->path[0] == '\0') && !(d->flags & SIFT_COLD_LOSSY) && sift_cgord_leaf_text(d));
}

bool
sift_cgord_leaf_time(const SiftColDef *d)
{
	return d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ;
}

void
sift_cgord_pad8(StringInfo out)
{
	while (out->len % 8)
		appendStringInfoChar(out, '\0');
}

/* the runs of equal group in ent[from, from + n): the groups, with the first entry and the number of the run, appended to out in the order of the entries (out NULL: only counted) */
static uint32
group_runs(const OrdEnt *ent, uint32 from, uint32 n, SiftCGOrdGrp *out, bool nulls)
{
	uint32		m = 0;

	for (uint32 i = 0; i < n;)
	{
		uint32		j = i + 1;

		while (j < n && ent[from + j].group == ent[from + i].group)
			j++;
		if (out != NULL)
		{
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
		}
		m++;
		i = j;
	}
	return m;
}

/* The codes of the addresses of a chunk, block << ob | offset into dv, and ob: the bits of the largest offset of the chunk, not the 9 of the point directory */
uint32
sift_cgord_addr_codes(const int64 *av, uint32 m, int64 *dv)
{
	uint64		maxoff = 0;
	uint32		ob;

	for (uint32 i = 0; i < m; i++)
		maxoff = Max(maxoff, (uint64) av[i] & (SIFT_CPOINT_TID_MAXOFF - 1));
	ob = Max(1, 64 - __builtin_clzll(maxoff | 1));
	for (uint32 i = 0; i < m; i++)
		dv[i] = (int64) (((uint64) av[i] >> 9) << ob | ((uint64) av[i] & (SIFT_CPOINT_TID_MAXOFF - 1)));
	return ob;
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
	uint32		nf = 0,
				ob = sift_cgord_addr_codes(av, m, dv),
				k0 = body->len;
	bool		ascend = runs;
	StringInfoData one,
				first,
				gaps;

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
			sift_cgord_pad8(body);
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
	sift_cgord_pad8(body);
	addr_chunk(body, d, kv, av, m, false, pol);
	d->pad |= SIFT_CGORD_COMPACT;
	sift_cgord_pad8(body);
}

/* ... and as a whole one: the keys, and the addresses (as runs of equal keys where dense) */
static void
plain_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, bool dense, const SiftCcPolicy *pol)
{
	uint32		k0 = body->len;

	sift_cc_encode(body, SIFT_CC_INT, (int64 *) kv, m, NULL, pol, NULL);
	d->klen = body->len - k0;
	sift_cgord_pad8(body);
	k0 = body->len;
	if (dense)
		addr_chunk(body, d, kv, av, m, true, pol);
	else
	{
		sift_cc_encode(body, SIFT_CC_INT, (int64 *) av, m, NULL, pol, NULL);
		d->alen = body->len - k0;
	}
	sift_cgord_pad8(body);
}

/* the percent of the bytes of the plain chunk a seekable one may take (a testing build can make every chunk seekable: sift.test_col_gorder_seek) */
static int
seek_pct(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_gorder_seek)
		return 1000000;
#endif
	return (int) sift_limit(gord_seek_pct, SIFT_CGORD_SEEK_MAX_PCT);
}

/*
 * The plain chunk written to pb, as a seekable one where the block is written so and that takes at most seek_pct() percent of its bytes (d is then the seekable one's entry, with the
 * first key and the offset it had). Whether the chunk is compact (compact_smaller) is decided against the plain bytes, so that the seekable form does not change which chunks are.
 */
static void
seek_upgrade(StringInfo pb, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, bool dense)
{
	StringInfoData sb;
	SiftCGOrdDir sd = *d;
	bool		seek;

	if (!dense || m < SIFT_CGORD_SUB || seek_pct() <= 0)
		return;
	initStringInfo(&sb);
	seek = sift_cgord_seek_chunk(&sb, &sd, kv, av, m) && (uint64) sb.len * 100 <= (uint64) pb->len * (uint64) seek_pct();
	if (seek)
	{
		*d = (SiftCGOrdDir) {d->first, d->off, sd.klen, sd.alen, sd.pad};
		resetStringInfo(pb);
		appendBinaryStringInfo(pb, sb.data, sb.len);
	}
	pfree(sb.data);
}

/* ... and as a whole one: plain, or seekable where that is not larger (seek_upgrade) */
static void
whole_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m, bool dense, const SiftCcPolicy *pol)
{
	StringInfoData pb;

	initStringInfo(&pb);
	plain_chunk(&pb, d, kv, av, m, dense, pol);
	seek_upgrade(&pb, d, kv, av, m, dense);
	appendBinaryStringInfo(body, pb.data, pb.len);
	pfree(pb.data);
}

/* The longest run of equal keys of the chunk (sorted), within a group when e, the entries of a grouped block, is given */
static uint32
longest_run(const int64 *kv, const OrdEnt *e, uint32 m)
{
	uint32		best = 0,
				run = 0;

	for (uint32 i = 0; i < m; i++)
	{
		run = i > 0 && kv[i] == kv[i - 1] && (e == NULL || e[i].group == e[i - 1].group) ? run + 1 : 1;
		best = Max(best, run);
	}
	return best;
}

/*
 * A chunk of a grouped block as a compact one. The keys of a group ascend and those of the next start again, which the buckets (they only ascend) cannot follow: the chunk is written
 * as if every group came one bucket after the one before it, and the first key of each group after the first (the chunk's first key is the first group's) is kept exact after the
 * addresses with its position, so that the reader gives each group its own base again. False, nothing written, when more groups start in the chunk than the directory counts (255) or the
 * shifted keys would pass 62 bits. e are the entries of the chunk.
 */
static bool
compact_group_chunk(StringInfo body, SiftCGOrdDir *d, const OrdEnt *e, const int64 *kv, const int64 *av, uint32 m, const SiftCcPolicy *pol)
{
	uint64		gapsum = 0,
				ngap = 0,
				off = 0,
				offa = 0;
	uint32		sh = 0,
				nan = 0,
				a = 0,
				k0 = body->len;
	uint16		pos[255];
	int64		anc[255];
	int64	   *tv;

	for (uint32 i = 1; i < m; i++)
		if (e[i].group == e[i - 1].group)
		{
			gapsum += (uint64) kv[i] - (uint64) kv[i - 1];
			ngap++;
		}
		else if (++nan > lengthof(pos))
			return false;
	if (ngap > 0 && gapsum / ngap / SIFT_GQ_Q >= 2)
		sh = 63 - __builtin_clzll(gapsum / ngap / SIFT_GQ_Q);
	tv = palloc(m * sizeof(int64));
	tv[0] = kv[0];
	nan = 0;
	for (uint32 i = 1; i < m; i++)
	{
		if (e[i].group == e[i - 1].group)
			off = offa + ((uint64) kv[i] - (uint64) kv[a]);
		else
		{
			a = i;
			off = offa = ((off >> sh) + 1) << sh;
			pos[nan] = (uint16) i;
			anc[nan++] = kv[i];
		}
		if (off >= UINT64CONST(1) << 62 || off < offa)
		{
			pfree(tv);
			return false;
		}
		tv[i] = (int64) ((uint64) kv[0] + off);
	}
	sift_gq_encode_sh(tv, m, sh, body);
	d->klen = body->len - k0;
	sift_cgord_pad8(body);
	addr_chunk(body, d, kv, av, m, false, pol);
	d->pad |= SIFT_CGORD_COMPACT | (nan << SIFT_CGORD_RUNS_SHIFT);
	sift_cgord_pad8(body);
	if (nan > 0)
	{
		appendBinaryStringInfo(body, (const char *) pos, nan * sizeof(uint16));
		sift_cgord_pad8(body);
		appendBinaryStringInfo(body, (const char *) anc, nan * sizeof(int64));
	}
	pfree(tv);
	return true;
}

/*
 * Writes the chunk compact when that is at least a fifth smaller than the whole one and has no run of SIFT_CGORD_TIE_RUN equal keys, the whole one otherwise; true when it is the compact
 * one. A run of equal keys is cheap to address as a run, which a compact chunk has no form for, and a bound that falls on it makes the compact chunk ask the heap for the exact key of
 * every row of the run (a table of half unique timestamps and half 50 days read up to 4 times the blocks of the whole order). force: compact wherever it can be, with the runs marked.
 * d has the first key and the offset of the chunk; e the entries of a chunk of a grouped block.
 */
static bool
compact_smaller(StringInfo body, SiftCGOrdDir *d, const OrdEnt *e, const int64 *kv, const int64 *av, uint32 m, bool dense, bool force, const SiftCcPolicy *pol)
{
	StringInfoData cb,
				wb;
	SiftCGOrdDir cd = *d,
				wd = *d;
	uint32		best = longest_run(kv, e, m);
	bool		smaller;

	initStringInfo(&cb);
	initStringInfo(&wb);
	if (e != NULL)
		smaller = compact_group_chunk(&cb, &cd, e, kv, av, m, pol);
	else
		compact_chunk(&cb, &cd, kv, av, m, pol), smaller = true;
	plain_chunk(&wb, &wd, kv, av, m, dense, pol);
	smaller = smaller && (force || ((uint64) cb.len * 5 <= (uint64) wb.len * 4 && best < SIFT_CGORD_TIE_RUN));
	if (smaller && best >= SIFT_CGORD_TIE_RUN)
		cd.pad |= SIFT_CGORD_TIES;
	if (!smaller)
		seek_upgrade(&wb, &wd, kv, av, m, dense);
	*d = smaller ? cd : wd;
	appendBinaryStringInfo(body, smaller ? cb.data : wb.data, smaller ? cb.len : wb.len);
	pfree(cb.data);
	pfree(wb.data);
	return smaller;
}

/* The rows of the plain block by address, each with its position in the block as its group */
static OrdEnt *
plain_by_code(const SiftCGOrdPlain *p)
{
	uint32		n = p->nvals + p->nnull;
	OrdEnt	   *e = sift_rows_alloc(((Size) n + 1) * sizeof(OrdEnt));

	for (uint32 i = 0; i < p->nvals; i++)
		e[i] = (OrdEnt) {0, p->key[i], sift_cpoint_tid_code(p->tid[i])};
	for (uint32 i = 0; i < p->nnull; i++)
		e[p->nvals + i] = (OrdEnt) {0, 0, sift_cpoint_tid_code(p->ntid[i])};
	ord_sort(e, p->nvals);
	ord_sort(e + p->nvals, p->nnull);
	for (uint32 i = 0; i < n; i++)
		e[i].group = i;
	ord_by_code(e, n);
	return e;
}

/* The positions in the plain block of the n entries ent (palloc'd) */
static int64 *
entry_positions(const SiftCGOrdPlain *plain, const OrdEnt *ent, uint64 n)
{
	OrdEnt	   *pe = plain_by_code(plain);
	int64	   *pv = palloc(Max(n, 1) * sizeof(int64));

	for (uint64 i = 0; i < n; i++)
	{
		uint32		lo = 0,
					hi = plain->nvals + plain->nnull;

		SIFT_BISECT(uint32, mid, lo, hi, pe[mid].code < ent[i].code);
		Assert(lo < plain->nvals + plain->nnull && pe[lo].code == ent[i].code);
		pv[i] = pe[lo].group;
	}
	pfree(pe);
	return pv;
}

/*
 * The chunk of the entries e of a grouped block (their positions pv in the plain block) as a positional one, in place of the one written to body from k0 on, when the positions of a
 * group are no farther apart than SIFT_CGORD_POS_MAX_GAP on the mean (past that each entry a scan reads is in a chunk of the plain order of its own) and it takes at most
 * SIFT_CGORD_POS_MAX_PCT percent of the bytes; true when it does (d is its entry, with the first key and the offset it had).
 */
static bool
pos_smaller(StringInfo body, uint32 k0, SiftCGOrdDir *d, const OrdEnt *e, const int64 *pv, uint32 m)
{
	StringInfoData pb;
	SiftCGOrdDir pd = *d;
	uint64		sum = 0,
				n = 0;
	bool		smaller;

	for (uint32 i = 1; i < m; i++)
		if (e[i].group == e[i - 1].group)
		{
			sum += (uint64) (pv[i] - pv[i - 1]);
			n++;
		}
	if (n == 0 || sum > n * (uint64) sift_limit(gord_pos_gap, SIFT_CGORD_POS_MAX_GAP))
		return false;
	initStringInfo(&pb);
	sift_cgord_pos_chunk(&pb, &pd, pv, m);
	smaller = (uint64) pb.len * 100 <= (uint64) (body->len - k0) * (uint64) sift_limit(gord_pos_pct, SIFT_CGORD_POS_MAX_PCT);
	if (smaller)
	{
		body->len = k0;
		appendBinaryStringInfo(body, pb.data, pb.len);
		*d = (SiftCGOrdDir) {d->first, d->off, pd.klen, pd.alen, pd.pad};
	}
	pfree(pb.data);
	return smaller;
}

static void
build_block(OrdEnt *ent, uint32 nvals, uint32 nnull, int gcol, bool grouped, bool dense, StringInfo out, uint32 collid, const StringInfoData *strs, uint32 strver, uint32 head, uint32 vtotal, int layout,
			const SiftCGOrdPlain *plain)
{
	bool		compact = !grouped && layout != SIFT_CGORD_L_WHOLE,
				gcompact = grouped && layout != SIFT_CGORD_L_WHOLE,
				anycompact = false;
	uint64		n = (uint64) nvals + nnull;
	uint32		crows = sift_cgord_rows();
	uint32		nchunks = (uint32) ((n + crows - 1) / crows);
	int64	   *kv = palloc(SIFT_CGORD_CAP * sizeof(int64)),
			   *av = palloc(SIFT_CGORD_CAP * sizeof(int64)),
			   *pv = NULL;
	StringInfoData body,
				gtab = {0};
	SiftCGOrdDir *dir = palloc0((nchunks + 1) * sizeof(SiftCGOrdDir));
	SiftCGOrdGrp *gv = NULL,
			   *gn = NULL,
			   *gs = NULL;
	uint32		ngv = 0,
				ngn = 0,
				ng = 0;
	uint32		hdr[8] = {SIFT_CGORD_MAGIC, strs != NULL ? strver : SIFT_CGORD_VERSION, (uint32) n, nvals, nchunks, crows, head, vtotal};
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

		/* a table of the runs, not of the entries: the groups are few */
		ngv = group_runs(ent, 0, nvals, NULL, false);
		ngn = group_runs(ent, nvals, nnull, NULL, true);
		gv = palloc0((ngv + 1) * sizeof(SiftCGOrdGrp));
		gn = palloc0((ngn + 1) * sizeof(SiftCGOrdGrp));
		gs = palloc0(((Size) ngv + ngn + 1) * sizeof(SiftCGOrdGrp));
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
		initStringInfo(&gtab);
		if (nnull == 0)
			hdr[7] |= sift_cgord_grp_pack(&gtab, gs, ng, nvals);
		/* the positions of a group are about as many entries apart as the segment has groups: with many more than the writer keeps them for no chunk will be positional */
		if (plain != NULL && ng <= 4 * (uint64) sift_limit(gord_pos_gap, SIFT_CGORD_POS_MAX_GAP))
			pv = entry_positions(plain, ent, n);
	}
	sift_cc_policy_default(&pol);
	initStringInfo(&body);
	for (uint32 c = 0; c < nchunks; c++)
	{
		uint32		m = (uint32) Min((uint64) crows, n - (uint64) c * crows),
					k0;
		uint64		at = (uint64) c * crows;
		bool		cmp = false;

		for (uint32 i = 0; i < m; i++)
		{
			kv[i] = ent[at + i].key;
			av[i] = (int64) ent[at + i].code;
		}
		dir[c].first = kv[0];
		dir[c].off = k0 = (uint32) body.len;		/* made absolute below */
		if (grouped ? gcompact && at + m <= nvals : compact && at >= 2 * sift_cgord_ends() && at + m + 2 * sift_cgord_ends() <= nvals)
			cmp = compact_smaller(&body, &dir[c], grouped ? ent + at : NULL, kv, av, m, dense, layout == SIFT_CGORD_L_COMPACT, &pol);
		else
			whole_chunk(&body, &dir[c], kv, av, m, dense, &pol);
		if (pv != NULL && pos_smaller(&body, k0, &dir[c], ent + at, pv + at, m))
			cmp = false;
		anycompact = cmp || anycompact;
	}
	if (grouped && anycompact)
		hdr[1] = SIFT_CGORD_VERSION_GROUPQ;
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
	sift_cgord_pad8(out);
	if (strs != NULL)
	{
		/* version 3: the strings end the block */
		appendBinaryStringInfo(out, strs->data, strs->len);
	}
	if (grouped)
	{
		if (gtab.len > 0)
			appendBinaryStringInfo(out, gtab.data, gtab.len);
		else
			appendBinaryStringInfo(out, (const char *) gs, ng * sizeof(SiftCGOrdGrp));
		pfree(gtab.data);
		pfree(gv);
		pfree(gn);
		pfree(gs);
	}
	pfree(kv);
	pfree(av);
	pfree(dir);
	pfree(body.data);
	if (pv != NULL)
		pfree(pv);
}

#ifdef SIFT_TESTING
bool		sift_test_col_gorder_seek = false;

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
	OrdEnt	   *ent = sift_rows_alloc(((Size) nvals + nnull + 1) * sizeof(OrdEnt));
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
	build_block(ent, nvals, nnull, 0, false, dense, out, 0, NULL, 0, head, vtotal, dense && vtotal == 0 && nvals > 8 * sift_cgord_ends() && sift_cgord_rows() == SIFT_CGORD_ROWS_FINE ? layout : SIFT_CGORD_L_WHOLE, NULL);
	pfree(ent);
}

/* The entries of the rows with a group (ok[i]): n values and nn NULL rows of which nvals and nnull have one */
void
sift_cgord_build_grouped(const int64 *key, const uint64 *tid, const int64 *group, const uint8 *ok, uint32 n, uint32 nvals, const uint64 *ntid, const int64 *ngroup,
						 const uint8 *nok, uint32 nn, uint32 nnull, int gcol, bool dense, int layout, const SiftCGOrdPlain *plain, StringInfo out)
{
	OrdEnt	   *ent = sift_rows_alloc(((Size) nvals + nnull + 1) * sizeof(OrdEnt));
	uint32		k = 0;

	for (uint32 i = 0; i < n; i++)
		if (ok[i])
			ent[k++] = (OrdEnt) {group[i], key[i], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nn; i++)
		if (nok[i])
			ent[k++] = (OrdEnt) {ngroup[i], 0, sift_cpoint_tid_code(ntid[i])};
	build_block(ent, nvals, nnull, gcol, true, dense, out, 0, NULL, 0, 0, 0, dense && nvals > 8 * sift_cgord_ends() && sift_cgord_rows() == SIFT_CGORD_ROWS_FINE ? layout : SIFT_CGORD_L_WHOLE,
				dense ? plain : NULL);
	pfree(ent);
}

SiftCStrCmp sift_cgord_strcmp = NULL;

static int
str_cmp_bytes(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	int			c = memcmp(a, b, Min(la, lb));

	return c != 0 ? c : SIFT_CMP3(la, lb);
}

char *
sift_cgord_prefix_end(const char *s, uint32 len, uint32 *olen)
{
	char	   *e;

	while (len > 0 && (uint8) s[len - 1] == 0xFF)
		len--;
	if (len == 0)
		return NULL;
	e = palloc(len);
	memcpy(e, s, len);
	e[len - 1]++;
	*olen = len;
	return e;
}

int
sift_cgord_str_order(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	int			c = (sift_cgord_strcmp != NULL ? sift_cgord_strcmp : str_cmp_bytes) (a, la, b, lb, collid);

	return c != 0 ? c : str_cmp_bytes(a, la, b, lb, 0);
}

typedef struct RankEnt
{
	const char *s;
	uint32		len;
	uint32		code;
} RankEnt;

static uint32 rank_coll;

/* the codes in the order of their strings */
static int
rank_cmp(const void *pa, const void *pb)
{
	const RankEnt *a = pa,
			   *b = pb;

	return sift_cgord_str_order(a->s, a->len, b->s, b->len, rank_coll);
}

/* The strings of d ranked: ent[r] is the r-th, by the collation, then the bytes; the strings are decoded once, not at each comparison */
static RankEnt *
rank_strings(const SiftCDict *d, uint32 collid)
{
	uint32		nstr = d->n;
	RankEnt    *ent = sift_rows_alloc(((Size) nstr + 1) * sizeof(RankEnt));

	for (uint32 i = 0; i < nstr; i++)
	{
		ent[i].code = i;
		ent[i].s = sift_cdict_str(d, i, &ent[i].len);
	}
	rank_coll = collid;
	qsort(ent, nstr, sizeof(RankEnt), rank_cmp);
	return ent;
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

uint32 *
sift_cgord_text_order(const SiftCDict *d, uint32 collid)
{
	RankEnt    *re = rank_strings(d, collid);
	uint32	   *order = sift_rows_alloc(((Size) d->n + 1) * sizeof(uint32));

	for (uint32 r = 0; r < d->n; r++)
		order[r] = re[r].code;
	pfree(re);
	return order;
}

void
sift_cgord_build_text(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, const SiftCDict *d, uint32 collid, const uint32 *order, bool dense, bool perm, StringInfo out)
{
	uint32		nstr = d->n,
				nblk = (nstr + SIFT_CGORD_STR_BLK - 1) / SIFT_CGORD_STR_BLK;
	uint32	   *rank = sift_rows_alloc(((Size) nstr + 1) * sizeof(uint32)),
			   *boff = palloc((nblk + 1) * sizeof(uint32));
	OrdEnt	   *ent = sift_rows_alloc(((Size) nvals + nnull + 1) * sizeof(OrdEnt));
	StringInfoData body,
				strs;
	uint32		hdr[4] = {nstr, SIFT_CGORD_STR_BLK, nblk, 0};
	const char *prev = NULL;
	uint32		plen = 0;

	for (uint32 r = 0; r < nstr; r++)
		rank[order[r]] = r;
	initStringInfo(&strs);
	if (perm)
	{
		uint32		bits = sift_cdict_perm_bits(nstr),
					ph[4] = {nstr, bits, 0, 0};

		appendBinaryStringInfo(&strs, (const char *) ph, sizeof(ph));
		put_packed(&strs, order, nstr, bits);
	}
	else
	{
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
		pfree(body.data);
	}
	sift_cgord_pad8(&strs);
	for (uint32 i = 0; i < nvals; i++)
		ent[i] = (OrdEnt) {0, rank[key[i]], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nnull; i++)
		ent[nvals + i] = (OrdEnt) {0, 0, sift_cpoint_tid_code(ntid[i])};
	build_block(ent, nvals, nnull, 0, false, dense, out, collid, &strs, perm ? SIFT_CGORD_VERSION_TEXTPERM : SIFT_CGORD_VERSION_TEXT, 0, 0, SIFT_CGORD_L_WHOLE, NULL);
	pfree(rank);
	pfree(boff);
	pfree(ent);
	pfree(strs.data);
}
