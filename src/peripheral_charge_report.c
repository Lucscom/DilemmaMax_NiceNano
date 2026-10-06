/*
 * Dilemma Max - Ladezustand der rechten Haelfte an das Central melden.
 *
 * ZMK v0.3 kennt dafuer keinen Kanal: die Peripheral schickt ueber den Split-Link nur
 * Tasten, Input-Events und ihren Akkustand, letzteren als ganz normalen BAS-Wert
 * (Battery Service), den das Central abonniert. Ein eigener GATT-Dienst waere die saubere
 * Loesung, kostet aber auf dem Central eine zweite Service-Discovery parallel zu der von
 * ZMK.
 *
 * Stattdessen steckt der Ladezustand hier in der Parittaet des gemeldeten Werts:
 * ungerade = laedt, gerade = laedt nicht. Das kostet ein Prozent Genauigkeit, und der
 * Wert wird um hoechstens 1 verfaelscht. Die Gegenseite liegt in src/status_screen.c.
 *
 * Achtung: beide Haelften muessen zusammen passen. Laeuft rechts eine Firmware ohne
 * diese Kodierung, deutet das Central jeden ungeraden Akkustand als "laedt".
 *
 * Ob USB Strom liefert, fragen wir direkt an der VBUS-Erkennung des nRF52840 ab:
 * CONFIG_ZMK_USB (und damit zmk_usb_is_powered()) ist auf einer Peripheral nicht
 * erlaubt, der Ladechip haengt aber unabhaengig davon am USB-Anschluss.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <hal/nrf_power.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

static bool usb_powered(void) { return nrf_power_usbregstatus_vbusdet_get(NRF_POWER); }

static void report(uint8_t level, bool charging) {
    // 0 heisst beim Central "nicht verbunden", solange melden wir lieber nichts
    if (level == 0) {
        return;
    }

    // 100 waere als ungerade 101 und damit ausserhalb des erlaubten Bereichs
    uint8_t encoded = charging ? (MIN(level, 99) | 1) : (level & ~1);

    if (bt_bas_get_battery_level() == encoded) {
        return;
    }

    int rc = bt_bas_set_battery_level(encoded);
    if (rc != 0) {
        LOG_WRN("Failed to report battery level %u (err %d)", encoded, rc);
    }
}

/*
 * Zwischen zwei Messungen (Default 60 s) merkt sonst niemand, dass das Kabel steckt.
 *
 * Die Schleife startet aus dem Listener heraus und nicht per SYS_INIT: hinter
 * hal/nrf_power.h liegt ein Header, der die Makro-Expansion von SYS_INIT zerlegt
 * ("expected ')' before numeric constant"). Kostet nichts, denn ZMK misst den Akku
 * einmal beim Start, das Event kommt also in den ersten Sekunden.
 */
static void poll_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_cb);

static void schedule_poll(void) {
    // Laeuft die Schleife schon, ist das ein No-Op
    k_work_schedule(&poll_work, K_SECONDS(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_POLL_SEC));
}

static void poll_work_cb(struct k_work *work) {
    report(zmk_battery_state_of_charge(), usb_powered());
    schedule_poll();
}

/*
 * ZMK setzt den BAS-Wert selbst, bevor es das Event ausloest, und schreibt ihn bei jeder
 * Messung erneut, sobald er von seinem eigenen Stand abweicht. Der Wert geht dann also
 * zweimal raus: einmal roh von ZMK, einmal kodiert von hier.
 */
static int battery_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev != NULL) {
        report(ev->state_of_charge, usb_powered());
        schedule_poll();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_charge_report, battery_listener);
ZMK_SUBSCRIPTION(dilemma_max_charge_report, zmk_battery_state_changed);
