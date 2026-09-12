#pragma once

/* Settings live under one NVS namespace.  The project was called Argus before
 * it was called Observore, so a device flashed with an older build has its
 * mute rules, Wi-Fi credentials and notifier settings under the old name.
 * Renaming the namespace without moving the data would silently orphan all of
 * it -- the device would come up looking factory-fresh and quietly stop
 * suppressing everything its owner had taught it to ignore.
 *
 * Call once, before any module reads its settings. */
void observore_nvs_migrate(void);

#define OBSERVORE_NVS_NAMESPACE "observore"
