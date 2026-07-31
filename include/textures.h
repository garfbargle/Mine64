#include <nusys.h>
#include "blocks.h"
#include "items.h"
#include "texture_data.h"

typedef struct {
  u8 block;
  u8 top;
  u8 bottom;
  u8 sides;
} FaceSpec;

FaceSpec dirt_faces[] = {
  {DIRT,  TRUE, TRUE, TRUE},
  {GRASS, FALSE, TRUE, FALSE}
};

FaceSpec stone_faces[] = {
  {STONE, TRUE, TRUE, TRUE}
};

FaceSpec coal_ore_faces[] = {
  {COAL_ORE, TRUE, TRUE, TRUE}
};

FaceSpec iron_ore_faces[] = {
  {IRON_ORE, TRUE, TRUE, TRUE}
};

FaceSpec bedrock_faces[] = {
  {BEDROCK, TRUE, TRUE, TRUE}
};

FaceSpec mossy_cobblestone_faces[] = {
  {MOSSY_COBBLESTONE, TRUE, TRUE, TRUE}
};

FaceSpec grass_top_faces[] = {
  {GRASS, TRUE, FALSE, FALSE}
};

FaceSpec grass_side_faces[] = {
  {GRASS, FALSE, FALSE, TRUE}
};

FaceSpec cobblestone_faces[] = {
  {COBBLESTONE, TRUE, TRUE, TRUE}
};

FaceSpec sand_faces[] = {
  {SAND, TRUE, TRUE, TRUE}
};

FaceSpec water_faces[] = {
  {WATER, TRUE, TRUE, TRUE}
};

FaceSpec wood_top_faces[] = {
  {WOOD, TRUE, TRUE, FALSE}
};

FaceSpec wood_side_faces[] = {
  {WOOD, FALSE, FALSE, TRUE}
};

FaceSpec leaves_faces[] = {
  {LEAVES, TRUE, TRUE, TRUE}
};

FaceSpec planks_faces[] = {
  {PLANKS, TRUE, TRUE, TRUE},
  {CRAFTING_TABLE, TRUE, TRUE, TRUE}
};

FaceSpec bricks_faces[] = {
  {BRICKS, TRUE, TRUE, TRUE}
};

typedef struct {
  Texture *texture;
  u8 n_faces;
  FaceSpec *faces;
} TextureSpec;

TextureSpec dirt_spec = {
  &dirt_texture, 2, dirt_faces
};

TextureSpec stone_spec = {
  &stone_texture, 1, stone_faces
};

TextureSpec coal_ore_spec = {
  &coal_ore_texture, 1, coal_ore_faces
};

TextureSpec iron_ore_spec = {
  &iron_ore_texture, 1, iron_ore_faces
};

TextureSpec bedrock_spec = {
  &bedrock_texture, 1, bedrock_faces
};

TextureSpec mossy_cobblestone_spec = {
  &mossy_cobblestone_texture, 1, mossy_cobblestone_faces
};

TextureSpec grass_top_spec = {
  &grass_top_texture, 1, grass_top_faces
};

TextureSpec grass_side_spec = {
  &grass_side_texture, 1, grass_side_faces
};

TextureSpec cobblestone_spec = {
  &cobblestone_texture, 1, cobblestone_faces
};

TextureSpec sand_spec = {
  &sand_texture, 1, sand_faces
};

TextureSpec water_spec = {
  &water_texture, 1, water_faces
};

TextureSpec wood_top_spec = {
  &wood_top_texture, 1, wood_top_faces
};

TextureSpec wood_side_spec = {
  &wood_side_texture, 1, wood_side_faces
};

TextureSpec leaves_spec = {
  &leaves_texture, 1, leaves_faces
};

TextureSpec planks_spec = {
  &planks_texture, 2, planks_faces
};

TextureSpec bricks_spec = {
  &bricks_texture, 1, bricks_faces
};

TextureSpec *textures[] = {
  &dirt_spec, &stone_spec, &grass_top_spec, &grass_side_spec, &cobblestone_spec, &sand_spec,
  &water_spec, &wood_top_spec, &wood_side_spec, &leaves_spec, &planks_spec, &bricks_spec,
  &coal_ore_spec, &iron_ore_spec, &bedrock_spec, &mossy_cobblestone_spec
};

Texture *preview_textures[ITEM_TYPE_COUNT + 1] = {
  [DIRT] = &dirt_texture,
  [STONE] = &stone_texture,
  [GRASS] = &grass_side_texture,
  [COBBLESTONE] = &cobblestone_texture,
  [SAND] = &sand_texture,
  [WOOD] = &wood_side_texture,
  [LEAVES] = &leaves_texture,
  [PLANKS] = &planks_texture,
  [BRICKS] = &bricks_texture,
  [CRAFTING_TABLE] = &planks_texture,
  [SAPLING] = &leaves_texture,
  [WOOL] = &wool_texture,
  [WOOD_STAIRS] = &planks_texture,
  [STONE_STAIRS] = &cobblestone_texture,
  [WOOD_DOOR] = &planks_texture,
  [GLASS_WINDOW] = &sand_texture
};

#define NUM_TEXTURES (sizeof(textures) / sizeof(textures[0]))
