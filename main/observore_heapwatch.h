#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Remembering when the internal heap ran low, not just how low.
 *
 * The low-water mark alone says a device nearly ran out of memory and nothing
 * about when or during what. Two overnight runs each ended with a number and no
 * lead: the moment it happened was logged, to a serial port with nobody on it.
 * This keeps the last few drops on the device so they can be read back over
 * the network the next morning.
 *
 * Kept free of ESP-IDF so the ring can be tested on the host; the caller passes
 * in the measurements. */

/* The pool every allocation in this firmware actually draws from.
 *
 * MALLOC_CAP_INTERNAL on its own includes regions that are 32-bit-access only
 * -- IRAM that cannot hold ordinary data -- so a "largest free block"
 * measured that way can be a block no malloc() will ever return. Every figure
 * this device reports about its heap was measured that way, and the figures
 * were used to judge whether allocations would fit. On the 3.5" CYD that read
 * 16,164 bytes free with a 10,240-byte largest block while the console's
 * 4,096-byte buffer could not be had at all.
 *
 * So the caps are named once, here, and mean "internal RAM that can hold
 * data". A number that cannot be compared against the allocation it is being
 * used to explain is worse than no number, because it sends the search
 * somewhere else -- which it has done at least twice in this project.
 *
 * Callers include esp_heap_caps.h; this header stays free of ESP-IDF so the
 * ring can be tested on the host. */
#define OBSERVORE_HEAP_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

#define OBSERVORE_HEAPWATCH_EVENTS 8

/* Only a drop this large past the last recorded one is a new event, so a slow
 * slide records a handful of milestones rather than every byte. */
#define OBSERVORE_HEAPWATCH_STEP 2048

typedef struct {
    uint32_t free_min;     /* the low-water mark at that moment */
    uint32_t largest;      /* largest free block at that moment */
    uint32_t queued;       /* notifications waiting to be sent */
    int64_t  at_us;        /* uptime */
    /* What the device was doing: patrol, uplink, console, or update -- the
     * last being a TLS session for a check or a download, which is the one
     * activity large enough to account for a dip on its own. Seven characters
     * plus a terminator, which "console" exactly fills. */
    char     mode[8];
} observore_heap_event_t;

void observore_heapwatch_init(void);

/* Record a reading. Returns true if it was a new low worth logging. */
bool observore_heapwatch_note(uint32_t free_min, uint32_t largest,
                              uint32_t queued, const char *mode, int64_t now_us);

/* Events oldest first. Returns how many are valid in `out`. */
size_t observore_heapwatch_events(observore_heap_event_t *out, size_t cap);

/* The most recent event, or NULL if the heap has never dropped. */
const observore_heap_event_t *observore_heapwatch_latest(void);
