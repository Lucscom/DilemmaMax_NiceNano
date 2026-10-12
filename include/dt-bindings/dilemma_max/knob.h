/*
 * Dilemma Max - Parameter des &knob-Behaviors (src/knob.c): welcher Encoder in welche
 * Richtung gedreht wurde. Was daraufhin passiert, steht im Menue unter Input.
 */

#pragma once

#define DM_KNOB_LEFT_DEC 0  /* linker Encoder, Richtung "weniger" (leiser, runter, dunkler) */
#define DM_KNOB_LEFT_INC 1  /* linker Encoder, Richtung "mehr" */
#define DM_KNOB_RIGHT_DEC 2 /* rechter Encoder, Richtung "weniger" */
#define DM_KNOB_RIGHT_INC 3 /* rechter Encoder, Richtung "mehr" */
