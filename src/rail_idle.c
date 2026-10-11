/*
 * Dilemma Max - 3,3-V-Schiene im Idle abschalten.
 *
 * An der geschalteten 3,3-V-Schiene des nice!nano (P0.13) haengen pro Haelfte 51 LEDs,
 * links das nice!view und rechts das Trackpad. Die LEDs ziehen auch dunkel ihren Ruhestrom,
 * zusammen 20 bis 35 mA - das war fast der gesamte Verbrauch einer wachen, unbenutzten
 * Haelfte. Der Controller haengt nicht an der Schiene, Tasten und Funk laufen also weiter.
 *
 * Central (links): wann die Schiene aus- und wieder angeht, entscheidet src/power.c nach
 * der im Menue eingestellten Zeit. Haengt die Haelfte am USB-Kabel, bleibt sie an, damit
 * das Display lesbar bleibt.
 *
 * Peripheral (rechts): folgt dem Central, das seinen Zustand zusammen mit dem Underglow
 * schickt (src/rgb_split_sync.c ruft dafuer das Behavior unten auf). Eine eigene Idle-
 * Erkennung taugt dafuer nicht, sie saehe nur die Tasten der rechten Haelfte. Zusaetzlich:
 *  - ein eigener Tastendruck schaltet sofort ein, ohne auf das Central zu warten,
 *  - ohne Split-Verbindung ist die Schiene aus, beim Verbinden geht sie an,
 *  - in den Deep Sleep geht die Haelfte auf Kommando des Centrals, oder von selbst, wenn
 *    sie zwei Minuten lang keine Verbindung hat.
 *
 * Was mit der Schiene ausgeht und wieder anlaufen muss:
 *  - Trackpad: der Treiber wird per PM SUSPEND stillgelegt und initialisiert den Chip bei
 *    RESUME neu (drivers/input/input_maxtouch.c). Das Pad kann die Tastatur damit nicht
 *    mehr aufwecken und ist nach einem Tastendruck erst nach etwa einer Sekunde wieder da.
 *    Der I2C-Bus wird mit abgeschaltet: seine Pull-ups wuerden die Schiene sonst speisen.
 *  - nice!view: verliert sein Bild. Nach dem Einschalten wird der ganze Screen neu
 *    gezeichnet.
 *  - LEDs: ZMK schreibt den Strip ohnehin alle 50 ms neu, solange das Underglow an ist.
 *
 * Geschaltet wird der Pin direkt und nicht ueber den ext_power-Treiber von ZMK: der merkt
 * sich jeden Wechsel in den Settings, und eine Haelfte, die mit abgeschalteter Schiene
 * ausgeht, kaeme mit abgeschalteter Schiene wieder hoch. Der Treiber haelt die Schiene
 * also weiter fuer "an"; vor dem Deep Sleep schaltet er sie wie bisher selbst ab.
 */

#define DT_DRV_COMPAT dilemma_behavior_rail_power

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#include <dilemma_max/power.h>
#include <drivers/behavior.h>
#include <zmk/activity.h>
#include <zmk/pm.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_REGISTER(dilemma_max_rail, CONFIG_ZMK_LOG_LEVEL);

#define RAIL_NODE DT_INST(0, zmk_ext_power_generic)
#define TRACKPAD_NODE DT_NODELABEL(trackpad)
#define HAS_TRACKPAD DT_NODE_HAS_STATUS(TRACKPAD_NODE, okay)

// Erst abschalten, wenn das Underglow sicher schwarz ist: es geht beim selben Idle-Wechsel
// aus, rechts kommt das Kommando dafuer per Funk.
#define RAIL_OFF_DELAY K_MSEC(300)
// So lange braucht das nice!view nach dem Einschalten, bis es Daten annimmt
#define DISPLAY_REDRAW_DELAY K_MSEC(50)

static const struct gpio_dt_spec rail = GPIO_DT_SPEC_GET(RAIL_NODE, control_gpios);

#if HAS_TRACKPAD
static const struct device *const trackpad = DEVICE_DT_GET(TRACKPAD_NODE);
static const struct device *const trackpad_bus = DEVICE_DT_GET(DT_BUS(TRACKPAD_NODE));
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)
#include <lvgl.h>
#include <zmk/display.h>

static void display_redraw_work_cb(struct k_work *work) {
    if (zmk_display_is_initialized()) {
        lv_obj_invalidate(lv_scr_act());
    }
}

static K_WORK_DELAYABLE_DEFINE(display_redraw_work, display_redraw_work_cb);
#endif

// Nur aus der System-Workqueue heraus anfassen
static bool rail_is_on = true;
static bool rail_wanted = true;

static void rail_work_cb(struct k_work *work) {
    bool on = rail_wanted;

    if (on == rail_is_on) {
        return;
    }

    LOG_DBG("3,3-V-Schiene %s", on ? "an" : "aus");

    if (on) {
        gpio_pin_set_dt(&rail, 1);
#if HAS_TRACKPAD
        pm_device_action_run(trackpad_bus, PM_DEVICE_ACTION_RESUME);
        pm_device_action_run(trackpad, PM_DEVICE_ACTION_RESUME);
#endif
#if IS_ENABLED(CONFIG_ZMK_DISPLAY)
        // LVGL gehoert der Display-Workqueue
        k_work_schedule_for_queue(zmk_display_work_q(), &display_redraw_work,
                                  DISPLAY_REDRAW_DELAY);
#endif
    } else {
#if HAS_TRACKPAD
        pm_device_action_run(trackpad, PM_DEVICE_ACTION_SUSPEND);
        pm_device_action_run(trackpad_bus, PM_DEVICE_ACTION_SUSPEND);
#endif
        gpio_pin_set_dt(&rail, 0);
    }

    rail_is_on = on;
}

static K_WORK_DELAYABLE_DEFINE(rail_work, rail_work_cb);

void dilemma_max_rail_set(bool on) {
    rail_wanted = on;
    k_work_reschedule(&rail_work, on ? K_NO_WAIT : RAIL_OFF_DELAY);
}

/*
 * Deep Sleep, so wie ihn ZMK ausloest (app/src/activity.c): SLEEP-Event fuer alle, die
 * vorher aufraeumen muessen, Geraete stilllegen, abschalten. ZMKs eigener Timeout steht auf
 * 24 h; wann geschlafen wird, entscheidet src/power.c auf dem Central.
 */
void dilemma_max_sleep_now(void) {
    raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = ZMK_ACTIVITY_SLEEP});

    if (zmk_pm_suspend_devices() < 0) {
        LOG_ERR("Failed to suspend all the devices");
        zmk_pm_resume_devices();
        return;
    }

    sys_poweroff();
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zmk/events/split_peripheral_status_changed.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

// Ohne Central gibt es niemanden, der diese Haelfte schlafen schickt
#define ORPHAN_SLEEP_DELAY K_SECONDS(120)
// Kurz warten, damit das Kommando noch quittiert wird
#define COMMAND_SLEEP_DELAY K_MSEC(200)

static void sleep_work_cb(struct k_work *work) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    // Wie ZMK: am Kabel wird nicht geschlafen (und das USB-Log bleibt erreichbar)
    if (zmk_usb_is_powered()) {
        return;
    }
#endif
    dilemma_max_sleep_now();
}

static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_work_cb);

static int rail_idle_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(eh);

    if (status != NULL) {
        dilemma_max_rail_set(status->connected);
        if (status->connected) {
            k_work_cancel_delayable(&sleep_work);
        } else {
            k_work_reschedule(&sleep_work, ORPHAN_SLEEP_DELAY);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);

    // Nur einschalten. Ausgeschaltet wird auf Kommando des Centrals: diese Haelfte ist auch
    // dann "idle", wenn gerade links getippt wird.
    if (activity != NULL && activity->state == ZMK_ACTIVITY_ACTIVE) {
        dilemma_max_rail_set(true);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dilemma_max_rail_idle, rail_idle_listener);
ZMK_SUBSCRIPTION(dilemma_max_rail_idle, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(dilemma_max_rail_idle, zmk_split_peripheral_status_changed);

// Nach dem Start ist noch nichts verbunden, und ein "getrennt" kommt dann nie
static int rail_idle_init(void) {
    k_work_reschedule(&sleep_work, ORPHAN_SLEEP_DELAY);
    return 0;
}

SYS_INIT(rail_idle_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* Peripheral */

/*
 * Behavior, ueber das das Central diese Haelfte schaltet: param1 ist DILEMMA_MAX_RAIL_OFF,
 * _ON oder _SLEEP. Nicht fuer die Keymap gedacht.
 */
static int rail_power_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case DILEMMA_MAX_RAIL_OFF:
        dilemma_max_rail_set(false);
        break;
    case DILEMMA_MAX_RAIL_ON:
        dilemma_max_rail_set(true);
        break;
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    case DILEMMA_MAX_RAIL_SLEEP:
        k_work_reschedule(&sleep_work, COMMAND_SLEEP_DELAY);
        break;
#endif
    default:
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int rail_power_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api rail_power_driver_api = {
    .binding_pressed = rail_power_binding_pressed,
    .binding_released = rail_power_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &rail_power_driver_api);
