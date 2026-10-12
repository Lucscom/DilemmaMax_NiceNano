/*
 * Dilemma Max - Betriebssystem des verbundenen Rechners (src/os_mode.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/kernel.h>

/* Gilt der gerade gewaehlte Ausgang (Bluetooth-Profil oder USB) als Windows? */
bool dilemma_max_os_is_windows(void);

/* Schaltet den gerade gewaehlten Ausgang zwischen Mac und Windows um. */
void dilemma_max_os_toggle(void);

/* Name des gerade gewaehlten Ausgangs, "BT 1".."BT 5" oder "USB". */
void dilemma_max_os_output_name(char *name, size_t name_len);

/*
 * Ausgaenge: jedes Bluetooth-Profil und USB. Alle Einstellungen aus dem Menue werden pro
 * Ausgang gemerkt und wechseln mit ihm (src/power.c, src/input_settings.c, src/os_mode.c).
 */
#if IS_ENABLED(CONFIG_DILEMMA_MAX_OS_MODE)
#include <zmk/ble.h>
#define DILEMMA_MAX_OUTPUT_COUNT (ZMK_BLE_PROFILE_COUNT + 1)
/* Index des gerade gewaehlten Ausgangs, 0..DILEMMA_MAX_OUTPUT_COUNT-1; USB ist der letzte. */
int dilemma_max_output_index(void);
#else
#define DILEMMA_MAX_OUTPUT_COUNT 1
static inline int dilemma_max_output_index(void) { return 0; }
#endif
