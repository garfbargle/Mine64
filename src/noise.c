#include "noise.h"
#include "math.h"

// From https://gist.github.com/nowl/828013

/*
 * const u8, not int: every value fits a byte, so the table is 256 bytes of
 * read-only data instead of 1 KiB of writable .data.  hashAt is the innermost
 * operation of all terrain generation and the accesses are effectively
 * random, so the smaller footprint is a quarter of the D-cache pressure of
 * the wider table it replaces.
 */
static const u8 hash[] = {208,34,231,213,32,248,233,56,161,78,24,140,71,48,140,254,245,255,247,247,40,
                     185,248,251,245,28,124,204,204,76,36,1,107,28,234,163,202,224,245,128,167,204,
                     9,92,217,54,239,174,173,102,193,189,190,121,100,108,167,44,43,77,180,204,8,81,
                     70,223,11,38,24,254,210,210,177,32,81,195,243,125,8,169,112,32,97,53,195,13,
                     203,9,47,104,125,117,114,124,165,203,181,235,193,206,70,180,174,0,167,181,41,
                     164,30,116,127,198,245,146,87,224,149,206,57,4,192,210,65,210,129,240,178,105,
                     228,108,245,148,140,40,35,195,38,58,65,207,215,253,65,85,208,76,62,3,237,55,89,
                     232,50,217,64,244,157,199,121,252,90,17,212,203,149,152,140,187,234,177,73,174,
                     193,100,192,143,97,53,145,135,19,103,13,90,135,151,199,91,239,247,33,39,145,
                     101,120,99,3,186,86,99,41,237,203,111,79,220,135,158,42,30,154,120,67,87,167,
                     135,176,183,191,253,115,184,21,233,58,129,233,142,39,128,211,118,137,139,255,
                     114,20,218,113,154,27,127,246,250,1,8,198,250,209,92,222,173,21,88,102,219};

static int hashAt(int value)
{
    /* Two's complement makes the mask exactly the old "modulo, then fix the
       sign" pair: both reduce to the value's low eight bits. */
    return hash[value & 255];
}

/*
 * Cell lookup must floor, not truncate toward zero.  A cast rounds -0.3 to 0,
 * which hands smooth_inter a fraction of -0.3 -- outside the 0..1 the curve is
 * shaped for, so it *extrapolates*, and terrain sampled at fractional negative
 * coordinates turns into bands of garbage.  The fixed world never noticed
 * because its sample offsets kept 2D inputs positive; a streaming world walks
 * straight past them.  (The 3D cave fields already sampled negative x/z
 * through this bug, so flooring changes cave layout for a given seed.  Saves
 * carry blocks, so nothing existing moves.)
 */
static int floorToInt(float value)
{
    int truncated = (int) value;
    return value < (float) truncated ? truncated - 1 : truncated;
}

float lin_inter(float x, float y, float s)
{
    return x + s * (y-x);
}

float smooth_inter(float x, float y, float s)
{
    return lin_inter(x, y, s * s * (3-2*s));
}

/*
 * These sample world_seed, never the gameplay RNG.  They used to read `seed`,
 * which random() advances on every call -- so the noise field these describe
 * silently became a different field partway through generating a world.
 *
 * The corner hashes share their outer links: the four corners of a 2D cell
 * lie on two rows, so the row hash `hashAt(y + seed)` is computed once per
 * row rather than once per corner, and the x cubic is shared between the two
 * lerps it feeds.  Algebraically identical to hashing every corner from
 * scratch -- the host harness checks worlds are byte-identical -- but six
 * table lookups per sample instead of eight.
 */
float noise2d(float x, float y)
{
    int x_int = floorToInt(x);
    int y_int = floorToInt(y);
    float x_frac = x - x_int;
    float y_frac = y - y_int;
    int row0 = hashAt((int)((u32)y_int + world_seed));
    int row1 = hashAt((int)((u32)(y_int + 1) + world_seed));
    int s = hashAt(row0 + x_int);
    int t = hashAt(row0 + x_int + 1);
    int u = hashAt(row1 + x_int);
    int v = hashAt(row1 + x_int + 1);
    float x_s = x_frac * x_frac * (3 - 2 * x_frac);
    float low = lin_inter(s, t, x_s);
    float high = lin_inter(u, v, x_s);
    return smooth_inter(low, high, y_frac);
}

float perlin2d(float x, float y, float freq, int depth)
{
    float xa = x*freq;
    float ya = y*freq;
    float amp = 1.0;
    float fin = 0;
    float div = 0.0;

    int i;
    for(i=0; i<depth; i++)
    {
        div += 256 * amp;
        fin += noise2d(xa, ya) * amp;
        amp /= 2;
        xa *= 2;
        ya *= 2;
    }

    return fin/div;
}

/*
 * The 3D sampler chains hashes z, then y, then x -- the same links the old
 * per-corner noise3 walked, kept so caves stay deterministic for a world
 * seed.  Its eight corners share two z hashes and four (z, y) prefixes, so
 * hoisting each level computes fourteen lookups where hashing every corner
 * independently cost twenty-four.  Same values, same field.
 */
static float noise3d(float x, float y, float z)
{
    int x_int = floorToInt(x);
    int y_int = floorToInt(y);
    int z_int = floorToInt(z);
    float x_frac = x - x_int;
    float y_frac = y - y_int;
    float z_frac = z - z_int;
    int plane0 = hashAt((int)((u32)z_int + world_seed));
    int plane1 = hashAt((int)((u32)(z_int + 1) + world_seed));
    int row00 = hashAt(plane0 + y_int);
    int row01 = hashAt(plane0 + y_int + 1);
    int row10 = hashAt(plane1 + y_int);
    int row11 = hashAt(plane1 + y_int + 1);
    float x_s = x_frac * x_frac * (3 - 2 * x_frac);
    float y_s = y_frac * y_frac * (3 - 2 * y_frac);
    float x00 = lin_inter(hashAt(row00 + x_int), hashAt(row00 + x_int + 1), x_s);
    float x10 = lin_inter(hashAt(row01 + x_int), hashAt(row01 + x_int + 1), x_s);
    float x01 = lin_inter(hashAt(row10 + x_int), hashAt(row10 + x_int + 1), x_s);
    float x11 = lin_inter(hashAt(row11 + x_int), hashAt(row11 + x_int + 1), x_s);
    float low = lin_inter(x00, x10, y_s);
    float high = lin_inter(x01, x11, y_s);

    return smooth_inter(low, high, z_frac);
}

float perlin3d(float x, float y, float z, float freq, int depth)
{
    float xa = x * freq;
    float ya = y * freq;
    float za = z * freq;
    float amp = 1.0;
    float fin = 0;
    float div = 0.0;
    int i;

    for (i = 0; i < depth; i++) {
        div += 256 * amp;
        fin += noise3d(xa, ya, za) * amp;
        amp /= 2;
        xa *= 2;
        ya *= 2;
        za *= 2;
    }

    return fin / div;
}
