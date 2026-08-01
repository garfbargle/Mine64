#include "math.h"
#include "graphics.h"

u32 seed;
u32 world_seed;

u32 random(u32 limit) {
  if (limit == 0) {
    return 0;
  }
  if (seed == 0) {
    seed = 0x6D2B79F5;
  }
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed % limit;
}

float min(float a, float b) {
  return a < b? a : b;
}

float max(float a, float b) {
  return a > b? a : b;
}

int floor(float v) {
  int i = v;
  if (i == v || v > 0) {
    return i;
  } else {
    return i - 1;
  }
}

float tanf(float angle) {
  return sinf(angle) / cosf(angle);
}

/*
 * Heading of a direction vector under the gameplay yaw convention, where
 * forward is (-sin yaw, -cos yaw); pass the raw direction and this handles
 * the negation.  Result is in [0, 360).
 *
 * A polynomial on the octant ratio, not a library call: the arc-tangent is
 * only ever needed to point an entity somewhere, a tenth of a degree is far
 * below what a 320-pixel viewport can show, and this costs one divide and
 * three multiplies where a correctly-rounded atan2f would cost a great deal
 * more on a machine with no hardware transcendentals.
 */
float directionYaw(float x, float z) {
  float ax = x < 0 ? -x : x;
  float az = z < 0 ? -z : z;
  float larger = max(ax, az);
  float ratio;
  float square;
  float angle;

  if (larger < 1e-6f) {
    return 0;
  }
  ratio = min(ax, az) / larger;
  square = ratio * ratio;
  /* Degrees directly; the constants are the usual minimax fit scaled by
     180/pi so no radian conversion survives into the caller. */
  angle = ((-2.66406f * square + 9.12803f) * square - 18.77136f) *
    square * ratio + 57.29578f * ratio;
  if (ax > az) {
    angle = 90.f - angle;
  }
  /* Fold the octant back into a full turn measured from -Z. */
  if (z > 0) {
    angle = 180.f - angle;
  }
  if (x > 0) {
    angle = 360.f - angle;
  }
  return angle >= 360.f ? angle - 360.f : angle;
}

float wrapDegrees(float angle) {
  while (angle >= 180.f) {
    angle -= 360.f;
  }
  while (angle < -180.f) {
    angle += 360.f;
  }
  return angle;
}

float approachAngle(float from, float to, float step) {
  float difference = wrapDegrees(to - from);

  if (difference > step) {
    difference = step;
  } else if (difference < -step) {
    difference = -step;
  }
  from += difference;
  while (from >= 360.f) {
    from -= 360.f;
  }
  while (from < 0) {
    from += 360.f;
  }
  return from;
}

float * at(Vector3 *v, int i) {
  switch (i) {
    case 0:
      return &(v->x);
    case 1:
      return &(v->y);
    case 2:
      return &(v->z);
  }
  return 0;
}

int * ati(Vector3i *v, int i) {
  switch (i) {
    case 0:
      return &(v->x);
    case 1:
      return &(v->y);
    case 2:
      return &(v->z);
  }
  return 0;
}

Vector3 add(Vector3 a, Vector3 b) {
  Vector3 out = {a.x + b.x, a.y + b.y, a.z + b.z};
  return out;
}

Vector3i addi(Vector3i a, Vector3i b) {
  Vector3i out = {a.x + b.x, a.y + b.y, a.z + b.z};
  return out;
}

Vector3 mul(Vector3 a, float b) {
  Vector3 out = {a.x * b, a.y * b, a.z * b};
  return out;
}

Vector3 div(Vector3 a, float b) {
  Vector3 out = {a.x / b, a.y / b, a.z / b};
  return out;
}

Vector3i divToInt(Vector3 a, float b) {
  /* C's float-to-int conversion truncates toward zero.  Grid cells instead
   * use floor on both sides of the origin: -0.1 belongs to cell -1, not 0.
   * The collision DDA relies on that invariant; truncation made a negative
   * sweep start one cell ahead, yielding a negative hit time and expanding
   * the resolver's next sweep beyond the current frame. */
  Vector3i out = {floor(a.x / b), floor(a.y / b), floor(a.z / b)};
  return out;
}

float dot(Vector3 a, Vector3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 rotateX(Vector3 v, float angle) {
  Vector3 out = {
    v.x,
    v.y * cosf(angle * M_DTOR) - v.z * sinf(angle * M_DTOR),
    v.y * sinf(angle * M_DTOR) + v.z * cosf(angle * M_DTOR)
  };
  return out;
}

Vector3 rotateY(Vector3 v, float angle) {
  Vector3 out = {
    v.x * cosf(angle * M_DTOR) - v.z * sinf(angle * M_DTOR),
    v.y, 
    v.x * sinf(angle * M_DTOR) + v.z * cosf(angle * M_DTOR)
  };
  return out;
}

void rayStepAxis(float origin, float direction, int block, float *t, Vector3i *step, int axis) {
  float nt;
  if (direction < 0) {
    nt = (block * BLOCK_SIZE - origin) / direction;
    if (nt < *t) {
      *t = nt;

      *ati(step, axis) = -1;
      *ati(step, (axis + 1) % 3) = 0;
      *ati(step, (axis + 2) % 3) = 0;
    }
  } else if (direction > 0) {
    nt = ((block + 1) * BLOCK_SIZE - origin) / direction;
    if (nt < *t) {
      *t = nt;

      *ati(step, axis) = 1;
      *ati(step, (axis + 1) % 3) = 0;
      *ati(step, (axis + 2) % 3) = 0;
    }
  }
}
