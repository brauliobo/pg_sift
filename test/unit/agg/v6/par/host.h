/*
 * agg/v6/par/host.h - the controls of par_host.c.
 */
#ifndef SIFT_PAR_HOST_H
#define SIFT_PAR_HOST_H

/* The callbacks the backend registered, in the order it runs them: before_shmem_exit (the pool drains a job), then on_proc_exit (the pool joins its threads) */
extern void par_host_before_shmem_exit(void);
extern void par_host_proc_exit(void);

/* The cancels ProcessInterrupts took: a statement that is cancelled is unwound to its PG_TRY once each */
extern volatile int par_host_cancels_taken;

#endif
