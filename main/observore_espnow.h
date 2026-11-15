#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

/* The census exchange on the radio.
 *
 * This is the one file that touches ESP-NOW, and it never switches Wi-Fi
 * modes: main.c does that, because the mode a node is in decides what is safe
 * to do to the radio, and main.c is the only place that knows (the project's
 * rule for anything that moves the radio between patrol, console and uplink).
 * Everything here is a step main.c asks for: whether a window should open,
 * open it, take one step, close it.
 *
 * ESP-NOW exists only between open and close. Outside a window nothing has
 * initialised it, so nothing can deliver a frame to this node at all, and a
 * stranger cannot even feed it garbage to be counted. That is the property the
 * rest of the design leans on.
 *
 * The exchange is a build option, off in every board profile. Without it this
 * header still declares the two functions below, so main.c calls them in every
 * build and the option needs no #if at the call site.
 */

/* Whether this build has the exchange. A constant, like
 * observore_ble_can_warn(): it says what the image contains, not what the owner
 * has chosen. */
bool observore_espnow_available(void);

/* Call once, after observore_meshkey_init(): the exchange derives its key from
 * the household secret that call loads. Empty in a build without the exchange. */
void observore_espnow_init(void);

/* The console's side of the exchange. Everything from here to the #if below is
 * declared in every build and answers sensibly without the option, so the web
 * layer calls it with no #if at the call site, as it calls
 * observore_ble_can_warn().
 *
 * The two sizes below equal OBSERVORE_CX_FRIENDS_MAX and
 * OBSERVORE_CX_RESULT_TEXT_LEN; observore_espnow.c asserts it under the option,
 * which keeps this header free of the option's types. */
#define OBSERVORE_ESPNOW_FRIENDS  8
#define OBSERVORE_ESPNOW_TEXT_LEN 224

/* The largest free block of internal memory a window needs to open; with less
 * it is skipped. Why this much is beside its use in observore_espnow.c. */
#define OBSERVORE_ESPNOW_MIN_HEAP 8192

/* What the console shows of the exchange, copied out under the exchange's lock
 * so a reader on the HTTP task sees a state that agrees with itself.
 *
 *   available     whether this build has the exchange at all. When false every
 *                 other field is zero and the console shows no controls.
 *   secret        whether a household secret is set. The secret itself, the key
 *                 derived from it and its check never leave observore_meshkey.c.
 *   mode, at_min, channel
 *                 what the owner set: 0 off, 1 listen, 2 exchange (the order of
 *                 observore_cx_mode_name()); the window time as minutes since
 *                 local midnight; the household channel.
 *   next_s        seconds until the next scheduled window, 0 inside its grace and
 *                 -1 when there will be none.
 *   blocked       why there will be none: "" when a window will come, otherwise
 *                 "off", "no household secret", "no clock", or "no memory at
 *                 boot" when the exchange's state could not be allocated.
 *   peer_entries, peer_max
 *                 identities held from the owner's other nodes, and the table's
 *                 size.
 *   forgot        why that table last emptied, as an observore_census_forget_t.
 *   have_last     whether a window has closed since boot. The result is kept in
 *                 RAM only: a board with no card forgets it at a reboot.
 *   last_*        the last window's result, as observore_cx_result_t has it.
 *                 last_share is true for exchange mode, last_interrupted for a
 *                 window cut short, last_secs how long it ran, last_left the
 *                 observore_mode_t the node was in before the window took it to
 *                 patrol, and last_closed_us the monotonic time it closed.
 *   last_text     the same result as one line, counts and node ids only.
 *   low_mem       the largest free internal block the last window found when it
 *                 was skipped for memory, against OBSERVORE_ESPNOW_MIN_HEAP; 0
 *                 when the last window to try did open. A board can be built
 *                 with the exchange and still never have the memory for it,
 *                 and without this the console would only ever say a window
 *                 is coming. */
typedef struct observore_espnow_status_t {
    bool     available;
    bool     secret;
    uint8_t  mode;
    uint16_t at_min;
    uint8_t  channel;
    long     next_s;
    const char *blocked;
    unsigned peer_entries;
    unsigned peer_max;
    uint8_t  forgot;
    bool     have_last;
    bool     last_share;
    bool     last_interrupted;
    uint8_t  last_secs;
    uint8_t  last_left;
    uint16_t last_sent;
    uint8_t  last_friends;
    uint16_t last_friend[OBSERVORE_ESPNOW_FRIENDS];
    uint16_t last_merged, last_bad_tag, last_day_skew, last_malformed, last_foreign;
    uint16_t low_mem;            /* in the padding before the int64_t: no growth */
    int64_t  last_closed_us;
    char     last_text[OBSERVORE_ESPNOW_TEXT_LEN];
} observore_espnow_status_t;

/* Fill `out` from live state. `now_us` is the monotonic time, for the schedule. */
void observore_espnow_status(int64_t now_us, observore_espnow_status_t *out);

/* Set the owner's mode, window time and channel; each argument may be NULL for
 * "unchanged". Every value given is validated before any is applied, so a
 * refusal changes nothing. Returns NULL on success, or the reason as a sentence
 * the console can show: a mode other than off is refused until a household
 * secret is set. A valid change is saved. */
const char *observore_espnow_configure(const char *mode, const char *at,
                                       const char *channel);

/* Ask for a window now. This only sets a flag, which the main loop takes in
 * observore_espnow_should_open() within about a second: switching the radio's
 * mode is main.c's alone to do, and a web handler must not block. Returns NULL
 * when the request was taken, or why not: the exchange is off, there is no
 * secret, there is no clock, or the last window closed under five minutes ago. */
const char *observore_espnow_request_now(int64_t now_us);

/* Empty the peer census, which is what the owner's other nodes reported; this
 * node's own census is kept. Returns NULL, or why it could not. */
const char *observore_espnow_forget(void);

/* The household secret was just set or cleared: empty what friends reported now,
 * under the right reason, rather than at the next window. */
void observore_espnow_secret_changed(void);

/* Whether a census window is open at `now_us`, and if so how many seconds are
 * left (rounded up, so it never reads 0 while the window is open) and whether
 * this node is sending in it rather than only listening. The one question the
 * screen and the LED ask, declared in every build and false without the option,
 * so neither needs an #if, as with observore_ble_can_warn(). Safe from any task:
 * it takes the exchange lock for a few instructions. `remaining_s` and `sharing`
 * may be NULL. */
bool observore_espnow_window(int64_t now_us, unsigned *remaining_s, bool *sharing);

#if CONFIG_OBSERVORE_MESH_CENSUS

/* Whether a window should open now, from live inputs: the clock, the owner's
 * mode and time, the household secret and any hand-opened request. The decision
 * itself is observore_cx_decide(); this gathers what it reads.
 *
 * Answering true for a scheduled window marks today's window as run before it
 * returns, so a window that then fails to open is not retried every pass of the
 * grace. Every reason for waiting is logged once when it first appears, never
 * once a pass. */
bool observore_espnow_should_open(int64_t now_us);

/* Open a window at `now_us`. `left_mode` is the observore_mode_t this node was
 * in before main.c moved it to patrol, kept for the result. The radio must
 * already be in patrol: an unassociated station is the one mode in which ESP-NOW
 * receive coexists stably with the BLE scan.
 *
 * Refused, logged and nothing initialised when the largest internal free block
 * is under OBSERVORE_CX_MIN_HEAP, no key can be derived, the band or the channel
 * cannot be set, or ESP-NOW will not start. Returns whether a window is open. */
bool observore_espnow_open(int64_t now_us, uint8_t left_mode);

/* One pass of an open window: take a bounded few frames from the receive queue,
 * authenticate and merge them, and send this node's round when one is due.
 * Returns false when the window should close, which is the caller's to do: the
 * time is up, the clock is lost, the secret is gone or the mode is set to off.
 * Call it from the main task only. */
bool observore_espnow_step(int64_t now_us);

/* Close the window, idempotently. ESP-NOW is deinitialised first, before
 * anything else can touch the radio, and the result is fixed and logged with the
 * main task's unused stack. `interrupted` is the caller's way of saying the
 * window ended for a reason other than the clock, such as the owner's gesture
 * or a mode change underneath it. */
void observore_espnow_close(int64_t now_us, bool interrupted);

/* Whether a window is open. Safe from any task. */
bool observore_espnow_is_open(void);

/* Keep windows from opening while another task owns the radio, as Improv does
 * for the length of a provisioning join. Taken under the exchange lock and
 * checked there by both the decision and the open, so once hold(true) has
 * returned either a window was already open (and is_open() says so) or none
 * will open until hold(false). A scheduled window held off is not marked as
 * run; it opens if the hold ends inside its grace. */
void observore_espnow_hold(bool hold);

/* Whether the next scheduled window opens within `lead_s` seconds, so the
 * caller can keep a long network visit from straddling it. False when there
 * will be none: off, no secret or no clock. */
bool observore_espnow_soon(int64_t now_us, int lead_s);

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
