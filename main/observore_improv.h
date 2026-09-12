#pragma once

#include <stdbool.h>

/* Improv Wi-Fi over serial: https://www.improv-wifi.com/serial/
 *
 * Lets the browser that just flashed the device also put it on a network,
 * over the same USB connection, without the user ever reading a password off
 * a serial log or joining a setup SoftAP.  That is the whole point: the
 * console password is printed on the log and nowhere else, so a device flashed
 * for somebody else was a device they could not configure.
 *
 * Serial only, deliberately.  Improv also defines a BLE transport, and it is
 * the wrong choice here: it would make a counter-surveillance detector
 * advertise, which contradicts what the device is for.  This one only ever
 * answers on a wire somebody has physically plugged in.
 */
void observore_improv_init(void);

/* True once credentials have arrived this way and the uplink came up, which
 * is what the protocol calls "provisioned". */
bool observore_improv_provisioned(void);
