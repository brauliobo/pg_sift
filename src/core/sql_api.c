/*
 * core/sql_api.c - see sql_api.h.
 */
#include "postgres.h"

#include "access/relation.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "col/api/format.h"
#include "core/catname.h"
#include "core/damage.h"
#include "core/f2/manifest.h"
#include "core/inspect.h"
#include "core/maint.h"
#include "core/sql.h"
#include "core/sql_api.h"
#include "core/store.h"

static const char *const am_names[] = {
	[SIFT_API_BM25] = "sift",
	[SIFT_API_COL] = "sift_col",
	[SIFT_API_TABLE] = "sift_table"
};

const char *
sift_api_am_name(SiftApiAm am)
{
	return am_names[am];
}

bool
sift_api_am_by_oid(Oid amoid, SiftApiAm *am)
{
	const char *name = OidIsValid(amoid) ? get_am_name(amoid) : NULL;

	for (int i = 0; name != NULL && i < (int) lengthof(am_names); i++)
		if (strcmp(name, am_names[i]) == 0)
		{
			*am = (SiftApiAm) i;
			return true;
		}
	return false;
}

bool
sift_api_am_of(Relation rel, SiftApiAm *am)
{
	return sift_api_am_by_oid(rel->rd_rel->relam, am);
}

SiftApiRel
sift_api_open(Oid relid, LOCKMODE lock)
{
	SiftApiRel	r = {0};

	sift_inspect_expect(relid, NULL, NULL);
	r.rel = relation_open(sift_rel_must_exist(relid), lock);
	if (!sift_api_am_of(r.rel, &r.am))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift index or sift_table", RelationGetRelationName(r.rel))));
	sift_check_inspect(r.rel);
	r.peek = sift_meta_peek_format(r.rel);
	if (r.peek.status == SIFT_FMT_OTHER)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is in format %u.%u, which this release does not read", RelationGetRelationName(r.rel), r.peek.format_major, r.peek.format_minor),
				 errhint("A newer release of pg_sift wrote it.")));
	if (r.peek.status == SIFT_FMT_BAD)
		SIFT_DAMAGE_INDEX(r.rel, errmsg("\"%s\" has no metapage: block 0 is not one of pg_sift", RelationGetRelationName(r.rel)));
	r.f2 = r.peek.status == SIFT_FMT_V2;
	return r;
}

void
sift_api_close(SiftApiRel *r, LOCKMODE lock)
{
	relation_close(r->rel, lock);
	r->rel = NULL;
}

void
sift_api_check_desc(TupleDesc desc, const Oid *types, int ncols)
{
	SIFT_INVARIANT(desc->natts == ncols, "the result row has %d columns, the C function has %d", desc->natts, ncols);
	for (int i = 0; i < ncols; i++)
		SIFT_INVARIANT(TupleDescAttr(desc, i)->atttypid == types[i], "column %d (\"%s\") of the result row has type %u, the C function has %u",
					   i + 1, NameStr(TupleDescAttr(desc, i)->attname), TupleDescAttr(desc, i)->atttypid, types[i]);
}

Datum
sift_api_numeric2(int64 hundredths)
{
	uint64		a = hundredths < 0 ? -(uint64) hundredths : (uint64) hundredths;

	return DirectFunctionCall3(numeric_in, CStringGetDatum(psprintf("%s" UINT64_FORMAT ".%02d", hundredths < 0 ? "-" : "", a / 100, (int) (a % 100))),
							   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

Datum
sift_api_percent(uint64 part, uint64 whole)
{
	return sift_api_numeric2((int64) (((__int128) part * 10000 + whole / 2) / whole));
}

Datum
sift_api_ratio(uint64 num, uint64 den)
{
	return sift_api_numeric2((int64) (((__int128) num * 100 + den / 2) / den));
}

const char *
sift_api_shrink_blocked_by(uint16 shrink_blocked)
{
	return shrink_blocked == SIFT_SHRINK_OPEN ? NULL : sift_shrink_block_name((SiftShrinkBlock) shrink_blocked);
}

SiftApiF2Count
sift_api_f2_count(Relation rel)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	SiftApiF2Count c = {0};

	for (int i = 0; i < mr.n; i++)
	{
		c.parts += mr.entries[i].nparts;
		c.slabs += mr.entries[i].l0_slab_blk != InvalidBlockNumber;
	}
	return c;
}

/*
 * sift.index_format(rel): the metapage format of a sift index, a sift_col index or a sift_table, whichever format it is in; a relation with no block yet reports what a first write gives it.
 * long_words, for a sift (BM25) index only: it was built by a release that stores a word over 255 bytes as a prefix and a hash (an index of an earlier build says false, a REINDEX tells).
 * reads, for the other two: the column formats this build reads.
 */
Datum
sift_index_format(PG_FUNCTION_ARGS)
{
	SiftApiRel	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	bool		bm25 = r.am == SIFT_API_BM25;
	bool		empty = r.peek.status == SIFT_FMT_EMPTY;
	Datum		v[4] = {Int32GetDatum(empty ? sift_new_relation_format_major() : r.peek.format_major), Int32GetDatum(empty ? 0 : r.peek.format_minor)};
	bool		nulls[4] = {false, false, !bm25, bm25};

	if (bm25)
		v[2] = BoolGetDatum(empty || r.f2 || (sift_dir_read(r.rel)->meta.flags & SIFT_META_LONG_WORDS) != 0);
	else
		v[3] = CStringGetTextDatum(psprintf("%d to %d", SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MAX_READ));
	sift_api_close(&r, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}
