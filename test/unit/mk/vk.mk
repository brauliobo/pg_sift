# The tests of the Vulkan path that need no device: the device layer is linked with the entry points of the driver faked (agg/vk/wait), or replaced (agg/vk/local), so that
# the loss of a device, the cancel of a query and a host without a driver are run on any machine. The real device is tools/vk.
CFLAGS += -DSIFT_VULKAN

VK_LAYER = agg/vk/run.c agg/vk/mem.c agg/vk/job.c agg/vk/tab.c agg/vk/cache.c agg/vk/pipe.c agg/vk/fast.c agg/vk/dev.c agg/vk/test.c agg/vk/fin.c agg/vk/seat.c

TESTS += agg/vk/local agg/vk/seat agg/vk/wait agg/vk/sums col/gpu_part
LIB_agg/vk/local = agg/gpu/local.c agg/vk/seat.c
LIB_agg/vk/seat  = agg/vk/seat.c
LIB_agg/vk/wait  = $(VK_LAYER)
LIB_agg/vk/sums  = agg/vk/fin.c
LIB_col/gpu_part = col/gpu.c

TESTS    += agg/gpu/worker
AUTOSTUB += agg/gpu/worker
XFLAGS_agg/gpu/worker = -D_GNU_SOURCE -Wno-unused-function
