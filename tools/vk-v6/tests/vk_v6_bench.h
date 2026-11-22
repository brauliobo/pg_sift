/*
 * tests/vk_v6_bench.h - the data and the device jobs of vk_v6_bench, shared with its probes (vk_v6_bench_probe.c).
 */
#ifndef VK_V6_BENCH_H
#define VK_V6_BENCH_H

#include "vk_v6_common.h"

enum { B_KS, B_KM, B_KL, B_QTY, B_PRICE, B_AMOUNT, B_NCOLS };

typedef struct Bench
{
	uint32_t	ngran;
	uint64_t	rows;
	SiftVkChunk *chunk[B_NCOLS];
	SiftVkGranule *vg;
	uint64_t	enc_bytes[B_NCOLS];
} Bench;

typedef struct Job
{
	Prog		p;
	SiftVkJob	j;
	SiftVkGranule *vg;
} Job;

void		job_init(Job *j, const Bench *b, SiftVkMode m);
void		jagg(Job *j, uint8_t kind, uint8_t vreg);
void		job_seal(Job *j);
void		make_job(Job *j, const Bench *b, int shape);
void		bench_probes(SiftVk *vk, const Bench *b);

#endif							/* VK_V6_BENCH_H */
