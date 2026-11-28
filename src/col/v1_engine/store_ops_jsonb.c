/*
 * col/v1_engine/store_ops_jsonb.c - SiftJsonbOps of Format 1 (core/store/ops.h).
 *
 * key_leaf: the shredded scalar leaf of a top-level key of a document whose chunks keep the JSON type of each row (a path declared with a type or as text), -1 for any other key. member_leaf: the leaf of
 * the float8 values of the map `doc.a.*` that a member `doc -> 'a' ->> 'k'` is read from (v1_store_ops_member.c), -1 for any other path.
 * path_declared: the paths option of the index declares the path of a document attribute.
 * key_exists: the source tests doc ? 'k' on the leaf of the key; the rows whose leaf has no value and may be a document that is no object (an array holding the string, the string itself) are
 * read again from the heap (sift_v1_src_filter_key), which also gives the presence column of the key (sift_v1_src_read_presence).
 * unnest_run: the aggregate scan of a LATERAL unnest of an array or a map column, of the keys of a map and of the tests of the elements of a row (v1_store_ops_unnest.c).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"

#include "col/v1/store_ops.h"
#include "col/v1/src_sel.h"
#include "col/v1/store_src.h"
#include "table/v1/rel.h"

static bool
typed_leaf(Oid typid)
{
	return typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == FLOAT4OID || typid == FLOAT8OID || typid == NUMERICOID || typid == TEXTOID;
}

static int
key_leaf(Oid relid, bool is_table, int att, const List *path)
{
	SiftV1Rel	r;
	const SiftSrcLayout *lay;
	int			found = -1;

	if (!is_table && get_rel_relkind(relid) != RELKIND_INDEX)
		return -1;
	r = sift_v1_rel_open(relid, is_table);
	lay = sift_v1_reader_of(is_table)->layout(r.rel);
	for (int i = 0; i < lay->nleaves && found < 0; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' && typed_leaf(l->typid) && list_length(path) == 1 && equal(l->path, path))
			found = i;
	}
	sift_v1_rel_close(&r);
	return found;
}

static int
jsonb_member_leaf(Oid indexoid, int att, const List *path)
{
	SiftV1Rel	r;
	int			leaf;

	if (get_rel_relkind(indexoid) != RELKIND_INDEX)
		return -1;
	r = sift_v1_rel_open(indexoid, false);
	leaf = sift_v1_member_value_leaf(sift_v1_idx_reader.layout(r.rel), att, path);
	sift_v1_rel_close(&r);
	return leaf;
}

/* the leaf is at the path, or is the object of free keys (`path.*`) whose member the path is */
static bool
leaf_declares(const SiftSrcLeaf *l, int att, const List *path)
{
	if (l->attno != att || l->parent >= 0)
		return false;
	if (l->kind == SCL_OBJECT)
		return list_length(path) == list_length(l->path) + 1 && equal(list_copy_head(path, list_length(l->path)), l->path);
	return equal(l->path, path);
}

static bool
jsonb_path_declared(Oid relid, bool is_table, int att, const List *path)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = sift_v1_reader_of(is_table)->layout(r.rel);
	bool		found = false;

	for (int i = 0; i < lay->nleaves && !found; i++)
		found = leaf_declares(&lay->leaf[i], att, path);
	sift_v1_rel_close(&r);
	return found;
}

/* the rows[0..n) of the unit whose document holds key as jsonb_exists says, set in bits from their heap rows (a row the snapshot does not see stays clear) */
static void
heap_exists(SiftV1Src *s, AttrNumber att, Datum key, const uint32 *rows, int n, uint64 *bits)
{
	ItemPointerData tids[SIFT_VEC_LEN];

	s->rd->tids(s->src, &s->unit, rows, n, tids);
	for (int j = 0; j < n; j++)
	{
		const uint64 tag = (uint64) ItemPointerGetBlockNumber(&tids[j]) << 16 | ItemPointerGetOffsetNumber(&tids[j]);
		Datum		doc;
		bool		isnull;

		if (sift_v1_src_fetch(&s->base, tag, &att, 1, &doc, &isnull) && !isnull && DatumGetBool(DirectFunctionCall2(jsonb_exists, doc, key)))
			bits[rows[j] >> 6] |= UINT64CONST(1) << (rows[j] & 63);
	}
}

/* the rows the leaf has a value of (any JSON type, a JSON null too), and of the others those whose heap row says the key is there; all of them when the unit cannot tell the types apart */
bool
sift_v1_src_filter_key(SiftV1Src *s, const SiftV1TypeTest *t, uint64 *bits)
{
	const SiftSrcLeaf *l = &s->lay->leaf[t->leaf];
	const AttrNumber att = sift_v1_att_heap_column(s->lay, l->attno);
	const int	nw = SIFT_COL_NWORDS(s->unit.nrow);
	MemoryContext old = MemoryContextSwitchTo(s->unit_cx);
	uint64	   *given = palloc(nw * sizeof(uint64));
	const Datum key = CStringGetTextDatum(strVal(linitial(l->path)));
	SiftSrcVec	v = {0};
	uint32		rows[SIFT_VEC_LEN];
	int			n = 0;
	bool		known;

	memcpy(given, bits, nw * sizeof(uint64));
	s->rd->filter_types(s->src, &s->unit, l->id, t->types, bits, &known);
	if (!known)
		memset(bits, 0, nw * sizeof(uint64));
	else if (!t->rootless)
		s->rd->read(s->src, &s->unit, l->id, &v);
	for (uint32 r = 0; r < (uint32) s->unit.nrow; r++)
	{
		if (!sift_src_bit(given, r) || sift_src_bit(bits, r) || (known && !t->rootless && (v.ovf == NULL || !sift_src_bit(v.ovf, r))))
			continue;
		if (n == SIFT_VEC_LEN)
		{
			heap_exists(s, att, key, rows, n, bits);
			n = 0;
		}
		rows[n++] = r;
	}
	heap_exists(s, att, key, rows, n, bits);
	MemoryContextSwitchTo(old);
	return sift_srcsel_rows_left(bits, s->unit.nrow);
}

/* a SQL NULL document has not the key: its heap row clears it as jsonb_exists of no document would */
void
sift_v1_src_read_presence(SiftV1Src *s, int i)
{
	const int	nw = SIFT_COL_NWORDS(s->unit.nrow);
	const SiftV1TypeTest t = {.leaf = s->jleaf[i] - s->lay->leaf,.types = SIFT_SRC_JT_ALL & ~SIFT_SRC_JT_NONE,.key = true,.rootless = s->jrootless[i]};
	uint64	   *has = MemoryContextAlloc(s->unit_cx, nw * sizeof(uint64));

	memcpy(has, s->bits, nw * sizeof(uint64));
	sift_v1_src_filter_key(s, &t, has);
	s->jtag[i] = MemoryContextAlloc(s->unit_cx, Max(s->unit.nrow, 1));
	for (uint32 r = 0; r < (uint32) s->unit.nrow; r++)
		s->jtag[i][r] = sift_src_bit(has, r);
}

void
sift_v1_src_put_presence(SiftV1Src *s, int i, uint32 r, int n)
{
	s->valid[i][n >> 6] |= UINT64CONST(1) << (n & 63);
	((int64 *) s->data[i])[n] = s->jtag[i][r];
}

const SiftJsonbOps v1_jsonb_ops = {
	.key_leaf = key_leaf,
	.member_leaf = jsonb_member_leaf,
	.key_exists = true,
	.key_presence = true,
	.path_declared = jsonb_path_declared,
	.table_elems = true,
	.unnest_run = sift_v1_unnest_run,
	.unnest_arms = sift_v1_unnest_run_arms
};
