/*
 * core/visibility_run.h - the count of a run of words of the visibility map (core/visibility.c: the pure C count; core/visibility_popcnt.c: the CPU's instruction).
 */
#ifndef SIFT_VISIBILITY_RUN_H
#define SIFT_VISIBILITY_RUN_H

#include "core/host.h"
#include "core/popcount.h"

/* The all-visible bit of each page (not its all-frozen bit) in a map word */
#define VM_VISIBLE_BITS	UINT64CONST(0x5555555555555555)

/* the counts of a pass over the map, and where in a superblock (8 words) it is */
typedef struct VmPass
{
	uint64		acc;			/* the words of the superblock so far, ANDed: all-visible bits kept only where every page has one */
	uint32		k;
	BlockNumber visible,
				clean;
} VmPass;

/* the bits of a word of the map: the instruction where the CPU has it, else the pure C count of core/popcount.h (the object is baseline x86-64: its builtin calls libgcc) */
#define VM_POPCOUNT(hw, m) ((hw) ? (uint32) __builtin_popcountll(m) : sift_pc64(m))

/* the words of a run that fill superblocks from the first word of one are counted 8 at a time, the head and the tail of a run word by word */
static inline __attribute__((always_inline)) void
vm_run_impl(VmPass *p, const uint64 *w, uint64 n, bool hw)
{
	while (n > 0 && p->k > 0)
	{
		uint64		m = *w++ & VM_VISIBLE_BITS;

		p->visible += VM_POPCOUNT(hw, m);
		p->acc &= m;
		n--;
		if (++p->k == 8)
		{
			p->clean += p->acc == VM_VISIBLE_BITS;
			p->acc = VM_VISIBLE_BITS;
			p->k = 0;
		}
	}
	for (; n >= 8; n -= 8, w += 8)
	{
		uint64		acc = VM_VISIBLE_BITS;
		uint32		vis = 0;

#pragma GCC unroll 8
		for (int j = 0; j < 8; j++)
		{
			uint64		m = w[j] & VM_VISIBLE_BITS;

			vis += VM_POPCOUNT(hw, m);
			acc &= m;
		}
		p->visible += vis;
		p->clean += acc == VM_VISIBLE_BITS;
	}
	for (; n > 0; n--)
	{
		uint64		m = *w++ & VM_VISIBLE_BITS;

		p->visible += VM_POPCOUNT(hw, m);
		p->acc &= m;
		p->k++;
	}
}

/* the same count with the CPU's popcount (core/visibility_popcnt.c: the object that holds the instruction, which the baseline check skips) */
extern void sift_vm_run_hw(VmPass *p, const uint64 *w, uint64 n);

#endif							/* SIFT_VISIBILITY_RUN_H */
