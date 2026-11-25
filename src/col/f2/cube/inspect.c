/*
 * col/f2/cube/inspect.c - sift.scope_stats(index): the Entity-Once scopes of a sift_col index or sift_table of Format 2.0, one row each, from the summary object each scope keeps as a part of the
 * index (col/f2/cube/meta.h): where it is, its keys, the levels of its summary, its bytes, the keys that wait for a refresh and whether the entity rows are clustered by the key.
 *
 * A scope has one level 2 root for each key; topk_keys counts the roots that keep a ring of the newest rows. dirty_keys is 0 for a summary that describes the parts and the pairs as they are
 * (sift_f2_scope_current) and NULL when it does not: the count of the keys that changed is known only to the refresh that makes them. The name of a scope is that of its declaration, in the
 * options of the index or of the table. No row for Format 1 or for a relation without a summary.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "storage/lmgr.h"

#include "col/f2/meta.h"
#include "col/f2/scope/scope.h"
#include "col/scope/scope.h"
#include "core/damage.h"
#include "core/sql/api.h"

PG_FUNCTION_INFO_V1(sift_scope_stats);

/* columns of sift.scope_stats, in the order of sql/6.0/15-unified-api.sql */
enum ScopeCol
{
	SC_SCOPE_ORD,
	SC_SCOPE_NAME,
	SC_HEADER_BLK,
	SC_META_GEN,
	SC_NKEYS,
	SC_L0_SLICES,
	SC_L1_NODES,
	SC_L2_ROOTS,
	SC_TOPK_KEYS,
	SC_BYTES,
	SC_DIRTY_KEYS,
	SC_CLUSTERED,
	SC_NCOLS
};

static const Oid scope_types[SC_NCOLS] = {
	INT2OID, TEXTOID, INT8OID, INT8OID, INT8OID, INT4OID, INT4OID, INT4OID, INT4OID, INT8OID, INT4OID, BOOLOID
};

static int32
roots_with_ring(const SiftScopeMeta *m)
{
	int32		n = 0;

	for (uint32 k = 0; k < m->hdr.nkeys; k++)
		n += m->roots[k].topk_count > 0;
	return n;
}

static void
scope_row(ReturnSetInfo *rsi, const SiftApiRel *r, const SiftManifestScopeRef *ref)
{
	SiftPart   *part = sift_part_open(r->rel, ref->header_blk, ref->meta_gen, 0);
	uint64		len;
	char	   *obj = sift_f2_scope_read_object(r->rel, part, &len);
	SiftScopeMeta m;
	SiftScopeSpecs *specs = sift_scope_specs(r->rel);
	Datum		v[SC_NCOLS] = {0};
	bool		nulls[SC_NCOLS] = {false};

	if (obj == NULL || !sift_scope_meta_open(&m, obj, len))
		SIFT_DAMAGE_INDEX(r->rel, errmsg("the summary of scope %u of \"%s\" is not readable", ref->scope_ord, RelationGetRelationName(r->rel)));
	v[SC_SCOPE_ORD] = Int16GetDatum((int16) ref->scope_ord);
	nulls[SC_SCOPE_NAME] = specs == NULL || ref->scope_ord >= specs->n;
	if (!nulls[SC_SCOPE_NAME])
		v[SC_SCOPE_NAME] = CStringGetTextDatum(specs->s[ref->scope_ord]->key_name);
	v[SC_HEADER_BLK] = Int64GetDatum((int64) ref->header_blk);
	v[SC_META_GEN] = Int64GetDatum((int64) ref->meta_gen);
	v[SC_NKEYS] = Int64GetDatum((int64) m.hdr.nkeys);
	v[SC_L0_SLICES] = Int32GetDatum((int32) m.hdr.nl0_slices);
	v[SC_L1_NODES] = Int32GetDatum((int32) m.hdr.nl1_nodes);
	v[SC_L2_ROOTS] = Int32GetDatum((int32) m.hdr.nkeys);
	v[SC_TOPK_KEYS] = Int32GetDatum(roots_with_ring(&m));
	v[SC_BYTES] = Int64GetDatum((int64) part->hdr.stream_bytes);
	nulls[SC_DIRTY_KEYS] = !sift_f2_scope_current(r->rel, ref->scope_ord);
	v[SC_DIRTY_KEYS] = Int32GetDatum(0);
	v[SC_CLUSTERED] = BoolGetDatum((m.hdr.flags & SIFT_SMETA_CLUSTERED) != 0);
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	sift_scope_meta_close(&m);
}

Datum
sift_scope_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftApiRel	r;

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(rsi->setDesc, scope_types, SC_NCOLS);
	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	if (r.am == SIFT_API_BM25)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is a sift index, which has no scopes", RelationGetRelationName(r.rel))));
	if (r.f2)
	{
		SiftColMetaPageData meta = sift_col_meta_read(r.rel);
		int			nref;
		SiftManifestScopeRef *refs = sift_manifest_read_scopes(r.rel, &meta, &nref);

		for (int i = 0; i < nref; i++)
			if (refs[i].flags & SIFT_SCOPEREF_ACTIVE)
				scope_row(rsi, &r, &refs[i]);
	}
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}
