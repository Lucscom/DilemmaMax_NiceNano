/*
 * Dilemma Max - Einstellungsmenue auf dem nice!view (linke Haelfte).
 *
 * Bedient wird es mit dem linken Encoder:
 *   NAV halten + druecken   oeffnen; im Menue: eine Ebene zurueck, ganz oben schliessen
 *   drehen                  Eintrag waehlen
 *   druecken                Untermenue oeffnen bzw. Wert des Eintrags weiterschalten
 * Nach 20 s ohne Bedienung geht es von selbst zu.
 *
 * Abgefangen wird dafuer nichts. Solange das Menue offen ist, ist der Layer DM_LAYER_MENU
 * der Keymap aktiv, und auf dem liegen Drehen und Druecken des Encoders auf dem Behavior
 * &menu aus dieser Datei. Alle anderen Tasten sind dort durchsichtig, man kann also
 * weitertippen.
 *
 * Die Einstellungen selbst liegen in src/power.c, src/input_settings.c und src/os_mode.c;
 * hier stehen nur Aufbau und Beschriftung. Gezeichnet wird in src/status_screen.c, das den
 * Zustand ueber die Funktionen aus dilemma_max/menu.h abholt.
 */

#define DT_DRV_COMPAT dilemma_behavior_menu

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include <drivers/behavior.h>
#include <dt-bindings/dilemma_max/menu.h>
#include <dilemma_max/input.h>
#include <dilemma_max/menu.h>
#include <dilemma_max/os.h>
#include <dilemma_max/power.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(dilemma_max_menu, CONFIG_ZMK_LOG_LEVEL);

#define AUTO_CLOSE K_SECONDS(20)

enum page_id {
    PAGE_ROOT,
    PAGE_POWER,
    PAGE_TOUCH,
    PAGE_INPUT,
};

enum entry_kind {
    KIND_PAGE,  // arg = page_id: oeffnet das Untermenue
    KIND_POWER, // arg = dilemma_max_power_item
    KIND_INPUT, // arg = dilemma_max_input_item
    KIND_OS,
};

struct entry {
    const char *label;
    uint8_t icon;
    uint8_t kind;
    uint8_t arg;
};

struct page {
    const char *title;
    const struct entry *entries;
    uint8_t count;
};

static const struct entry root_entries[] = {
    {"Power", DILEMMA_MAX_ICON_POWER, KIND_PAGE, PAGE_POWER},
    {"Touch", DILEMMA_MAX_ICON_PAD, KIND_PAGE, PAGE_TOUCH},
    {"Input", DILEMMA_MAX_ICON_KNOB, KIND_PAGE, PAGE_INPUT},
};

static const struct entry power_entries[] = {
    {"Light", DILEMMA_MAX_ICON_SUN, KIND_POWER, DILEMMA_MAX_POWER_LIGHT},
    {"Display", DILEMMA_MAX_ICON_MONITOR, KIND_POWER, DILEMMA_MAX_POWER_RAIL},
    {"Sleep", DILEMMA_MAX_ICON_MOON, KIND_POWER, DILEMMA_MAX_POWER_SLEEP},
};

static const struct entry touch_entries[] = {
    {"Speed", DILEMMA_MAX_ICON_CURSOR, KIND_INPUT, DILEMMA_MAX_INPUT_SPEED},
    {"Scroll", DILEMMA_MAX_ICON_SCROLL, KIND_INPUT, DILEMMA_MAX_INPUT_SCROLL},
    {"Invert", DILEMMA_MAX_ICON_INVERT, KIND_INPUT, DILEMMA_MAX_INPUT_INVERT},
    {"Glide", DILEMMA_MAX_ICON_GLIDE, KIND_INPUT, DILEMMA_MAX_INPUT_GLIDE},
    {"Tap", DILEMMA_MAX_ICON_TAP, KIND_INPUT, DILEMMA_MAX_INPUT_TAP},
};

static const struct entry input_entries[] = {
    {"Knob", DILEMMA_MAX_ICON_KNOB, KIND_INPUT, DILEMMA_MAX_INPUT_KNOB},
    {"OS", DILEMMA_MAX_ICON_MONITOR, KIND_OS, 0},
};

static const struct page pages[] = {
    [PAGE_ROOT] = {"MENU", root_entries, ARRAY_SIZE(root_entries)},
    [PAGE_POWER] = {"POWER", power_entries, ARRAY_SIZE(power_entries)},
    [PAGE_TOUCH] = {"TOUCH", touch_entries, ARRAY_SIZE(touch_entries)},
    [PAGE_INPUT] = {"INPUT", input_entries, ARRAY_SIZE(input_entries)},
};

BUILD_ASSERT(ARRAY_SIZE(root_entries) <= DILEMMA_MAX_MENU_MAX_ITEMS);
BUILD_ASSERT(ARRAY_SIZE(power_entries) <= DILEMMA_MAX_MENU_MAX_ITEMS);
BUILD_ASSERT(ARRAY_SIZE(touch_entries) <= DILEMMA_MAX_MENU_MAX_ITEMS);
BUILD_ASSERT(ARRAY_SIZE(input_entries) <= DILEMMA_MAX_MENU_MAX_ITEMS);

static bool menu_open;
static uint8_t menu_page;
static uint8_t menu_cursor;

static void menu_set_open(bool open) {
    if (open == menu_open) {
        return;
    }

    menu_open = open;
    if (open) {
        menu_page = PAGE_ROOT;
        menu_cursor = 0;
        zmk_keymap_layer_activate(DM_LAYER_MENU);
    } else {
        zmk_keymap_layer_deactivate(DM_LAYER_MENU);
    }
}

// Eine Ebene zurueck; ganz oben schliesst das Menue
static void menu_back(void) {
    if (menu_page == PAGE_ROOT) {
        menu_set_open(false);
        return;
    }

    // Auf den Eintrag zurueck, ueber den das Untermenue aufging
    uint8_t from = menu_page;
    menu_page = PAGE_ROOT;
    menu_cursor = 0;
    for (uint8_t i = 0; i < ARRAY_SIZE(root_entries); i++) {
        if (root_entries[i].kind == KIND_PAGE && root_entries[i].arg == from) {
            menu_cursor = i;
        }
    }
}

static void auto_close_work_cb(struct k_work *work) {
    menu_set_open(false);
    dilemma_max_status_screen_menu_changed();
}

static K_WORK_DELAYABLE_DEFINE(auto_close_work, auto_close_work_cb);

bool dilemma_max_menu_is_open(void) { return menu_open; }

const char *dilemma_max_menu_title(void) { return pages[menu_page].title; }

uint8_t dilemma_max_menu_item_count(void) { return pages[menu_page].count; }

uint8_t dilemma_max_menu_cursor(void) { return menu_cursor; }

uint8_t dilemma_max_menu_item_icon(uint8_t item) {
    return item < pages[menu_page].count ? pages[menu_page].entries[item].icon : 0;
}

// Werte haben hoechstens fuenf Zeichen, mehr passt in der Schrift nicht neben das Symbol

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

static void format_input(uint8_t item, char *value, size_t value_len) {
    static const char *const knob_names[] = {
        [DILEMMA_MAX_KNOB_VOLUME] = "vol",
        [DILEMMA_MAX_KNOB_SCROLL] = "scrl",
        [DILEMMA_MAX_KNOB_LIGHT] = "light",
    };
    uint8_t current = dilemma_max_input_get(item);

    switch (item) {
    case DILEMMA_MAX_INPUT_SPEED:
    case DILEMMA_MAX_INPUT_SCROLL:
        snprintf(value, value_len, "%u / 5", current + 1);
        break;
    case DILEMMA_MAX_INPUT_KNOB:
        snprintf(value, value_len, "%s", current < ARRAY_SIZE(knob_names) ? knob_names[current] : "");
        break;
    default:
        snprintf(value, value_len, "%s", current ? "on" : "off");
        break;
    }
}

void dilemma_max_menu_item_text(uint8_t item, char *label, size_t label_len, char *value,
                                size_t value_len) {
    label[0] = '\0';
    value[0] = '\0';
    if (item >= pages[menu_page].count) {
        return;
    }

    const struct entry *entry = &pages[menu_page].entries[item];
    snprintf(label, label_len, "%s", entry->label);

    switch (entry->kind) {
    case KIND_PAGE:
        snprintf(value, value_len, ">>");
        break;
    case KIND_POWER:
        format_timeout(dilemma_max_power_timeout_s(entry->arg), value, value_len);
        break;
    case KIND_INPUT:
        format_input(entry->arg, value, value_len);
        break;
    case KIND_OS:
        snprintf(value, value_len, "%s", dilemma_max_os_is_windows() ? "win" : "mac");
        break;
    default:
        break;
    }
}

static void menu_select(void) {
    const struct entry *entry = &pages[menu_page].entries[menu_cursor];

    switch (entry->kind) {
    case KIND_PAGE:
        menu_page = entry->arg;
        menu_cursor = 0;
        break;
    case KIND_POWER:
        dilemma_max_power_cycle(entry->arg);
        break;
    case KIND_INPUT:
        dilemma_max_input_cycle(entry->arg);
        break;
    case KIND_OS:
        dilemma_max_os_toggle();
        break;
    default:
        break;
    }
}

static int menu_binding_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    uint8_t count = pages[menu_page].count;

    switch (binding->param1) {
    case DM_MENU_OPEN:
        menu_set_open(true);
        break;
    case DM_MENU_UP:
        if (menu_open) {
            menu_cursor = (menu_cursor + count - 1) % count;
        }
        break;
    case DM_MENU_DOWN:
        if (menu_open) {
            menu_cursor = (menu_cursor + 1) % count;
        }
        break;
    case DM_MENU_SELECT:
        if (!menu_open) {
            break;
        }
        // Mit gehaltenem NAV geht derselbe Druck, mit dem das Menue aufging, wieder zurueck.
        // Der MENU-Layer liegt ueber NAV, deshalb landet er hier und nicht bei DM_MENU_OPEN.
        if (zmk_keymap_layer_active(DM_LAYER_NAV)) {
            menu_back();
        } else {
            menu_select();
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
