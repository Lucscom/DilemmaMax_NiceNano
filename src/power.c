/*
 * Dilemma Max - einstellbare Zeiten fuers Stromsparen (linke Haelfte, Central).
 *
 * ZMK kennt zwei feste, einkompilierte Zeiten: nach CONFIG_ZMK_IDLE_TIMEOUT gilt die
 * Tastatur als idle, nach CONFIG_ZMK_IDLE_SLEEP_TIMEOUT geht sie in den Deep Sleep. Hier
 * laeuft stattdessen eine eigene Uhr seit der letzten Eingabe, mit drei Zeiten, die sich
 * im Menue auf dem Display aendern lassen (src/menu.c) und pro Ausgang in den Settings
 * liegen:
 *
 *   LIGHT   Beleuchtung aus
 *   POWER   3,3-V-Schiene aus: LEDs stromlos, Display dunkel, Trackpad aus
 *   SLEEP   Deep Sleep
 *
 * Als Eingabe zaehlt dasselbe wie bei ZMK (app/src/activity.c): Tasten beider Haelften,
 * Encoder und alles vom Trackpad. ZMKs eigene Idle-Erkennung laeuft weiter, bewirkt aber
 * nichts Sichtbares mehr: CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE ist aus, und der Sleep-
 * Timeout steht auf 24 h.
 *
 * Die rechte Haelfte hat keine eigene Uhr. Sie bekommt Licht, Schiene und Deep Sleep von
 * hier kommandiert (src/rgb_split_sync.c), weil nur das Central die Eingaben beider
 * Haelften sieht.
 *
 * Am USB-Kabel bleibt die Schiene dieser Haelfte an (Display lesbar) und sie schlaeft
 * nicht ein. Die rechte Haelfte wird trotzdem abgeschaltet, sie haengt am eigenen Akku.
 */

#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <string.h>

#include <dilemma_max/os.h>
#include <dilemma_max/power.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/rgb_underglow.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

LOG_MODULE_REGISTER(dilemma_max_power, CONFIG_ZMK_LOG_LEVEL);

#define OPTION_COUNT 6
#define SETTINGS_KEY "dilemma/power"
// Nachschuesse nach dem Aufwachen, falls ein Kommando an die rechte Haelfte verloren geht
#define WAKE_RETRIES 2

// Sekunden, 0 = nie
static const uint16_t options[DILEMMA_MAX_POWER_ITEM_COUNT][OPTION_COUNT] = {
    [DILEMMA_MAX_POWER_LIGHT] = {10, 30, 60, 120, 300, 0},
    [DILEMMA_MAX_POWER_RAIL] = {30, 60, 120, 300, 600, 0},
    [DILEMMA_MAX_POWER_SLEEP] = {60, 120, 300, 600, 1800, 0},
};

// Standard: 30 s Licht, 30 s Schiene, 5 min Deep Sleep
#define DEFAULT_OPTIONS {1, 0, 2}

/*
 * Die Zeiten gelten pro Ausgang (Bluetooth-Profile und USB, siehe dilemma_max/os.h) und
 * wechseln mit ihm.
 *
 * dimmed_light: das Underglow ist nur deshalb aus, weil hier abgedunkelt wurde. ZMK
 * speichert den Zustand "aus" in seinen Settings; schlaeft die Tastatur so ein, kaeme
 * sie dunkel wieder hoch. Mit dem Merker schaltet der Start das Licht wieder ein.
 */
static struct {
    uint8_t option[DILEMMA_MAX_OUTPUT_COUNT][DILEMMA_MAX_POWER_ITEM_COUNT];
    uint8_t dimmed_light;
    // Power saving mode, gilt fuer alle Ausgaenge. Steht am Ende: aeltere Settings ohne das
    // Feld sind einfach ein Byte kuerzer.
    uint8_t eco;
} __packed saved = {
    .option = {[0 ... DILEMMA_MAX_OUTPUT_COUNT - 1] = DEFAULT_OPTIONS},
};

// Format vor der Trennung nach Ausgaengen: ein Satz Zeiten fuer alles
struct saved_v1 {
    uint8_t option[DILEMMA_MAX_POWER_ITEM_COUNT];
    uint8_t dimmed_light;
} __packed;

static atomic_t last_activity_ms;

// Nur auf der System-Workqueue
static bool dimmed;
static bool rail_off;
static bool remote_rail_off;
static bool remote_sleep_sent;

static void save_work_cb(struct k_work *work) {
    settings_save_one(SETTINGS_KEY, &saved, sizeof(saved));
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);

// Mit Abstand: ein kurzes Abdunkeln aendert am Ende nichts und kostet dann keinen
// Schreibzugriff, und beim Durchschalten im Menue wird nur der letzte Wert geschrieben.
static void save_later(void) { k_work_reschedule(&save_work, K_SECONDS(10)); }

uint16_t dilemma_max_power_timeout_s(enum dilemma_max_power_item item) {
    return options[item][saved.option[dilemma_max_output_index()][item]];
}

void dilemma_max_power_cycle(enum dilemma_max_power_item item) {
    uint8_t *option = &saved.option[dilemma_max_output_index()][item];

    *option = (*option + 1) % OPTION_COUNT;
    save_later();
}

bool dilemma_max_power_remote_rail_on(void) { return !remote_rail_off; }

bool dilemma_max_power_eco(void) { return saved.eco != 0; }

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void set_dimmed(bool dim) {
    if (dim == dimmed) {
        return;
    }
    dimmed = dim;

    if (dim) {
        bool on = false;
        if (zmk_rgb_underglow_get_state(&on) == 0 && on) {
            saved.dimmed_light = 1;
            zmk_rgb_underglow_off();
            save_later();
        }
    } else if (saved.dimmed_light) {
        saved.dimmed_light = 0;
        zmk_rgb_underglow_on();
        save_later();
    }
}

static void set_rail_off(bool off) {
    if (off == rail_off) {
        return;
    }
    rail_off = off;
    dilemma_max_rail_set(!off);
}

static void eval_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(eval_work, eval_work_cb);

static void eval_work_cb(struct k_work *work) {
    uint32_t idle_s = (k_uptime_get_32() - (uint32_t)atomic_get(&last_activity_ms)) / 1000;
    uint16_t light_s = dilemma_max_power_timeout_s(DILEMMA_MAX_POWER_LIGHT);
    uint16_t rail_s = dilemma_max_power_timeout_s(DILEMMA_MAX_POWER_RAIL);
    uint16_t sleep_s = dilemma_max_power_timeout_s(DILEMMA_MAX_POWER_SLEEP);
    bool usb = usb_powered();

    bool eco = saved.eco != 0;

    // Power saving mode: Schiene immer aus, auf beiden Haelften und auch am Kabel
    bool want_remote_rail_off = eco || (rail_s != 0 && idle_s >= rail_s);
    // Ohne Schiene leuchtet nichts, also spaetestens dann auch das Licht aus
    bool want_dim = (light_s != 0 && idle_s >= light_s) || want_remote_rail_off;
    // Sonst bleibt die Schiene am Kabel an, es sei denn, das ist zum Testen abgestellt
    bool want_rail_off = want_remote_rail_off &&
                         (eco || !(usb && IS_ENABLED(CONFIG_DILEMMA_MAX_POWER_RAIL_ON_USB)));

    bool changed = want_dim != dimmed || want_remote_rail_off != remote_rail_off;

    // Beim Aufwachen erst Strom, dann Licht; beim Abschalten umgekehrt
    if (!want_rail_off) {
        set_rail_off(false);
    }
    set_dimmed(want_dim);
    if (want_rail_off) {
        set_rail_off(true);
    }
    remote_rail_off = want_remote_rail_off;

    if (changed) {
        dilemma_max_split_sync(want_dim ? 0 : WAKE_RETRIES);
    }

    if (sleep_s != 0 && idle_s >= sleep_s) {
        if (!remote_sleep_sent) {
            // Die rechte Haelfte zuerst, diese eine Runde spaeter: das Kommando muss noch
            // ueber den Funk
            remote_sleep_sent = true;
            dilemma_max_split_send_sleep();
        } else if (!usb) {
            LOG_DBG("Deep Sleep nach %u s ohne Eingabe", idle_s);
            dilemma_max_sleep_now();
        }
    } else {
        remote_sleep_sent = false;
    }

    k_work_schedule(&eval_work, K_SECONDS(1));
}

void dilemma_max_power_set_eco(bool on) {
    if ((saved.eco != 0) == on) {
        return;
    }

    saved.eco = on ? 1 : 0;
    save_later();
    // Sofort anwenden und der rechten Haelfte sagen, auch wenn sich an "Schiene aus" nichts
    // aendert: dort entscheidet der Modus, ob das Ladekabel die Schiene anlaesst
    k_work_reschedule(&eval_work, K_NO_WAIT);
    dilemma_max_split_sync(WAKE_RETRIES);
}

static void note_activity(void) {
    atomic_set(&last_activity_ms, (atomic_val_t)k_uptime_get_32());

    // Nicht auf die naechste volle Sekunde warten, wenn etwas wieder angehen muss
    if (dimmed || rail_off || remote_rail_off) {
        k_work_reschedule(&eval_work, K_NO_WAIT);
    }
}

static int power_activity_listener(const zmk_event_t *eh) {
    note_activity();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_power, power_activity_listener);
ZMK_SUBSCRIPTION(dilemma_max_power, zmk_position_state_changed);
ZMK_SUBSCRIPTION(dilemma_max_power, zmk_sensor_event);
#if IS_ENABLED(CONFIG_ZMK_USB)
// Kabel gesteckt oder gezogen zaehlt als Eingabe: sonst liefe die Uhr am Kabel weiter, und
// nach dem Abziehen ginge das Display sofort aus.
ZMK_SUBSCRIPTION(dilemma_max_power, zmk_usb_conn_state_changed);
#endif

// Trackpad: kommt ueber den Split-Link als Input-Event an. Laeuft im Input-Thread.
static void power_input_listener(struct input_event *ev) { note_activity(); }

INPUT_CALLBACK_DEFINE(NULL, power_input_listener);

static int power_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    int rc;

    if (settings_name_next(name, NULL) != 0) {
        return -ENOENT;
    }

    if (len == sizeof(struct saved_v1) && len != sizeof(saved)) {
        // Alter Stand: fuer alle Ausgaenge uebernehmen
        struct saved_v1 old;
        rc = read_cb(cb_arg, &old, sizeof(old));
        if (rc < 0) {
            return rc;
        }
        for (int output = 0; output < DILEMMA_MAX_OUTPUT_COUNT; output++) {
            memcpy(saved.option[output], old.option, sizeof(old.option));
        }
        saved.dimmed_light = old.dimmed_light;
    } else if (len == sizeof(saved) || len == sizeof(saved) - sizeof(saved.eco)) {
        rc = read_cb(cb_arg, &saved, len);
        if (rc < 0) {
            return rc;
        }
    } else {
        return -ENOENT;
    }

    for (int output = 0; output < DILEMMA_MAX_OUTPUT_COUNT; output++) {
        for (int i = 0; i < DILEMMA_MAX_POWER_ITEM_COUNT; i++) {
            if (saved.option[output][i] >= OPTION_COUNT) {
                saved.option[output][i] = 0;
            }
        }
    }

    return 0;
}

// Laeuft am Ende von settings_load(), nachdem ZMK seinen Underglow-Zustand geladen hat
static int power_settings_commit(void) {
    if (saved.dimmed_light) {
        saved.dimmed_light = 0;
        zmk_rgb_underglow_on();
        save_later();
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dilemma_max_power, SETTINGS_KEY, NULL, power_settings_set,
                               power_settings_commit, NULL);

static int power_init(void) {
    atomic_set(&last_activity_ms, (atomic_val_t)k_uptime_get_32());
    k_work_schedule(&eval_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(power_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
