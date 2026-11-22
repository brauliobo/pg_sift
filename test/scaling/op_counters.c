/*
 * test/scaling/op_counters.c - the counters the compiler instrumentation reports (op_counters.h). Compiled without the instrumentation.
 */
#include "op_counters.h"

OpCounts op_now;
bool op_on;

void
__sanitizer_cov_trace_pc(void)
{
	OP_ADD(OP_STEPS, 1);
}

void
__sanitizer_cov_trace_cmp1(uint8_t a, uint8_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_cmp2(uint16_t a, uint16_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_cmp4(uint32_t a, uint32_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_cmp8(uint64_t a, uint64_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_const_cmp1(uint8_t a, uint8_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_const_cmp2(uint16_t a, uint16_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_const_cmp4(uint32_t a, uint32_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_const_cmp8(uint64_t a, uint64_t b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_switch(uint64_t val, void *cases)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_cmpf(float a, float b)
{
	OP_ADD(OP_CMPS, 1);
}

void
__sanitizer_cov_trace_cmpd(double a, double b)
{
	OP_ADD(OP_CMPS, 1);
}

void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memmove(void *dst, const void *src, size_t n);

void *
__wrap_memcpy(void *dst, const void *src, size_t n)
{
	OP_ADD(OP_MOVE_BYTES, n);
	return __real_memcpy(dst, src, n);
}

void *
__wrap_memmove(void *dst, const void *src, size_t n)
{
	OP_ADD(OP_MOVE_BYTES, n);
	return __real_memmove(dst, src, n);
}

const char *
op_kind_name(OpKind k)
{
	static const char *const names[OP_NKINDS] = {"steps", "cmps", "sort_calls", "allocs", "alloc_bytes", "move_bytes"};

	return names[k];
}

OpCounts
op_measure(void (*run) (uint32_t), uint32_t n)
{
	op_now = (OpCounts) {{0}};
	run(n);
	return op_now;
}

double
op_ratio(const OpCounts *small, const OpCounts *big, OpKind k)
{
	if (small->v[k] == 0)
		return big->v[k] == 0 ? 1.0 : 1e9;
	return (double) big->v[k] / (double) small->v[k];
}
