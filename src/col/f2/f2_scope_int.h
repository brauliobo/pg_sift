/*
 * col/f2/f2_scope_int.h - what the files of f2_scope share: the shape of the cubes the leaves of a relation make, and the entity parts as the source of a build and the entity side of a read.
 * The relation is a sift_col index or a sift_table; the store of either (col/f2/f2_store.h) has the leaves, the schema and the key, and a table's the declared scopes.
 */
#ifndef SIFT_F2_SCOPE_INT_H
#define SIFT_F2_SCOPE_INT_H

#include "access/tableam.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_store.h"
#include "col/f2/part_vis.h"
#include "col/scope.h"

/* What the leaves of the relation make of one declared scope: the member, the time, and the cube of the other leaves (measures: the numbers; dimensions: the booleans) */
typedef struct SiftF2ScopeCfg
{
	int			ord;
	const SiftScopeSpec *spec;
	int			member_leaf;
	int			time_leaf;
	int64		time_unit;
	SiftScopeShape shape;
	int			nrings;			/* the rings of the summary: 0 without a time leaf, 1 the time leaf, up to SIFT_SCOPE_TOPK_RINGS_CAP with the order leaves below */
	int			ring_leaf[SIFT_SCOPE_TOPK_RINGS_CAP - 1];
	int64		ring_unit[SIFT_SCOPE_TOPK_RINGS_CAP - 1];
	int			meas_leaf[SIFT_SCOPE_MEAS_CAP];
	int			dim_leaf[SIFT_SCOPE_DIMS_CAP];
} SiftF2ScopeCfg;

/* The granules of the entity parts: where each part's granules sit in the global numbering of a summary, and what is read of them */
typedef struct SiftF2ScopeSlot
{
	uint64		part_gen;
	uint64		part_id;
	SiftPartDesc d;
	uint32		base;
	uint32		ng;
	SiftPart   *part;
	struct SiftF2ColMap *map;	/* a sift_table: the chunks of the part as the leaves of the store have them, NULL when the part has the schema of the store */
	bool		map_read;
	SiftGranuleDesc *gdir;
	SiftZone   *zones;
	SiftDv	   *dv;
	bool		dv_read;
	const SiftPendDelIndex *px;	/* the pending deletes of its partition that the snapshot sees, NULL: none */
	SiftTidIndexEntry *tix;
	bool		unseen;			/* a row of it may be one the snapshot does not see: its partition has pending deletes, or its inserters are not settled */
} SiftF2ScopeSlot;

typedef struct SiftF2ScopeParts
{
	Relation	rel;
	SiftF2Store *store;
	const SiftF2ScopeCfg *sc;
	SiftF2ScopeSlot	   *s;
	int			n;
	int			nadd;			/* the parts the summary does not name, after the n it does (f2_scope_over.c) */
	int			cap;
	MemoryContext gcx;
	MemoryContext scx;		/* the strides of the granule asked for last: the caller reads them while it decodes the rows of others, which reset gcx */
	MemoryContext dcx;		/* the dead rows of the granules asked for, kept for the keys that follow */
	SiftScopeRows **dead_rows;
	Snapshot	snap;
	Relation	heap;
	SiftScopePairMap *pm;
	uint64		pair_gen;		/* the generation of the object whose pairs are the current ones: the map is the backend's (f2_scope_pairs.c); 0: read it from the pair relation */
	SiftScopeCubeAcc all;
	SiftScopeMonthTable months;
	bool		cube_live;
	int			excl_leaf;		/* a query for the rows whose leaf is NULL: the leaf whose non-NULL rows leave the cubes; -1 for none */
	SiftF2ScopeSlot *xs;		/* the granule the rows that leave the cubes were last made for */
	uint32		xg;
	uint32		xn;
	uint16	   *xrows;
	SiftXidCache xc;
	struct SiftF2ScopeRowSet *added;	/* the rows of the parts and slabs the summary does not hold, as the snapshot sees them (f2_scope_over.c) */
	struct SiftF2ScopeRowSet *gone;		/* the rows of the parts it holds that the snapshot does not see, read when the first key asks for them */
	bool		gone_many;				/* they are more than a read is worth */
} SiftF2ScopeParts;

static inline bool
sift_f2s_is_table(const SiftF2Store *store)
{
	return store->host->host_kind == SIFT_F2_HOST_TABLE;
}

extern bool sift_f2s_plain_leaf(const SiftColDef *d);
extern int	sift_f2s_ring_kind(const SiftColDef *d);
typedef struct SiftF2LeafShape
{
	uint64		nrows;
	uint64		nnull;
	uint64		nzones;
	uint64		nrise;
} SiftF2LeafShape;

extern void sift_f2s_leaf_shape(SiftF2ScopeParts *p, int leaf, SiftF2LeafShape *sh);
/* The scopes the relation declares; supported: one path and an integer member, the ones a summary is made of (an index is refused the others when it is made, a table is not) */
extern int	sift_f2s_nscopes(const SiftF2Store *store);
extern bool sift_f2s_supported(Relation rel, const SiftF2Store *store, int ord);
/* Can the summary of a scope be built now: supported and, for a sift_table, its pairs have a plain unique btree of integer columns and a journal (nothing records them for a table, as CREATE INDEX does) */
extern bool sift_f2s_ready(Relation rel, const SiftF2Store *store, int ord);
/* Is the leaf one the summary of the scope may be made of: every leaf of an index, the ones cols(..) names of a table (the ones a column delta never patches, f2_col_delta_io.c) */
extern bool sift_f2s_kept(const SiftF2Store *store, const SiftScopeSpec *sp, int leaf);
extern void sift_f2s_cfg(Relation rel, const SiftF2Store *store, int ord, SiftF2ScopeCfg *sc);
extern void sift_f2s_cfg_rings_of(SiftF2ScopeCfg *cfg, const SiftF2Store *store, const SiftScopeMeta *m);
extern SiftScopePairMap *sift_f2s_load_pairs(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap);
extern SiftF2ScopeParts *sift_f2s_parts_new(Relation rel, SiftF2Store *store, Relation heap, const SiftF2ScopeCfg *sc, Snapshot snap, SiftScopePairMap *pm, int nparts);
extern SiftF2ScopeSlot *sift_f2s_parts_add(SiftF2ScopeParts *p, const SiftPartDesc *d, uint64 part_id, uint32 base, uint32 ng);
extern int	sift_f2s_parts_in_manifest(Relation rel, const SiftManifestRead *mr);
extern void sift_f2s_parts_of_manifest(SiftF2ScopeParts *p, const SiftManifestRead *mr);
/* Does every part read as the cubes need: each leaf of the cubes in a chunk of its own form (leaf_direct), and no value the column could not hold; of an index always */
extern bool sift_f2s_leaf_direct(SiftF2ScopeParts *p, int leaf);
extern bool sift_f2s_reads_leaf(const SiftF2Store *store, const SiftF2ScopeCfg *sc, int leaf);
extern bool sift_f2s_parts_exact(SiftF2ScopeParts *p);
extern SiftScopeGranSource sift_f2s_source(SiftF2ScopeParts *p);
extern SiftScopeEntity sift_f2s_entity(SiftF2ScopeParts *p);
extern SiftPart *sift_f2s_slot_part(SiftF2ScopeParts *p, SiftF2ScopeSlot *s);
extern const SiftGranuleDesc *sift_f2s_slot_gdir(SiftF2ScopeParts *p, SiftF2ScopeSlot *s);
extern SiftF2ScopeSlot *sift_f2s_slot_of(SiftF2ScopeParts *p, uint32 gidx, uint32 *g);
extern SiftF2ScopeSlot *sift_f2s_slot_of_gen(SiftF2ScopeParts *p, uint64 gen);
/* The chunk of the part that holds the leaf: the leaf itself in an index and in a part that has the schema of the table */
extern int sift_f2s_slot_phys(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, int leaf);
extern SiftZone sift_f2s_slot_zone(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, int leaf, uint32 g);
extern const SiftDvGran *sift_f2s_slot_dv(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g);
/* The cells of the rows of one granule (slot s, granule g of its part), decoded once for all of them: cells[(rows[k] - base) * nleaves + j] is leaf leaves[j] of rows[k]; strings are copied into out */
extern void sift_f2s_granule_cells(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const SiftScopeNewest *const *rows, int nrows, const SiftScopeNewest *base, const int *leaves, int nleaves,
									SiftF2ScopeCell *cells, MemoryContext out);
/* The rows of granule g, or only the ascending rows at[0..nat) when at is given; member_only: their members and nothing else */
extern void sift_f2s_decode_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 *at, uint32 nat, bool member_only, SiftScopeRows *r);
extern uint32 sift_f2s_granule_excl(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 **rows);

/* f2_scope_over.c: the rows a summary lacks or counts too many, for the snapshot */
extern uint32 sift_f2s_unseen_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, uint16 *rows, uint32 n);
extern bool sift_f2s_overlay_open(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftScopeMeta *m);
/* f2_scope_cur.c: the summary of scope ord as the directory names it, opened in *m (the caller closes it); false when the scope has none. Does it name the part */
extern bool sift_f2s_object(Relation rel, int ord, SiftScopeMeta *m);
extern bool sift_f2s_meta_names(const SiftScopeMeta *m, uint64 part_gen);
extern bool sift_f2s_pairs_unchanged(Relation rel, int ord, const SiftScopeMeta *m, Snapshot snap);
/* Can this transaction not count the rows from the summary (a change of the relation that is not written, or SSI) */
extern bool sift_f2s_unreadable(Relation rel, const SiftF2Store *store, Snapshot snap);
/* Does no column delta of a partition of the manifest patch a column the summary of the scope reads */
extern bool sift_f2s_deltas_clear(Relation rel, const SiftF2Store *store, int ord, const SiftManifestRead *mr);
extern SiftScopePairMap *sift_f2s_pairs(SiftF2ScopeParts *p);
extern SiftScopePairMap *sift_f2s_object_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, uint64 object_gen, MemoryContext reader);
extern bool sift_f2s_added_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out);
extern SiftScopeGone sift_f2s_gone_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out);
extern bool sift_f2s_slot_dirty(const SiftF2ScopeParts *p, const SiftF2ScopeSlot *s);
extern uint32 sift_f2s_added_months(void *arg, int64 scope_key, const int32 **months);

#endif							/* SIFT_F2_SCOPE_INT_H */
