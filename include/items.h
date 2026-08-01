#ifndef ITEMS_H
#define ITEMS_H

#include <nusys.h>
#include "blocks.h"
#include "math.h"
#include "player.h"

#define MAX_DROPPED_ITEMS 16

/* Inventory-only IDs begin after the complete four-bit terrain namespace. */
#define STICK          16
#define WOOD_SWORD     17
#define WOOD_PICKAXE   18
#define SAPLING        19
#define WOOL           20
#define COAL           21
#define IRON_CHUNK     22
#define STONE_SWORD    23
#define STONE_PICKAXE  24
#define WOOD_AXE       25
#define STONE_AXE      26
#define APPLE          27
#define RAW_MUTTON     28
#define RAW_PORK       29
#define SLIME_GEL      30
#define IRON_SWORD     31
#define IRON_PICKAXE   32
#define IRON_AXE       33
#define TORCH          34
#define WOOD_STAIRS    35
#define STONE_STAIRS   36
#define WOOD_DOOR      37
#define GLASS_WINDOW   38
#define RAW_CHICKEN    39
#define FEATHER        40
#define FENCE          41
#define FENCE_GATE     42
#define LADDER         43
#define BED            44
/* Cooking is the crafting table plus coal, the same stand-in for a furnace
   that fires glass.  These sit at the end of the namespace rather than beside
   their raw forms because a save file stores the ID: inserting one in the
   middle would silently turn every stack above it into something else. */
#define COOKED_PORK    45
#define COOKED_MUTTON  46
#define COOKED_CHICKEN 47
#define ITEM_TYPE_COUNT COOKED_CHICKEN
#define ITEM_IS_VALID(item) \
  ((item) <= BLOCK_TYPE_COUNT || \
   ((item) >= STICK && (item) <= ITEM_TYPE_COUNT))

typedef struct {
  Vector3 position;
  Vector3 velocity;
  float rotation;
  u8 item;
  u8 count;
  float pickup_delay;
  float pickup_progress;
  u8 pickup_player;
  u8 active;
} DroppedItem;

extern DroppedItem dropped_items[MAX_DROPPED_ITEMS];
extern u8 pickup_message[MAX_PLAYERS];
extern u8 pickup_item[MAX_PLAYERS];

u8 itemMaxStack(u8 item);
const char *itemName(u8 item);
u8 itemIsTool(u8 item);
u8 itemIsSword(u8 item);
u8 itemIsPickaxe(u8 item);
u8 itemIsAxe(u8 item);
u8 itemIsDetail(u8 item);
/* Hunger points the item restores when eaten, or zero when it is not food.
   One table so the A button, the recipe browser and the HUD cannot disagree
   about what a thing is worth. */
u8 itemNourishment(u8 item);
u8 rollLeafDrop(u8 *item);
void initDroppedItems();
u8 spawnDroppedItem(u8 item, u8 count, int x, u8 y, int z);
void updateDroppedItems(float delta);

#endif /* ITEMS_H */
