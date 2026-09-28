/*
 * Dilemma Max - Trackpad-Gesten als Tastenkuerzel (Input-Processor, linke Haelfte).
 *
 * macOS nimmt von Fremdgeraeten kein Precision-Touchpad an, Gesten wie Space-Wechsel oder
 * Mission Control muessen also als Tastenkuerzel ankommen. Der maXTouch-Treiber auf der
 * rechten Haelfte erkennt die Geste und meldet einen eigenen Code
 * (dt-bindings/dilemma_max/gestures.h), dieser Processor faengt ihn im Input-Listener ab
 * und loest das im Devicetree zugeordnete Kuerzel aus.
 *
 * Warum keine Behaviours (zmk,input-processor-behaviors mit &kp): das liess beide Haelften
 * abstuerzen. Input-Processors laufen im Zephyr-Input-Thread, und dessen Stack hat auf dem
 * Central den Zephyr-Standard von 512 Bytes (ZMK erhoeht ihn nur auf Peripherals). Ein
 * Behaviour loest dort synchron die ganze Kette aus Keymap, HID-Report und Logging aus.
 * Hier wird im Input-Thread nur der Keycode in eine Queue gelegt; Druecken und Loslassen
 * laufen als keycode_state_changed-Events auf der System-Workqueue, wo ZMK auch sonst
 * Tasten verarbeitet. Keine Tastenposition, kein Behaviour, keine Keymap.
 */

#define DT_DRV_COMPAT dilemma_input_processor_gesture_keys

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>

#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// So lange bleibt das Kuerzel gedrueckt: Druecken und Loslassen im selben Moment kommen
// als zwei HID-Reports direkt hintereinander an, manche Hosts werten das nicht aus.
#define GK_TAP_MS 30

struct gk_config {
    size_t size;
    const uint16_t *codes;
    const uint32_t *keycodes;
};

K_MSGQ_DEFINE(gk_msgq, sizeof(uint32_t), 8, 4);

static uint32_t gk_held;
static bool gk_holding;

static void gk_press_cb(struct k_work *work);
static K_WORK_DEFINE(gk_press_work, gk_press_cb);

static void gk_release_cb(struct k_work *work) {
    raise_zmk_keycode_state_changed_from_encoded(gk_held, false, k_uptime_get());
    gk_holding = false;
    // Weitere Gesten, die waehrenddessen ankamen, der Reihe nach abarbeiten
    if (k_msgq_num_used_get(&gk_msgq) > 0) {
        k_work_submit(&gk_press_work);
    }
}
static K_WORK_DELAYABLE_DEFINE(gk_release_work, gk_release_cb);

static void gk_press_cb(struct k_work *work) {
    if (gk_holding || k_msgq_get(&gk_msgq, &gk_held, K_NO_WAIT) != 0) {
        return;
    }
    gk_holding = true;
    LOG_DBG("gesture key 0x%08x", gk_held);
    raise_zmk_keycode_state_changed_from_encoded(gk_held, true, k_uptime_get());
    k_work_schedule(&gk_release_work, K_MSEC(GK_TAP_MS));
}

static int gk_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *state) {
    const struct gk_config *cfg = dev->config;

    if (event->type != INPUT_EV_KEY) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    for (size_t i = 0; i < cfg->size; i++) {
        if (cfg->codes[i] != event->code) {
            continue;
        }
        // Der Treiber meldet Druecken + Loslassen; das Kuerzel wird beim Druecken als Ganzes
        // ausgeloest, das Loslassen nur verschluckt.
        if (event->value) {
            if (k_msgq_put(&gk_msgq, &cfg->keycodes[i], K_NO_WAIT) != 0) {
                LOG_WRN("gesture key queue full, dropping 0x%x", event->code);
            } else {
                k_work_submit(&gk_press_work);
            }
        }
        return ZMK_INPUT_PROC_STOP;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api gk_driver_api = {
    .handle_event = gk_handle_event,
};

#define GK_INST(n)                                                                                 \
    static const uint16_t gk_codes_##n[] = DT_INST_PROP(n, codes);                                 \
    static const uint32_t gk_keycodes_##n[] = DT_INST_PROP(n, keycodes);                           \
    BUILD_ASSERT(ARRAY_SIZE(gk_codes_##n) == ARRAY_SIZE(gk_keycodes_##n),                          \
                 "codes and keycodes need to be the same length");                                 \
    static const struct gk_config gk_config_##n = {                                                \
        .size = ARRAY_SIZE(gk_codes_##n),                                                          \
        .codes = gk_codes_##n,                                                                     \
        .keycodes = gk_keycodes_##n,                                                               \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &gk_config_##n, POST_KERNEL,                        \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gk_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GK_INST)
