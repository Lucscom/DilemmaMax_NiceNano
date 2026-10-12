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

#if IS_ENABLED(CONFIG_DILEMMA_MAX_MENU)
#include <dilemma_max/menu.h>
#endif
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
#include <dilemma_max/os.h>
#include <dt-bindings/dilemma_max/layers.h>
#endif

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

/*
 * Schraege Linien zeichnet LVGL mit Kantenglaettung, und bei einem Bit Farbtiefe faellt
 * jedes halb gedeckte Pixel auf den Hintergrund zurueck - eine 1 px breite Diagonale ist
 * damit unsichtbar. Schraeges wird deshalb Pixel fuer Pixel aus Rechtecken gesetzt.
 */
static void draw_pixel(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_canvas_draw_rect(canvas, x, y, 1, 1, &fg);
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

    uint8_t level = MIN(battery.level, 100);

    lv_canvas_draw_text(canvas, 0, y + 1, 8, &name_dsc, name);

    // Keine Verbindung: Kreuz rechts aussen, 10x9 px mit 2 px Strichstaerke
    if (level == 0) {
        for (int i = 0; i < 9; i++) {
            lv_canvas_draw_rect(canvas, BLOCK_SIZE - 10 + i, y + i, 2, 1, &fg);
            lv_canvas_draw_rect(canvas, BLOCK_SIZE - 2 - i, y + i, 2, 1, &fg);
        }
        return;
    }

    // Rahmen 20x9 mit Pol, Fuellung innen maximal 16 px
    lv_canvas_draw_rect(canvas, 9, y, 20, 9, &fg);
    lv_canvas_draw_rect(canvas, 10, y + 1, 18, 7, &bg);
    lv_canvas_draw_rect(canvas, 29, y + 2, 2, 5, &fg);
    // Beim Laden ist der Wert eine Schaetzung (src/battery_filter.c). 1 heisst dabei, dass
    // die Haelfte am Kabel gestartet ist und den Stand nicht kennt: dann nur Rahmen und Blitz.
    bool unknown = battery.charging && level <= 1;
    int fill = unknown ? 0 : (16 * level + 50) / 100;
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 11, y + 2, fill, 5, &fg);
    }

    // Blitz beim Laden, 5x9 px aus einzelnen Pixeln
    if (battery.charging) {
        static const uint8_t bolt[9] = {0x0c, 0x06, 0x03, 0x1f, 0x18, 0x0c, 0x06, 0x03, 0x00};
        for (int row = 0; row < 9; row++) {
            for (int col = 0; col < 5; col++) {
                if (bolt[row] & BIT(col)) {
                    draw_pixel(canvas, 33 + col, y + row);
                }
            }
        }
    }

    if (unknown) {
        return;
    }

    char value[4];
    snprintf(value, sizeof(value), "%u", level);
    lv_canvas_draw_text(canvas, 40, y + 1, BLOCK_SIZE - 40, &value_dsc, value);
}

#if IS_ENABLED(CONFIG_DILEMMA_MAX_MENU)
/*
 * Einstellungsmenue (src/menu.c): solange es offen ist, ersetzt es den oberen und den
 * mittleren Block. Der untere zeigt weiter den Layer, also "MENU".
 *
 * Eine Zeile pro Eintrag: links ein Symbol, daneben klein der Name und darunter der Wert.
 * Der gewaehlte Eintrag ist umrahmt und sein Symbol invertiert; rechts laeuft ein Balken
 * mit, der die Position zeigt.
 *
 * Das Layout ist in einem 68x136 px hohen Feld beschrieben, das ueber beide Bloecke geht;
 * jeder Block zeichnet seinen Ausschnitt (block_y = 0 oder BLOCK_SIZE). Rechtecke werden
 * an der Grenze abgeschnitten, Text nicht - die Zeilenhoehe ist so gewaehlt, dass in der
 * dritten Zeile der Name ueber und der Wert unter y = 68 liegt.
 */
#define MENU_ROW_HEIGHT 27
#define MENU_ICON_X 2
#define MENU_ICON_W 16
#define MENU_ICON_H 21
#define MENU_TEXT_X 21
#define MENU_TEXT_W 43
#define MENU_BAR_X 66

static void menu_rect(lv_obj_t *canvas, lv_coord_t block_y, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w, lv_coord_t h, lv_color_t color) {
    lv_coord_t top = MAX(y - block_y, 0);
    lv_coord_t bottom = MIN(y + h - block_y, BLOCK_SIZE);
    if (bottom <= top) {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    init_rect(&dsc, color);
    lv_canvas_draw_rect(canvas, x, top, w, bottom - top, &dsc);
}

static void menu_frame(lv_obj_t *canvas, lv_coord_t block_y, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_color_t color) {
    menu_rect(canvas, block_y, x, y, w, 1, color);
    menu_rect(canvas, block_y, x, y + h - 1, w, 1, color);
    menu_rect(canvas, block_y, x, y, 1, h, color);
    menu_rect(canvas, block_y, x + w - 1, y, 1, h, color);
}

static void menu_text(lv_obj_t *canvas, lv_coord_t block_y, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w, lv_color_t color, const lv_font_t *font,
                      lv_text_align_t align, const char *text) {
    if (y < block_y || y >= block_y + BLOCK_SIZE) {
        return;
    }

    lv_draw_label_dsc_t dsc;
    init_label(&dsc, color, font, align);
    lv_canvas_draw_text(canvas, x, y - block_y, w, &dsc, text);
}

// Symbole, Pixel fuer Pixel um den Mittelpunkt (cx, cy) gesetzt. Reihenfolge wie die
// Eintraege in src/menu.c: Licht, Schiene, Deep Sleep, Betriebssystem, Akku.
static void menu_icon(lv_obj_t *canvas, lv_coord_t by, uint8_t item, lv_coord_t cx, lv_coord_t cy,
                      lv_color_t c) {
    switch (item) {
    case 0: // Sonne
        menu_rect(canvas, by, cx - 2, cy - 2, 5, 5, c);
        menu_rect(canvas, by, cx, cy - 6, 1, 2, c);
        menu_rect(canvas, by, cx, cy + 5, 1, 2, c);
        menu_rect(canvas, by, cx - 6, cy, 2, 1, c);
        menu_rect(canvas, by, cx + 5, cy, 2, 1, c);
        menu_rect(canvas, by, cx - 4, cy - 4, 1, 1, c);
        menu_rect(canvas, by, cx + 4, cy - 4, 1, 1, c);
        menu_rect(canvas, by, cx - 4, cy + 4, 1, 1, c);
        menu_rect(canvas, by, cx + 4, cy + 4, 1, 1, c);
        break;
    case 1: // Ein/Aus: offener Ring mit Strich
        menu_rect(canvas, by, cx, cy - 6, 1, 6, c);
        menu_rect(canvas, by, cx - 5, cy - 1, 1, 4, c);
        menu_rect(canvas, by, cx + 5, cy - 1, 1, 4, c);
        menu_rect(canvas, by, cx - 2, cy + 5, 5, 1, c);
        menu_rect(canvas, by, cx - 4, cy - 2, 1, 1, c);
        menu_rect(canvas, by, cx + 4, cy - 2, 1, 1, c);
        menu_rect(canvas, by, cx - 3, cy - 3, 1, 1, c);
        menu_rect(canvas, by, cx + 3, cy - 3, 1, 1, c);
        menu_rect(canvas, by, cx - 4, cy + 3, 1, 1, c);
        menu_rect(canvas, by, cx + 4, cy + 3, 1, 1, c);
        menu_rect(canvas, by, cx - 3, cy + 4, 1, 1, c);
        menu_rect(canvas, by, cx + 3, cy + 4, 1, 1, c);
        break;
    case 2: // Zz
        menu_rect(canvas, by, cx - 5, cy - 6, 7, 1, c);
        menu_rect(canvas, by, cx - 5, cy, 7, 1, c);
        for (int i = 1; i < 6; i++) {
            menu_rect(canvas, by, cx + 1 - i, cy - 6 + i, 1, 1, c);
        }
        menu_rect(canvas, by, cx + 2, cy + 2, 4, 1, c);
        menu_rect(canvas, by, cx + 2, cy + 5, 4, 1, c);
        menu_rect(canvas, by, cx + 4, cy + 3, 1, 1, c);
        menu_rect(canvas, by, cx + 3, cy + 4, 1, 1, c);
        break;
    case 3: // Bildschirm
        menu_frame(canvas, by, cx - 6, cy - 6, 13, 9, c);
        menu_rect(canvas, by, cx - 1, cy + 3, 3, 2, c);
        menu_rect(canvas, by, cx - 3, cy + 5, 7, 1, c);
        break;
    case 4: // Akku
        menu_frame(canvas, by, cx - 6, cy - 3, 11, 7, c);
        menu_rect(canvas, by, cx + 5, cy - 1, 2, 3, c);
        menu_rect(canvas, by, cx - 4, cy - 1, 5, 3, c);
        break;
    default:
        break;
    }
}

static void draw_menu(lv_obj_t *canvas, lv_color_t cbuf[], lv_coord_t block_y) {
    uint8_t cursor = dilemma_max_menu_cursor();

    menu_rect(canvas, block_y, 0, block_y, BLOCK_SIZE, BLOCK_SIZE, COLOR_BG);

    for (uint8_t item = 0; item < DILEMMA_MAX_MENU_ITEM_COUNT; item++) {
        lv_coord_t y = item * MENU_ROW_HEIGHT;
        bool selected = item == cursor;
        char label[9];
        char value[9];

        dilemma_max_menu_item_text(item, label, sizeof(label), value, sizeof(value));

        if (selected) {
            menu_frame(canvas, block_y, 0, y, MENU_BAR_X - 1, MENU_ROW_HEIGHT - 1, COLOR_FG);
            menu_rect(canvas, block_y, MENU_ICON_X, y + 2, MENU_ICON_W, MENU_ICON_H + 1,
                      COLOR_FG);
        }
        menu_icon(canvas, block_y, item, MENU_ICON_X + MENU_ICON_W / 2, y + 13,
                  selected ? COLOR_BG : COLOR_FG);

        menu_text(canvas, block_y, MENU_TEXT_X, y + 1, MENU_TEXT_W, COLOR_FG,
                  &lv_font_montserrat_10, LV_TEXT_ALIGN_LEFT, label);
        menu_text(canvas, block_y, MENU_TEXT_X, y + 14, MENU_TEXT_W, COLOR_FG, &lv_font_unscii_8,
                  LV_TEXT_ALIGN_LEFT, value);
    }

    // Positionsbalken
    menu_rect(canvas, block_y, MENU_BAR_X + 1, 0, 1,
              DILEMMA_MAX_MENU_ITEM_COUNT * MENU_ROW_HEIGHT - 1, COLOR_FG);
    menu_rect(canvas, block_y, MENU_BAR_X, cursor * MENU_ROW_HEIGHT, 2, MENU_ROW_HEIGHT - 1,
              COLOR_FG);

    rotate_canvas(canvas, cbuf);
}

static bool menu_is_open(void) { return dilemma_max_menu_is_open(); }
#else
static bool menu_is_open(void) { return false; }
#endif

static void draw_top(void) {
    lv_obj_t *canvas = top_canvas;
    const struct output_state *out = &state.output;

#if IS_ENABLED(CONFIG_DILEMMA_MAX_MENU)
    if (menu_is_open()) {
        draw_menu(canvas, top_cbuf, 0);
        return;
    }
#endif

    lv_draw_rect_dsc_t bg;
    init_rect(&bg, COLOR_BG);
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_draw_label_dsc_t symbol_dsc;
    init_label(&symbol_dsc, COLOR_FG, &lv_font_montserrat_18, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t transport_dsc;
    init_label(&transport_dsc, COLOR_FG, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t status_dsc;
    // Montserrat 10 statt unscii_8: "Connecting..." braucht 66 px, unscii_8 (8 px pro
    // Zeichen) haette auf den 68 px nur Platz fuer 8 Zeichen.
    init_label(&status_dsc, COLOR_FG, &lv_font_montserrat_10, LV_TEXT_ALIGN_CENTER);

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
        status = "Wired";
        break;
    case ZMK_TRANSPORT_BLE:
    default:
        symbol = LV_SYMBOL_WIFI;
        snprintf(transport, sizeof(transport), "BT %d", out->active_profile + 1);
        if (!out->active_bonded) {
            status = "Pairing";
        } else if (out->active_connected) {
            status = "Connected";
        } else {
            status = "Connecting...";
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
 * dem eingestellten Betriebssystem (Mac: OPT und CMD, Windows: ALT und WIN).
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

#if IS_ENABLED(CONFIG_DILEMMA_MAX_MENU)
    if (menu_is_open()) {
        draw_menu(canvas, middle_cbuf, BLOCK_SIZE);
        return;
    }
#endif

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
    // Die Namen folgen dem Betriebssystem des gewaehlten Ausgangs (src/os_mode.c)
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
    bool windows = dilemma_max_os_is_windows();
#else
    bool windows = false;
#endif
    draw_mod(canvas, 0, 38, 33, windows ? "ALT" : "OPT", mods & (MOD_LALT | MOD_RALT));
    draw_mod(canvas, 35, 38, 33, windows ? "WIN" : "CMD", mods & (MOD_LGUI | MOD_RGUI));

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

    const char *name = state.layer.name;
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
    // Ohne weiteren Layer steht hier das Betriebssystem des Ausgangs: unter Windows ist der
    // WIN-Layer aktiv und heisst so, auf dem Mac gibt es nur den Basis-Layer.
    if (state.layer.index == DM_LAYER_BASE) {
        name = "MAC";
    }
#endif

    if (name != NULL && strlen(name) > 0) {
        lv_canvas_draw_text(canvas, 0, 4, BLOCK_SIZE, &label_dsc, name);
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

/*
 * Rechte Haelfte: Wert, den das Central per Split-BLE von der Peripheral abholt. Einen
 * Ladezustand kennt ZMK dafuer nicht, deshalb steckt er in der Parittaet des Werts:
 * ungerade = laedt (siehe src/peripheral_charge_report.c). Das kostet ein Prozent
 * Genauigkeit. 0 meldet ZMK selbst, sobald die Verbindung abreisst.
 */

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

    return (struct battery_state){
        .level = level,
        .charging = IS_ENABLED(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_DECODE) && (level & 1),
    };
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
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
    // Mac/Windows schaltet den WIN-Layer, und daran haengen die Namen der Modifier
    if (middle_canvas != NULL) {
        draw_middle();
        middle_canvas_drawn = true;
    }
#endif
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

#if IS_ENABLED(CONFIG_DILEMMA_MAX_MENU)
// Menue geoeffnet, geschlossen oder veraendert: beide Bloecke neu, auf der Display-Workqueue
static void menu_redraw_work_cb(struct k_work *work) {
    if (top_canvas == NULL || middle_canvas == NULL) {
        return;
    }
    draw_top();
    draw_middle();
    middle_canvas_drawn = true;
}

static K_WORK_DEFINE(menu_redraw_work, menu_redraw_work_cb);

void dilemma_max_status_screen_menu_changed(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &menu_redraw_work);
    }
}
#endif

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
