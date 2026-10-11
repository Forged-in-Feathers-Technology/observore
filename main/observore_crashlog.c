#include "observore_crashlog.h"

#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_log.h"

static const char *TAG = "observore.crash";

static bool s_present;
static char s_line[96];

void observore_crashlog_init(void)
{
    s_present = false;
    s_line[0] = '\0';

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_image_check() != ESP_OK) {
        return;            /* nothing stored, or nowhere to store it */
    }
    s_present = true;

    esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
    if (!sum) {
        /* Said rather than skipped: "there is a crash report and I could not
         * read it" is a different fact from "there is no crash report", and
         * the second one would send somebody looking in the wrong place. */
        ESP_LOGW(TAG, "a core dump is stored but there was no memory to read "
                      "its summary; it is still in flash");
        snprintf(s_line, sizeof(s_line), "stored, unread (out of memory)");
        return;
    }

    if (esp_core_dump_get_summary(sum) != ESP_OK) {
        ESP_LOGW(TAG, "a core dump is stored but its summary could not be read");
        snprintf(s_line, sizeof(s_line), "stored, unreadable");
        free(sum);
        return;
    }

    snprintf(s_line, sizeof(s_line), "task %.16s, pc 0x%08" PRIx32,
             sum->exc_task, sum->exc_pc);
    ESP_LOGE(TAG, "the previous run panicked in task \"%.16s\" at pc 0x%08"
                  PRIx32, sum->exc_task, sum->exc_pc);

#if CONFIG_IDF_TARGET_ARCH_XTENSA
    /* Xtensa gives a decoded backtrace, which is the whole point: these
     * addresses go straight into addr2line against the matching .elf. */
    if (sum->exc_bt_info.depth > 0) {
        char bt[160];
        size_t at = 0;
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && at < sizeof(bt) - 12; i++) {
            int n = snprintf(bt + at, sizeof(bt) - at, "0x%08" PRIx32 " ",
                             sum->exc_bt_info.bt[i]);
            if (n <= 0) { break; }
            at += (size_t)n;
        }
        ESP_LOGE(TAG, "backtrace%s: %s",
                 sum->exc_bt_info.corrupted ? " (corrupted)" : "", bt);
    }
#else
    /* RISC-V stores a raw stack dump rather than a decoded backtrace, so the
     * useful part here is the trap cause; the stack needs espcoredump.py. */
    ESP_LOGE(TAG, "mcause 0x%08" PRIx32 " mtval 0x%08" PRIx32
                  " ra 0x%08" PRIx32 " sp 0x%08" PRIx32,
             sum->ex_info.mcause, sum->ex_info.mtval,
             sum->ex_info.ra, sum->ex_info.sp);
#endif

    ESP_LOGW(TAG, "the dump is kept in flash; read it with espcoredump.py or "
                  "clear it from the console");
    free(sum);
#endif
}

bool observore_crashlog_present(void)
{
    return s_present;
}

size_t observore_crashlog_line(char *out, size_t len)
{
    if (!out || len == 0) {
        return 0;
    }
    int n = snprintf(out, len, "%s", s_line);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return ((size_t)n >= len) ? len - 1 : (size_t)n;
}

bool observore_crashlog_clear(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_image_erase() != ESP_OK) {
        return false;
    }
    s_present = false;
    s_line[0] = '\0';
    return true;
#else
    return false;
#endif
}
