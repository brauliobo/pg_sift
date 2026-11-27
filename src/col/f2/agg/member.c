/*
 * col/f2/agg/member.c - a member of a document that a column of the scan reads by a path (SiftV6Col.jmember) and converts as the statement does: of a map (`doc.* float8`, `doc.a.* float8`), found
 * by its key, where the group leaf of the map gives each row the number of its members, which follow those of the rows before it in the leaves of the keys and of the values; of a path the index
 * declares, its leaf. A value the leaf could not hold is the payload of its exception, and so is a map the leaves could not hold (more members than the index keeps, or no object, which has none).
 */
/* check-memory: allow-allocs the member of a column follows the columns of one statement, the payloads of a batch live in its batch context */
#include "postgres.h"

#include "agg/vexpr/str.h"
#include "col/f2/agg/scan_int.h"
#include "col/f2/store/store.h"

typedef enum CellKind
{
	MC_NULL,
	MC_FLOAT,					/* f: the number the leaf holds */
	MC_JSONB					/* pay: the jsonb of the member */
} CellKind;

typedef struct Cell
{
	CellKind	kind;
	double		f;
	const char *pay;
	uint32		plen;
} Cell;

V6Member *
sift_f2_v6_member_new(const V6Scan *s, const SiftV6Col *c)
{
	const SiftColSchema *sch = s->is_table ? sift_f2_store_open(s->host)->schema : sift_col_rel(s->host)->schema;
	const char *name = strVal(llast(sift_v6_member_path(s->spec, c->jmember)));
	V6Member   *m = palloc(sizeof(V6Member));

	*m = (V6Member) {.group = sch->cols[c->jleaf - 1].parent,.key = -1,.name = name,.len = strlen(name)};
	for (int l = 0; l < sch->ncols; l++)
		if (sch->cols[l].parent == m->group && strstr(sch->cols[l].path, "#key") != NULL)
			m->key = l;
	SIFT_INVARIANT(m->key >= 0, "the map of a member the scan reads has a leaf of its keys");
	return m;
}

/* the members of row r of a map (of a path the index declares: none) */
static uint32
row_members(const V6Col *c, const ColView *views, int r)
{
	return c->member == NULL || views[c->member->group].isnull[r] ? 0 : (uint32) views[c->member->group].i[r];
}

static Cell
jsonb_cell(V6Scan *s, const char *payload, uint32 len, const char *name)
{
	Cell		c = {.kind = MC_JSONB};

	return sift_v6_json_payload_member(payload, len, list_make1(makeString((char *) name)), s->batch_cx, &c.pay, &c.plen) ? c : (Cell) {.kind = MC_NULL};
}

/* a row whose leaves hold no map: no document or no value at the map (no member), a value that is no object (no member either), or a map the index cut, whose payload holds it whole */
static Cell
cut_cell(V6Scan *s, const V6Col *c, const ColView *g, int r)
{
	if (g->tag == NULL || g->tag[r] != SIFT_COLTAG_OBJECT)
		return (Cell) {.kind = MC_NULL};
	if (g->xpay == NULL || g->xpay[r] == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a map whose member the aggregate reads is not kept whole by the index"),
						errhint("Run the statement with sift.enable_agg = off.")));
	return jsonb_cell(s, g->xpay[r], g->xplen[r], c->member->name);
}

/* the value at i of a leaf of numbers: the number it holds, the payload of the exception of a value it could not hold, or none */
static Cell
leaf_cell(const ColView *v, uint32 i)
{
	if (!v->isnull[i])
		return (Cell) {.kind = MC_FLOAT,.f = v->f[i]};
	return v->xpay != NULL && v->xpay[i] != NULL ? (Cell) {.kind = MC_JSONB,.pay = v->xpay[i],.plen = v->xplen[i]} : (Cell) {.kind = MC_NULL};
}

/* the member of row r, whose elements are [first, first + n) of the leaves of the keys and the values of its map; of a path the index declares, its leaf */
static Cell
member_cell(V6Scan *s, const V6Col *c, const ColView *views, int r, uint32 first, uint32 n)
{
	const ColView *g,
			   *k,
			   *v = &views[c->leaf];

	if (c->member == NULL)
		return leaf_cell(v, (uint32) r);
	g = &views[c->member->group];
	k = &views[c->member->key];
	if (g->isnull[r])
		return cut_cell(s, c, g, r);
	for (uint32 e = first; e < first + n; e++)
	{
		SiftStrView key;

		if (k->isnull[e])
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a map whose member the aggregate reads has a key the index could not keep"),
							errhint("Run the statement with sift.enable_agg = off.")));
		key = sift_f2_v6_text_view(k, (int) e, false);
		if (key.len == c->member->len && memcmp(sift_vstr_bytes(&key), c->member->name, key.len) == 0)
			return leaf_cell(v, e);
	}
	return (Cell) {.kind = MC_NULL};
}

/* the cell as the value of the column at i: false when its cast raises */
static bool
put_cell(V6Scan *s, V6Col *c, int i, const Cell *m)
{
	SiftV6JsonValue val = {0};
	SiftV6JsonExc res = SV6JX_NULL;

	if (m->kind == MC_FLOAT && c->typid == FLOAT8OID)
	{
		val.value = Float8GetDatum(m->f);
		res = SV6JX_VALUE;
	}
	else if (m->kind == MC_FLOAT)
		res = sift_v6_json_float_int(c->typid, m->f, &val.value) ? SV6JX_VALUE : SV6JX_RAISES;
	else if (m->kind == MC_JSONB)
		res = sift_v6_json_exception_read(c->typid, c->jsonb_cast, m->pay, m->plen, s->batch_cx, &val);
	if (res != SV6JX_VALUE)
		return res != SV6JX_RAISES;
	if (c->typid == FLOAT8OID)
		((double *) c->vec.data)[i] = DatumGetFloat8(val.value);
	else
		((int64 *) c->vec.data)[i] = sift_v6_json_int(c->typid, val.value);
	c->valid[i >> 6] |= UINT64CONST(1) << (i & 63);
	return true;
}

static uint32
members_before(const V6Col *c, const ColView *views, int r)
{
	uint32		e = 0;

	for (int i = 0; i < r; i++)
		e += row_members(c, views, i);
	return e;
}

uint64 *
sift_f2_v6_fill_member(V6Scan *s, V6Col *c, const ColView *views, int off, int n)
{
	SiftVec    *v = &c->vec;
	uint64	   *raises = NULL;
	uint32		e = members_before(c, views, off);

	memset(c->valid, 0, sizeof(c->valid));
	for (int i = 0; i < n; e += row_members(c, views, off + i), i++)
	{
		Cell		m;

		if (!((s->bs.mask[i >> 6] >> (i & 63)) & 1))
			continue;
		m = member_cell(s, c, views, off + i, e, row_members(c, views, off + i));
		if (put_cell(s, c, i, &m))
			continue;
		raises = raises != NULL ? raises : MemoryContextAllocZero(s->batch_cx, SIFT_VEC_WORDS * sizeof(uint64));
		raises[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	*v = (SiftVec) {.data = v->data,.valid = c->valid,.len = n,.all_valid = false,.dense = true,.type = c->typid == FLOAT8OID ? SVEC_F64 : SVEC_I64};
	return raises;
}

void
sift_f2_v6_member_raise(V6Scan *s, const V6Col *c, const ColView *views, int r)
{
	const Cell	m = member_cell(s, c, views, r, members_before(c, views, r), row_members(c, views, r));

	if (m.kind == MC_FLOAT)
		sift_v6_json_float_int_raise(c->typid, m.f);
	sift_v6_json_exception_raise(c->typid, c->jsonb_cast, m.pay, m.plen, s->batch_cx);
}
