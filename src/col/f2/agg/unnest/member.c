/*
 * col/f2/agg/unnest/member.c - a member of a document read by a path (SiftV6Col.jmember) as a column of the rows of an unnest. The pull reads a map as one more array of the row (its lengths, keys
 * and values), in which the member of each row is found by its key, and a path the index declares as the column of its leaf. The rows whose member the pull cannot hold (the slab, an exception of a
 * chunk, a key the index could not keep, a value no number) are read from the heap, the member from their document; the cast raises for a row the statement keeps, as PostgreSQL's does.
 */
/* check-memory: allow-allocs the maps follow the columns of one statement, the values of a batch live in its batch context */
#include "postgres.h"

#include "access/htup_details.h"
#include "utils/jsonb.h"

#include "agg/vexpr/str.h"
#include "col/f2/agg/unnest/int.h"

/* the map whose values are leaf value, among those the pull reads */
static int
map_of(UnRun *u, const UnLeaves *r, int value)
{
	const int	group = r->schema->cols[value].parent;
	UnMap		m = {.group = group,.key = -1,.value = value};

	for (int i = 0; i < u->nmaps; i++)
		if (u->map[i].group == group)
			return i;
	for (int l = 0; l < r->schema->ncols; l++)
		if (r->schema->cols[l].parent == group && strstr(r->schema->cols[l].path, "#key") != NULL)
			m.key = l;
	SIFT_INVARIANT(m.key >= 0, "the map of a member the scan reads has a leaf of its keys");
	if (u->nst + u->nmaps == SIFT_F2_PULL_MAX_GROUPS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d arrays and maps of a row", SIFT_F2_PULL_MAX_GROUPS),
						errhint("Read the members of fewer maps of the table that is unnested.")));
	u->map[u->nmaps] = m;
	return u->nmaps++;
}

int
sift_f2_unnest_member_col(UnRun *u, const UnLeaves *r, const SiftV6Col *c)
{
	const bool	length = r->schema->cols[c->jleaf - 1].kind == SIFT_COLK_GROUP;
	const int	map = !length && r->schema->cols[c->jleaf - 1].parent >= 0 ? 1 + map_of(u, r, c->jleaf - 1) : 0;
	const int	leaf = map > 0 ? u->map[map - 1].group : c->jleaf - 1;
	List	   *path = sift_v6_member_path(u->spec, c->jmember);
	const char *key = length ? "" : strVal(llast(path));

	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].member && u->cols[i].leaf == leaf && u->cols[i].jsonb_cast == c->jsonb_cast && u->cols[i].typid == c->typid && strcmp(u->cols[i].key, key) == 0)
			return i;
	if (u->ncols == SIFT_F2_PULL_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_F2_PULL_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	u->cols[u->ncols] = (UnCol) {.leaf = leaf,.cls = sift_v6_class(c->typid),.json = true,.typid = c->typid,.read = true,.jsonb_cast = c->jsonb_cast,
		.heap_att = heap_attno(u, c->attno),.key = key,.keylen = strlen(key),.member = true,.map = map,.path = path,.length = length};
	return u->ncols++;
}

/* the cast of the member the leaves hold (a number) or the heap gives (a jsonb), at row r of the column: raised for a row the statement keeps, as PostgreSQL's cast is */
static void
put(UnRun *u, int i, uint32 r, bool number, double f, const char *pay, uint32 len)
{
	const UnCol *c = &u->cols[i];
	SiftV6JsonValue val = {.value = Float8GetDatum(f)};
	SiftV6JsonExc res = SV6JX_VALUE;

	if (number && c->typid != FLOAT8OID && sift_v6_json_float_int(c->typid, f, &val.value) != SV6JX_VALUE)
		sift_v6_json_float_int_raise(c->typid, f);
	if (!number)
		res = sift_v6_json_exception_read(c->typid, c->jsonb_cast, pay, len, u->bcx, &val);
	if (res == SV6JX_RAISES)
		sift_v6_json_exception_raise(c->typid, c->jsonb_cast, pay, len, u->bcx);
	if (res != SV6JX_VALUE)
		return;
	if (c->typid == FLOAT8OID)
		((double *) u->jdata[i])[r] = DatumGetFloat8(val.value);
	else
		((int64 *) u->jdata[i])[r] = sift_v6_json_int(c->typid, val.value);
	u->jvalid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
}

/* the number of elements of the list of a row of the heap, whose value that is no array raises as jsonb_array_length does */
static void
put_length(UnRun *u, int i, uint32 r, const char *pay, uint32 len)
{
	int64		n;

	if (!sift_v6_json_array_length(pay, len, u->bcx, &n))
		sift_v6_json_array_length_raise(pay, len, u->bcx);
	((int64 *) u->jdata[i])[r] = n;
	u->jvalid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
}

/* the member of row r from the document of the heap row in the slot of the vet */
static void
slot_member(UnRun *u, int i, uint32 r)
{
	const UnCol *c = &u->cols[i];
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	bool		isnull;
	const Datum doc = slot_getattr(u->vet.slot, c->heap_att, &isnull);
	const char *pay;
	uint32		len;

	if (!isnull && sift_v6_json_payload_member((const char *) DatumGetJsonbP(doc), VARSIZE(DatumGetJsonbP(doc)), c->path, u->bcx, &pay, &len))
		c->length ? put_length(u, i, r, pay, len) : put(u, i, r, false, 0, pay, len);
	MemoryContextSwitchTo(old);
}

/* the member of a row the pull cannot hold, from the document of its row of the heap */
static void
heap_member(UnRun *u, int i, uint64 tid, uint32 r)
{
	const bool	found = sift_v6_vet_fetch(&u->vet, tid);

	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its map is read");
	slot_member(u, i, r);
}

/* the number a leaf holds of the member at row r: the document's when the integer of its cast is not known from the leaf */
static void
number_member(UnRun *u, int i, uint64 tid, uint32 r, double f)
{
	Datum		ignored;

	if (u->cols[i].typid != FLOAT8OID && sift_v6_json_float_int(u->cols[i].typid, f, &ignored) == SV6JX_INEXACT)
		heap_member(u, i, tid, r);
	else
		put(u, i, r, true, f, NULL, 0);
}

/* the element of the member in the elements [lo, hi) of a row, SIFT_GATHER_NONE for none; false when a key is one the index could not keep */
static bool
find(const UnCol *c, const SiftF2ElemStream *es, uint32 lo, uint32 hi, uint32 *at)
{
	const SiftStrView *keys = es->child_data[0];

	*at = SIFT_GATHER_NONE;
	for (uint32 e = lo; e < hi; e++)
	{
		if (!((es->child_valid[0][e >> 6] >> (e & 63)) & 1))
			return false;
		if (keys[e].len == (uint32) c->keylen && memcmp(sift_vstr_bytes(&keys[e]), c->key, c->keylen) == 0)
			*at = e;
	}
	return true;
}

/* the member of a map at row r: the number its element of the key holds, none, or the heap's when the pull cannot hold the map of the row */
static void
map_member(UnRun *u, int i, const SiftF2ExtBatch *b, uint32 r)
{
	const SiftF2ElemStream *es = &b->elem[u->nst + u->cols[i].map - 1];
	uint32		at;

	if (es->layout.n_parents < (uint32) b->n || ((es->poison[r >> 6] >> (r & 63)) & 1) || !find(&u->cols[i], es, es->layout.offsets[r], es->layout.offsets[r + 1], &at))
		heap_member(u, i, b->tid[r], r);
	else if (at != SIFT_GATHER_NONE && ((es->child_valid[1][at >> 6] >> (at & 63)) & 1))
		number_member(u, i, b->tid[r], r, ((const double *) es->child_data[1])[at]);
}

void
sift_f2_unnest_member_column(UnRun *u, int i, const SiftF2ExtBatch *b, const SiftBatchSel *bs)
{
	const SiftVec *leaf = &b->col[i];

	memset(u->jvalid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (int w = 0; w < (b->n + 63) / 64; w++)
		for (uint64 bits = bs->mask[w] & ~b->elem->poison[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 r = w * 64 + (uint32) __builtin_ctzll(bits);

			if (u->cols[i].map > 0)
				map_member(u, i, b, r);
			else if (u->cols[i].length && (leaf->all_valid || ((leaf->valid[r >> 6] >> (r & 63)) & 1)))
			{
				((int64 *) u->jdata[i])[r] = ((const int64 *) leaf->data)[r];
				u->jvalid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
			}
			else if (leaf->all_valid || ((leaf->valid[r >> 6] >> (r & 63)) & 1))
				number_member(u, i, b->tid[r], r, ((const double *) leaf->data)[r]);
			else
				heap_member(u, i, b->tid[r], r);
		}
	u->fv[i] = (SiftVec) {.data = u->jdata[i],.valid = u->jvalid[i],.len = b->n,.all_valid = false,.dense = true,.type = u->cols[i].typid == FLOAT8OID ? SVEC_F64 : SVEC_I64};
}

void
sift_f2_unnest_member_heap_row(UnRun *u, uint32 r)
{
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].member)
			slot_member(u, i, r);
}
