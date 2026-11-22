/*
 * col/f2/f2_defaults.c - SEC_SCHEMA_DEFAULTS (col/f2/f2_defaults.h): encode, decode with every byte checked, look up, merge and compare with the catalog.
 */
#include "core/host.h"

#include "col/f2/f2_defaults.h"

static inline uint64
align8(uint64 n)
{
	return (n + 7) & ~(uint64) 7;
}

static bool
zero_bytes(const uint8 *p, uint64 n)
{
	for (uint64 i = 0; i < n; i++)
		if (p[i] != 0)
			return false;
	return true;
}

static bool
attnum_ok(int64 a)
{
	return a >= 1 && a <= SIFT_DEFAULTS_MAX_ATTNUM;
}

SiftSchemaDefaults *
sift_defaults_new(void)
{
	return palloc0(sizeof(SiftSchemaDefaults));
}

void
sift_defaults_free(SiftSchemaDefaults *d)
{
	if (d == NULL)
		return;
	for (int i = 0; i < d->n; i++)
		if (d->e[i].s != NULL)
			pfree((void *) d->e[i].s);
	if (d->e != NULL)
		pfree(d->e);
	if (d->dropped != NULL)
		pfree(d->dropped);
	pfree(d);
}

static bool
value_ok(const SiftDefaultVal *v)
{
	if (v->isnull)
		return v->kind >= SIFT_DEFAULT_INT && v->kind <= SIFT_DEFAULT_STR && v->slen == 0;
	return v->kind == SIFT_DEFAULT_STR ? v->slen >= 1 && v->slen <= SIFT_DEFAULTS_MAX_VALUE && v->s != NULL : v->kind == SIFT_DEFAULT_INT || v->kind == SIFT_DEFAULT_F8;
}

SiftDefaultsStatus
sift_defaults_add(SiftSchemaDefaults *d, const SiftDefaultVal *v)
{
	SiftDefaultVal *e;

	if (!attnum_ok(v->attnum))
		return SIFT_DEFAULTS_ATTNUM_BAD;
	if (d->n > 0 && v->attnum <= d->e[d->n - 1].attnum)
		return SIFT_DEFAULTS_ORDER_BAD;
	if (sift_defaults_is_dropped(d, v->attnum))
		return SIFT_DEFAULTS_OVERLAP;
	if (v->kind == SIFT_DEFAULT_STR && !v->isnull && v->slen > SIFT_DEFAULTS_MAX_VALUE)
		return SIFT_DEFAULTS_TOO_BIG;
	if (!value_ok(v))
		return v->kind < SIFT_DEFAULT_INT || v->kind > SIFT_DEFAULT_STR ? SIFT_DEFAULTS_KIND_BAD : SIFT_DEFAULTS_VALUE_BAD;
	if (d->n == d->cap)
	{
		d->cap = d->cap == 0 ? 8 : d->cap * 2;
		d->e = d->e == NULL ? palloc(d->cap * sizeof(SiftDefaultVal)) : repalloc(d->e, d->cap * sizeof(SiftDefaultVal));
	}
	e = &d->e[d->n++];
	*e = *v;
	e->s = NULL;
	if (v->kind == SIFT_DEFAULT_STR && !v->isnull)
	{
		uint8	   *copy = palloc(v->slen);

		memcpy(copy, v->s, v->slen);
		e->s = copy;
	}
	return SIFT_DEFAULTS_OK;
}

SiftDefaultsStatus
sift_defaults_add_dropped(SiftSchemaDefaults *d, int16 attnum)
{
	if (!attnum_ok(attnum))
		return SIFT_DEFAULTS_ATTNUM_BAD;
	if (d->ndropped > 0 && attnum <= d->dropped[d->ndropped - 1])
		return SIFT_DEFAULTS_ORDER_BAD;
	if (sift_defaults_find(d, attnum) != NULL)
		return SIFT_DEFAULTS_OVERLAP;
	if (d->ndropped == d->dropcap)
	{
		d->dropcap = d->dropcap == 0 ? 8 : d->dropcap * 2;
		d->dropped = d->dropped == NULL ? palloc(d->dropcap * sizeof(int16)) : repalloc(d->dropped, d->dropcap * sizeof(int16));
	}
	d->dropped[d->ndropped++] = attnum;
	return SIFT_DEFAULTS_OK;
}

static uint32
value_len(const SiftDefaultVal *v)
{
	return v->isnull ? 0 : v->kind == SIFT_DEFAULT_STR ? v->slen : 8;
}

SiftDefaultsStatus
sift_defaults_encode(SiftSchemaDefaults *d, uint8 **out, uint64 *len)
{
	uint64		values = 0,
				at;
	uint8	   *buf;
	SiftDefaultsHdr *h;
	SiftDefaultsEnt *ent;
	uint8	   *vals;

	for (int i = 0; i < d->n; i++)
		values += align8(value_len(&d->e[i]));
	if (values > UINT32_MAX)
		return SIFT_DEFAULTS_TOO_BIG;
	at = sizeof(SiftDefaultsHdr) + (uint64) d->n * sizeof(SiftDefaultsEnt) + align8((uint64) d->ndropped * sizeof(int16));
	*len = at + values;
	buf = palloc0(*len);
	h = (SiftDefaultsHdr *) buf;
	ent = (SiftDefaultsEnt *) (buf + sizeof(SiftDefaultsHdr));
	vals = buf + at;
	*h = (SiftDefaultsHdr) {.magic = SIFT_DEFAULTS_MAGIC,.version = SIFT_DEFAULTS_VERSION,.nentries = d->n,.ndropped = d->ndropped,.values_len = (uint32) values};
	values = 0;
	for (int i = 0; i < d->n; i++)
	{
		const SiftDefaultVal *v = &d->e[i];

		ent[i] = (SiftDefaultsEnt) {.attnum = v->attnum,.kind = v->kind,.flags = v->isnull ? SIFT_DEFAULT_FLAG_NULL : 0,.off = (uint32) values,.len = value_len(v)};
		if (v->kind == SIFT_DEFAULT_INT && !v->isnull)
			memcpy(vals + values, &v->i, 8);
		else if (v->kind == SIFT_DEFAULT_F8 && !v->isnull)
			memcpy(vals + values, &v->f, 8);
		else if (!v->isnull)
			memcpy(vals + values, v->s, v->slen);
		values += align8(ent[i].len);
	}
	if (d->ndropped > 0)
		memcpy(buf + sizeof(SiftDefaultsHdr) + (uint64) d->n * sizeof(SiftDefaultsEnt), d->dropped, (uint64) d->ndropped * sizeof(int16));
	d->stamp = h->stamp = sift_crc32c(buf + sizeof(SiftDefaultsHdr), *len - sizeof(SiftDefaultsHdr));
	*out = buf;
	return SIFT_DEFAULTS_OK;
}

static SiftDefaultsStatus
decode_header(const uint8 *buf, uint64 len, SiftDefaultsHdr *h, uint64 *dropped_at, uint64 *values_at)
{
	uint64		need;

	if (len < sizeof(SiftDefaultsHdr))
		return SIFT_DEFAULTS_SHORT;
	memcpy(h, buf, sizeof(*h));
	if (h->magic != SIFT_DEFAULTS_MAGIC)
		return SIFT_DEFAULTS_MAGIC_BAD;
	if (h->version != SIFT_DEFAULTS_VERSION)
		return SIFT_DEFAULTS_VERSION_BAD;
	if (h->flags != 0 || h->reserved != 0)
		return SIFT_DEFAULTS_FLAGS_BAD;
	if (h->nentries > SIFT_DEFAULTS_MAX_ATTNUM || h->ndropped > SIFT_DEFAULTS_MAX_ATTNUM || (h->values_len & 7) != 0)
		return SIFT_DEFAULTS_COUNT_BAD;
	*dropped_at = sizeof(*h) + (uint64) h->nentries * sizeof(SiftDefaultsEnt);
	*values_at = *dropped_at + align8((uint64) h->ndropped * sizeof(int16));
	need = *values_at + h->values_len;
	if (len < need)
		return SIFT_DEFAULTS_SHORT;
	if (len > need)
		return SIFT_DEFAULTS_TRAILING;
	if (sift_crc32c(buf + sizeof(*h), len - sizeof(*h)) != h->stamp)
		return SIFT_DEFAULTS_CRC_BAD;
	return SIFT_DEFAULTS_OK;
}

static SiftDefaultsStatus
decode_entry(const SiftDefaultsEnt *en, const uint8 *vals, const SiftDefaultsHdr *h, uint64 off, int prev, SiftDefaultVal *v)
{
	bool		isnull = (en->flags & ~SIFT_DEFAULT_FLAG_NULL) == 0 && (en->flags & SIFT_DEFAULT_FLAG_NULL) != 0;

	if (en->reserved != 0 || (en->flags & ~SIFT_DEFAULT_FLAG_NULL) != 0)
		return SIFT_DEFAULTS_FLAGS_BAD;
	if (!attnum_ok(en->attnum))
		return SIFT_DEFAULTS_ATTNUM_BAD;
	if (en->attnum <= prev || en->off != off)
		return SIFT_DEFAULTS_ORDER_BAD;
	if (en->kind < SIFT_DEFAULT_INT || en->kind > SIFT_DEFAULT_STR)
		return SIFT_DEFAULTS_KIND_BAD;
	if (isnull ? en->len != 0 : en->kind == SIFT_DEFAULT_STR ? en->len < 1 || en->len > SIFT_DEFAULTS_MAX_VALUE : en->len != 8)
		return SIFT_DEFAULTS_VALUE_BAD;
	if ((uint64) en->off + align8(en->len) > h->values_len)
		return SIFT_DEFAULTS_COUNT_BAD;
	if (!zero_bytes(vals + en->off + en->len, align8(en->len) - en->len))
		return SIFT_DEFAULTS_PADDING_BAD;
	*v = (SiftDefaultVal) {.attnum = en->attnum,.kind = en->kind,.isnull = isnull,.slen = isnull || en->kind != SIFT_DEFAULT_STR ? 0 : en->len};
	if (isnull)
		return SIFT_DEFAULTS_OK;
	if (en->kind == SIFT_DEFAULT_INT)
		memcpy(&v->i, vals + en->off, 8);
	else if (en->kind == SIFT_DEFAULT_F8)
		memcpy(&v->f, vals + en->off, 8);
	return SIFT_DEFAULTS_OK;
}

SiftDefaultsStatus
sift_defaults_decode(const uint8 *buf, uint64 len, SiftSchemaDefaults **out)
{
	SiftDefaultsHdr h;
	uint64		dropped_at,
				values_at,
				off = 0;
	SiftDefaultsStatus st = decode_header(buf, len, &h, &dropped_at, &values_at);
	SiftSchemaDefaults *d;
	int			prev = 0;

	*out = NULL;
	if (st != SIFT_DEFAULTS_OK)
		return st;
	d = sift_defaults_new();
	for (uint32 i = 0; i < h.nentries && st == SIFT_DEFAULTS_OK; i++)
	{
		SiftDefaultsEnt en;
		SiftDefaultVal v;

		memcpy(&en, buf + sizeof(h) + (uint64) i * sizeof(en), sizeof(en));
		st = decode_entry(&en, buf + values_at, &h, off, prev, &v);
		if (st == SIFT_DEFAULTS_OK)
		{
			v.s = en.kind == SIFT_DEFAULT_STR && !v.isnull ? buf + values_at + en.off : NULL;
			st = sift_defaults_add(d, &v);
			off += align8(en.len);
			prev = en.attnum;
		}
	}
	if (st == SIFT_DEFAULTS_OK && off != h.values_len)
		st = SIFT_DEFAULTS_COUNT_BAD;
	for (uint32 i = 0; i < h.ndropped && st == SIFT_DEFAULTS_OK; i++)
	{
		int16		a;

		memcpy(&a, buf + dropped_at + (uint64) i * sizeof(a), sizeof(a));
		st = sift_defaults_add_dropped(d, a);
	}
	if (st == SIFT_DEFAULTS_OK && !zero_bytes(buf + dropped_at + (uint64) h.ndropped * sizeof(int16), align8((uint64) h.ndropped * sizeof(int16)) - (uint64) h.ndropped * sizeof(int16)))
		st = SIFT_DEFAULTS_PADDING_BAD;
	if (st != SIFT_DEFAULTS_OK)
	{
		sift_defaults_free(d);
		return st;
	}
	d->stamp = h.stamp;
	*out = d;
	return SIFT_DEFAULTS_OK;
}

const SiftDefaultVal *
sift_defaults_find(const SiftSchemaDefaults *d, int16 attnum)
{
	int			lo = 0,
				hi = d->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (d->e[mid].attnum == attnum)
			return &d->e[mid];
		if (d->e[mid].attnum < attnum)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

bool
sift_defaults_is_dropped(const SiftSchemaDefaults *d, int16 attnum)
{
	int			lo = 0,
				hi = d->ndropped - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (d->dropped[mid] == attnum)
			return true;
		if (d->dropped[mid] < attnum)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

bool
sift_defaults_val_equal(const SiftDefaultVal *a, const SiftDefaultVal *b)
{
	if (a->attnum != b->attnum || a->kind != b->kind || a->isnull != b->isnull)
		return false;
	if (a->isnull)
		return true;
	if (a->kind == SIFT_DEFAULT_INT)
		return a->i == b->i;
	if (a->kind == SIFT_DEFAULT_F8)
		return memcmp(&a->f, &b->f, sizeof(double)) == 0;
	return a->slen == b->slen && memcmp(a->s, b->s, a->slen) == 0;
}

SiftDefaultsStatus
sift_defaults_merge(const SiftSchemaDefaults *newer, const SiftSchemaDefaults *older, SiftSchemaDefaults **out)
{
	SiftSchemaDefaults *m = sift_defaults_new();
	SiftDefaultsStatus st = SIFT_DEFAULTS_OK;
	int			a = 0,
				b = 0,
				da = 0,
				db = 0;

	*out = NULL;
	for (int i = 0; i < newer->n && st == SIFT_DEFAULTS_OK; i++)
		st = sift_defaults_is_dropped(older, newer->e[i].attnum) ? SIFT_DEFAULTS_MISMATCH : SIFT_DEFAULTS_OK;
	while (st == SIFT_DEFAULTS_OK && (a < newer->n || b < older->n))
	{
		bool		from_newer = b == older->n || (a < newer->n && newer->e[a].attnum <= older->e[b].attnum);
		const SiftDefaultVal *pick = from_newer ? &newer->e[a] : &older->e[b];

		if (from_newer && b < older->n && older->e[b].attnum == pick->attnum)
			b++;
		a += from_newer;
		b += !from_newer;
		if (!sift_defaults_is_dropped(newer, pick->attnum) && !sift_defaults_is_dropped(older, pick->attnum))
			st = sift_defaults_add(m, pick);
	}
	while (st == SIFT_DEFAULTS_OK && (da < newer->ndropped || db < older->ndropped))
	{
		bool		from_newer = db == older->ndropped || (da < newer->ndropped && newer->dropped[da] <= older->dropped[db]);
		int16		x = from_newer ? newer->dropped[da++] : older->dropped[db++];

		if (m->ndropped == 0 || m->dropped[m->ndropped - 1] != x)
			st = sift_defaults_add_dropped(m, x);
	}
	if (st != SIFT_DEFAULTS_OK)
	{
		sift_defaults_free(m);
		return st;
	}
	*out = m;
	return SIFT_DEFAULTS_OK;
}

SiftDefaultsStatus
sift_defaults_check(const SiftSchemaDefaults *part, const SiftSchemaDefaults *catalog, int16 *bad_attnum)
{
	for (int i = 0; i < part->n; i++)
	{
		const SiftDefaultVal *c = sift_defaults_find(catalog, part->e[i].attnum);

		if (c != NULL && !sift_defaults_val_equal(c, &part->e[i]))
		{
			*bad_attnum = part->e[i].attnum;
			return SIFT_DEFAULTS_MISMATCH;
		}
	}
	for (int i = 0; i < part->ndropped; i++)
		if (!sift_defaults_is_dropped(catalog, part->dropped[i]))
		{
			*bad_attnum = part->dropped[i];
			return SIFT_DEFAULTS_MISMATCH;
		}
	return SIFT_DEFAULTS_OK;
}

static const char *const status_text[] = {
	[SIFT_DEFAULTS_OK] = "ok",
	[SIFT_DEFAULTS_SHORT] = "the section is shorter than its header says",
	[SIFT_DEFAULTS_TRAILING] = "the section is longer than its header says",
	[SIFT_DEFAULTS_MAGIC_BAD] = "the section has a wrong magic number",
	[SIFT_DEFAULTS_VERSION_BAD] = "the section is of a version this build does not know",
	[SIFT_DEFAULTS_FLAGS_BAD] = "the section sets a flag or a reserved field that is not defined",
	[SIFT_DEFAULTS_COUNT_BAD] = "the section counts more entries or bytes than a table can have",
	[SIFT_DEFAULTS_CRC_BAD] = "the section does not match its stamp",
	[SIFT_DEFAULTS_ATTNUM_BAD] = "the section names an attribute number that no table has",
	[SIFT_DEFAULTS_ORDER_BAD] = "the section lists attributes out of order",
	[SIFT_DEFAULTS_KIND_BAD] = "the section has a value of a kind that is not defined",
	[SIFT_DEFAULTS_VALUE_BAD] = "the section has a value whose length its kind rules out",
	[SIFT_DEFAULTS_OVERLAP] = "the section gives a default to a dropped attribute",
	[SIFT_DEFAULTS_PADDING_BAD] = "the section has padding that is not zero",
	[SIFT_DEFAULTS_TOO_BIG] = "a default value is larger than a part keeps",
	[SIFT_DEFAULTS_MISMATCH] = "the defaults of the part and of the catalog differ",
};

const char *
sift_defaults_status_text(SiftDefaultsStatus st)
{
	return status_text[st];
}

void
sift_defaults_map_columns(const int16 *part_attnums, int npart, const int16 *cur_attnums, int ncur, int16 *phys)
{
	int			p = 0;

	for (int c = 0; c < ncur; c++)
	{
		while (p < npart && part_attnums[p] < cur_attnums[c])
			p++;
		phys[c] = p < npart && part_attnums[p] == cur_attnums[c] ? (int16) p : -1;
	}
}

void
sift_defaults_synth(const SiftDefaultVal *v, SiftSynth *out)
{
	*out = (SiftSynth) {.enc = SIFT_SYNTH_ALL_NULL};
	if (v == NULL || v->isnull)
		return;
	*out = (SiftSynth) {.enc = SIFT_SYNTH_CONST,.kind = v->kind,.i = v->i,.f = v->f,.s = v->s,.slen = v->slen};
}

void
sift_synth_fill(const SiftSynth *sy, uint32 nrows, int64 *i, double *f, bool *isnull)
{
	for (uint32 r = 0; r < nrows; r++)
	{
		if (isnull != NULL)
			isnull[r] = sy->enc == SIFT_SYNTH_ALL_NULL;
		if (i != NULL)
			i[r] = sy->enc == SIFT_SYNTH_CONST && sy->kind == SIFT_DEFAULT_INT ? sy->i : 0;
		if (f != NULL)
			f[r] = sy->enc == SIFT_SYNTH_CONST && sy->kind == SIFT_DEFAULT_F8 ? sy->f : 0;
	}
}
