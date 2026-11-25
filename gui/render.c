/* render.c - Vulkan renderer for rectangles and glyphs */
#include <stdlib.h>
#include <string.h>

#include "gui.h"

static const uint32_t vert_spv[] =
#include "ui.vert.inc"
;
static const uint32_t frag_spv[] =
#include "ui.frag.inc"
;

#define ATLAS 2048
#define MAXIMG 8

struct inst {
	float rect[4], uv[4], color[4], params[4];
};

struct batch {
	uint32_t first, count;
	VkRect2D sc;
};

static struct {
	VkInstance inst;
	VkSurfaceKHR surf;
	VkPhysicalDevice pd;
	VkDevice dev;
	uint32_t qf;
	VkQueue q;
	VkSwapchainKHR sc;
	VkFormat fmt;
	VkExtent2D ext;
	uint32_t nimg, idx;
	VkImage img[MAXIMG];
	VkImageView view[MAXIMG];
	VkSemaphore done[MAXIMG], acq;
	VkFence fence;
	VkCommandPool pool;
	VkCommandBuffer cb;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dp;
	VkDescriptorSet ds;
	VkImage atlas;
	VkDeviceMemory atlas_mem;
	VkImageView atlas_view;
	VkSampler sampler;
	VkBuffer stage, vb;
	VkDeviceMemory stage_mem, vb_mem;
	void *stage_map, *vb_map;
	size_t vb_cap;
	int atlas_dirty, atlas_init;
	struct inst *data;
	size_t n, cap;
	struct batch *b;
	size_t nb, bcap;
	float scale;
	VkRect2D cur_sc;
	char *shot;
	VkImageUsageFlags usage;
} vk;

#define CHECK(x) do { if ((x) != VK_SUCCESS) { \
	warnx("vulkan: %s failed", #x); return -1; } } while (0)

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(vk.pd, &mp);
	for (i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return 0;
}

static int buffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b,
		  VkDeviceMemory *m, void **map)
{
	VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements mr;

	bi.size = size;
	bi.usage = use;
	CHECK(vkCreateBuffer(vk.dev, &bi, NULL, b));
	vkGetBufferMemoryRequirements(vk.dev, *b, &mr);
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = memtype(mr.memoryTypeBits,
				     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CHECK(vkAllocateMemory(vk.dev, &ai, NULL, m));
	vkBindBufferMemory(vk.dev, *b, *m, 0);
	CHECK(vkMapMemory(vk.dev, *m, 0, size, 0, map));
	return 0;
}

static void destroy_swapchain(void)
{
	uint32_t i;

	for (i = 0; i < vk.nimg; i++) {
		vkDestroyImageView(vk.dev, vk.view[i], NULL);
		vkDestroySemaphore(vk.dev, vk.done[i], NULL);
	}
	if (vk.sc)
		vkDestroySwapchainKHR(vk.dev, vk.sc, NULL);
	vk.sc = VK_NULL_HANDLE;
	vk.nimg = 0;
}

static int make_swapchain(void)
{
	VkSwapchainCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR fmts[32];
	uint32_t nf = 32, i;
	int w, h;

	vkDeviceWaitIdle(vk.dev);
	destroy_swapchain();
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.pd, vk.surf, &caps);
	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.pd, vk.surf, &nf, fmts);
	vk.fmt = fmts[0].format;
	for (i = 0; i < nf; i++)
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
		    fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
			vk.fmt = fmts[i].format;
	plat_size(&w, &h);
	if (caps.currentExtent.width != 0xffffffff) {
		vk.ext = caps.currentExtent;
	} else {
		vk.ext.width = w;
		vk.ext.height = h;
	}
	if (!vk.ext.width || !vk.ext.height)
		return -1;
	ci.surface = vk.surf;
	ci.minImageCount = caps.minImageCount + 1;
	if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
		ci.minImageCount = caps.maxImageCount;
	ci.imageFormat = vk.fmt;
	ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	ci.imageExtent = vk.ext;
	ci.imageArrayLayers = 1;
	vk.usage = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | vk.usage;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = (caps.supportedCompositeAlpha &
			     VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
	    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
	    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	ci.clipped = VK_TRUE;
	CHECK(vkCreateSwapchainKHR(vk.dev, &ci, NULL, &vk.sc));
	vk.nimg = MAXIMG;
	vkGetSwapchainImagesKHR(vk.dev, vk.sc, &vk.nimg, vk.img);
	for (i = 0; i < vk.nimg; i++) {
		VkImageViewCreateInfo vi = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };

		vi.image = vk.img[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = vk.fmt;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		CHECK(vkCreateImageView(vk.dev, &vi, NULL, &vk.view[i]));
		CHECK(vkCreateSemaphore(vk.dev, &si, NULL, &vk.done[i]));
	}
	return 0;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule m = VK_NULL_HANDLE;

	ci.codeSize = size;
	ci.pCode = code;
	vkCreateShaderModule(vk.dev, &ci, NULL, &m);
	return m;
}

static int make_pipeline(void)
{
	VkVertexInputBindingDescription bind = { 0, sizeof(struct inst),
		VK_VERTEX_INPUT_RATE_INSTANCE };
	VkVertexInputAttributeDescription attr[4];
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState ba = { 0 };
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineRenderingCreateInfo ri = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	VkGraphicsPipelineCreateInfo pi = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	VkDescriptorSetLayoutBinding dslb = { 0,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dsli = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0,
		2 * sizeof(float) };
	VkPipelineLayoutCreateInfo pli = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	int i;

	dsli.bindingCount = 1;
	dsli.pBindings = &dslb;
	CHECK(vkCreateDescriptorSetLayout(vk.dev, &dsli, NULL, &vk.dsl));
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &vk.dsl;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	CHECK(vkCreatePipelineLayout(vk.dev, &pli, NULL, &vk.pl));
	for (i = 0; i < 4; i++) {
		attr[i].location = i;
		attr[i].binding = 0;
		attr[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attr[i].offset = i * 4 * sizeof(float);
	}
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = shader(vert_spv, sizeof(vert_spv));
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = shader(frag_spv, sizeof(frag_spv));
	st[1].pName = "main";
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 4;
	vi.pVertexAttributeDescriptions = attr;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	ba.blendEnable = VK_TRUE;
	ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.colorBlendOp = VK_BLEND_OP_ADD;
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.alphaBlendOp = VK_BLEND_OP_ADD;
	ba.colorWriteMask = 0xf;
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &vk.fmt;
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &ds;
	pi.layout = vk.pl;
	CHECK(vkCreateGraphicsPipelines(vk.dev, VK_NULL_HANDLE, 1, &pi, NULL,
					&vk.pipe));
	vkDestroyShaderModule(vk.dev, st[0].module, NULL);
	vkDestroyShaderModule(vk.dev, st[1].module, NULL);
	return 0;
}

static int make_atlas(void)
{
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		1 };
	VkDescriptorPoolCreateInfo dpi = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorSetAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	VkDescriptorImageInfo dii;
	VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	VkMemoryRequirements mr;

	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8_UNORM;
	ii.extent.width = ATLAS;
	ii.extent.height = ATLAS;
	ii.extent.depth = 1;
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	CHECK(vkCreateImage(vk.dev, &ii, NULL, &vk.atlas));
	vkGetImageMemoryRequirements(vk.dev, vk.atlas, &mr);
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = memtype(mr.memoryTypeBits,
				     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	CHECK(vkAllocateMemory(vk.dev, &ai, NULL, &vk.atlas_mem));
	vkBindImageMemory(vk.dev, vk.atlas, vk.atlas_mem, 0);
	vi.image = vk.atlas;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = VK_FORMAT_R8_UNORM;
	vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vi.subresourceRange.levelCount = 1;
	vi.subresourceRange.layerCount = 1;
	CHECK(vkCreateImageView(vk.dev, &vi, NULL, &vk.atlas_view));
	si.magFilter = si.minFilter = VK_FILTER_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW =
	    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CHECK(vkCreateSampler(vk.dev, &si, NULL, &vk.sampler));
	if (buffer(ATLAS * ATLAS, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vk.stage,
		   &vk.stage_mem, &vk.stage_map))
		return -1;
	memset(vk.stage_map, 0, ATLAS * ATLAS);
	vk.atlas_dirty = 1;
	dpi.maxSets = 1;
	dpi.poolSizeCount = 1;
	dpi.pPoolSizes = &ps;
	CHECK(vkCreateDescriptorPool(vk.dev, &dpi, NULL, &vk.dp));
	dai.descriptorPool = vk.dp;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &vk.dsl;
	CHECK(vkAllocateDescriptorSets(vk.dev, &dai, &vk.ds));
	dii.sampler = vk.sampler;
	dii.imageView = vk.atlas_view;
	dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	w.dstSet = vk.ds;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(vk.dev, 1, &w, 0, NULL);
	return 0;
}

/* create the Vulkan instance, device, swapchain and pipeline */
int r_init(void)
{
	const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, plat_vk_ext() };
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkPhysicalDeviceVulkan13Features f13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	VkDeviceQueueCreateInfo qi = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkCommandPoolCreateInfo cpi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkPhysicalDevice pds[16];
	uint32_t npd = 16, i, j, best = 0;
	float prio = 1;

	app.pApplicationName = "epis";
	app.apiVersion = VK_API_VERSION_1_3;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = 2;
	ici.ppEnabledExtensionNames = iext;
	CHECK(vkCreateInstance(&ici, NULL, &vk.inst));
	if (plat_vk_surface(vk.inst, &vk.surf))
		return -1;
	CHECK(vkEnumeratePhysicalDevices(vk.inst, &npd, pds));
	vk.pd = VK_NULL_HANDLE;
	for (i = 0; i < npd; i++) {
		VkPhysicalDeviceProperties p;
		VkQueueFamilyProperties qf[16];
		uint32_t nq = 16, score;

		vkGetPhysicalDeviceProperties(pds[i], &p);
		if (p.apiVersion < VK_API_VERSION_1_3)
			continue;
		vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qf);
		for (j = 0; j < nq; j++) {
			VkBool32 present = 0;

			vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], j,
							     vk.surf, &present);
			if (!(qf[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
			    !present)
				continue;
			/* integrated GPUs first: they draw a UI with less power */
			score = p.deviceType ==
			    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3 :
			    p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
			    ? 2 : 1;
			if (score > best) {
				best = score;
				vk.pd = pds[i];
				vk.qf = j;
			}
			break;
		}
	}
	if (!vk.pd) {
		warnx("vulkan: no suitable device");
		return -1;
	}
	qi.queueFamilyIndex = vk.qf;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	f13.dynamicRendering = VK_TRUE;
	dci.pNext = &f13;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = dext;
	CHECK(vkCreateDevice(vk.pd, &dci, NULL, &vk.dev));
	vkGetDeviceQueue(vk.dev, vk.qf, 0, &vk.q);
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = vk.qf;
	CHECK(vkCreateCommandPool(vk.dev, &cpi, NULL, &vk.pool));
	cai.commandPool = vk.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	CHECK(vkAllocateCommandBuffers(vk.dev, &cai, &vk.cb));
	CHECK(vkCreateSemaphore(vk.dev, &si, NULL, &vk.acq));
	fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	CHECK(vkCreateFence(vk.dev, &fi, NULL, &vk.fence));
	make_swapchain();
	if (make_pipeline() || make_atlas())
		return -1;
	return 0;
}

void r_quit(void)
{
	if (!vk.dev)
		return;
	vkDeviceWaitIdle(vk.dev);
	destroy_swapchain();
	if (vk.vb) {
		vkDestroyBuffer(vk.dev, vk.vb, NULL);
		vkFreeMemory(vk.dev, vk.vb_mem, NULL);
	}
	vkDestroyBuffer(vk.dev, vk.stage, NULL);
	vkFreeMemory(vk.dev, vk.stage_mem, NULL);
	vkDestroySampler(vk.dev, vk.sampler, NULL);
	vkDestroyImageView(vk.dev, vk.atlas_view, NULL);
	vkDestroyImage(vk.dev, vk.atlas, NULL);
	vkFreeMemory(vk.dev, vk.atlas_mem, NULL);
	vkDestroyDescriptorPool(vk.dev, vk.dp, NULL);
	vkDestroyPipeline(vk.dev, vk.pipe, NULL);
	vkDestroyPipelineLayout(vk.dev, vk.pl, NULL);
	vkDestroyDescriptorSetLayout(vk.dev, vk.dsl, NULL);
	vkDestroySemaphore(vk.dev, vk.acq, NULL);
	vkDestroyFence(vk.dev, vk.fence, NULL);
	vkDestroyCommandPool(vk.dev, vk.pool, NULL);
	vkDestroyDevice(vk.dev, NULL);
	vkDestroySurfaceKHR(vk.inst, vk.surf, NULL);
	vkDestroyInstance(vk.inst, NULL);
	free(vk.data);
	free(vk.b);
}

/* copy a region of the glyph atlas for upload on the next frame */
void r_atlas(const uint8_t *pix, int w, int h)
{
	int y;

	for (y = 0; y < h && y < ATLAS; y++)
		memcpy((uint8_t *)vk.stage_map + y * ATLAS, pix + y * w,
		       w < ATLAS ? w : ATLAS);
	vk.atlas_dirty = 1;
}

static uint32_t clear_color;

/* start a frame, returns -1 when nothing can be drawn */
int r_begin(uint32_t clear)
{
	VkResult res;
	int w, h;

	vkWaitForFences(vk.dev, 1, &vk.fence, VK_TRUE, UINT64_MAX);
	plat_size(&w, &h);
	if (!vk.sc || (uint32_t)w != vk.ext.width ||
	    (uint32_t)h != vk.ext.height)
		if (make_swapchain())
			return -1;
	res = vkAcquireNextImageKHR(vk.dev, vk.sc, UINT64_MAX, vk.acq,
				    VK_NULL_HANDLE, &vk.idx);
	if (res == VK_ERROR_OUT_OF_DATE_KHR) {
		if (make_swapchain())
			return -1;
		res = vkAcquireNextImageKHR(vk.dev, vk.sc, UINT64_MAX, vk.acq,
					    VK_NULL_HANDLE, &vk.idx);
	}
	if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		return -1;
	vk.scale = plat_scale();
	vk.n = vk.nb = 0;
	vk.cur_sc.offset.x = vk.cur_sc.offset.y = 0;
	vk.cur_sc.extent = vk.ext;
	clear_color = clear;
	return 0;
}

static void push(struct inst *in)
{
	if (!vk.nb || memcmp(&vk.b[vk.nb - 1].sc, &vk.cur_sc,
			     sizeof(vk.cur_sc))) {
		if (vk.nb == vk.bcap) {
			vk.bcap = vk.bcap * 2 + 16;
			vk.b = xrealloc(vk.b, vk.bcap * sizeof(*vk.b));
		}
		vk.b[vk.nb].first = vk.n;
		vk.b[vk.nb].count = 0;
		vk.b[vk.nb++].sc = vk.cur_sc;
	}
	if (vk.n == vk.cap) {
		vk.cap = vk.cap * 2 + 1024;
		vk.data = xrealloc(vk.data, vk.cap * sizeof(*vk.data));
	}
	vk.data[vk.n++] = *in;
	vk.b[vk.nb - 1].count++;
}

static void unpack(uint32_t c, float *out)
{
	out[0] = (c >> 24 & 0xff) / 255.0f;
	out[1] = (c >> 16 & 0xff) / 255.0f;
	out[2] = (c >> 8 & 0xff) / 255.0f;
	out[3] = (c & 0xff) / 255.0f;
}

/* draw a rounded rectangle in logical coordinates; border > 0 outlines */
void r_rect(float x, float y, float w, float h, uint32_t c, float radius,
	    float border)
{
	struct inst in = { 0 };
	float s = vk.scale;

	in.rect[0] = x * s;
	in.rect[1] = y * s;
	in.rect[2] = w * s;
	in.rect[3] = h * s;
	unpack(c, in.color);
	in.params[0] = radius * s;
	in.params[1] = border * s;
	push(&in);
}

/* draw an atlas glyph at physical pixel coordinates */
void r_glyph(float x, float y, float w, float h, const float uv[4],
	     uint32_t c)
{
	struct inst in = { 0 };

	in.rect[0] = x;
	in.rect[1] = y;
	in.rect[2] = w;
	in.rect[3] = h;
	memcpy(in.uv, uv, sizeof(in.uv));
	unpack(c, in.color);
	in.params[2] = 1;
	push(&in);
}

void r_scissor(float x, float y, float w, float h)
{
	float s = vk.scale;
	int x0, y0, x1, y1;

	if (w < 0) {
		vk.cur_sc.offset.x = vk.cur_sc.offset.y = 0;
		vk.cur_sc.extent = vk.ext;
		return;
	}
	x0 = x * s < 0 ? 0 : x * s;
	y0 = y * s < 0 ? 0 : y * s;
	x1 = (x + w) * s;
	y1 = (y + h) * s;
	if (x1 > (int)vk.ext.width)
		x1 = vk.ext.width;
	if (y1 > (int)vk.ext.height)
		y1 = vk.ext.height;
	vk.cur_sc.offset.x = x0;
	vk.cur_sc.offset.y = y0;
	vk.cur_sc.extent.width = x1 > x0 ? x1 - x0 : 0;
	vk.cur_sc.extent.height = y1 > y0 ? y1 - y0 : 0;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to,
		    VkPipelineStageFlags src, VkPipelineStageFlags dst,
		    VkAccessFlags sa, VkAccessFlags da)
{
	VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

	b.oldLayout = from;
	b.newLayout = to;
	b.srcAccessMask = sa;
	b.dstAccessMask = da;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex =
	    VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(vk.cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkBuffer shot_buf;
static VkDeviceMemory shot_mem;
static void *shot_map;

/* copy the current swapchain image into a host buffer */
static int shot_record(void)
{
	VkBufferImageCopy r = { 0 };
	size_t size = (size_t)vk.ext.width * vk.ext.height * 4;

	if (!vk.usage || buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				&shot_buf, &shot_mem, &shot_map)) {
		warnx("screenshots are not supported by this surface");
		free(vk.shot);
		vk.shot = NULL;
		return -1;
	}
	barrier(vk.img[vk.idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT);
	r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	r.imageSubresource.layerCount = 1;
	r.imageExtent.width = vk.ext.width;
	r.imageExtent.height = vk.ext.height;
	r.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(vk.cb, vk.img[vk.idx],
			       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buf,
			       1, &r);
	return 0;
}

/* write the captured frame as a binary PPM */
static void shot_write(void)
{
	FILE *f = fopen(vk.shot, "wb");
	const uint8_t *p = shot_map;
	size_t i, n = (size_t)vk.ext.width * vk.ext.height;
	int bgr = vk.fmt == VK_FORMAT_B8G8R8A8_UNORM ||
	    vk.fmt == VK_FORMAT_B8G8R8A8_SRGB;

	vkWaitForFences(vk.dev, 1, &vk.fence, VK_TRUE, UINT64_MAX);
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", vk.ext.width, vk.ext.height);
		for (i = 0; i < n; i++, p += 4) {
			fputc(p[bgr ? 2 : 0], f);
			fputc(p[1], f);
			fputc(p[bgr ? 0 : 2], f);
		}
		fclose(f);
	} else {
		warnx("cannot write %s", vk.shot);
	}
	vkDestroyBuffer(vk.dev, shot_buf, NULL);
	vkFreeMemory(vk.dev, shot_mem, NULL);
	free(vk.shot);
	vk.shot = NULL;
}

/* save the next frame to path */
void r_screenshot(const char *path)
{
	free(vk.shot);
	vk.shot = xstrdup(path);
}

/* record and submit the frame, then present it */
void r_end(void)
{
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkRenderingAttachmentInfo ca = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO };
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkViewport vp = { 0 };
	VkDeviceSize off = 0;
	float screen[2], cc[4];
	size_t need = vk.n * sizeof(struct inst), i;

	if (need > vk.vb_cap) {
		if (vk.vb) {
			vkDestroyBuffer(vk.dev, vk.vb, NULL);
			vkFreeMemory(vk.dev, vk.vb_mem, NULL);
		}
		vk.vb_cap = need * 2;
		if (buffer(vk.vb_cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			   &vk.vb, &vk.vb_mem, &vk.vb_map)) {
			vk.vb = VK_NULL_HANDLE;
			vk.vb_cap = 0;
			return;
		}
	}
	if (need)
		memcpy(vk.vb_map, vk.data, need);
	vkResetFences(vk.dev, 1, &vk.fence);
	vkResetCommandBuffer(vk.cb, 0);
	vkBeginCommandBuffer(vk.cb, &bi);
	if (vk.atlas_dirty) {
		VkBufferImageCopy r = { 0 };

		barrier(vk.atlas, vk.atlas_init ?
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			VK_ACCESS_TRANSFER_WRITE_BIT);
		r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		r.imageSubresource.layerCount = 1;
		r.imageExtent.width = ATLAS;
		r.imageExtent.height = ATLAS;
		r.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(vk.cb, vk.stage, vk.atlas,
				       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
				       &r);
		barrier(vk.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		vk.atlas_dirty = 0;
		vk.atlas_init = 1;
	}
	barrier(vk.img[vk.idx], VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	unpack(clear_color, cc);
	ca.imageView = vk.view[vk.idx];
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	memcpy(ca.clearValue.color.float32, cc, sizeof(cc));
	ri.renderArea.extent = vk.ext;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &ca;
	vkCmdBeginRendering(vk.cb, &ri);
	if (vk.n && vk.vb) {
		vkCmdBindPipeline(vk.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				  vk.pipe);
		vkCmdBindDescriptorSets(vk.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					vk.pl, 0, 1, &vk.ds, 0, NULL);
		vkCmdBindVertexBuffers(vk.cb, 0, 1, &vk.vb, &off);
		vp.width = vk.ext.width;
		vp.height = vk.ext.height;
		vp.maxDepth = 1;
		vkCmdSetViewport(vk.cb, 0, 1, &vp);
		screen[0] = vk.ext.width;
		screen[1] = vk.ext.height;
		vkCmdPushConstants(vk.cb, vk.pl, VK_SHADER_STAGE_VERTEX_BIT, 0,
				   sizeof(screen), screen);
		for (i = 0; i < vk.nb; i++) {
			if (!vk.b[i].sc.extent.width ||
			    !vk.b[i].sc.extent.height)
				continue;
			vkCmdSetScissor(vk.cb, 0, 1, &vk.b[i].sc);
			vkCmdDraw(vk.cb, 6, vk.b[i].count, 0, vk.b[i].first);
		}
	}
	vkCmdEndRendering(vk.cb);
	if (vk.shot && !shot_record())
		barrier(vk.img[vk.idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT, 0);
	else
		barrier(vk.img[vk.idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	vkEndCommandBuffer(vk.cb);
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &vk.acq;
	si.pWaitDstStageMask = &ws;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &vk.cb;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &vk.done[vk.idx];
	vkQueueSubmit(vk.q, 1, &si, vk.fence);
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &vk.done[vk.idx];
	pi.swapchainCount = 1;
	pi.pSwapchains = &vk.sc;
	pi.pImageIndices = &vk.idx;
	if (vkQueuePresentKHR(vk.q, &pi) == VK_ERROR_OUT_OF_DATE_KHR)
		make_swapchain();
	if (vk.shot)
		shot_write();
}
