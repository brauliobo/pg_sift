/*
 * col/vstore/vstore.h - the shared value store of a column: one relation for each (table, column) that several sift_col indexes of the table share
 * (docs/COL-VSTORE.md). The relation <table>_<column>_sift_values has the table access method sift_values (no rows: the pages are the
 * store layout of core/store.h) and depends on its table; every index that uses it depends on it.
 */
#ifndef SIFT_COL_VSTORE_H
#define SIFT_COL_VSTORE_H

#include "postgres.h"

#include "nodes/bitmapset.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

#include "col/codec.h"
#include "col/enc.h"

#define SIFT_VSTORE_AM_NAME		"sift_values"
#define SIFT_VSTORE_LABEL		"sift_values"
#define SIFT_VSTORE_META_FLAG	0x00040000	/* SiftMetaPageData.flags: this relation is a shared value store */

/* The store of the column of the table: found by its dependency on the table, else adopted by its name (a restored dump has no dependency), else made; the index depends on it */
extern bool sift_vstore_exists(Relation heap, AttrNumber attno);
extern Oid	sift_vstore_ensure(Relation heap, Relation index, AttrNumber attno);
/* The store of the column, InvalidOid when there is none */
extern Oid	sift_vstore_find(Relation heap, AttrNumber attno);
extern Oid	sift_vstore_find_oid(Oid heapoid, AttrNumber attno);
/* The attribute of the table that a string leaf of the index holds as it is (not a path, a carried leaf, a group child or a lane), InvalidAttrNumber for the others */
extern AttrNumber sift_vstore_leaf_attno(Relation index, const SiftColDef *leaf);
/* The store that leaf c of the index reads its strings from (the store of its column when the index depends on it), InvalidOid: the leaf has dictionaries of its own */
extern Oid	sift_vstore_leaf_store(Relation index, const SiftColDef *leaf);
/* The same for every leaf of the schema, NULL when the index has no store */
extern Oid *sift_vstore_leaf_stores(Relation index, const SiftColSchema *schema);
/* The rule (col/vstore/vstore_rule.c): whether sharing the strings of the leaf pays, and the build step that makes the stores of the leaves it does */
typedef enum SiftVStoreState
{
	SIFT_VSTORE_NONE,			/* nothing to share: no other index holds the column, or it is too small a gain */
	SIFT_VSTORE_SHARED,			/* another index of the table has the same order: the strings are shared */
	SIFT_VSTORE_OTHER_ORDER		/* the indexes that hold the column have another order: this one keeps its own strings */
} SiftVStoreState;

typedef struct SiftVStoreVerdict
{
	SiftVStoreState state;
	char	   *other;			/* the index that has the column (the one of the same order, else the first of another) */
	char	   *other_order;	/* its order, as the build resolved it ("key_cut = a, b; key_order = c", "table order") */
	char	   *order;			/* this index's, when it differs */
	double		duplicate_bytes;	/* the strings the index stores again, estimated from the statistics of the column */
	double		saving_bytes;		/* what the order of the other index would spare it: those strings less the references that would take their place */
} SiftVStoreVerdict;

extern void sift_vstore_verdict(Relation heap, Relation index, const SiftColDef *leaf, SiftVStoreVerdict *v);
extern void sift_vstore_choice_texts(Relation heap, Relation index, const SiftColIndexMeta *defs, char **texts);
/* The order of an index as the rule compares it ("key_cut = a, b; key_order = c", "table order") */
extern char *sift_vstore_order(Relation index);
extern bool sift_vstore_pays(Relation heap, Relation index, const SiftColDef *leaf);
extern bool sift_vstore_leaf_left(Relation heap, Relation index, const SiftColDef *leaf);
extern bool *sift_vstore_left_leaves(Relation index, const SiftColSchema *schema);
/* An index that is rebuilt and no longer shares the column stops depending on its store (the store goes when no index is left) */
extern void sift_vstore_release(Relation heap, Relation index, AttrNumber attno);
extern void sift_vstore_auto(Relation heap, Relation index);
/* Whether a relation is a value store */
extern bool sift_vstore_is_store(Oid relid);
extern List *sift_vstore_members(Oid store);
/* Whether a segment of the index refers to the store, whatever the dependencies say */
extern bool sift_vstore_index_names(Relation index, Oid store);
extern Oid	sift_vstore_table(Oid store);
/* Drops the stores of the table that no index depends on any more (VACUUM of the table) */
extern void sift_vstore_drop_unused(Relation heap);
/* The owner of the stores of the table follows the table's */
extern void sift_vstore_follow_owner(Oid heapoid);

/* The hooks of src/core/ops.c: the library loads (the transaction callback that drops a store with its last index), an index is being dropped */
extern void sift_vstore_init(void);
extern void sift_vstore_note_drop(Oid relid);

/*
 * The values of a store (col/vstore/vstore_run.c). A store is opened with a lock for the length of the call that uses it; the view of its values
 * is kept by the backend and read up to the newest run when it is asked for.
 */
typedef struct SiftVStore SiftVStore;

extern SiftVStore *sift_vstore_open(Oid store, LOCKMODE lock);
extern void sift_vstore_close(SiftVStore *vs);
extern uint64 sift_vstore_ncodes(SiftVStore *vs);
extern uint64 sift_vstore_epoch(SiftVStore *vs);
extern void sift_vstore_bump_epoch(SiftVStore *vs);
extern bool sift_vstore_old_epochs(SiftVStore *vs);
extern uint32 sift_vstore_current_epoch(uint32 store);
extern void sift_vstore_vacuum(Relation index);
extern PGDLLIMPORT int sift_vstore_gc_kb;
extern PGDLLIMPORT bool sift_col_value_store;	/* sift.col_value_store: CREATE INDEX and REINDEX share the strings of a column that the rule selects */
extern int	sift_vstore_retire(SiftVStore *vs, const Bitmapset *in_use);
extern uint64 sift_vstore_dir_codes(Relation rel, uint64 *epoch);
extern void sift_vstore_usage(Oid store, uint64 *epoch, uint64 *total, uint64 *live, Bitmapset **epochs);
/* map[i] = the code in the store of the value i of the batch; the values it has not got are appended as one run; returns how many were */
extern uint32 sift_vstore_intern(SiftVStore *vs, const SiftCDict *batch, const SiftCcPolicy *pol, uint32 *map, uint64 *epoch, uint64 *ncodes);

/* The view up to the newest run (sift_vstore_find_code looks in the view as it was last refreshed: a reader refreshes once), and the code of a string in it, -1 when the store has none */
extern void sift_vstore_refresh(SiftVStore *vs);
extern int64 sift_vstore_find_code(SiftVStore *vs, const char *s, uint32 len);

/* Whether the store has at least ncodes codes of the epoch (low 32 bits), after reading its runs again when the view has fewer or is of another; the string of a code of it, valid until the store is read again */
extern const char *sift_vstore_string(SiftVStore *vs, uint32 code, uint32 *len);

/*
 * The dictionary of a leaf of a segment as a reference to the store (col/vstore/vstore_ref.c, form DICT_REF): the writer interns the strings of dict
 * into the store and appends the form to out; the reader makes the dictionary in the code order of the segment from the form (NULL if malformed)
 */
extern void sift_vstore_ref_serialize(Oid store, const SiftCDict *dict, const SiftCcPolicy *pol, StringInfo out);
extern SiftCDict *sift_vstore_ref_parse(MemoryContext mcxt, Oid store, const char *data, uint64 len);
/* The segment reader and writer use the store through the host struct of col/enc.h */
extern void sift_vstore_ref_install(void);
/* The strings of a store by code, read in place from its runs (SiftCRefHost.open and .str); the handle goes with mcxt */
extern void *sift_vstore_read_open(Relation index, uint32 store, uint32 epoch, MemoryContext mcxt);
extern const char *sift_vstore_read_str(void *handle, uint32 code, uint32 *len, char *buf);
extern void sift_vstore_read_strs(void *handle, const uint32 *codes, uint32 n, StringInfo all, uint32 *off);
extern uint64 sift_vstore_read_ncodes(void *handle);
extern void sift_vstore_read_match(void *handle, SiftColOp op, const SiftColValue *c, int nvals, uint64 *bits);

#ifdef SIFT_TESTING
extern void sift_vstore_forget_views(void);
#endif

#endif
