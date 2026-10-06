/*
 * Dilemma Max - Underglow des Peripherals an den Split-Link koppeln.
 *
 * Das Peripheral entscheidet ueber sein Underglow nicht mehr selbst, das Central
 * gibt den Zustand vor (src/rgb_split_sync.c). Ist das Central weg - ausgeschaltet,
 * im Deep Sleep oder ausser Reichweite - kommt kein Kommando mehr und der Strip
 * bliebe bis zum eigenen Sleep-Timeout an, also bis zu 15 Minuten.
 *
 * Deshalb gilt hier: ohne Split-Verbindung ist das Underglow aus.
 *
 *  - Faellt die Verbindung weg, geht der Strip aus (nach CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT,
 *    Default 4 s).
 *  - Nach dem Start bleibt er aus. ZMK stellt beim Laden der Settings den zuletzt
 *    gespeicherten Zustand wieder her, und der ist "an", wenn die Haelfte leuchtend
 *    ausgeschaltet wurde oder eingeschlafen ist.
 *  - Beim Verbinden passiert hier nichts. Das Central schickt seinen Zustand, sobald die
 *    Verbindung steht, und erst der schaltet ein.
 *
 * Frueher wurde beim Verbinden auf Verdacht eingeschaltet, weil das Central nach dem
 * Aufwachen sein RGB_ON schon geschickt hatte, bevor die Verbindung stand. Das war
 * falsch, sobald das Underglow des Centrals aus war (idle oder per &rgb_ug RGB_TOG):
 * die rechte Haelfte leuchtete dann nach dem Einschalten, bis der Resync des Centrals
 * sie nach bis zu zwei Minuten wieder ausschaltete.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/rgb_underglow.h>
#include <zmk/split/bluetooth/peripheral.h>

LOG_MODULE_REGISTER(dilemma_max_rgb_follow, CONFIG_ZMK_LOG_LEVEL);

static int rgb_split_follow_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Split-Link %s", ev->connected ? "verbunden, warte auf das Central" : "getrennt");

    if (!ev->connected) {
        zmk_rgb_underglow_off();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_rgb_split_follow, rgb_split_follow_listener);
ZMK_SUBSCRIPTION(dilemma_max_rgb_split_follow, zmk_split_peripheral_status_changed);

#if IS_ENABLED(CONFIG_SETTINGS)
/*
 * Der Commit-Handler laeuft am Ende von settings_load(), also nachdem ZMK den gespeicherten
 * Underglow-Zustand wiederhergestellt hat. Eigene Settings gibt es unter dem Namen nicht,
 * der Eintrag ist nur der Haken dafuer.
 */
static int rgb_split_follow_settings_commit(void) {
    if (!zmk_split_bt_peripheral_is_connected()) {
        zmk_rgb_underglow_off();
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dilemma_max_rgb_follow, "dilemma/rgbfollow", NULL, NULL,
                               rgb_split_follow_settings_commit, NULL);
#endif // IS_ENABLED(CONFIG_SETTINGS)
