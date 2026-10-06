/*
 * Dilemma Max - Gesten-Codes des Trackpads.
 *
 * Der maXTouch-Treiber (rechte Haelfte) meldet erkannte Gesten als INPUT_EV_KEY mit diesen
 * Codes, jeweils Druecken + Loslassen. zmk,input-split uebertraegt Typ, Code und Wert
 * unveraendert zur linken Haelfte, dort setzt der Input-Processor
 * "dilemma,input-processor-gesture-keys" sie in Tastenkuerzel um.
 *
 * Die Codes liegen bewusst ausserhalb von INPUT_BTN_0..INPUT_BTN_4: nur diese wertet der
 * ZMK-Input-Listener als Maustasten aus. Ohne den Processor verpuffen die Gesten also.
 * 0x2c0 ist unter Linux der Beginn von BTN_TRIGGER_HAPPY, in Zephyr unbelegt.
 */

#pragma once

#define DM_GESTURE_SWIPE3_LEFT 0x2c0
#define DM_GESTURE_SWIPE3_RIGHT 0x2c1
#define DM_GESTURE_SWIPE3_UP 0x2c2
#define DM_GESTURE_SWIPE3_DOWN 0x2c3
#define DM_GESTURE_PINCH_OUT 0x2c4 /* Finger auseinander = hineinzoomen */
#define DM_GESTURE_PINCH_IN 0x2c5  /* Finger zusammen = herauszoomen */
