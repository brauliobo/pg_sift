/*
 * agg/vk/dev.c - the open and the close of the Vulkan device
 * libvulkan.so.1 by dlopen, the instance, the ranking of the devices (type, memory, and the bandwidth of the PCI Express link that
 * VK_EXT_pci_bus_info and sysfs tell: two identical cards in slots of different speed are not equal for a cold upload), the device with
 * the features the kernels need, its queues (a transfer family of its own when there is one), the pipelines, the slots, the cache.
 */
#include <ctype.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

#include "agg/vk/spv.h"

#define MAXDEV 16

typedef struct Probe
{
	VkPhysicalDevice pd;
	SiftVkDevInfo info;
	bool		budget, pcibus;
	uint32_t	subgroup;
} Probe;

static bool
read_sysfs(const char *dir, const char *file, char *buf, size_t n)
{
	char		path[PATH_MAX];
	FILE	   *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (!f)
		return false;
	buf[0] = 0;
	if (!fgets(buf, (int) n, f))
		buf[0] = 0;
	fclose(f);
	return buf[0] != 0;
}

static double
link_gts(const char *dir, const char *file)
{
	char		b[64];

	return read_sysfs(dir, file, b, sizeof(b)) ? atof(b) : 0.0;
}

/* GB/s a link of gts gigatransfers a second and width lanes moves: the line coding (8b/10b up to 5 GT/s, 128b/130b above) and the protocol (0.78 measured) */
static double
link_gbps(double gts, uint32_t width)
{
	double		coding = gts <= 5.0 ? 0.8 : 128.0 / 130.0;

	return gts * coding * width / 8.0 * 0.78;
}

/*
 * The PCI Express link of the device as the kernel reports it: the width it trained to, and the speed the slot allows (the lesser of the
 * card's and the upstream port's maximum, which a chipset slot caps; the current speed falls with the power state of an idle card).
 */
static void
probe_link(SiftVkDevInfo *d)
{
	char		dir[PATH_MAX],
				real[PATH_MAX],
				b[64];
	double		cap;

	snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%04x:%02x:%02x.%x", d->pci_domain, d->pci_bus, d->pci_dev, d->pci_fn);
	if (!realpath(dir, real))
		return;
	d->link_cur_gts = link_gts(real, "current_link_speed");
	d->link_max_gts = link_gts(real, "max_link_speed");
	d->link_cur_width = read_sysfs(real, "current_link_width", b, sizeof(b)) ? (uint32_t) atoi(b) : 0;
	d->link_max_width = read_sysfs(real, "max_link_width", b, sizeof(b)) ? (uint32_t) atoi(b) : 0;
	cap = d->link_max_gts;
	if (d->link_cur_gts > cap)
		cap = d->link_cur_gts;
	{
		char	   *slash = strrchr(real, '/');

		if (slash)
		{
			double		up;

			*slash = 0;
			up = link_gts(real, "max_link_speed");
			if (up > 0 && up < cap)
				cap = up;
		}
	}
	if (cap > 0 && d->link_cur_width > 0)
		d->pcie_gbps = link_gbps(cap, d->link_cur_width);
}

static int
type_rank(int t)
{
	return t == 2 ? 0 : t == 1 ? 1 : t == 3 ? 2 : 3;
}

/* the order of the devices: type, then the memory, then the bandwidth of the link, then the index */
static bool
better(const Probe *a, int ia, const Probe *b, int ib)
{
	uint64_t	ga = a->info.vram_bytes >> 30,
				gb = b->info.vram_bytes >> 30;

	if (a->info.usable != b->info.usable)
		return a->info.usable;
	if (type_rank(a->info.type) != type_rank(b->info.type))
		return type_rank(a->info.type) < type_rank(b->info.type);
	if (ga != gb)
		return ga > gb;
	if (a->info.pcie_gbps != b->info.pcie_gbps)
		return a->info.pcie_gbps > b->info.pcie_gbps;
	return ia < ib;
}

static bool
all_digits(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (!isdigit((unsigned char) *s))
			return false;
	return true;
}

static bool
name_has(const char *name, const char *sub)
{
	size_t		n = strlen(sub);

	for (; *name; name++)
		if (strncasecmp(name, sub, n) == 0)
			return true;
	return false;
}

static bool
bus_matches(const SiftVkDevInfo *d, const char *sel)
{
	unsigned	bus,
				dev;

	if (!d->pci || sscanf(sel, "%x:%x", &bus, &dev) != 2)
		return false;
	return d->pci_bus == bus && d->pci_dev == dev;
}

/* the loader of the platform: the Vulkan loader of Linux, then the one of the Vulkan SDK and MoltenVK on macOS */
static const char *const loaders[] = {"libvulkan.so.1", "libvulkan.1.dylib", "libMoltenVK.dylib"};

/* an instance that was never made started no thread of a driver: its loader goes back (the library of a live instance stays mapped, see drop_instance) */
static void
close_loader(SiftVk *vk)
{
	dlclose(vk->lib);
	vk->lib = NULL;
	vk->gipa = NULL;
}

static bool
open_instance(SiftVk *vk, char *err, size_t errlen)
{
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pg_sift", .apiVersion = VK_API_VERSION_1_2};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	PFN_vkCreateInstance create;
	sigset_t	old;
	VkResult	r;

	for (size_t i = 0; !vk->lib && i < sizeof(loaders) / sizeof(loaders[0]); i++)
		vk->lib = dlopen(loaders[i], RTLD_NOW | RTLD_LOCAL);
	if (!vk->lib)
	{
		snprintf(err, errlen, "the Vulkan loader (%s) cannot be loaded: %s", loaders[0], dlerror());
		return false;
	}
	vk->gipa = (PFN_vkGetInstanceProcAddr) dlsym(vk->lib, "vkGetInstanceProcAddr");
	create = vk->gipa ? (PFN_vkCreateInstance) vk->gipa(NULL, "vkCreateInstance") : NULL;
	if (!create)
	{
		snprintf(err, errlen, "the Vulkan loader has no vkCreateInstance");
		close_loader(vk);
		return false;
	}
	sigs_block(&old);
	r = create(&ici, NULL, &vk->inst);
	if (r != VK_SUCCESS)
		snprintf(err, errlen, r == VK_ERROR_INCOMPATIBLE_DRIVER ? "no Vulkan driver is installed" : "vkCreateInstance failed (VkResult %d)", (int) r);
	else
	{
#define X(n) vk->n = (PFN_##n) vk->gipa(vk->inst, #n);
		VK_INSTANCE_FNS(X)
#undef X
	}
	sigs_restore(&old);
	if (r != VK_SUCCESS)
		close_loader(vk);
	return r == VK_SUCCESS;
}

static bool
has_ext(SiftVk *vk, VkPhysicalDevice pd, const char *name)
{
	uint32_t	n = 0;
	VkExtensionProperties *ep;
	bool		found = false;

	vk->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
	ep = calloc(n ? n : 1, sizeof(*ep));
	if (!ep)
		return false;
	vk->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, ep);
	for (uint32_t i = 0; i < n; i++)
		found = found || strcmp(ep[i].extensionName, name) == 0;
	free(ep);
	return found;
}

static void
probe_device(SiftVk *vk, VkPhysicalDevice pd, Probe *p)
{
	VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
	VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
	VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
	VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11};
	VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
	VkPhysicalDeviceMemoryProperties mp;
	SiftVkDevInfo *d = &p->info;
	const VkPhysicalDeviceLimits *lim;
	int			t;

	memset(p, 0, sizeof(*p));
	p->pd = pd;
	p->pcibus = has_ext(vk, pd, "VK_EXT_pci_bus_info");
	p->budget = has_ext(vk, pd, "VK_EXT_memory_budget");
	if (p->pcibus)
		sg.pNext = &pci;
	vk->vkGetPhysicalDeviceProperties2(pd, &p2);
	vk->vkGetPhysicalDeviceFeatures2(pd, &f2);
	lim = &p2.properties.limits;
	snprintf(d->name, sizeof(d->name), "%.127s", p2.properties.deviceName);
	d->vendor = p2.properties.vendorID;
	d->device_id = p2.properties.deviceID;
	t = p2.properties.deviceType;
	d->type = t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : t == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 3 : 4;
	p->subgroup = sg.subgroupSize;
	if (p->pcibus)
	{
		d->pci = true;
		d->pci_domain = pci.pciDomain;
		d->pci_bus = pci.pciBus;
		d->pci_dev = pci.pciDevice;
		d->pci_fn = pci.pciFunction;
		if (d->type == 2)
			probe_link(d);
	}
	{
		VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};

		vk->vkGetPhysicalDeviceMemoryProperties2(pd, &mp2);
		mp = mp2.memoryProperties;
	}
	for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
		if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > d->vram_bytes)
			d->vram_bytes = mp.memoryHeaps[i].size;
	d->usable = true;
	if (p2.properties.apiVersion < VK_API_VERSION_1_2)
		snprintf(d->why, sizeof(d->why), "Vulkan 1.2 is needed"), d->usable = false;
	else if (!f2.features.shaderInt64 || !f2.features.shaderFloat64 || !f2.features.shaderInt16 || !f12.shaderBufferInt64Atomics ||
			 !f12.shaderSharedInt64Atomics || !f12.storageBuffer8BitAccess || !f11.storageBuffer16BitAccess || !f12.shaderInt8 ||
			 !f12.shaderSubgroupExtendedTypes)
		snprintf(d->why, sizeof(d->why), "no 64-bit shader arithmetic and atomics, 8 and 16 bit storage or extended subgroup types"), d->usable = false;
	else if (!(sg.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) || !(sg.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) ||
			 !(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT))
		snprintf(d->why, sizeof(d->why), "no subgroup arithmetic and ballot in compute shaders"), d->usable = false;
	else if (lim->maxComputeWorkGroupSize[0] < 256 || lim->maxComputeSharedMemorySize < 32768 || lim->maxPerStageDescriptorStorageBuffers < 12 ||
			 lim->maxPushConstantsSize < 16 || lim->maxStorageBufferRange < ((uint32_t) 64 << 20))
		snprintf(d->why, sizeof(d->why), "below the limits of the kernels"), d->usable = false;
}

static int
choose(const Probe *pr, int n, const char *sel, bool allow_software)
{
	int			best = -1;

	for (int i = 0; i < n; i++)
	{
		if (sel[0] && all_digits(sel))
		{
			if ((uint32_t) atoi(sel) == (uint32_t) i)
				return i;
		}
		else if (sel[0] && strchr(sel, ':'))
		{
			if (bus_matches(&pr[i].info, sel))
				return i;
		}
		else if (sel[0])
		{
			if (name_has(pr[i].info.name, sel))
				return i;
		}
		else if ((pr[i].info.type != 4 || allow_software) && pr[i].info.usable && (best < 0 || better(&pr[i], i, &pr[best], best)))
			best = i;
	}
	return sel[0] ? -1 : best;
}

static int
probe_all(SiftVk *vk, Probe *pr, char *err, size_t errlen)
{
	VkPhysicalDevice pd[MAXDEV];
	uint32_t	n = 0;
	VkResult	r = vk->vkEnumeratePhysicalDevices(vk->inst, &n, NULL);

	if (r != VK_SUCCESS || n == 0)
	{
		snprintf(err, errlen, "no Vulkan device");
		return -1;
	}
	if (n > MAXDEV)
		n = MAXDEV;
	if (vk->vkEnumeratePhysicalDevices(vk->inst, &n, pd) < 0)
	{
		snprintf(err, errlen, "vkEnumeratePhysicalDevices failed");
		return -1;
	}
	for (uint32_t i = 0; i < n; i++)
		probe_device(vk, pd[i], &pr[i]);
	return (int) n;
}

static void
drop_instance(SiftVk *vk)
{
	sigset_t	old;

	sigs_block(&old);
	if (vk->inst)
		vk->vkDestroyInstance(vk->inst, NULL);
	vk->inst = VK_NULL_HANDLE;
	sigs_restore(&old);
	/* the library stays mapped: a driver may have threads or atexit handlers that run code in it */
}

int
sift_vk_list_devices(SiftVkDevInfo *out, int max, int *chosen, const char *device, bool allow_software, char *err, size_t errlen)
{
	SiftVk	   *vk = calloc(1, sizeof(SiftVk));
	Probe	   *pr = calloc(MAXDEV, sizeof(Probe));
	int			n;
	sigset_t	old;

	if (!vk || !pr)
	{
		free(vk);
		free(pr);
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	if (!open_instance(vk, err, errlen))
	{
		if (vk->inst)
			drop_instance(vk);
		free(vk);
		free(pr);
		return -1;
	}
	sigs_block(&old);
	n = probe_all(vk, pr, err, errlen);
	sigs_restore(&old);
	if (n >= 0)
	{
		for (int i = 0; i < n && i < max; i++)
			out[i] = pr[i].info;
		if (chosen)
			*chosen = choose(pr, n, device ? device : "", allow_software);
	}
	drop_instance(vk);
	free(vk);
	free(pr);
	return n;
}

static bool
pick_queues(SiftVk *vk)
{
	uint32_t	nq = 0;
	VkQueueFamilyProperties qfp[32];

	vk->vkGetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, NULL);
	if (nq > 32)
		nq = 32;
	vk->vkGetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, qfp);
	vk->cf = vk->tf = UINT32_MAX;
	for (uint32_t i = 0; i < nq; i++)
		if ((qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (vk->cf == UINT32_MAX || !(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)))
		{
			vk->cf = i;
			vk->stamps = qfp[i].timestampValidBits > 0 && vk->props.limits.timestampPeriod > 0;
		}
	if (vk->cf == UINT32_MAX)
	{
		snprintf(vk->err, sizeof(vk->err), "\"%.100s\" has no compute queue", vk->props.deviceName);
		return false;
	}
	for (uint32_t i = 0; i < nq; i++)
		if ((qfp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qfp[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)))
			vk->tf = i;
	vk->split_queues = vk->tf != UINT32_MAX;
	if (!vk->split_queues)
		vk->tf = vk->cf;
	vk->caps.dedicated_transfer = vk->split_queues;
	return true;
}

/* the bandwidth of the device memory: a few fills of the cache arena, timed on the host around the fence */
static void
calibrate(SiftVk *vk)
{
	Slot	   *s = &vk->slot[0];
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->ccb};
	uint64_t	span = vk->arena.size < ((uint64_t) 256 << 20) ? vk->arena.size : ((uint64_t) 256 << 20);
	double		t0;
	int			reps = 4;

	vk->caps.vram_gbps = 0;
	if (vk->vkResetCommandBuffer(s->ccb, 0) != VK_SUCCESS || vk->vkBeginCommandBuffer(s->ccb, &bi) != VK_SUCCESS)
		return;
	for (int i = 0; i < reps + 1; i++)
	{
		vk->vkCmdFillBuffer(s->ccb, vk->arena.b, 0, span, (uint32_t) i);
		VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
		vk->vkCmdPipelineBarrier(s->ccb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	}
	vk->vkEndCommandBuffer(s->ccb);
	t0 = now_ms();
	if (vk->vkQueueSubmit(vk->cq, 1, &si, s->fence) != VK_SUCCESS)
		return;
	if (vk->vkWaitForFences(vk->dev, 1, &s->fence, VK_TRUE, 5000000000ull) == VK_SUCCESS)
		vk->caps.vram_gbps = (double) span * (reps + 1) / ((now_ms() - t0) * 1e6);
	vk->vkResetFences(vk->dev, 1, &s->fence);
}

static bool
make_device(SiftVk *vk, uint64_t cache_bytes)
{
	float		prio = 1.0f;
	VkDeviceQueueCreateInfo qi[2] = {
		{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->cf, .queueCount = 1, .pQueuePriorities = &prio},
		{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->tf, .queueCount = 1, .pQueuePriorities = &prio}};
	VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .storageBuffer16BitAccess = VK_TRUE};
	VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11, .shaderBufferInt64Atomics = VK_TRUE,
		.shaderSharedInt64Atomics = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE, .shaderInt8 = VK_TRUE, .shaderSubgroupExtendedTypes = VK_TRUE};
	VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
	const char *exts[1] = {"VK_EXT_memory_budget"};
	VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = vk->split_queues ? 2 : 1, .pQueueCreateInfos = qi,
		.enabledExtensionCount = vk->caps.ext_budget ? 1 : 0, .ppEnabledExtensionNames = exts};
	VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk->cf};

	f2.features.shaderInt64 = VK_TRUE;
	f2.features.shaderFloat64 = VK_TRUE;
	f2.features.shaderInt16 = VK_TRUE;
	CK(vk->vkCreateDevice(vk->pd, &di, NULL, &vk->dev));
#define X(n) vk->n = (PFN_##n) vk->vkGetDeviceProcAddr(vk->dev, #n);
	VK_DEVICE_FNS(X)
#undef X
	vk->vkGetDeviceQueue(vk->dev, vk->cf, 0, &vk->cq);
	vk->vkGetDeviceQueue(vk->dev, vk->tf, 0, &vk->tq);
	CK(vk->vkCreateCommandPool(vk->dev, &ci, NULL, &vk->cpool));
	ci.queueFamilyIndex = vk->tf;
	CK(vk->vkCreateCommandPool(vk->dev, &ci, NULL, &vk->tpool));
	vk->cache_want = cache_bytes;
	if (!make_cache(vk, cache_bytes) || !make_pipelines(vk) || !make_slots(vk))
		return false;
	calibrate(vk);
	return true;
}

static void
release(SiftVk *vk)
{
	if (vk->dev)
	{
		if (!vk->lost && !vk->inflight)
			vk->vkDeviceWaitIdle(vk->dev);
		free_tabs(vk);
		free_slots(vk);
		buf_free(vk, &vk->arena);
		free_pipelines(vk);
		if (vk->cpool)
			vk->vkDestroyCommandPool(vk->dev, vk->cpool, NULL);
		if (vk->tpool)
			vk->vkDestroyCommandPool(vk->dev, vk->tpool, NULL);
		vk->vkDestroyDevice(vk->dev, NULL);
		vk->dev = VK_NULL_HANDLE;
	}
	if (vk->inst)
		drop_instance(vk);
	free(vk->slab);
	free(vk->ent);
	free(vk->topn_buf);
	vk->slab = NULL;
	vk->ent = NULL;
	vk->topn_buf = NULL;
	vk->topn_cap = 0;
}

void
sift_vk_close(SiftVk *vk)
{
	sigset_t	old;

	if (!vk)
		return;
	sigs_block(&old);
	if (vk->inflight)
		settle_all(vk);
	release(vk);
	sigs_restore(&old);
	account(vk, -vk->accounted);
	free(vk);
}

SiftVk *
sift_vk_open(const char *device, bool allow_software, uint64_t cache_bytes, char *err, size_t errlen)
{
	SiftVk	   *vk = calloc(1, sizeof(SiftVk));
	Probe	   *pr = calloc(MAXDEV, sizeof(Probe));
	sigset_t	old;
	bool		ok = false;
	int			n,
				pick;

	if (!vk || !pr)
	{
		free(vk);
		free(pr);
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	if (!open_instance(vk, err, errlen))
	{
		if (vk->inst)
			drop_instance(vk);
		free(vk);
		free(pr);
		return NULL;
	}
	sigs_block(&old);
	n = probe_all(vk, pr, vk->err, sizeof(vk->err));
	pick = n < 0 ? -1 : choose(pr, n, device ? device : "", allow_software);
	if (n >= 0 && pick < 0)
		snprintf(vk->err, sizeof(vk->err), device && device[0] ? "no Vulkan device matches \"%s\"" : "no usable hardware Vulkan device (sift.gpu = on takes a software one)",
				 device ? device : "");
	else if (pick >= 0 && !pr[pick].info.usable)
		snprintf(vk->err, sizeof(vk->err), "\"%.100s\": %s", pr[pick].info.name, pr[pick].info.why);
	else if (pick >= 0)
	{
		const SiftVkDevInfo *d = &pr[pick].info;

		vk->pd = pr[pick].pd;
		vk->props.limits.timestampPeriod = 0;
		vk->vkGetPhysicalDeviceProperties(vk->pd, &vk->props);
		{
			VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};

			vk->vkGetPhysicalDeviceMemoryProperties2(vk->pd, &mp2);
			vk->mp = mp2.memoryProperties;
		}
		snprintf(vk->caps.name, sizeof(vk->caps.name), "%s", d->name);
		vk->caps.software = d->type == 4;
		vk->caps.ext_budget = pr[pick].budget;
		vk->caps.ext_pci = pr[pick].pcibus;
		vk->caps.subgroup_size = pr[pick].subgroup;
		vk->caps.pcie_gbps = d->pcie_gbps;
		vk->caps.vram_bytes = d->vram_bytes;
		vk->caps.pci_bus = d->pci_bus;
		vk->caps.pci_dev = d->pci_dev;
		vk->caps.uma = d->type == 1;
		for (uint32_t i = 0; i < vk->mp.memoryTypeCount; i++)
		{
			VkMemoryPropertyFlags f = vk->mp.memoryTypes[i].propertyFlags;

			if ((f & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) == (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
				vk->mp.memoryHeaps[vk->mp.memoryTypes[i].heapIndex].size >= ((uint64_t) 512 << 20))
				vk->caps.rebar = true;
		}
		ok = pick_queues(vk) && make_device(vk, cache_bytes);
		vk->caps.vram_budget_free = heap_budget_free(vk);
	}
	sigs_restore(&old);
	free(pr);
	if (!ok)
	{
		snprintf(err, errlen, "%s", vk->err[0] ? vk->err : "the Vulkan device cannot be opened");
		sigs_block(&old);
		release(vk);
		sigs_restore(&old);
		free(vk);
		return NULL;
	}
	vk->usable = true;
	vk->batch = 1;
	return vk;
}

#endif							/* SIFT_VULKAN */
