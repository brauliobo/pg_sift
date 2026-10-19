/*
 * sift_col_preagg.c - pre-aggregates of a unit: exact integer sums, the mini cube (GROUP BY dim -> count, per measure non-NULL count and sum),
 * its byte format, its option and its verification. Design and the rules of use: docs/COL-PREAGG.md. Portable (sift_host.h only).
 *
 * Cube blob of a unit (all integers LEB128 varints unless said): u8 ndims, a directory of ndims x (leaf, bytes of its body) so that one dim is
 * found without reading the others, then the bodies:
 *     u8 nmeas, ngroups, u8 flags (bit 0: the last group is the NULL group), nmeas x measure leaf,
 *     keys of the groups with a value in ascending order (first: zigzag, then the positive differences),
 *     counts[ngroups],
 *     per measure: nn[ngroups], then sum[ngroups] (INT measure: zigzag 128-bit; float measure: 8 bytes, the double's bits)
 *
 * Element blob of a unit (column format minor 4): u8 nlists, nlists x (leaf of the list, bytes of its cube) as varints, then the cubes in that
 * order, each in the format above with the leaves of the list as dims and measures; their rows are the elements of the list.
 */
#include "sift_host.h"

#include "sift_col_preagg.h"

#define PAGG_NULLGROUP	0x01

/* ---------- leaves that can be dims and measures ---------- */

/* a scalar leaf of the top level, or of a list of the top level: a list inside a list has no cube */
static bool
depth1(const SiftColSchema *s, const SiftColDef *d)
{
	return d->kind == SIFT_COLK_SCALAR && (d->parent < 0 || s->cols[d->parent].parent < 0);
}

bool
sift_cpagg_dim_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return depth1(s, d) && (d->store == SIFT_COLS_INT || d->store == SIFT_COLS_STR);
}

bool
sift_cpagg_meas_ok(const SiftColSchema *s, const SiftColDef *m, const SiftColDef *dim)
{
	if (m->parent != dim->parent || !depth1(s, m))
		return false;
	if (m->store == SIFT_COLS_F4 || m->store == SIFT_COLS_F8)
		return true;
	return m->store == SIFT_COLS_INT &&
		(m->type == SIFT_COLT_INT2 || m->type == SIFT_COLT_INT4 || m->type == SIFT_COLT_INT8 || m->type == SIFT_COLT_NUMERIC);
}

/* ---------- the option ---------- */

/* The default measures of a dim: the measures of its list, in order of usefulness: decimals and floats (money, ratings), then the integers */
static int
default_meas(const SiftColSchema *s, int dim, int16 *meas)
{
	int			nm = 0;

	for (int pass = 0; pass < 2; pass++)
		for (int c = 0; c < s->ncols && nm < SIFT_PAGG_DEF_MEAS; c++)
		{
			const SiftColDef *d = &s->cols[c];
			bool		first = d->type == SIFT_COLT_NUMERIC || d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;

			if (c != dim && sift_cpagg_meas_ok(s, d, &s->cols[dim]) && first == (pass == 0))
				meas[nm++] = c;
		}
	return nm;
}

/* The dims of the spec in the top level (list < 0) or in one list */
static int
dims_in(const SiftCPaggSpec *spec, const SiftColSchema *s, int list)
{
	int			n = 0;

	for (int i = 0; i < spec->ndims; i++)
		n += s->cols[spec->dim[i].leaf].parent == list;
	return n;
}

void
sift_cpagg_spec_default(const SiftColSchema *s, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->max_groups = SIFT_PAGG_DEF_GROUPS;
	spec->permille = SIFT_PAGG_DEF_PERMILLE;
	/* the top level first, so that the dims of a table without lists are those of 3.0 */
	for (int lists = 0; lists < 2; lists++)
		for (int c = 0; c < s->ncols; c++)
		{
			SiftCPaggDim *sd;

			if (!sift_cpagg_dim_ok(s, &s->cols[c]) || (s->cols[c].parent >= 0) != lists || spec->ndims == SIFT_PAGG_SPEC_DIMS ||
				dims_in(spec, s, s->cols[c].parent) == SIFT_PAGG_MAX_DIMS)
				continue;
			sd = &spec->dim[spec->ndims++];
			sd->leaf = c;
			sd->nmeas = default_meas(s, c, sd->meas);
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

/* The scalar leaf of this name: a list of scalars or a map and its element leaf share a path, and only the element is a dim or a measure */
static int
find_scalar(const SiftColSchema *s, const char *const *names, const char *name)
{
	for (int i = 0; i < s->ncols; i++)
		if (names[i] && s->cols[i].kind == SIFT_COLK_SCALAR && strcmp(names[i], name) == 0)
			return i;
	return -1;
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
		return psprintf("expected default, off or cube(...), got \"%s\"", opt);
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
			leaf = find_scalar(s, names, dn);
			if (leaf < 0)
				return psprintf("cube dimension \"%s\" is not a column of the index", dn);
			if (!sift_cpagg_dim_ok(s, &s->cols[leaf]))
				return psprintf("cube dimension \"%s\" must be a scalar integer, date, numeric or text column", dn);
			if (dims_in(spec, s, s->cols[leaf].parent) == SIFT_PAGG_MAX_DIMS)
				return "a cube holds at most 16 dimensions";
			if (spec->ndims == SIFT_PAGG_SPEC_DIMS)
				return "a cube option holds at most 32 dimensions";
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
				m = find_scalar(s, names, mn);
				if (m < 0)
					return psprintf("cube measure \"%s\" is not a column of the index", mn);
				if (s->cols[m].parent != s->cols[sd->leaf].parent)
					return psprintf("cube measure \"%s\" is not a column of the same list as the dimension \"%s\"", mn, dn);
				if (!sift_cpagg_meas_ok(s, &s->cols[m], &s->cols[sd->leaf]))
					return psprintf("cube measure \"%s\" must be a smallint, integer, bigint, numeric, real or double precision column", mn);
				if (sd->nmeas == SIFT_PAGG_MAX_MEAS)
					return "a cube dimension holds at most 8 measures";
				sd->meas[sd->nmeas++] = m;
			} while (eat(c, ','));
		}
	} while (eat(c, ';'));
	if (!eat(c, ')'))
		return "expected ) at the end of cube(...)";
	if (!rest_blank(c->p))
		return psprintf("unexpected text after cube(...): \"%s\"", c->p);
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
	uint64		w = 0;
	const uint8 *p = r->p;

	/* nearly every sum fits 63 bits: no 128-bit shifts for those */
	for (int sh = 0; sh < 63; sh += 7)
	{
		uint8		b;

		if (p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *p++;
		w |= (uint64) (b & 0x7F) << sh;
		if (!(b & 0x80))
		{
			r->p = p;
			return (SiftCI128) ((int64) (w >> 1) ^ -(int64) (w & 1));
		}
	}
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
	int			list;				/* the list the dim is a leaf of, -1: the top level */
	StringInfoData blob;
	Size		bytes;
	bool		ok;
	bool		keep;
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

/* The writer keeps the overflow bitmap of a leaf once a unit needed it, zeroed for the next ones: no bit set is no overflow value */
static bool
has_ovf(const SiftCPaggCol *c)
{
	for (uint32 w = 0; c->ovf != NULL && w < (c->n + 63) / 64; w++)
		if (c->ovf[w])
			return true;
	return false;
}

static inline bool
col_usable(const SiftCPaggCol *c, uint32 nrows)
{
	return c->ready && c->n == nrows && !has_ovf(c);
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

static int
kept_in(const DimBuild *db, int nd, int list)
{
	int			kept = 0;

	for (int d = 0; d < nd; d++)
		kept += db[d].keep && db[d].list == list;
	return kept;
}

/* The kept dims of one list (-1: the top level) as a cube blob: the directory of (leaf, bytes), then the bodies; nothing when none is kept */
static void
emit_cube(const SiftCPaggSpec *spec, const DimBuild *db, int nd, int list, StringInfo out)
{
	int			kept = kept_in(db, nd, list);

	if (kept == 0)
		return;
	appendStringInfoChar(out, (char) kept);
	for (int d = 0; d < nd; d++)
		if (db[d].keep && db[d].list == list)
		{
			put_u(out, spec->dim[db[d].spec].leaf);
			put_u(out, db[d].blob.len);
		}
	for (int d = 0; d < nd; d++)
		if (db[d].keep && db[d].list == list)
			appendBinaryStringInfo(out, db[d].blob.data, db[d].blob.len);
}

/* The element blob: the cube of every list that has a kept dim, in ascending list order */
static void
emit_elems(const SiftColSchema *s, const SiftCPaggSpec *spec, const DimBuild *db, int nd, StringInfo out)
{
	StringInfoData body[SIFT_PAGG_SPEC_DIMS];
	int			list[SIFT_PAGG_SPEC_DIMS],
				nl = 0;

	for (int l = 0; l < s->ncols; l++)
		if (kept_in(db, nd, l) > 0)
		{
			initStringInfo(&body[nl]);
			emit_cube(spec, db, nd, l, &body[nl]);
			list[nl++] = l;
		}
	if (nl == 0)
		return;
	appendStringInfoChar(out, (char) nl);
	for (int i = 0; i < nl; i++)
	{
		put_u(out, list[i]);
		put_u(out, body[i].len);
	}
	for (int i = 0; i < nl; i++)
	{
		appendBinaryStringInfo(out, body[i].data, body[i].len);
		pfree(body[i].data);
	}
}

/* The histogram of every dim of the spec; the dims of the lists only when elements are wanted (eout) and the list has no overflow value */
static void
build_dims(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, uint32 maxg, bool elems, DimBuild *db)
{
	uint32		hsize = 16,
				maxn = nrows;
	Scratch		sc;

	for (int d = 0; d < spec->ndims; d++)
		maxn = Max(maxn, cols[spec->dim[d].leaf].n);
	while (hsize < 2 * (maxg + 1))
		hsize *= 2;
	sc.hsize = hsize;
	sc.gid = palloc(maxn * sizeof(uint32));
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
		const SiftCPaggDim *sd = &spec->dim[d];
		int			list = s->cols[sd->leaf].parent;
		uint32		n = list < 0 ? nrows : cols[sd->leaf].n;

		db[d] = (DimBuild) {.spec = d, .list = list};
		initStringInfo(&db[d].blob);
		/* an overflow value in the list's counts: the elements are not all there */
		if (list < 0 || (elems && col_usable(&cols[list], nrows)))
			db[d].ok = build_dim(s, sd, cols, n, maxg, &sc, &db[d].blob);
		db[d].bytes = db[d].blob.len;
	}
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

void
sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes,
				StringInfo out, StringInfo eout)
{
	DimBuild	db[SIFT_PAGG_SPEC_DIMS];
	Size		budget = 0,
				used = 0;

	if (spec->off || spec->ndims == 0 || nrows == 0)
		return;
	build_dims(s, spec, cols, nrows, Min(spec->max_groups, SIFT_PAGG_CUBE_GROUPS), eout != NULL, db);
	if (spec->permille > 0)
		budget = Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * spec->permille);
	/* smallest dims first under the budget: the cheapest histograms answer the most questions per byte */
	for (int round = 0; round < spec->ndims; round++)
	{
		int			best = -1;

		for (int d = 0; d < spec->ndims; d++)
			if (db[d].ok && !db[d].keep && (best < 0 || db[d].bytes < db[best].bytes))
				best = d;
		if (best < 0 || (budget > 0 && used + db[best].bytes + 1 > budget))
			break;
		db[best].keep = true;
		used += db[best].bytes;
	}
	emit_cube(spec, db, spec->ndims, -1, out);
	if (eout)
		emit_elems(s, spec, db, spec->ndims, eout);
	for (int d = 0; d < spec->ndims; d++)
		pfree(db[d].blob.data);
}

/* ---------- reading ---------- */

/* One dim's body (cube blob format above, after the directory): false when it is not consistent with the schema */
static bool
decode_dim(const SiftColSchema *s, Rd *r, int leaf, SiftCHistDim *hd)
{
	uint32		ng,
				nk;

	memset(hd, 0, sizeof(*hd));
	hd->leaf = leaf;
	if (r->p + 2 > r->end)
		return false;
	hd->nmeas = *r->p++;
	ng = (uint32) get_u(r);
	if (r->p >= r->end || hd->nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
		return false;
	hd->ngroups = ng;
	hd->has_null = (*r->p++ & PAGG_NULLGROUP) != 0;
	for (int m = 0; m < hd->nmeas; m++)
	{
		uint64		ml = get_u(r);

		if (ml >= (uint64) s->ncols)
			return false;
		hd->meas[m] = (int16) ml;
	}
	hd->key = palloc0(ng * sizeof(int64));
	hd->count = palloc(ng * sizeof(uint32));
	nk = ng - (hd->has_null ? 1 : 0);
	for (uint32 i = 0; i < nk; i++)
		hd->key[i] = i == 0 ? get_zz(r) : (int64) ((uint64) hd->key[i - 1] + get_u(r));
	for (uint32 i = 0; i < ng; i++)
		hd->count[i] = (uint32) get_u(r);
	for (int m = 0; m < hd->nmeas; m++)
	{
		const SiftColDef *md = &s->cols[hd->meas[m]];

		hd->nn[m] = palloc(ng * sizeof(uint32));
		for (uint32 i = 0; i < ng; i++)
			hd->nn[m][i] = (uint32) get_u(r);
		if (md->store == SIFT_COLS_F4 || md->store == SIFT_COLS_F8)
		{
			if ((Size) (r->end - r->p) < (Size) ng * sizeof(double))
				return false;
			hd->sumf[m] = palloc(ng * sizeof(double));
			memcpy(hd->sumf[m], r->p, ng * sizeof(double));
			r->p += ng * sizeof(double);
		}
		else
		{
			hd->sum[m] = palloc(ng * sizeof(SiftCI128));
			for (uint32 i = 0; i < ng; i++)
				hd->sum[m][i] = get_zz128(r);
		}
	}
	return !r->bad;
}

/*
 * The directory of a blob: leaf and body bytes of each entry (at most max of them; the dims of a cube, or the lists of an element blob); body
 * points at the first body. false when inconsistent
 */
static bool
read_dir(const SiftColSchema *s, const uint8 *blob, uint32 len, int max, int *nd, int *leaf, uint32 *blen, const uint8 **body)
{
	Rd			r = {blob, blob + len, false};
	uint64		total = 0;

	if (len == 0)
		return false;
	*nd = *r.p++;
	if (*nd == 0 || *nd > max)
		return false;
	for (int d = 0; d < *nd; d++)
	{
		uint64		l = get_u(&r),
					n = get_u(&r);

		if (r.bad || l >= (uint64) s->ncols || n == 0 || n > len)
			return false;
		leaf[d] = (int) l;
		blen[d] = (uint32) n;
		total += n;
	}
	*body = r.p;
	return (uint64) (r.end - r.p) == total;
}

bool
sift_cpagg_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCHist *h)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd,
				leaf[SIFT_PAGG_MAX_DIMS];
	uint32		blen[SIFT_PAGG_MAX_DIMS];
	const uint8 *p;
	bool		ok = false;

	memset(h, 0, sizeof(*h));
	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, &nd, leaf, blen, &p))
		goto done;
	for (int d = 0; d < nd; d++)
	{
		Rd			r = {p, p + blen[d], false};

		if (!decode_dim(s, &r, leaf[d], &h->dim[d]) || r.p != r.end)
			goto done;
		p += blen[d];
	}
	h->ndims = nd;
	ok = true;
done:
	MemoryContextSwitchTo(old);
	return ok;
}

int
sift_cpagg_decode_dim(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, int leaf, SiftCHistDim *out)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd,
				lf[SIFT_PAGG_MAX_DIMS],
				res = -1;
	uint32		blen[SIFT_PAGG_MAX_DIMS];
	const uint8 *p;

	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, &nd, lf, blen, &p))
		goto done;
	res = 0;
	for (int d = 0; d < nd; d++)
	{
		if (lf[d] == leaf)
		{
			Rd			r = {p, p + blen[d], false};

			res = decode_dim(s, &r, leaf, out) && r.p == r.end ? 1 : -1;
			break;
		}
		p += blen[d];
	}
done:
	MemoryContextSwitchTo(old);
	return res;
}

const uint8 *
sift_cpagg_elem_cube(const SiftColSchema *s, const uint8 *eblob, uint32 elen, int group, uint32 *len)
{
	int			nl,
				list[SIFT_PAGG_SPEC_DIMS];
	uint32		blen[SIFT_PAGG_SPEC_DIMS];
	const uint8 *p;

	*len = 0;
	if (!read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, &nl, list, blen, &p))
	{
		*len = UINT32_MAX;
		return NULL;
	}
	for (int i = 0; i < nl; p += blen[i++])
		if (list[i] == group)
		{
			*len = blen[i];
			return p;
		}
	return NULL;
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

/* Calls fn for the cube of the unit and the cube of each list of its element blob; false when a blob does not decode or fn says so */
typedef bool (*CubeFn) (const SiftColSchema *s, const SiftCHist *h, void *arg);

static bool
each_cube(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, MemoryContext cx, CubeFn fn, void *arg)
{
	SiftCHist	h;
	int			nl,
				list[SIFT_PAGG_SPEC_DIMS];
	uint32		blen[SIFT_PAGG_SPEC_DIMS];
	const uint8 *p;

	if (len > 0 && (!sift_cpagg_decode(s, blob, len, cx, &h) || !fn(s, &h, arg)))
		return false;
	if (elen == 0)
		return true;
	if (!read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, &nl, list, blen, &p))
		return false;
	for (int i = 0; i < nl; p += blen[i++])
		if (!sift_cpagg_decode(s, p, blen[i], cx, &h) || !fn(s, &h, arg))
			return false;
	return true;
}

static bool
mark_leaves(const SiftColSchema *s, const SiftCHist *h, void *arg)
{
	bool	   *need = arg;

	for (int d = 0; d < h->ndims; d++)
	{
		need[h->dim[d].leaf] = true;
		if (s->cols[h->dim[d].leaf].parent >= 0)
			need[s->cols[h->dim[d].leaf].parent] = true;
		for (int m = 0; m < h->dim[d].nmeas; m++)
			need[h->dim[d].meas[m]] = true;
	}
	return true;
}

bool
sift_cpagg_leaves(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, bool *need)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg leaves", ALLOCSET_SMALL_SIZES);
	bool		ok = each_cube(s, blob, len, eblob, elen, cx, mark_leaves, need);

	MemoryContextDelete(cx);
	return ok;
}

/* The dims and measures of a stored cube, appended to the spec that recomputes it */
static bool
add_to_spec(const SiftColSchema *s, const SiftCHist *h, void *arg)
{
	SiftCPaggSpec *spec = arg;

	if (spec->ndims + h->ndims > SIFT_PAGG_SPEC_DIMS)
		return false;
	for (int d = 0; d < h->ndims; d++, spec->ndims++)
	{
		spec->dim[spec->ndims].leaf = h->dim[d].leaf;
		spec->dim[spec->ndims].nmeas = h->dim[d].nmeas;
		memcpy(spec->dim[spec->ndims].meas, h->dim[d].meas, sizeof(spec->dim[0].meas));
	}
	return true;
}

static bool
same_bytes(const StringInfoData *a, const uint8 *b, uint32 len)
{
	return a->len == (int) len && (len == 0 || memcmp(a->data, b, len) == 0);
}

bool
sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen)
{
	SiftCPaggSpec spec;
	StringInfoData again,
				eagain;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	bool		ok;

	memset(&spec, 0, sizeof(spec));
	spec.fixed = true;
	spec.max_groups = SIFT_PAGG_CUBE_GROUPS;
	ok = each_cube(s, blob, len, eblob, elen, cx, add_to_spec, &spec);
	MemoryContextDelete(cx);
	if (!ok)
		return false;
	initStringInfo(&again);
	initStringInfo(&eagain);
	sift_cpagg_cube(s, &spec, cols, nrows, 0, &again, elen > 0 ? &eagain : NULL);
	ok = same_bytes(&again, blob, len) && same_bytes(&eagain, eblob, elen);
	pfree(again.data);
	pfree(eagain.data);
	return ok;
}
