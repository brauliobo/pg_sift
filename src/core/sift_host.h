/*
 * sift_host.h - the host interface of the portable core.
 *
 * The files listed in portable.list (the codec, dictionary, segment reader/writer, evaluator, scoring kernels, analyzer
 * and query language) are compiled by every host: the PostgreSQL extension, the SQLite extension and whatever comes next.
 * They include this header and nothing else of the host: scripts/check-portable.sh enforces it in CI.
 *
 * The contract is the vocabulary below, which every host provides with the semantics of PostgreSQL's:
 *
 *   memory       palloc/palloc0/repalloc/pfree/pstrdup, MemoryContext (AllocSet*, MemoryContextReset/Delete/SwitchTo),
 *                everything allocated in a context goes away with it
 *   errors       ereport(ERROR, ...)/elog never return; the host unwinds to its caller (longjmp, exception, ...)
 *   strings      StringInfo, List (pointers), HTAB (fixed-size keys), text/varlena (len + bytes), UTF-8 helpers (pg_mb*)
 *   regex        pg_regcomp/pg_regexec/pg_regfree over pg_wchar
 *   bits, atomics, spin locks, dsa_*: the primitives of sift_parallel.c, which a host without shared memory implements
 *                with threads and malloc
 *   temp files   BufFile (the sorted runs of sift_runs.c), SharedFileSet (only the PostgreSQL parallel build asks for one), binaryheap
 *   storage      the page/extent store of sift_store.h: Relation, Buffer, ReadBuffer/ReleaseBuffer, GenericXLog and the
 *                stream reader/writer. This is a link-time interface (sift_store.c in PostgreSQL, sqlite/sqstore.c in
 *                SQLite): the hot path (sift_sr_get) is a plain call, never a table of function pointers.
 *   options      sift_host_enum_option: a process setting with a fixed set of values (sift.simd)
 *
 * PostgreSQL: the real headers, so the macros are PostgreSQL's own and the core compiles to exactly the calls it made
 * before this header existed. SQLite: sqlite/host/host.h implements the vocabulary over malloc, setjmp and SQLite rows.
 * MariaDB: mariadb/host/host.h, the same C library vocabulary (it includes sqlite/host/host.h) over mapped segment files.
 * To add a host, provide a header with this vocabulary, define SIFT_HOST_<NAME> and add the branch below
 * walks through what the SQLite one does. New code in a portable file uses this vocabulary only (CONTRIBUTING.md).
 */
#ifndef SIFT_HOST_H
#define SIFT_HOST_H

#if defined(SIFT_HOST_SQLITE) || defined(SIFT_HOST_MARIADB)

#include "host.h"

#else							/* PostgreSQL */

#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "catalog/pg_collation_d.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/shortest_dec.h"
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "lib/binaryheap.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/tidbitmap.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "port/pg_bswap.h"
#include "regex/regex.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/s_lock.h"
#include "storage/sharedfileset.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* the PostgreSQL 17..19 differences (and the planner and executor headers only the PostgreSQL host needs) */
#include "sift_compat.h"

typedef struct config_enum_entry SiftEnumEntry;

#define sift_host_enum_option(name, short_desc, long_desc, var, boot, options, assign) \
	DefineCustomEnumVariable(name, short_desc, long_desc, var, boot, options, PGC_USERSET, 0, NULL, assign, NULL)

#endif

#endif							/* SIFT_HOST_H */
