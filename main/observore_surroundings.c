#include "observore_surroundings.h"

#include <string.h>

/* Thirty-two is generous for a home and enough of a sample anywhere else:
 * the question is what fraction came back, and a fraction does not get more
 * accurate by counting every access point in a city block. */
#define MAX_APS 32

typedef struct {
    uint32_t hash;
    int64_t  last_us;
    bool     used;
} ap_t;

static ap_t s_recent[MAX_APS];

typedef struct {
    uint32_t hash;
    bool     used;
    bool     seen_again;
} marked_t;

static marked_t s_marked[MAX_APS];
static size_t   s_marked_count;

void observore_surroundings_reset(void)
{
    memset(s_recent, 0, sizeof(s_recent));
    memset(s_marked, 0, sizeof(s_marked));
    s_marked_count = 0;
}

void observore_surroundings_note(uint32_t hash, int64_t now_us)
{
    if (hash == 0) {
        return;
    }
    /* Anything already marked that is heard again is one fewer thing that
     * changed about this place. */
    for (size_t i = 0; i < MAX_APS; i++) {
        if (s_marked[i].used && s_marked[i].hash == hash) {
            s_marked[i].seen_again = true;
            break;
        }
    }

    size_t free_slot = MAX_APS, oldest = 0;
    for (size_t i = 0; i < MAX_APS; i++) {
        if (s_recent[i].used && s_recent[i].hash == hash) {
            s_recent[i].last_us = now_us;
            return;
        }
        if (!s_recent[i].used && free_slot == MAX_APS) {
            free_slot = i;
        }
        if (s_recent[i].used && s_recent[i].last_us < s_recent[oldest].last_us) {
            oldest = i;
        }
    }
    size_t slot = (free_slot != MAX_APS) ? free_slot : oldest;
    s_recent[slot].hash    = hash;
    s_recent[slot].last_us = now_us;
    s_recent[slot].used    = true;
}

void observore_surroundings_mark(int64_t now_us, int64_t max_age_us)
{
    memset(s_marked, 0, sizeof(s_marked));
    s_marked_count = 0;
    for (size_t i = 0; i < MAX_APS; i++) {
        if (!s_recent[i].used || now_us - s_recent[i].last_us > max_age_us) {
            continue;
        }
        s_marked[s_marked_count].hash       = s_recent[i].hash;
        s_marked[s_marked_count].used       = true;
        s_marked[s_marked_count].seen_again = false;
        s_marked_count++;
    }
}

int observore_surroundings_overlap_pct(void)
{
    if (s_marked_count == 0) {
        return -1;
    }
    size_t seen = 0;
    for (size_t i = 0; i < s_marked_count; i++) {
        if (s_marked[i].seen_again) {
            seen++;
        }
    }
    return (int)((seen * 100) / s_marked_count);
}

size_t observore_surroundings_marked(void)
{
    return s_marked_count;
}
