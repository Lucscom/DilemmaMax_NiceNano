/*
 * Dilemma Max - Mac oder Windows, pro Ausgang.
 *
 * Eine Tastatur erfaehrt nicht, welches Betriebssystem am anderen Ende laeuft, weder ueber
 * USB noch ueber Bluetooth. Deshalb wird es eingestellt (Menue, Eintrag OS) und pro Ausgang
 * gemerkt: jedes Bluetooth-Profil und USB hat seinen eigenen Wert. Wechselt man das Profil,
 * schaltet die Tastatur mit um.
 *
 * Was sich unterscheidet, steht nicht hier, sondern
 *  - in der Keymap: solange Windows gilt, ist der Layer DM_LAYER_WIN aktiv und ueberschreibt
 *    einzelne Tasten,
 *  - bei den Trackpad-Gesten: src/gesture_keys.c nimmt dann die Kuerzel aus keycodes-win.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <stdio.h>

#include <dt-bindings/dilemma_max/layers.h>
#include <dilemma_max/os.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(dilemma_max_os, CONFIG_ZMK_LOG_LEVEL);

#define SETTINGS_KEY "dilemma/os"
#define OUTPUT_USB ZMK_BLE_PROFILE_COUNT
#define OUTPUT_COUNT (ZMK_BLE_PROFILE_COUNT + 1)

// 0 = Mac, 1 = Windows
static uint8_t windows[OUTPUT_COUNT];

static int current_output(void) {
    struct zmk_endpoint_instance endpoint = zmk_endpoints_selected();

    if (endpoint.transport == ZMK_TRANSPORT_USB) {
        return OUTPUT_USB;
    }

    return CLAMP(zmk_ble_active_profile_index(), 0, ZMK_BLE_PROFILE_COUNT - 1);
}

bool dilemma_max_os_is_windows(void) { return windows[current_output()] != 0; }

void dilemma_max_os_output_name(char *name, size_t name_len) {
    int output = current_output();

    if (output == OUTPUT_USB) {
        snprintf(name, name_len, "USB");
    } else {
        snprintf(name, name_len, "BT %d", output + 1);
    }
}

// Layer-Wechsel loesen selbst Events aus, deshalb nicht aus einem Listener heraus
static void apply_work_cb(struct k_work *work) {
    bool want = dilemma_max_os_is_windows();

    if (want == zmk_keymap_layer_active(DM_LAYER_WIN)) {
        return;
    }

    LOG_DBG("Ausgang gilt als %s", want ? "Windows" : "Mac");
    if (want) {
        zmk_keymap_layer_activate(DM_LAYER_WIN);
    } else {
        zmk_keymap_layer_deactivate(DM_LAYER_WIN);
    }
}

static K_WORK_DEFINE(apply_work, apply_work_cb);

static void save_work_cb(struct k_work *work) {
    settings_save_one(SETTINGS_KEY, windows, sizeof(windows));
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

void dilemma_max_os_toggle(void) {
    int output = current_output();

    windows[output] = !windows[output];
    k_work_submit(&apply_work);
    k_work_reschedule(&save_work, K_SECONDS(5));
}

static int os_mode_listener(const zmk_event_t *eh) {
    k_work_submit(&apply_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_os_mode, os_mode_listener);
ZMK_SUBSCRIPTION(dilemma_max_os_mode, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dilemma_max_os_mode, zmk_ble_active_profile_changed);

static int os_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (settings_name_next(name, NULL) != 0 || len != sizeof(windows)) {
        return -ENOENT;
    }

    int rc = read_cb(cb_arg, windows, sizeof(windows));
    return rc < 0 ? rc : 0;
}

static int os_settings_commit(void) {
    k_work_submit(&apply_work);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dilemma_max_os, SETTINGS_KEY, NULL, os_settings_set,
                               os_settings_commit, NULL);
