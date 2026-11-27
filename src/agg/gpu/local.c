/*
 * agg/gpu/local.c - the device of a backend of its own, where the GPU worker cannot be had (agg/gpu/shm.h).
 *
 * The device stays open for the session (agg/vk/seat.c opens it again when it is lost, and does not look for a driver of a host that has none at each query); what it holds beyond the
 * device is given back as the backend lets go of it: the buffers of the jobs when the transaction ends, the cache of decoded chunks when the other caches of the backend (sift.backend_cache_kb) need the room.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/ipc.h"

#include "agg/gpu/shm.h"
#include "agg/gpu/scan.h"
#include "core/cachebudget.h"
#include "core/dispatch.h"

#ifdef SIFT_VULKAN

/* MEM[MEM-VK-03]: per-backend local SiftVk (device kept for the session; job buffers freed at transaction end, cache of sift.gpu_cache_mb freed under sift.backend_cache_kb pressure) */
static struct LocalDevice
{
	SiftVkSeat	seat;
	uint64		tick;
	bool		hooks;
}			local;

static void
local_exit(pg_attribute_unused() int code, pg_attribute_unused() Datum arg)
{
	sift_vk_seat_close(&local.seat);
}

static void
local_xact(XactEvent event, pg_attribute_unused() void *arg)
{
	if (local.seat.vk != NULL && (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT))
		sift_vk_release(local.seat.vk, false);
}

static uint64
local_oldest(void)
{
	return local.seat.vk != NULL && sift_vk_caps(local.seat.vk)->cache_bytes > 0 ? local.tick : UINT64_MAX;
}

static bool
local_evict_one(void)
{
	return local.seat.vk != NULL && sift_vk_release(local.seat.vk, true);
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
	sift_xact_callback(local_xact, NULL);
	on_proc_exit(local_exit, 0);
	local.hooks = true;
}

SiftVk *
sift_gpu_local_open(const char **why)
{
	const uint32 opens = local.seat.opens;
	SiftVk	   *vk = sift_vk_seat_follow(&local.seat, sift_gpu_device, sift_gpu_mode == SIFT_GPU_ON, (uint64) sift_gpu_cache_mb << 20);

	if (vk == NULL)
	{
		*why = local.seat.err;
		return NULL;
	}
	if (local.seat.opens != opens)
	{
		sift_vk_set_envelope(vk, SIFT_VK_VRAM_ENVELOPE, SIFT_VK_VRAM_FIXED);
		local_hooks();
	}
	local.tick = sift_cache_tick();
	return vk;
}

#endif							/* SIFT_VULKAN */
