/*
 * core/page_io.h - what every page of an index does the same way in every format: the standard page header with the special space of core/page.h, and the
 * report of a page, record or directory the format does not allow. Format-neutral: a file of a format that does not read the store of core/store.h includes this one.
 */
#ifndef SIFT_PAGE_IO_H
#define SIFT_PAGE_IO_H

#include "core/host.h"

#include "core/page.h"
#include "core/damage.h"

/* The index as REINDEX INDEX takes it: schema-qualified and quoted where PostgreSQL has a schema (core/errctx.c) */
extern const char *sift_index_ident(Relation index);

/* A page, record or directory of the index that the format does not allow (index: the Relation) */
#define SIFT_CORRUPT(index, ...) \
	SIFT_DAMAGE_INDEX(index, \
					  errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
					  errdetail(__VA_ARGS__), \
					  errhint("REINDEX INDEX %s rebuilds it from the table.", sift_index_ident(index)))


/* The same for one segment of the index (gen: its generation) */
#define SIFT_SEG_CORRUPT(index, gen, ...) \
	SIFT_DAMAGE_INDEX(index, \
					  errmsg("sift index \"%s\" segment %llu is corrupted", RelationGetRelationName(index), (unsigned long long) (gen)), \
					  errdetail(__VA_ARGS__), \
					  errhint("REINDEX INDEX %s rebuilds it from the table.", sift_index_ident(index)))

/* Zeroes the page and gives it the standard header and the special space of the type and generation */
extern void sift_page_init(Page page, SiftPageType type, uint64 gen);

#endif
