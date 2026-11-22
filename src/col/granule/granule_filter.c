/*
 * col/granule_filter.c - the predicates of a scan run on the chunks of a granule as they are stored (col/granule_filter.h). A chunk keeps its values for the rows that have one, one after the
 * other: the filter runs over those, in the domain of the codes, and the bits it makes are spread over the rows of the granule through the validity of the chunk.
 */
#include "postgres.h"

#include <math.h>

#include "col/f2/f2_pred.h"
#include "col/fsst.h"
#include "col/granule/cfilt_cc.h"
#include "col/granule/cfilt_str_simd.h"
#include "col/granule/granule_filter.h"
#include "col/like_simd.h"
#include "core/cmp.h"
#include "core/damage.h"
#include "core/f2/bitscatter.h"
#include "core/popcount.h"
#include "port/pg_bitutils.h"

#define DENSE_WORDS		(SIFT_COL_NWORDS(SIFT_GRANULE_ROWS) + 8)
#define SET_SCAN_MAX	8		/* a set of at most this many keys is read as that many ranges of the codec; a larger one by testing each key */

/* ---- the dictionary predicate ---- */

static uint32
dict_lower(const char *bytes, const uint32 *off, uint32 n, const char *s, uint32 len)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (sift_f2_str_cmp(bytes + off[mid], off[mid + 1] - off[mid], s, len) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void
set_codes(SiftDictPredCache *r, uint32 from, uint32 to)
{
	for (uint32 c = from; c < to; c++)
		r->bits[c >> 6] |= UINT64CONST(1) << (c & 63);
}

static int
value_cmp(const void *a, const void *b)
{
	const SiftColValue *x = *(const SiftColValue *const *) a,
			   *y = *(const SiftColValue *const *) b;

	return sift_f2_str_cmp(x->v.s, x->len, y->v.s, y->len);
}

/* The codes of the dictionary whose strings are among the m values of p: a lookup of each when they are few against the dictionary, else the values sorted and walked together with it */
static void
set_equal_codes(SiftDictPredCache *r, const SiftColPred *p, int m, uint32 n, const uint32 *off, const char *bytes)
{
	const SiftColValue **sorted;

	if (m <= 8 || (uint64) m * 16 < n)
	{
		for (int j = 0; j < m; j++)
		{
			uint32		c = dict_lower(bytes, off, n, p->vals[j].v.s, p->vals[j].len);

			if (c < n && sift_f2_str_cmp(bytes + off[c], off[c + 1] - off[c], p->vals[j].v.s, p->vals[j].len) == 0)
				set_codes(r, c, c + 1);
		}
		return;
	}
	/* check-memory: allow the values of one predicate, bounded by the query */
	sorted = palloc(m * sizeof(*sorted));
	for (int j = 0; j < m; j++)
		sorted[j] = &p->vals[j];
	qsort(sorted, m, sizeof(*sorted), value_cmp);
	for (uint32 c = 0, j = 0; c < n && j < (uint32) m; c++)
	{
		const char *s = bytes + off[c];
		uint32		len = off[c + 1] - off[c];

		while (j < (uint32) m && sift_f2_str_cmp(s, len, sorted[j]->v.s, sorted[j]->len) > 0)
			j++;
		if (j < (uint32) m && sift_f2_str_cmp(s, len, sorted[j]->v.s, sorted[j]->len) == 0)
			set_codes(r, c, c + 1);
	}
	pfree(sorted);
}

static bool
has_prefix(const char *bytes, const uint32 *off, uint32 c, const char *s, uint32 len)
{
	return off[c + 1] - off[c] >= len && memcmp(bytes + off[c], s, len) == 0;
}

SiftDictPredCache *
sift_dict_pred_prepare(const SiftColPred *p, uint32 n, const uint32 *off, const char *bytes)
{
	SiftDictPredCache *r = palloc0(sizeof(SiftDictPredCache));
	const SiftColValue *v = &p->vals[0];

	r->dict_n = n;
	r->nwords64 = (n + 1 + 63) / 64;
	r->bits = palloc0((r->nwords64 + 1) * sizeof(uint64));
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			set_equal_codes(r, p, p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN ? p->nvals : 1, n, off, bytes);
			break;
		case SIFT_COLOP_LT:
			set_codes(r, 0, dict_lower(bytes, off, n, v->v.s, v->len));
			break;
		case SIFT_COLOP_GE:
			set_codes(r, dict_lower(bytes, off, n, v->v.s, v->len), n);
			break;
		case SIFT_COLOP_LE:
		case SIFT_COLOP_GT:
			{
				uint32		c = dict_lower(bytes, off, n, v->v.s, v->len);

				if (c < n && sift_f2_str_cmp(bytes + off[c], off[c + 1] - off[c], v->v.s, v->len) == 0)
					c++;
				if (p->op == SIFT_COLOP_LE)
					set_codes(r, 0, c);
				else
					set_codes(r, c, n);
			}
			break;
		case SIFT_COLOP_PREFIX:
			{
				uint32		from = dict_lower(bytes, off, n, v->v.s, v->len),
							to = from;

				while (to < n && has_prefix(bytes, off, to, v->v.s, v->len))
					to++;
				set_codes(r, from, to);
			}
			break;
		case SIFT_COLOP_CONTAINS:
		case SIFT_COLOP_NOTCONTAINS:
		case SIFT_COLOP_ICONTAINS:
			{
				SiftLikeNeedle nd;

				sift_like_needle_borrow(&nd, (const uint8 *) v->v.s, v->len, p->op == SIFT_COLOP_ICONTAINS);
				sift_like_dict(&nd, n, off, (const uint8 *) bytes, r->bits, NULL);
			}
			break;
		case SIFT_COLOP_IPREFIX:
		case SIFT_COLOP_IEQ:
		case SIFT_COLOP_ARR_OVERLAP:
		case SIFT_COLOP_ARR_CONTAINS:
			r->maybe_bits = palloc0((r->nwords64 + 1) * sizeof(uint64));
			for (uint32 c = 0; c < n; c++)
			{
				SiftLikeVerdict lv = sift_f2_str_verdict(p, bytes + off[c], off[c + 1] - off[c]);

				if (lv == SIFT_LIKE_YES)
					set_codes(r, c, c + 1);
				else if (lv == SIFT_LIKE_MAYBE)
				{
					r->maybe_bits[c >> 6] |= UINT64CONST(1) << (c & 63);
					r->n_maybe++;
				}
			}
			break;
		default:
			for (uint32 c = 0; c < n; c++)
				if (sift_f2_str_pred(p, bytes + off[c], off[c + 1] - off[c]))
					set_codes(r, c, c + 1);
	}
	if (p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTIN || p->op == SIFT_COLOP_NOTCONTAINS)
		sift_cfs_invert(r->bits, n);
	r->min_match = n;
	for (uint32 w = 0, nw = (n + 63) / 64; w < nw; w++)
	{
		uint64 word = r->bits[w];

		if (w + 1 == nw && (n & 63))
			word &= (UINT64CONST(1) << (n & 63)) - 1;
		if (word == 0)
			continue;
		if (r->n_matches == 0)
			r->min_match = (w << 6) + (uint32) __builtin_ctzll(word);
		r->max_match = (w << 6) + (uint32) (63 - __builtin_clzll(word));
		r->n_matches += sift_pc64(word);
	}
	r->is_contiguous = r->n_matches > 0 && r->n_matches == r->max_match - r->min_match + 1;
	r->is_all = r->n_matches == n;
	return r;
}

/* ---- which predicates run on a chunk ---- */

bool
sift_cfilt_compile(SiftCfiltPred *cp, const SiftColDef *def, const SiftColPred *p)
{
	*cp = (SiftCfiltPred) {.def = def,.pred = p};
	if (def->parent >= 0 || def->lane > 0 || sift_col_def_is_wide(def))
		return false;
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
	{
		cp->nulltest = true;
		return true;
	}
	if (p->op == SIFT_COLOP_KEY_EXISTS || p->op == SIFT_COLOP_JSON_TYPE || SIFT_COLOP_IS_EXT(p->op) || sift_f2_undecided(def, p))
		return false;
	if (def->store == SIFT_COLS_INT || def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
	{
		if (p->op < SIFT_COLOP_EQ || p->op > SIFT_COLOP_NOTIN)
			return false;
		for (int j = 0; def->store != SIFT_COLS_INT && j < p->nvals; j++)
			if (isnan(p->vals[j].v.f))
				return false;
		sift_ctest_make(def, NULL, p, &cp->ct);
		cp->numeric = true;
		return true;
	}
	cp->str = def->store == SIFT_COLS_STR && p->op != SIFT_COLOP_SHAPE;
	return cp->str;
}

/* ---- the dense bits: one for each value of the chunk ---- */

static void
dense_clear_tail(uint64 *dense, uint32 nn)
{
	if (nn & 63)
		dense[nn >> 6] &= (UINT64CONST(1) << (nn & 63)) - 1;
	for (uint32 w = (nn + 63) / 64; w < DENSE_WORDS; w++)
		dense[w] = 0;
}

static void
dense_invert(uint64 *dense, uint32 nn)
{
	for (uint32 w = 0; w < (nn + 63) / 64; w++)
		dense[w] = ~dense[w];
	dense_clear_tail(dense, nn);
}

static SiftCfiltResult
dense_result(uint64 *dense, uint32 nn)
{
	uint32		n = 0;

	dense_clear_tail(dense, nn);
	for (uint32 w = 0; w < (nn + 63) / 64; w++)
		n += sift_pc64(dense[w]);
	return n == 0 ? SIFT_CFILT_ALL_FALSE : n == nn ? SIFT_CFILT_ALL_TRUE : SIFT_CFILT_BITS;
}

/* The keys of a test that is a set, run on a cascade: a few of them as that many ranges, a larger set by the kernels of col/granule/cfilt_cc.h (the codec decodes the keys and searches each), or by binary search of the decoded keys where they do not read the nodes */
static void
cascade_set(const SiftCc *cc, const SiftCTest *t, uint32 nn, uint64 *dense)
{
	if (t->nset <= SET_SCAN_MAX)
	{
		uint64		one[DENSE_WORDS];

		memset(dense, 0, sizeof(uint64) * DENSE_WORDS);
		for (uint32 i = 0; i < t->nset; i++)
		{
			sift_cc_filter(cc, t->set[i], t->set[i], one);
			for (uint32 w = 0; w < (nn + 63) / 64; w++)
				dense[w] |= one[w];
		}
	}
	else if (sift_cf_cc_filter(cc, &(SiftCfTest) {.kind = SIFT_CFT_SET,.set = t->set,.nset = t->nset}, dense))
		memset(dense + (nn + 63) / 64, 0, sizeof(uint64) * (DENSE_WORDS - (nn + 63) / 64));
	else
	{
		int64	   *keys = palloc(sizeof(int64) * ((size_t) nn + SIFT_COL_PAD / 8));

		sift_cc_decode(cc, keys);
		memset(dense, 0, sizeof(uint64) * DENSE_WORDS);
		for (uint32 i = 0; i < nn; i++)
			dense[i >> 6] |= (uint64) (bsearch(&keys[i], t->set, t->nset, sizeof(int64), sift_cmp_i64) != NULL) << (i & 63);
		pfree(keys);
	}
}

/* The test of a numeric leaf on a cascade of its keys */
static SiftCfiltResult
cascade_test(const SiftCc *cc, const SiftCTest *t, uint32 nn, uint64 *dense)
{
	switch (t->kind)
	{
		case SIFT_CT_NONE:
			return SIFT_CFILT_ALL_FALSE;
		case SIFT_CT_NONNULL:
			return SIFT_CFILT_ALL_TRUE;
		case SIFT_CT_RANGE:
			sift_cc_filter(cc, t->lo, t->hi, dense);
			return dense_result(dense, nn);
		case SIFT_CT_NRANGE:
			sift_cc_filter(cc, t->lo, t->hi, dense);
			dense_invert(dense, nn);
			return dense_result(dense, nn);
		case SIFT_CT_SET:
			cascade_set(cc, t, nn, dense);
			return dense_result(dense, nn);
		case SIFT_CT_NSET:
			cascade_set(cc, t, nn, dense);
			dense_invert(dense, nn);
			return dense_result(dense, nn);
		default:
			return SIFT_CFILT_UNSUPPORTED;
	}
}

/* The keys of a float cascade lie between those of the infinities unless a NaN is among them: a NaN has no place in the order of the keys the way it has in the order of the floats */
static bool
cascade_has_nan(const SiftCc *cc)
{
	return sift_cc_min(cc) < sift_col_f8_key(-INFINITY) || sift_cc_max(cc) > sift_col_f8_key(INFINITY);
}

static SiftCfiltResult
numeric_dense(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *dense)
{
	uint32		nn = c->nnonnull;
	SiftCfiltResult res;
	SiftCc	   *cc;

	switch (c->enc)
	{
		case SIFT_CENC_INT_CONST:
			{
				int64		key;

				memcpy(&key, c->body, 8);
				return sift_ctest_key(&cp->ct, key) ? SIFT_CFILT_ALL_TRUE : SIFT_CFILT_ALL_FALSE;
			}
		case SIFT_CENC_BOOL_BITS:
			switch (sift_cf_bool((const uint8 *) c->body, nn, sift_ctest_key(&cp->ct, 0), sift_ctest_key(&cp->ct, 1), dense))
			{
				case SIFT_CF_FALSE:
					return SIFT_CFILT_ALL_FALSE;
				case SIFT_CF_TRUE:
					return SIFT_CFILT_ALL_TRUE;
				default:
					return dense_result(dense, nn);
			}
		case SIFT_CENC_INT_FOR:
		case SIFT_CENC_INT_DELTA:
		case SIFT_CENC_RLE:
			if (cp->def->store != SIFT_COLS_INT)
				return SIFT_CFILT_UNSUPPORTED;
			cc = sift_cc_blob_open(c->body, c->body_len, nn, NULL);
			break;
		case SIFT_CENC_NUM64:
			if (cp->def->store != SIFT_COLS_INT || c->body_len < 8)
				return SIFT_CFILT_UNSUPPORTED;
			cc = sift_cc_blob_open(c->body + 8, c->body_len - 8, nn, NULL);
			break;
		case SIFT_CENC_ALP:
			if (cp->def->store == SIFT_COLS_INT)
				return SIFT_CFILT_UNSUPPORTED;
			cc = sift_cc_blob_open(c->body, c->body_len, nn, NULL);
			if (cascade_has_nan(cc))
			{
				sift_cc_close(cc);
				return SIFT_CFILT_UNSUPPORTED;
			}
			break;
		default:
			return SIFT_CFILT_UNSUPPORTED;
	}
	res = cascade_test(cc, &cp->ct, nn, dense);
	sift_cc_close(cc);
	return res;
}

static SiftCfiltResult
dict_dense(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *dense)
{
	SiftGranuleDict d;
	const char *codes;
	uint32		codes_len,
				nn = c->nnonnull;
	const SiftDictPredCache *dc = cp->part;
	SiftDictPredCache *own = NULL;
	SiftCc	   *cc;
	SiftCfiltResult res;

	sift_chunk_dict(c, &d, &codes, &codes_len);
	if (!(c->ref->enc & SIFT_ENC_PART_DICT) || dc == NULL || dc->dict_n != d.n)
		dc = own = sift_dict_pred_prepare(cp->pred, d.n, d.off, d.bytes);
	if (dc->n_matches == 0)
		res = SIFT_CFILT_ALL_FALSE;
	else if (dc->is_all)
		res = SIFT_CFILT_ALL_TRUE;
	else
	{
		cc = sift_cc_blob_open(codes, codes_len, nn, NULL);
		if (dc->is_contiguous)
			sift_cc_filter(cc, dc->min_match, dc->max_match, dense);
		else
			sift_cc_filter_member(cc, dc->bits, dc->dict_n, dense);
		sift_cc_close(cc);
		res = dense_result(dense, nn);
	}
	if (own != NULL)
	{
		pfree(own->bits);
		pfree(own);
	}
	if (!d.part)
	{
		pfree((void *) d.off);
		pfree((void *) d.bytes);
	}
	return res;
}

/* ---- FSST ---- */

#define FSST_SET_MIN	4		/* an IN list of more needles than this is a set: the chunk is read once for all of them */

static void
free_if(void *p)
{
	if (p != NULL)
		pfree(p);
}

/* The chunk's table: the part's (the chunk keeps none) or the one it holds ahead of its strings */
bool
sift_fsst_chunk_open(const SiftChunk *c, SiftFsstChunk *f)
{
	const uint8 *body = (const uint8 *) c->body;
	uint32		avail = c->body_len,
				at = 0,
				hdr[2],
				whole;

	*f = (SiftFsstChunk) {0};
	if (c->ref->enc & SIFT_ENC_PART_DICT)
	{
		const SiftPartDict *pd = sift_part_dict_find(c->view->dicts, c->idx - 1, SIFT_DICT_KIND_FSST);

		if (pd == NULL)
			return false;
		f->t = pd->fsst;
	}
	else
	{
		f->t = f->own = palloc(sizeof(SiftFsst));
		at = sift_fsst_read_table(f->own, body, avail);
		if (at == 0)
			return false;
	}
	if (avail - at < 16)
		return false;
	memcpy(hdr, body + at, 8);
	memcpy(&whole, body + at + 8, 4);
	whole = 8 + MAXALIGN(whole);
	at += 8;
	if (whole > avail - at || hdr[1] >= avail - at - whole)
		return false;
	f->lens = sift_cc_blob_open((const char *) body + at, avail - at, c->nnonnull, NULL);
	f->coded = body + at + whole;
	f->coded_bytes = hdr[1];
	f->raw_bytes = hdr[0];
	return true;
}

/* The coded length of each string and where it starts: decoded once for the chunk */
void
sift_fsst_chunk_offsets(SiftFsstChunk *f, uint32 nn)
{
	if (f->off != NULL)
		return;
	f->clen = palloc(sizeof(int64) * ((size_t) nn + SIFT_COL_PAD / 8));
	f->off = palloc(sizeof(uint32) * ((size_t) nn + 1));
	sift_cc_decode(f->lens, f->clen);
	f->off[0] = 0;
	for (uint32 k = 0; k < nn; k++)
	{
		if (f->clen[k] < 0 || (uint64) f->clen[k] > f->coded_bytes - f->off[k])
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a coded string is past the end of its chunk"));
		f->off[k + 1] = f->off[k] + (uint32) f->clen[k];
	}
}

void
sift_fsst_chunk_close(SiftFsstChunk *f)
{
	sift_cc_close(f->lens);
	free_if(f->own);
	free_if(f->clen);
	free_if(f->off);
}

static void
fsst_needle(const SiftFsst *t, const SiftColPred *p, int j, SiftFsstNeedle *nd)
{
	const char *needle = p->vals[j].v.s;
	uint32		nlen = p->vals[j].len;

	nd->enc = palloc(2 * (size_t) nlen + 2);
	nd->raw = nlen;
	nd->elen = p->op == SIFT_COLOP_PREFIX ? sift_fsst_encode_safe_prefix(t, needle, nlen, nd->enc, &nd->raw) : sift_fsst_encode(t, needle, nlen, nd->enc);
}

static uint32
fsst_hash(const uint8 *s, uint32 len)
{
	uint32		h = 2166136261U;

	for (uint32 i = 0; i < len; i++)
		h = (h ^ s[i]) * 16777619U;
	return h;
}

SiftFsstPredCache *
sift_fsst_pred_prepare(const SiftFsst *fsst, const SiftColPred *p)
{
	SiftFsstPredCache *fc = palloc0(sizeof(SiftFsstPredCache));
	bool		set = p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN;

	fc->fsst = fsst;
	fc->n = set ? p->nvals : 1;
	/* check-memory: allow the values of one predicate, bounded by the query */
	fc->needles = palloc(fc->n * sizeof(SiftFsstNeedle));
	fc->min_elen = UINT32_MAX;
	for (int j = 0; j < fc->n; j++)
	{
		const SiftFsstNeedle *nd = &fc->needles[j];

		fsst_needle(fsst, p, j, &fc->needles[j]);
		fc->min_elen = Min(fc->min_elen, nd->elen);
		fc->max_elen = Max(fc->max_elen, nd->elen);
		if (nd->elen < 64)
			fc->len_mask |= UINT64CONST(1) << nd->elen;
		else
			fc->has_long = true;
	}
	if (set && fc->n > FSST_SET_MIN)
	{
		uint32		cap = pg_nextpower2_32(Max(16, (uint32) fc->n * 2));

		/* check-memory: allow the values of one predicate, bounded by the query */
		fc->ht = palloc0(cap * sizeof(SiftFsstHashSlot));
		fc->ht_mask = cap - 1;
		for (int j = 0; j < fc->n; j++)
		{
			uint32		h = fsst_hash(fc->needles[j].enc, fc->needles[j].elen),
						at = h & fc->ht_mask;

			while (fc->ht[at].idx != 0)
				at = (at + 1) & fc->ht_mask;
			fc->ht[at] = (SiftFsstHashSlot) {.hash = h,.idx = j + 1};
		}
	}
	return fc;
}

void
sift_fsst_pred_free(SiftFsstPredCache *fc)
{
	for (int j = 0; j < fc->n; j++)
		pfree(fc->needles[j].enc);
	pfree(fc->needles);
	free_if(fc->ht);
	pfree(fc);
}

static bool
fsst_set_contains(const SiftFsstPredCache *fc, const uint8 *s, uint32 len)
{
	uint32		h = fsst_hash(s, len);

	for (uint32 at = h & fc->ht_mask; fc->ht[at].idx != 0; at = (at + 1) & fc->ht_mask)
	{
		const SiftFsstNeedle *nd = &fc->needles[fc->ht[at].idx - 1];

		if (fc->ht[at].hash == h && nd->elen == len && memcmp(nd->enc, s, len) == 0)
			return true;
	}
	return false;
}

/* probe &= live over the words of nn bits */
static void
dense_and(uint64 *probe, const uint64 *live, uint32 nn)
{
	if (live != NULL)
		for (uint32 w = 0; w < (nn + 63) / 64; w++)
			probe[w] &= live[w];
}

/* The strings among those of probe whose coded bytes begin with enc (elen of them) and, for a prefix, whose rest begins with the tail: the length filter that made probe is exact */
static bool
fsst_rows(const SiftFsstChunk *f, const uint8 *enc, uint32 elen, uint32 nn, const uint64 *probe, uint64 *dense, const uint8 *tail, uint32 tail_len)
{
	bool		any = false;

	for (uint32 w = 0; w < (nn + 63) / 64; w++)
		for (uint64 bits = probe[w]; bits != 0; bits &= bits - 1)
		{
			uint32		k = (w << 6) + (uint32) __builtin_ctzll(bits);
			const uint8 *s = f->coded + f->off[k];

			if (memcmp(s, enc, elen) != 0)
				continue;
			if (tail_len > 0)
			{
				uint8		out[8 * 2 * 8 + SIFT_FSST_SLACK + 16];
				uint32		rem = f->clen[k] - elen,
							take = Min(rem, 2 * tail_len),
							got = sift_fsst_decode(f->t, s + elen, take, out);

				if (got == UINT32_MAX && take < rem)
					got = sift_fsst_decode(f->t, s + elen, take + 1, out);
				if (got == UINT32_MAX || got < tail_len || memcmp(out, tail, tail_len) != 0)
					continue;
			}
			dense[k >> 6] |= UINT64CONST(1) << (k & 63);
			any = true;
		}
	return any;
}

/* The needles one after the other, each by the length filter of the codec and the strings of that length */
static bool
fsst_each_dense(const SiftCfiltPred *cp, SiftFsstChunk *f, uint32 nn, const uint64 *dense_live, uint64 *dense)
{
	const SiftColPred *p = cp->pred;
	const SiftFsstPredCache *fc = cp->fsst_part != NULL && cp->fsst_part->fsst == f->t ? cp->fsst_part : NULL;
	bool		any = false;
	uint64		probe[DENSE_WORDS];

	for (int j = 0; j < (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN ? p->nvals : 1); j++)
	{
		SiftFsstNeedle own;
		const SiftFsstNeedle *nd = &own;

		if (fc != NULL)
			nd = &fc->needles[j];
		else
			fsst_needle(f->t, p, j, &own);
		sift_cc_filter(f->lens, nd->elen, p->op == SIFT_COLOP_PREFIX ? PG_INT64_MAX : nd->elen, probe);
		dense_and(probe, dense_live, nn);
		if (dense_result(probe, nn) != SIFT_CFILT_ALL_FALSE)
		{
			sift_fsst_chunk_offsets(f, nn);
			any |= fsst_rows(f, nd->enc, nd->elen, nn, probe, dense, (const uint8 *) p->vals[j].v.s + nd->raw, p->vals[j].len - nd->raw);
		}
		if (fc == NULL)
			pfree(own.enc);
	}
	return any;
}

/* An IN list of many needles: one length filter for all of them, then each string of a length some needle has is looked up in the set of the needles */
static bool
fsst_set_dense(const SiftCfiltPred *cp, SiftFsstChunk *f, uint32 nn, const uint64 *dense_live, uint64 *dense)
{
	SiftFsstPredCache *own = NULL;
	const SiftFsstPredCache *fc = cp->fsst_part != NULL && cp->fsst_part->fsst == f->t ? cp->fsst_part : (own = sift_fsst_pred_prepare(f->t, cp->pred));
	bool		any = false;
	uint64		probe[DENSE_WORDS];

	sift_cc_filter(f->lens, fc->min_elen, fc->max_elen, probe);
	dense_and(probe, dense_live, nn);
	if (dense_result(probe, nn) != SIFT_CFILT_ALL_FALSE)
	{
		sift_fsst_chunk_offsets(f, nn);
		for (uint32 w = 0; w < (nn + 63) / 64; w++)
			for (uint64 bits = probe[w]; bits != 0; bits &= bits - 1)
			{
				uint32		k = (w << 6) + (uint32) __builtin_ctzll(bits),
							el = (uint32) f->clen[k];

				if (el < 64 ? !((fc->len_mask >> el) & 1) : !fc->has_long)
					continue;
				if (fsst_set_contains(fc, f->coded + f->off[k], el))
				{
					dense[w] |= UINT64CONST(1) << (k & 63);
					any = true;
				}
			}
	}
	if (own != NULL)
		sift_fsst_pred_free(own);
	return any;
}

static SiftCfiltResult
fsst_dense(const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint64 *dense)
{
	const SiftColPred *p = cp->pred;
	SiftFsstChunk f;
	uint32		nn = c->nnonnull;
	bool		invert = p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTIN,
				set = (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN) && p->nvals > FSST_SET_MIN,
				any;

	if (p->op != SIFT_COLOP_EQ && p->op != SIFT_COLOP_NE && p->op != SIFT_COLOP_IN && p->op != SIFT_COLOP_NOTIN && p->op != SIFT_COLOP_PREFIX)
		return SIFT_CFILT_UNSUPPORTED;
	if (!sift_fsst_chunk_open(c, &f))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the symbol table of a string chunk is damaged"));
	memset(dense, 0, sizeof(uint64) * DENSE_WORDS);
	any = set ? fsst_set_dense(cp, &f, nn, dense_live, dense) : fsst_each_dense(cp, &f, nn, dense_live, dense);
	sift_fsst_chunk_close(&f);
	if (!any)
		return invert && nn > 0 ? SIFT_CFILT_ALL_TRUE : SIFT_CFILT_ALL_FALSE;
	if (invert)
		dense_invert(dense, nn);
	return dense_result(dense, nn);
}

/* ---- from the values to the rows ---- */

static void
validity_words(const SiftChunk *c, uint64 *valid)
{
	uint32		nwords = (c->nrows + 63) / 64;

	memset(valid, 0, sizeof(uint64) * (nwords + 1));
	memcpy(valid, c->validity, ((size_t) c->nrows + 7) / 8);
	if (c->nrows & 63)
		valid[nwords - 1] &= (UINT64CONST(1) << (c->nrows & 63)) - 1;
}

SiftCfiltResult
sift_granule_filter_compressed(const SiftGranuleView *v, const SiftCfiltPred *cp, uint64 *out_mask)
{
	return sift_granule_filter_compressed_live(v, cp, NULL, out_mask);
}

/* The rows of live that hold a value, as the bits of the values of the chunk (one for each row that has one): the count of them */
static uint32
dense_live_make(const SiftChunk *c, const uint64 *live, uint64 *dense_live)
{
	uint32		nw = (c->nrows + 63) / 64,
				n = 0,
				rank = 0;
	uint64		valid[SIFT_CFILT_WORDS + 1];

	memset(dense_live, 0, sizeof(uint64) * DENSE_WORDS);
	if (c->validity == NULL)
	{
		memcpy(dense_live, live, sizeof(uint64) * nw);
		dense_clear_tail(dense_live, c->nrows);
		for (uint32 w = 0; w < nw; w++)
			n += sift_pc64(dense_live[w]);
		return n;
	}
	validity_words(c, valid);
	for (uint32 w = 0; w < nw; w++)
	{
		for (uint64 bits = live[w] & valid[w]; bits != 0; bits &= bits - 1)
		{
			uint32		b = (uint32) __builtin_ctzll(bits),
						k = rank + sift_pc64(valid[w] & ((UINT64CONST(1) << b) - 1));

			dense_live[k >> 6] |= UINT64CONST(1) << (k & 63);
			n++;
		}
		rank += sift_pc64(valid[w]);
	}
	return n;
}

static SiftCfiltResult
nulltest_rows(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *out_mask)
{
	bool		want_null = cp->pred->op == SIFT_COLOP_ISNULL;
	uint32		nrows = c->nrows;

	if (c->validity == NULL)
		return want_null ? SIFT_CFILT_ALL_FALSE : SIFT_CFILT_ALL_TRUE;
	validity_words(c, out_mask);
	if (want_null)
	{
		for (uint32 w = 0; w < (nrows + 63) / 64; w++)
			out_mask[w] = ~out_mask[w];
		if (nrows & 63)
			out_mask[nrows / 64] &= (UINT64CONST(1) << (nrows & 63)) - 1;
	}
	return SIFT_CFILT_BITS;
}

/* The predicate on the values of an opened chunk, as one bit for each of them; UNSUPPORTED: the chunk or the predicate is one the decoder takes */
static SiftCfiltResult
dense_rows(const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *live, uint64 *dense)
{
	uint64		dense_live[DENSE_WORDS];
	uint32		live_nn = 0;
	SiftCfiltResult res;

	if (cp->numeric)
		return numeric_dense(cp, c, dense);
	if (c->enc == SIFT_CENC_DICT_CODE)
		return dict_dense(cp, c, dense);
	if (live != NULL)
	{
		live_nn = dense_live_make(c, live, dense_live);
		if (live_nn == 0)
			return SIFT_CFILT_ALL_FALSE;
	}
	res = c->enc == SIFT_CENC_STR_FSST ? fsst_dense(cp, c, live != NULL ? dense_live : NULL, dense) : SIFT_CFILT_UNSUPPORTED;
	if (res == SIFT_CFILT_UNSUPPORTED && sift_cfstr_bits_live(cp, c, live != NULL ? dense_live : NULL, live_nn, dense))
		res = dense_result(dense, c->nnonnull);
	return res;
}

static SiftCfiltResult
filter_opened(const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *live, uint64 *out_mask)
{
	uint64		dense[DENSE_WORDS];
	uint64		valid[SIFT_CFILT_WORDS + 1];
	uint32		nrows = c->nrows;
	SiftCfiltResult res;

	if (c->ref->flags & SIFT_CHUNK_ALL_NULL)
		return cp->pred->op == SIFT_COLOP_ISNULL ? SIFT_CFILT_ALL_TRUE : SIFT_CFILT_ALL_FALSE;
	if (cp->nulltest)
		return nulltest_rows(cp, c, out_mask);
	res = dense_rows(cp, c, live, dense);
	if (res == SIFT_CFILT_UNSUPPORTED || res == SIFT_CFILT_ALL_FALSE)
		return res;
	if (res == SIFT_CFILT_ALL_TRUE)
	{
		if (c->validity == NULL)
			return SIFT_CFILT_ALL_TRUE;
		validity_words(c, out_mask);
		return SIFT_CFILT_BITS;
	}
	if (c->validity == NULL)
		memcpy(out_mask, dense, sizeof(uint64) * ((nrows + 63) / 64));
	else
	{
		validity_words(c, valid);
		sift_bits_scatter_valid(out_mask, dense, valid, nrows);
	}
	return SIFT_CFILT_BITS;
}

SiftCfiltResult
sift_granule_filter_compressed_live(const SiftGranuleView *v, const SiftCfiltPred *cp, const uint64 *live, uint64 *out_mask)
{
	int			col = cp->pred->col;
	SiftChunk	c;
	SiftCfiltResult res;

	if (col < 0 || col + 1 >= v->hdr->nchunks || (v->hdr->chunks[col + 1].flags & (SIFT_CHUNK_HAS_OVERFLOW | SIFT_CHUNK_HAS_KINDS | SIFT_CHUNK_ELEMS)))
		return SIFT_CFILT_UNSUPPORTED;
	sift_chunk_open(v, col + 1, &c);
	res = filter_opened(cp, &c, live, out_mask);
	sift_chunk_close(&c);
	return res;
}
