/*
 * Dilemma Max - eigenes nice!view-Layout fuer die linke Haelfte (Central).
 *
 * Ersetzt das Status-Widget des nice_view-Shields (CONFIG_NICE_VIEW_WIDGET_STATUS=n).
 * Hochkant von oben nach unten:
 *
 *   oben    Akkustand links und rechts, darunter Verbindung (USB oder Bluetooth)
 *   Mitte   gehaltene Modifier
 *   unten   aktiver Layer
 *
 * Das Display ist nativ 160x68 und steht auf der Tastatur hochkant. Wie beim
 * Original-Widget wird jeder Block auf einem 68x68-Canvas in Leserichtung gezeichnet
 * und danach um 90 Grad gedreht. Der Block ganz rechts im nativen Bild ist oben in
 * der Hochkant-Ansicht; vom untersten Block sind nur die oberen 24 px sichtbar.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>
#include <zmk/usb.h>

#include <dt-bindings/zmk/modifiers.h>

#define BLOCK_SIZE 68

#define COLOR_BG                                                                                   \
    (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_black() : lv_color_white())
#define COLOR_FG                                                                                   \
    (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_white() : lv_color_black())

// Die rechte Haelfte ist die einzige Peripheral und hat damit immer Slot 0.
#define RIGHT_SOURCE 0

struct battery_state {
    uint8_t level;
    bool charging;
};

struct output_state {
    struct zmk_endpoint_instance endpoint;
    int active_profile;
    bool active_connected;
    bool active_bonded;
};

struct mods_state {
    zmk_mod_flags_t mods;
};

struct layer_state {
    zmk_keymap_layer_index_t index;
    const char *name;
};

// Wird nur auf der Display-Workqueue gelesen und geschrieben
static struct {
    struct battery_state left;
    struct battery_state right;
    struct output_state output;
    struct mods_state mods;
    struct layer_state layer;
} state;

static lv_obj_t *top_canvas;
static lv_obj_t *middle_canvas;
static lv_obj_t *bottom_canvas;
static bool middle_canvas_drawn;
static lv_color_t top_cbuf[BLOCK_SIZE * BLOCK_SIZE];
static lv_color_t middle_cbuf[BLOCK_SIZE * BLOCK_SIZE];
static lv_color_t bottom_cbuf[BLOCK_SIZE * BLOCK_SIZE];

static void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]) {
    static lv_color_t cbuf_tmp[BLOCK_SIZE * BLOCK_SIZE];
    memcpy(cbuf_tmp, cbuf, sizeof(cbuf_tmp));

    lv_img_dsc_t img = {
        .data = (void *)cbuf_tmp,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .header.w = BLOCK_SIZE,
        .header.h = BLOCK_SIZE,
    };

    lv_canvas_fill_bg(canvas, COLOR_BG, LV_OPA_COVER);
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, BLOCK_SIZE / 2,
                        BLOCK_SIZE / 2, true);
}

static void init_label(lv_draw_label_dsc_t *dsc, lv_color_t color, const lv_font_t *font,
                       lv_text_align_t align) {
    lv_draw_label_dsc_init(dsc);
    dsc->color = color;
    dsc->font = font;
    dsc->align = align;
}

static void init_rect(lv_draw_rect_dsc_t *dsc, lv_color_t color) {
    lv_draw_rect_dsc_init(dsc);
    dsc->bg_color = color;
}

static void init_line(lv_draw_line_dsc_t *dsc, uint8_t width) {
    lv_draw_line_dsc_init(dsc);
    dsc->color = COLOR_FG;
    dsc->width = width;
}

/*
 * Eine Akku-Zeile, 9 px hoch: Buchstabe, Akku-Symbol, Blitz beim Laden, Prozentwert.
 * level == 0 steht fuer "keine Verbindung" - ZMK meldet beim Trennen der Peripheral
 * genau diesen Wert, und vor dem ersten Bericht steht er ebenfalls auf 0.
 */
static void draw_battery_row(lv_obj_t *canvas, lv_coord_t y, const char *name,
                             struct battery_state battery) {
    lv_draw_rect_dsc_t bg;
    init_rect(&bg, COLOR_BG);
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_draw_label_dsc_t name_dsc;
    init_label(&name_dsc, COLOR_FG, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t value_dsc;
    init_label(&value_dsc, COLOR_FG, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);
    lv_draw_line_dsc_t line_dsc;
    init_line(&line_dsc, 1);

    uint8_t level = MIN(battery.level, 100);

    lv_canvas_draw_text(canvas, 0, y + 1, 8, &name_dsc, name);

    if (level == 0) {
        lv_canvas_draw_text(canvas, 10, y + 1, BLOCK_SIZE - 10, &name_dsc, "OFFLINE");
        return;
    }

    // Rahmen 20x9 mit Pol, Fuellung innen maximal 16 px
    lv_canvas_draw_rect(canvas, 9, y, 20, 9, &fg);
    lv_canvas_draw_rect(canvas, 10, y + 1, 18, 7, &bg);
    lv_canvas_draw_rect(canvas, 29, y + 2, 2, 5, &fg);
    int fill = (16 * level + 50) / 100;
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 11, y + 2, fill, 5, &fg);
    }

    if (battery.charging) {
        lv_point_t bolt[] = {{37, y}, {34, y + 4}, {37, y + 4}, {34, y + 8}};
        lv_canvas_draw_line(canvas, bolt, ARRAY_SIZE(bolt), &line_dsc);
    }

    char value[4];
    snprintf(value, sizeof(value), "%u", level);
    lv_canvas_draw_text(canvas, 40, y + 1, BLOCK_SIZE - 40, &value_dsc, value);
}

static void draw_top(void) {
    lv_obj_t *canvas = top_canvas;
    const struct output_state *out = &state.output;

    lv_draw_rect_dsc_t bg;
    init_rect(&bg, COLOR_BG);
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_draw_label_dsc_t symbol_dsc;
    init_label(&symbol_dsc, COLOR_FG, &lv_font_montserrat_18, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t transport_dsc;
    init_label(&transport_dsc, COLOR_FG, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t status_dsc;
    init_label(&status_dsc, COLOR_FG, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, BLOCK_SIZE, BLOCK_SIZE, &bg);

    draw_battery_row(canvas, 1, "L", state.left);
    draw_battery_row(canvas, 13, "R", state.right);

    lv_canvas_draw_rect(canvas, 0, 25, BLOCK_SIZE, 1, &fg);

    // Verbindung: Symbol und Transport, darunter der Zustand
    const char *symbol;
    char transport[8];
    const char *status;

    switch (out->endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        symbol = LV_SYMBOL_USB;
        strcpy(transport, "USB");
        status = "WIRED";
        break;
    case ZMK_TRANSPORT_BLE:
    default:
        symbol = LV_SYMBOL_WIFI;
        snprintf(transport, sizeof(transport), "BT %d", out->active_profile + 1);
        if (!out->active_bonded) {
            status = "PAIRING";
        } else if (out->active_connected) {
            status = "ONLINE";
        } else {
            status = "WAITING";
        }
        break;
    }

    lv_canvas_draw_text(canvas, 1, 31, 28, &symbol_dsc, symbol);
    lv_canvas_draw_text(canvas, 28, 32, BLOCK_SIZE - 30, &transport_dsc, transport);
    lv_canvas_draw_text(canvas, 0, 56, BLOCK_SIZE, &status_dsc, status);

    rotate_canvas(canvas, top_cbuf);
}

/*
 * Modifier als 2x2-Raster. Gehalten = invertiertes Kaestchen.
 * Linke und rechte Variante eines Modifiers werden zusammengefasst; die Namen folgen
 * macOS (OPT = Alt, CMD = GUI).
 */
static void draw_mod(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                     const char *name, bool active) {
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_draw_label_dsc_t label_dsc;
    init_label(&label_dsc, active ? COLOR_BG : COLOR_FG, &lv_font_unscii_8,
               LV_TEXT_ALIGN_CENTER);

    if (active) {
        lv_canvas_draw_rect(canvas, x, y, w, 14, &fg);
    }
    lv_canvas_draw_text(canvas, x, y + 3, w, &label_dsc, name);
}

static void draw_middle(void) {
    lv_obj_t *canvas = middle_canvas;
    zmk_mod_flags_t mods = state.mods.mods;

    lv_draw_rect_dsc_t bg;
    init_rect(&bg, COLOR_BG);
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);

    // Trennlinien oben zur Verbindung und unten zum Layer
    lv_canvas_draw_rect(canvas, 0, 0, BLOCK_SIZE, BLOCK_SIZE, &bg);
    lv_canvas_draw_rect(canvas, 0, 8, BLOCK_SIZE, 1, &fg);
    lv_canvas_draw_rect(canvas, 0, 59, BLOCK_SIZE, 1, &fg);

    draw_mod(canvas, 0, 16, 33, "SHFT", mods & (MOD_LSFT | MOD_RSFT));
    draw_mod(canvas, 35, 16, 33, "CTRL", mods & (MOD_LCTL | MOD_RCTL));
    draw_mod(canvas, 0, 38, 33, "OPT", mods & (MOD_LALT | MOD_RALT));
    draw_mod(canvas, 35, 38, 33, "CMD", mods & (MOD_LGUI | MOD_RGUI));

    rotate_canvas(canvas, middle_cbuf);
}

// Aktiver Layer als invertiertes Band in den sichtbaren 24 px
static void draw_bottom(void) {
    lv_obj_t *canvas = bottom_canvas;

    lv_draw_rect_dsc_t bg;
    init_rect(&bg, COLOR_BG);
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_draw_label_dsc_t label_dsc;
    init_label(&label_dsc, COLOR_BG, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, BLOCK_SIZE, BLOCK_SIZE, &bg);
    lv_canvas_draw_rect(canvas, 0, 2, BLOCK_SIZE, 21, &fg);

    if (state.layer.name != NULL && strlen(state.layer.name) > 0) {
        lv_canvas_draw_text(canvas, 0, 4, BLOCK_SIZE, &label_dsc, state.layer.name);
    } else {
        char text[12];
        snprintf(text, sizeof(text), "Layer %d", state.layer.index);
        lv_canvas_draw_text(canvas, 0, 4, BLOCK_SIZE, &label_dsc, text);
    }

    rotate_canvas(canvas, bottom_cbuf);
}

// Linke Haelfte: eigener Akku, geladen wird, solange USB Strom liefert.

static void left_battery_update_cb(struct battery_state battery) {
    state.left = battery;
    draw_top();
}

static struct battery_state left_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(left_battery, struct battery_state, left_battery_update_cb,
                            left_battery_get_state)
ZMK_SUBSCRIPTION(left_battery, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(left_battery, zmk_usb_conn_state_changed);
#endif

// Rechte Haelfte: Wert, den das Central per Split-BLE von der Peripheral abholt.
// Ob sie laedt, uebertraegt ZMK nicht.

static void right_battery_update_cb(struct battery_state battery) {
    state.right = battery;
    draw_top();
}

static struct battery_state right_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    uint8_t level = 0;
    if (ev != NULL && ev->source == RIGHT_SOURCE) {
        level = ev->state_of_charge;
    } else {
        zmk_split_central_get_peripheral_battery_level(RIGHT_SOURCE, &level);
    }

    return (struct battery_state){.level = level};
}

ZMK_DISPLAY_WIDGET_LISTENER(right_battery, struct battery_state, right_battery_update_cb,
                            right_battery_get_state)
ZMK_SUBSCRIPTION(right_battery, zmk_peripheral_battery_state_changed);

// Verbindung: gewaehlter Ausgang und Zustand des aktiven Bluetooth-Profils

static void output_update_cb(struct output_state output) {
    state.output = output;
    draw_top();
}

static struct output_state output_get_state(const zmk_event_t *eh) {
    struct output_state output = {
        .endpoint = zmk_endpoints_selected(),
        .active_profile = zmk_ble_active_profile_index(),
        .active_connected = zmk_ble_active_profile_is_connected(),
        .active_bonded = !zmk_ble_active_profile_is_open(),
    };
    return output;
}

ZMK_DISPLAY_WIDGET_LISTENER(output_status, struct output_state, output_update_cb,
                            output_get_state)
ZMK_SUBSCRIPTION(output_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(output_status, zmk_ble_active_profile_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(output_status, zmk_usb_conn_state_changed);
#endif

/*
 * Modifier. ZMK v0.3 loest zmk_modifiers_state_changed nie aus, deshalb
 * haengt der Listener an jedem Keycode-Event. Die Modifier werden erst im Callback auf
 * der Display-Workqueue gelesen: die laeuft nach dem Event, wenn der HID-Listener sie
 * sicher schon uebernommen hat. Unveraenderte Modifier loesen kein Neuzeichnen aus.
 */

static void mods_update_cb(struct mods_state mods) {
    mods.mods = zmk_hid_get_explicit_mods();
    if (mods.mods == state.mods.mods && middle_canvas_drawn) {
        return;
    }
    state.mods = mods;
    middle_canvas_drawn = true;
    draw_middle();
}

static struct mods_state mods_get_state(const zmk_event_t *eh) { return (struct mods_state){}; }

ZMK_DISPLAY_WIDGET_LISTENER(mods_status, struct mods_state, mods_update_cb, mods_get_state)
ZMK_SUBSCRIPTION(mods_status, zmk_keycode_state_changed);

// Layer: hoechster aktiver Layer mit seinem display-name aus der Keymap

static void layer_update_cb(struct layer_state layer) {
    state.layer = layer;
    draw_bottom();
}

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_state){
        .index = index,
        .name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(layer_status, struct layer_state, layer_update_cb, layer_get_state)
ZMK_SUBSCRIPTION(layer_status, zmk_layer_state_changed);

static lv_obj_t *create_block(lv_obj_t *parent, lv_color_t cbuf[], lv_coord_t x_ofs) {
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_obj_align(canvas, LV_ALIGN_TOP_RIGHT, x_ofs, 0);
    lv_canvas_set_buffer(canvas, cbuf, BLOCK_SIZE, BLOCK_SIZE, LV_IMG_CF_TRUE_COLOR);
    return canvas;
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    // Von oben nach unten in der Hochkant-Ansicht; der unterste Block ragt links aus
    // dem nativen Bild heraus, sichtbar bleiben seine oberen 24 px
    top_canvas = create_block(screen, top_cbuf, 0);
    middle_canvas = create_block(screen, middle_cbuf, -BLOCK_SIZE);
    bottom_canvas = create_block(screen, bottom_cbuf, -2 * BLOCK_SIZE);

    left_battery_init();
    right_battery_init();
    output_status_init();
    mods_status_init();
    layer_status_init();

    return screen;
}
