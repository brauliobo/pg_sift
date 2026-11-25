/*
 * test_gpu_local - the device of a backend of its own (agg/gpu/local.c) on a host the driver is faked on: a device that was lost or did not answer is closed and opened again by the
 * next query, and a host with no driver is asked once for a while, not once per query.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/ipc.h"

#include "agg/gpu/shm.h"
#include "agg/gpu/v6.h"
#include "core/cachebudget.h"
#include "test/common/check.h"

#define RETRY_MS 30000.0

struct SiftVk
{
	bool		usable,
				lost;
};

int			sift_gpu_mode = SIFT_GPU_ON;
int			sift_gpu_min_rows = 1;
char	   *sift_gpu_device = "";
int			sift_gpu_cache_mb = 64;

static int	opens,
			closes;
static bool	no_driver;
static double clock_ms = 1000;

double
now_ms(void)
{
	return clock_ms;
}

SiftVk *
sift_vk_open(const char *device, bool software, uint64_t cache_bytes, char *err, size_t errlen)
{
	SiftVk	   *vk;

	opens++;
	if (no_driver)
	{
		snprintf(err, errlen, "no Vulkan driver");
		return NULL;
	}
	vk = calloc(1, sizeof(*vk));
	vk->usable = true;
	return vk;
}

void
sift_vk_close(SiftVk *vk)
{
	if (vk == NULL)
		return;
	closes++;
	free(vk);
}

bool
sift_vk_usable(const SiftVk *vk)
{
	return vk != NULL && vk->usable && !vk->lost;
}

const SiftVkCaps *
sift_vk_caps(const SiftVk *vk)
{
	static SiftVkCaps caps;

	return &caps;
}

bool
sift_vk_release(SiftVk *vk, bool cache)
{
	return true;
}

bool
sift_vk_set_cache(SiftVk *vk, uint64_t bytes)
{
	return true;
}

void
sift_vk_set_envelope(SiftVk *vk, uint64_t envelope, uint64_t fixed)
{
}

void
RegisterXactCallback(XactCallback callback, void *arg)
{
}

void
on_proc_exit(pg_on_exit_callback function, Datum arg)
{
}

void
sift_cache_register(const SiftCacheKind *kind)
{
}

uint64
sift_cache_tick(void)
{
	return 1;
}

static SiftVk *
open_ok(void)
{
	const char *why = NULL;
	SiftVk	   *vk = sift_gpu_local_open(&why);

	SIFT_CHECK(vk != NULL);
	return vk;
}

static void
test_device_is_kept(void)
{
	SiftVk	   *vk = open_ok();

	SIFT_CHECK(opens == 1);
	SIFT_CHECK(open_ok() == vk);
	SIFT_CHECK(opens == 1 && closes == 0);
}

static void
test_lost_device_is_opened_again(void)
{
	SiftVk	   *vk = open_ok();

	vk->lost = true;
	vk = open_ok();
	SIFT_CHECK(opens == 2 && closes == 1);
	SIFT_CHECK(sift_vk_usable(vk));
}

static void
test_timed_out_device_is_opened_again(void)
{
	SiftVk	   *vk = open_ok();
	const int	before = opens;

	vk->usable = false;
	vk = open_ok();
	SIFT_CHECK(opens == before + 1);
	SIFT_CHECK(sift_vk_usable(vk));
}

static void
test_no_driver_is_asked_once(void)
{
	const char *why = NULL;
	const int	before = opens;

	open_ok()->lost = true;
	no_driver = true;
	for (int i = 0; i < 100; i++)
	{
		why = NULL;
		SIFT_CHECK(sift_gpu_local_open(&why) == NULL);
		SIFT_CHECK(why != NULL && strcmp(why, "no Vulkan driver") == 0);
	}
	SIFT_CHECK(opens == before + 1);
	clock_ms += RETRY_MS + 1;
	SIFT_CHECK(sift_gpu_local_open(&why) == NULL);
	SIFT_CHECK(opens == before + 2);
}

static void
test_no_driver_is_not_remembered_for_another_device(void)
{
	const char *why = NULL;
	const int	before = opens;

	sift_gpu_device = "other";
	SIFT_CHECK(sift_gpu_local_open(&why) == NULL);
	SIFT_CHECK(opens == before + 1);
	sift_gpu_device = "";
}

static void
test_driver_comes_back(void)
{
	no_driver = false;
	clock_ms += RETRY_MS + 1;
	SIFT_CHECK(sift_vk_usable(open_ok()));
}

int
main(void)
{
	test_device_is_kept();
	test_lost_device_is_opened_again();
	test_timed_out_device_is_opened_again();
	test_no_driver_is_asked_once();
	test_no_driver_is_not_remembered_for_another_device();
	test_driver_comes_back();
	puts("test_gpu_local: ok");
	return 0;
}
