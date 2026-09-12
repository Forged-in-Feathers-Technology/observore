#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Every persisted setting, in one list.
 *
 * It exists as an X-macro because the list was previously written out four
 * times -- once per owning module, and once more in the migration table -- with
 * nothing linking them.  Adding a setting therefore meant remembering an edit
 * in a file you were not otherwise touching, and forgetting it lost that
 * setting silently at the next namespace change.  That had already happened:
 * "ap_pass" was added with the generated console password and never reached the
 * migration table.  Now the migration is derived from this list, so it cannot
 * fall behind. */
#define OBSERVORE_NVS_KEYS(X)                    \
    X(MUTES,      "mutes",      OBSERVORE_NVS_BLOB) \
    X(STA_SSID,   "sta_ssid",   OBSERVORE_NVS_STR)  \
    X(STA_PASS,   "sta_pass",   OBSERVORE_NVS_STR)  \
    X(AP_PASS,    "ap_pass",    OBSERVORE_NVS_STR)  \
    X(GOTIFY_URL, "gotify_url", OBSERVORE_NVS_STR)  \
    X(GOTIFY_TOK, "gotify_tok", OBSERVORE_NVS_STR)

#define OBSERVORE_NVS_NAMESPACE "observore"

typedef enum {
    OBSERVORE_NVS_STR,
    OBSERVORE_NVS_BLOB,
} observore_nvs_type_t;

/* One key's worth of transfer.  `buf` is the destination on a read and the
 * source on a write; `len` is the capacity going into a read and the length of
 * a blob going into a write (ignored for strings). */
typedef struct {
    const char           *key;
    observore_nvs_type_t  type;
    void                 *buf;
    size_t                len;
    bool                  found;   /* set by read */
} observore_nvs_item_t;

/* Read several keys under one open.  A key that is absent leaves its buffer
 * untouched and reports found = false; that is not an error, since every
 * setting has a "not configured yet" state. */
esp_err_t observore_nvs_read(observore_nvs_item_t *items, size_t n);

/* Write several keys under ONE open and ONE commit.
 *
 * The batching is the point, not the tidiness: each commit is a flash write,
 * and settings that belong together -- an SSID and its password -- must land
 * together or a power cut can leave a network configured with the wrong
 * credential. */
esp_err_t observore_nvs_write(const observore_nvs_item_t *items, size_t n);

/* Copy anything left under the project's former name.  Call once, before any
 * module reads its settings. */
void observore_nvs_migrate(void);
