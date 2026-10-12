/*
 * Dilemma Max - Cursor- und Scroll-Geschwindigkeit des Trackpads (Input-Processor, linke
 * Haelfte).
 *
 * Ersetzt den festen zip_xy_scaler im Trackpad-Listener: die Verhaeltnisse kommen aus den
 * Einstellungen (src/input_settings.c, Menue Touch) und lassen sich zur Laufzeit aendern.
 * Die mittlere Stufe entspricht dem bisherigen Stand, 2:5 fuer den Cursor und 1:1 fuers
 * Scrollen. Wie beim zip_xy_scaler wandert der Rest einer Teilung in das naechste Event,
 * langsame Bewegungen gehen also nicht verloren.
 */

#define DT_DRV_COMPAT dilemma_input_processor_pad_tune

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <dilemma_max/input.h>
#include <drivers/input_processor.h>

struct ratio {
    uint8_t mul;
    uint8_t div;
};

// Cursor: halb, drei Viertel, wie bisher, anderthalbfach, doppelt
static const struct ratio cursor_ratio[] = {{1, 5}, {3, 10}, {2, 5}, {3, 5}, {4, 5}};
// Scrollen: dieselben Stufen um 1:1
static const struct ratio scroll_ratio[] = {{1, 2}, {3, 4}, {1, 1}, {3, 2}, {2, 1}};

// Rest je Achse: X, Y, Rad, Rad quer. Es gibt nur ein Trackpad.
static int32_t remainder[4];

static int pad_tune_handle_event(const struct device *dev, struct input_event *event,
                                 uint32_t param1, uint32_t param2,
                                 struct zmk_input_processor_state *state) {
    const struct ratio *ratio;
    int axis;
    bool invert = false;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    switch (event->code) {
    case INPUT_REL_X:
    case INPUT_REL_Y:
        axis = event->code == INPUT_REL_X ? 0 : 1;
        ratio = &cursor_ratio[dilemma_max_input_get(DILEMMA_MAX_INPUT_SPEED)];
        break;
    case INPUT_REL_WHEEL:
    case INPUT_REL_HWHEEL:
        axis = event->code == INPUT_REL_WHEEL ? 2 : 3;
        ratio = &scroll_ratio[dilemma_max_input_get(DILEMMA_MAX_INPUT_SCROLL)];
        invert = dilemma_max_input_get(DILEMMA_MAX_INPUT_INVERT) != 0;
        break;
    default:
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int32_t scaled = event->value * ratio->mul + remainder[axis];
    int32_t out = scaled / ratio->div;
    remainder[axis] = scaled - out * ratio->div;

    event->value = invert ? -out : out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api pad_tune_driver_api = {
    .handle_event = pad_tune_handle_event,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &pad_tune_driver_api);
