/*
 * test_vk_seat - the device a process holds for the settings it is asked with (agg/vk/seat.c), as the shared worker follows the settings of each job: a change of the cache resizes the device, a
 * change of the device or a software one asked for replaces it, a lost one is replaced, and a failed open is remembered for the settings it failed for and no others.
 */
#include "postgres.h"

#include "agg/vk/vk.h"
#include "unit.h"

struct SiftVk
{
	bool		usable;
};

static int	opens,
			closes,
			resizes;
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
		snprintf(err, errlen, "no device matches \"%s\"", device);
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
	return vk != NULL && vk->usable;
}

bool
sift_vk_set_cache(SiftVk *vk, uint64_t bytes)
{
	resizes++;
	return true;
}

static void
test_cache_resize(void)
{
	SiftVkSeat	seat = {0};
	SiftVk	   *vk = sift_vk_seat_follow(&seat, "", false, 64);

	CHECK(vk != NULL && seat.opens == 1 && resizes == 0);
	CHECK(sift_vk_seat_follow(&seat, "", false, 64) == vk && resizes == 0);
	CHECK(sift_vk_seat_follow(&seat, "", false, 128) == vk && resizes == 1 && seat.cache_bytes == 128);
	sift_vk_seat_close(&seat);
	CHECK(seat.vk == NULL && closes == 1);
}

static void
test_software_and_device_replace_the_device(void)
{
	SiftVkSeat	seat = {0};

	CHECK(sift_vk_seat_follow(&seat, "", false, 64) != NULL);
	CHECK(sift_vk_seat_follow(&seat, "", false, 64) != NULL && seat.opens == 1);
	CHECK(sift_vk_seat_follow(&seat, "", true, 64) != NULL && seat.opens == 2);
	CHECK(sift_vk_seat_follow(&seat, "", false, 64) != NULL && seat.opens == 2);
	CHECK(sift_vk_seat_follow(&seat, "1", false, 64) != NULL && seat.opens == 3);
	sift_vk_seat_close(&seat);
}

static void
test_failed_open_is_remembered_for_its_settings(void)
{
	SiftVkSeat	seat = {0};
	const int	before = opens;

	no_driver = true;
	for (int i = 0; i < 5; i++)
		CHECK(sift_vk_seat_follow(&seat, "1", false, 64) == NULL && strcmp(seat.err, "no device matches \"1\"") == 0);
	CHECK(opens == before + 1);
	CHECK(sift_vk_seat_follow(&seat, "1", true, 64) == NULL && opens == before + 2);
	CHECK(sift_vk_seat_follow(&seat, "2", true, 64) == NULL && opens == before + 3);
	no_driver = false;
	CHECK(sift_vk_seat_follow(&seat, "2", true, 64) == NULL && opens == before + 3);
	clock_ms += SIFT_VK_RETRY_MS;
	CHECK(sift_vk_seat_follow(&seat, "2", true, 64) != NULL && opens == before + 4);
	CHECK(seat.failed_ms == 0);
	sift_vk_seat_close(&seat);
}

static void
test_lost_device_is_replaced_and_the_seat_survives_a_close(void)
{
	SiftVkSeat	seat = {0};
	SiftVk	   *vk = sift_vk_seat_follow(&seat, "", false, 64);
	const int	before = opens;

	vk->usable = false;
	CHECK(sift_vk_seat_follow(&seat, "", false, 64) != NULL && opens == before + 1 && seat.opens == 2);
	sift_vk_seat_close(&seat);
	sift_vk_seat_close(&seat);
	CHECK(sift_vk_seat_follow(&seat, "", false, 64) != NULL && opens == before + 2);
	sift_vk_seat_close(&seat);
}

int
main(void)
{
	test_cache_resize();
	test_software_and_device_replace_the_device();
	test_failed_open_is_remembered_for_its_settings();
	test_lost_device_is_replaced_and_the_seat_survives_a_close();
	puts("test_vk_seat: ok");
	return 0;
}
