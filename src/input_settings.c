/*
 * Dilemma Max - Einstellungen fuer Trackpad und Encoder (linke Haelfte, Central).
 *
 * Haelt die Werte, die im Menue unter Touch und Input stehen, und legt sie pro Ausgang in
 * den Settings ab. Wirksam werden sie an drei Stellen:
 *
 *  - Cursor- und Scroll-Geschwindigkeit und die Scroll-Richtung rechnet src/pad_tune.c in
 *    die Trackpad-Events ein, die ueber den Split-Link hier ankommen.
 *  - Tippen und Auslaufen entscheidet der Trackpad-Treiber auf der rechten Haelfte. Die
 *    beiden Schalter gehen deshalb mit dem uebrigen Abgleich dorthin
 *    (src/rgb_split_sync.c); die rechte Haelfte merkt sie sich nicht selbst.
 *  - Die Funktion der beiden Encoder liest src/knob.c bei jedem Dreh hier ab.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <string.h>

#include <dilemma_max/input.h>
#include <dilemma_max/os.h>
#include <zmk/event_manager.h>
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#endif
#include <dilemma_max/power.h>

LOG_MODULE_REGISTER(dilemma_max_input, CONFIG_ZMK_LOG_LEVEL);

#define SETTINGS_KEY "dilemma/input"

// Anzahl der Werte pro Eintrag
static const uint8_t option_count[DILEMMA_MAX_INPUT_ITEM_COUNT] = {
    [DILEMMA_MAX_INPUT_SPEED] = 5,  [DILEMMA_MAX_INPUT_SCROLL] = 5,
    [DILEMMA_MAX_INPUT_INVERT] = 2, [DILEMMA_MAX_INPUT_GLIDE] = 2,
    [DILEMMA_MAX_INPUT_TAP] = 2,
    [DILEMMA_MAX_INPUT_KNOB_LEFT] = DILEMMA_MAX_KNOB_COUNT,
    [DILEMMA_MAX_INPUT_KNOB_RIGHT] = DILEMMA_MAX_KNOB_COUNT,
};

// Standard: alles wie vor dem Menue
#define DEFAULT_VALUES                                                                             \
    {                                                                                              \
        [DILEMMA_MAX_INPUT_SPEED] = 2,  [DILEMMA_MAX_INPUT_SCROLL] = 2,                            \
        [DILEMMA_MAX_INPUT_INVERT] = 0, [DILEMMA_MAX_INPUT_GLIDE] = 1,                             \
        [DILEMMA_MAX_INPUT_TAP] = 1,    [DILEMMA_MAX_INPUT_KNOB_LEFT] = DILEMMA_MAX_KNOB_VOLUME,   \
        [DILEMMA_MAX_INPUT_KNOB_RIGHT] = DILEMMA_MAX_KNOB_SCROLL,                                  \
    }

// Pro Ausgang (Bluetooth-Profile und USB, siehe dilemma_max/os.h): die Werte wechseln mit ihm
static uint8_t saved[DILEMMA_MAX_OUTPUT_COUNT][DILEMMA_MAX_INPUT_ITEM_COUNT] = {
    [0 ... DILEMMA_MAX_OUTPUT_COUNT - 1] = DEFAULT_VALUES,
};

uint8_t dilemma_max_input_get(enum dilemma_max_input_item item) {
    return saved[dilemma_max_output_index()][item];
}

uint32_t dilemma_max_input_pad_options(void) {
    return DILEMMA_MAX_PAD_OPTIONS |
           (dilemma_max_input_get(DILEMMA_MAX_INPUT_TAP) ? DILEMMA_MAX_PAD_OPTION_TAP : 0) |
           (dilemma_max_input_get(DILEMMA_MAX_INPUT_GLIDE) ? DILEMMA_MAX_PAD_OPTION_GLIDE : 0);
}

static void save_work_cb(struct k_work *work) {
    settings_save_one(SETTINGS_KEY, saved, sizeof(saved));
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

void dilemma_max_input_cycle(enum dilemma_max_input_item item) {
    uint8_t *value = &saved[dilemma_max_output_index()][item];

    *value = (*value + 1) % option_count[item];
    k_work_reschedule(&save_work, K_SECONDS(5));

    if (item == DILEMMA_MAX_INPUT_TAP || item == DILEMMA_MAX_INPUT_GLIDE) {
        dilemma_max_split_sync(1);
    }
}

static int input_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    int rc;

    if (settings_name_next(name, NULL) != 0) {
        return -ENOENT;
    }

    if (len == sizeof(saved[0]) && len != sizeof(saved)) {
        // Stand vor der Trennung nach Ausgaengen: fuer alle uebernehmen
        uint8_t old[DILEMMA_MAX_INPUT_ITEM_COUNT];
        rc = read_cb(cb_arg, old, sizeof(old));
        if (rc < 0) {
            return rc;
        }
        for (int output = 0; output < DILEMMA_MAX_OUTPUT_COUNT; output++) {
            memcpy(saved[output], old, sizeof(old));
        }
    } else if (len == sizeof(saved)) {
        rc = read_cb(cb_arg, saved, sizeof(saved));
        if (rc < 0) {
            return rc;
        }
    } else {
        return -ENOENT;
    }

    for (int output = 0; output < DILEMMA_MAX_OUTPUT_COUNT; output++) {
        for (int i = 0; i < DILEMMA_MAX_INPUT_ITEM_COUNT; i++) {
            if (saved[output][i] >= option_count[i]) {
                saved[output][i] = 0;
            }
        }
    }

    return 0;
}

#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
// Anderer Ausgang, andere Werte: Tippen und Auslaufen muessen neu zur rechten Haelfte
static int input_output_listener(const zmk_event_t *eh) {
    dilemma_max_split_sync(1);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_input_output, input_output_listener);
ZMK_SUBSCRIPTION(dilemma_max_input_output, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dilemma_max_input_output, zmk_ble_active_profile_changed);
#endif

SETTINGS_STATIC_HANDLER_DEFINE(dilemma_max_input, SETTINGS_KEY, NULL, input_settings_set, NULL,
                               NULL);
