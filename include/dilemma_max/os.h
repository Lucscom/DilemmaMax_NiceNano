/*
 * Dilemma Max - Betriebssystem des verbundenen Rechners (src/os_mode.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Gilt der gerade gewaehlte Ausgang (Bluetooth-Profil oder USB) als Windows? */
bool dilemma_max_os_is_windows(void);

/* Schaltet den gerade gewaehlten Ausgang zwischen Mac und Windows um. */
void dilemma_max_os_toggle(void);

/* Name des gerade gewaehlten Ausgangs, "BT 1".."BT 5" oder "USB". */
void dilemma_max_os_output_name(char *name, size_t name_len);
