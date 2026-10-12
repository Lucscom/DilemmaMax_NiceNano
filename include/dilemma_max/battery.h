/*
 * Dilemma Max - Schnittstelle zwischen Akkufilter (src/battery_filter.c) und Ladezustands-
 * Kodierung der rechten Haelfte (src/peripheral_charge_report.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bereinigter Akkustand in Prozent, 1..100. 0, solange noch nichts gemessen wurde. */
uint8_t dilemma_max_battery_filter_level(void);

/* Bereinigte Spannung in mV, 0 solange noch nichts gemessen wurde. Am USB-Kabel ist das die
 * Versorgung und nicht der Akku. */
int32_t dilemma_max_battery_filter_mv(void);

/*
 * Steckt den Ladezustand in die Parittaet: ungerade = laedt, gerade = laedt nicht. Das
 * Ergebnis ist nie 0, denn 0 heisst auf dem Central "nicht verbunden".
 */
uint8_t dilemma_max_charge_encode(uint8_t level);

/* Rechte Haelfte: liefert USB gerade Strom? (src/peripheral_charge_report.c) */
bool dilemma_max_usb_powered(void);
