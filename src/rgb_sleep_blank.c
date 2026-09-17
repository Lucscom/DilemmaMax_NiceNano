/*
 * Dilemma Max - Underglow vor dem Deep Sleep sicher loeschen.
 *
 * WS2812/SK6805 halten ihren letzten Zustand, sobald der Controller nichts mehr
 * sendet. Geht eine Haelfte mit leuchtendem Strip in sys_poweroff(), leuchtet der
 * Strip weiter und zieht den Akku leer.
 *
 * ZMK deckt das im Normalfall ueber CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE ab: der
 * Idle-Timeout (30 s) liegt weit vor dem Sleep-Timeout (15 min), der Strip ist zum
 * Poweroff also laengst schwarz. Auf dem Peripheral ist die Idle-Abschaltung hier
 * aber deaktiviert (config/dilemma_max_right.conf), weil das Central den Zustand
 * vorgibt - siehe src/rgb_split_sync.c. Damit kann die Seite ihren eigenen
 * Sleep-Timeout erreichen, waehrend das Central sie noch auf "an" haelt.
 *
 * zmk_rgb_underglow_off() reicht dafuer nicht: es schiebt das Loeschen auf die
 * Low-Prio-Workqueue (Prio 10, preemptible), waehrend activity.c direkt im Anschluss
 * aus der kooperativen System-Workqueue heraus sys_poweroff() aufruft. Der Thread
 * kommt bis dahin nicht mehr dran. Deshalb hier zusaetzlich synchron loeschen.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/rgb_underglow.h>

LOG_MODULE_REGISTER(dilemma_max_rgb_blank, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *led_strip = DEVICE_DT_GET(STRIP_NODE);
// Nicht const: led_strip_update_rgb() darf den Puffer laut API ueberschreiben.
static struct led_rgb blank_pixels[STRIP_NUM_PIXELS];

static int rgb_sleep_blank_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev == NULL || ev->state != ZMK_ACTIVITY_SLEEP) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Stoppt den Animations-Timer, damit kein Tick die Pixel wieder ueberschreibt.
    zmk_rgb_underglow_off();

    if (device_is_ready(led_strip)) {
        memset(blank_pixels, 0, sizeof(blank_pixels));
        led_strip_update_rgb(led_strip, blank_pixels, STRIP_NUM_PIXELS);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_rgb_sleep_blank, rgb_sleep_blank_listener);
ZMK_SUBSCRIPTION(dilemma_max_rgb_sleep_blank, zmk_activity_state_changed);
