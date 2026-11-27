/*
 * test_u64set - the set of 64 bit keys (core/u64set.h): from the stack of the caller it grows past the keys it was sized for, keeps every key it was given (0 and the keys that hash to one
 * slot included) and no other, and it never fills: a set sized for no key takes as many as a share of a large work_mem holds, one by one.
 */
#include "postgres.h"

#include "core/u64set.h"
#include "test/common/check.h"

static uint64
key_of(uint64 i)
{
	return i * UINT64CONST(0x100000000);
}

static void
check_grows_from(uint32 sized_for, uint64 n)
{
	SiftU64Set	s;

	sift_u64set_init(&s, sized_for);
	for (uint64 i = 0; i < n; i++)
		SIFT_CHECK(sift_u64set_add(&s, key_of(i)));
	for (uint64 i = 0; i < n; i++)
		SIFT_CHECK(!sift_u64set_add(&s, key_of(i)) && sift_u64set_has(&s, key_of(i)));
	for (uint64 i = n; i < 2 * n + 10; i++)
		SIFT_CHECK(!sift_u64set_has(&s, key_of(i)) && !sift_u64set_has(&s, key_of(i) + 1));
	SIFT_CHECK(s.n == n && 2 * (uint64) s.n <= (uint64) s.mask + 1);
	SIFT_CHECK((n <= SIFT_U64SET_STACK) == (s.key == s.stack_key));
	sift_u64set_free(&s);
}

static void
test_stack_set_grows(void)
{
	const uint64 sizes[] = {0, 1, SIFT_U64SET_STACK, SIFT_U64SET_STACK + 1, 1000, 100000};

	for (int i = 0; i < lengthof(sizes); i++)
	{
		check_grows_from(0, sizes[i]);
		check_grows_from(SIFT_U64SET_STACK, sizes[i]);
	}
}

static void
test_sized_set_takes_its_keys_in_one_table(void)
{
	SiftU64Set	s;
	uint32		mask;

	sift_u64set_init(&s, 5000);
	mask = s.mask;
	for (uint64 i = 0; i < 5000; i++)
		SIFT_CHECK(sift_u64set_add(&s, i));
	SIFT_CHECK(s.mask == mask);
	sift_u64set_free(&s);
}

int
main(void)
{
	test_stack_set_grows();
	test_sized_set_takes_its_keys_in_one_table();
	puts("test_u64set: ok");
	return 0;
}
