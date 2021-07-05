#include "vk_rtx.h"

#include "vk_core.h"
#include "vk_common.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_cvar.h"
#include "vk_textures.h"
#include "vk_light.h"
#include "vk_descriptor.h"
#include "vk_ray_internal.h"

#include "eiface.h"
#include "xash3d_mathlib.h"

#include <string.h>

#define MAX_SCRATCH_BUFFER (16*1024*1024)
#define MAX_ACCELS_BUFFER (64*1024*1024)

#define MAX_LIGHT_LEAVES 8192

// TODO settings/realtime modifiable/adaptive
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 720

// TODO sync with shaders
// TODO optimal values
#define WG_W 16
#define WG_H 8

typedef struct {
	vec3_t pos;
	float radius;
	vec3_t color;
	float padding_;
} vk_light_t;

typedef struct {
	float t;
	int bounces;
	float prev_frame_blend_factor;
} vk_rtx_push_constants_t;

typedef struct {
	int min_cell[4], size[3]; // 4th element is padding
	vk_lights_cell_t cells[MAX_LIGHT_CLUSTERS];
} vk_ray_shader_light_grid;

enum {
	RayDescBinding_DestImage = 0,
	RayDescBinding_TLAS = 1,
	RayDescBinding_UBOMatrices = 2,
	RayDescBinding_Kusochki = 3,
	RayDescBinding_Indices = 4,
	RayDescBinding_Vertices = 5,
	RayDescBinding_UBOLights = 6,
	RayDescBinding_EmissiveKusochki = 7,
	RayDescBinding_PrevFrame = 8,
	RayDescBinding_LightClusters = 9,
	RayDescBinding_Textures = 10,
	RayDescBinding_COUNT
};

static struct {
	vk_descriptors_t descriptors;
	VkDescriptorSetLayoutBinding desc_bindings[RayDescBinding_COUNT];
	vk_descriptor_value_t desc_values[RayDescBinding_COUNT];
	VkDescriptorSet desc_sets[1];

	VkPipeline pipeline;

	// Stores AS built data. Lifetime similar to render buffer:
	// - some portion lives for entire map lifetime
	// - some portion lives only for a single frame (may have several frames in flight)
	// TODO: unify this with render buffer
	// Needs: AS_STORAGE_BIT, SHADER_DEVICE_ADDRESS_BIT
	vk_buffer_t accels_buffer;
	vk_ring_buffer_t accels_buffer_alloc;

	// Temp: lives only during a single frame (may have many in flight)
	// Used for building ASes;
	// Needs: AS_STORAGE_BIT, SHADER_DEVICE_ADDRESS_BIT
	vk_buffer_t scratch_buffer;
	VkDeviceAddress accels_buffer_addr, scratch_buffer_addr;

	// Temp-ish: used for making TLAS, contains addressed to all used BLASes
	// Lifetime and nature of usage similar to scratch_buffer
	// TODO: unify them
	// Needs: SHADER_DEVICE_ADDRESS, STORAGE_BUFFER, AS_BUILD_INPUT_READ_ONLY
	vk_buffer_t tlas_geom_buffer;

	// Planned to contain seveal types of data:
	// - grid structure itself
	// - lights data:
	//   - dlights (fully dynamic)
	//   - entity lights (can be dynamic with light styles)
	//   - surface lights (map geometry is static, however brush models can have them too and move around (e.g. wagonchik and elevators))
	// Therefore, this is also dynamic and lifetime is per-frame
	// TODO: unify with scratch buffer
	// Needs: STORAGE_BUFFER
	// Can be potentially crated using compute shader (would need shader write bit)
	vk_buffer_t light_grid_buffer;

	// TODO need several TLASes for N frames in flight
	VkAccelerationStructureKHR tlas;

	// Per-frame data that is accumulated between RayFrameBegin and End calls
	struct {
		uint32_t scratch_offset; // for building dynamic blases
	} frame;

	unsigned frame_number;
	vk_image_t frames[2];

	qboolean reload_pipeline;
} g_rtx = {0};

VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer) {
	const VkBufferDeviceAddressInfo bdai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
	return vkGetBufferDeviceAddress(vk_core.device, &bdai);
}

static VkDeviceAddress getASAddress(VkAccelerationStructureKHR as) {
	VkAccelerationStructureDeviceAddressInfoKHR asdai = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
		.accelerationStructure = as,
	};
	return vkGetAccelerationStructureDeviceAddressKHR(vk_core.device, &asdai);
}

// TODO split this into smaller building blocks in a separate module
qboolean createOrUpdateAccelerationStructure(VkCommandBuffer cmdbuf, const as_build_args_t *args, vk_ray_model_t *model) {
	qboolean should_create = *args->p_accel == VK_NULL_HANDLE;
#if 1 // update does not work at all on AMD gpus
	qboolean is_update = false; // FIXME this crashes for some reason !should_create && args->dynamic;
#else
	qboolean is_update = !should_create && args->dynamic;
#endif

	VkAccelerationStructureBuildGeometryInfoKHR build_info = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.type = args->type,
		.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | ( args->dynamic ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0),
		.mode =  is_update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
		.geometryCount = args->n_geoms,
		.pGeometries = args->geoms,
		.srcAccelerationStructure = *args->p_accel,
	};

	VkAccelerationStructureBuildSizesInfoKHR build_size = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
	};

	uint32_t scratch_buffer_size = 0;

	ASSERT(args->geoms);
	ASSERT(args->n_geoms > 0);
	ASSERT(args->p_accel);

	vkGetAccelerationStructureBuildSizesKHR(
		vk_core.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info, args->max_prim_counts, &build_size);

	scratch_buffer_size = is_update ? build_size.updateScratchSize : build_size.buildScratchSize;

#if 0
	{
		uint32_t max_prims = 0;
		for (int i = 0; i < args->n_geoms; ++i)
			max_prims += args->max_prim_counts[i];
		gEngine.Con_Reportf(
			"AS max_prims=%u, n_geoms=%u, build size: %d, scratch size: %d\n", max_prims, args->n_geoms, build_size.accelerationStructureSize, build_size.buildScratchSize);
	}
#endif

	if (MAX_SCRATCH_BUFFER < g_rtx.frame.scratch_offset + scratch_buffer_size) {
		gEngine.Con_Printf(S_ERROR "Scratch buffer overflow: left %u bytes, but need %u\n",
			MAX_SCRATCH_BUFFER - g_rtx.frame.scratch_offset,
			scratch_buffer_size);
		return false;
	}

	if (should_create) {
		const uint32_t as_size = build_size.accelerationStructureSize;
		const uint32_t buffer_offset = VK_RingBuffer_Alloc(&g_rtx.accels_buffer_alloc, as_size, 256);
		const VkAccelerationStructureCreateInfoKHR asci = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
			.buffer = g_rtx.accels_buffer.buffer,
			.offset = buffer_offset,
			.type = args->type,
			.size = as_size,
		};

		if (buffer_offset == AllocFailed) {
			gEngine.Con_Printf(S_ERROR "Failed to allocated %u bytes for accel buffer\n", asci.size);
			return false;
		}

		XVK_CHECK(vkCreateAccelerationStructureKHR(vk_core.device, &asci, NULL, args->p_accel));
		SET_DEBUG_NAME(*args->p_accel, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, args->debug_name);

		if (model) {
			model->size = asci.size;
			model->debug.as_offset = buffer_offset;
		}

		// gEngine.Con_Reportf("AS=%p, n_geoms=%u, build: %#x %d %#x\n", *args->p_accel, args->n_geoms, buffer_offset, asci.size, buffer_offset + asci.size);
	}

	// If not enough data for building, just create
	if (!cmdbuf || !args->build_ranges)
		return true;

	if (model) {
		ASSERT(model->size >= build_size.accelerationStructureSize);
	}

	build_info.dstAccelerationStructure = *args->p_accel;
	build_info.scratchData.deviceAddress = g_rtx.scratch_buffer_addr + g_rtx.frame.scratch_offset;
	//uint32_t scratch_offset_initial = g_rtx.frame.scratch_offset;
	g_rtx.frame.scratch_offset += scratch_buffer_size;
	g_rtx.frame.scratch_offset = ALIGN_UP(g_rtx.frame.scratch_offset, vk_core.physical_device.properties_accel.minAccelerationStructureScratchOffsetAlignment);

	//gEngine.Con_Reportf("AS=%p, n_geoms=%u, scratch: %#x %d %#x\n", *args->p_accel, args->n_geoms, scratch_offset_initial, scratch_buffer_size, scratch_offset_initial + scratch_buffer_size);

	vkCmdBuildAccelerationStructuresKHR(cmdbuf, 1, &build_info, &args->build_ranges);
	return true;
}

static void createTlas( VkCommandBuffer cmdbuf ) {
	const VkAccelerationStructureGeometryKHR tl_geom[] = {
		{
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			//.flags = VK_GEOMETRY_OPAQUE_BIT,
			.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
			.geometry.instances =
				(VkAccelerationStructureGeometryInstancesDataKHR){
					.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
					.data.deviceAddress = getBufferDeviceAddress(g_rtx.tlas_geom_buffer.buffer),
					.arrayOfPointers = VK_FALSE,
				},
		},
	};
	const uint32_t tl_max_prim_counts[ARRAYSIZE(tl_geom)] = { cmdbuf == VK_NULL_HANDLE ? MAX_ACCELS : g_ray_model_state.frame.num_models };
	const VkAccelerationStructureBuildRangeInfoKHR tl_build_range = {
		.primitiveCount = g_ray_model_state.frame.num_models,
	};
	const as_build_args_t asrgs = {
		.geoms = tl_geom,
		.max_prim_counts = tl_max_prim_counts,
		.build_ranges =  cmdbuf == VK_NULL_HANDLE ? NULL : &tl_build_range,
		.n_geoms = ARRAYSIZE(tl_geom),
		.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		// we can't really rebuild TLAS because instance count changes are not allowed .dynamic = true,
		.dynamic = false,
		.p_accel = &g_rtx.tlas,
		.debug_name = "TLAS",
	};
	if (!createOrUpdateAccelerationStructure(cmdbuf, &asrgs, NULL)) {
		gEngine.Host_Error("Could not create/update TLAS\n");
		return;
	}
}

void VK_RayNewMap( void ) {
	ASSERT(vk_core.rtx);

	VK_RingBuffer_Clear(&g_rtx.accels_buffer_alloc);
	VK_RingBuffer_Clear(&g_ray_model_state.kusochki_alloc);

	// Clear model cache
	for (int i = 0; i < ARRAYSIZE(g_ray_model_state.models_cache); ++i) {
		vk_ray_model_t *model = g_ray_model_state.models_cache + i;
		VK_RayModelDestroy(model);
	}

	// Recreate tlas
	// Why here and not in init: to make sure that its memory is preserved. Map init will clear all memory regions.
	{
		if (g_rtx.tlas != VK_NULL_HANDLE) {
			vkDestroyAccelerationStructureKHR(vk_core.device, g_rtx.tlas, NULL);
			g_rtx.tlas = VK_NULL_HANDLE;
		}

		createTlas(VK_NULL_HANDLE);
	}
}

void VK_RayMapLoadEnd( void ) {
	VK_RingBuffer_Fix(&g_rtx.accels_buffer_alloc);
	VK_RingBuffer_Fix(&g_ray_model_state.kusochki_alloc);
}

void VK_RayFrameBegin( void )
{
	ASSERT(vk_core.rtx);

	g_rtx.frame.scratch_offset = 0;

	if (g_ray_model_state.freeze_models)
		return;

	XVK_RayModel_ClearForNextFrame();

	// TODO shouldn't we do this in freeze models mode anyway?
	VK_LightsFrameInit();
}

static void createPipeline( void )
{
	struct RayShaderSpec {
		int max_dlights;
		int max_emissive_kusochki;
		uint32_t max_visible_dlights;
		uint32_t max_visible_surface_lights;
		float light_grid_cell_size;
		int max_light_clusters;
	} spec_data = {
		.max_dlights = MAX_DLIGHTS,
		.max_emissive_kusochki = MAX_EMISSIVE_KUSOCHKI,
		.max_visible_dlights = MAX_VISIBLE_DLIGHTS,
		.max_visible_surface_lights = MAX_VISIBLE_SURFACE_LIGHTS,
		.light_grid_cell_size = LIGHT_GRID_CELL_SIZE,
		.max_light_clusters = MAX_LIGHT_CLUSTERS,
	};
	const VkSpecializationMapEntry spec_map[] = {
		{.constantID = 0, .offset = offsetof(struct RayShaderSpec, max_dlights), .size = sizeof(int) },
		{.constantID = 1, .offset = offsetof(struct RayShaderSpec, max_emissive_kusochki), .size = sizeof(int) },
		{.constantID = 2, .offset = offsetof(struct RayShaderSpec, max_visible_dlights), .size = sizeof(uint32_t) },
		{.constantID = 3, .offset = offsetof(struct RayShaderSpec, max_visible_surface_lights), .size = sizeof(uint32_t) },
		{.constantID = 4, .offset = offsetof(struct RayShaderSpec, light_grid_cell_size), .size = sizeof(float) },
		{.constantID = 5, .offset = offsetof(struct RayShaderSpec, max_light_clusters), .size = sizeof(int) },
	};

	VkSpecializationInfo spec = {
		.mapEntryCount = ARRAYSIZE(spec_map),
		.pMapEntries = spec_map,
		.dataSize = sizeof(spec_data),
		.pData = &spec_data,
	};
	const vk_pipeline_compute_create_info_t ci = {
		.layout = g_rtx.descriptors.pipeline_layout,
		.shader_filename = "rtx.comp.spv",
		.specialization_info = &spec,
	};

	g_rtx.pipeline = VK_PipelineComputeCreate(&ci);
	ASSERT(g_rtx.pipeline);
}

void VK_RayFrameEnd(const vk_ray_frame_render_args_t* args)
{
	const VkCommandBuffer cmdbuf = args->cmdbuf;
	const vk_image_t* frame_src = g_rtx.frames + ((g_rtx.frame_number + 1) % 2);
	const vk_image_t* frame_dst = g_rtx.frames + (g_rtx.frame_number % 2);

	ASSERT(vk_core.rtx);
	// ubo should contain two matrices
	// FIXME pass these matrices explicitly to let RTX module handle ubo itself
	ASSERT(args->ubo.size == sizeof(float) * 16 * 2);

	if (vk_core.debug)
		XVK_RayModel_Validate();

	g_rtx.frame_number++;

	// Finalize and update dynamic lights
	{
		VK_LightsFrameFinalize();

		// Upload light grid
		{
			vk_ray_shader_light_grid *grid = g_rtx.light_grid_buffer.mapped;
			ASSERT(g_lights.map.grid_cells <= MAX_LIGHT_CLUSTERS);
			VectorCopy(g_lights.map.grid_min_cell, grid->min_cell);
			VectorCopy(g_lights.map.grid_size, grid->size);
			memcpy(grid->cells, g_lights.cells, g_lights.map.grid_cells * sizeof(vk_lights_cell_t));
		}

		// Upload dynamic emissive kusochki
		{
			vk_emissive_kusochki_t *ek = g_ray_model_state.emissive_kusochki_buffer.mapped;
			ASSERT(g_lights.num_emissive_surfaces <= MAX_EMISSIVE_KUSOCHKI);
			ek->num_kusochki = g_lights.num_emissive_surfaces;
			for (int i = 0; i < g_lights.num_emissive_surfaces; ++i) {
				ek->kusochki[i].kusok_index = g_lights.emissive_surfaces[i].kusok_index;
				VectorCopy(g_lights.emissive_surfaces[i].emissive, ek->kusochki[i].emissive_color);
				Matrix3x4_Copy(ek->kusochki[i].transform, g_lights.emissive_surfaces[i].transform);
			}
		}
	}

	if (g_rtx.reload_pipeline) {
		gEngine.Con_Printf(S_WARN "Reloading RTX shaders/pipelines\n");
		// TODO gracefully handle reload errors: need to change createPipeline, loadShader, VK_PipelineCreate...
		vkDestroyPipeline(vk_core.device, g_rtx.pipeline, NULL);
		createPipeline();
		g_rtx.reload_pipeline = false;
	}

	// Upload all blas instances references to GPU mem
	{
		VkAccelerationStructureInstanceKHR* inst = g_rtx.tlas_geom_buffer.mapped;
		for (int i = 0; i < g_ray_model_state.frame.num_models; ++i) {
			const vk_ray_draw_model_t* const model = g_ray_model_state.frame.models + i;
			ASSERT(model->model);
			ASSERT(model->model->as != VK_NULL_HANDLE);
			inst[i] = (VkAccelerationStructureInstanceKHR){
				.instanceCustomIndex = model->model->kusochki_offset,
				.mask = 0xff,
				.instanceShaderBindingTableRecordOffset = 0,
				.flags = 0,
				.accelerationStructureReference = getASAddress(model->model->as), // TODO cache this addr
			};
			memcpy(&inst[i].transform, model->transform_row, sizeof(VkTransformMatrixKHR));
		}
	}

	// Barrier for building all BLASes
	// BLAS building is now in cmdbuf, need to synchronize with results
	{
		VkBufferMemoryBarrier bmb[] = { {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, // | VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
			.buffer = g_rtx.accels_buffer.buffer,
			.offset = 0,
			.size = VK_WHOLE_SIZE,
		} };
		vkCmdPipelineBarrier(cmdbuf,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			0, 0, NULL, ARRAYSIZE(bmb), bmb, 0, NULL);
	}

	// 2. Build TLAS
	if (g_ray_model_state.frame.num_models > 0)
	{
		createTlas(cmdbuf);
	}

	if (g_rtx.tlas != VK_NULL_HANDLE)
	{
		// 3. Update descriptor sets (bind dest image, tlas, projection matrix)
		VkDescriptorImageInfo dii_all_textures[MAX_TEXTURES];

		g_rtx.desc_values[RayDescBinding_DestImage].image = (VkDescriptorImageInfo){
			.sampler = VK_NULL_HANDLE,
			.imageView = frame_dst->view,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};

		g_rtx.desc_values[RayDescBinding_PrevFrame].image = (VkDescriptorImageInfo){
			.sampler = VK_NULL_HANDLE,
			.imageView = frame_src->view,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};

		g_rtx.desc_values[RayDescBinding_UBOMatrices].buffer = (VkDescriptorBufferInfo){
			.buffer = args->ubo.buffer,
			.offset = args->ubo.offset,
			.range = args->ubo.size,
		};

		g_rtx.desc_values[RayDescBinding_Kusochki].buffer = (VkDescriptorBufferInfo){
			.buffer = g_ray_model_state.kusochki_buffer.buffer,
			.offset = 0,
			.range = VK_WHOLE_SIZE, // TODO fails validation when empty g_rtx_scene.num_models * sizeof(vk_kusok_data_t),
		};

		g_rtx.desc_values[RayDescBinding_Indices].buffer = (VkDescriptorBufferInfo){
			.buffer = args->geometry_data.buffer,
			.offset = 0,
			.range = VK_WHOLE_SIZE, // TODO fails validation when empty args->geometry_data.size,
		};

		g_rtx.desc_values[RayDescBinding_Vertices].buffer = (VkDescriptorBufferInfo){
			.buffer = args->geometry_data.buffer,
			.offset = 0,
			.range = VK_WHOLE_SIZE, // TODO fails validation when empty args->geometry_data.size,
		};

		g_rtx.desc_values[RayDescBinding_TLAS].accel = (VkWriteDescriptorSetAccelerationStructureKHR){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount = 1,
			.pAccelerationStructures = &g_rtx.tlas,
		};

		g_rtx.desc_values[RayDescBinding_UBOLights].buffer = (VkDescriptorBufferInfo){
			.buffer = args->dlights.buffer,
			.offset = args->dlights.offset,
			.range = args->dlights.size,
		};

		g_rtx.desc_values[RayDescBinding_EmissiveKusochki].buffer = (VkDescriptorBufferInfo){
			.buffer = g_ray_model_state.emissive_kusochki_buffer.buffer,
			.offset = 0,
			.range = VK_WHOLE_SIZE,
		};

		g_rtx.desc_values[RayDescBinding_LightClusters].buffer = (VkDescriptorBufferInfo){
			.buffer = g_rtx.light_grid_buffer.buffer,
			.offset = 0,
			.range = VK_WHOLE_SIZE,
		};

		g_rtx.desc_values[RayDescBinding_Textures].image_array = dii_all_textures;

		// TODO: move this to vk_texture.c
		for (int i = 0; i < MAX_TEXTURES; ++i) {
			const vk_texture_t *texture = findTexture(i);
			const qboolean exists = texture->vk.image_view != VK_NULL_HANDLE;
			dii_all_textures[i].sampler = VK_NULL_HANDLE;
			dii_all_textures[i].imageView = exists ? texture->vk.image_view : findTexture(tglob.defaultTexture)->vk.image_view;
			ASSERT(dii_all_textures[i].imageView != VK_NULL_HANDLE);
			dii_all_textures[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		VK_DescriptorsWrite(&g_rtx.descriptors);
	}

		// 4. Barrier for TLAS build and dest image layout transfer
		{
			VkBufferMemoryBarrier bmb[] = { {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				.buffer = g_rtx.accels_buffer.buffer,
				.offset = 0,
				.size = VK_WHOLE_SIZE,
			} };
			VkImageMemoryBarrier image_barrier[] = { {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.image = frame_dst->image,
				.srcAccessMask = 0,
				.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.subresourceRange = (VkImageSubresourceRange) {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
			}} };
			vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
				0, NULL, ARRAYSIZE(bmb), bmb, ARRAYSIZE(image_barrier), image_barrier);
		}

	if (g_rtx.tlas) {
		// 4. dispatch compute
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, g_rtx.pipeline);
		{
			vk_rtx_push_constants_t push_constants = {
				.t = gpGlobals->realtime,
				.bounces = vk_rtx_bounces->value,
				.prev_frame_blend_factor = vk_rtx_prev_frame_blend_factor->value,
			};
			vkCmdPushConstants(cmdbuf, g_rtx.descriptors.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);
		}
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, g_rtx.descriptors.pipeline_layout, 0, 1, g_rtx.descriptors.desc_sets + 0, 0, NULL);
		vkCmdDispatch(cmdbuf, (FRAME_WIDTH + WG_W - 1) / WG_W, (FRAME_HEIGHT + WG_H - 1) / WG_H, 1);
	}

	// Blit RTX frame onto swapchain image
	{
		VkImageMemoryBarrier image_barriers[] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.image = frame_dst->image,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.subresourceRange =
				(VkImageSubresourceRange){
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			},
			{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.image = args->dst.image,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.subresourceRange =
				(VkImageSubresourceRange){
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
		}};
		vkCmdPipelineBarrier(args->cmdbuf,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, ARRAYSIZE(image_barriers), image_barriers);
	}

	{
		VkImageBlit region = {0};
		region.srcOffsets[1].x = FRAME_WIDTH;
		region.srcOffsets[1].y = FRAME_HEIGHT;
		region.srcOffsets[1].z = 1;
		region.dstOffsets[1].x = args->dst.width;
		region.dstOffsets[1].y = args->dst.height;
		region.dstOffsets[1].z = 1;
		region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
		vkCmdBlitImage(args->cmdbuf, frame_dst->image, VK_IMAGE_LAYOUT_GENERAL,
			args->dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
			VK_FILTER_NEAREST);
	}

	{
		VkImageMemoryBarrier image_barriers[] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.image = args->dst.image,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.subresourceRange =
				(VkImageSubresourceRange){
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
		}};
		vkCmdPipelineBarrier(args->cmdbuf,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, NULL, 0, NULL, ARRAYSIZE(image_barriers), image_barriers);
	}
}

static void createLayouts( void ) {
	VkSampler samplers[MAX_TEXTURES];

	g_rtx.descriptors.bindings = g_rtx.desc_bindings;
	g_rtx.descriptors.num_bindings = ARRAYSIZE(g_rtx.desc_bindings);
	g_rtx.descriptors.values = g_rtx.desc_values;
	g_rtx.descriptors.num_sets = 1;
	g_rtx.descriptors.desc_sets = g_rtx.desc_sets;
	g_rtx.descriptors.push_constants = (VkPushConstantRange){
		.offset = 0,
		.size = sizeof(vk_rtx_push_constants_t),
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_DestImage] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_DestImage,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_TLAS] = (VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_TLAS,
		.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_UBOMatrices] = (VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_UBOMatrices,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_Kusochki] = (VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_Kusochki,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_Indices] = (VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_Indices,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_Vertices] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_Vertices,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_UBOLights] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_UBOLights,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_EmissiveKusochki] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_EmissiveKusochki,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_PrevFrame] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_PrevFrame,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_LightClusters] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_LightClusters,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};

	g_rtx.desc_bindings[RayDescBinding_Textures] =	(VkDescriptorSetLayoutBinding){
		.binding = RayDescBinding_Textures,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = MAX_TEXTURES,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.pImmutableSamplers = samplers,
	};

	for (int i = 0; i < ARRAYSIZE(samplers); ++i)
		samplers[i] = vk_core.default_sampler;

	VK_DescriptorsCreate(&g_rtx.descriptors);
}

static void reloadPipeline( void ) {
	g_rtx.reload_pipeline = true;
}

static void freezeModels( void ) {
	g_ray_model_state.freeze_models = !g_ray_model_state.freeze_models;
}

qboolean VK_RayInit( void )
{
	ASSERT(vk_core.rtx);
	// TODO complain and cleanup on failure
	if (!createBuffer("ray accels_buffer", &g_rtx.accels_buffer, MAX_ACCELS_BUFFER,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		))
	{
		return false;
	}
	g_rtx.accels_buffer_addr = getBufferDeviceAddress(g_rtx.accels_buffer.buffer);
	g_rtx.accels_buffer_alloc.size = g_rtx.accels_buffer.size;

	if (!createBuffer("ray scratch_buffer", &g_rtx.scratch_buffer, MAX_SCRATCH_BUFFER,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		)) {
		return false;
	}
	g_rtx.scratch_buffer_addr = getBufferDeviceAddress(g_rtx.scratch_buffer.buffer);

	if (!createBuffer("ray tlas_geom_buffer", &g_rtx.tlas_geom_buffer, sizeof(VkAccelerationStructureInstanceKHR) * MAX_ACCELS,
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		// FIXME complain, handle
		return false;
	}

	if (!createBuffer("ray kusochki_buffer", &g_ray_model_state.kusochki_buffer, sizeof(vk_kusok_data_t) * MAX_KUSOCHKI,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT /* | VK_BUFFER_USAGE_TRANSFER_DST_BIT */,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		// FIXME complain, handle
		return false;
	}
	g_ray_model_state.kusochki_alloc.size = MAX_KUSOCHKI;

	if (!createBuffer("ray emissive_kusochki_buffer", &g_ray_model_state.emissive_kusochki_buffer, sizeof(vk_emissive_kusochki_t),
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT /* | VK_BUFFER_USAGE_TRANSFER_DST_BIT */,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		// FIXME complain, handle
		return false;
	}

	if (!createBuffer("ray light_grid_buffer", &g_rtx.light_grid_buffer, sizeof(vk_ray_shader_light_grid),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT /* | VK_BUFFER_USAGE_TRANSFER_DST_BIT */,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
		// FIXME complain, handle
		return false;
	}

	createLayouts();
	createPipeline();

	for (int i = 0; i < ARRAYSIZE(g_rtx.frames); ++i) {
		g_rtx.frames[i] = VK_ImageCreate(FRAME_WIDTH, FRAME_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	}

	// Start with black previous frame
	{
		const vk_image_t *frame_src = g_rtx.frames + 1;
		const VkImageMemoryBarrier image_barriers[] = { {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.image = frame_src->image,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.subresourceRange = (VkImageSubresourceRange) {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
		}} };

		const VkClearColorValue clear_value = {0};

		const VkCommandBufferBeginInfo beginfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		XVK_CHECK(vkBeginCommandBuffer(vk_core.cb, &beginfo));
		vkCmdPipelineBarrier(vk_core.cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			0, NULL, 0, NULL, ARRAYSIZE(image_barriers), image_barriers);
		vkCmdClearColorImage(vk_core.cb, frame_src->image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &image_barriers->subresourceRange);
		XVK_CHECK(vkEndCommandBuffer(vk_core.cb));

		{
			const VkSubmitInfo subinfo = {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &vk_core.cb,
			};
			XVK_CHECK(vkQueueSubmit(vk_core.queue, 1, &subinfo, VK_NULL_HANDLE));
			XVK_CHECK(vkQueueWaitIdle(vk_core.queue));
		}
	}

	if (vk_core.debug) {
		gEngine.Cmd_AddCommand("vk_rtx_reload", reloadPipeline, "Reload RTX shader");
		gEngine.Cmd_AddCommand("vk_rtx_freeze", freezeModels, "Freeze models, do not update/add/delete models from to-draw list");
	}

	return true;
}

void VK_RayShutdown( void )
{
	ASSERT(vk_core.rtx);

	for (int i = 0; i < ARRAYSIZE(g_rtx.frames); ++i)
		VK_ImageDestroy(g_rtx.frames + i);

	vkDestroyPipeline(vk_core.device, g_rtx.pipeline, NULL);
	VK_DescriptorsDestroy(&g_rtx.descriptors);

	if (g_rtx.tlas != VK_NULL_HANDLE)
		vkDestroyAccelerationStructureKHR(vk_core.device, g_rtx.tlas, NULL);

	for (int i = 0; i < ARRAYSIZE(g_ray_model_state.models_cache); ++i) {
		vk_ray_model_t *model = g_ray_model_state.models_cache + i;
		if (model->as != VK_NULL_HANDLE)
			vkDestroyAccelerationStructureKHR(vk_core.device, model->as, NULL);
		model->as = VK_NULL_HANDLE;
	}

	destroyBuffer(&g_rtx.scratch_buffer);
	destroyBuffer(&g_rtx.accels_buffer);
	destroyBuffer(&g_rtx.tlas_geom_buffer);
	destroyBuffer(&g_ray_model_state.kusochki_buffer);
	destroyBuffer(&g_ray_model_state.emissive_kusochki_buffer);
	destroyBuffer(&g_rtx.light_grid_buffer);
}
