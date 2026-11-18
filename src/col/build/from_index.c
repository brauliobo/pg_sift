/*
 * col/build/from_index.c - REINDEX of a sift_col index takes the rows from the index it replaces
 * The old index holds every heap tuple the heap would give the new one (and the dead ones no VACUUM has taken), in segments cut by the key the new index cuts by, and
 * in write buffers: the buffers become runs of the new index, and one merge by key (the rewrite of sift.col_maintain) writes the segments of the new index. The heap is
 * not scanned and no row goes to a temporary file; the memory of the merge does not depend on the rows.
 * Whether the old index can give the rows (from_old_index) is exact: any other layout, key, option or state is built from the heap.
 */
#include "postgres.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "core/membudget.h"
#include "col/build/build_int.h"
#include "col/conn.h"
#include "col/scope.h"
#include "col/vstore/vstore.h"
#include "col/wbuf.h"
#include "col/xstamp.h"

#ifdef SIFT_TESTING
bool		sift_test_col_from_index_off = false;
int			sift_test_col_from_index_mutant = 0;
int			sift_test_col_from_index_used = 0;
#endif

/* A version of the pg_class row of the index: where it is, where its update went, and the file it names */
typedef struct ClassVersion
{
	ItemPointerData self;
	ItemPointerData next;
	RelFileNumber file;
	Oid			space;
} ClassVersion;

static ClassVersion *
class_version_after(List *versions, ItemPointer self)
{
	foreach_ptr(ClassVersion, v, versions)
		if (ItemPointerEquals(&v->next, self) && !ItemPointerEquals(&v->self, self))
			return v;
	return NULL;
}

/*
 * The file REINDEX replaced: REINDEX gives the index a new file before its build and schedules the old one for deletion at commit, so it is still there and complete, and the
 * rows of the table cannot change (ShareLock). Its number is in the version of the pg_class row that the swap updated: the versions this transaction made since (the options the
 * build chooses are written there) name the new file, the one before them the old. false when the row has no version that names another file.
 */
static bool
replaced_locator(Relation index, RelFileLocator *loc)
{
	Relation	classrel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *versions = NIL;
	ClassVersion *cur = NULL,
			   *prev;

	ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(index)));
	scan = systable_beginscan(classrel, ClassOidIndexId, true, SnapshotAny, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);
		ClassVersion *v = palloc(sizeof(ClassVersion));

		*v = (ClassVersion) {.self = tup->t_self,.next = tup->t_data->t_ctid,.file = form->relfilenode,.space = form->reltablespace};
		versions = lappend(versions, v);
		if (ItemPointerEquals(&v->self, &v->next))
			cur = v;
	}
	systable_endscan(scan);
	table_close(classrel, AccessShareLock);
	SIFT_INVARIANT(cur != NULL && cur->file == index->rd_locator.relNumber, "sift: no current pg_class row names the file of index %u", RelationGetRelid(index));
	while ((prev = class_version_after(versions, &cur->self)) != NULL && prev->file == cur->file)
		cur = prev;
	if (prev == NULL)
		return false;
	*loc = (RelFileLocator) {.spcOid = prev->space != InvalidOid ? prev->space : MyDatabaseTableSpace,.dbOid = MyDatabaseId,.relNumber = prev->file};
	return true;
}

static bool
index_in_service(Relation index)
{
	return index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive;
}

/*
 * The index REINDEX replaces, with its old file. Never for REINDEX CONCURRENTLY, whose writers change the old index while it is read, nor for a rewrite of the table
 * (VACUUM FULL, CLUSTER, ALTER TABLE) that rebuilds its indexes: the addresses of the rows are other ones there.
 */
static Relation
replaced_index(Relation heap, Relation index, IndexInfo *info)
{
	RelFileLocator loc;

	if (info->ii_Concurrent || !ReindexIsProcessingIndex(RelationGetRelid(index)) || !index_in_service(index) ||
		heap->rd_createSubid != InvalidSubTransactionId || heap->rd_newRelfilelocatorSubid != InvalidSubTransactionId || !replaced_locator(index, &loc))
		return NULL;
	return sift_col_rel_file_view(index, &loc);
}

typedef struct FromOld
{
	SiftSegEntry in[SIFT_MAX_SEGMENTS];
	int			n;
	int			nbuffers;
	SiftSegEntry buffers[SIFT_MAX_SEGMENTS];
} FromOld;

/* The segments and write buffers of the old index; the reason it cannot give the rows (NULL: it can) */
static const char *
old_state(Relation old, const SiftColRel *r, FromOld *f)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col from-index check", ALLOCSET_SMALL_SIZES);
	SiftColSegment *seg[SIFT_MAX_SEGMENTS] = {NULL};
	bool		stream[SIFT_MAX_SEGMENTS];
	const char *why = NULL;

	sift_col_dir_read(old, snap, false);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		uint64		rows,
					pages;

		if (e->kind == SIFT_SEG_IMMUTABLE)
			f->in[f->n++] = *e;
		else if ((e->kind == SIFT_SEG_WBUF_OPEN || e->kind == SIFT_SEG_WBUF_SEALED) && (sift_cwbuf_summary(old, e, &rows, &pages), rows > 0))
			f->buffers[f->nbuffers++] = *e;
	}
	if (f->n + f->nbuffers == 0)
		why = "the index holds no rows";
	for (int i = 0; i < f->n && why == NULL; i++)
	{
		if (f->in[i].flags & SIFT_SEG_SCOPE)
			why = "the index has scope segments";
		else
		{
			seg[i] = sift_cseg_open(old, &f->in[i], cx);
			if (!sift_cschema_same_leaves(seg[i]->schema, r->schema, RelationGetDescr(old)))
				why = "the index has other column leaves";
			else if (!sift_cseg_kinds_exact(seg[i]))
				why = "a segment does not keep the kinds of its values";
		}
	}
	for (int i = 0; i < f->nbuffers && why == NULL && r->track; i++)
		if (sift_cwbuf_inserted_since(old, &f->buffers[i], r->schema, ReadNextTransactionId()) > 0)
			why = "a write buffer record does not say who inserted it";
	if (why == NULL && r->track && f->n > 0 && !TransactionIdIsNormal(sift_xstamp_inputs(f->in, f->n).next))
		why = "a segment does not say who inserted its rows";
	if (why == NULL && sift_colk_streams(stream, seg, f->n, r->cut, r->ncut, false, cx))
		why = "the segments are cut by another key";
	for (int i = 0; i < f->n && why == NULL && sift_col_key_cut(r); i++)
		if (!stream[i])
			why = "the segments are cut by another key";
	for (int i = 0; i < f->n; i++)
		if (seg[i] != NULL)
			sift_cseg_close(seg[i]);
	MemoryContextDelete(cx);
	pfree(snap);
	return why;
}

/* What the new index cannot take from another: scopes and connections (their segments and journals follow the heap), the shared value store */
static const char *
new_layout_refused(Relation index, const SiftColRel *r)
{
	Oid		   *stores;

	if (sift_col_format_minor != SIFT_COL_FORMAT_MINOR)
		return "another column format minor is asked for";
	if (r->nscope > 0)
		return "the index has a scope";
	if (r->nconn > 0)
		return "the index has connections";
	stores = sift_vstore_leaf_stores(index, r->schema);
	for (int i = 0; stores != NULL && i < r->schema->ncols; i++)
		if (OidIsValid(stores[i]))
			return "the index shares its strings in a value store";
	return NULL;
}

/*
 * The rows of a write buffer of the old index as a segment of the new one (the rows of a promotion, and as exact: who inserted them is stamped as a promotion stamps them);
 * false when it holds none. wt is the target of the runs, whose pages the build gives back once the merge has read them.
 */
static bool
buffer_run(Relation old, Relation index, SiftWriteTarget *wt, const SiftColRel *r, const SiftSegEntry *buf, SiftSegEntry *out, MemoryContext cx)
{
	SiftXWait	xwait = {InvalidTransactionId};
	int			nb,
				format = sift_col_format_minor;
	SiftColBatch **batches = sift_cwbuf_batches(old, buf, r->schema, cx, &nb, &xwait);
	SiftColSegWriter *w;
	SiftColFill *fill;
	uint64		rows = 0;
	uint32		aim;

	if (nb == 0)
		return false;
	for (int i = 0; i < nb; i++)
	{
		rows += batches[i]->nrows;
		if (!sift_cbatch_kinds_exact(batches[i]))
			format = Min(format, SIFT_COL_FORMAT_TAGS);
	}
	w = sift_colw_begin(wt, sift_store_new_gen(index), r->schema, r->policy, format, cx);
	fill = sift_conn_fill_begin(index, cx, rows);
	sift_colw_set_fill(w, fill);
	sift_colw_set_index_rows(w, rows);
	sift_colw_set_preagg(w, &r->pagg);
	if (sift_col_key_cut(r))
	{
		aim = sift_colk_run_rows(index, batches, nb);
		sift_colw_set_keyed(w, aim, r->cut, r->ncut);
		sift_colk_write(w, batches, nb, r->schema, r->cut, r->ncut, sift_colk_unit_rows(aim), cx, 0);
	}
	else
		for (int i = 0; i < nb; i++)
			sift_colw_add(w, batches[i]);
	sift_colw_finish(w, out);
	out->flags |= buf->flags & SIFT_SEG_ROOTS;
	sift_conn_fill_end(fill);
	if (r->track)
		sift_seg_set_xstamp(out, sift_xstamp_make(&xwait));
	return true;
}

static IndexBuildResult *
build_from(Relation heap, Relation index, Relation old)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col from-index", ALLOCSET_DEFAULT_SIZES);
	SIFT_MEM_SCOPE(col_from_index, cx);
	MemoryContext caller = MemoryContextSwitchTo(cx);
	const SiftColRel *r = sift_col_rel_private(index, cx);
	FromOld    *f = palloc0(sizeof(FromOld));
	const char *why;
	SiftSegEntry *in,
			   *out;
	SiftWriteTarget *wt = NULL;
	SiftExtent *release = NULL;
	int			nrelease = 0,
				nin,
				nout;
	double		rows = 0;
	IndexBuildResult *res = NULL;

	if ((why = new_layout_refused(index, r)) == NULL)
		why = old_state(old, r, f);
#ifdef SIFT_TESTING
	if (sift_test_col_from_index_mutant == 1)
		f->nbuffers = 0;
#endif
	if (why != NULL)
		elog(DEBUG1, "sift_col index \"%s\" is built from the heap: %s", RelationGetRelationName(index), why);
	else
	{
		out = palloc(SIFT_COL_BUILD_SEGMENTS * sizeof(SiftSegEntry));
		in = palloc((f->n + f->nbuffers) * sizeof(SiftSegEntry));
		memcpy(in, f->in, f->n * sizeof(SiftSegEntry));
		nin = f->n;
		sift_colb_init_meta(index);
		if (f->nbuffers > 0)
			wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
		for (int i = 0; i < f->nbuffers; i++)
		{
			MemoryContext run = AllocSetContextCreate(cx, "sift_col from-index buffer", ALLOCSET_DEFAULT_SIZES);

			if (buffer_run(old, index, wt, r, &f->buffers[i], &in[nin], run))
			{
				int			k;
				SiftExtent *ext = sift_store_immutable_extents(index, &in[nin], &k);

				release = release ? repalloc(release, (nrelease + k) * sizeof(SiftExtent)) : palloc(k * sizeof(SiftExtent));
				memcpy(release + nrelease, ext, k * sizeof(SiftExtent));
				nrelease += k;
				nin++;
			}
			MemoryContextDelete(run);
		}
		nout = sift_col_merge_from(index, old, in, nin, f->n, out, SIFT_COL_BUILD_SEGMENTS);
		if (wt != NULL)
		{
			sift_wt_release(wt, release, nrelease);
			sift_wt_finish(wt);
		}
		for (int i = 0; i < nin; i++)
			rows += in[i].ndocs;
		sift_colb_publish(index, out, nout, false);
		res = MemoryContextAllocZero(caller, sizeof(IndexBuildResult));
		res->index_tuples = rows;
		res->heap_tuples = heap->rd_rel->reltuples >= 0 ? heap->rd_rel->reltuples : rows;
	}
	SIFT_MEM_CHECKPOINT();
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(cx);
	return res;
}

/*
 * The rows of the old index, merged by the key of the new one into the segments of the new one: the build of REINDEX and REINDEX CONCURRENTLY when the old index can give them,
 * else NULL and the heap is read as for any build. The rows the old index lists that the heap no longer has (dead, not yet vacuumed) stay as the old index has them: a read
 * checks them against the heap as it does there.
 */
IndexBuildResult *
sift_colb_from_old(Relation heap, Relation index, IndexInfo *info)
{
	Relation	old;
	IndexBuildResult *res;

#ifdef SIFT_TESTING
	sift_test_col_from_index_used = 0;
	if (sift_test_col_from_index_off)
		return NULL;
#endif
	if ((old = replaced_index(heap, index, info)) == NULL)
		return NULL;
	res = build_from(heap, index, old);
	sift_col_rel_file_view_close(old);
#ifdef SIFT_TESTING
	sift_test_col_from_index_used = res != NULL;
#endif
	return res;
}
