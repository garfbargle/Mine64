#ifndef ITEMS_H
#define ITEMS_H

#include <nusys.h>
#include "math.h"

#define MAX_DROPPED_ITEMS 16

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

void initDroppedItems();
void spawnDroppedItem(u8 item, u8 count, u8 x, u8 y, u8 z);
void updateDroppedItems(float delta);

#endif /* ITEMS_H */
