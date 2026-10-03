#include "session/iot_backoff.h"

uint32_t iot_backoff_next(uint32_t *current, uint32_t min_ms, uint32_t max_ms, uint32_t rnd)
{
    uint32_t b = *current;
    if (b < min_ms) {
        b = min_ms;
    }
    if (b > max_ms) {
        b = max_ms;
    }
    const uint32_t half = b / 2u;
    const uint32_t wait = half + (rnd % (b - half + 1u));
    *current = (b > max_ms / 2u) ? max_ms : b * 2u;
    return wait;
}
