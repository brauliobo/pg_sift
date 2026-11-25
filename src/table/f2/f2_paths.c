/*
 * table/f2/f2_paths.c - the paths a sift_table declares for its jsonb columns (sift.table_set_paths) as leaves of its store.
 *
 * The grammar and the shredder are those of the `paths` option of a sift_col index. The leaf of the column keeps the exact bytes of the document; the leaves of its paths follow it, so a part
 * written under a declaration holds both and a part written before it holds the first only. A part names its leaves by the attribute and the hash of the path (SiftPartColIdentity), which is how
 * a store finds what a part has of the paths it declares now (f2_ddl.c).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/pg.h"
#include "table/f2/f2_tbl.h"

char *
sift_tbl_f2_declared_paths(Relation rel, Oid paths_relid, MemoryContext cx)
{
	Oid			nsp = get_namespace_oid("sift", true);
	Oid			reloid = OidIsValid(nsp) ? get_relname_relid("table_paths", nsp) : InvalidOid;
	TupleDesc	desc = RelationGetDescr(rel);
	StringInfoData spec;
	Relation	pr;
	Snapshot	snap;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;

	if (!OidIsValid(reloid))
		return NULL;
	initStringInfo(&spec);
	pr = table_open(reloid, AccessShareLock);
	snap = RegisterSnapshot(GetTransactionSnapshot());
	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(paths_relid));
	scan = systable_beginscan(pr, InvalidOid, false, snap, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		Name		name = DatumGetName(heap_getattr(tup, 2, RelationGetDescr(pr), &isnull));

		for (int a = 1; a <= desc->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, a - 1);

			if (!att->attisdropped && att->atttypid == JSONBOID && namestrcmp(name, NameStr(att->attname)) == 0)
				appendStringInfo(&spec, "%s%s", spec.len ? ", " : "", TextDatumGetCString(heap_getattr(tup, 3, RelationGetDescr(pr), &isnull)));
		}
	}
	systable_endscan(scan);
	UnregisterSnapshot(snap);
	table_close(pr, AccessShareLock);
	return spec.len ? MemoryContextStrdup(cx, spec.data) : NULL;
}

int
sift_tbl_f2_path_leaf_count(Relation rel, SiftF2Store *store, const char *paths)
{
	int			n = 0;

	if (paths == NULL)
		return 0;
	store->paths = sift_col_shred_create_for(store->cx, RelationGetDescr(rel), RelationGetDescr(rel)->natts, paths);
	sift_col_shred_keep_payloads(store->paths);
	for (int a = 1; a <= store->natts; a++)
		if (sift_f2_attr_has_leaf(TupleDescAttr(RelationGetDescr(rel), a - 1)))
			n += sift_col_shred_nleaves(store->paths, a);
	return n;
}

/* What a part keeps to tell this leaf from the others of its column: the hash of the path and of how its values are held, in the 24 bits of the identity that a column leaves free */
static uint32
path_hash(const SiftColDef *d)
{
	const char *below = strncmp(d->path, d->name, strlen(d->name)) == 0 ? d->path + strlen(d->name) : d->path;	/* under the name of its column: a rename of the column keeps the leaves */
	uint32		h = hash_bytes((const unsigned char *) below, strlen(below));

	return hash_combine(h, (uint32) d->kind << 24 | (uint32) d->store << 16 | (uint32) d->type << 8 | (d->flags & 0xFF)) & 0x00FFFFFF;
}

void
sift_tbl_f2_path_identity(const SiftF2Store *store, int leaf, uint8 *flags, uint16 *hash16)
{
	const uint32 h = sift_f2_leaf_is_path(store, leaf) ? path_hash(&store->path_def[leaf]) : 0;

	*flags = (uint8) (h >> 16);
	*hash16 = (uint16) h;
}

void
sift_tbl_f2_paths_changed(Relation rel)
{
	sift_tbl_f2_flush_all_slabs(rel);
	sift_f2_store_invalidate(rel);
}

int
sift_tbl_f2_add_path_leaves(Relation rel, SiftF2Store *store, int a, int n)
{
	const int	k = sift_col_shred_nleaves(store->paths, a);
	const char *name = NameStr(TupleDescAttr(RelationGetDescr(rel), a - 1)->attname);

	store->path_first[a] = k > 0 ? n : -1;
	for (int i = 0; i < k; i++)
	{
		SiftColDef *d = &store->path_def[n + i];

		sift_col_shred_leaf(store->paths, a, i, d);
		d->id = n + i;
		d->attno = a;
		d->name = MemoryContextStrdup(store->cx, name);
		d->parent = d->parent < 0 ? -1 : n + d->parent;
		store->leaf_attno[n + i] = a;
		store->kinds[n + i] = sift_f2_def_kind(d);
		store->hints[n + i] = sift_f2_def_hint(d);
		store->scales[n + i] = d->scale;
		store->parent[n + i] = d->parent;
		store->has_groups = store->has_groups || d->kind == SIFT_COLK_GROUP;
		for (int j = 0; j < i; j++)
			if (path_hash(&store->path_def[n + j]) == path_hash(d))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("the paths \"%s\" and \"%s\" of column \"%s\" of table \"%s\" cannot be told apart", store->path_def[n + j].path,
																				 d->path, name, RelationGetRelationName(rel))));
	}
	return n + k;
}

/* ---- whether every part has the leaves ---- */

/* A part written before a declaration lacks the leaves it makes, and a store that has them gives no leaf of the paths to a scan until a rewrite has taken those parts out */
static bool
part_has_paths(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint64 part_id)
{
	SiftTblF2Extra *x = sift_tbl_extra(store);
	bool		found;
	SiftF2ColMap *map;
	bool		ready = true;
	MemoryContext tmp;
	MemoryContext old;

	if (x->path_parts == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(uint64),.entrysize = sizeof(uint64),.hcxt = store->cx};

		/* MEM[MEM-TBL-18]: the parts of a table known to hold every leaf of its declared paths (8 B a part; a table that declares none has none) */
		x->path_parts = hash_create("sift table path parts", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	(void) hash_search(x->path_parts, &d->part_gen, HASH_FIND, &found);
	if (found)
		return true;
	/* MEM[MEM-TBL-19]: transient decode ctx of one part's schema (ALLOCSET_SMALL_SIZES) */
	tmp = AllocSetContextCreate(CurrentMemoryContext, "sift table path parts check", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(tmp);
	map = sift_tbl_f2_build_col_map(store, rel, sift_part_open(rel, d->header_blk, d->part_gen, (uint32) part_id), tmp);
	for (int c = 0; map != NULL && c < store->ncols; c++)
		ready = ready && !(map->phys_col[c] < 0 && sift_f2_leaf_is_path(store, c));
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	if (ready)
		(void) hash_search(x->path_parts, &d->part_gen, HASH_ENTER, &found);
	return ready;
}

bool
sift_tbl_f2_paths_ready(Relation rel, SiftF2Store *store)
{
	const SiftPlanSnapshot *snap = sift_plan_cache_get(rel, CurrentMemoryContext);

	for (int i = 0; i < snap->npartitions; i++)
		for (int k = 0; k < snap->partlists[i].nparts; k++)
			if (!part_has_paths(rel, store, &snap->partlists[i].parts[k], snap->partitions[i].part_id))
				return false;
	return true;
}
