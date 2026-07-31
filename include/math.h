#ifndef MATH_H
#define MATH_H

#include <nusys.h>

typedef struct {
  float x;
  float y;
  float z;
} Vector3;

typedef struct {
  int x;
  int y;
  int z;
} Vector3i;

/*
 * The gameplay RNG state.  Every random() call advances it, so it is a stream,
 * not a value: nothing that has to be reproducible may read it.
 */
extern u32 seed;

/*
 * The world's identity.  Terrain noise and decoration are pure functions of a
 * coordinate and this, so a chunk generates the same blocks whenever and in
 * whatever order it is asked for -- which is what lets an unmodified chunk be
 * evicted and regenerated later instead of stored.  It is written once when a
 * world is created and never again.
 */
extern u32 world_seed;

u32 random(u32 limit);

float min(float a, float b);

float max(float a, float b);

int floor(float v);

float tanf(float angle);

float * at(Vector3 *v, int i);

int * ati(Vector3i *v, int i);

Vector3 add(Vector3 a, Vector3 b);

Vector3i addi(Vector3i a, Vector3i b);

Vector3 mul(Vector3 a, float b);

Vector3 div(Vector3 a, float b);

Vector3i divToInt(Vector3 a, float b);

float dot(Vector3 a, Vector3 b);

Vector3 rotateX(Vector3 v, float angle);

Vector3 rotateY(Vector3 v, float angle);

void rayStepAxis(float origin, float direction, int block, float *t, Vector3i *step, int axis);

#endif /* MATH_H */
