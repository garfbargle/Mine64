#ifndef ITEMS_H
#define ITEMS_H

#include <nusys.h>
#include "math.h"

#define MAX_DROPPED_ITEMS 16

/* Block IDs are also item IDs.  Tools start after the placeable block range
   so they can share inventory stacks without ever being placed in the world. */
#define STICK         11
#define WOOD_SWORD    12
#define WOOD_PICKAXE  13

typedef struct {
  Vector3 position;
  Vector3 velocity;
  float rotation;
  u8 item;
  u8 count;
  u8 pickup_delay;
  u8 active;
} DroppedItem;

extern DroppedItem dropped_items[MAX_DROPPED_ITEMS];

u8 itemMaxStack(u8 item);
const char *itemName(u8 item);
void initDroppedItems();
u8 spawnDroppedItem(u8 item, u8 count, u8 x, u8 y, u8 z);
void updateDroppedItems(float delta);

#endif /* ITEMS_H */
