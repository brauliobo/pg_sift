/*
 * sift_span.c - the failure hook of sift_span.h in the hosts: a failed bounds check on stored bytes is corrupt data
 * (ERRCODE_DATA_CORRUPTED), raised through the host's ereport like every other corruption error of the readers.
 */
#include "sift_host.h"

#include "sift_span.h"

void
sift_span_fail(const char *what, size_t need, size_t have)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("corrupt data: a read of %s needs %zu bytes, %zu are left", what, need, have)));
}
