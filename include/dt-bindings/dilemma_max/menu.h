/*
 * Dilemma Max - Kommandos fuer das Einstellungsmenue auf dem nice!view (&menu, src/menu.c).
 */

#pragma once

#include <dt-bindings/dilemma_max/layers.h>

#define DM_MENU_OPEN 0   /* oeffnen */
#define DM_MENU_UP 1     /* einen Eintrag nach oben */
#define DM_MENU_DOWN 2   /* einen Eintrag nach unten */
#define DM_MENU_SELECT 3 /* Untermenue oeffnen bzw. Wert weiterschalten; mit NAV: schliessen */
#define DM_MENU_BACK 4   /* NAV-Taste im Menue: antippen = eine Ebene zurueck */
