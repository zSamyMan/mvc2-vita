/*
 * nl_math.c  libnaomi's nlAtan256 (src/lib/libnaomi/nlmatha.src, SH-4 assembler), in C
 *
 * The angle of the vector (x, y) in 256ths of a turn. The smaller of |x| and |y| over the larger looks up the
 * first eighth of a turn in nlAtan256tbl; the octant (y negative, x negative, |y| > |x|) gives the base angle to
 * add and whether the table's angle is taken away. Like the assembler, the result is not masked (256 can come out
 * of the fourth octant) and 0/0 reads the table's first entry. The game aims with it (o_sub1.c: Spider-Man's
 * Maximum Spider and Web Swing, among others); before this the port had a stub that returned 0.
 */

#include <stdint.h>

/* the angle of each ratio 0/128 to 128/128 */
static const signed char nlAtan256tbl[129] = {
    0,  0,  1,  1,  1,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  5,  5,  5,  6,  6,  6,  7,  7,  7,  8,  8,
    8,  8,  9,  9,  9,  10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 15, 15, 15, 15,
    16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22,
    22, 23, 23, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 28,
    28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 30, 30, 30, 30, 30, 31, 31, 31, 31, 31, 31, 32, 32, 32, 32,
};

/* for each octant (bit 2: y < 0, bit 1: |y| > |x|, bit 0: x < 0), the base angle doubled, and 1 where the
   table's angle is taken away */
static const int16_t octant[8] = { 0x0000, 0x0101, 0x0081, 0x0080, 0x0201, 0x0100, 0x0180, 0x0181 };

int32_t nlAtan256(float y, float x) {
    int idx = 0, entry, t;
    float ratio;
    if (0.0f > y) {
        y = -y;
        idx |= 4;
    }
    if (0.0f > x) {
        x = -x;
        idx |= 1;
    }
    if (y > x) {
        ratio = x / y;
        idx |= 2;
    } else {
        ratio = x != 0.0f ? y / x : 0.0f;
    }
    /* ftrc of 128 * ratio + 0.5, its low byte (a ratio is at most 1: an index of at most 128) */
    t = nlAtan256tbl[(uint8_t)(int32_t)(128.0f * ratio + 0.5f) & 0xFF];
    entry = octant[idx];
    return (entry & 1 ? -t : t) + (entry >> 1);
}
