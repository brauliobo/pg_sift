/*
 * core/span.c - the failure hook of core/span.h in the hosts: a failed bounds check on stored bytes is corrupt data
 * (ERRCODE_DATA_CORRUPTED), raised through the host's ereport like every other corruption error of the readers.
 */
#include "core/host.h"

#include "core/span.h"
#include "core/damage.h"

void
sift_span_fail(const char *what, size_t need, size_t have)
{
	SIFT_DAMAGE_DATA(NULL, errmsg("corrupt data: a read of %s needs %zu bytes, %zu are left", what, need, have));
}
