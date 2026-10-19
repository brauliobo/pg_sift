/*
 * sift_aggback.c - the backends of the aggregate scan (sift_aggback.h): the registry, and the CPU, which is the default and has no callbacks of
 * its own: its kernels are the ones of sift_agg.c.
 */
#include "postgres.h"

#include "sift_aggback.h"

const SiftAggBackend sift_agg_cpu_backend = {.name = "cpu"};

/* in the order they are offered a scan */
static const SiftAggBackend *const backends[] = {&sift_agg_gpu_backend};

void
sift_aggbe_init(void)
{
	for (int i = 0; i < lengthof(backends); i++)
		backends[i]->init();
}

const SiftAggBackend *
sift_aggbe_begin(const SiftAggScan *scan, void **state, bool *exclusive)
{
	for (int i = 0; i < lengthof(backends); i++)
	{
		*state = backends[i]->begin(scan, exclusive);
		if (*state != NULL)
			return backends[i];
	}
	*state = NULL;
	return &sift_agg_cpu_backend;
}
