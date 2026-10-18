/*
 * pgshim.h - the backend of the standalone harnesses (tools/colcodec, tools/analyze, tools/query, test/fuzz): the allocation, string
 * and error symbols of the sources under test, so that they link and run without a server, under AddressSanitizer and UBSan.
 *
 * Every palloc is its own malloc, so that AddressSanitizer sees any overrun, with the limits of the real allocator (more than 1 GB
 * needs the huge variants) and all chunks linked: pgshim_reset frees what a statement left behind (a backend frees it with its memory
 * context), and does it at exit, ahead of LeakSanitizer, so that what it reports is a real malloc leak.
 */
#ifndef PGSHIM_H
#define PGSHIM_H

#include <stdbool.h>

/* The SQLSTATE (0: an elog(ERROR) has none), message and source line of the last error */
extern int	pgshim_sqlstate;
extern char pgshim_errmsg[512];
extern char pgshim_errloc[128];
/* What GetDatabaseEncoding() answers (PG_UTF8) */
extern int	pgshim_encoding;

/* Runs fn(arg) the way a backend runs a statement: an ereport(ERROR) unwinds to here and the call returns false. Outside it, an error aborts */
extern bool pgshim_try(void (*fn) (void *), void *arg);
/* Everything palloc'd so far (the set-up of a target) stays; pgshim_reset frees what comes after */
extern void pgshim_keep(void);
extern void pgshim_reset(void);
/* The harness links no storage layer: aborts with the symbol that was reached */
extern void pgshim_unreached(const char *what) __attribute__((noreturn));

#endif
