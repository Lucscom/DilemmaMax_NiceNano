/*
 * Dilemma Max - Einstellungen fuer Trackpad und Encoder (src/input_settings.c).
 */

#pragma once

#include <stdint.h>

enum dilemma_max_input_item {
    DILEMMA_MAX_INPUT_SPEED,  /* Cursor-Geschwindigkeit, 0..4, 2 = wie bisher */
    DILEMMA_MAX_INPUT_SCROLL, /* Scroll-Geschwindigkeit, 0..4, 2 = wie bisher */
    DILEMMA_MAX_INPUT_INVERT, /* Scroll-Richtung umkehren, 0/1 */
    DILEMMA_MAX_INPUT_GLIDE,  /* Scrollen laeuft nach dem Abheben aus, 0/1 */
    DILEMMA_MAX_INPUT_TAP,    /* Tippen aufs Pad klickt, 0/1 */
    DILEMMA_MAX_INPUT_KNOB,   /* linker Encoder: DILEMMA_MAX_KNOB_* */
    DILEMMA_MAX_INPUT_ITEM_COUNT,
};

#define DILEMMA_MAX_KNOB_VOLUME 0
#define DILEMMA_MAX_KNOB_SCROLL 1
#define DILEMMA_MAX_KNOB_LIGHT 2

uint8_t dilemma_max_input_get(enum dilemma_max_input_item item);

/* Schaltet auf den naechsten Wert, am Ende wieder von vorn. */
void dilemma_max_input_cycle(enum dilemma_max_input_item item);

/*
 * Kommando ans railpwr-Behavior der rechten Haelfte, das Tippen und Auslaufen im
 * Trackpad-Treiber schaltet: DILEMMA_MAX_PAD_OPTIONS mit den beiden Bits darunter.
 */
#define DILEMMA_MAX_PAD_OPTIONS 0x10
#define DILEMMA_MAX_PAD_OPTION_TAP 0x01
#define DILEMMA_MAX_PAD_OPTION_GLIDE 0x02

uint32_t dilemma_max_input_pad_options(void);
