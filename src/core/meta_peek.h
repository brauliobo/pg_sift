/*
 * core/meta_peek.h - the format of a relation, read from the 16 bytes of block 0 that every format starts with: the one place that tells a Format 1 relation from a
 * Format 2 one, before any file of either format opens it. The peek raises no error and records no damage: a block that is not a metapage is a status of its own.
 */
#ifndef SIFT_META_PEEK_H
#define SIFT_META_PEEK_H

#include "core/host.h"

#include "core/sift.h"

typedef enum SiftFormatStatus
{
	SIFT_FMT_EMPTY,				/* no block yet: a relation that is being built or was truncated */
	SIFT_FMT_V1,				/* a metapage of format major 1 */
	SIFT_FMT_V2,				/* a metapage of format major 2 */
	SIFT_FMT_OTHER,				/* a metapage of another format major: a newer release wrote it */
	SIFT_FMT_BAD				/* block 0 is not a metapage of the sift formats */
} SiftFormatStatus;

typedef struct SiftMetaPeek
{
	SiftFormatStatus status;
	uint16		format_major;
	uint16		format_minor;
	uint32		flags;			/* SIFT_META_QUARANTINED when the metapage says the index is out of service, whichever format keeps the flag; 0 for a block 0 that is not a metapage */
} SiftMetaPeek;

extern SiftMetaPeek sift_meta_peek_format(Relation rel);

/* The format major of an existing relation: that of its metapage, that of a new relation while it has none; ERROR for a newer one, damage for a bad block 0 */
extern uint16 sift_meta_resolve_major(Relation rel);

/* The journal of the pairs of a scope is a key log of format major 1: the index a scope makes for one is of that format whatever a new index is */
extern bool sift_new_format_journal;

/* The format major a CREATE INDEX or a first write gives a relation */
extern uint16 sift_new_relation_format_major(void);

/*
 * The relations made until the matching pop get the major pushed, whatever the build would give them (sift.upgrade_table makes Format 2.0 when sift.test_create_format1 is on). The
 * pushes nest, up to SIFT_NEW_FORMAT_DEPTH; a push is followed by its pop in a PG_FINALLY.
 */
#define SIFT_NEW_FORMAT_DEPTH	4
extern void sift_tbl_push_new_format(uint16 major);
extern void sift_tbl_pop_new_format(void);

#ifdef SIFT_TESTING
/* sift.test_create_format1 (testing build only): the relations made while it is on take Format 1 */
extern PGDLLIMPORT bool sift_test_create_format1;
#endif

#endif
