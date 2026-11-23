/*
 * agg/gpu_local.c - the device of a backend of its own, where the GPU worker cannot be had (agg/gpu_shm.h).
 *
 * The device stays open for the session; what it holds beyond the device is given back as the backend lets go of it: the buffers of the jobs when the transaction ends, the cache
 * of decoded chunks when the other caches of the backend (sift.backend_cache_kb) need the room.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/ipc.h"

#include "agg/gpu_shm.h"
#include "agg/gpu_v6.h"
#include "core/cachebudget.h"

#ifdef SIFT_VULKAN

/* MEM[MEM-VK-03]: per-backend local SiftVk (device kept for the session; job buffers freed at transaction end, cache of sift.gpu_cache_mb freed under sift.backend_cache_kb pressure) */
static struct LocalDevice
{
	SiftVk	   *vk;
	char		device[64];
	bool		software;
	uint64		cache_bytes;
	uint64		tick;
	bool		hooks;
}			local;

static void
local_exit(pg_attribute_unused() int code, pg_attribute_unused() Datum arg)
{
	sift_vk_close(local.vk);
	local.vk = NULL;
}

static void
local_xact(XactEvent event, pg_attribute_unused() void *arg)
{
	if (local.vk != NULL && (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT))
		sift_vk_release(local.vk, false);
}

static uint64
local_oldest(void)
{
	return local.vk != NULL && sift_vk_caps(local.vk)->cache_bytes > 0 ? local.tick : UINT64_MAX;
}

static bool
local_evict_one(void)
{
	return local.vk != NULL && sift_vk_release(local.vk, true);
}

static void
local_evict_all(void)
{
	(void) local_evict_one();
}

static void
local_hooks(void)
{
	static const SiftCacheKind kind = {"gpu local", local_oldest, local_evict_one, local_evict_all};

	if (local.hooks)
		return;
	sift_cache_register(&kind);
	RegisterXactCallback(local_xact, NULL);
	on_proc_exit(local_exit, 0);
	local.hooks = true;
}

SiftVk *
sift_gpu_local_open(const char **why)
{
	static char err[200];
	uint64		bytes = (uint64) sift_gpu_cache_mb << 20;
	bool		software = sift_gpu_mode == SIFT_GPU_ON;

	if (local.vk != NULL && (strcmp(local.device, sift_gpu_device) != 0 || (software && !local.software)))
	{
		sift_vk_close(local.vk);
		local.vk = NULL;
	}
	if (local.vk == NULL)
	{
		local.vk = sift_vk_open(sift_gpu_device, software, bytes, err, sizeof(err));
		if (local.vk == NULL)
		{
			*why = err;
			return NULL;
		}
		sift_vk_set_envelope(local.vk, SIFT_VK_VRAM_ENVELOPE, SIFT_VK_VRAM_FIXED);
		strlcpy(local.device, sift_gpu_device, sizeof(local.device));
		local.software = software;
		local.cache_bytes = bytes;
		local_hooks();
	}
	else if (bytes != local.cache_bytes && sift_vk_set_cache(local.vk, bytes))
		local.cache_bytes = bytes;
	local.tick = sift_cache_tick();
	return local.vk;
}

#endif							/* SIFT_VULKAN */
