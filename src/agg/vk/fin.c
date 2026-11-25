/*
 * agg/vk/fin.c - what of a result the host takes as the device gives it: the sums of floats.
 */
#include <math.h>
#include <string.h>

#include "agg/vk/job.h"

#ifdef SIFT_VULKAN

static bool
sums_finite(const SiftVkJob *job, const int64_t *acc, uint32_t groups, uint32_t first, uint32_t stride)
{
	for (uint32_t g = 0; g < groups; g++)
		for (uint32_t i = 0; i < job->naggs; i++)
		{
			double		sum;

			if (job->agg[i].kind != SIFT_VK_AK_SUM_F)
				continue;
			memcpy(&sum, acc + (uint64_t) g * stride + first + 3 * i, sizeof(sum));
			if (!isfinite(sum))
				return false;
		}
	return true;
}

bool
sift_vk_sums_finite(const SiftVkJob *job, const SiftVkResult *res)
{
	if (res->acc == NULL || res->ngroups == 0)
		return true;
	if (job->mode == SIFT_VK_UNGROUPED || job->mode == SIFT_VK_DENSE)
		return sums_finite(job, res->acc, res->ngroups, 1, 1 + 3 * job->naggs);
	if (job->mode == SIFT_VK_HASH && (job->finish == SIFT_VK_FIN_GROUPS || job->finish == SIFT_VK_FIN_TOPK))
		return sums_finite(job, res->acc, res->ngroups, job->nkeys + 2, res->rec_words);
	return true;
}

#endif							/* SIFT_VULKAN */
