# The tests of the Vulkan path that need no device: the device layer is linked with the entry points of the driver faked (test_vk_wait), or replaced (test_gpu_local), so that
# the loss of a device, the cancel of a query and a host without a driver are run on any machine. The real device is tools/vk-v6.
CFLAGS += -DSIFT_VULKAN

VK_LAYER = agg/vk/run.c agg/vk/mem.c agg/vk/job.c agg/vk/tab.c agg/vk/cache.c agg/vk/pipe.c agg/vk/fast.c agg/vk/dev.c agg/vk/test.c agg/vk/fin.c agg/vk/seat.c

TESTS += test_gpu_local test_vk_seat test_vk_wait test_vk_sums test_gpu_part
LIB_test_gpu_local = agg/gpu/local.c agg/vk/seat.c
LIB_test_vk_seat   = agg/vk/seat.c
LIB_test_vk_wait   = $(VK_LAYER)
LIB_test_vk_sums   = agg/vk/fin.c
LIB_test_gpu_part  = col/gpu.c
