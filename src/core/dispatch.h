/*
 * core/dispatch.h - the one entry of the library at each hook point a plain statement passes, and its one callback of the ends of transactions (core/dispatch.c).
 */
#ifndef SIFT_CORE_DISPATCH_H
#define SIFT_CORE_DISPATCH_H

#include "access/xact.h"

/* _PG_init: begin before the first hook of a subsystem is installed, end after the last one that serves a statement of a relation of the library */
extern void sift_dispatch_begin(void);
extern void sift_dispatch_end(void);

/* A callback of the ends of the transactions (subtransactions) of a backend: called as RegisterXactCallback would call it, but not in a database without the library */
extern void sift_xact_callback(XactCallback fn, void *arg);
extern void sift_subxact_callback(SubXactCallback fn, void *arg);

#endif							/* SIFT_CORE_DISPATCH_H */
