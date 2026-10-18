/*
 * sift_col_preagg.c - pre-aggregates of a unit: exact integer sums, the mini cube (GROUP BY dim -> count, per measure non-NULL count and sum),
 * its byte format, its option and its verification. Design and the rules of use: docs/COL-PREAGG.md. Portable (sift_host.h only).
 *
 * Cube blob of a unit (all integers LEB128 varints unless said):
 *   u8 ndims, then per dim:
 *     leaf, u8 nmeas, ngroups, u8 flags (bit 0: the last group is the NULL group), nmeas x measure leaf,
 *     keys of the groups with a value in ascending order (first: zigzag, then the positive differences),
 *     counts[ngroups],
 *     per measure: nn[ngroups], then sum[ngroups] (INT measure: zigzag 128-bit; float measure: 8 bytes, the double's bits)
 */
#include "sift_host.h"

#include "sift_col_preagg.h"

#define PAGG_NULLGROUP	0x01

/* ---------- leaves that can be dims and measures ---------- */

bool
sift_cpagg_dim_ok(const SiftColDef *d)
{
	return d->parent < 0 && d->kind == SIFT_COLK_SCALAR && (d->store == SIFT_COLS_INT || d->store == SIFT_COLS_STR);
}

bool
sift_cpagg_meas_ok(const SiftColDef *d)
{
	if (d->parent >= 0 || d->kind != SIFT_COLK_SCALAR)
		return false;
	if (d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8)
		return true;
	return d->store == SIFT_COLS_INT &&
		(d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_INT8 || d->type == SIFT_COLT_NUMERIC);
}

/* ---------- the option ---------- */

void
sift_cpagg_spec_default(const SiftColSchema *s, SiftCPaggSpec *spec)
{
	int			meas[SIFT_COL_MAX_COLS],
				nm = 0;

	memset(spec, 0, sizeof(*spec));
	spec->max_groups = SIFT_PAGG_DEF_GROUPS;
	spec->permille = SIFT_PAGG_DEF_PERMILLE;
	/* measures in order of usefulness: decimals and floats (money, scores), then the integers */
	for (int pass = 0; pass < 2; pass++)
		for (int c = 0; c < s->ncols; c++)
		{
			const SiftColDef *d = &s->cols[c];
			bool		first = d->type == SIFT_COLT_NUMERIC || d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;

			if (sift_cpagg_meas_ok(d) && first == (pass == 0))
				meas[nm++] = c;
		}
	for (int c = 0; c < s->ncols && spec->ndims < SIFT_PAGG_MAX_DIMS; c++)
	{
		SiftCPaggDim *sd;

		if (!sift_cpagg_dim_ok(&s->cols[c]))
			continue;
		sd = &spec->dim[spec->ndims++];
		sd->leaf = c;
		for (int i = 0; i < nm && sd->nmeas < SIFT_PAGG_DEF_MEAS; i++)
			if (meas[i] != c)
				sd->meas[sd->nmeas++] = meas[i];
	}
}

typedef struct OptCur
{
	const char *p;
} OptCur;

static void
skip_ws(OptCur *c)
{
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n')
		c->p++;
}

/* A name: up to the next delimiter, trimmed; empty at the end of the text */
static char *
take_name(OptCur *c)
{
	const char *b,
			   *e;
	char	   *n;

	skip_ws(c);
	b = c->p;
	while (*c->p && *c->p != ':' && *c->p != ',' && *c->p != ';' && *c->p != ')')
		c->p++;
	e = c->p;
	while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'))
		e--;
	n = palloc(e - b + 1);
	memcpy(n, b, e - b);
	n[e - b] = '\0';
	return n;
}

static bool
eat(OptCur *c, char ch)
{
	skip_ws(c);
	if (*c->p != ch)
		return false;
	c->p++;
	return true;
}

static bool
rest_blank(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return *p == '\0';
}

/* ASCII case-insensitive prefix: does p start with the lower-case word w */
static bool
has_prefix(const char *p, const char *w)
{
	for (; *w; w++, p++)
		if (*p == '\0' || (*p | 0x20) != *w)
			return false;
	return true;
}

/* The option is exactly the word (after blanks) */
static bool
is_word(const char *p, const char *w)
{
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return has_prefix(p, w) && rest_blank(p + strlen(w));
}

static const char *
msg(const char *fmt, const char *a)
{
	StringInfoData b;

	initStringInfo(&b);
	appendStringInfo(&b, fmt, a);
	return b.data;
}

/*
 * One parser for both the syntax check (s == NULL: names are not resolved) and the real thing.
 */
static const char *
parse_option(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	OptCur		cur = {opt};
	OptCur	   *c = &cur;

	if (s)
		sift_cpagg_spec_default(s, spec);
	if (opt == NULL || rest_blank(opt) || is_word(opt, "default"))
		return NULL;
	if (is_word(opt, "off"))
	{
		if (s)
		{
			memset(spec, 0, sizeof(*spec));
			spec->off = true;
		}
		return NULL;
	}
	skip_ws(c);
	if (!has_prefix(c->p, "cube"))
		return msg("expected default, off or cube(...), got \"%s\"", opt);
	c->p += 4;
	if (!eat(c, '('))
		return "expected ( after cube";
	if (s)
	{
		memset(spec, 0, sizeof(*spec));
		spec->fixed = true;
		spec->max_groups = SIFT_PAGG_CUBE_GROUPS;
	}
	do
	{
		char	   *dn = take_name(c);
		SiftCPaggDim *sd = NULL;
		int			leaf = -1;

		if (*dn == '\0')
			return "a cube entry needs a dimension name";
		if (s)
		{
			for (int i = 0; i < s->ncols && leaf < 0; i++)
				if (names[i] && strcmp(names[i], dn) == 0)
					leaf = i;
			if (leaf < 0)
				return msg("cube dimension \"%s\" is not a column of the index", dn);
			if (!sift_cpagg_dim_ok(&s->cols[leaf]))
				return msg("cube dimension \"%s\" must be a scalar integer, date, numeric or text column", dn);
			if (spec->ndims == SIFT_PAGG_MAX_DIMS)
				return "a cube holds at most 16 dimensions";
			sd = &spec->dim[spec->ndims++];
			sd->leaf = leaf;
		}
		if (eat(c, ':'))
		{
			do
			{
				char	   *mn = take_name(c);
				int			m = -1;

				if (*mn == '\0')
					return "a measure name is missing";
				if (!s)
					continue;
				for (int i = 0; i < s->ncols && m < 0; i++)
					if (names[i] && strcmp(names[i], mn) == 0)
						m = i;
				if (m < 0)
					return msg("cube measure \"%s\" is not a column of the index", mn);
				if (!sift_cpagg_meas_ok(&s->cols[m]))
					return msg("cube measure \"%s\" must be a smallint, integer, bigint, numeric, real or double precision column", mn);
				if (sd->nmeas == SIFT_PAGG_MAX_MEAS)
					return "a cube dimension holds at most 8 measures";
				sd->meas[sd->nmeas++] = m;
			} while (eat(c, ','));
		}
	} while (eat(c, ';'));
	if (!eat(c, ')'))
		return "expected ) at the end of cube(...)";
	if (!rest_blank(c->p))
		return msg("unexpected text after cube(...): \"%s\"", c->p);
	return NULL;
}

const char *
sift_cpagg_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	return parse_option(opt, s, names, spec);
}

const char *
sift_cpagg_syntax(const char *opt)
{
	return parse_option(opt, NULL, NULL, NULL);
}

/* ---------- varints ---------- */

static void
put_u(StringInfo out, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(out, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(out, (char) v);
}

static void
put_zz(StringInfo out, int64 v)
{
	put_u(out, ((uint64) v << 1) ^ (uint64) (v >> 63));
}

static void
put_zz128(StringInfo out, SiftCI128 v)
{
	unsigned __int128 u = ((unsigned __int128) v << 1) ^ (unsigned __int128) (v >> 127);

	while (u >= 0x80)
	{
		appendStringInfoChar(out, (char) ((uint8) u | 0x80));
		u >>= 7;
	}
	appendStringInfoChar(out, (char) (uint8) u);
}

typedef struct Rd
{
	const uint8 *p;
	const uint8 *end;
	bool		bad;
} Rd;

static uint64
get_u(Rd *r)
{
	uint64		v = 0;

	for (int sh = 0; sh < 70; sh += 7)
	{
		uint8		b;

		if (r->p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *r->p++;
		v |= (uint64) (b & 0x7F) << sh;
		if (!(b & 0x80))
			return v;
	}
	r->bad = true;
	return 0;
}

static int64
get_zz(Rd *r)
{
	uint64		u = get_u(r);

	return (int64) (u >> 1) ^ -(int64) (u & 1);
}

static SiftCI128
get_zz128(Rd *r)
{
	unsigned __int128 u = 0;

	for (int sh = 0; sh < 133; sh += 7)
	{
		uint8		b;

		if (r->p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *r->p++;
		u |= (unsigned __int128) (b & 0x7F) << sh;
		if (!(b & 0x80))
			return (SiftCI128) (u >> 1) ^ -(SiftCI128) (u & 1);
	}
	r->bad = true;
	return 0;
}

/* ---------- exact leaf sums ---------- */

static inline bool
is_null(const uint64 *nulls, uint32 i)
{
	return nulls != NULL && ((nulls[i / 64] >> (i % 64)) & 1);
}

void
sift_cpagg_leaf(const SiftCPaggCol *c, SiftCLeafAgg *a)
{
	memset(a, 0, sizeof(*a));
	for (uint32 i = 0; i < c->n; i++)
	{
		SiftCI128	k;

		if (is_null(c->nulls, i))
			continue;
		k = c->key[i];
		a->sum += k;				/* 74,496 values of 2^63 cannot leave int128 */
		if (__builtin_add_overflow(a->sumsq, k * k, &a->sumsq))
			a->flags |= SIFT_CAGG_SUMSQ_OVF;
	}
}

/* ---------- the cube of a unit ---------- */

typedef struct GroupKey
{
	int64		key;
	uint32		g;
	uint32		isnull;
} GroupKey;

/* Work areas of one unit's cube, sized by the group cap */
typedef struct Scratch
{
	uint32		hsize;
	uint32	   *gid;				/* [nrows] */
	int64	   *hk;					/* [hsize] */
	int32	   *hg;					/* [hsize] */
	int64	   *gkey;				/* [maxg + 1] */
	uint32	   *cnt;
	uint32	   *nn;
	GroupKey   *ord;
	double	   *sf;
	SiftCI128  *si;
} Scratch;

typedef struct DimBuild
{
	int			spec;				/* index into spec->dim */
	StringInfoData blob;
	Size		bytes;
	bool		ok;
} DimBuild;

static int
cmp_group(const void *a, const void *b)
{
	const GroupKey *x = a,
			   *y = b;

	if (x->isnull != y->isnull)
		return x->isnull ? 1 : -1;
	return x->key < y->key ? -1 : x->key > y->key;
}

static inline bool
col_usable(const SiftCPaggCol *c, uint32 nrows)
{
	return c->ready && c->ovf == NULL && c->n == nrows;
}

/* Builds the histogram of dim sd over the unit; false when it has more than maxg groups or the dim cannot be used (overflow values, inline text) */
static bool
build_dim(const SiftColSchema *s, const SiftCPaggDim *sd, const SiftCPaggCol *cols, uint32 nrows, uint32 maxg, const Scratch *sc,
		  StringInfo out)
{
	const SiftCPaggCol *dc = &cols[sd->leaf];
	uint32		ng = 0,
				nullg = UINT32_MAX,
				hsize = sc->hsize,
				mask = hsize - 1,
			   *gid = sc->gid,
			   *cnt = sc->cnt,
			   *nn = sc->nn;
	int64	   *hk = sc->hk,
			   *gkey = sc->gkey;
	int32	   *hg = sc->hg;
	int			mm[SIFT_PAGG_MAX_MEAS],
				nm = 0;
	GroupKey   *ord = sc->ord;

	if (!col_usable(dc, nrows) || nrows == 0)
		return false;
	memset(hg, 0xFF, hsize * sizeof(int32));
	for (uint32 i = 0; i < nrows; i++)
	{
		uint32		g;

		if (is_null(dc->nulls, i))
		{
			if (nullg == UINT32_MAX)
			{
				if (ng == maxg)
					return false;
				nullg = ng;
				gkey[ng] = 0;
				cnt[ng++] = 0;
			}
			g = nullg;
		}
		else
		{
			int64		k = dc->key[i];
			uint32		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;

			while (hg[h] >= 0 && hk[h] != k)
				h = (h + 1) & mask;
			if (hg[h] < 0)
			{
				if (ng == maxg)
					return false;
				hk[h] = k;
				hg[h] = ng;
				gkey[ng] = k;
				cnt[ng++] = 0;
			}
			g = hg[h];
		}
		gid[i] = g;
		cnt[g]++;
	}
	for (uint32 g = 0; g < ng; g++)
		ord[g] = (GroupKey) {gkey[g], g, g == nullg};
	qsort(ord, ng, sizeof(GroupKey), cmp_group);
	for (int i = 0; i < sd->nmeas; i++)
		if (sd->meas[i] != sd->leaf && col_usable(&cols[sd->meas[i]], nrows))
			mm[nm++] = sd->meas[i];
	put_u(out, sd->leaf);
	appendStringInfoChar(out, (char) nm);
	put_u(out, ng);
	appendStringInfoChar(out, nullg == UINT32_MAX ? 0 : PAGG_NULLGROUP);
	for (int i = 0; i < nm; i++)
		put_u(out, mm[i]);
	{
		int64		prev = 0;
		uint32		nk = ng - (nullg == UINT32_MAX ? 0 : 1);

		for (uint32 i = 0; i < nk; i++)
		{
			if (i == 0)
				put_zz(out, ord[i].key);
			else
				put_u(out, (uint64) ord[i].key - (uint64) prev);
			prev = ord[i].key;
		}
	}
	for (uint32 i = 0; i < ng; i++)
		put_u(out, cnt[ord[i].g]);
	for (int m = 0; m < nm; m++)
	{
		const SiftCPaggCol *mc = &cols[mm[m]];
		bool		fl = s->cols[mm[m]].store == SIFT_COLS_F4 || s->cols[mm[m]].store == SIFT_COLS_F8;
		double	   *sf = sc->sf;
		SiftCI128  *si = sc->si;

		memset(nn, 0, ng * sizeof(uint32));
		if (fl)
		{
			for (uint32 i = 0; i < nrows; i++)
			{
				double		v;

				if (is_null(mc->nulls, i))
					continue;
				v = sift_col_f8_from_key(mc->key[i]);
				/* the first value seeds the sum, as float8pl's transition does: a group of -0 sums to -0 */
				sf[gid[i]] = nn[gid[i]]++ == 0 ? v : sf[gid[i]] + v;
			}
			for (uint32 i = 0; i < ng; i++)
				put_u(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
			{
				double		v = nn[ord[i].g] ? sf[ord[i].g] : 0;

				appendBinaryStringInfo(out, (const char *) &v, sizeof(double));
			}
		}
		else
		{
			memset(si, 0, ng * sizeof(SiftCI128));
			for (uint32 i = 0; i < nrows; i++)
				if (!is_null(mc->nulls, i))
				{
					si[gid[i]] += mc->key[i];
					nn[gid[i]]++;
				}
			for (uint32 i = 0; i < ng; i++)
				put_u(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
				put_zz128(out, si[ord[i].g]);
		}
	}
	return true;
}

void
sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes, StringInfo out)
{
	DimBuild	db[SIFT_PAGG_MAX_DIMS];
	int			nd = 0,
				kept = 0;
	uint32		maxg = Min(spec->max_groups, SIFT_PAGG_CUBE_GROUPS),
				hsize = 16;
	Scratch		sc;
	Size		budget = 0,
				used = 0;
	bool		keep[SIFT_PAGG_MAX_DIMS] = {false};

	if (spec->off || spec->ndims == 0 || nrows == 0)
		return;
	while (hsize < 2 * (maxg + 1))
		hsize *= 2;
	sc.hsize = hsize;
	sc.gid = palloc(nrows * sizeof(uint32));
	sc.hk = palloc(hsize * sizeof(int64));
	sc.hg = palloc(hsize * sizeof(int32));
	sc.gkey = palloc((maxg + 1) * sizeof(int64));
	sc.cnt = palloc((maxg + 1) * sizeof(uint32));
	sc.nn = palloc((maxg + 1) * sizeof(uint32));
	sc.ord = palloc((maxg + 1) * sizeof(GroupKey));
	sc.sf = palloc((maxg + 1) * sizeof(double));
	sc.si = palloc((maxg + 1) * sizeof(SiftCI128));
	for (int d = 0; d < spec->ndims; d++)
	{
		db[nd].spec = d;
		initStringInfo(&db[nd].blob);
		db[nd].ok = build_dim(s, &spec->dim[d], cols, nrows, maxg, &sc, &db[nd].blob);
		db[nd].bytes = db[nd].blob.len;
		nd++;
	}
	if (spec->permille > 0)
		budget = Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * spec->permille);
	/* smallest dims first under the budget: the cheapest histograms answer the most questions per byte */
	for (int round = 0; round < nd; round++)
	{
		int			best = -1;

		for (int d = 0; d < nd; d++)
			if (db[d].ok && !keep[d] && (best < 0 || db[d].bytes < db[best].bytes))
				best = d;
		if (best < 0 || (budget > 0 && used + db[best].bytes + 1 > budget))
			break;
		keep[best] = true;
		used += db[best].bytes;
		kept++;
	}
	if (kept > 0)
	{
		appendStringInfoChar(out, (char) kept);
		for (int d = 0; d < nd; d++)
			if (keep[d])
				appendBinaryStringInfo(out, db[d].blob.data, db[d].blob.len);
	}
	for (int d = 0; d < nd; d++)
		pfree(db[d].blob.data);
	pfree(sc.gid);
	pfree(sc.hk);
	pfree(sc.hg);
	pfree(sc.gkey);
	pfree(sc.cnt);
	pfree(sc.nn);
	pfree(sc.ord);
	pfree(sc.sf);
	pfree(sc.si);
}

/* ---------- reading ---------- */

bool
sift_cpagg_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCHist *h)
{
	Rd			r = {blob, blob + len, false};
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd;
	bool		ok = false;

	memset(h, 0, sizeof(*h));
	if (len == 0)
		goto done;
	nd = *r.p++;
	if (nd > SIFT_PAGG_MAX_DIMS)
		goto done;
	for (int d = 0; d < nd; d++)
	{
		SiftCHistDim *hd = &h->dim[d];
		uint64		leaf = get_u(&r);
		uint32		ng,
					nk;

		if (r.p >= r.end || leaf >= (uint64) s->ncols)
			goto done;
		hd->leaf = (int) leaf;
		hd->nmeas = *r.p++;
		ng = (uint32) get_u(&r);
		if (r.p >= r.end || hd->nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
			goto done;
		hd->ngroups = ng;
		hd->has_null = (*r.p++ & PAGG_NULLGROUP) != 0;
		for (int m = 0; m < hd->nmeas; m++)
		{
			uint64		ml = get_u(&r);

			if (ml >= (uint64) s->ncols)
				goto done;
			hd->meas[m] = (int16) ml;
		}
		hd->key = palloc0(ng * sizeof(int64));
		hd->count = palloc(ng * sizeof(uint32));
		nk = ng - (hd->has_null ? 1 : 0);
		for (uint32 i = 0; i < nk; i++)
		{
			if (i == 0)
				hd->key[i] = get_zz(&r);
			else
				hd->key[i] = (int64) ((uint64) hd->key[i - 1] + get_u(&r));
		}
		for (uint32 i = 0; i < ng; i++)
			hd->count[i] = (uint32) get_u(&r);
		for (int m = 0; m < hd->nmeas; m++)
		{
			const SiftColDef *md = &s->cols[hd->meas[m]];
			bool		fl = md->store == SIFT_COLS_F4 || md->store == SIFT_COLS_F8;

			hd->nn[m] = palloc(ng * sizeof(uint32));
			for (uint32 i = 0; i < ng; i++)
				hd->nn[m][i] = (uint32) get_u(&r);
			if (fl)
			{
				hd->sumf[m] = palloc(ng * sizeof(double));
				if ((Size) (r.end - r.p) < (Size) ng * sizeof(double))
					goto done;
				memcpy(hd->sumf[m], r.p, ng * sizeof(double));
				r.p += ng * sizeof(double);
			}
			else
			{
				hd->sum[m] = palloc(ng * sizeof(SiftCI128));
				for (uint32 i = 0; i < ng; i++)
					hd->sum[m][i] = get_zz128(&r);
			}
		}
		if (r.bad)
			goto done;
	}
	h->ndims = nd;
	ok = !r.bad && r.p == r.end;
done:
	MemoryContextSwitchTo(old);
	return ok;
}

const SiftCHistDim *
sift_cpagg_find(const SiftCHist *h, int leaf)
{
	for (int d = 0; d < h->ndims; d++)
		if (h->dim[d].leaf == leaf)
			return &h->dim[d];
	return NULL;
}

int
sift_cpagg_meas_slot(const SiftCHistDim *d, int m)
{
	for (int i = 0; i < d->nmeas; i++)
		if (d->meas[i] == m)
			return i;
	return -1;
}

bool
sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len)
{
	SiftCHist	h;
	SiftCPaggSpec spec;
	StringInfoData again;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	bool		ok;

	if (len == 0)
		return true;
	if (!sift_cpagg_decode(s, blob, len, cx, &h))
	{
		MemoryContextDelete(cx);
		return false;
	}
	memset(&spec, 0, sizeof(spec));
	spec.fixed = true;
	spec.max_groups = SIFT_PAGG_CUBE_GROUPS;
	spec.ndims = h.ndims;
	for (int d = 0; d < h.ndims; d++)
	{
		spec.dim[d].leaf = h.dim[d].leaf;
		spec.dim[d].nmeas = h.dim[d].nmeas;
		memcpy(spec.dim[d].meas, h.dim[d].meas, sizeof(spec.dim[d].meas));
	}
	initStringInfo(&again);
	sift_cpagg_cube(s, &spec, cols, nrows, 0, &again);
	ok = again.len == (int) len && memcmp(again.data, blob, len) == 0;
	pfree(again.data);
	MemoryContextDelete(cx);
	return ok;
}
