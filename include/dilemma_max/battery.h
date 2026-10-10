/*
 * Dilemma Max - Schnittstelle zwischen Akkufilter (src/battery_filter.c) und Ladezustands-
 * Kodierung der rechten Haelfte (src/peripheral_charge_report.c).
 */

#pragma once

#include <stdint.h>

/* Bereinigter Akkustand in Prozent, 1..100. 0, solange noch nichts gemessen wurde. */
uint8_t dilemma_max_battery_filter_level(void);

/*
 * Steckt den Ladezustand in die Parittaet: ungerade = laedt, gerade = laedt nicht. Das
 * Ergebnis ist nie 0, denn 0 heisst auf dem Central "nicht verbunden".
 */
uint8_t dilemma_max_charge_encode(uint8_t level);
