/*
 * Dilemma Max - Encoder im Einstellungsmenue (Sensor-Behavior, linke Haelfte).
 *
 * Fuer das Menue reicht ZMKs sensor-rotate-Behavior nicht: es zaehlt die Grad, die der
 * Encoder meldet (4,5 pro Flanke, 18 pro Rastung), und loest bei jedem vollen Vielfachen
 * von 18 aus. Steht der Zaehler in der Ruhelage nicht genau auf einem Vielfachen - eine
 * einzige verschluckte oder geprellte Flanke genuegt -, bleibt er dauerhaft versetzt.
 * In eine Richtung faellt das nicht auf, beim Umkehren aber braucht es dann zwei
 * Rastungen bis zum naechsten Schritt.
 *
 * Hier wird deshalb anders gezaehlt:
 *  - Steht der Encoder laenger als REST_MS still, liegt er in einer Rastung, und der
 *    Zaehler beginnt bei null. Ein Versatz ueberlebt so hoechstens eine Bewegung.
 *  - Ausgeloest wird schon nach drei Vierteln einer Rastung, eine fehlende Flanke kostet
 *    also keinen Schritt. Danach wird eine ganze Rastung abgezogen.
 */

#define DT_DRV_COMPAT dilemma_behavior_menu_knob

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <dilemma_max/menu.h>
#include <zmk/behavior.h>
#include <zmk/sensors.h>

#define REST_MS 200

// In Mikrograd, damit die halben Grad des Encoders nicht verloren gehen
static int32_t accumulated;
static uint32_t last_ms;
static int pending_steps;

static int menu_knob_accept_data(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event,
                                 const struct zmk_sensor_config *sensor_config,
                                 size_t channel_data_size,
                                 const struct zmk_sensor_channel_data *channel_data) {
    const struct sensor_value value = channel_data[0].value;
    int32_t detent = 360 * 1000000 / MAX(sensor_config->triggers_per_rotation, 1);
    uint32_t now = k_uptime_get_32();

    if (now - last_ms > REST_MS) {
        accumulated = 0;
    }
    last_ms = now;

    accumulated += value.val1 * 1000000 + value.val2;

    pending_steps = 0;
    while (accumulated >= detent * 3 / 4) {
        accumulated -= detent;
        pending_steps++;
    }
    while (accumulated <= -(detent * 3 / 4)) {
        accumulated += detent;
        pending_steps--;
    }

    return 0;
}

static int menu_knob_process(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event,
                             enum behavior_sensor_binding_process_mode mode) {
    int steps = pending_steps;
    pending_steps = 0;

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER || steps == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    // Positive Grad waren bisher "nach oben" (DM_MENU_UP), das bleibt so
    dilemma_max_menu_step(-steps);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api menu_knob_driver_api = {
    .sensor_binding_accept_data = menu_knob_accept_data,
    .sensor_binding_process = menu_knob_process,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &menu_knob_driver_api);
