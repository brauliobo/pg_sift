/*
 * agg/vk/seat.c - the device a process holds for the settings it is asked with: the shared worker and a backend of its own keep one seat each. A device that was lost or did not answer
 * is closed and opened again by the next job, one opened for other settings is replaced, and an open that failed (no driver, no device) is not tried again before SIFT_VK_RETRY_MS, so
 * that a host with no driver loads no library and scans no directory for each query.
 */
#include <stdio.h>
#include <string.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

static bool
stale(const SiftVkSeat *s, const char *device, bool software)
{
	return !sift_vk_usable(s->vk) || strcmp(s->device, device) != 0 || (software && !s->software);
}

static bool
failed_recently(const SiftVkSeat *s, const char *device, bool software)
{
	return s->failed_ms > 0 && s->software == software && strcmp(s->device, device) == 0 && now_ms() - s->failed_ms < SIFT_VK_RETRY_MS;
}

SiftVk *
sift_vk_seat_follow(SiftVkSeat *s, const char *device, bool software, uint64_t cache_bytes)
{
	if (s->vk != NULL && stale(s, device, software))
		sift_vk_seat_close(s);
	if (s->vk == NULL)
	{
		if (failed_recently(s, device, software))
			return NULL;
		s->vk = sift_vk_open(device, software, cache_bytes, s->err, sizeof(s->err));
		snprintf(s->device, sizeof(s->device), "%s", device);
		s->software = software;
		s->failed_ms = s->vk == NULL ? now_ms() : 0;
		if (s->vk == NULL)
			return NULL;
		s->cache_bytes = cache_bytes;
		s->opens++;
	}
	else if (cache_bytes != s->cache_bytes && sift_vk_set_cache(s->vk, cache_bytes))
		s->cache_bytes = cache_bytes;
	return s->vk;
}

void
sift_vk_seat_close(SiftVkSeat *s)
{
	sift_vk_close(s->vk);
	s->vk = NULL;
}

#endif							/* SIFT_VULKAN */
