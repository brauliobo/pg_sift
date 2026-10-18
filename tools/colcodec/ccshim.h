/* The error capture of shim.c, shared by the standalone test and benchmark. */
#ifndef CC_CCSHIM_H
#define CC_CCSHIM_H

extern int	cc_sqlstate;
extern char cc_errmsg[256];

/* Runs fn(arg): true when it returned, false when it raised an error (cc_sqlstate and cc_errmsg tell which) */
extern bool cc_try(void (*fn) (void *), void *arg);

extern void cc_arena_reset(void);

#endif
