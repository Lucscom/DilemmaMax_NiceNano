/*
 * Dilemma Max - Kommandos fuer das Einstellungsmenue auf dem nice!view (&menu, src/menu.c).
 */

#pragma once

#include <dt-bindings/dilemma_max/layers.h>

#define DM_MENU_OPEN 0   /* oeffnen, bzw. schliessen, wenn es offen ist */
#define DM_MENU_UP 1     /* einen Eintrag nach oben */
#define DM_MENU_DOWN 2   /* einen Eintrag nach unten */
#define DM_MENU_SELECT 3 /* Wert des Eintrags weiterschalten, bei EXIT schliessen */
