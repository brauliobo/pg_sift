/*
 * col/f2/f2_store.h - one Format 2.0 columnar relation descriptor for both hosts: a sift_col index (SIFT_F2_HOST_INDEX) and a sift_table (SIFT_F2_HOST_TABLE).
 *
 * Everything that differs between the two lives in the eight pointers of SiftF2HostOps; the engine (granules, parts, manifest, compaction, scans) takes a const SiftF2Store
 * and never branches on the host in a hot loop. A NULL pointer is skipped in one branch.
 */
#ifndef SIFT_F2_STORE_H
#define SIFT_F2_STORE_H

#include "postgres.h"

#include "access/transam.h"
#include "access/tupdesc.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

#include "col/f2/col_meta.h"
#include "col/f2/rowbuf.h"
#include "core/f2/manifest.h"
#include "core/f2/meta.h"

/* A varlena value longer than this is kept outside the granules by a host that has such a store (a table); its cell is a null cell with the tag below and the raw value as its payload */
#define SIFT_F2_WIDE_THRESHOLD		4096
#define SIFT_F2_TAG_WIDE_RAW		0xFD

typedef enum SiftF2HostKind
{
	SIFT_F2_HOST_INDEX = 0,
	SIFT_F2_HOST_TABLE = 1
} SiftF2HostKind;

struct SiftF2Store;
struct SiftF2ColMap;
struct SiftExtVec;
struct SiftColSchema;
struct SiftColShred;
struct SiftScopeDecl;
struct SiftConnDecl;
struct SiftCompressPolicy;
struct SiftSkipPolicy;
struct SiftSketchPolicy;

typedef struct SiftF2HostOps
{
	SiftF2HostKind host_kind;
	SiftAmKind	am_kind;
	uint64		base_compat_flags;

	/* The logical leaves [0, store->ncols) mapped to the chunks of an opened part; NULL on sift_col */
	struct SiftF2ColMap *(*build_col_map) (const struct SiftF2Store *store, Relation rel, const SiftPart *part, MemoryContext cx);

	/* The wide-value extents of the rows a compaction drops, appended to release; NULL on sift_col */
	void		(*reclaim_dead_blobs) (Relation rel, const struct SiftF2Store *store, const SiftGranuleView *v, const uint64 *dropped_mask, struct SiftExtVec *release);

	/* The settled dead addresses, before compaction drops their rows, so that the secondary indexes lose them first; NULL on sift_col */
	void		(*on_vacuum_dead_tids) (void *vac_ctx, const uint64 *tids, uint32 ntids);

	/* The heap visibility map check of a granule; NULL on sift_table, whose MVCC is native and complete */
	uint16		(*filter_granule_vis) (void *vis_ctx, const SiftTidIndexEntry *tidx, const uint64 *tids, uint16 nrows, uint64 *sel_mask);

	/* Whether a scan must test its rows against the heap again; NULL on sift_table */
	bool		(*needs_heap_vet) (const struct SiftF2Store *store, const SiftColMetaPageData *meta);

	/* A wide value read back from its extents; NULL on sift_col */
	Datum		(*read_wide_blob) (Relation rel, const char *ref_bytes, uint32 ref_len, Oid typid, int16 typlen, MemoryContext dst_cx);

	/* The wide values of the rows of a stage written to their own extents, just before the rows are encoded as a part or a slab batch (col/f2/l0_stage.c); NULL on sift_col */
	void		(*spill_wide_blobs) (Relation rel, SiftRowBuf *rb);

	/* The options of the relation: reloptions for an index, sift.table_option_values for a table */
	void		(*load_options) (Relation rel, struct SiftF2Store *store, MemoryContext cx);

	/* The columns of a part written under another tuple descriptor than the relation has now, for a compaction that rewrites its rows; NULL (the pointer or the result): its columns are those of the store */
	const struct SiftColRemap *(*build_col_remap) (const struct SiftF2Store *store, Relation rel, const SiftPart *part, MemoryContext cx);
} SiftF2HostOps;

typedef struct SiftF2StoreOpts
{
	char	   *key_order;		/* "" or "auto", "off", or "c1, c2 DESC, ..." */
	char	   *paths;
	char	   *compression;
	char	   *scope;
	char	   *connect;
	char	   *preagg;
	char	   *sketches;
	char	   *skip_index;
	int			dead_rewrite_pct;	/* 0: sift.dead_rewrite_pct */
	bool		auto_maint;
	bool		vacuum_truncate;
	bool		track_changes;
} SiftF2StoreOpts;

typedef struct SiftF2Store
{
	Oid			relid;
	RelFileLocator locator;
	const SiftF2HostOps *host;
	MemoryContext cx;
	MemoryContext row_cx;

	SiftF2StoreOpts opts;
	struct SiftColSchema *schema;
	struct SiftColShred *shred;
	uint32		schema_hash;

	int			ncols;
	int			natts;
	int		   *att_first;		/* [natts + 1]: first leaf of attribute a (1-based), -1 when dropped */
	int16	   *leaf_attno;		/* [ncols] */
	SiftCellKind *kinds;
	uint8	   *hints;
	uint8	   *scales;
	int16	   *parent;
	bool	   *key_leaf;
	const char **key_name;
	uint16	   *key_len;
	int16	   *key_attno;
	bool		has_keys;
	bool		has_groups;
	bool		has_jsonb;
	bool		headerless;		/* sift_table (SIFT_COMPAT_NATIVE_STRINGS): text, varchar, char(n), bytea and name keep their bytes bare and a uuid its 16 bytes, as a sift_col index does */

	uint16		key_cols[SIFT_KEY_ORDER_MAX];
	int			nkey;
	int64		time_unit[SIFT_MANIFEST_TIME_COLS];

	int			nscopes;
	struct SiftScopeDecl *scopes;
	int			nconns;
	struct SiftConnDecl *conns;

	struct SiftF2Packer *packers;	/* a table: how each attribute with a leaf becomes a cell, made with the first row packed (f2_store.c); NULL before */
	int			npackers;
	struct SiftCompressPolicy *comp_policy;
	struct SiftSkipPolicy *skip_policy;
	struct SiftSketchPolicy *sketch_policy;

	Oid		   *opfamily;		/* [natts + 1]: the default btree operator family of each attribute and its equality operator, InvalidOid for an attribute without a leaf or an order; NULL on an index store */
	Oid		   *eq_opno;

	SiftPartSpec spec;
	void	   *host_data;		/* sift_col: SiftColRel; sift_table: SiftTblF2Extra */
} SiftF2Store;

extern SiftF2Store *sift_f2_store_open(Relation rel);
extern SiftF2Store *sift_f2_store_from_oid(Oid relid);

/* The cells of a row and its key for either host (the one conversion of aminsert, multi_insert and a build) */
extern void sift_f2_store_pack_row(Relation rel, SiftF2Store *store, MemoryContext row_cx, const ItemPointerData *tid, const Datum *values, const bool *isnull,
								   bool spill_wide_blobs, SiftCell *cells, SiftCell **elem, uint8 *key);

/* The kind of cell the leaf of a table attribute is: floats are SIFT_CELL_F8, other by-value types SIFT_CELL_INT, the rest a string image */
extern SiftCellKind sift_f2_kind_of_attribute(Form_pg_attribute att);

/* A virtual generated column (PostgreSQL 18) is computed where it is read: a row never stores a value for it, and the slot of a scan has it null */
static inline bool
sift_f2_attr_is_virtual(Form_pg_attribute att)
{
#ifdef ATTRIBUTE_GENERATED_VIRTUAL
	return att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL;
#else
	(void) att;
	return false;
#endif
}

/* The attributes of a table that have a leaf: not a dropped column, not a virtual one. att_first[a] of the others is -1 and no cell, chunk or key is made for them */
static inline bool
sift_f2_attr_has_leaf(Form_pg_attribute att)
{
	return !att->attisdropped && !sift_f2_attr_is_virtual(att);
}

/*
 * The cell of the value of an attribute of a table store; with spill, a varlena over SIFT_F2_WIDE_THRESHOLD becomes a null cell tagged SIFT_F2_TAG_WIDE_RAW that keeps the raw value (the varlena
 * with its header) as its payload. A headerless store keeps the bytes of a text, varchar, char(n) or bytea without their header and a name without its padding; any other string is the datum image.
 */
extern void sift_f2_cell_of_datum(const SiftF2Store *store, Form_pg_attribute att, Datum d, MemoryContext row_cx, bool spill, SiftCell *c);

/* The datum of a string cell of a headerless store: the varlena (a short header up to 126 bytes) or the zero padded name, in cx */
extern Datum sift_f2_bare_datum(Form_pg_attribute att, const char *bytes, uint32 len, MemoryContext cx);

/*
 * The relation invalidations of the Format 2.0 caches (the stores, the part pins, the address indexes, the granules): the backend takes one syscache callback on RELOID, whose
 * slots are its own (relcache callback slots are shared with every library, and running out is a FATAL), and it runs the invalidators registered here. An invalidator gets the
 * hash value of the relation that changed or was dropped, 0 for all of them, and asks sift_f2_inval_hits whether a relation of its cache is meant. The RELOID hash value of a
 * relation is taken when its cache entry is made (sift_f2_rel_hash) and kept: a syscache callback runs outside a transaction and must not look anything up.
 */
typedef void (*SiftF2Invalidator) (uint32 hashvalue);

extern void sift_f2_invalidator_register(SiftF2Invalidator fn);
extern uint32 sift_f2_rel_hash(Relation rel);
extern bool sift_f2_inval_hits(uint32 relhash, uint32 hashvalue);

extern PGDLLIMPORT const SiftF2HostOps sift_col_host_ops;
extern PGDLLIMPORT const SiftF2HostOps sift_tbl_host_ops;

#endif							/* SIFT_F2_STORE_H */
