/*
 * core/visibility_popcnt.c - the run of words of the visibility map counted with the CPU's popcount. Its own object: the baseline check of the build (scripts/isa_check.sh)
 * allows the instruction only in the objects named for an ISA, and core/visibility.c is baseline x86-64.
 */
#include "postgres.h"

#include "core/visibility_run.h"

#if defined(__x86_64__) || defined(SIFT_HAVE_NEON)
/* POPCNT is a target of this function alone on x86-64; CNT of Advanced SIMD is in every AArch64 build */
#if defined(__x86_64__)
__attribute__((target("popcnt")))
#endif
void
sift_vm_run_hw(VmPass *p, const uint64 *w, uint64 n)
{
	vm_run_impl(p, w, n, true);
}

#endif
