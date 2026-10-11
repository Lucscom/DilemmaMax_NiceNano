/*
 * Dilemma Max - Schnittstellen rund ums Stromsparen: einstellbare Zeiten (src/power.c),
 * 3,3-V-Schiene und Deep Sleep (src/rail_idle.c), Abgleich mit der rechten Haelfte
 * (src/rgb_split_sync.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum dilemma_max_power_item {
    DILEMMA_MAX_POWER_LIGHT, /* Beleuchtung aus nach ... */
    DILEMMA_MAX_POWER_RAIL,  /* 3,3-V-Schiene (LEDs, Display, Trackpad) aus nach ... */
    DILEMMA_MAX_POWER_SLEEP, /* Deep Sleep nach ... */
    DILEMMA_MAX_POWER_ITEM_COUNT,
};

/* Eingestellte Zeit in Sekunden ohne Eingabe, 0 = nie. */
uint16_t dilemma_max_power_timeout_s(enum dilemma_max_power_item item);

/* Schaltet die Zeit auf den naechsten Wert der Liste, am Ende wieder von vorn. */
void dilemma_max_power_cycle(enum dilemma_max_power_item item);

/* Soll die Schiene der rechten Haelfte gerade an sein? */
bool dilemma_max_power_remote_rail_on(void);

/* Parameter des railpwr-Behaviors, ueber das das Central die rechte Haelfte schaltet */
#define DILEMMA_MAX_RAIL_OFF 0
#define DILEMMA_MAX_RAIL_ON 1
#define DILEMMA_MAX_RAIL_SLEEP 2

/* Schaltet die 3,3-V-Schiene dieser Haelfte (aus mit kurzer Verzoegerung). */
void dilemma_max_rail_set(bool on);

/* Deep Sleep, wie ihn sonst ZMK ausloest. Kehrt nur zurueck, wenn das Einschlafen scheitert. */
void dilemma_max_sleep_now(void);

/* Schickt Underglow- und Schienenzustand an die rechte Haelfte, mit Wiederholungen. */
void dilemma_max_split_sync(uint8_t retries);

/* Schickt die rechte Haelfte in den Deep Sleep. */
void dilemma_max_split_send_sleep(void);
