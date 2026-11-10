/*
 * col/vstore/vstore.h - the shared value store of a column: one relation for each (table, column) that several sift_col indexes of the table share
 * (docs/COL-VSTORE.md). The relation <table>_<column>_sift_values has the table access method sift_values (no rows: the pages are the
 * store layout of core/store.h) and depends on its table; every index that uses it depends on it.
 */
#ifndef SIFT_COL_VSTORE_H
#define SIFT_COL_VSTORE_H

#include "postgres.h"

#include "storage/lockdefs.h"
#include "utils/relcache.h"

#include "col/codec.h"
#include "col/enc.h"

#define SIFT_VSTORE_AM_NAME		"sift_values"
#define SIFT_VSTORE_LABEL		"sift_values"
#define SIFT_VSTORE_META_FLAG	0x00040000	/* SiftMetaPageData.flags: this relation is a shared value store */

/* The store of the column of the table: found by its dependency on the table, else adopted by its name (a restored dump has no dependency), else made; the index depends on it */
extern Oid	sift_vstore_ensure(Relation heap, Relation index, AttrNumber attno);
/* The store of the column, InvalidOid when there is none */
extern Oid	sift_vstore_find(Relation heap, AttrNumber attno);
/* Whether a relation is a value store */
extern bool sift_vstore_is_store(Oid relid);
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
/* The values of the store in code order, as a dictionary whose code of a value is its code in the store (do not intern into it) */
extern SiftCDict *sift_vstore_view(SiftVStore *vs);
extern uint64 sift_vstore_epoch(SiftVStore *vs);
/* map[i] = the code in the store of the value i of the batch; the values it has not got are appended as one run; returns how many were */
extern uint32 sift_vstore_intern(SiftVStore *vs, const SiftCDict *batch, const SiftCcPolicy *pol, uint32 *map);

#ifdef SIFT_TESTING
extern void sift_vstore_forget_views(void);
#endif

#endif
