/*
 * agg/pipe.h - what a batch of the fact table goes through between the scan and the groups when the statement joins it to small relations: for each dimension (agg/star/star.h) the rows
 * of the batch are looked up, the rows an inner join does not keep leave the mask, and the payload columns the keys and aggregates read are gathered into vectors of the batch. A dimension
 * that holds a key more than once makes a batch of (fact row, dimension row) pairs of its own, which goes on to the next dimension and then to the groups; so does the unnest of
 * v6_unnest_run.c with its pairs of (row, element).
 *
 * The vectors of a batch are numbered: the slots of the fact side (the columns of the scan, and the element of an unnest) first, then the payload columns of the dimensions in the order of the
 * spec. A key or an argument of the spec is one of them (key_slot, arg_slot; -1: none, a count(*)).
 */
#ifndef SIFT_AGG_V6_PIPE_H
#define SIFT_AGG_V6_PIPE_H

#include "postgres.h"

#include "agg/engine.h"
#include "agg/ext.h"
#include "agg/join/lookup.h"
#include "agg/star/star.h"

typedef struct SiftV6PipeBatch SiftV6PipeBatch;

typedef struct SiftV6KeyExpr SiftV6KeyExpr;

typedef struct SiftV6Pipe
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6BatchFn sink;			/* where the batches go instead of the group: the pipe of a compute thread (agg/par/star.h) */
	void	   *sink_arg;
	SiftV6Rows *rows;			/* a row Top-N: the pairs go to its heap, with the heap address the last fact slot holds (NULL: they go to the groups) */
	int			nfact;
	int			nslots;
	int			ndims;
	const SiftStarDim *dim[SIFT_STAR_MAX_DIMS];
	uint32		nparts[SIFT_STAR_MAX_DIMS];	/* a dimension too big for the memory of a join is read in this many parts of its keys, one after the other */
	uint32		part[SIFT_STAR_MAX_DIMS];	/* the part of the key the current pass joins */
	int			fslot[SIFT_STAR_MAX_DIMS][SIFT_STAR_MAX_KEYS];	/* the slots of the fact keys of each dimension */
	SiftV6KeyExpr *kexpr[SIFT_STAR_MAX_DIMS][SIFT_STAR_MAX_KEYS];	/* the keys that are expressions (NULL: the slot of fslot) */
	int			pay_slot[SIFT_STAR_MAX_DIMS];	/* the first payload slot of each dimension */
	SiftVecType *type;			/* [nslots] */
	SiftV6Resid *resid;			/* the conditions of the join no key says, run on the pairs that went through every dimension (NULL: none) */
	int			rslot[SIFT_V6_RESID_COLS];	/* the slot of each of their columns */
	int		   *key_slot;		/* [nkeys] */
	int		   *arg_slot;		/* [nvec] */
	SiftVec   **keyvec;
	SiftVec   **argvec;
	uint32	   *match;
	uint16	   *fact_idx;
	uint32	   *dim_row;
	int			semi_slot;		/* the slot of the number of the fact row of each pair: only the first pair of a row that goes through every dimension and condition goes on to the groups (-1: all do) */
	uint64	   *semi_seen;		/* the rows of the batch whose pair has gone on */
	SiftV6PipeBatch *lvl[SIFT_STAR_MAX_DIMS + 1];
	MemoryContext cx;
	uint64		rows_out;
} SiftV6Pipe;

/* The types of the fact slots (SVEC_I64, SVEC_F64 or SVEC_STRVIEW); the dimensions are in place with their parts chosen */
extern SiftV6Pipe *sift_v6_pipe_new(MemoryContext cx, const SiftV6Spec *spec, SiftV6Group *group, SiftV6Rows *rows, int nfact, const SiftVecType *ftype);
/* A pipe over the dimensions, parts and slots of p, with every level made, whose batches go to sink: running it calls neither palloc nor the server, so that a compute thread may run it (the dimensions are read, never written) */
extern SiftV6Pipe *sift_v6_pipe_clone(MemoryContext cx, const SiftV6Pipe *p, SiftV6BatchFn sink, void *sink_arg);
/* The column the vector v of a batch is: argvec[j] is the argument of aggregate j, argvec[naggs + j] its second, then the columns of the COUNT(DISTINCT ..) rows and those of the FILTER clauses; NULL for none */
extern const SiftV6Col *sift_v6_vec_col(const SiftV6Spec *spec, int v);
/* The slot of the payload column of a dimension */
extern int	sift_v6_pipe_dim_slot(const SiftV6Pipe *p, int dim, int pay);
/* An EXISTS over the elements: slot is the number of the fact row of each pair, and each batch of rows starts with sift_v6_pipe_semi_reset */
extern void sift_v6_pipe_semi(SiftV6Pipe *p, int slot);
extern void sift_v6_pipe_semi_reset(SiftV6Pipe *p);
/* One batch of the fact side: fact[i] is slot i; bs gives its rows. The vectors must live until the call returns */
extern void sift_v6_pipe_run(SiftV6Pipe *p, SiftVec *const *fact, SiftBatchSel *bs);
/* The fact side has ended: the rows of the dimensions of a RIGHT or FULL join that no row matched go through the dimensions after them, with a NULL fact side */
extern void sift_v6_pipe_finish(SiftV6Pipe *p);

#endif							/* SIFT_AGG_V6_PIPE_H */
