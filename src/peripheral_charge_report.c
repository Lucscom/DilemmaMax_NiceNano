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
 * Der Akkufilter (src/battery_filter.c) gibt auf dieser Haelfte schon den kodierten Wert an
 * ZMK weiter, damit ZMK und dieser Code nicht abwechselnd zwei verschiedene Werte in den
 * Battery Service schreiben. Hier bleibt, was ZMK nicht tut: den Wert auch dann aktuell
 * halten, wenn die Haelfte selbst idle ist (ZMK misst dann nicht), einen Kabelwechsel
 * innerhalb von Sekunden melden und den Wert regelmaessig wiederholen.
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

#include <dilemma_max/battery.h>
#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>

// Jede so vielte Runde geht der Wert auch unveraendert raus. Der Battery Service meldet
// nur Aenderungen, und der gefilterte Akkustand bleibt lange gleich: hat das Central den
// Wert nach dem Verbinden verpasst, zeigte es sonst dauerhaft "nicht verbunden".
#define REPEAT_EVERY_POLLS 6
// Nach dem Verbinden braucht das Central einen Moment fuer Verschluesselung, Discovery und
// das Abonnieren der Benachrichtigung. Eine Meldung davor geht ins Leere.
#define CONNECT_DELAY K_SECONDS(3)
#define CONNECT_REPEATS 2

static uint8_t polls_since_report;
static uint8_t forced_reports;

static bool usb_powered(void) { return nrf_power_usbregstatus_vbusdet_get(NRF_POWER); }

uint8_t dilemma_max_charge_encode(uint8_t level) {
    // 100 waere als ungerade 101 und damit ausserhalb des erlaubten Bereichs, 1 als gerade 0
    // und damit "nicht verbunden"
    return usb_powered() ? (MIN(level, 99) | 1) : MAX(level & ~1, 2);
}

static uint8_t current_level(void) {
#if IS_ENABLED(CONFIG_DILEMMA_MAX_BATTERY_FILTER)
    // Direkt vom Filter: der misst auch, waehrend diese Haelfte idle ist
    return dilemma_max_battery_filter_level();
#else
    return zmk_battery_state_of_charge();
#endif
}

static void report(bool force) {
    uint8_t level = current_level();

    // Noch nichts gemessen
    if (level == 0) {
        return;
    }

    uint8_t encoded = dilemma_max_charge_encode(level);

    if (!force && bt_bas_get_battery_level() == encoded) {
        return;
    }

    polls_since_report = 0;

    // Benachrichtigt das Central auch dann, wenn sich der Wert nicht geaendert hat
    int rc = bt_bas_set_battery_level(encoded);
    if (rc != 0) {
        LOG_WRN("Failed to report battery level %u (err %d)", encoded, rc);
    }
}

/*
 * Zwischen zwei Messungen von ZMK (Default 60 s, und nur solange die Haelfte aktiv ist)
 * merkt sonst niemand, dass das Kabel steckt.
 *
 * Die Schleife startet aus dem Listener heraus und nicht per SYS_INIT: hinter
 * hal/nrf_power.h liegt ein Header, der die Makro-Expansion von SYS_INIT zerlegt
 * ("expected ')' before numeric constant"). Kostet nichts, denn ZMK misst den Akku
 * einmal beim Start, das Event kommt also in den ersten Sekunden.
 */
static void poll_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_cb);

static void poll_work_cb(struct k_work *work) {
    bool force = false;

    if (forced_reports > 0) {
        forced_reports--;
        force = true;
    } else if (++polls_since_report >= REPEAT_EVERY_POLLS) {
        force = true;
    }

    report(force);
    k_work_schedule(&poll_work, K_SECONDS(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_POLL_SEC));
}

static int charge_report_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(eh);

    if (status != NULL) {
        if (status->connected) {
            forced_reports = CONNECT_REPEATS;
            k_work_reschedule(&poll_work, CONNECT_DELAY);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_battery_state_changed(eh) != NULL) {
        report(false);
        // Laeuft die Schleife schon, ist das ein No-Op
        k_work_schedule(&poll_work, K_SECONDS(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_POLL_SEC));
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_charge_report, charge_report_listener);
ZMK_SUBSCRIPTION(dilemma_max_charge_report, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(dilemma_max_charge_report, zmk_split_peripheral_status_changed);
