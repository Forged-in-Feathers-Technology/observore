#include "observore_rotary.h"

#include "sdkconfig.h"

#if CONFIG_OBSERVORE_ROTARY

#include "driver/pulse_cnt.h"
#include "esp_log.h"

static const char *TAG = "observore.rotary";

#define A_GPIO ((int)CONFIG_OBSERVORE_ROTARY_A_GPIO)
#define B_GPIO ((int)CONFIG_OBSERVORE_ROTARY_B_GPIO)

/* Both edges of both channels are counted, which is four counts for one
 * click of a detented encoder. Dividing here rather than in the caller
 * keeps the unit of the public API a detent, which is the thing a person
 * actually did. */
#define COUNTS_PER_DETENT 4

/* Wide enough that a knob spun hard between two reads cannot reach the end
 * and wrap. At four counts a detent and a UI that reads several times a
 * second, this is thousands of detents of headroom. */
#define LIMIT 10000

/* An encoder's contacts bounce, and a bounce on a quadrature pair is not
 * noise that averages out -- it is a count in the wrong direction. 1 us is
 * far below the fastest a hand can turn a knob and far above the bounce. */
#define GLITCH_NS 1000

static pcnt_unit_handle_t s_unit;
static int s_last;

void observore_rotary_init(void)
{
    pcnt_unit_config_t ucfg = {
        .high_limit = LIMIT,
        .low_limit  = -LIMIT,
    };
    if (pcnt_new_unit(&ucfg, &s_unit) != ESP_OK) {
        ESP_LOGW(TAG, "no PCNT unit available; knob disabled");
        s_unit = NULL;
        return;
    }

    pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = GLITCH_NS };
    pcnt_unit_set_glitch_filter(s_unit, &fcfg);

    /* Two channels watching the same pair from opposite sides: each counts
     * the edges of one pin and uses the other pin's level to decide which
     * way that edge was going. That is what makes it direction-aware, and
     * it is why a single channel would count a jiggle as travel. */
    pcnt_chan_config_t a = { .edge_gpio_num = A_GPIO, .level_gpio_num = B_GPIO };
    pcnt_chan_config_t b = { .edge_gpio_num = B_GPIO, .level_gpio_num = A_GPIO };
    pcnt_channel_handle_t ch_a = NULL, ch_b = NULL;
    if (pcnt_new_channel(s_unit, &a, &ch_a) != ESP_OK ||
        pcnt_new_channel(s_unit, &b, &ch_b) != ESP_OK) {
        ESP_LOGW(TAG, "could not claim gpio %d/%d; knob disabled",
                 A_GPIO, B_GPIO);
        pcnt_del_unit(s_unit);
        s_unit = NULL;
        return;
    }

    pcnt_channel_set_edge_action(ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    if (pcnt_unit_enable(s_unit) != ESP_OK ||
        pcnt_unit_clear_count(s_unit) != ESP_OK ||
        pcnt_unit_start(s_unit) != ESP_OK) {
        ESP_LOGW(TAG, "PCNT would not start; knob disabled");
        s_unit = NULL;
        return;
    }
    s_last = 0;
    ESP_LOGI(TAG, "knob on gpio %d/%d, %d counts a detent",
             A_GPIO, B_GPIO, COUNTS_PER_DETENT);
}

int observore_rotary_take(void)
{
    if (!s_unit) {
        return 0;
    }
    int now = 0;
    if (pcnt_unit_get_count(s_unit, &now) != ESP_OK) {
        return 0;
    }

    int moved = now - s_last;
    int detents = moved / COUNTS_PER_DETENT;
    /* Only whole detents are consumed. The remainder stays in the counter by
     * moving s_last forward just the amount that was reported, so a knob
     * left between clicks does not lose the counts it has already made. */
    s_last += detents * COUNTS_PER_DETENT;
    return detents;
}

#else  /* !CONFIG_OBSERVORE_ROTARY */

/* Stubs, so that every board links whether or not it has a knob. Nine of
 * eleven boards once failed to link because an #else half of a module like
 * this one was missing two functions. */
void observore_rotary_init(void) { }
int  observore_rotary_take(void) { return 0; }

#endif
