/*
 * fz_shred.c - fuzz target for the JSON shredder (src/col/shred.c, docs/COL-SHRED.md) through its event interface, the one every
 * driver uses: col/shred_pg.c walks a jsonb into exactly these calls, in this order. The input is
 *
 *   u8 flags (bit 0: search wide objects by key, as the jsonb driver does; bits 1-3: small limits; bit 7: spec only)
 *   u16 spec length, the spec text (the path list of the index option), then a JSON text
 *
 * The JSON text is parsed here, lenient where a jsonb cannot be (a number is delivered only in the normalized form numeric_normalize
 * gives, a plain decimal; anything else is a null), and delivered with the same decisions as the jsonb walk: a container the shredder
 * does not enter is skipped, an object ends at SIFT_SHRED_STOP, an array at sift_shred_full. Then the document is checked: ranges
 * monotonic and inside their group, states below 4, text inside the text buffer, map key ids inside the dictionary, parents inside
 * the parent group at every SIMD level, kernels run on every column. A spec that parses must deparse to a text that parses to
 * itself.
 *
 *   FZ_GEN=DIR target     writes the seed corpus and exits
 */
#include "postgres.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "col/shred.h"
#include "fz_shim.h"

#define MAX_DEPTH 100

typedef struct Parser
{
	const char *s,
			   *end;
	SiftShred  *sh;
	int			depth;
} Parser;

static void
ws(Parser *p)
{
	while (p->s < p->end && (*p->s == ' ' || *p->s == '\n' || *p->s == '\t' || *p->s == '\r'))
		p->s++;
}

/* A string token (p->s at the opening quote) unescaped into a palloc'd buffer; false when it is not terminated */
static bool
read_string(Parser *p, char **out, int *len)
{
	char	   *buf = palloc(p->end - p->s + 1);
	int			n = 0;

	p->s++;
	while (p->s < p->end && *p->s != '"')
	{
		if (*p->s == '\\' && p->s + 1 < p->end)
		{
			p->s++;
			buf[n++] = *p->s == 'n' ? '\n' : *p->s == 't' ? '\t' : *p->s;
			p->s++;
			continue;
		}
		buf[n++] = *p->s++;
	}
	if (p->s >= p->end)
		return false;
	p->s++;
	buf[n] = '\0';
	*out = buf;
	*len = n;
	return true;
}

static bool parse_value(Parser *p, bool deliver);

static bool
parse_array(Parser *p, bool deliver)
{
	bool		full = false;

	p->s++;
	ws(p);
	if (p->s < p->end && *p->s == ']')
	{
		p->s++;
		return true;
	}
	for (;;)
	{
		if (!parse_value(p, deliver && !full))
			return false;
		if (deliver && !full && sift_shred_full(p->sh))
			full = true;
		ws(p);
		if (p->s >= p->end)
			return false;
		if (*p->s == ',')
		{
			p->s++;
			ws(p);
			continue;
		}
		if (*p->s == ']')
		{
			p->s++;
			return true;
		}
		return false;
	}
}

static bool
parse_object(Parser *p, bool deliver)
{
	bool		stopped = false;

	p->s++;
	ws(p);
	if (p->s < p->end && *p->s == '}')
	{
		p->s++;
		return true;
	}
	for (;;)
	{
		char	   *key;
		int			klen;
		SiftShredKey act = SIFT_SHRED_SKIP;

		if (p->s >= p->end || *p->s != '"' || !read_string(p, &key, &klen))
			return false;
		ws(p);
		if (p->s >= p->end || *p->s++ != ':')
			return false;
		ws(p);
		if (deliver && !stopped)
			act = sift_shred_key(p->sh, key, klen);
		if (!parse_value(p, deliver && !stopped && act != SIFT_SHRED_SKIP))
			return false;
		if (act == SIFT_SHRED_STOP)
			stopped = true;
		ws(p);
		if (p->s >= p->end)
			return false;
		if (*p->s == ',')
		{
			p->s++;
			ws(p);
			continue;
		}
		if (*p->s == '}')
		{
			p->s++;
			return true;
		}
		return false;
	}
}

static bool
literal(Parser *p, const char *w)
{
	size_t		n = strlen(w);

	if ((size_t) (p->end - p->s) < n || memcmp(p->s, w, n))
		return false;
	p->s += n;
	return true;
}

static bool
parse_number(Parser *p, bool deliver)
{
	const char *start = p->s,
			   *q = p->s;
	bool		plain;

	if (q < p->end && *q == '-')
		q++;
	if (q >= p->end || *q < '0' || *q > '9')
		return false;
	while (q < p->end && *q >= '0' && *q <= '9')
		q++;
	if (q < p->end && *q == '.')
	{
		q++;
		while (q < p->end && *q >= '0' && *q <= '9')
			q++;
	}
	/* an exponent is a number too, but not a normalized one: the jsonb driver never sees it */
	plain = !(q < p->end && (*q == 'e' || *q == 'E'));
	while (q < p->end && (*q == 'e' || *q == 'E' || *q == '+' || *q == '-' || (*q >= '0' && *q <= '9')))
		q++;
	p->s = q;
	if (!deliver)
		return true;
	if (!plain || !sift_shred_wants_number(p->sh))
		sift_shred_number(p->sh, NULL, 0);
	else
		sift_shred_number(p->sh, start, (int) (q - start));
	return true;
}

static bool
parse_value(Parser *p, bool deliver)
{
	bool		ok;

	ws(p);
	if (p->s >= p->end || p->depth > MAX_DEPTH)
		return false;
	switch (*p->s)
	{
		case '{':
		case '[':
			{
				bool		is_array = *p->s == '[';

				if (deliver && !sift_shred_enter(p->sh, is_array))
					deliver = false;
				else if (deliver)
					p->depth++;
				ok = is_array ? parse_array(p, deliver) : parse_object(p, deliver);
				if (deliver)
				{
					p->depth--;
					sift_shred_leave(p->sh);
				}
				return ok;
			}
		case '"':
			{
				char	   *s;
				int			len;

				if (!read_string(p, &s, &len))
					return false;
				if (deliver)
					sift_shred_string(p->sh, s, len);
				return true;
			}
		case 't':
			if (!literal(p, "true"))
				return false;
			if (deliver)
				sift_shred_bool(p->sh, true);
			return true;
		case 'f':
			if (!literal(p, "false"))
				return false;
			if (deliver)
				sift_shred_bool(p->sh, false);
			return true;
		case 'n':
			if (!literal(p, "null"))
				return false;
			if (deliver)
				sift_shred_null(p->sh);
			return true;
		default:
			return parse_number(p, deliver);
	}
}

static void
check_doc(const SiftShredSpec *spec, SiftShred *sh)
{
	const SiftShredDoc *doc = &sh->doc;

	for (int g = 0; g < spec->ngroups; g++)
	{
		const SiftShredGroup *gr = &doc->grp[g];
		uint32		prev = 0;
		uint32	   *parent;

		if (gr->n > gr->cap && g > 0)
			fz_bad("group %d: n %u > cap %u", g, gr->n, gr->cap);
		for (uint32 i = 0; i < gr->pn; i++)
		{
			if (gr->end[i] < prev || gr->end[i] > gr->n)
				fz_bad("group %d: end[%u] = %u out of order or past n = %u", g, i, gr->end[i], gr->n);
			if (gr->lstate[i] > SIFT_SHRED_OVERFLOW)
				fz_bad("group %d: list state %u", g, gr->lstate[i]);
			prev = gr->end[i];
		}
		if (g > 0 && gr->pn && gr->lstate[gr->pn - 1] == SIFT_SHRED_VALUE && gr->end[gr->pn - 1] != gr->n)
			fz_bad("group %d: the last list ends at %u, not n = %u", g, gr->end[gr->pn - 1], gr->n);
		if (spec->groups[g].kind == SIFT_SHRED_MAP)
			for (uint32 i = 0; i < gr->n; i++)
				if (gr->keyid[i] != SIFT_SHRED_NO_KEY && (!sh->dict || gr->keyid[i] >= sh->dict->n))
					fz_bad("group %d: key id %u outside the dictionary", g, gr->keyid[i]);
		if (g > 0 && gr->pn)
		{
			SiftSimdLevel lv[3];
			int			nl = fz_levels(lv);
			uint32	   *first = NULL;

			for (int l = 0; l < nl; l++)
			{
				parent = palloc0(sizeof(uint32) * (gr->n + 64));
				fz_use_level(lv[l]);
				sift_shred_expand_parents(doc, g, parent);
				for (uint32 i = 0; i < gr->n; i++)
					if (parent[i] >= gr->pn)
						fz_bad("group %d: parent[%u] = %u >= pn = %u", g, i, parent[i], gr->pn);
				if (l == 0)
					first = parent;
				else if (memcmp(first, parent, sizeof(uint32) * gr->n))
					fz_bad("group %d: expand_parents differs at SIMD level %d", g, (int) lv[l]);
			}
			fz_use_level(SIFT_SIMD_SCALAR);
		}
	}
	for (int c = 0; c < spec->ncols; c++)
	{
		const SiftShredCol *col = &doc->col[c];
		const SiftShredColDef *def = &spec->cols[c];
		uint32		n = doc->grp[def->group].n;
		uint32		counts[4];
		uint64	   *nulls = palloc0(8 * (n / 64 + 2)),
				   *ovf = palloc0(8 * (n / 64 + 2));

		for (uint32 i = 0; i < n; i++)
		{
			if (col->state[i] > SIFT_SHRED_OVERFLOW)
				fz_bad("column %d: state %u", c, col->state[i]);
			if (def->type == SIFT_SHRED_TEXT && col->state[i] == SIFT_SHRED_VALUE &&
				((uint64) col->val[i].s.off + col->val[i].s.len > doc->ntext || col->val[i].s.len > def->maxlen))
				fz_bad("column %d: text [%u, +%u) outside the %u bytes of text (maxlen %u)", c, col->val[i].s.off, col->val[i].s.len,
					   doc->ntext, def->maxlen);
		}
		sift_shred_kern->state_counts(col->state, n, counts);
		if ((uint64) counts[0] + counts[1] + counts[2] + counts[3] != n)
			fz_bad("column %d: state counts do not add up to %u", c, n);
		sift_shred_kern->state_bits(col->state, n, nulls, ovf);
		if (def->type == SIFT_SHRED_FLOAT8 || def->type == SIFT_SHRED_FLOAT4)
		{
			double		lo,
						hi;

			(void) sift_shred_kern->minmax_f64(col->state, (const double *) col->val, n, &lo, &hi);
		}
	}
}

typedef struct Job
{
	const uint8 *data;
	size_t		len;
} Job;

static void
spec_roundtrip(const char *text)
{
	SiftShredSpec *spec = sift_shred_spec_parse(text);
	const char *canon = pstrdup(sift_shred_spec_deparse(spec));
	SiftShredSpec *again = sift_shred_spec_parse(canon);

	if (strcmp(canon, sift_shred_spec_deparse(again)))
		fz_bad("deparse is not a fixed point: \"%s\" -> \"%s\"", canon, sift_shred_spec_deparse(again));
	if (spec->fingerprint != again->fingerprint)
		fz_bad("fingerprint of \"%s\" changes when parsed again", canon);
}

static void
shred_run(void *a)
{
	Job		   *job = a;
	uint8		flags = job->data[0];
	uint16		slen;
	char	   *spec_text;
	SiftShredSpec *spec;
	SiftShredLimits lim = {SIFT_SHRED_DEFAULT_ELEMS, SIFT_SHRED_DEFAULT_BYTES, SIFT_SHRED_DEFAULT_KEYS};
	SiftShred  *sh;

	memcpy(&slen, job->data + 1, 2);
	if ((size_t) slen + 3 > job->len)
		slen = (uint16) (job->len - 3);
	spec_text = palloc(slen + 1);
	memcpy(spec_text, job->data + 3, slen);
	spec_text[slen] = '\0';
	if (strlen(spec_text) != slen)
		return;					/* the option is text: no embedded NUL */
	spec_roundtrip(spec_text);
	if (flags & 0x80)
		return;
	spec = sift_shred_spec_parse(spec_text);
	if (flags & 0x0e)
	{
		lim.max_elems = 1 + (flags >> 1 & 7) * 3;
		lim.max_bytes = 8 + (flags >> 1 & 7) * 20;
		lim.max_keys = 1 + (flags >> 2 & 3);
	}
	sh = sift_shred_create(spec, &lim, (flags & 0x10) != 0);
	for (int doc = 0; doc < 2; doc++)	/* the second document runs on the state the first left */
	{
		Parser p = {(const char *) job->data + 3 + slen, (const char *) job->data + job->len, sh, 0};

		sift_shred_begin(sh);
		(void) parse_value(&p, true);
		sift_shred_finish(sh);
		check_doc(spec, sh);
	}
	if (flags & 1)
	{
		/* the lookup form: the declared keys of an object, found one by one */
		sift_shred_begin(sh);
		for (int i = 0, n = sift_shred_object_keys(sh); i < n; i++)
		{
			int			len;
			const char *key = sift_shred_object_key(sh, i, &len);

			if (len < 0 || len > SIFT_SHRED_MAX_KEY_BYTES * 4)
				fz_bad("object key %d has length %d", i, len);
			for (int k = 0; k < len; k++)
				(void) key[k];
		}
	}
	sift_shred_free(sh);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	Job			job = {data, size};

	if (size < 4)
		return 0;
	fz_init();
	fz_try(shred_run, &job);
	pgshim_reset();
	return 0;
}

static const char *const seeds[][2] = {
	{"a.b int4, a.c int4, d int4, e.f int4", "{\"a\":{\"b\":1},\"d\":null,\"e\":\"x\"}"},
	{"a[] int4", "{\"a\":[1,2,3,null,\"x\",4.5,{\"y\":1},[1]]}"},
	{"a[] int4", "{\"a\":[]}"},
	{"items[]{category text, kind text, qty int4}", "{\"items\":[{\"category\":\"x\",\"kind\":\"p\",\"qty\":1},{\"category\":\"x\",\"kind\":\"q\"},5,null,[1],{}]}"},
	{"items[]{category text, sku text, qty int4, price float4}, notes[]{type text}",
	 "{\"items\":[{\"category\":\"c\",\"sku\":\"s1\",\"qty\":3,\"price\":9.5},{\"sku\":\"s2\",\"price\":-0.25}],\"notes\":[{\"type\":\"t\"},{}]}"},
	{"m.* float8, n.*{x int4, y[] text}", "{\"m\":{\"a\":1.5,\"b\":2,\"c\":\"z\"},\"n\":{\"k1\":{\"x\":1,\"y\":[\"p\",\"q\"]},\"k2\":{\"x\":2}}}"},
	{"a int2, b int8, c bool, d text(4), e float8, f float4", "{\"a\":70000,\"b\":9223372036854775807,\"c\":true,\"d\":\"abcdefgh\",\"e\":1.7976931348623157,\"f\":3.5}"},
	{"a int8", "{\"a\":123456789012345678901234567890}"},
	{"m[][] float4, t[] text(64)", "{\"m\":[[1,2],[3],[]],\"t\":[\"a\",\"b\"]}"},
	{"a[]{b[]{c int4, d text}, e float8}, f int8", "{\"a\":[{\"b\":[{\"c\":1,\"d\":\"x\"},{\"c\":2}],\"e\":0.5},{\"b\":[]},{\"e\":3}],\"f\":7}"},
	{"a.*.* int4", "{\"a\":{\"x\":{\"p\":1,\"q\":2},\"y\":{\"p\":3}}}"},
	{" \"a.b\" -> \"c d\" text ,\"say \"\"hi\"\"\" int4", "{\"a.b\":{\"c d\":\"v\"},\"say \\\"hi\\\"\":4}"},
	{"naïve int4, ключ text", "{\"naïve\":1,\"ключ\":\"значение\"}"},
	{"a text", "{\"a\":\"\\u00e9\\n\\\"q\\\"\"}"},
	{"x int4", "[1,2,3]"},
	{"x int4", "5"},
	{"a[] int4, b.* text", "{\"a\":[1,2,3,4,5,6,7,8,9,10],\"b\":{\"k1\":\"v\",\"k2\":\"w\",\"k3\":\"u\",\"k4\":\"t\",\"k5\":\"s\"}}"},
};

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *dir = getenv("FZ_GEN");

	fz_init();
	pgshim_keep();
	if (dir)
	{
		mkdir(dir, 0755);
		for (size_t i = 0; i < lengthof(seeds); i++)
			for (int f = 0; f < 4; f++)
			{
				char		path[512];
				FILE	   *fp;
				uint8		flags = f == 0 ? 0 : f == 1 ? 1 : f == 2 ? 0x1c : 0x80;
				uint16		slen = (uint16) strlen(seeds[i][0]);

				snprintf(path, sizeof(path), "%s/shred-%zu-%d", dir, i, f);
				if (!(fp = fopen(path, "wb")))
					abort();
				fputc(flags, fp);
				fwrite(&slen, 2, 1, fp);
				fwrite(seeds[i][0], 1, slen, fp);
				fwrite(seeds[i][1], 1, strlen(seeds[i][1]), fp);
				fclose(fp);
			}
		_exit(0);
	}
	return 0;
}
