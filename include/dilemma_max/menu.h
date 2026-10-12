/*
 * Dilemma Max - Einstellungsmenue auf dem nice!view (src/menu.c, gezeichnet in
 * src/status_screen.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DILEMMA_MAX_MENU_ITEM_COUNT 4

bool dilemma_max_menu_is_open(void);
uint8_t dilemma_max_menu_cursor(void);

/* Name (hoechstens 8 Zeichen) und aktueller Wert (hoechstens 5) eines Eintrags. */
void dilemma_max_menu_item_text(uint8_t item, char *label, size_t label_len, char *value,
                                size_t value_len);

/* In src/status_screen.c: Menue wurde geoeffnet, geschlossen oder veraendert. */
void dilemma_max_status_screen_menu_changed(void);
