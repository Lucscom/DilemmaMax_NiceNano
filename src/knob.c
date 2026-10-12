/*
 * Dilemma Max - Encoder mit einstellbarer Funktion (linke Haelfte, Central).
 *
 * In der Keymap liegen beide Encoder auf dem Behavior &knob, das nur meldet, welcher
 * Encoder in welche Richtung gedreht wurde (dt-bindings/dilemma_max/knob.h). Was daraus
 * wird - Lautstaerke, Scrollen, Helligkeit des Underglow oder Zoom - steht in den Einstellungen
 * (Menue Input > L Knob / R Knob, src/input_settings.c).
 *
 * Ausgefuehrt wird die Funktion ueber die vorhandenen ZMK-Behaviors &kp, &msc und &rgb_ug:
 * dieses Behavior reicht Druecken und Loslassen mit dem passenden Parameter an sie weiter.
 * &rgb_ug laeuft dabei wie von einer Taste aus auf beiden Haelften.
 */

#define DT_DRV_COMPAT dilemma_behavior_knob

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/dilemma_max/knob.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/rgb.h>
// Wie in der Keymap: 100 * 16 ms / 1000 = 1,6 Einheiten pro Tick
#define ZMK_POINTING_DEFAULT_SCRL_VAL 100
#include <dt-bindings/zmk/pointing.h>
#include <dilemma_max/input.h>
#include <dilemma_max/os.h>
#include <zmk/behavior.h>

LOG_MODULE_REGISTER(dilemma_max_knob, CONFIG_ZMK_LOG_LEVEL);

#define KP_DEV DEVICE_DT_NAME(DT_NODELABEL(kp))
#define MSC_DEV DEVICE_DT_NAME(DT_NODELABEL(msc))
#define RGB_UG_DEV DEVICE_DT_NAME(DT_NODELABEL(rgb_ug))

// Je Funktion: [0] = weniger, [1] = mehr
static const struct zmk_behavior_binding actions[][2] = {
    [DILEMMA_MAX_KNOB_VOLUME] = {{.behavior_dev = KP_DEV, .param1 = C_VOL_DN},
                                 {.behavior_dev = KP_DEV, .param1 = C_VOL_UP}},
    [DILEMMA_MAX_KNOB_SCROLL] = {{.behavior_dev = MSC_DEV, .param1 = SCRL_DOWN},
                                 {.behavior_dev = MSC_DEV, .param1 = SCRL_UP}},
    [DILEMMA_MAX_KNOB_LIGHT] = {{.behavior_dev = RGB_UG_DEV, .param1 = RGB_BRD_CMD},
                                {.behavior_dev = RGB_UG_DEV, .param1 = RGB_BRI_CMD}},
    // Zoom auf dem Mac: Cmd und die Keypad-Tasten - / +, die unabhaengig vom Layout des
    // Rechners dieselben Zeichen liefern (wie bei der Pinch-Geste)
    [DILEMMA_MAX_KNOB_ZOOM] = {{.behavior_dev = KP_DEV, .param1 = LG(KP_MINUS)},
                               {.behavior_dev = KP_DEV, .param1 = LG(KP_PLUS)}},
};

// Zoom unter Windows: dasselbe mit Ctrl
static const struct zmk_behavior_binding zoom_windows[2] = {
    {.behavior_dev = KP_DEV, .param1 = LC(KP_MINUS)},
    {.behavior_dev = KP_DEV, .param1 = LC(KP_PLUS)},
};

static int knob_invoke(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event, bool pressed) {
    bool right = binding->param1 == DM_KNOB_RIGHT_DEC || binding->param1 == DM_KNOB_RIGHT_INC;
    bool increase = binding->param1 == DM_KNOB_LEFT_INC || binding->param1 == DM_KNOB_RIGHT_INC;
    uint8_t function = dilemma_max_input_get(right ? DILEMMA_MAX_INPUT_KNOB_RIGHT
                                                   : DILEMMA_MAX_INPUT_KNOB_LEFT);

    if (function >= ARRAY_SIZE(actions)) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    const struct zmk_behavior_binding *action = &actions[function][increase ? 1 : 0];
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
    if (function == DILEMMA_MAX_KNOB_ZOOM && dilemma_max_os_is_windows()) {
        action = &zoom_windows[increase ? 1 : 0];
    }
#endif

    int err = zmk_behavior_invoke_binding(action, event, pressed);
    if (err < 0) {
        LOG_WRN("Encoder-Funktion %u fehlgeschlagen (%d)", function, err);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int knob_binding_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    return knob_invoke(binding, event, true);
}

static int knob_binding_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    return knob_invoke(binding, event, false);
}

static const struct behavior_driver_api knob_driver_api = {
    .binding_pressed = knob_binding_pressed,
    .binding_released = knob_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &knob_driver_api);
