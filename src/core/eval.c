/* core/eval.c - one validated list for developer experiments. */
#include "postgres.h"
#include "core/eval.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/varlena.h"

static char *methods;
static unsigned enabled;
static const char *const names[] = {"penalty_maintenance", "snapshot_masks", "unit_rewrite", "latest_hint"};

static bool
check_methods(char **value, void **extra, GucSource source)
{
	char *raw = pstrdup(*value);
	List *items = NIL;
	ListCell *lc;
	unsigned bits = 0;
	bool ok = SplitIdentifierString(raw, ',', &items);

	if (ok)
		foreach (lc, items)
		{
			const char *item = lfirst(lc);
			unsigned i;
			for (i = 0; i < lengthof(names); i++)
				if (strcmp(item, names[i]) == 0)
					break;
			if (i == lengthof(names))
			{
				GUC_check_errdetail("Unknown evaluation method: %s", item);
				ok = false;
				break;
			}
			bits |= 1U << i;
		}
	list_free(items);
	pfree(raw);
	if (ok)
	{
		unsigned *parsed = guc_malloc(ERROR, sizeof(*parsed));
		*parsed = bits;
		*extra = parsed;
	}
	return ok;
}

static void
assign_methods(const char *value, void *extra)
{
	enabled = *(unsigned *)extra;
}

bool
sift_eval_on(SiftEvalMethod method)
{
	return (enabled & (1U << method)) != 0;
}

void
sift_eval_init(void)
{
	DefineCustomStringVariable("sift.eval_methods", "Comma-separated developer evaluation methods that are on.", NULL, &methods, "", PGC_SUSET,
							   GUC_LIST_INPUT | GUC_NOT_IN_SAMPLE, check_methods, assign_methods, NULL);
}
