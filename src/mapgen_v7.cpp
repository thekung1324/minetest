/*
Minetest
Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include "mapgen.h"
#include "voxel.h"
#include "noise.h"
#include "mapblock.h"
#include "mapnode.h"
#include "map.h"
#include "content_sao.h"
#include "nodedef.h"
#include "voxelalgorithms.h"
#include "profiler.h"
#include "settings.h" // For g_settings
#include "main.h" // For g_profiler
#include "emerge.h"
#include "dungeongen.h"
#include "cavegen.h"
#include "treegen.h"
#include "mg_biome.h"
#include "mg_ore.h"
#include "mg_decoration.h"
#include "mapgen_v7.h"


FlagDesc flagdesc_mapgen_v7[] = {
	{"mountains", MGV7_MOUNTAINS},
	{"ridges",    MGV7_RIDGES},
	{NULL,        0}
};

///////////////////////////////////////////////////////////////////////////////


MapgenV7::MapgenV7(int mapgenid, MapgenParams *params, EmergeManager *emerge)
	: Mapgen(mapgenid, params, emerge)
{
	this->m_emerge = emerge;
	this->bmgr     = emerge->biomemgr;

	//// amount of elements to skip for the next index
	//// for noise/height/biome maps (not vmanip)
	this->ystride = csize.X;
	this->zstride = csize.X * (csize.Y + 2);

	this->biomemap  = new u8[csize.X * csize.Z];
	this->heightmap = new s16[csize.X * csize.Z];
	this->ridge_heightmap = new s16[csize.X * csize.Z];

	MapgenV7Params *sp = (MapgenV7Params *)params->sparams;
	this->spflags = sp->spflags;

	//// Terrain noise
	noise_terrain_base    = new Noise(&sp->np_terrain_base,    seed, csize.X, csize.Z);
	noise_terrain_alt     = new Noise(&sp->np_terrain_alt,     seed, csize.X, csize.Z);
	noise_terrain_persist = new Noise(&sp->np_terrain_persist, seed, csize.X, csize.Z);
	noise_height_select   = new Noise(&sp->np_height_select,   seed, csize.X, csize.Z);
	noise_filler_depth    = new Noise(&sp->np_filler_depth,    seed, csize.X, csize.Z);
	noise_mount_height    = new Noise(&sp->np_mount_height,    seed, csize.X, csize.Z);
	noise_ridge_uwater    = new Noise(&sp->np_ridge_uwater,    seed, csize.X, csize.Z);

	//// 3d terrain noise
	noise_mountain = new Noise(&sp->np_mountain, seed, csize.X, csize.Y + 2, csize.Z);
	noise_ridge    = new Noise(&sp->np_ridge,    seed, csize.X, csize.Y + 2, csize.Z);
	noise_cave1    = new Noise(&sp->np_cave1,    seed, csize.X, csize.Y + 2, csize.Z);
	noise_cave2    = new Noise(&sp->np_cave2,    seed, csize.X, csize.Y + 2, csize.Z);

	//// Biome noise
	noise_heat     = new Noise(&params->np_biome_heat,     seed, csize.X, csize.Z);
	noise_humidity = new Noise(&params->np_biome_humidity, seed, csize.X, csize.Z);

	//// Resolve nodes to be used
	INodeDefManager *ndef = emerge->ndef;

	c_stone           = ndef->getId("mapgen_stone");
	c_dirt            = ndef->getId("mapgen_dirt");
	c_dirt_with_grass = ndef->getId("mapgen_dirt_with_grass");
	c_sand            = ndef->getId("mapgen_sand");
	c_water_source    = ndef->getId("mapgen_water_source");
	c_lava_source     = ndef->getId("mapgen_lava_source");
	c_ice             = ndef->getId("default:ice");
	c_cobble          = ndef->getId("mapgen_cobble");
	c_desert_stone    = ndef->getId("mapgen_desert_stone");
	c_mossycobble     = ndef->getId("mapgen_mossycobble");
	c_sandbrick       = ndef->getId("mapgen_sandstonebrick");
	c_stair_cobble    = ndef->getId("mapgen_stair_cobble");
	c_stair_sandstone = ndef->getId("mapgen_stair_sandstone");
	if (c_ice == CONTENT_IGNORE)
		c_ice = CONTENT_AIR;
	if (c_mossycobble == CONTENT_IGNORE)
		c_mossycobble = c_cobble;
	if (c_sandbrick == CONTENT_IGNORE)
		c_sandbrick = c_desert_stone;
	if (c_stair_cobble == CONTENT_IGNORE)
		c_stair_cobble = c_cobble;
	if (c_stair_sandstone == CONTENT_IGNORE)
		c_stair_sandstone = c_sandbrick;
}


MapgenV7::~MapgenV7()
{
	delete noise_terrain_base;
	delete noise_terrain_persist;
	delete noise_height_select;
	delete noise_terrain_alt;
	delete noise_filler_depth;
	delete noise_mount_height;
	delete noise_ridge_uwater;
	delete noise_mountain;
	delete noise_ridge;
	delete noise_cave1;
	delete noise_cave2;

	delete noise_heat;
	delete noise_humidity;

	delete[] ridge_heightmap;
	delete[] heightmap;
	delete[] biomemap;
}


MapgenV7Params::MapgenV7Params()
{
	spflags = MGV7_MOUNTAINS | MGV7_RIDGES;

	np_terrain_base    = NoiseParams(4,    70,  v3f(300, 300, 300), 82341, 6, 0.7,  2.0);
	np_terrain_alt     = NoiseParams(4,    25,  v3f(600, 600, 600), 5934,  5, 0.6,  2.0);
	np_terrain_persist = NoiseParams(0.6,  0.1, v3f(500, 500, 500), 539,   3, 0.6,  2.0);
	np_height_select   = NoiseParams(-0.5, 1,   v3f(250, 250, 250), 4213,  5, 0.69, 2.0);
	np_filler_depth    = NoiseParams(0,    1.2, v3f(150, 150, 150), 261,   4, 0.7,  2.0);
	np_mount_height    = NoiseParams(100,  30,  v3f(500, 500, 500), 72449, 4, 0.6,  2.0);
	np_ridge_uwater    = NoiseParams(0,    1,   v3f(500, 500, 500), 85039, 4, 0.6,  2.0);
	np_mountain        = NoiseParams(-0.6, 1,   v3f(250, 350, 250), 5333,  5, 0.68, 2.0);
	np_ridge           = NoiseParams(0,    1,   v3f(100, 100, 100), 6467,  4, 0.75, 2.0);
	np_cave1           = NoiseParams(0,    12,  v3f(100, 100, 100), 52534, 4, 0.5,  2.0);
	np_cave2           = NoiseParams(0,    12,  v3f(100, 100, 100), 10325, 4, 0.5,  2.0);
}


void MapgenV7Params::readParams(const Settings *settings)
{
	settings->getFlagStrNoEx("mgv7_spflags", spflags, flagdesc_mapgen_v7);

	settings->getNoiseParams("mgv7_np_terrain_base",    np_terrain_base);
	settings->getNoiseParams("mgv7_np_terrain_alt",     np_terrain_alt);
	settings->getNoiseParams("mgv7_np_terrain_persist", np_terrain_persist);
	settings->getNoiseParams("mgv7_np_height_select",   np_height_select);
	settings->getNoiseParams("mgv7_np_filler_depth",    np_filler_depth);
	settings->getNoiseParams("mgv7_np_mount_height",    np_mount_height);
	settings->getNoiseParams("mgv7_np_ridge_uwater",    np_ridge_uwater);
	settings->getNoiseParams("mgv7_np_mountain",        np_mountain);
	settings->getNoiseParams("mgv7_np_ridge",           np_ridge);
	settings->getNoiseParams("mgv7_np_cave1",           np_cave1);
	settings->getNoiseParams("mgv7_np_cave2",           np_cave2);
}


void MapgenV7Params::writeParams(Settings *settings) const
{
	settings->setFlagStr("mgv7_spflags", spflags, flagdesc_mapgen_v7, (u32)-1);

	settings->setNoiseParams("mgv7_np_terrain_base",    np_terrain_base);
	settings->setNoiseParams("mgv7_np_terrain_alt",     np_terrain_alt);
	settings->setNoiseParams("mgv7_np_terrain_persist", np_terrain_persist);
	settings->setNoiseParams("mgv7_np_height_select",   np_height_select);
	settings->setNoiseParams("mgv7_np_filler_depth",    np_filler_depth);
	settings->setNoiseParams("mgv7_np_mount_height",    np_mount_height);
	settings->setNoiseParams("mgv7_np_ridge_uwater",    np_ridge_uwater);
	settings->setNoiseParams("mgv7_np_mountain",        np_mountain);
	settings->setNoiseParams("mgv7_np_ridge",           np_ridge);
	settings->setNoiseParams("mgv7_np_cave1",           np_cave1);
	settings->setNoiseParams("mgv7_np_cave2",           np_cave2);
}


///////////////////////////////////////


int MapgenV7::getGroundLevelAtPoint(v2s16 p)
{
	// Base terrain calculation
	s16 y = baseTerrainLevelAtPoint(p.X, p.Y);

	// Ridge/river terrain calculation
	float width = 0.2;
	float uwatern = NoisePerlin2D(&noise_ridge_uwater->np, p.X, p.Y, seed) * 2;
	// actually computing the depth of the ridge is much more expensive;
	// if inside a river, simply guess
	if (fabs(uwatern) <= width)
		return water_level - 10;

	// Mountain terrain calculation
	int iters = 128; // don't even bother iterating more than 128 times..
	while (iters--) {
		//current point would have been air
		if (!getMountainTerrainAtPoint(p.X, y, p.Y))
			return y;

		y++;
	}

	return y;
}


void MapgenV7::makeChunk(BlockMakeData *data)
{
	// Pre-conditions
	assert(data->vmanip);
	assert(data->nodedef);
	assert(data->blockpos_requested.X >= data->blockpos_min.X &&
		   data->blockpos_requested.Y >= data->blockpos_min.Y &&
		   data->blockpos_requested.Z >= data->blockpos_min.Z);
	assert(data->blockpos_requested.X <= data->blockpos_max.X &&
		   data->blockpos_requested.Y <= data->blockpos_max.Y &&
		   data->blockpos_requested.Z <= data->blockpos_max.Z);

	this->generating = true;
	this->vm   = data->vmanip;
	this->ndef = data->nodedef;
	//TimeTaker t("makeChunk");

	v3s16 blockpos_min = data->blockpos_min;
	v3s16 blockpos_max = data->blockpos_max;
	node_min = blockpos_min * MAP_BLOCKSIZE;
	node_max = (blockpos_max + v3s16(1, 1, 1)) * MAP_BLOCKSIZE - v3s16(1, 1, 1);
	full_node_min = (blockpos_min - 1) * MAP_BLOCKSIZE;
	full_node_max = (blockpos_max + 2) * MAP_BLOCKSIZE - v3s16(1, 1, 1);

	blockseed = getBlockSeed2(full_node_min, seed);

	// Make some noise
	calculateNoise();

	// Generate base terrain, mountains, and ridges with initial heightmaps
	s16 stone_surface_max_y = generateTerrain();

	// Create heightmap
	updateHeightmap(node_min, node_max);

	// Create biomemap at heightmap surface
	bmgr->calcBiomes(csize.X, csize.Z, noise_heat->result,
		noise_humidity->result, heightmap, biomemap);

	// Actually place the biome-specific nodes
	bool desert_stone = generateBiomes(noise_heat->result, noise_humidity->result);

	if (flags & MG_CAVES)
		generateCaves(stone_surface_max_y);

	if ((flags & MG_DUNGEONS) && (stone_surface_max_y >= node_min.Y)) {
		DungeonParams dp;

		dp.np_rarity  = nparams_dungeon_rarity;
		dp.np_density = nparams_dungeon_density;
		dp.np_wetness = nparams_dungeon_wetness;
		dp.c_water = c_water_source;
		if (desert_stone) {
			dp.c_cobble  = c_sandbrick;
			dp.c_moss    = c_sandbrick; // should make this 'cracked sandstone' later
			dp.c_stair   = c_stair_sandstone;

			dp.diagonal_dirs = true;
			dp.mossratio  = 0.0;
			dp.holesize   = v3s16(2, 3, 2);
			dp.roomsize   = v3s16(2, 5, 2);
			dp.notifytype = GENNOTIFY_TEMPLE;
		} else {
			dp.c_cobble  = c_cobble;
			dp.c_moss    = c_mossycobble;
			dp.c_stair   = c_stair_cobble;

			dp.diagonal_dirs = false;
			dp.mossratio  = 3.0;
			dp.holesize   = v3s16(1, 2, 1);
			dp.roomsize   = v3s16(0, 0, 0);
			dp.notifytype = GENNOTIFY_DUNGEON;
		}

		DungeonGen dgen(this, &dp);
		dgen.generate(blockseed, full_node_min, full_node_max);
	}

	// Generate the registered decorations
	m_emerge->decomgr->placeAllDecos(this, blockseed, node_min, node_max);

	// Generate the registered ores
	m_emerge->oremgr->placeAllOres(this, blockseed, node_min, node_max);

	// Sprinkle some dust on top after everything else was generated
	dustTopNodes();

	//printf("makeChunk: %dms\n", t.stop());

	updateLiquid(&data->transforming_liquid, full_node_min, full_node_max);

	if (flags & MG_LIGHT)
		calcLighting(node_min - v3s16(0, 1, 0), node_max + v3s16(0, 1, 0),
			full_node_min, full_node_max);

	//setLighting(node_min - v3s16(1, 0, 1) * MAP_BLOCKSIZE,
	//			node_max + v3s16(1, 0, 1) * MAP_BLOCKSIZE, 0xFF);

	this->generating = false;
}


void MapgenV7::calculateNoise()
{
	//TimeTaker t("calculateNoise", NULL, PRECISION_MICRO);
	int x = node_min.X;
	int y = node_min.Y - 1;
	int z = node_min.Z;

	noise_terrain_persist->perlinMap2D(x, z);
	float *persistmap = noise_terrain_persist->result;

	noise_terrain_base->perlinMap2D(x, z, persistmap);
	noise_terrain_alt->perlinMap2D(x, z, persistmap);
	noise_height_select->perlinMap2D(x, z);

	if (flags & MG_CAVES) {
		noise_cave1->perlinMap3D(x, y, z);
		noise_cave2->perlinMap3D(x, y, z);
	}

	if ((spflags & MGV7_RIDGES) && node_max.Y >= water_level) {
		noise_ridge->perlinMap3D(x, y, z);
		noise_ridge_uwater->perlinMap2D(x, z);
	}

	if ((spflags & MGV7_MOUNTAINS) && node_max.Y >= 0) {
		noise_mountain->perlinMap3D(x, y, z);
		noise_mount_height->perlinMap2D(x, z);
	}

	if (node_max.Y >= water_level) {
		noise_filler_depth->perlinMap2D(x, z);
		noise_heat->perlinMap2D(x, z);
		noise_humidity->perlinMap2D(x, z);
	}
	//printf("calculateNoise: %dus\n", t.stop());
}


Biome *MapgenV7::getBiomeAtPoint(v3s16 p)
{
	float heat      = NoisePerlin2D(&noise_heat->np, p.X, p.Z, seed);
	float humidity  = NoisePerlin2D(&noise_humidity->np, p.X, p.Z, seed);
	s16 groundlevel = baseTerrainLevelAtPoint(p.X, p.Z);

	return bmgr->getBiome(heat, humidity, groundlevel);
}

//needs to be updated
float MapgenV7::baseTerrainLevelAtPoint(int x, int z)
{
	float hselect = NoisePerlin2D(&noise_height_select->np, x, z, seed);
	hselect = rangelim(hselect, 0.0, 1.0);

	float persist = NoisePerlin2D(&noise_terrain_persist->np, x, z, seed);

	noise_terrain_base->np.persist = persist;
	float height_base = NoisePerlin2D(&noise_terrain_base->np, x, z, seed);

	noise_terrain_alt->np.persist = persist;
	float height_alt = NoisePerlin2D(&noise_terrain_alt->np, x, z, seed);

	if (height_alt > height_base)
		return height_alt;

	return (height_base * hselect) + (height_alt * (1.0 - hselect));
}


float MapgenV7::baseTerrainLevelFromMap(int index)
{
	float hselect     = rangelim(noise_height_select->result[index], 0.0, 1.0);
	float height_base = noise_terrain_base->result[index];
	float height_alt  = noise_terrain_alt->result[index];

	if (height_alt > height_base)
		return height_alt;

	return (height_base * hselect) + (height_alt * (1.0 - hselect));
}


bool MapgenV7::getMountainTerrainAtPoint(int x, int y, int z)
{
	float mnt_h_n = NoisePerlin2D(&noise_mount_height->np, x, z, seed);
	float mnt_n = NoisePerlin3D(&noise_mountain->np, x, y, z, seed);
	return mnt_n * mnt_h_n >= (float)y;
}


bool MapgenV7::getMountainTerrainFromMap(int idx_xyz, int idx_xz, int y)
{
	float mounthn = noise_mount_height->result[idx_xz];
	float mountn = noise_mountain->result[idx_xyz];
	return mountn * mounthn >= (float)y;
}


#if 0
void MapgenV7::carveRivers() {
	MapNode n_air(CONTENT_AIR), n_water_source(c_water_source);
	MapNode n_stone(c_stone);
	u32 index = 0;

	int river_depth = 4;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index++) {
		float terrain_mod  = noise_terrain_mod->result[index];
		NoiseParams *np = noise_terrain_river->np;
		np.persist = noise_terrain_persist->result[index];
		float terrain_river = NoisePerlin2DNoTxfm(np, x, z, seed);
		float height = terrain_river * (1 - abs(terrain_mod)) *
						noise_terrain_river->np.scale;
		height = log(height * height); //log(h^3) is pretty interesting for terrain

		s16 y = heightmap[index];
		if (height < 1.0 && y > river_depth &&
			y - river_depth >= node_min.Y && y <= node_max.Y) {

			for (s16 ry = y; ry != y - river_depth; ry--) {
				u32 vi = vm->m_area.index(x, ry, z);
				vm->m_data[vi] = n_air;
			}

			u32 vi = vm->m_area.index(x, y - river_depth, z);
			vm->m_data[vi] = n_water_source;
		}
	}
}
#endif


int MapgenV7::generateTerrain()
{
	int ymax = generateBaseTerrain();

	if (spflags & MGV7_MOUNTAINS)
		ymax = generateMountainTerrain(ymax);

	if (spflags & MGV7_RIDGES)
		generateRidgeTerrain();

	return ymax;
}


int MapgenV7::generateBaseTerrain()
{
	MapNode n_air(CONTENT_AIR);
	MapNode n_stone(c_stone);
	MapNode n_water(c_water_source);

	int stone_surface_max_y = -MAP_GENERATION_LIMIT;
	v3s16 em = vm->m_area.getExtent();
	u32 index = 0;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index++) {
		float surface_height = baseTerrainLevelFromMap(index);
		s16 surface_y = (s16)surface_height;

		heightmap[index]       = surface_y;
		ridge_heightmap[index] = surface_y;

		if (surface_y > stone_surface_max_y)
			stone_surface_max_y = surface_y;

		u32 i = vm->m_area.index(x, node_min.Y - 1, z);
		for (s16 y = node_min.Y - 1; y <= node_max.Y + 1; y++) {
			if (vm->m_data[i].getContent() == CONTENT_IGNORE) {
				if (y <= surface_y)
					vm->m_data[i] = n_stone;
				else if (y <= water_level)
					vm->m_data[i] = n_water;
				else
					vm->m_data[i] = n_air;
			}
			vm->m_area.add_y(em, i, 1);
		}
	}

	return stone_surface_max_y;
}


int MapgenV7::generateMountainTerrain(int ymax)
{
	if (node_max.Y < 0)
		return ymax;

	MapNode n_stone(c_stone);
	u32 j = 0;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 y = node_min.Y - 1; y <= node_max.Y + 1; y++) {
		u32 vi = vm->m_area.index(node_min.X, y, z);
		for (s16 x = node_min.X; x <= node_max.X; x++) {
			int index = (z - node_min.Z) * csize.X + (x - node_min.X);
			content_t c = vm->m_data[vi].getContent();

			if (getMountainTerrainFromMap(j, index, y)
					&& (c == CONTENT_AIR || c == c_water_source)) {
				vm->m_data[vi] = n_stone;
				if (y > ymax)
					ymax = y;
			}

			vi++;
			j++;
		}
	}

	return ymax;
}


void MapgenV7::generateRidgeTerrain()
{
	if (node_max.Y < water_level)
		return;

	MapNode n_water(c_water_source);
	MapNode n_air(CONTENT_AIR);
	u32 index = 0;
	float width = 0.2; // TODO: figure out acceptable perlin noise values

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 y = node_min.Y - 1; y <= node_max.Y + 1; y++) {
		u32 vi = vm->m_area.index(node_min.X, y, z);
		for (s16 x = node_min.X; x <= node_max.X; x++, index++, vi++) {
			int j = (z - node_min.Z) * csize.X + (x - node_min.X);

			if (heightmap[j] < water_level - 16)
				continue;

			float uwatern = noise_ridge_uwater->result[j] * 2;
			if (fabs(uwatern) > width)
				continue;

			float altitude = y - water_level;
			float height_mod = (altitude + 17) / 2.5;
			float width_mod  = width - fabs(uwatern);
			float nridge = noise_ridge->result[index] * MYMAX(altitude, 0) / 7.0;

			if (nridge + width_mod * height_mod < 0.6)
				continue;

			if (y < ridge_heightmap[j])
				ridge_heightmap[j] = y - 1;

			vm->m_data[vi] = (y > water_level) ? n_air : n_water;
		}
	}
}


bool MapgenV7::generateBiomes(float *heat_map, float *humidity_map)
{
	if (node_max.Y < water_level)
		return false;

	v3s16 em = vm->m_area.getExtent();
	u32 index = 0;
	bool desert_stone = false;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index++) {
		Biome *biome = NULL;
		s16 dfiller = 0;
		s16 y0_top = 0;
		s16 y0_filler = 0;
		s16 depth_water_top = 0;

		s16 nplaced = 0;
		u32 i = vm->m_area.index(x, node_max.Y, z);

		content_t c_above = vm->m_data[i + em.X].getContent();
		bool have_air = c_above == CONTENT_AIR;

		for (s16 y = node_max.Y; y >= node_min.Y; y--) {
			content_t c = vm->m_data[i].getContent();

			if (c != CONTENT_IGNORE && c != CONTENT_AIR && (y == node_max.Y || have_air)) {
				biome           = bmgr->getBiome(heat_map[index], humidity_map[index], y);
				dfiller         = biome->depth_filler + noise_filler_depth->result[index];
				y0_top          = biome->depth_top;
				y0_filler       = biome->depth_top + dfiller;
				depth_water_top = biome->depth_water_top;

				if (biome->c_stone == c_desert_stone)
					desert_stone = true;
			}

			if (c == c_stone && have_air) {
				content_t c_below = vm->m_data[i - em.X].getContent();

				if (c_below != CONTENT_AIR) {
					if (nplaced < y0_top) {
						vm->m_data[i] = MapNode(biome->c_top);
						nplaced++;
					} else if (nplaced < y0_filler && nplaced >= y0_top) {
						vm->m_data[i] = MapNode(biome->c_filler);
						nplaced++;
					} else if (c == c_stone) {
						have_air = false;
						nplaced  = 0;
						vm->m_data[i] = MapNode(biome->c_stone);
					} else {
						have_air = false;
						nplaced  = 0;
					}
				} else if (c == c_stone) {
					have_air = false;
					nplaced = 0;
					vm->m_data[i] = MapNode(biome->c_stone);
				}
			} else if (c == c_stone) {
				have_air = false;
				nplaced = 0;
				vm->m_data[i] = MapNode(biome->c_stone);
			} else if (c == c_water_source) {
				have_air = true;
				nplaced = 0;
				if (y > water_level - depth_water_top)
					vm->m_data[i] = MapNode(biome->c_water_top);
				else
					vm->m_data[i] = MapNode(biome->c_water);
			} else if (c == CONTENT_AIR) {
				have_air = true;
				nplaced = 0;
			}

			vm->m_area.add_y(em, i, -1);
		}
	}

	return desert_stone;
}


void MapgenV7::dustTopNodes()
{
	if (node_max.Y < water_level)
		return;

	v3s16 em = vm->m_area.getExtent();
	u32 index = 0;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index++) {
		Biome *biome = (Biome *)bmgr->get(biomemap[index]);

		if (biome->c_dust == CONTENT_IGNORE)
			continue;

		u32 vi = vm->m_area.index(x, full_node_max.Y, z);
		content_t c_full_max = vm->m_data[vi].getContent();
		s16 y_start;

		if (c_full_max == CONTENT_AIR) {
			y_start = full_node_max.Y - 1;
		} else if (c_full_max == CONTENT_IGNORE) {
			vi = vm->m_area.index(x, node_max.Y + 1, z);
			content_t c_max = vm->m_data[vi].getContent();

			if (c_max == CONTENT_AIR)
				y_start = node_max.Y;
			else
				continue;
		} else {
			continue;
		}

		vi = vm->m_area.index(x, y_start, z);
		for (s16 y = y_start; y >= node_min.Y - 1; y--) {
			if (vm->m_data[vi].getContent() != CONTENT_AIR)
				break;

			vm->m_area.add_y(em, vi, -1);
		}

		content_t c = vm->m_data[vi].getContent();
		if (!ndef->get(c).buildable_to && c != CONTENT_IGNORE && c != biome->c_dust) {
			vm->m_area.add_y(em, vi, 1);
			vm->m_data[vi] = MapNode(biome->c_dust);
		}
	}
}


#if 0
void MapgenV7::addTopNodes()
{
	v3s16 em = vm->m_area.getExtent();
	s16 ntopnodes;
	u32 index = 0;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index++) {
		Biome *biome = bmgr->biomes[biomemap[index]];

		//////////////////// First, add top nodes below the ridge
		s16 y = ridge_heightmap[index];

		// This cutoff is good enough, but not perfect.
		// It will cut off potentially placed top nodes at chunk boundaries
		if (y < node_min.Y)
			continue;
		if (y > node_max.Y) {
			y = node_max.Y; // Let's see if we can still go downward anyway
			u32 vi = vm->m_area.index(x, y, z);
			content_t c = vm->m_data[vi].getContent();
			if (ndef->get(c).walkable)
				continue;
		}

		// N.B.  It is necessary to search downward since ridge_heightmap[i]
		// might not be the actual height, just the lowest part in the chunk
		// where a ridge had been carved
		u32 i = vm->m_area.index(x, y, z);
		for (; y >= node_min.Y; y--) {
			content_t c = vm->m_data[i].getContent();
			if (ndef->get(c).walkable)
				break;
			vm->m_area.add_y(em, i, -1);
		}

		if (y != node_min.Y - 1 && y >= water_level) {
			ridge_heightmap[index] = y; //update ridgeheight
			ntopnodes = biome->top_depth;
			for (; y <= node_max.Y && ntopnodes; y++) {
				ntopnodes--;
				vm->m_data[i] = MapNode(biome->c_top);
				vm->m_area.add_y(em, i, 1);
			}
			// If dirt, grow grass on it.
			if (y > water_level - 10 &&
				vm->m_data[i].getContent() == CONTENT_AIR) {
				vm->m_area.add_y(em, i, -1);
				if (vm->m_data[i].getContent() == c_dirt)
					vm->m_data[i] = MapNode(c_dirt_with_grass);
			}
		}

		//////////////////// Now, add top nodes on top of the ridge
		y = heightmap[index];
		if (y > node_max.Y) {
			y = node_max.Y; // Let's see if we can still go downward anyway
			u32 vi = vm->m_area.index(x, y, z);
			content_t c = vm->m_data[vi].getContent();
			if (ndef->get(c).walkable)
				continue;
		}

		i = vm->m_area.index(x, y, z);
		for (; y >= node_min.Y; y--) {
			content_t c = vm->m_data[i].getContent();
			if (ndef->get(c).walkable)
				break;
			vm->m_area.add_y(em, i, -1);
		}

		if (y != node_min.Y - 1) {
			ntopnodes = biome->top_depth;
			// Let's see if we've already added it...
			if (y == ridge_heightmap[index] + ntopnodes - 1)
				continue;

			for (; y <= node_max.Y && ntopnodes; y++) {
				ntopnodes--;
				vm->m_data[i] = MapNode(biome->c_top);
				vm->m_area.add_y(em, i, 1);
			}
			// If dirt, grow grass on it.
			if (y > water_level - 10 &&
				vm->m_data[i].getContent() == CONTENT_AIR) {
				vm->m_area.add_y(em, i, -1);
				if (vm->m_data[i].getContent() == c_dirt)
					vm->m_data[i] = MapNode(c_dirt_with_grass);
			}
		}
	}
}
#endif


void MapgenV7::generateCaves(int max_stone_y)
{
	if (max_stone_y >= node_min.Y) {
		u32 index   = 0;
		u32 index2d = 0;

		for (s16 z = node_min.Z; z <= node_max.Z; z++) {
			for (s16 y = node_min.Y - 1; y <= node_max.Y + 1; y++) {
				u32 i = vm->m_area.index(node_min.X, y, z);
				for (s16 x = node_min.X; x <= node_max.X;
						x++, i++, index++, index2d++) {
					Biome *biome = (Biome *)bmgr->get(biomemap[index2d]);
					content_t c = vm->m_data[i].getContent();
					if (c == CONTENT_AIR || (y <= water_level &&
						c != biome->c_stone && c != c_stone))
						continue;

					float d1 = contour(noise_cave1->result[index]);
					float d2 = contour(noise_cave2->result[index]);
					if (d1 * d2 > 0.3)
						vm->m_data[i] = MapNode(CONTENT_AIR);
				}
				index2d -= ystride;
			}
			index2d += ystride;
		}
	}

	PseudoRandom ps(blockseed + 21343);
	u32 bruises_count = (ps.range(1, 5) == 1) ? ps.range(1, 2) : 0;
	for (u32 i = 0; i < bruises_count; i++) {
		CaveV7 cave(this, &ps);
		cave.makeCave(node_min, node_max, max_stone_y);
	}
}

