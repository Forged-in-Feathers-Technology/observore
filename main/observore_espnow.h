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

/* Whether the next scheduled window opens within `lead_s` seconds, so the
 * caller can keep a long network visit from straddling it. False when there
 * will be none: off, no secret or no clock. */
bool observore_espnow_soon(int64_t now_us, int lead_s);

#endif /* CONFIG_OBSERVORE_MESH_CENSUS */
