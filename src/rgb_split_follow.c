/*
 * Dilemma Max - Underglow des Peripherals an den Split-Link koppeln.
 *
 * Das Peripheral entscheidet ueber sein Underglow nicht mehr selbst, das Central
 * gibt den Zustand vor (src/rgb_split_sync.c). Ist das Central weg - ausgeschaltet,
 * im Deep Sleep oder ausser Reichweite - kommt kein Kommando mehr und der Strip
 * bliebe bis zum eigenen Sleep-Timeout an, also bis zu 15 Minuten.
 *
 * Deshalb haengt das Underglow hier zusaetzlich an der Split-Verbindung: faellt sie
 * weg, geht der Strip aus (nach CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT, Default 4 s).
 *
 * Beim Verbinden wird eingeschaltet: das Peripheral verbindet sich nur, wenn das
 * Central laeuft, und das heisst praktisch immer, dass gerade getippt wird. Wartet
 * man hier stattdessen auf ein Kommando, bliebe die Seite nach jedem Aufwachen aus
 * dem Deep Sleep dunkel - das Central hat sein RGB_ON dann schon geschickt, bevor
 * die Verbindung ueberhaupt stand. Liegt die Annahme daneben, weil das Underglow per
 * &rgb_ug RGB_TOG aus ist oder das Central bereits idle war, korrigieren das die
 * Nachschuesse, die das Central nach dem Aufwachen schickt.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/rgb_underglow.h>

LOG_MODULE_REGISTER(dilemma_max_rgb_follow, CONFIG_ZMK_LOG_LEVEL);

static int rgb_split_follow_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Split-Link %s, Underglow %s", ev->connected ? "verbunden" : "getrennt",
            ev->connected ? "an" : "aus");

    if (ev->connected) {
        zmk_rgb_underglow_on();
    } else {
        zmk_rgb_underglow_off();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_rgb_split_follow, rgb_split_follow_listener);
ZMK_SUBSCRIPTION(dilemma_max_rgb_split_follow, zmk_split_peripheral_status_changed);
