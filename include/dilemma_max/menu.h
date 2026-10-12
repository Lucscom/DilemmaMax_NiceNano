/*
 * Dilemma Max - Einstellungsmenue auf dem nice!view (src/menu.c, gezeichnet in
 * src/status_screen.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* So viele Eintraege hat eine Seite hoechstens - mehr passen nicht aufs Display. */
#define DILEMMA_MAX_MENU_MAX_ITEMS 5

/* Symbole, gezeichnet in src/status_screen.c */
enum dilemma_max_menu_icon {
    DILEMMA_MAX_ICON_SUN,
    DILEMMA_MAX_ICON_POWER,
    DILEMMA_MAX_ICON_SLEEP,
    DILEMMA_MAX_ICON_MONITOR,
    DILEMMA_MAX_ICON_PAD,
    DILEMMA_MAX_ICON_KNOB,
    DILEMMA_MAX_ICON_CURSOR,
    DILEMMA_MAX_ICON_SCROLL,
    DILEMMA_MAX_ICON_INVERT,
    DILEMMA_MAX_ICON_GLIDE,
    DILEMMA_MAX_ICON_TAP,
    DILEMMA_MAX_ICON_COUNT,
};

bool dilemma_max_menu_is_open(void);

/* Name der offenen Seite fuer das Band unten im Display, z.B. "MENU" oder "TOUCH". */
const char *dilemma_max_menu_title(void);

/* Eintraege der offenen Seite */
uint8_t dilemma_max_menu_item_count(void);
uint8_t dilemma_max_menu_cursor(void);
uint8_t dilemma_max_menu_item_icon(uint8_t item);

/* Name (hoechstens 7 Zeichen) und aktueller Wert (hoechstens 5) eines Eintrags. */
void dilemma_max_menu_item_text(uint8_t item, char *label, size_t label_len, char *value,
                                size_t value_len);

/* In src/status_screen.c: Menue wurde geoeffnet, geschlossen oder veraendert. */
void dilemma_max_status_screen_menu_changed(void);
