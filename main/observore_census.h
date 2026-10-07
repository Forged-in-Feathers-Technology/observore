#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* What is always around.
 *
 * A stationary node learns the furniture: the doorbell, the printer, the
 * neighbour's television, the household's own phones. The point of knowing
 * that is to stop reporting it -- see #132 -- but this is deliberately only
 * the learning half. Nothing here suppresses anything yet.
 *
 * That split is on purpose rather than for convenience. Twice this project
 * has silenced the thing it exists to notice: a baseline that blinded the
 * device outright (v0.9.1), and a baseline that quieted a Flipper Zero by
 * name for a week (#123). Both were acting on a judgement that had never been
 * watched. So membership is earned, persisted and reportable first, and
 * acting on it is a separate change against data that has had time to be
 * wrong in public.
 *
 * ## Days, not hours
 *
 * Membership is earned across separate days and never within one evening.
 * "Furniture" and "here right now" are different claims, and conflating them
 * is the mistake behind every revision of the follower class -- a device at
 * your elbow for three hours is interesting precisely because it is not
 * furniture.
 *
 * So each identity carries a bitmap of the days it was seen on. One bit per
 * day, the newest at bit 0, shifted along as days pass. Decay needs no code:
 * a day that falls off the end of the window is forgotten because the bit
 * went with it. A visitor's phone seen three days running stops being
 * household a fortnight after they leave.
 *
 * ## Identity is the caller's problem
 *
 * The id here is opaque. Whether a household member is an address, a
 * fingerprint, or something else is a question with real consequences -- an
 * address rotates, and a fingerprint identifies a *kind* of device, so two
 * identical handsets share one -- and it is not settled by this file. What is
 * settled here is the rule about days, which is the part that was worth
 * getting right before anything depends on it.
 */

#define OBSERVORE_CENSUS_MAX       64  /* identities tracked */
#define OBSERVORE_CENSUS_WINDOW    16  /* trailing days: the width of the mask */
#define OBSERVORE_CENSUS_MIN_DAYS   3  /* distinct days before it is furniture */

/* A day with no number.
 *
 * The device detects from the moment it powers on and only learns the time
 * when it next reaches a network, so early sightings genuinely have no date.
 * Counting them against day zero would hand membership to whatever happened
 * to be in the room during the first minute after every boot, which is the
 * opposite of earning it over days. They are dropped instead. */
#define OBSERVORE_CENSUS_NO_DAY   (-1)

/* The last day the record can hold, because `last_day` is sixteen bits: day
 * 65535 is in 2179. A date beyond it is refused rather than stored, since a
 * truncated day number is not a near miss -- it silently claims a different
 * date, and the window would then be measured from it. A test asking about
 * day 100000 is what turned that up, and the bound lives here rather than in
 * the test. */
#define OBSERVORE_CENSUS_DAY_MAX  65535

/* How many distinct addresses one identity is tracked under.
 *
 * Eight, to match OBSERVORE_MUTE_ADDRESS_LIMIT -- the count at which the mute
 * store retires a fingerprint rule for describing a kind of device rather than
 * one. Measuring the census against the same number means the two can be
 * compared directly when the suppression slice has to choose a threshold.
 *
 * What this measures, and what it does not. An identity seen under many
 * addresses is ambiguous: it is either one device rotating its address, which
 * is exactly what keying on the advert fingerprint is *for*, or several
 * identical devices sharing a shape. The census cannot tell those apart, and
 * neither can anything else here. What it can say is how often the ambiguous
 * case arises at all -- which is the fact that decides whether suppression
 * keyed on a fingerprint is viable, and the reason this is being measured
 * before anything is suppressed.
 */
#define OBSERVORE_CENSUS_ADDRS 8

typedef struct {
    uint32_t id;        /* opaque: whatever the caller uses to mean "this device" */
    uint16_t days;      /* bit 0 is `last_day`, bit n is n days before it */
    uint16_t last_day;  /* the day bit 0 refers to */
    /* Sixteen-bit hashes of the addresses this identity has been seen under.
     * Hashes rather than addresses: the question is how many, not which, and
     * six bytes apiece would quadruple the table for an answer nobody needs.
     * They do not decay -- a device that rotated through eight addresses last
     * month really has been seen under eight, and that is the measurement. */
    uint16_t addr[OBSERVORE_CENSUS_ADDRS];
    uint8_t  addr_n;    /* how many of addr[] are in use */
    uint8_t  addr_over; /* a further distinct address arrived with the set full */
} observore_census_entry_t;

/* The stored format. Version 1 was this table without the address set, and is
 * discarded rather than read: at eight bytes an entry against this one's
 * larger size, an old blob with an even number of entries divides evenly into
 * the new size and would restore as half as many entries of garbage. That is
 * the same trap the touch calibration hit, and the answer is the same one --
 * record which code wrote it rather than trying to recognise the shape. */
#define OBSERVORE_CENSUS_FORMAT 2

/* The local day a timestamp falls in, or OBSERVORE_CENSUS_NO_DAY.
 *
 * Local rather than UTC, because the rule is about evenings. An evening that
 * straddles midnight UTC would otherwise count as two days, and two days is
 * two thirds of the way to being furniture.
 *
 * The count runs from 2000-01-01 and is derived from the broken-down date
 * rather than from the timestamp, so it moves with the configured zone and
 * with daylight saving. */
int observore_census_day(time_t when);

/* Same, from a date already broken down. Separated out so the day arithmetic
 * can be tested without a timezone in the way. */
int observore_census_day_from_tm(const struct tm *lt);

void observore_census_init(void);

/* Record that `id` was seen on `day`, at address `mac` (six bytes, or NULL
 * when there is no address to attribute it to). Repeat sightings on the same
 * day at the same address are the normal case and cost nothing. */
void observore_census_note(uint32_t id, int day, const uint8_t *mac);

/* How many distinct addresses `id` has been seen under, saturating at
 * OBSERVORE_CENSUS_ADDRS. `over`, if given, is set when more arrived after
 * the set was full -- so "8" and "8 and counting" are distinguishable, which
 * matters when the number is being compared against a limit. */
int observore_census_addresses(uint32_t id, bool *over);

/* Whether `id` is furniture as of `day`: seen on at least
 * OBSERVORE_CENSUS_MIN_DAYS distinct days inside the trailing window. */
bool observore_census_is_household(uint32_t id, int day);

/* How many distinct days inside the window `id` has been seen on, 0 if it is
 * not known at all. For the console, and for seeing the rule work before
 * anything acts on it. */
int observore_census_days_seen(uint32_t id, int day);

/* How many identities are household as of `day`, and how many are tracked at
 * all. Either pointer may be NULL. */
void observore_census_counts(int day, int *household, int *tracked);

/* The address spread, in three buckets: identities seen under one address,
 * under two to seven, and at the eight-address ceiling.
 *
 * This is the shape of the measurement that decides whether suppression keyed
 * on an advert fingerprint is viable at all, so it belongs somewhere a person
 * can read without a browser and without a password. If most identities sit
 * in `one`, a fingerprint names a device. If many sit in `many`, a fingerprint
 * names a population, and quieting one would quiet the lot.
 *
 * Any pointer may be NULL. */
void observore_census_addr_spread(int *one, int *few, int *many);

/* The table, for persistence and for the console. Returns the number of
 * entries and points `out` at them. */
size_t observore_census_entries(const observore_census_entry_t **out);

/* Serialise the table, header and all, into `out`. Returns the number of
 * bytes the blob needs; with `out` NULL or `cap` too small it writes nothing
 * and returns that size, so a caller can ask first. */
size_t observore_census_blob(void *out, size_t cap);

/* Replace the table wholesale, from a blob previously saved. A blob whose
 * header does not say this code wrote it, or whose length disagrees with its
 * own header, is refused: restoring half an entry is worse than starting
 * empty. Returns false if the blob was rejected. */
bool observore_census_restore(const void *blob, size_t len);
