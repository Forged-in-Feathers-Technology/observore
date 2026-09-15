#include "observore_heapwatch.h"

#include <stdio.h>
#include <string.h>

static observore_heap_event_t s_ring[OBSERVORE_HEAPWATCH_EVENTS];
static size_t   s_head;      /* next slot to write */
static size_t   s_count;
static uint32_t s_reported_min;

void observore_heapwatch_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_head = 0;
    s_count = 0;
    s_reported_min = UINT32_MAX;
}

bool observore_heapwatch_note(uint32_t free_min, uint32_t largest,
                              uint32_t queued, const char *mode, int64_t now_us)
{
    if (free_min + OBSERVORE_HEAPWATCH_STEP >= s_reported_min) {
        return false;
    }
    s_reported_min = free_min;

    observore_heap_event_t *e = &s_ring[s_head];
    e->free_min = free_min;
    e->largest  = largest;
    e->queued   = queued;
    e->at_us    = now_us;
    snprintf(e->mode, sizeof(e->mode), "%s", mode ? mode : "?");

    s_head = (s_head + 1) % OBSERVORE_HEAPWATCH_EVENTS;
    if (s_count < OBSERVORE_HEAPWATCH_EVENTS) {
        s_count++;
    }
    return true;
}

size_t observore_heapwatch_events(observore_heap_event_t *out, size_t cap)
{
    size_t n = s_count < cap ? s_count : cap;
    /* Oldest first: the oldest retained entry sits at head when the ring is
     * full, and at zero before it has wrapped. */
    size_t start = (s_count == OBSERVORE_HEAPWATCH_EVENTS) ? s_head : 0;
    /* When asked for fewer than we hold, hand back the most recent ones. */
    size_t skip = s_count - n;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(start + skip + i) % OBSERVORE_HEAPWATCH_EVENTS];
    }
    return n;
}

const observore_heap_event_t *observore_heapwatch_latest(void)
{
    if (s_count == 0) {
        return NULL;
    }
    return &s_ring[(s_head + OBSERVORE_HEAPWATCH_EVENTS - 1) % OBSERVORE_HEAPWATCH_EVENTS];
}
