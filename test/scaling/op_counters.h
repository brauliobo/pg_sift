/*
 * test/scaling/op_counters.h - deterministic operation counts of the code under test, the unit of the scaling gate (scripts/check-scaling-ops.sh).
 *
 * The sources under test are compiled with -fsanitize-coverage=trace-pc,trace-cmp: the compiler calls __sanitizer_cov_trace_pc at every basic block and
 * __sanitizer_cov_trace_cmp* at every integer comparison they execute, and op_counters.c counts them between OP_BEGIN and OP_END. The allocator, the sort and the
 * memory moves are counted by the shim (pg_shim_ops.c). A run does the same work on every machine, at every load and under AddressSanitizer: no clock is read.
 */
#ifndef SIFT_SCALING_OP_COUNTERS_H
#define SIFT_SCALING_OP_COUNTERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum OpKind
{
	OP_STEPS,					/* basic blocks executed */
	OP_CMPS,					/* integer comparisons executed */
	OP_SORT_CALLS,				/* comparator calls of qsort and qsort_arg */
	OP_ALLOCS,					/* palloc, repalloc calls */
	OP_ALLOC_BYTES,				/* bytes asked of palloc and repalloc */
	OP_MOVE_BYTES,				/* bytes of memcpy and memmove */
	OP_NKINDS
} OpKind;

typedef struct OpCounts
{
	uint64_t	v[OP_NKINDS];
} OpCounts;

extern OpCounts op_now;
extern bool op_on;

/* The instrumentation is inserted after the optimizer ran: a barrier keeps the stores of op_on around the code it counts */
#define OP_BARRIER() __asm__ __volatile__("" ::: "memory")
#define OP_BEGIN() do { OP_BARRIER(); op_on = true; OP_BARRIER(); } while (0)
#define OP_END() do { OP_BARRIER(); op_on = false; OP_BARRIER(); } while (0)
#define OP_ADD(kind, n) do { if (op_on) op_now.v[kind] += (uint64_t) (n); } while (0)

/* The ceiling of the ratio ops(big) / ops(small) of one counter. pending: the lane that makes the code meet the ceiling; until then the ratio must stay above it */
typedef struct OpGate
{
	OpKind		kind;
	double		max_ratio;
	const char *pending;
} OpGate;

typedef struct ScalingCase
{
	const char *name;
	const char *under_test;
	uint32_t	n_small;
	uint32_t	factor;
	void		(*run) (uint32_t n);
	const OpGate *gates;
	int			ngates;
} ScalingCase;

extern const char *op_kind_name(OpKind k);
extern OpCounts op_measure(void (*run) (uint32_t), uint32_t n);
extern double op_ratio(const OpCounts *small, const OpCounts *big, OpKind k);

#define GATES(...) .gates = (const OpGate[]) {__VA_ARGS__}, .ngates = (int) (sizeof((const OpGate[]) {__VA_ARGS__}) / sizeof(OpGate))

#endif
