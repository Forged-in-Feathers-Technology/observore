#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The previous panic, read back from flash.
 *
 * A device that crashes at three in the morning prints a backtrace to a
 * serial port nobody is watching. That is not a hypothetical here: the 3.5"
 * CYD ended five runs in a panic, 3.5 to 9.5 hours apart, and two attempts to
 * be listening failed -- one of them by capturing the thing and then losing
 * the log. The run record could say a panic happened and never why.
 *
 * ESP-IDF writes a core dump to its own flash partition when the panic
 * handler runs. This reads the summary back on the next boot and puts it in
 * the log, so the common case needs no host tooling and no one sitting at a
 * terminal: the device says what killed it the last time, the moment it comes
 * back.
 *
 * The dump is kept, not erased, so it can also be read over the console and
 * pulled off with espcoredump.py for a full backtrace with line numbers. It
 * is erased only when asked.
 *
 * A board flashed before the coredump partition existed has nowhere to write
 * one; esp_core_dump finds its partition by name, so it simply never writes,
 * and everything here reports "none". That is the honest state rather than an
 * error, because an OTA cannot deliver a partition table.
 */

/* Read any stored dump and log a summary. Call once at boot, after the
 * partition table is available. */
void observore_crashlog_init(void);

/* Whether a valid core dump is stored. */
bool observore_crashlog_present(void);

/* A one-line summary for the console: the faulting task and program counter,
 * or an empty string when there is nothing stored. Returns the length. */
size_t observore_crashlog_line(char *out, size_t len);

/* Forget the stored dump. The only way it goes away. */
bool observore_crashlog_clear(void);
