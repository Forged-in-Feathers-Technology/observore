#include "observore_battery.h"

#include <stddef.h>

#ifndef OBSERVORE_HOST_TEST
#include "sdkconfig.h"
#endif

/* The curve, which needs no hardware and is therefore testable.
 *
 * Millivolts against percent for a single lithium-polymer cell at rest.
 * Deliberately coarse: the point is to tell "most of the evening left" from
 * "about to stop", and a curve pretending to one-percent accuracy from one
 * ADC reading would be inventing precision. The flat middle is why this is a
 * table at all -- between 3.9 and 3.7 volts a linear reading is wildly
 * optimistic, and that span is most of the discharge. */
static const struct { int mv; int pct; } CURVE[] = {
    {4200, 100}, {4100,  94}, {4000,  85}, {3950,  76}, {3900,  66},
    {3850,  55}, {3800,  44}, {3780,  35}, {3760,  27}, {3730,  20},
    {3700,  14}, {3650,   9}, {3600,   5}, {3500,   2}, {3300,   0},
};

int observore_battery_pct_from_mv(int mv)
{
    if (mv <= 0) {
        return -1;
    }
    if (mv >= CURVE[0].mv) {
        return 100;
    }
    size_t last = sizeof(CURVE) / sizeof(CURVE[0]) - 1;
    if (mv <= CURVE[last].mv) {
        return 0;
    }
    for (size_t i = 1; i <= last; i++) {
        if (mv >= CURVE[i].mv) {
            /* Straight line between the two points either side, which is all
             * the resolution the table claims. */
            int span_mv  = CURVE[i - 1].mv  - CURVE[i].mv;
            int span_pct = CURVE[i - 1].pct - CURVE[i].pct;
            return CURVE[i].pct + ((mv - CURVE[i].mv) * span_pct) / span_mv;
        }
    }
    return 0;
}

#if CONFIG_OBSERVORE_BATTERY

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "observore.battery";

/* Waveshare's own figures: ADC1, 12-bit, 12 dB of attenuation so the range
 * reaches 3.3 V, and the cell arrives divided by three. */
#define BATTERY_DIVIDER 3

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_ready;

void observore_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC unit");
        return;
    }
    adc_oneshot_chan_cfg_t chan = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    if (adc_oneshot_config_channel(s_adc, CONFIG_OBSERVORE_BATTERY_ADC_CHANNEL,
                                   &chan) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC channel %d", CONFIG_OBSERVORE_BATTERY_ADC_CHANNEL);
        return;
    }
    /* Calibration turns a raw count into millivolts using the figures burnt
     * into this particular chip. Without it the reading is out by a few
     * percent, which matters more at the flat part of the curve than
     * anywhere else. */
    adc_cali_curve_fitting_config_t cal = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "uncalibrated; readings will be approximate");
        s_cali = NULL;
    }
    s_ready = true;
    int mv = observore_battery_mv();
    ESP_LOGI(TAG, "cell on ADC1 channel %d: %d mV (%d%%)",
             CONFIG_OBSERVORE_BATTERY_ADC_CHANNEL, mv,
             observore_battery_pct_from_mv(mv));
}

int observore_battery_mv(void)
{
    if (!s_ready) {
        return -1;
    }
    /* Four readings, because one ADC sample on a board with two radios is
     * noisy and this costs microseconds. */
    int total = 0, taken = 0;
    for (int i = 0; i < 4; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, CONFIG_OBSERVORE_BATTERY_ADC_CHANNEL,
                             &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            total += mv;
        } else {
            total += raw * 3300 / 4096;
        }
        taken++;
    }
    if (taken == 0) {
        return -1;
    }
    return (total / taken) * BATTERY_DIVIDER;
}

bool observore_battery_available(void) { return s_ready; }

#else

void observore_battery_init(void) {}
int observore_battery_mv(void) { return -1; }
bool observore_battery_available(void) { return false; }

#endif
