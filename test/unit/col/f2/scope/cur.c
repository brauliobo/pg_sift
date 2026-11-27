/*
 * test/unit/col/f2/scope/cur.c - the columns a scope summary reads, as the bloom of the attributes a column delta page names (col/f2/scope/cur.c read_bloom): a sift_col index names a leaf
 * by the leaf plus one (col/f2/col_delta/idx.h), not by its attribute, so a jsonb column of several leaves before the member leaves the bloom on the leaf the delta patches (S120-18). A
 * sift_table names its columns by their attribute.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#include "col/f2/scope/cur.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

volatile sig_atomic_t InterruptPending = false;

static int	read_leaf;

bool
sift_f2s_reads_leaf(const SiftF2Store *store, const SiftF2ScopeCfg *sc, int leaf)
{
	return leaf == read_leaf;
}

bool
sift_f2s_kept(const SiftF2Store *store, const SiftScopeSpec *sp, int leaf)
{
	return false;
}

/* doc (two leaves), dim_a_id, v: the member dim_a_id is leaf 2 of attribute 2 */
static uint64
member_bloom(SiftF2HostKind kind)
{
	SiftF2HostOps host = {.host_kind = kind};
	int16		attno[4] = {1, 1, 2, 3};
	SiftF2Store store = {.host = &host,.ncols = 4,.leaf_attno = attno};
	SiftF2ScopeCfg sc = {.member_leaf = 2,.time_leaf = -1};

	read_leaf = 2;
	return read_bloom(&store, &sc);
}

int
main(void)
{
	SIFT_CHECK(member_bloom(SIFT_F2_HOST_INDEX) == sift_cd_att_bit(3));
	SIFT_CHECK(member_bloom(SIFT_F2_HOST_TABLE) == sift_cd_att_bit(2));
	puts("test_scope_cur: ok");
	return 0;
}
