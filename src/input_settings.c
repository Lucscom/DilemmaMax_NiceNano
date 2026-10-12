/*
 * Dilemma Max - Einstellungen fuer Trackpad und Encoder (linke Haelfte, Central).
 *
 * Haelt die Werte, die im Menue unter Touch und Input stehen, und legt sie in den Settings
 * ab. Wirksam werden sie an drei Stellen:
 *
 *  - Cursor- und Scroll-Geschwindigkeit und die Scroll-Richtung rechnet src/pad_tune.c in
 *    die Trackpad-Events ein, die ueber den Split-Link hier ankommen.
 *  - Tippen und Auslaufen entscheidet der Trackpad-Treiber auf der rechten Haelfte. Die
 *    beiden Schalter gehen deshalb mit dem uebrigen Abgleich dorthin
 *    (src/rgb_split_sync.c); die rechte Haelfte merkt sie sich nicht selbst.
 *  - Die Funktion des linken Encoders steht in der Keymap: je ein Layer fuer Scrollen und
 *    Helligkeit, den dieser Code ein- und ausschaltet. Ohne Layer gilt die Lautstaerke.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dt-bindings/dilemma_max/layers.h>
#include <dilemma_max/input.h>
#include <dilemma_max/power.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(dilemma_max_input, CONFIG_ZMK_LOG_LEVEL);

#define SETTINGS_KEY "dilemma/input"

// Anzahl der Werte pro Eintrag
static const uint8_t option_count[DILEMMA_MAX_INPUT_ITEM_COUNT] = {
    [DILEMMA_MAX_INPUT_SPEED] = 5,  [DILEMMA_MAX_INPUT_SCROLL] = 5,
    [DILEMMA_MAX_INPUT_INVERT] = 2, [DILEMMA_MAX_INPUT_GLIDE] = 2,
    [DILEMMA_MAX_INPUT_TAP] = 2,    [DILEMMA_MAX_INPUT_KNOB] = 3,
};

// Standard: alles wie vor dem Menue
static uint8_t saved[DILEMMA_MAX_INPUT_ITEM_COUNT] = {
    [DILEMMA_MAX_INPUT_SPEED] = 2, [DILEMMA_MAX_INPUT_SCROLL] = 2,
    [DILEMMA_MAX_INPUT_INVERT] = 0, [DILEMMA_MAX_INPUT_GLIDE] = 1,
    [DILEMMA_MAX_INPUT_TAP] = 1,   [DILEMMA_MAX_INPUT_KNOB] = DILEMMA_MAX_KNOB_VOLUME,
};

uint8_t dilemma_max_input_get(enum dilemma_max_input_item item) { return saved[item]; }

uint32_t dilemma_max_input_pad_options(void) {
    return DILEMMA_MAX_PAD_OPTIONS |
           (saved[DILEMMA_MAX_INPUT_TAP] ? DILEMMA_MAX_PAD_OPTION_TAP : 0) |
           (saved[DILEMMA_MAX_INPUT_GLIDE] ? DILEMMA_MAX_PAD_OPTION_GLIDE : 0);
}

static void set_layer(uint8_t layer, bool active) {
    if (active == zmk_keymap_layer_active(layer)) {
        return;
    }
    if (active) {
        zmk_keymap_layer_activate(layer);
    } else {
        zmk_keymap_layer_deactivate(layer);
    }
}

// Layer-Wechsel loesen selbst Events aus, deshalb ueber die Workqueue
static void knob_work_cb(struct k_work *work) {
    uint8_t knob = saved[DILEMMA_MAX_INPUT_KNOB];

    set_layer(DM_LAYER_KNOB_SCROLL, knob == DILEMMA_MAX_KNOB_SCROLL);
    set_layer(DM_LAYER_KNOB_LIGHT, knob == DILEMMA_MAX_KNOB_LIGHT);
}

static K_WORK_DEFINE(knob_work, knob_work_cb);

static void save_work_cb(struct k_work *work) {
    settings_save_one(SETTINGS_KEY, saved, sizeof(saved));
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

void dilemma_max_input_cycle(enum dilemma_max_input_item item) {
    saved[item] = (saved[item] + 1) % option_count[item];
    k_work_reschedule(&save_work, K_SECONDS(5));

    if (item == DILEMMA_MAX_INPUT_KNOB) {
        k_work_submit(&knob_work);
    } else if (item == DILEMMA_MAX_INPUT_TAP || item == DILEMMA_MAX_INPUT_GLIDE) {
        dilemma_max_split_sync(1);
    }
}

static int input_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    if (settings_name_next(name, NULL) != 0 || len != sizeof(saved)) {
        return -ENOENT;
    }

    int rc = read_cb(cb_arg, saved, sizeof(saved));
    if (rc < 0) {
        return rc;
    }

    for (int i = 0; i < DILEMMA_MAX_INPUT_ITEM_COUNT; i++) {
        if (saved[i] >= option_count[i]) {
            saved[i] = 0;
        }
    }

    return 0;
}

static int input_settings_commit(void) {
    k_work_submit(&knob_work);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dilemma_max_input, SETTINGS_KEY, NULL, input_settings_set,
                               input_settings_commit, NULL);
