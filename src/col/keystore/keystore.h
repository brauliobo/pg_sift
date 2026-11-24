/*
 * col/keystore/keystore.h - the scope key store of a column: the relation <table>_<column>_sift_values that holds the keys of a text, varchar or uuid scope
 * key (docs/COL-SCOPE.md "Key types") and gives each the code, an int8, by which the scope machinery names it. It has the table access method sift_values
 * (no rows: the pages are the store layout of core/store.h) and depends on its table; the scope index that uses it depends on it.
 */
#ifndef SIFT_COL_KEYSTORE_H
#define SIFT_COL_KEYSTORE_H

#include "postgres.h"

#include "storage/lockdefs.h"
#include "utils/relcache.h"

#include "col/codec.h"
#include "col/v1/enc.h"

#define SIFT_KEYSTORE_AM_NAME		"sift_values"
#define SIFT_KEYSTORE_LABEL			"sift_values"
#define SIFT_KEYSTORE_META_FLAG		0x00040000	/* SiftMetaPageData.flags: this relation is a scope key store */
#define SIFT_KEYSTORE_MAX_CODES		(UINT64CONST(1) << 30)	/* keys of one store: a code is 32 bits (all ones: no code) and the table of a view holds two slots for each */

/* The store of the column of the table: found by its dependency on the table, else adopted by its name (a restored dump has no dependency), else made; the index depends on it */
extern Oid	sift_keystore_ensure(Relation heap, Relation index, AttrNumber attno);
/* The store of the column, InvalidOid when there is none */
extern Oid	sift_keystore_find(Relation heap, AttrNumber attno);
/* Whether a relation is a scope key store */
extern bool sift_keystore_is_store(Oid relid);
/* Drops the stores of the table that no index depends on any more */
extern void sift_keystore_drop_unused(Relation heap);
/* The owner of the stores of the table follows the table's */
extern void sift_keystore_follow_owner(Oid heapoid);
/* The stores of a table take the names its name and schema give them */
extern void sift_keystore_follow_names(Oid heapoid);

/* The hooks of src/core/ops.c: the library loads (the transaction callback that drops a store with its last index), an index is being dropped */
extern void sift_keystore_init(void);
extern void sift_keystore_note_drop(Oid relid);

/*
 * The keys of a store (col/keystore/keystore_run.c). A store is opened with a lock for the length of the call that uses it; the view of its keys
 * is kept by the backend and read up to the newest run when it is asked for.
 */
typedef struct SiftKeyStore SiftKeyStore;

extern SiftKeyStore *sift_keystore_open(Oid store, LOCKMODE lock);
/* ... the runs of an epoch: a 5.x store reached the epoch of its compactions, a store 6.0 writes has epoch 0 */
extern SiftKeyStore *sift_keystore_open_epoch(Oid store, LOCKMODE lock, uint64 epoch);
extern void sift_keystore_close(SiftKeyStore *ks);
/* map[i] = the code in the store of the key i of the batch; the keys it has not got are appended as one run; returns how many were */
extern uint32 sift_keystore_intern(SiftKeyStore *ks, const SiftCDict *batch, const SiftCcPolicy *pol, uint32 *map);

/* The view up to the newest run (sift_keystore_find_code looks in the view as it was last refreshed: a reader refreshes once), and the code of a string in it, -1 when the store has none */
extern void sift_keystore_refresh(SiftKeyStore *ks);
extern int64 sift_keystore_find_code(SiftKeyStore *ks, const char *s, uint32 len);

/* The codes the view has: those of the runs it read, the codes 0 .. n - 1 */
extern uint64 sift_keystore_ncodes(SiftKeyStore *ks);

/* The string of a code, after reading the runs again when the view has fewer codes; valid until the store is read again */
extern const char *sift_keystore_string(SiftKeyStore *ks, uint32 code, uint32 *len);

#ifdef SIFT_TESTING
extern void sift_keystore_forget_views(void);
#endif

#endif
