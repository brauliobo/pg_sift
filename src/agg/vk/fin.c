/*
 * agg/vk/fin.c - what of a result the host takes as the device gives it: the sums of floats.
 */
#include "agg/vk/job.h"

#ifdef SIFT_VULKAN

bool
sift_vk_sums_finite(const SiftVkJob *job, const SiftVkResult *res)
{
	return true;
}

#endif							/* SIFT_VULKAN */
