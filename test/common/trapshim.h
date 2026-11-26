/*
 * trapshim.h - the harness of a pool thread that may not touch PostgreSQL. The tests link pgshim.c with the calls of trapped.def wrapped:
 * from a thread that set sift_test_in_worker, palloc, the memory contexts, StringInfo and ereport/elog print the call and abort, so "zero
 * palloc, zero MemoryContext, zero ereport in workers" is a failure of the run. The setup thread keeps the behavior of pgshim (it is the only
 * one that may call it, and not while a worker runs: pgshim keeps no lock).
 */
#ifndef SIFT_TRAPSHIM_H
#define SIFT_TRAPSHIM_H

#include "postgres.h"

/* Set by the thread function of a worker, first thing */
extern _Thread_local bool sift_test_in_worker;

/* true: a trapped call is counted and goes through, for the test of the trap itself. Set before the workers start */
extern bool sift_test_trap_soft;

/* -1: every pthread_create runs; n >= 0: n more succeed, then each fails with EAGAIN (a process at its limit of threads) */
extern int	sift_test_threads_left;

/* The trapped calls so far */
extern uint64 sift_test_trap_count(void);

#endif
