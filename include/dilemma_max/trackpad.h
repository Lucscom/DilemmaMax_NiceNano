/*
 * Dilemma Max - zur Laufzeit schaltbare Optionen des maXTouch-Treibers
 * (drivers/input/input_maxtouch.c).
 */

#pragma once

#include <stdbool.h>
#include <zephyr/device.h>

/* tap: Tippen klickt. momentum: Scrollen laeuft nach dem Abheben aus. */
void mxt_set_options(const struct device *dev, bool tap, bool momentum);
