/*
 * Dilemma Max - Einstellungsmenue auf dem nice!view (linke Haelfte).
 *
 * Bedient wird es mit dem linken Encoder: NAV halten und den Encoder druecken oeffnet es,
 * drehen waehlt einen Eintrag, druecken schaltet dessen Wert weiter. NAV und Encoder
 * druecken oder 20 s ohne Bedienung schliessen es wieder.
 *
 * Abgefangen wird dafuer nichts. Solange das Menue offen ist, ist der Layer DM_LAYER_MENU
 * der Keymap aktiv, und auf dem liegen Drehen und Druecken des Encoders auf dem Behavior
 * &menu aus dieser Datei. Alle anderen Tasten sind dort durchsichtig, man kann also
 * weitertippen.
 *
 * Gezeichnet wird in src/status_screen.c, das den Zustand ueber die Funktionen aus
 * dilemma_max/menu.h abholt.
 */

#define DT_DRV_COMPAT dilemma_behavior_menu

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include <drivers/behavior.h>
#include <dt-bindings/dilemma_max/menu.h>
#include <dilemma_max/menu.h>
#include <dilemma_max/os.h>
#include <dilemma_max/power.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(dilemma_max_menu, CONFIG_ZMK_LOG_LEVEL);

// Die ersten Eintraege sind die Zeiten aus dilemma_max/power.h, in derselben Reihenfolge
#define ITEM_OS DILEMMA_MAX_POWER_ITEM_COUNT

BUILD_ASSERT(ITEM_OS + 1 == DILEMMA_MAX_MENU_ITEM_COUNT);

#define AUTO_CLOSE K_SECONDS(20)

static bool menu_open;
static uint8_t menu_cursor;

static void menu_set_open(bool open) {
    if (open == menu_open) {
        return;
    }

    menu_open = open;
    if (open) {
        menu_cursor = 0;
        zmk_keymap_layer_activate(DM_LAYER_MENU);
    } else {
        zmk_keymap_layer_deactivate(DM_LAYER_MENU);
    }
}

static void auto_close_work_cb(struct k_work *work) {
    menu_set_open(false);
    dilemma_max_status_screen_menu_changed();
}

static K_WORK_DELAYABLE_DEFINE(auto_close_work, auto_close_work_cb);

bool dilemma_max_menu_is_open(void) { return menu_open; }

uint8_t dilemma_max_menu_cursor(void) { return menu_cursor; }

// Hoechstens fuenf Zeichen, mehr passt in der Schrift nicht neben das Symbol
static void format_timeout(uint16_t seconds, char *value, size_t value_len) {
    if (seconds == 0) {
        snprintf(value, value_len, "never");
    } else if (seconds < 60) {
        snprintf(value, value_len, "%u s", seconds);
    } else if (seconds < 600) {
        snprintf(value, value_len, "%u min", seconds / 60);
    } else {
        snprintf(value, value_len, "%umin", seconds / 60);
    }
}

void dilemma_max_menu_item_text(uint8_t item, char *label, size_t label_len, char *value,
                                size_t value_len) {
    static const char *const labels[DILEMMA_MAX_MENU_ITEM_COUNT] = {
        [DILEMMA_MAX_POWER_LIGHT] = "Light", [DILEMMA_MAX_POWER_RAIL] = "Power",
        [DILEMMA_MAX_POWER_SLEEP] = "Sleep", [ITEM_OS] = "OS",
    };

    label[0] = '\0';
    value[0] = '\0';
    if (item >= DILEMMA_MAX_MENU_ITEM_COUNT) {
        return;
    }

    snprintf(label, label_len, "%s", labels[item]);

    if (item < DILEMMA_MAX_POWER_ITEM_COUNT) {
        format_timeout(dilemma_max_power_timeout_s(item), value, value_len);
    } else if (item == ITEM_OS) {
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
        snprintf(value, value_len, "%s", dilemma_max_os_is_windows() ? "win" : "mac");
#endif
    }
}

static int menu_binding_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case DM_MENU_OPEN:
        menu_set_open(!menu_open);
        break;
    case DM_MENU_UP:
        if (menu_open) {
            menu_cursor =
                (menu_cursor + DILEMMA_MAX_MENU_ITEM_COUNT - 1) % DILEMMA_MAX_MENU_ITEM_COUNT;
        }
        break;
    case DM_MENU_DOWN:
        if (menu_open) {
            menu_cursor = (menu_cursor + 1) % DILEMMA_MAX_MENU_ITEM_COUNT;
        }
        break;
    case DM_MENU_SELECT:
        if (!menu_open) {
            break;
        }
        // Mit gehaltenem NAV schliesst derselbe Druck das Menue, mit dem es aufging. Der
        // MENU-Layer liegt ueber NAV, deshalb landet der Druck hier und nicht bei DM_MENU_OPEN.
        if (zmk_keymap_layer_active(DM_LAYER_NAV)) {
            menu_set_open(false);
            break;
        }
        if (menu_cursor < DILEMMA_MAX_POWER_ITEM_COUNT) {
            dilemma_max_power_cycle(menu_cursor);
        } else if (menu_cursor == ITEM_OS) {
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
            dilemma_max_os_toggle();
#endif
        }
        break;
    default:
        return -ENOTSUP;
    }

    if (menu_open) {
        k_work_reschedule(&auto_close_work, AUTO_CLOSE);
    } else {
        k_work_cancel_delayable(&auto_close_work);
    }
    dilemma_max_status_screen_menu_changed();

    return ZMK_BEHAVIOR_OPAQUE;
}

static int menu_binding_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api menu_driver_api = {
    .binding_pressed = menu_binding_pressed,
    .binding_released = menu_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &menu_driver_api);
