#include "xoroshiro.h"

static inline u64 rotl(const u64 x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static inline void seed(struct state_array *obj, u64 s0, u64 s1)
{
    obj->array[0] = s0;
    obj->array[1] = s1;
}

u64 xoro_next(struct state_array *obj)  // 亂數
{
    const u64 s0 = obj->array[0];
    u64 s1 = obj->array[1];
    const u64 result = rotl(s0 + s1, 24) + s0;

    s1 ^= s0;
    obj->array[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16);
    obj->array[1] = rotl(s1, 37);

    return result;
}

void xoro_jump(struct state_array *obj)
{
    static const u64 JUMP[] = {0xdf900294d8f554a5, 0x170865df4b3201fc};

    u64 s0 = 0;
    u64 s1 = 0;
    int i, b;
    for (i = 0; i < sizeof(JUMP) / sizeof(*JUMP); i++) {
        for (b = 0; b < 64; b++) {
            if (JUMP[i] & (u64) (1) << b) {
                s0 ^= obj->array[0];
                s1 ^= obj->array[1];
            }
            xoro_next(obj);
        }
    }

    obj->array[0] = s0;
    obj->array[1] = s1;
}

void xoro_init(struct state_array *obj)
{
    seed(obj, 314159265, 1618033989);
}
