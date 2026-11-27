/*
 * agg/jsonb_order.c - the FILTER of an aggregate that is the presence of the key it reads (agg/jsonb_order.c): a FILTER of one operator of one operand (a prefix operator) is no such test, and
 * its one operand is all that is read of it.
 */
#include "postgres.h"

#include "agg/jsonb_order.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

/* a list of exactly n cells, so that ASan sees a read past its end */
static List *
list_of(int n, void **items)
{
	List	   *l = malloc(sizeof(List));

	*l = (List) {.type = T_List,.length = n,.max_length = n,.elements = malloc(sizeof(ListCell) * n)};
	for (int i = 0; i < n; i++)
		l->elements[i].ptr_value = items[i];
	return l;
}

static void
list_free_all(List *l)
{
	free(l->elements);
	free(l);
}

static void
test_prefix_operator_filter(void)
{
	Var			v = {.xpr.type = T_Var,.varattno = 1};
	OpExpr		op = {.xpr.type = T_OpExpr,.args = list_of(1, (void *[]) {&v})};
	List	   *quals = list_of(1, (void *[]) {&op});
	SiftV6Spec	s = {.filter = list_of(1, (void *[]) {quals})};

	SIFT_CHECK(!filter_is_presence(&s, 1, NULL));
	list_free_all(s.filter);
	list_free_all(quals);
	list_free_all(op.args);
}

int
main(void)
{
	if (unit_wants("jsonb_order_prefix_filter"))
		test_prefix_operator_filter();
	puts("jsonb_order: ok");
	return 0;
}
