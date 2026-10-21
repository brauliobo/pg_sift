/*
 * table/store_int.h - shared between the files of the layout of a sift_table (store.c and the files that hold the rest of it).
 */
#ifndef SIFT_STORE_INT_H
#define SIFT_STORE_INT_H

#include "table/table.h"

#define corrupt(rel, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_DATA_CORRUPTED), \
			 errmsg("sift_table relation \"%s\" is corrupted", RelationGetRelationName(rel)), \
			 errdetail(__VA_ARGS__)))

static inline bool
is_tomb(const SiftSegEntry *e)
{
	return e->kind != SIFT_SEG_IMMUTABLE && (e->flags & SIFT_TBL_SEG_TOMB);
}

static inline bool
is_wbuf(const SiftSegEntry *e)
{
	return e->kind != SIFT_SEG_IMMUTABLE && (e->flags & SIFT_TBL_SEG_WBUF);
}

static inline const SiftSegEntry *
find_seg(const SiftDirSnapshot *dir, bool (*pred) (const SiftSegEntry *), SiftSegKind kind)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (pred(&dir->segs[i]) && (kind == 0 || dir->segs[i].kind == kind))
			return &dir->segs[i];
	return NULL;
}

#endif							/* SIFT_STORE_INT_H */
