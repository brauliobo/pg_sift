/*
 * col/granule/filter_stubs.c - the symbols that the filters of a granule and the predicate code under them reach and test_filter does not run: the string memo of the leader (the workers keep theirs in the
 * arena), the dictionaries and chunks of the retired v1 reader that ctest.c serves for the units of a segment, and the collations of the catalog (the fixture's columns have none).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "test/common/pgshim.h"

#define UNREACHED(name) \
	void name(void) { pgshim_unreached(#name); }

MemoryContext
GetMemoryChunkContext(void *pointer)
{
	pgshim_unreached("GetMemoryChunkContext");
}

void
MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
	pgshim_unreached("MemoryContextRegisterResetCallback");
}

UNREACHED(cc_open)
UNREACHED(istr_split)
UNREACHED(sift_cchunk_decode)
UNREACHED(sift_cchunk_decode_sel)
UNREACHED(sift_cchunk_strs)
UNREACHED(sift_cdict_find)
UNREACHED(sift_cdict_match)
UNREACHED(sift_cstr_matches)
UNREACHED(sift_cstrs_contains)
UNREACHED(get_collation_isdeterministic)
UNREACHED(pg_newlocale_from_collation)
#if PG_VERSION_NUM < 180000
UNREACHED(lc_collate_is_c)
#endif
