/*
 * col/f2/scope/int.h - what the files of f2_scope share: the shape of the cubes the leaves of a relation make, and the entity parts as the source of a build and the entity side of a read.
 * The relation is a sift_col index or a sift_table; the store of either (col/f2/store/store.h) has the leaves, the schema and the key, and a table's the declared scopes.
 */
#ifndef SIFT_F2_SCOPE_INT_H
#define SIFT_F2_SCOPE_INT_H

#include "access/tableam.h"

#include "col/f2/del/delvec.h"
#include "col/f2/scope/scope.h"
#include "col/f2/store/store.h"
#include "col/f2/part/vis.h"
#include "col/scope/scope.h"

/* What the leaves of the relation make of one declared scope: the member, the time, and the cube of the other leaves (measures: the numbers; dimensions: the booleans) */
typedef struct SiftF2ScopeCfg
{
	int			ord;
	const SiftScopeSpec *spec;
	struct SiftF2Mem *mem;		/* the codes of a text, varchar or uuid member (f2_scope_mem.h), NULL for an integer member */
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
	bool		px_lazy;		/* its partition has pending deletes that px does not hold yet: the first read of the rows of a granule builds it (sift_f2s_slot_px) */
	struct SiftColDeltaPartMap *cd;	/* the address changes the snapshot sees in the column delta chain of its partition, NULL: none */
	bool		cd_read;
	bool		exc_read;
	uint32		nexc;
	const struct SiftVariantExcSummary *exc;	/* the leaves the part keeps exceptions of (SEC_VARIANT_EXC), read once */
} SiftF2ScopeSlot;

/* part_gen -> a small integer (the slot of the part, its index in a manifest, ...): open-addressed, load at most one half; a gen of 0 is no key */
typedef struct SiftGenMap
{
	uint64	   *gen;
	uint32	   *val;
	uint32		mask;
} SiftGenMap;

/* The rows of a granule (of the slot of an entity part) that leave the cubes, kept for the 16 granules asked for last */
typedef struct SiftF2ScopeExcl
{
	int			slot;			/* -1: none */
	uint32		g;
	uint32		n;
	uint16	   *rows;
} SiftF2ScopeExcl;

#define SIFT_F2S_EXCL_SLOTS 16

#define SIFT_SCOPE_OVERLAY_KEPT_PCT 60

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
	MemoryContext dcx2;		/* the member and time of the granule the stride scans decoded last: the strides of a granule are scanned one after the other */
	SiftScopeRows dec_rows;
	int			dec_slot;
	uint32		dec_g;
	SiftGenMap	gen_map;		/* part_gen -> slot of the first gen_map_n slots */
	int			gen_map_n;
	Snapshot	snap;
	Relation	heap;
	SiftKeyMap *km;				/* the map of the codes of the scope key the statement reads by, NULL for an integer key */
	SiftScopePairMap *pm;
	SiftScopePairMap *pm_object;	/* with edited pairs: the map of the object that pm patches, NULL: no read settled it (pm is then the pairs now) */
	const SiftScopeMeta *meta;		/* the summary of a live read: its stamps say which pairs the object was built from */
	const int64 *fkeys;			/* the keys the read asks for (ascending, distinct; none: every key): a row of the overlay whose member owns none of them is not kept, unless the rows are kept for other statements */
	uint32		nfkeys;
	const int64 *changed;		/* the members whose pairs changed since the summary for the keys of the read, ascending (the edits pm holds) */
	uint32		nchanged;
	uint64		pair_gen;		/* the generation of the object whose pairs are the current ones: the map is the backend's (f2_scope_pairs.c); 0: read it from the pair relation */
	SiftScopeCubeAcc all;
	SiftScopeMonthTable months;
	bool		cube_live;
	int			excl_leaf;		/* a query for the rows whose leaf is NULL: the leaf whose non-NULL rows leave the cubes; -1 for none */
	SiftF2ScopeExcl xcache[SIFT_F2S_EXCL_SLOTS];
	uint16	   *xscratch;		/* the dead and the non-NULL rows of a granule while the rows that leave are made: 3 * SIFT_GRANULE_ROWS */
	SiftXidCache xc;
	struct SiftF2ScopeRowSet *added;	/* the rows of the parts and slabs the summary does not hold, as the snapshot sees them (f2_scope_over.c) */
	struct SiftF2ScopeRowSet *gone;		/* the rows of the parts it holds that the snapshot does not see, read when the first key asks for them */
	bool		gone_many;				/* they are more than a read is worth */
	SiftManifestRead mr;				/* the partitions the slots belong to, as the statement read them */
	const SiftPendDelIndex **px_part;	/* the pending deletes of each partition of mr that the snapshot sees, built when first asked for (sift_f2s_slot_px) */
	bool	   *px_built;
	bool		settled_ok;				/* the rows may be kept: the snapshot is settled for them and the read keeps no leaf out */
	struct SiftF2ScopeSnap *settled;	/* the entry of the backend's cache that holds the rows above for the statements that follow while the parts are as they are (f2_scope_snap.h), NULL: they are the statement's own */
} SiftF2ScopeParts;

#define SIFT_F2S_SRC_PART 0x80000000U

struct SiftF2ScopeRowSet
{
	uint32		n;
	uint32		cap;
	int64	   *member;
	int64	   *time;
	uint8	   *time_null;
	int64	   *dim[SIFT_SCOPE_DIMS_CAP];
	int64	   *meas[SIFT_SCOPE_MEAS_CAP];
	uint8	   *meas_null[SIFT_SCOPE_MEAS_CAP];
	SiftScopeRows rows;
	uint32		nruns;
	uint32	   *run_first;		/* the rows are ordered by member: run j is rows run_first[j] .. run_first[j + 1] - 1, those of one member */
	uint32		nkeys;			/* the keys of the pairs of the object, ascending (key_val), each with the runs of the members that hold it: key_run[key_off[k] .. key_off[k + 1] - 1] */
	int64	   *key_val;
	uint32	   *key_off;
	uint32	   *key_run;
	SiftScopeShape shape;		/* the rows are kept for other statements: nothing here points into the one that read them */
	const SiftF2ScopeCfg *sc;
	uint32	   *src;			/* where the cells of the row are: SIFT_F2S_SRC_PART | slot of the entity part (then g and row: the granule and the row of it), else the index in bufs (then row) */
	uint32	   *g;
	uint32	   *row;
	struct SiftRowBuf **bufs;	/* the slabs the rows were read from, kept for the cells of the rows (the context of the rows) */
	uint32		nbufs;
	int			cur_slot;		/* while the rows of a granule are added: its slot and its index in the part */
	uint32		cur_g;
	SiftF2ScopeParts *only_keys;	/* a statement's own rows: only those whose member owns one of the keys it asks for (p->fkeys) are kept */
};

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
extern bool sift_f2s_int_type(Oid type);
/* km: the map of the codes of the scope key (NULL: an integer key), the statement's own for a read; intern: km is a writer's (the build, the refresh), which gives the keys the store lacks a code */
/* The pairs for a build: the keys of the scope, a member's keys asked of the btree (f2_scope_probe.c), or the load of all the pairs where that cannot be (a scope of two paths, a text member) */
extern SiftScopePairMap *sift_f2s_build_pairs(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km);
extern SiftScopePairMap *sift_f2s_load_pairs(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, bool intern);
/* The map of the codes of the key of the scope: NULL for an integer key */
extern SiftKeyMap *sift_f2s_key_map(Relation heap, Relation index, const SiftScopeSpec *spec, bool write);
extern SiftF2ScopeParts *sift_f2s_parts_new(Relation rel, SiftF2Store *store, Relation heap, const SiftF2ScopeCfg *sc, Snapshot snap, SiftScopePairMap *pm, int nparts);
extern SiftF2ScopeSlot *sift_f2s_parts_add(SiftF2ScopeParts *p, const SiftPartDesc *d, uint64 part_id, uint32 base, uint32 ng);
/* The parts of an entry as the commit of edit (NULL: none) leaves them: the generations it drops out, the parts it replaces and adds in; the entity parts of a manifest read are those of its entries */
extern SiftPartDesc *sift_f2s_entry_parts(Relation rel, const SiftPartitionEntry *e, const SiftManifestEdit *edit, int *np);
extern int	sift_f2s_parts_in_manifest(Relation rel, const SiftManifestRead *mr, const SiftManifestEdit *edit);
extern void sift_f2s_parts_of_manifest(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftManifestEdit *edit);
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
extern void sift_f2s_gen_map_init(SiftGenMap *gm, uint32 expected);
extern void sift_f2s_gen_map_put(SiftGenMap *gm, uint64 gen, uint32 val);
extern bool sift_f2s_gen_map_find(const SiftGenMap *gm, uint64 gen, uint32 *val);
/* The part's exceptions of leaves (SEC_VARIANT_EXC), read once; NULL: none */
extern const struct SiftVariantExcSummary *sift_f2s_slot_exc(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 *n);
/* What a decode of a granule reads: the leaves of the member, the time, the dimensions, the measures (the rings the row asks for in want_extra) */
enum { SIFT_F2S_NEED_MEMBER = 1, SIFT_F2S_NEED_TIME = 2, SIFT_F2S_NEED_DIMS = 4, SIFT_F2S_NEED_MEAS = 8, SIFT_F2S_NEED_ALL = 15 };
extern void sift_f2s_decode_cols(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 *at, uint32 nat, uint32 need, SiftScopeRows *r);
extern bool sift_f2s_has_row(const uint16 *rows, uint32 n, uint32 row);
extern void sift_f2s_members_cubes(void *arg, const int64 *members, uint32 n, SiftScopeSpan span, SiftScopeCubeAcc *out, bool *has);
extern bool sift_f2s_member_cube(void *arg, int64 member, SiftScopeSpan span, SiftScopeCubeAcc *out);
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
extern bool sift_f2s_overlay_open(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftScopeMeta *m, const SiftScopeMeta *cached);
/* The pending deletes of the partition of the slot that the snapshot sees (built on the first ask), NULL: none */
extern const SiftPendDelIndex *sift_f2s_slot_px(SiftF2ScopeParts *p, SiftF2ScopeSlot *s);
/* What the column delta chain says of granule g of the slot for the snapshot, NULL: none (f2_scope_slot.c) */
extern const struct SiftColDeltaGranMap *sift_f2s_slot_gran_map(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g);
/* The member belongs to the scope key under the pairs of the read */
extern bool sift_f2s_owns(SiftF2ScopeParts *p, int64 member, int64 key);
extern uint32 *sift_f2s_key_runs(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a, int64 key, uint32 *n);
extern bool sift_f2s_owns_any(SiftF2ScopeParts *p, int64 member, const int64 *keys, uint32 nkeys);
/* f2_scope_cur.c: the summary of scope ord as the directory names it, opened in *m (the caller closes it); false when the scope has none. Does it name the part */
extern bool sift_f2s_object(Relation rel, int ord, SiftScopeMeta *m, uint32 open_flags);
/* gm: the part generations the summary names -> their index in its part map */
extern void sift_f2s_meta_map(const SiftScopeMeta *m, SiftGenMap *gm);
extern bool sift_f2s_pairs_unchanged(Relation rel, int ord, const SiftScopeMeta *m, Snapshot snap);

/* A summary of a table is of the leaves the table had when it was built: with another schema (a column added, dropped, renamed or changed in type since) it is another set of cubes and is not read */
extern bool sift_f2s_schema_matches(const SiftF2Store *store, const SiftScopeMeta *m);
/* The members whose pairs of the scope keys changed since the summary, ascending, palloc'd in *out (NULL: none); false when the journals cannot name them, or they are more than a read overlays. nkeys 0: every key, and any change is false */
extern bool sift_f2s_pair_edits(Relation rel, int ord, const SiftScopeMeta *m, Snapshot snap, const int64 *keys, uint32 nkeys, SiftKeyMap *km, int64 **out, uint32 *nout);
/* Can this transaction not count the rows from the summary (a change of the relation that is not written, or SSI) */
extern bool sift_f2s_unreadable(Relation rel, const SiftF2Store *store, Snapshot snap);
/* Does no column delta of a partition of the manifest patch a column the summary of the scope reads */
extern bool sift_f2s_deltas_clear(Relation rel, const SiftF2Store *store, int ord, const SiftManifestRead *mr);
extern SiftScopePairMap *sift_f2s_pairs(SiftF2ScopeParts *p);
/* The pairs of the object with those of the changed members as the snapshot sees them, for the scope keys the read asks for (f2_scope_pairs.c) */
extern SiftScopePairMap *sift_f2s_edited_pairs(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged);
/* The map of the object of generation object_gen that the backend kept, pinned for reader; NULL when it holds none */
extern SiftScopePairMap *sift_f2s_cached_pairs(Relation index, uint64 object_gen, MemoryContext reader);
/* The pairs as the snapshot sees them; kept for the object of generation object_gen when settled (no pair of any key changed since the summary: they are the object's), else in reader */
extern SiftScopePairMap *sift_f2s_object_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, uint64 object_gen, MemoryContext reader, bool settled);
extern bool sift_f2s_added_cube(void *arg, int64 scope_key, SiftScopeSpan span, SiftScopeCubeAcc *out);
extern SiftScopeGone sift_f2s_gone_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out);
extern bool sift_f2s_slot_dirty(const SiftF2ScopeParts *p, const SiftF2ScopeSlot *s);
extern uint32 sift_f2s_added_months(void *arg, int64 scope_key, const int32 **months);

#endif							/* SIFT_F2_SCOPE_INT_H */
