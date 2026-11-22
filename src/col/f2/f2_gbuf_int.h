/*
 * col/f2/f2_gbuf_int.h - what f2_gbuf.c (the part pins and the decoded granules) and f2_locate.c (the lookups of rows by address) share: the one relcache callback that drops what an
 * invalidated relation left in either cache, and the lists of the entries of a cache by relation.
 */
#ifndef SIFT_F2_GBUF_INT_H
#define SIFT_F2_GBUF_INT_H

#include "postgres.h"

#include "lib/ilist.h"
#include "nodes/pg_list.h"
#include "utils/hsearch.h"

#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_store.h"

/* Registers the invalidator once (sift_f2_invalidator_register): it calls f2_gbuf_invalidate_pins and f2_locate_invalidate for the relations the hash value names (0: every one) */
extern void f2_gbuf_register_invalidation(void);

/* Drops the pins of the relation that nobody holds and marks the others stale; the address indexes of the relation, unless a visit reads one */
extern void f2_gbuf_invalidate_pins(Oid relid);
extern void f2_locate_invalidate(Oid relid);
extern void f2_locate_invalidate_hits(uint32 hashvalue);

/* The entries of a cache that belong to one relation, found without a walk of the cache: a list of the nodes of the entries for each relation */
typedef struct RelList
{
	Oid			relid;
	dlist_head	head;
} RelList;

static inline HTAB *
rel_lists_create(const char *name, MemoryContext cx)
{
	HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(RelList),.hcxt = cx};

	return hash_create(name, 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static inline void
rel_list_link(HTAB *lists, Oid relid, dlist_node *node)
{
	bool		found;
	RelList    *l = hash_search(lists, &relid, HASH_ENTER, &found);

	if (!found)
		dlist_init(&l->head);
	dlist_push_tail(&l->head, node);
}

static inline void
rel_list_unlink(HTAB *lists, Oid relid, dlist_node *node)
{
	RelList    *l = hash_search(lists, &relid, HASH_FIND, NULL);

	dlist_delete(node);
	if (dlist_is_empty(&l->head))
		hash_search(lists, &relid, HASH_REMOVE, NULL);
}

/* Moves the nodes of the relation into out (which the caller owns) and forgets the list: the entries are then freed or linked again one by one, with no list that changes under the walk */
static inline void
rel_list_detach(HTAB *lists, Oid relid, dlist_head *out)
{
	RelList    *l = hash_search(lists, &relid, HASH_FIND, NULL);

	dlist_init(out);
	if (l == NULL)
		return;
	while (!dlist_is_empty(&l->head))
		dlist_push_tail(out, dlist_pop_head_node(&l->head));
	hash_search(lists, &relid, HASH_REMOVE, NULL);
}

/* The relations of the lists that the hash value of an invalidation names, as a list of OIDs: a walk of the relations the cache holds, not of its entries */
static inline List *
rel_lists_hits(HTAB *lists, uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	RelList    *l;
	List	   *hits = NIL;

	hash_seq_init(&seq, lists);
	while ((l = hash_seq_search(&seq)) != NULL)
		if (sift_f2_inval_hits(l->relid, hashvalue))
			hits = lappend_oid(hits, l->relid);
	return hits;
}

#endif							/* SIFT_F2_GBUF_INT_H */
