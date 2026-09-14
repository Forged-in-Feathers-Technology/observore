#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Reading and comparing firmware versions.
 *
 * Kept free of ESP-IDF so the comparison can be tested on the host. Deciding
 * that a remote build is newer than the running one is the step that would
 * otherwise only be exercised by cutting a release, and getting it wrong means
 * either offering an update that is actually a downgrade or never offering one
 * at all. */

typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
    /* True for a build made after the tag, which ESP-IDF stamps from
     * `git describe` as "v0.5.0-3-gce8e56e". Such a build is ahead of the tag
     * it names, not behind it. */
    bool     dev;
} observore_version_t;

/* Accepts "v0.5.0", "0.5.0" and "v0.5.0-3-gce8e56e". Returns false for
 * anything it cannot read, rather than guessing at a number. */
bool observore_version_parse(const char *text, observore_version_t *out);

/* Negative, zero or positive as `a` is older, the same as, or newer than `b`.
 * A dev build sorts after the tag it was cut from. */
int observore_version_cmp(const observore_version_t *a,
                          const observore_version_t *b);

/* Whether `candidate` is worth offering to a device running `running`.
 *
 * False when either version cannot be parsed. An update that cannot be
 * reasoned about is not offered: the failure mode of a wrong yes is a device
 * downgrading itself over the air. */
bool observore_version_is_newer(const char *candidate, const char *running);

/* Copy the string value of a top-level JSON field.
 *
 * Deliberately not a JSON parser. It reads one known key out of a small
 * document this project publishes itself, and returns false rather than
 * guessing when the shape is not what it expects. */
bool observore_json_string_field(const char *json, const char *key,
                                 char *out, size_t out_len);
