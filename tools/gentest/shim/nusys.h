/*
 * Enough of nusys.h to compile world.c, noise.c and math.c on a development
 * machine.  Mine64 cannot be run or observed from a host, but world generation
 * is pure arithmetic over these types -- no RSP, no RDP, no cart -- so the one
 * property the streaming design rests on can be checked here instead of by
 * bisecting on the console.
 */
#ifndef GENTEST_NUSYS_H
#define GENTEST_NUSYS_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Supplied by the SDK's gu.h on the console; math.c wants it for rotations. */
#define M_DTOR 0.017453292519943295

float sinf(float angle);
float cosf(float angle);
float sqrtf(float value);

/* The harness drives this so a world can be generated from a chosen seed. */
typedef u64 OSTime;
u64 osGetTime(void);
void gentestSetTime(u64 time);

#endif /* GENTEST_NUSYS_H */
