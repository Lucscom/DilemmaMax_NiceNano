/*
 * Dilemma Max - Underglow-Zustand des Centrals auf die Peripherals spiegeln.
 *
 * ZMK schaltet das Underglow bei CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE anhand des
 * lokalen Activity-States ab (zmk/app/src/rgb_underglow.c). Der Activity-State wird
 * aber nur aus lokalen Events gespeist (zmk/app/src/activity.c) und ueber den Split-
 * Link nicht uebertragen: das Central sieht die Tastendruecke beider Haelften, das
 * Peripheral nur seine eigenen. Deshalb bleibt rechts dunkel, wenn man links tippt.
 *
 * Dieser Code laeuft nur auf dem Central. Nach jedem Wechsel des Activity-States
 * schickt er den eigenen Underglow-Zustand per rgb_ug-Behavior (RGB_ON bzw. RGB_OFF)
 * an alle Peripherals. Rechts ist die eigene Idle-Abschaltung dafuer deaktiviert
 * (config/dilemma_max_right.conf), die Seite entscheidet also nicht mehr selbst.
 *
 * Geschickt wird bewusst der tatsaechliche Zustand und nicht einfach "aktiv":
 * ist das Underglow per &rgb_ug RGB_TOG ausgeschaltet, laesst ZMK es beim Aufwachen
 * aus - die Peripherals muessen dann ebenfalls aus bleiben.
 *
 * Lokal wird nichts ausgeloest: das Central braucht seinen eigenen Auto-Off-Pfad,
 * der sich den Zustand vor dem Idle merkt und ihn beim Aufwachen wiederherstellt.
 *
 * Denselben Zustand bekommt ein Peripheral, sobald es sich verbindet - bis dahin haelt
 * es sein Underglow aus (src/rgb_split_follow.c).
 */

#include <zephyr/bluetooth/conn.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/rgb.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/rgb_underglow.h>
#include <zmk/split/central.h>

LOG_MODULE_REGISTER(dilemma_max_rgb_sync, CONFIG_ZMK_LOG_LEVEL);

// Das Peripheral sucht das Behavior ueber seinen Device-Namen (max. 8 Zeichen).
#define RGB_UG_DEV DEVICE_DT_NAME(DT_NODELABEL(rgb_ug))

// Kleiner Abstand zum Activity-Event, damit der Auto-Off-Listener von ZMK den
// lokalen Zustand sicher schon gesetzt hat - die Listener-Reihenfolge ist
// Linker-Order. Die Listener-Kette laeuft in Mikrosekunden durch.
#define DISPATCH_DELAY K_MSEC(5)

// Ein Kommando kann unterwegs verloren gehen, deshalb nach jedem Wechsel ein paar
// Nachschuesse.
#define WAKE_RETRIES 2
#define WAKE_RETRY_INTERVAL K_SECONDS(2)

// Ein Peripheral, das sich gerade verbunden hat, bleibt dunkel, bis es den Zustand von
// hier bekommt (src/rgb_split_follow.c). Direkt nach dem Verbinden laeuft ein Kommando
// aber noch ins Leere: erst muessen Verschluesselung und Service-Discovery durch sein.
// Deshalb der Abstand vor dem ersten Versuch und mehr Nachschuesse als beim Aufwachen.
#define CONNECT_FIRST_DELAY K_SECONDS(1)
#define CONNECT_RETRIES 5

static void rgb_split_sync_send(bool on) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = RGB_UG_DEV,
        .param1 = on ? RGB_ON_CMD : RGB_OFF_CMD,
        .param2 = 0,
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    for (uint8_t i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        // Das rgb_ug-Behavior reagiert nur auf "pressed", daher state = true.
        int err = zmk_split_central_invoke_behavior(i, &binding, event, true);
        if (err < 0) {
            LOG_DBG("Underglow-Sync an Peripheral %d fehlgeschlagen (%d)", i, err);
        }
    }
}

static void rgb_split_sync_work_cb(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(rgb_split_sync_work, rgb_split_sync_work_cb);
static atomic_t wake_retries_left;

static void rgb_split_sync_work_cb(struct k_work *work) {
    bool on = false;

    if (zmk_rgb_underglow_get_state(&on) == 0) {
        rgb_split_sync_send(on);
    }

    if (atomic_get(&wake_retries_left) > 0) {
        atomic_dec(&wake_retries_left);
        k_work_reschedule(&rgb_split_sync_work, WAKE_RETRY_INTERVAL);
        return;
    }

#if CONFIG_DILEMMA_MAX_RGB_SPLIT_SYNC_RESYNC_SEC > 0
    k_work_reschedule(&rgb_split_sync_work,
                      K_SECONDS(CONFIG_DILEMMA_MAX_RGB_SPLIT_SYNC_RESYNC_SEC));
#endif
}

static int rgb_split_sync_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    // Beim Deep Sleep schaltet sich jede Haelfte selbst ab, da ist nichts zu schicken.
    if (ev == NULL || ev->state == ZMK_ACTIVITY_SLEEP) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    atomic_set(&wake_retries_left, ev->state == ZMK_ACTIVITY_ACTIVE ? WAKE_RETRIES : 0);
    k_work_reschedule(&rgb_split_sync_work, DISPATCH_DELAY);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_rgb_split_sync, rgb_split_sync_listener);
ZMK_SUBSCRIPTION(dilemma_max_rgb_split_sync, zmk_activity_state_changed);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
/*
 * Verbindet sich ein Peripheral, bekommt es den aktuellen Zustand. Ohne das erfuhr eine
 * Haelfte, die bei laufendem Central eingeschaltet wurde, erst beim naechsten Wechsel des
 * Activity-States oder beim Resync nach zwei Minuten, was gilt.
 *
 * ZMK v0.3 hat auf dem Central kein Event fuer "Peripheral verbunden", deshalb direkt die
 * Callbacks des Bluetooth-Stacks. Die laufen auch fuer die Verbindung zum Rechner, da ist
 * diese Haelfte aber selbst Peripheral.
 */
static void rgb_split_sync_connected(struct bt_conn *conn, uint8_t conn_err) {
    struct bt_conn_info info;

    if (conn_err != 0 || bt_conn_get_info(conn, &info) != 0 ||
        info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }

    atomic_set(&wake_retries_left, CONNECT_RETRIES);
    k_work_reschedule(&rgb_split_sync_work, CONNECT_FIRST_DELAY);
}

BT_CONN_CB_DEFINE(rgb_split_sync_conn_callbacks) = {
    .connected = rgb_split_sync_connected,
};
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)

static int rgb_split_sync_init(void) {
    // Erster Abgleich, sobald der Split-Link steht: ein frisch gestartetes Peripheral
    // stellt seinen zuletzt gespeicherten Zustand wieder her, der falsch sein kann.
    k_work_reschedule(&rgb_split_sync_work, K_SECONDS(10));
    return 0;
}

SYS_INIT(rgb_split_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
