/*
 * granule_stubs.c - the symbols of the storage layer that the granule writer and reader reach and a unit test does not run: the checksum of the header, the identity of an index (for the
 * text of a damage error), the part's dictionaries (the fixture builds its own), the transaction counter. Everything else is tools/pgshim.c.
 */
#include "postgres.h"

#include "access/transam.h"
#include "lib/stringinfo.h"
#include "port/pg_crc32c.h"
#include "utils/rel.h"

#include "col/granule/granule.h"
#include "granule_fixture.h"
#include "pgshim.h"

static FormData_pg_class fake_class;
static RelationData fake_rel = {.rd_rel = &fake_class};

Relation
granule_fixture_rel(void)
{
	return &fake_rel;
}

const char *
sift_index_ident(Relation index)
{
	return "granule_fixture_idx";
}

uint32
sift_crc32c(const void *data, Size len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, data, len);
	FIN_CRC32C(crc);
	return crc;
}

void
appendStringInfoSpaces(StringInfo s, int n)
{
	while (n-- > 0)
		appendStringInfoChar(s, ' ');
}

void *
palloc_aligned(Size size, Size alignto, int flags)
{
	return MemoryContextAllocAligned(CurrentMemoryContext, size, alignto, flags);
}

const SiftPartDicts *
sift_part_dicts(Relation rel, const SiftPart *part)
{
	pgshim_unreached("sift_part_dicts");
}

FullTransactionId
ReadNextFullTransactionId(void)
{
	pgshim_unreached("ReadNextFullTransactionId");
}
