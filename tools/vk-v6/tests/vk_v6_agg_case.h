/*
 * tests/vk_v6_agg_case.h - what the aggregation tests share (vk_v6_test_agg.c T2..T9, vk_v6_test_emit.c T10): the columns of the logical data, a job with its program, and the check of a job
 * on the device against the oracle.
 */
#ifndef VK_V6_AGG_CASE_H
#define VK_V6_AGG_CASE_H

#include "vk_v6_common.h"

enum
{
	C_K0, C_K1, C_BIG, C_V32, C_DBL, C_FLAG, C_TS, C_DATE, C_NUM, C_DIV, C_KB, C_KL, C_NCOLS
};

typedef struct Case
{
	Prog		p;
	SiftVkJob	j;
} Case;

int64_t dbits(double d);
void case_init(Case *c, const TData *d, SiftVkMode mode);
void add_agg(Case *c, uint8_t kind, uint8_t vreg, uint8_t freg);
void build_base(TData *d, uint32_t ngran, Rng *rng, uint32_t last_rows);
/* the job (its operations and sets are taken from c->p) on the device, on the fast kernel when it has the shape and on the machine, against the oracle; true when the fast kernel ran it */
bool case_check(const char *name, Case *c, const TData *d, double ftol);
/* T10 (vk_v6_test_emit.c) */
void test_emit(SiftVk *dev, Rng *rng);
/* T12 (vk_v6_test_finish.c) */
void test_finish(SiftVk *dev, Rng *rng);

#endif
