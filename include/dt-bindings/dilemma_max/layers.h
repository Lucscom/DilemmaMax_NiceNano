/*
 * Dilemma Max - Layer der Keymap, auf die sich die Firmware bezieht. Die Reihenfolge der
 * Layer in der Keymap muss dazu passen.
 *
 * Hoehere Layer gewinnen:
 *  - WIN und die KNOB-Layer liegen unter NAV, NAV ueberschreibt sie also weiterhin.
 *  - NAV_WIN liegt ueber NAV (Windows-Varianten einzelner NAV-Tasten).
 *  - MENU liegt ganz oben, damit der Encoder im Menue immer dem Menue gehoert.
 */

#pragma once

#define DM_LAYER_BASE 0
#define DM_LAYER_WIN 1         /* aktiv, solange der verbundene Rechner als Windows gilt */
#define DM_LAYER_KNOB_SCROLL 2 /* linker Encoder scrollt (Menue: Input > Knob) */
#define DM_LAYER_KNOB_LIGHT 3  /* linker Encoder regelt die Helligkeit */
#define DM_LAYER_NAV 4
#define DM_LAYER_NAV_WIN 5     /* NAV und WIN zusammen, per conditional-layers */
#define DM_LAYER_MENU 6        /* aktiv, solange das Einstellungsmenue offen ist */
