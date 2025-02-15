#include "vk_light.h"
#include "vk_mapents.h"
#include "vk_textures.h"
#include "vk_brush.h"
#include "vk_lightmap.h"
#include "vk_cvar.h"
#include "vk_common.h"
#include "profiler.h"

#include "mod_local.h"
#include "xash3d_mathlib.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h> // isalnum...

#include "camera.h"
#include "pm_defs.h"
#include "pmtrace.h"

#define PROFILER_SCOPES(X) \
	X(finalize , "VK_LightsFrameFinalize"); \
	X(emissive_surface, "VK_LightsAddEmissiveSurface"); \
	X(static_lights, "add static lights"); \
	X(dlights, "add dlights"); \
	//X(canSurfaceLightAffectAABB, "canSurfaceLightAffectAABB"); \

#define SCOPE_DECLARE(scope, name) APROF_SCOPE_DECLARE(scope)
PROFILER_SCOPES(SCOPE_DECLARE)
#undef SCOPE_DECLARE

static struct {
	qboolean enabled;
	char name_filter[256];
} debug_dump_lights;

static void debugDumpLights( void ) {
	debug_dump_lights.enabled = true;
	if (gEngine.Cmd_Argc() > 1) {
		Q_strncpy(debug_dump_lights.name_filter, gEngine.Cmd_Argv(1), sizeof(debug_dump_lights.name_filter));
	} else {
		debug_dump_lights.name_filter[0] = '\0';
	}
}

vk_lights_t g_lights = {0};

void VK_LightsInit( void ) {
	PROFILER_SCOPES(APROF_SCOPE_INIT);

	gEngine.Cmd_AddCommand("vk_lights_dump", debugDumpLights, "Dump all light sources for next frame");
}

static void clusterBitMapShutdown( void );

void VK_LightsShutdown( void ) {
	gEngine.Cmd_RemoveCommand("vk_lights_dump");
	clusterBitMapShutdown();
}

typedef struct {
	int num;
	int leafs[];
} vk_light_leaf_set_t;

typedef struct {
	vk_light_leaf_set_t *potentially_visible_leafs;
} vk_surface_metadata_t;

static struct {
	// Worldmodel surfaces
	int num_surfaces;
	vk_surface_metadata_t *surfaces;

	// Used for accumulating potentially visible leafs
	struct {
		int count;

		// This buffer space is used for two things:
		// As a growing array of u16 leaf indexes (low 16 bits)
		// As a bit field for marking added leafs (highest {31st} bit)
		uint32_t leafs[MAX_MAP_LEAFS];

		byte visbytes[(MAX_MAP_LEAFS+7)/8];
	} accum;

} g_lights_bsp = {0};

static void loadRadData( const model_t *map, const char *fmt, ... ) {
	fs_offset_t size;
	char *data;
	byte *buffer;
	char filename[1024];

	va_list argptr;
	va_start( argptr, fmt );
	vsnprintf( filename, sizeof filename, fmt, argptr );
	va_end( argptr );

	buffer = gEngine.COM_LoadFile( filename, &size, false);

	if (!buffer) {
		gEngine.Con_Printf(S_ERROR "Couldn't load RAD data from file %s, the map will be completely black\n", filename);
		return;
	}

	gEngine.Con_Reportf("Loading RAD data from file %s\n", filename);

	data = (char*)buffer;
	for (;;) {
		string name;
		float r=0, g=0, b=0, scale=0;
		int num;
		char* line_end;

		while (*data != '\0' && isspace(*data)) ++data;
		if (*data == '\0')
			break;

		line_end = Q_strchr(data, '\n');
		if (line_end) *line_end = '\0';

		name[0] = '\0';
		num = sscanf(data, "%s %f %f %f %f", name, &r, &g, &b, &scale);
		gEngine.Con_Printf("raw rad entry (%d): %s %f %f %f %f\n", num, name, r, g, b, scale);
		if (Q_strstr(name, "//") != NULL) {
			num = 0;
		}

		if (num == 2) {
			r = g = b;
		} else if (num == 5) {
			scale /= 255.f;
			r *= scale;
			g *= scale;
			b *= scale;
		} else if (num == 4) {
			// Ok, rgb only, no scaling
		} else {
			gEngine.Con_Printf( "skipping rad entry %s\n", name[0] ? name : "(empty)" );
			num = 0;
		}

		if (num != 0) {
			gEngine.Con_Printf("rad entry (%d): %s %f %f %f (%f)\n", num, name, r, g, b, scale);

			{
				const char *wad_name = NULL;
				char *texture_name = Q_strchr(name, '/');
				string texname;
				int tex_id;
				const qboolean enabled = (r != 0 || g != 0 || b != 0);

				if (!texture_name) {
					texture_name = name;
				} else {
					// name is now just a wad name
					texture_name[0] = '\0';
					wad_name = name;

					texture_name += 1;
				}

				// FIXME replace this with findTexturesNamedLike from vk_materials.c

				// Try bsp texture first
				tex_id = XVK_TextureLookupF("#%s:%s.mip", map->name, texture_name);

				// Try wad texture if bsp is not there
				if (!tex_id && wad_name) {
					tex_id = XVK_TextureLookupF("%s.wad/%s.mip", wad_name, texture_name);
				}

				if (!tex_id) {
					const char *wad = g_map_entities.wadlist;
					for (; *wad;) {
						const char *const wad_end = Q_strchr(wad, ';');
						tex_id = XVK_TextureLookupF("%.*s/%s.mip", wad_end - wad, wad, texture_name);
						if (tex_id)
							break;
						wad = wad_end + 1;
					}
				}

				if (tex_id) {
					vk_emissive_texture_t *const etex = g_lights.map.emissive_textures + tex_id;
					ASSERT(tex_id < MAX_TEXTURES);

					etex->emissive[0] = r;
					etex->emissive[1] = g;
					etex->emissive[2] = b;
					etex->set = enabled;

					// See DIRECT_SCALE in qrad/lightmap.c
					VectorScale(etex->emissive, 0.1f, etex->emissive);

					if (!enabled)
						gEngine.Con_Reportf("rad entry %s disabled due to zero intensity\n", name);
				}
			}
		}

		if (!line_end)
			break;

		data = line_end + 1;
	}

	Mem_Free(buffer);
}

static void leafAccumPrepare( void ) {
	memset(&g_lights_bsp.accum, 0, sizeof(g_lights_bsp.accum));
}

#define LEAF_ADDED_BIT 0x8000000ul

static qboolean leafAccumAdd( uint16_t leaf_index ) {
	// Check whether this leaf was already added
	if (g_lights_bsp.accum.leafs[leaf_index] & LEAF_ADDED_BIT)
		return false;

	g_lights_bsp.accum.leafs[leaf_index] |= LEAF_ADDED_BIT;

	g_lights_bsp.accum.leafs[g_lights_bsp.accum.count++] |= leaf_index;
	return true;
}

static void leafAccumFinalize( void ) {
	for (int i = 0; i < g_lights_bsp.accum.count; ++i)
		g_lights_bsp.accum.leafs[i] &= 0xffffu;
}

static int leafAccumAddPotentiallyVisibleFromLeaf(const model_t *const map, const mleaf_t *leaf, qboolean print_debug) {
	int pvs_leaf_index = 0;
	int leafs_added = 0;
	const byte *pvs = leaf->compressed_vis;
	for (;pvs_leaf_index < map->numleafs; ++pvs) {
		uint8_t bits = pvs[0];

		// PVS is RLE encoded
		if (bits == 0) {
			const int skip = pvs[1];
			pvs_leaf_index += skip * 8;
			++pvs;
			continue;
		}

		for (int k = 0; k < 8; ++k, ++pvs_leaf_index, bits >>= 1) {
			if ((bits&1) == 0)
				continue;

			if (leafAccumAdd( pvs_leaf_index + 1 )) {
				leafs_added++;
				if (print_debug)
					gEngine.Con_Reportf(" .%d", pvs_leaf_index + 1);
			}
		}
	}

	return leafs_added;
}

vk_light_leaf_set_t *getMapLeafsAffectedByMapSurface( const msurface_t *surf ) {
	const model_t *const map = gEngine.pfnGetModelByIndex( 1 );
	const int surf_index = surf - map->surfaces;
	vk_surface_metadata_t * const smeta = g_lights_bsp.surfaces + surf_index;
	const qboolean verbose_debug = false;
	ASSERT(surf_index >= 0);
	ASSERT(surf_index < g_lights_bsp.num_surfaces);

	// Check if PVL hasn't been collected yet
	if (!smeta->potentially_visible_leafs) {
		int leafs_direct = 0, leafs_pvs = 0;
		leafAccumPrepare();

		// Enumerate all the map leafs and pick ones that have this surface referenced
		if (verbose_debug)
			gEngine.Con_Reportf("Collecting visible leafs for surface %d:", surf_index);
		for (int i = 1; i <= map->numleafs; ++i) {
			const mleaf_t *leaf = map->leafs + i;
			//if (verbose_debug) gEngine.Con_Reportf("    leaf %d(c%d)/%d:", i, leaf->cluster, map->numleafs);
			for (int j = 0; j < leaf->nummarksurfaces; ++j) {
				const msurface_t *leaf_surf = leaf->firstmarksurface[j];
				if (leaf_surf != surf) {
					/* if (verbose_debug) { */
					/* 	const int leaf_surf_index = leaf_surf - map->surfaces; */
					/* 	gEngine.Con_Reportf(" !%d", leaf_surf_index); */
					/* } */
					continue;
				}

				// FIXME split direct leafs marking from pvs propagation
				leafs_direct++;
				if (leafAccumAdd( i )) {
					if (verbose_debug) gEngine.Con_Reportf(" %d", i);
				} else {
					// This leaf was already added earlier by PVS
					// but it really should be counted as direct
					--leafs_pvs;
				}

				// Get all PVS leafs
				leafs_pvs += leafAccumAddPotentiallyVisibleFromLeaf(map, leaf, verbose_debug);
			}

			//if (verbose_debug) gEngine.Con_Reportf("\n");
		}
		if (verbose_debug)
			gEngine.Con_Reportf(" (sum=%d, direct=%d, pvs=%d)\n", g_lights_bsp.accum.count, leafs_direct, leafs_pvs);

		leafAccumFinalize();

		smeta->potentially_visible_leafs = (vk_light_leaf_set_t*)Mem_Malloc(vk_core.pool, sizeof(smeta->potentially_visible_leafs) + sizeof(int) * g_lights_bsp.accum.count);
		smeta->potentially_visible_leafs->num = g_lights_bsp.accum.count;

		for (int i = 0; i < g_lights_bsp.accum.count; ++i) {
			smeta->potentially_visible_leafs->leafs[i] = g_lights_bsp.accum.leafs[i];
		}
	}

	return smeta->potentially_visible_leafs;
}


static struct {
#define CLUSTERS_BIT_MAP_SIZE_UINT ((g_lights.map.grid_cells + 31) / 32)
	uint32_t *clusters_bit_map;
} g_lights_tmp;

static void clusterBitMapClear( void ) {
	memset(g_lights_tmp.clusters_bit_map, 0, CLUSTERS_BIT_MAP_SIZE_UINT * sizeof(uint32_t));
}

// Returns true if wasn't set
static qboolean clusterBitMapCheckOrSet( int cell_index ) {
	uint32_t *const bits = g_lights_tmp.clusters_bit_map + (cell_index / 32);
	const uint32_t bit = 1u << (cell_index % 32);

	if ((*bits) & bit)
		return false;

	(*bits) |= bit;
	return true;
}

static void clusterBitMapInit( void ) {
	ASSERT(!g_lights_tmp.clusters_bit_map);

	g_lights_tmp.clusters_bit_map = Mem_Malloc(vk_core.pool, CLUSTERS_BIT_MAP_SIZE_UINT * sizeof(uint32_t));
	clusterBitMapClear();
}

static void clusterBitMapShutdown( void ) {
	if (g_lights_tmp.clusters_bit_map)
		Mem_Free(g_lights_tmp.clusters_bit_map);
	g_lights_tmp.clusters_bit_map = NULL;
}

int R_LightCellIndex( const int light_cell[3] ) {
	if (light_cell[0] < 0 || light_cell[1] < 0 || light_cell[2] < 0
		|| (light_cell[0] >= g_lights.map.grid_size[0])
		|| (light_cell[1] >= g_lights.map.grid_size[1])
		|| (light_cell[2] >= g_lights.map.grid_size[2]))
		return -1;

	return light_cell[0] + light_cell[1] * g_lights.map.grid_size[0] + light_cell[2] * g_lights.map.grid_size[0] * g_lights.map.grid_size[1];
}

vk_light_leaf_set_t *getMapLeafsAffectedByMovingSurface( const msurface_t *surf, const matrix3x4 *transform_row ) {
	const model_t *const map = gEngine.pfnGetModelByIndex( 1 );
	const mextrasurf_t *const extra = surf->info;

	// This is a very conservative way to construct a bounding sphere. It's not great.
	const vec3_t bbox_center = {
		(extra->mins[0] + extra->maxs[0]) / 2.f,
		(extra->mins[1] + extra->maxs[1]) / 2.f,
		(extra->mins[2] + extra->maxs[2]) / 2.f,
	};

	const vec3_t bbox_size = {
		extra->maxs[0] - extra->mins[0],
		extra->maxs[1] - extra->mins[1],
		extra->maxs[2] - extra->mins[2],
	};

	int leafs_direct = 0, leafs_pvs = 0;

	const float radius = .5f * VectorLength(bbox_size);
	vec3_t origin;

	Matrix3x4_VectorTransform(*transform_row, bbox_center, origin);

	if (debug_dump_lights.enabled) {
		gEngine.Con_Reportf("\torigin = %f, %f, %f, R = %f\n",
			origin[0], origin[1], origin[2], radius
		);
	}

	leafAccumPrepare();

	// TODO it's possible to somehow more efficiently traverse the bsp and collect only the affected leafs
	// (origin + radius will accidentally touch leafs that are really should not be affected)
	gEngine.R_FatPVS(origin, radius, g_lights_bsp.accum.visbytes, /*merge*/ false, /*fullvis*/ false);
	if (debug_dump_lights.enabled)
		gEngine.Con_Reportf("Collecting visible leafs for moving surface %p: %f,%f,%f %f: ", surf,
			origin[0], origin[1], origin[2], radius);

	for (int i = 0; i <= map->numleafs; ++i) {
		const mleaf_t *leaf = map->leafs + i;
		if( !CHECKVISBIT( g_lights_bsp.accum.visbytes, i ))
			continue;

		leafs_direct++;

		if (leafAccumAdd( i + 1 )) {
			if (debug_dump_lights.enabled)
				gEngine.Con_Reportf(" %d", i + 1);
		} else {
			// This leaf was already added earlier by PVS
			// but it really should be counted as direct
			leafs_pvs--;
		}
	}

	if (debug_dump_lights.enabled)
		gEngine.Con_Reportf(" (sum=%d, direct=%d, pvs=%d)\n", g_lights_bsp.accum.count, leafs_direct, leafs_pvs);

	leafAccumFinalize();

	// ...... oh no
	return (vk_light_leaf_set_t*)&g_lights_bsp.accum.count;
}

static void prepareSurfacesLeafVisibilityCache( void ) {
	const model_t	*map = gEngine.pfnGetModelByIndex( 1 );
	if (g_lights_bsp.surfaces != NULL) {
		for (int i = 0; i < g_lights_bsp.num_surfaces; ++i) {
			vk_surface_metadata_t *smeta = g_lights_bsp.surfaces + i;
			if (smeta->potentially_visible_leafs)
				Mem_Free(smeta->potentially_visible_leafs);
		}
		Mem_Free(g_lights_bsp.surfaces);
	}

	g_lights_bsp.num_surfaces = map->numsurfaces;
	g_lights_bsp.surfaces = Mem_Malloc(vk_core.pool, g_lights_bsp.num_surfaces * sizeof(vk_surface_metadata_t));
	for (int i = 0; i < g_lights_bsp.num_surfaces; ++i)
		g_lights_bsp.surfaces[i].potentially_visible_leafs = NULL;
}

void VK_LightsNewMap( void ) {
	const model_t	*map = gEngine.pfnGetModelByIndex( 1 );

	// 1. Determine map bounding box (and optimal grid size?)
		// map->mins, maxs
	vec3_t map_size, min_cell, max_cell;
	VectorSubtract(map->maxs, map->mins, map_size);

	VectorDivide(map->mins, LIGHT_GRID_CELL_SIZE, min_cell);
	min_cell[0] = floorf(min_cell[0]);
	min_cell[1] = floorf(min_cell[1]);
	min_cell[2] = floorf(min_cell[2]);
	VectorCopy(min_cell, g_lights.map.grid_min_cell);

	VectorDivide(map->maxs, LIGHT_GRID_CELL_SIZE, max_cell);
	max_cell[0] = ceilf(max_cell[0]);
	max_cell[1] = ceilf(max_cell[1]);
	max_cell[2] = ceilf(max_cell[2]);

	VectorSubtract(max_cell, min_cell, g_lights.map.grid_size);
	g_lights.map.grid_cells = g_lights.map.grid_size[0] * g_lights.map.grid_size[1] * g_lights.map.grid_size[2];

	ASSERT(g_lights.map.grid_cells < MAX_LIGHT_CLUSTERS);

	gEngine.Con_Reportf("Map mins:(%f, %f, %f), maxs:(%f, %f, %f), size:(%f, %f, %f), min_cell:(%f, %f, %f) cells:(%d, %d, %d); total: %d\n",
		map->mins[0], map->mins[1], map->mins[2],
		map->maxs[0], map->maxs[1], map->maxs[2],
		map_size[0], map_size[1], map_size[2],
		min_cell[0], min_cell[1], min_cell[2],
		g_lights.map.grid_size[0],
		g_lights.map.grid_size[1],
		g_lights.map.grid_size[2],
		g_lights.map.grid_cells
	);

	clusterBitMapShutdown();
	clusterBitMapInit();

	prepareSurfacesLeafVisibilityCache();
}

void VK_LightsFrameInit( void ) {
	g_lights.num_emissive_surfaces = g_lights.num_static.emissive_surfaces;
	g_lights.num_point_lights = g_lights.num_static.point_lights;

	for (int i = 0; i < g_lights.map.grid_cells; ++i) {
		vk_lights_cell_t *const cell = g_lights.cells + i;
		cell->num_point_lights = cell->num_static.point_lights;
		cell->num_emissive_surfaces = cell->num_static.emissive_surfaces;
	}
}

static qboolean addSurfaceLightToCell( int cell_index, int emissive_surface_index ) {
	vk_lights_cell_t *const cluster = g_lights.cells + cell_index;

	if (cluster->num_emissive_surfaces == MAX_VISIBLE_SURFACE_LIGHTS) {
		return false;
	}

	if (debug_dump_lights.enabled) {
		gEngine.Con_Reportf("    adding surface light %d to cell %d (count=%d)\n", emissive_surface_index, cell_index, cluster->num_emissive_surfaces+1);
	}

	cluster->emissive_surfaces[cluster->num_emissive_surfaces++] = emissive_surface_index;
	return true;
}

static qboolean addLightToCell( int cell_index, int light_index ) {
	vk_lights_cell_t *const cluster = g_lights.cells + cell_index;

	if (cluster->num_point_lights == MAX_VISIBLE_POINT_LIGHTS)
		return false;

	if (debug_dump_lights.enabled) {
		gEngine.Con_Reportf("    adding point light %d to cell %d (count=%d)\n", light_index, cell_index, cluster->num_point_lights+1);
	}

	cluster->point_lights[cluster->num_point_lights++] = light_index;
	return true;
}

static qboolean canSurfaceLightAffectAABB(const model_t *mod, const msurface_t *surf, const vec3_t emissive, const float minmax[6]) {
	//APROF_SCOPE_BEGIN_EARLY(canSurfaceLightAffectAABB); // DO NOT DO THIS. We have like 600k of these calls per frame :feelsbadman:
	qboolean retval = true;
	// FIXME transform surface
	// this here only works for static map model

	// Use bbox center for normal culling estimation
	const vec3_t bbox_center = {
		(minmax[0] + minmax[3]) / 2.f,
		(minmax[1] + minmax[4]) / 2.f,
		(minmax[2] + minmax[5]) / 2.f,
	};

	float bbox_plane_dist = PlaneDiff(bbox_center, surf->plane);
	if( FBitSet( surf->flags, SURF_PLANEBACK ))
		bbox_plane_dist = -bbox_plane_dist;

	if (bbox_plane_dist < 0.f) {
		// Fast conservative estimate by max distance from bbox center
		// TODO is enumerating all points or finding a closest one is better/faster?
		const float size_x = minmax[0] - minmax[3];
		const float size_y = minmax[1] - minmax[4];
		const float size_z = minmax[2] - minmax[5];
		const float plane_dist_guard_sqr = (size_x * size_x + size_y * size_y + size_z * size_z) * .25f;

		// Check whether this bbox is completely behind the surface
		if (bbox_plane_dist*bbox_plane_dist > plane_dist_guard_sqr)
			retval = false;
	}

	//APROF_SCOPE_END(canSurfaceLightAffectAABB);

	return retval;
}

void VK_LightsAddEmissiveSurface( const struct vk_render_geometry_s *geom, const matrix3x4 *transform_row, qboolean static_map ) {
	APROF_SCOPE_BEGIN_EARLY(emissive_surface);
	const model_t* const world = gEngine.pfnGetModelByIndex( 1 );
	const int texture_num = geom->texture; // Animated texture
	vk_emissive_surface_t *retval = NULL;
	vec3_t emissive_color = {0};

	ASSERT(texture_num >= 0);
	ASSERT(texture_num < MAX_TEXTURES);

	// Only brush model surfaces are supported to be emissive. This is not _strictly_ necessary, but is a bit simpler.
	if (!geom->surf)
		goto fin; // TODO break? no surface means that model is not brush

	// Find out whether this surface is emissive
	{
		const int surface_index = geom->surf - world->surfaces;
		const xvk_patch_surface_t *psurf = g_map_entities.patch.surfaces ? g_map_entities.patch.surfaces + surface_index : NULL;

		ASSERT(surface_index >= 0);
		ASSERT(surface_index < world->numsurfaces);

		if (psurf && psurf->flags & Patch_Surface_Emissive) {
			VectorCopy(psurf->emissive, emissive_color);
		} else if (geom->material == kXVkMaterialEmissive) {
			VectorCopy(geom->emissive, emissive_color);
		} else if (g_lights.map.emissive_textures[texture_num].set) {
			VectorCopy(g_lights.map.emissive_textures[texture_num].emissive, emissive_color);
		} else {
			goto fin;
		}

		if (emissive_color[0] == 0 && emissive_color[1] == 0 && emissive_color[2] == 0) {
			if (static_map) {
				gEngine.Con_Reportf("Surface %d got zero emissive color, not adding as a light source\n", surface_index);
			}
			goto fin;
		}
	}

	if (g_lights.num_emissive_surfaces >= 256)
		goto fin;

	if (debug_dump_lights.enabled) {
		const vk_texture_t *tex = findTexture(texture_num);
		ASSERT(tex);
		gEngine.Con_Reportf("surface light %d: %s (%f %f %f)\n", g_lights.num_emissive_surfaces, tex->name,
			emissive_color[0],
			emissive_color[1],
			emissive_color[2]);
	}

	{
		const vk_light_leaf_set_t *const leafs = static_map
			? getMapLeafsAffectedByMapSurface( geom->surf )
			: getMapLeafsAffectedByMovingSurface( geom->surf, transform_row );
		vk_emissive_surface_t *esurf = g_lights.emissive_surfaces + g_lights.num_emissive_surfaces;

		// Insert into emissive surfaces
		esurf->kusok_index = geom->kusok_index;
		VectorCopy(emissive_color, esurf->emissive);
		Matrix3x4_Copy(esurf->transform, *transform_row);

		clusterBitMapClear();

		// Iterate through each visible/potentially affected leaf to get a range of grid cells
		for (int i = 0; i < leafs->num; ++i) {
			const mleaf_t *const leaf = world->leafs + leafs->leafs[i];

			const int min_x = floorf(leaf->minmaxs[0] / LIGHT_GRID_CELL_SIZE);
			const int min_y = floorf(leaf->minmaxs[1] / LIGHT_GRID_CELL_SIZE);
			const int min_z = floorf(leaf->minmaxs[2] / LIGHT_GRID_CELL_SIZE);

			const int max_x = floorf(leaf->minmaxs[3] / LIGHT_GRID_CELL_SIZE) + 1;
			const int max_y = floorf(leaf->minmaxs[4] / LIGHT_GRID_CELL_SIZE) + 1;
			const int max_z = floorf(leaf->minmaxs[5] / LIGHT_GRID_CELL_SIZE) + 1;

			const qboolean not_visible = static_map && !canSurfaceLightAffectAABB(world, geom->surf, esurf->emissive, leaf->minmaxs);

			if (debug_dump_lights.enabled) {
				gEngine.Con_Reportf("  adding leaf %d (%d of %d) min=(%d, %d, %d), max=(%d, %d, %d) total=%d\n",
					leaf->cluster, i, leafs->num,
					min_x, min_y, min_z,
					max_x, max_y, max_z,
					(max_x - min_x) * (max_y - min_y) * (max_z - min_z)
				);
			}

			if (not_visible)
				continue;

			for (int x = min_x; x < max_x; ++x)
			for (int y = min_y; y < max_y; ++y)
			for (int z = min_z; z < max_z; ++z) {
				const int cell[3] = {
					x - g_lights.map.grid_min_cell[0],
					y - g_lights.map.grid_min_cell[1],
					z - g_lights.map.grid_min_cell[2]
				};

				const int cell_index = R_LightCellIndex( cell );
				if (cell_index < 0)
					continue;

				if (clusterBitMapCheckOrSet( cell_index )) {
					const float minmaxs[6] = {
						x * LIGHT_GRID_CELL_SIZE,
						y * LIGHT_GRID_CELL_SIZE,
						z * LIGHT_GRID_CELL_SIZE,
						(x+1) * LIGHT_GRID_CELL_SIZE,
						(y+1) * LIGHT_GRID_CELL_SIZE,
						(z+1) * LIGHT_GRID_CELL_SIZE,
					};

					if (static_map && !canSurfaceLightAffectAABB(world, geom->surf, esurf->emissive, minmaxs))
						continue;

					if (!addSurfaceLightToCell(cell_index, g_lights.num_emissive_surfaces)) {
						ERROR_THROTTLED(10, "Cluster %d,%d,%d(%d) ran out of emissive surfaces slots",
							cell[0], cell[1],  cell[2], cell_index);
					}
				}
			}
		}

		++g_lights.num_emissive_surfaces;
		retval = esurf;
	}

fin:
	APROF_SCOPE_END(emissive_surface);
}

static void addLightIndexToleaf( const mleaf_t *leaf, int index ) {
	const int min_x = floorf(leaf->minmaxs[0] / LIGHT_GRID_CELL_SIZE);
	const int min_y = floorf(leaf->minmaxs[1] / LIGHT_GRID_CELL_SIZE);
	const int min_z = floorf(leaf->minmaxs[2] / LIGHT_GRID_CELL_SIZE);

	const int max_x = ceilf(leaf->minmaxs[3] / LIGHT_GRID_CELL_SIZE);
	const int max_y = ceilf(leaf->minmaxs[4] / LIGHT_GRID_CELL_SIZE);
	const int max_z = ceilf(leaf->minmaxs[5] / LIGHT_GRID_CELL_SIZE);

	if (debug_dump_lights.enabled) {
		gEngine.Con_Reportf("  adding leaf %d min=(%d, %d, %d), max=(%d, %d, %d) total=%d\n",
			leaf->cluster,
			min_x, min_y, min_z,
			max_x, max_y, max_z,
			(max_x - min_x) * (max_y - min_y) * (max_z - min_z)
		);
	}

	for (int x = min_x; x < max_x; ++x)
	for (int y = min_y; y < max_y; ++y)
	for (int z = min_z; z < max_z; ++z) {
		const int cell[3] = {
			x - g_lights.map.grid_min_cell[0],
			y - g_lights.map.grid_min_cell[1],
			z - g_lights.map.grid_min_cell[2]
		};

		const int cell_index = R_LightCellIndex( cell );
		if (cell_index < 0)
			continue;

		if (clusterBitMapCheckOrSet( cell_index )) {
			if (!addLightToCell(cell_index, index)) {
				ERROR_THROTTLED(10, "Cluster %d,%d,%d(%d) ran out of light slots",
					cell[0], cell[1],  cell[2], cell_index);
			}
		}
	}
}

static void addPointLightToClusters( int index ) {
	vk_point_light_t *const light = g_lights.point_lights + index;
	const model_t* const world = gEngine.pfnGetModelByIndex( 1 );
	const mleaf_t* leaf = gEngine.Mod_PointInLeaf(light->origin, world->nodes);
	const vk_light_leaf_set_t *const leafs = (vk_light_leaf_set_t*)&g_lights_bsp.accum.count;

	leafAccumPrepare();
	leafAccumAddPotentiallyVisibleFromLeaf( world, leaf, false);
	leafAccumFinalize();

	clusterBitMapClear();
	for (int i = 0; i < leafs->num; ++i) {
		const mleaf_t *const leaf = world->leafs + leafs->leafs[i];
		addLightIndexToleaf( leaf, index );
	}
}

static void addPointLightToAllClusters( int index ) {
	const model_t* const world = gEngine.pfnGetModelByIndex( 1 );

	clusterBitMapClear();
	for (int i = 1; i <= world->numleafs; ++i) {
		const mleaf_t *const leaf = world->leafs + i;
		addLightIndexToleaf( leaf, index );
	}
}

static int addPointLight( const vec3_t origin, const vec3_t color, float radius, int lightstyle, float hack_attenuation ) {
	const int index = g_lights.num_point_lights;
	vk_point_light_t *const plight = g_lights.point_lights + index;

	if (g_lights.num_point_lights >= MAX_POINT_LIGHTS) {
		ERROR_THROTTLED(10, "Too many lights, MAX_POINT_LIGHTS=%d", MAX_POINT_LIGHTS);
		return -1;
	}

	if (debug_dump_lights.enabled) {
		gEngine.Con_Printf("point light %d: origin=(%f %f %f) R=%f color=(%f %f %f)\n", index,
			origin[0], origin[1], origin[2], radius,
			color[0], color[1], color[2]);
	}

	*plight = (vk_point_light_t){0};
	VectorCopy(origin, plight->origin);
	plight->radius = radius;

	VectorScale(color, hack_attenuation, plight->base_color);
	VectorCopy(plight->base_color, plight->color);
	plight->lightstyle = lightstyle;

	// Omnidirectional light
	plight->stopdot = plight->stopdot2 = -1.f;
	VectorSet(plight->dir, 0, 0, 0);

	addPointLightToClusters( index );
	g_lights.num_point_lights++;
	return index;
}

static int addSpotLight( const vk_light_entity_t *le, float radius, int lightstyle, float hack_attenuation, qboolean all_clusters ) {
	const int index = g_lights.num_point_lights;
	vk_point_light_t *const plight = g_lights.point_lights + index;

	if (g_lights.num_point_lights >= MAX_POINT_LIGHTS) {
		ERROR_THROTTLED(10, "Too many lights, MAX_POINT_LIGHTS=%d", MAX_POINT_LIGHTS);
		return -1;
	}

	if (debug_dump_lights.enabled) {
		gEngine.Con_Printf("%s light %d: origin=(%f %f %f) color=(%f %f %f) dir=(%f %f %f)\n",
			le->type == LightTypeEnvironment ? "environment" : "spot",
			index,
			le->origin[0], le->origin[1], le->origin[2],
			le->color[0], le->color[1], le->color[2],
			le->dir[0], le->dir[1], le->dir[2]);
	}

	*plight = (vk_point_light_t){0};
	VectorCopy(le->origin, plight->origin);
	plight->radius = radius;

	VectorScale(le->color, hack_attenuation, plight->base_color);
	VectorCopy(plight->base_color, plight->color);
	plight->lightstyle = lightstyle;

	VectorCopy(le->dir, plight->dir);
	plight->stopdot = le->stopdot;
	plight->stopdot2 = le->stopdot2;

	if (le->type == LightTypeEnvironment)
		plight->flags = LightFlag_Environment;

	if (all_clusters)
		addPointLightToAllClusters( index );
	else
		addPointLightToClusters( index );

	g_lights.num_point_lights++;
	return index;
}

void R_LightAddFlashlight(const struct cl_entity_s *ent, qboolean local_player ) {
	// parameters
	const float hack_attenuation = 1.0;
	float radius = 1.0;
	// TODO: better tune it
	const float _cone = 1.0;
	const float _cone2 = 32.5;
	const vec3_t light_color = {255, 255, 210};
	float light_intensity = 400;

	vec3_t color;
	vec3_t origin;
	vec3_t angles;
	vk_light_entity_t le;

	float thirdperson_offset = 25;
	vec3_t forward, view_ofs;
	vec3_t vecSrc, vecEnd;
	pmtrace_t *trace;
	if( local_player )
	{
		// local player case
		// position
		if (gEngine.EngineGetParm(PARM_THIRDPERSON, 0)) { // thirdperson
			AngleVectors( g_camera.viewangles, forward, NULL, NULL );
			view_ofs[0] = view_ofs[1] = 0.0f;
			if( ent->curstate.usehull == 1 ) {
				view_ofs[2] = 12.0f; // VEC_DUCK_VIEW;
			} else {
				view_ofs[2] = 28.0f; // DEFAULT_VIEWHEIGHT
			}
			VectorAdd( ent->origin, view_ofs, vecSrc );
			VectorMA( vecSrc, thirdperson_offset, forward, vecEnd );
			trace = gEngine.EV_VisTraceLine( vecSrc, vecEnd, PM_STUDIO_BOX );
			VectorCopy( trace->endpos, origin );
			VectorCopy( forward, le.dir);
		} else { // firstperson
			// based on https://github.com/SNMetamorph/PrimeXT/blob/0869b1abbddd13c1229769d8cd71941610be0bf3/client/flashlight.cpp#L35
			// TODO: tune it
			origin[0] = g_camera.vieworg[0] + (g_camera.vright[0] * (-6.0f)) + (g_camera.vforward[0] * 14.0); // forward-back
			origin[1] = g_camera.vieworg[1] + (g_camera.vright[1] * (-6.0f)) + (g_camera.vforward[1] * 14.0); // left-right
			origin[2] = g_camera.vieworg[2] + (g_camera.vright[2] * (-6.0f)) + (g_camera.vforward[2] * 14.0); // up-down
			origin[2] += 2.0f;
			VectorCopy(g_camera.vforward, le.dir);
		}
	}
	else // non-local player case
	{
		thirdperson_offset = 10;
		radius = 10;
		light_intensity = 60;

		VectorCopy( ent->angles, angles );
		// NOTE: pitch divided by 3.0 twice. So we need apply 3^2 = 9
		angles[PITCH] = ent->curstate.angles[PITCH] * 9.0f;
		angles[YAW] = ent->angles[YAW];
		angles[ROLL] = 0.0f; // roll not used

		AngleVectors( angles, angles, NULL, NULL );
		view_ofs[0] = view_ofs[1] = 0.0f;
		if( ent->curstate.usehull == 1 ) {
			view_ofs[2] = 12.0f; // VEC_DUCK_VIEW;
		} else {
			view_ofs[2] = 28.0f; // DEFAULT_VIEWHEIGHT
		}
		VectorAdd( ent->origin, view_ofs, vecSrc );
		VectorMA( vecSrc, thirdperson_offset, angles, vecEnd );
		trace = gEngine.EV_VisTraceLine( vecSrc, vecEnd, PM_STUDIO_BOX );
		VectorCopy( trace->endpos, origin );
		VectorCopy( angles, le.dir );
	}

	VectorCopy(origin, le.origin);

	// prepare colors by parseEntPropRgbav
	VectorScale(light_color, light_intensity / 255.0f, color);

	// convert colors by weirdGoldsrcLightScaling
	float l1 = Q_max(color[0], Q_max(color[1], color[2]));
	l1 = l1 * l1 / 10;
	VectorScale(color, l1, le.color);

	// convert stopdots by parseStopDot
	le.stopdot = cosf(_cone * M_PI / 180.f);
	le.stopdot2 = cosf(_cone2 * M_PI / 180.f);

	/*
	gEngine.Con_Printf("flashlight: origin=(%f %f %f) color=(%f %f %f) dir=(%f %f %f)\n",
		le.origin[0], le.origin[1], le.origin[2],
		le.color[0], le.color[1], le.color[2],
		le.dir[0], le.dir[1], le.dir[2]);
	*/

	addSpotLight(&le, radius, 0, hack_attenuation, false);
}

static float sphereSolidAngleFromDistDiv2Pi(float r, float d) {
	return 1. - sqrt(d*d - r*r)/d;
}

static void addDlight( const dlight_t *dlight ) {
	cl_entity_t	*entPlayer;
	
	const float k_light_radius = 2.f;
	const float k_threshold = 1.f;

	float max_comp;
	vec3_t color;
	int index;
	float scaler;

	max_comp = Q_max(dlight->color.r, Q_max(dlight->color.g, dlight->color.b));
	if (max_comp < k_threshold || dlight->radius <= k_light_radius)
		return;

	scaler = k_threshold / (max_comp * sphereSolidAngleFromDistDiv2Pi(k_light_radius, dlight->radius));

	VectorSet(
		color,
		dlight->color.r * scaler,
		dlight->color.g * scaler,
		dlight->color.b * scaler);

	index = addPointLight(dlight->origin, color, k_light_radius, -1, 1.f);
	if (index < 0)
		return;
}

static void processStaticPointLights( void ) {
	APROF_SCOPE_BEGIN_EARLY(static_lights);
	const model_t* const world = gEngine.pfnGetModelByIndex( 1 );
	ASSERT(world);

	g_lights.num_point_lights = 0;
	for (int i = 0; i < g_map_entities.num_lights; ++i) {
		const vk_light_entity_t *le = g_map_entities.lights + i;
		const float default_radius = 2.f;
		const float hack_attenuation = 1.f;
		const float hack_attenuation_spot = 1.f;
		const float radius = le->radius > 0.f ? le->radius : default_radius;
		int index;

		switch (le->type) {
			case LightTypePoint:
				index = addPointLight(le->origin, le->color, radius, le->style, hack_attenuation);
				break;

			case LightTypeSpot:
				index = addSpotLight(le, radius, le->style, hack_attenuation_spot, i == g_map_entities.single_environment_index);
				break;
			case LightTypeEnvironment:
				index = addSpotLight(le, radius, le->style, hack_attenuation_spot, i == g_map_entities.single_environment_index);
				break;

			default:
				ASSERT(!"Unexpected light type");
				continue;
		}

		if (index < 0)
			break;
	}
	APROF_SCOPE_END(static_lights);
}

void VK_LightsLoadMapStaticLights( void ) {
	const model_t *map = gEngine.pfnGetModelByIndex( 1 );

	//debug_dump_lights.enabled = true;

	// Clear static lights counts
	{
		g_lights.num_emissive_surfaces = g_lights.num_static.emissive_surfaces = 0;
		g_lights.num_point_lights = g_lights.num_static.point_lights = 0;

		for (int i = 0; i < g_lights.map.grid_cells; ++i) {
			vk_lights_cell_t *const cell = g_lights.cells + i;
			cell->num_point_lights = cell->num_static.point_lights = 0;
			cell->num_emissive_surfaces = cell->num_static.emissive_surfaces = 0;
		}
	}

	processStaticPointLights();

	// Load RAD data based on map name
	memset(g_lights.map.emissive_textures, 0, sizeof(g_lights.map.emissive_textures));
	loadRadData( map, "maps/lights.rad" );

	{
		int name_len = Q_strlen(map->name);

		// Strip ".bsp" suffix
		if (name_len > 4 && 0 == Q_stricmp(map->name + name_len - 4, ".bsp"))
			name_len -= 4;

		loadRadData( map, "%.*s.rad", name_len, map->name );
	}

	// Load static map model
	{
		matrix3x4 xform;
		const vk_brush_model_t *const bmodel = map->cache.data;
		ASSERT(bmodel);
		Matrix3x4_LoadIdentity(xform);

		for (int i = 0; i < bmodel->render_model.num_geometries; ++i) {
			const vk_render_geometry_t *geom = bmodel->render_model.geometries + i;
			VK_LightsAddEmissiveSurface( geom, &xform, true );
				// TODO how to differentiate between this and non-emissive gEngine.Con_Printf(S_ERROR "Ran out of surface light slots, geom %d of %d\n", i, bmodel->render_model.num_geometries);
		}
	}

	// Fix static counts
	{
		g_lights.num_static.emissive_surfaces = g_lights.num_emissive_surfaces;
		g_lights.num_static.point_lights = g_lights.num_point_lights;

		for (int i = 0; i < g_lights.map.grid_cells; ++i) {
			vk_lights_cell_t *const cell = g_lights.cells + i;
			cell->num_static.point_lights = cell->num_point_lights;
			cell->num_static.emissive_surfaces = cell->num_emissive_surfaces;
		}
	}
}

void XVK_GetEmissiveForTexture( vec3_t out, int texture_id ) {
	ASSERT(texture_id >= 0);
	ASSERT(texture_id < MAX_TEXTURES);

	{
		vk_emissive_texture_t *const etex = g_lights.map.emissive_textures + texture_id;
		if (etex->set) {
			VectorCopy(etex->emissive, out);
		} else {
			VectorSet(out, 0, 0, 0);
		}
	}
}

void VK_LightsFrameFinalize( void ) {
	APROF_SCOPE_BEGIN_EARLY(finalize);
	const model_t* const world = gEngine.pfnGetModelByIndex( 1 );

	if (g_lights.num_emissive_surfaces > UINT8_MAX) {
		ERROR_THROTTLED(10, "Too many emissive surfaces found: %d; some areas will be dark", g_lights.num_emissive_surfaces);
		g_lights.num_emissive_surfaces = UINT8_MAX;
	}

	/* for (int i = 0; i < MAX_ELIGHTS; ++i) { */
	/* 	const dlight_t *dlight = gEngine.GetEntityLight(i); */
	/* 	if (!addDlight(dlight)) { */
	/* 		ERROR_THROTTLED(10,"Too many elights, MAX_POINT_LIGHTS=%d", MAX_POINT_LIGHTS); */
	/* 		break; */
	/* 	} */
	/* } */

	for (int i = 0; i < g_lights.num_point_lights; ++i) {
		vk_point_light_t *const light = g_lights.point_lights + i;
		if (light->lightstyle < 0 || light->lightstyle >= MAX_LIGHTSTYLES)
			continue;

		{
			const float scale = g_lightmap.lightstylevalue[light->lightstyle] / 255.f;
			VectorScale(light->base_color, scale, light->color);
		}
	}

	APROF_SCOPE_BEGIN(dlights);
	for (int i = 0; i < MAX_DLIGHTS; ++i) {
		const dlight_t *dlight = gEngine.GetDynamicLight(i);
		if( !dlight || dlight->die < gpGlobals->time || !dlight->radius )
			continue;
		addDlight(dlight);
	}
	APROF_SCOPE_END(dlights);

	if (debug_dump_lights.enabled) {
#if 0
		// Print light grid stats
		gEngine.Con_Reportf("Emissive surfaces found: %d\n", g_lights.num_emissive_surfaces);

		{
			#define GROUPSIZE 4
			int histogram[1 + (MAX_VISIBLE_SURFACE_LIGHTS + GROUPSIZE - 1) / GROUPSIZE] = {0};
			for (int i = 0; i < g_lights.map.grid_cells; ++i) {
				const vk_lights_cell_t *cluster = g_lights.cells + i;
				const int hist_index = cluster->num_emissive_surfaces ? 1 + cluster->num_emissive_surfaces / GROUPSIZE : 0;
				histogram[hist_index]++;
			}

			gEngine.Con_Reportf("Built %d light clusters. Stats:\n", g_lights.map.grid_cells);
			gEngine.Con_Reportf("  0: %d\n", histogram[0]);
			for (int i = 1; i < ARRAYSIZE(histogram); ++i)
				gEngine.Con_Reportf("  %d-%d: %d\n",
					(i - 1) * GROUPSIZE,
					i * GROUPSIZE - 1,
					histogram[i]);
		}

		{
			int num_clusters_with_lights_in_range = 0;
			for (int i = 0; i < g_lights.map.grid_cells; ++i) {
				const vk_lights_cell_t *cluster = g_lights.cells + i;
				if (cluster->num_emissive_surfaces > 0) {
					gEngine.Con_Reportf(" cluster %d: emissive_surfaces=%d\n", i, cluster->num_emissive_surfaces);
				}

				for (int j = 0; j < cluster->num_emissive_surfaces; ++j) {
					const int index = cluster->emissive_surfaces[j];
					if (index >= vk_rtx_light_begin->value && index < vk_rtx_light_end->value) {
						++num_clusters_with_lights_in_range;
					}
				}
			}

			gEngine.Con_Reportf("Clusters with filtered lights: %d\n", num_clusters_with_lights_in_range);
		}
#endif
	}

	debug_dump_lights.enabled = false;
	APROF_SCOPE_END(finalize);
}
