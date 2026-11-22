/*
 * core/quarantine_set.h - the quarantine of format major 2: a fixed table in shared memory of the relations that found their own damage, and no subtransaction around a write.
 *
 * Damage is found at the boundaries a page is checked at, before any change reaches a shared buffer or the log, so a write has nothing to roll back and needs no subtransaction (and
 * burns no transaction id) to catch it. The relation is put in the table, and the generation of the table moves: every backend sees it on its next check, which on the write
 * path is one atomic load of the generation and, when it did not move, a look at the backend's own copy.
 */
#ifndef SIFT_QUARANTINE_SET_H
#define SIFT_QUARANTINE_SET_H

#include "core/host.h"

#include "storage/relfilelocator.h"

#define SIFT_QSET_SLOTS			256

extern void sift_qset_add(const RelFileLocator *loc);
extern void sift_qset_remove(const RelFileLocator *loc);
extern bool sift_qset_has(const RelFileLocator *loc);

/* The generation of the table (it moves with every add and remove) and the relations in it */
extern uint64 sift_qset_generation(void);
extern int	sift_qset_count(void);

#endif							/* SIFT_QUARANTINE_SET_H */
