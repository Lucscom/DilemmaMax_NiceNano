/*
 * Dilemma Max - eigenes nice!view-Layout fuer die linke Haelfte (Central).
 *
 * Ersetzt das Status-Widget des nice_view-Shields (CONFIG_NICE_VIEW_WIDGET_STATUS=n)
 * und zeigt vorerst nur den Akkustand beider Haelften, oben links, darunter rechts.
 *
 * Das Display ist nativ 160x68 und steht auf der Tastatur hochkant. Wie beim
 * Original-Widget wird jeder Block auf einem 68x68-Canvas in Leserichtung gezeichnet
 * und danach um 90 Grad gedreht. Der Block ganz rechts im nativen Bild ist oben
 * in der Hochkant-Ansicht.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>

#define BLOCK_SIZE 68

#define COLOR_BG                                                                                   \
    (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_black() : lv_color_white())
#define COLOR_FG                                                                                   \
    (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_white() : lv_color_black())

// Die rechte Haelfte ist die einzige Peripheral und hat damit immer Slot 0.
#define RIGHT_SOURCE 0

struct battery_block_state {
    uint8_t level;
    bool charging;
};

static lv_obj_t *left_canvas;
static lv_obj_t *right_canvas;
static lv_color_t left_cbuf[BLOCK_SIZE * BLOCK_SIZE];
static lv_color_t right_cbuf[BLOCK_SIZE * BLOCK_SIZE];

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

static void init_label(lv_draw_label_dsc_t *dsc, const lv_font_t *font, lv_text_align_t align) {
    lv_draw_label_dsc_init(dsc);
    dsc->color = COLOR_FG;
    dsc->font = font;
    dsc->align = align;
}

static void init_rect(lv_draw_rect_dsc_t *dsc, lv_color_t color) {
    lv_draw_rect_dsc_init(dsc);
    dsc->bg_color = color;
}

/*
 * Ein Block: Titel oben, Prozentwert in der Mitte, Akku-Balken unten. level == 0
 * steht fuer "unbekannt" - ZMK meldet beim Trennen der Peripheral genau diesen Wert.
 */
static void draw_battery_block(lv_obj_t *canvas, lv_color_t cbuf[], const char *title,
                               struct battery_block_state state) {
    lv_draw_rect_dsc_t bg;
    init_rect(&bg, COLOR_BG);
    lv_draw_rect_dsc_t fg;
    init_rect(&fg, COLOR_FG);
    lv_draw_label_dsc_t title_dsc;
    init_label(&title_dsc, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t charge_dsc;
    init_label(&charge_dsc, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t value_dsc;
    init_label(&value_dsc, &lv_font_montserrat_22, LV_TEXT_ALIGN_CENTER);

    uint8_t level = MIN(state.level, 100);

    lv_canvas_draw_rect(canvas, 0, 0, BLOCK_SIZE, BLOCK_SIZE, &bg);

    lv_canvas_draw_text(canvas, 2, 1, BLOCK_SIZE - 4, &title_dsc, title);
    if (state.charging) {
        lv_canvas_draw_text(canvas, 2, 1, BLOCK_SIZE - 4, &charge_dsc, LV_SYMBOL_CHARGE);
    }

    char value[8];
    if (level > 0) {
        snprintf(value, sizeof(value), "%u%%", level);
    } else {
        strcpy(value, "--");
    }
    lv_canvas_draw_text(canvas, 0, 20, BLOCK_SIZE, &value_dsc, value);

    // Akku-Symbol: Rahmen 60x13, Pol rechts, Fuellung innen maximal 56 px breit
    lv_canvas_draw_rect(canvas, 2, 49, 60, 13, &fg);
    lv_canvas_draw_rect(canvas, 3, 50, 58, 11, &bg);
    lv_canvas_draw_rect(canvas, 62, 52, 3, 7, &fg);
    int fill = (56 * level + 50) / 100;
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 4, 51, fill, 9, &fg);
    }

    rotate_canvas(canvas, cbuf);
}

// Linke Haelfte: eigener Akku, geladen wird, solange USB Strom liefert.

static void left_battery_update_cb(struct battery_block_state state) {
    draw_battery_block(left_canvas, left_cbuf, "LINKS", state);
}

static struct battery_block_state left_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_block_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(left_battery, struct battery_block_state, left_battery_update_cb,
                            left_battery_get_state)
ZMK_SUBSCRIPTION(left_battery, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(left_battery, zmk_usb_conn_state_changed);
#endif

// Rechte Haelfte: Wert, den das Central per Split-BLE von der Peripheral abholt.
// Ob sie laedt, uebertraegt ZMK nicht.

static void right_battery_update_cb(struct battery_block_state state) {
    draw_battery_block(right_canvas, right_cbuf, "RECHTS", state);
}

static struct battery_block_state right_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    uint8_t level = 0;
    if (ev != NULL && ev->source == RIGHT_SOURCE) {
        level = ev->state_of_charge;
    } else {
        zmk_split_central_get_peripheral_battery_level(RIGHT_SOURCE, &level);
    }

    return (struct battery_block_state){.level = level};
}

ZMK_DISPLAY_WIDGET_LISTENER(right_battery, struct battery_block_state, right_battery_update_cb,
                            right_battery_get_state)
ZMK_SUBSCRIPTION(right_battery, zmk_peripheral_battery_state_changed);

static lv_obj_t *create_block(lv_obj_t *parent, lv_color_t cbuf[], lv_coord_t x_ofs) {
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_obj_align(canvas, LV_ALIGN_TOP_RIGHT, x_ofs, 0);
    lv_canvas_set_buffer(canvas, cbuf, BLOCK_SIZE, BLOCK_SIZE, LV_IMG_CF_TRUE_COLOR);
    return canvas;
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    // Die 24 px unter den beiden Bloecken bleiben vorerst leer
    lv_obj_set_style_bg_color(screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    left_canvas = create_block(screen, left_cbuf, 0);
    right_canvas = create_block(screen, right_cbuf, -BLOCK_SIZE);

    left_battery_init();
    right_battery_init();

    return screen;
}
