/*
 * col/f2/f2_scope_cur.c - whether the summary of a scope is what the entity parts and the pairs are now, and whether a snapshot may read it (f2_scope.h), for an index and for a sift_table: the parts
 * it names, the schema of the leaves, the journals of the pairs, the visibility of the rows of its parts, and for a table the column deltas of its partitions.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/snapmgr.h"

#include "col/f2/f2_scope.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_vis.h"
#include "col/scope.h"
#include "table/f2/f2_col_delta.h"

#include "col/f2/f2_scope_int.h"

/* The summary of scope ord as the directory names it, opened in *m with open_flags (the caller closes it; SIFT_SMETA_OPEN_LAZY_TOPK leaves the rings packed for a read of the header and the parts); false when the scope has none */
bool
sift_f2s_object(Relation rel, int ord, SiftScopeMeta *m, uint32 open_flags)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			n;
	SiftManifestScopeRef *refs = sift_manifest_read_scopes(rel, &meta, &n);

	for (int i = 0; i < n; i++)
		if (refs[i].scope_ord == ord && (refs[i].flags & SIFT_SCOPEREF_ACTIVE))
		{
			uint64		len;
			char	   *bytes = sift_f2_scope_read_object(rel, sift_part_open(rel, refs[i].header_blk, refs[i].meta_gen, 0), &len);

			if (bytes == NULL || !sift_scope_meta_open_ex(m, bytes, len, open_flags))
				SIFT_CORRUPT(rel, "The summary of scope %d is damaged.", ord);
			return true;
		}
	return false;
}

void
sift_f2s_meta_map(const SiftScopeMeta *m, SiftGenMap *gm)
{
	sift_f2s_gen_map_init(gm, m->hdr.nparts);
	for (uint32 e = 0; e < m->hdr.nparts; e++)
	{
		uint32		have;

		if (m->parts[e].part_gen != 0 && !sift_f2s_gen_map_find(gm, m->parts[e].part_gen, &have))
			sift_f2s_gen_map_put(gm, m->parts[e].part_gen, e);
	}
}

/*
 * The summary names the parts of the manifest and no other. settled: the inserters of each are older than the snapshot and committed (the deletion vectors are read by the entity side
 * under the snapshot, a deleter at a time), and no partition keeps a slab or a pending delete: what the cubes count is then what the heap, or the versions of the table, show the snapshot.
 */
static bool
parts_match(Relation rel, const SiftManifestRead *manifest, const SiftScopeMeta *m, Snapshot snap, bool settled)
{
	const SiftManifestRead mr = *manifest;
	FullTransactionId xmin = sift_vis_snap_xmin(snap);
	uint32		named = 0,
				found = 0,
				at;
	SiftGenMap	gm;

	sift_f2s_meta_map(m, &gm);
	for (uint32 e = 0; e < m->hdr.nparts; e++)
		named += m->parts[e].part_gen != 0;
	for (int k = 0; k < mr.n; k++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[k], &np);

		if (settled && (mr.entries[k].l0_slab_blk != InvalidBlockNumber || mr.entries[k].pend_del_blk != InvalidBlockNumber))
			return false;
		for (int j = 0; j < np; j++)
		{
			if (!sift_f2s_gen_map_find(&gm, parts[j].part_gen, &at))
				return false;
			if (settled && !sift_vis_part_inserts_settled(&parts[j], xmin))
				return false;
			found++;
		}
	}
	return found == named;
}

/* The columns the summary reads or may be asked to read, as the bloom of attributes a column delta page keeps */
static uint64
read_bloom(const SiftF2Store *store, const SiftF2ScopeCfg *sc)
{
	uint64		bloom = 0;

	for (int l = 0; l < store->ncols; l++)
		if (sift_f2s_is_table(store) ? l == sc->member_leaf || l == sc->time_leaf || sift_f2s_kept(store, sc->spec, l) : sift_f2s_reads_leaf(store, sc, l))
			bloom |= sift_cd_att_bit(store->leaf_attno[l]);
	return bloom;
}

bool
sift_f2s_deltas_clear(Relation rel, const SiftF2Store *store, int ord, const SiftManifestRead *mr)
{
	SiftF2ScopeCfg sc;
	uint64		bloom;

	sift_f2s_cfg(rel, store, ord, &sc);
	bloom = read_bloom(store, &sc);
	for (int k = 0; k < mr->n; k++)
	{
		SiftColDeltaChain chain;
		bool		clear = true;

		if (!sift_col_delta_load_chain(rel, &mr->entries[k], 0, &chain))
			continue;
		for (int i = 0; i < chain.npages; i++)
			clear &= (sift_col_delta_page_hdr(sift_col_delta_chain_page(&chain, i))->attnum_bloom & bloom) == 0;
		sift_col_delta_chain_free(&chain);
		if (!clear)
			return false;
	}
	return true;
}

/* The records of the journals of the pairs are tagged by integer and string keys: for the keys of a date or a timestamp a read takes every change, and cuts them to its keys itself */
static bool
journal_tags_keys(Relation rel)
{
	return rel->rd_index == NULL || !sift_scope_stamp_key(rel);
}

bool
sift_f2s_pair_edits(Relation rel, int ord, const SiftScopeMeta *m, Snapshot snap, const int64 *keys, uint32 nkeys, SiftKeyMap *km, int64 **out, uint32 *nout)
{
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS] = {0};
	SiftConnKeys changed;
	uint32		n = 0;

	for (int j = 0; j < sift_scope_njournals(sift_scope_specs(rel)->s[ord]); j++)
		memcpy(&stamp[j], m->hdr.stamps[j].raw, sizeof(SiftConnStamp));
	*out = NULL;
	*nout = 0;
	if (sift_scope_vouch(rel, ord, stamp, snap) != SIFT_CONN_OK ||
		sift_scope_changed(rel, ord, stamp, snap, nkeys > 0 ? SIFT_SCOPE_OVERLAY_MAX_PAIR_EDITS : 1, nkeys > 0 && journal_tags_keys(rel) ? keys : NULL, nkeys, km, &changed) != SIFT_CONN_OK)
		return false;
	for (uint32 i = 0; i < changed.n; i++)
		if (nkeys == 0 || changed.kept == NULL || !changed.kept[i])
			changed.keys[n++] = changed.keys[i];
	if (n > 0 && (nkeys == 0 || sift_scope_pairs_multiset(rel, sift_scope_specs(rel)->s[ord])))
		return false;
	*out = n > 0 ? changed.keys : NULL;
	*nout = n;
	return true;
}

bool
sift_f2s_pairs_unchanged(Relation rel, int ord, const SiftScopeMeta *m, Snapshot snap)
{
	int64	   *none;
	uint32		n;

	return sift_f2s_pair_edits(rel, ord, m, snap, NULL, 0, NULL, &none, &n) && n == 0;
}

/* The schema of an index is never altered */
bool
sift_f2s_schema_matches(const SiftF2Store *store, const SiftScopeMeta *m)
{
	return !sift_f2s_is_table(store) || m->hdr.schema_hash == store->schema->hash;
}

static bool
scope_matches(Relation rel, const SiftF2Store *store, int ord, Snapshot snap, bool settled)
{
	SiftScopeMeta m;
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	bool		ok;

	if (!sift_f2s_object(rel, ord, &m, SIFT_SMETA_OPEN_LAZY_TOPK))
		return false;
	ok = sift_f2s_schema_matches(store, &m) && parts_match(rel, &mr, &m, snap, settled) && sift_f2s_pairs_unchanged(rel, ord, &m, snap) && (!settled || sift_f2s_deltas_clear(rel, store, ord, &mr));
	sift_scope_meta_close(&m);
	return ok;
}

/* Is the summary of scope ord what the entity parts and the pairs are now: every part it names is in the manifest, no part of a later generation is there, the journals have not moved */
bool
sift_f2_scope_current(Relation rel, int ord)
{
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());
	SiftF2Store *store = sift_f2_store_open(rel);
	bool		ok = sift_f2s_supported(rel, store, ord) && scope_matches(rel, store, ord, snap, false);

	UnregisterSnapshot(snap);
	return ok;
}

/*
 * Can this transaction not count the rows from the summary: it has a change of the relation that is not written (an index must hear of the heap's, track_changes, and has no row of the stage; a
 * table has no row or delete staged), or, of a table, it reads under SSI, whose predicate locks the cubes do not take.
 */
bool
sift_f2s_unreadable(Relation rel, const SiftF2Store *store, Snapshot snap)
{
	int			nstaged;

	if (sift_f2s_is_table(store))
		return IsolationIsSerializable() || sift_stage_has_pending(rel);
	if (!sift_col_rel(rel)->track || sift_stage_rows(rel) > 0)
		return true;
	(void) sift_stage_dead(rel, snap, &nstaged);
	return nstaged != 0;
}

/*
 * Do the cubes of the summary count what the snapshot sees. The heap of an index is not asked, so the index must have heard of every change the snapshot sees (track_changes) and this
 * transaction must have none that it has not written; a table's versions are its own, and every column delta of a partition leaves the columns of the cubes as they are. The parts are those of the
 * summary and are read as they are (parts_match).
 */
bool
sift_f2_scope_servable(Relation rel, int ord, Snapshot snap)
{
	SiftF2Store *store = sift_f2_store_open(rel);

	if (snap->snapshot_type != SNAPSHOT_MVCC || sift_f2s_unreadable(rel, store, snap))
		return false;
	return sift_f2s_supported(rel, store, ord) && scope_matches(rel, store, ord, snap, true);
}
